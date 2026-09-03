/* scgui: the machine on its own thread.
 *
 * Copyright (c) 2026 ian karlsson
 * SPDX-License-Identifier: BSD-3-Clause
 */
#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "machine.h"
#include "audio.h"
#include "roms.h"
#include "session.h"
#include "smf.h"
#include "midi_io.h"

#define BLOCK 256
#define QUEUE_SIZE 64
#define TIMED_KEYS 32

typedef enum command_kind
{
	CMD_PLAY, CMD_PAUSE, CMD_STOP, CMD_BUTTON, CMD_POWER, CMD_GAIN, CMD_MIDI_IN, CMD_MIDI_OUT,
	CMD_RESET, CMD_MAP, CMD_QUIT
} command_kind_t;

typedef struct command
{
	command_kind_t kind;
	int a, b, c;
	float f;
	char *path;
} command_t;

struct machine
{
	scplay_roms_t roms;
	scemu_t *m;
	uint32_t rate;
	session_t session;
	scplay_audio_t *audio;
	midi_io_t *midi;
	machine_options_t opt;
	char audio_driver[64];

	pthread_t thread;
	pthread_mutex_t lock;
	command_t queue[QUEUE_SIZE];
	int q_head, q_count;
	machine_state_t state;

	/* the thread's own */
	smf_t smf;
	bool have_smf, playing, paused, power;
	uint64_t pos, end_frame, lead;
	size_t next_event;
	bool held[SCEMU_BUTTON_COUNT];
	struct { scemu_button_t b; bool down; uint64_t due; } timed[TIMED_KEYS];
	int timed_count;
	uint64_t frames;          /* rendered since the start; what the timed keys wait on */
	float gain;
	machine_reset_t reset;
	double clock_start;
	uint64_t clock_frames;
	bool started;
};

static double now_seconds(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static void sleep_ms(int ms)
{
	struct timespec ts = { 0, ms * 1000000L };
	nanosleep(&ts, NULL);
}

static void post(machine_t *mc, command_t c)
{
	pthread_mutex_lock(&mc->lock);
	if (mc->q_count < QUEUE_SIZE)
	{
		mc->queue[(mc->q_head + mc->q_count) % QUEUE_SIZE] = c;
		mc->q_count++;
	}
	else
		free(c.path);
	pthread_mutex_unlock(&mc->lock);
}

static bool take(machine_t *mc, command_t *c)
{
	pthread_mutex_lock(&mc->lock);
	bool got = mc->q_count > 0;
	if (got)
	{
		*c = mc->queue[mc->q_head];
		mc->q_head = (mc->q_head + 1) % QUEUE_SIZE;
		mc->q_count--;
	}
	pthread_mutex_unlock(&mc->lock);
	return got;
}

/* ---------------------------------------------------------------- the snapshot */

static void publish(machine_t *mc, bool booting)
{
	machine_state_t s;
	memset(&s, 0, sizeof(s));
	if (mc->power)
	{
		const scemu_lcd_t *lcd = scemu_lcd(mc->m);
		s.lcd = *lcd;
		s.leds = scemu_leds(mc->m);
	}
	s.power = mc->power;
	s.booting = booting;
	s.playing = mc->playing;
	s.paused = mc->paused;
	s.finished = mc->have_smf && !mc->playing && mc->pos >= mc->end_frame;
	s.position = (double)mc->pos / mc->rate;
	s.length = mc->have_smf ? (double)mc->end_frame / mc->rate : 0;
	s.underruns = mc->audio ? audio_underruns(mc->audio) : 0;
	pthread_mutex_lock(&mc->lock);
	snprintf(s.song, sizeof(s.song), "%s", mc->state.song);
	snprintf(s.title, sizeof(s.title), "%s", mc->state.title);
	s.generation = mc->state.generation;
	if (memcmp(&s.lcd, &mc->state.lcd, sizeof(s.lcd)) != 0 || s.leds != mc->state.leds || s.power != mc->state.power
	    || s.booting != mc->state.booting || s.playing != mc->state.playing || s.paused != mc->state.paused
	    || s.finished != mc->state.finished || s.position != mc->state.position || s.underruns != mc->state.underruns)
		s.generation++;
	mc->state = s;
	pthread_mutex_unlock(&mc->lock);
}

static void set_song(machine_t *mc, const char *song, const char *title)
{
	pthread_mutex_lock(&mc->lock);
	snprintf(mc->state.song, sizeof(mc->state.song), "%s", song ? song : "");
	snprintf(mc->state.title, sizeof(mc->state.title), "%s", title ? title : "");
	mc->state.generation++;
	pthread_mutex_unlock(&mc->lock);
}

void machine_snapshot(machine_t *mc, machine_state_t *out)
{
	pthread_mutex_lock(&mc->lock);
	*out = mc->state;
	pthread_mutex_unlock(&mc->lock);
}

/* ---------------------------------------------------------------- the thread */

static bool boot_progress(void *user, uint64_t frames)
{
	machine_t *mc = user;
	if (frames % (BLOCK * 16) == 0)
		publish(mc, true);
	return true;
}

static void key(machine_t *mc, scemu_button_t b, bool down)
{
	mc->held[b] = down;
	if (mc->power)
		scemu_button(mc->m, b, down);
}

/* the timed keys, in the order they were posted, once their time has come */
static void timed_keys(machine_t *mc)
{
	int kept = 0;
	for (int n = 0; n < mc->timed_count; n++)
	{
		if (mc->timed[n].due <= mc->frames)
			key(mc, mc->timed[n].b, mc->timed[n].down);
		else
			mc->timed[kept++] = mc->timed[n];
	}
	mc->timed_count = kept;
}

static void hold_keys(machine_t *mc)
{
	for (int n = 0; n < SCEMU_BUTTON_COUNT; n++)
		if (mc->held[n])
			scemu_button(mc->m, (scemu_button_t)n, true);
}

static void boot(machine_t *mc, bool use_cache)
{
	mc->power = true;
	hold_keys(mc);
	publish(mc, true);
	session_boot(&mc->session, use_cache, boot_progress, mc);
	scemu_set_map(mc->m, mc->opt.map);
	scemu_set_midi_rate(mc->m, mc->opt.midi_rate);
	publish(mc, false);
}

static void unload_song(machine_t *mc)
{
	if (mc->have_smf)
		smf_free(&mc->smf);
	mc->have_smf = mc->playing = false;
	mc->pos = mc->end_frame = 0;
	mc->next_event = 0;
}

static void load_song(machine_t *mc, const char *path)
{
	unload_song(mc);
	if (!smf_load(&mc->smf, path, mc->rate))
	{
		set_song(mc, session_base_name(path), "not a Standard MIDI File");
		return;
	}
	mc->have_smf = true;
	static const uint8_t gm_on[] = { 0xf0, 0x7e, 0x7f, 0x09, 0x01, 0xf7 };
	static const uint8_t gm2_on[] = { 0xf0, 0x7e, 0x7f, 0x09, 0x03, 0xf7 };
	static const uint8_t gs_reset[] = { 0xf0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7f, 0x00, 0x41, 0xf7 };
	static const uint8_t mode_single[] = { 0xf0, 0x41, 0x10, 0x42, 0x12, 0x00, 0x00, 0x7f, 0x00, 0x01, 0xf7 };
	static const uint8_t mode_double[] = { 0xf0, 0x41, 0x10, 0x42, 0x12, 0x00, 0x00, 0x7f, 0x01, 0x00, 0xf7 };
	const uint8_t *msg = NULL;
	size_t msg_size = 0;
	switch (mc->reset)
	{
	case MACHINE_RESET_GM: msg = gm_on; msg_size = sizeof(gm_on); break;
	case MACHINE_RESET_GS: msg = gs_reset; msg_size = sizeof(gs_reset); break;
	case MACHINE_RESET_GM2: msg = gm2_on; msg_size = sizeof(gm2_on); break;
	case MACHINE_RESET_SC88_SINGLE: msg = mode_single; msg_size = sizeof(mode_single); break;
	case MACHINE_RESET_SC88_DOUBLE: msg = mode_double; msg_size = sizeof(mode_double); break;
	default: break;
	}
	mc->lead = 0;
	if (msg)
	{
		for (int port = 0; port < 2; port++)
		{
			scemu_midi_write(mc->m, port, msg, msg_size, 0);
			midi_io_write(mc->midi, port ? MIDI_IO_SONG_B : MIDI_IO_SONG_A, msg, msg_size);
		}
		mc->lead = mc->rate / 4;
	}
	mc->end_frame = (uint64_t)mc->smf.last_frame + mc->lead + (uint64_t)(mc->opt.tail * mc->rate);
	mc->playing = true;
	mc->paused = false;
	set_song(mc, session_base_name(path), mc->smf.name);
}

static void feed_events(machine_t *mc, size_t n)
{
	while (mc->next_event < mc->smf.count && mc->smf.events[mc->next_event].frame + mc->lead < mc->pos + n)
	{
		const smf_event_t *e = &mc->smf.events[mc->next_event++];
		uint64_t at = e->frame + mc->lead;
		uint32_t offset = at > mc->pos ? (uint32_t)(at - mc->pos) : 0;
		if (e->tempo_change || (e->port != SMF_PORT_UNSET && e->port > 1))
			continue;
		int port = e->port == 1 ? SCEMU_MIDI_IN_B : SCEMU_MIDI_IN_A;
		int song = e->port == 1 ? MIDI_IO_SONG_B : MIDI_IO_SONG_A;
		if (e->status[0] == 0xf0 && e->bytes)
		{
			scemu_midi_write(mc->m, port, e->status, 1, offset);
			scemu_midi_write(mc->m, port, e->bytes, e->length - 1u, offset);
			midi_io_write(mc->midi, song, e->status, 1);
			midi_io_write(mc->midi, song, e->bytes, e->length - 1u);
		}
		else if (e->bytes)
		{
			scemu_midi_write(mc->m, port, e->bytes, e->length, offset);
			midi_io_write(mc->midi, song, e->bytes, e->length);
		}
		else
		{
			scemu_midi_write(mc->m, port, e->status, e->length, offset);
			midi_io_write(mc->midi, song, e->status, e->length);
		}
	}
}

static void render_block(machine_t *mc, size_t n);

/* every note and every sound off on every part of both ports, and time for
 * the firmware to act on it */
static void quiet(machine_t *mc)
{
	for (int port = 0; port < 2; port++)
		for (int ch = 0; ch < 16; ch++)
		{
			uint8_t off[6] = { (uint8_t)(0xb0 | ch), 0x7b, 0x00, (uint8_t)(0xb0 | ch), 0x78, 0x00 };
			scemu_midi_write(mc->m, port, off, sizeof(off), 0);
			midi_io_write(mc->midi, port ? MIDI_IO_SONG_B : MIDI_IO_SONG_A, off, sizeof(off));
		}
	for (size_t done = 0; done < mc->rate / 8; done += BLOCK)
	{
		while (mc->audio && audio_space(mc->audio) < BLOCK)
			sleep_ms(1);
		render_block(mc, BLOCK);
	}
}

static void handle(machine_t *mc, const command_t *c)
{
	switch (c->kind)
	{
	case CMD_PLAY:
		if (mc->power)
			load_song(mc, c->path);
		break;
	case CMD_PAUSE:
		if (mc->power && c->a && !mc->paused)
			quiet(mc);
		mc->paused = c->a != 0;
		break;
	case CMD_STOP:
		if (mc->power && mc->have_smf)
			quiet(mc);
		unload_song(mc);
		set_song(mc, "", "");
		break;
	case CMD_BUTTON:
		if (c->c > 0 && mc->timed_count < TIMED_KEYS)
		{
			mc->timed[mc->timed_count].b = (scemu_button_t)c->a;
			mc->timed[mc->timed_count].down = c->b != 0;
			mc->timed[mc->timed_count].due = mc->frames + (uint64_t)c->c * mc->rate / 1000;
			mc->timed_count++;
		}
		else
			key(mc, (scemu_button_t)c->a, c->b != 0);
		break;
	case CMD_POWER:
		if (c->a && !mc->power)
		{
			scemu_reset(mc->m);
			boot(mc, false);
		}
		else if (!c->a && mc->power)
		{
			mc->power = false;
			unload_song(mc);
			set_song(mc, "", "");
			publish(mc, false);
		}
		break;
	case CMD_GAIN:
		mc->gain = c->f;
		break;
	case CMD_MIDI_IN:
		midi_io_connect_input(mc->midi, c->a, c->b, c->c);
		break;
	case CMD_MIDI_OUT:
		midi_io_connect_output(mc->midi, c->a, c->b, c->c);
		break;
	case CMD_RESET:
		mc->reset = (machine_reset_t)c->a;
		break;
	case CMD_MAP:
		mc->opt.map = (scemu_map_t)c->a;
		if (mc->power)
			scemu_set_map(mc->m, mc->opt.map);
		break;
	case CMD_QUIT:
		break;
	}
}

static void to_s16(const int32_t *in, int16_t *out, size_t samples, float gain)
{
	for (size_t n = 0; n < samples; n++)
	{
		float v = (float)(in[n] >> 8) * gain;
		out[n] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
	}
}

static void render_block(machine_t *mc, size_t n)
{
	int32_t raw[BLOCK * 2];
	int16_t pcm[BLOCK * 2];
	int32_t *const out[2] = { raw, NULL };
	scemu_render(mc->m, out, n);
	to_s16(raw, pcm, n * 2, mc->gain);
	if (mc->audio)
	{
		audio_push(mc->audio, pcm, n);
		if (!mc->started && audio_space(mc->audio) == 0)
		{
			audio_pause(mc->audio, false);
			mc->started = true;
		}
		else if (mc->started)
			audio_pause(mc->audio, false);
	}
	mc->clock_frames += n;
	mc->frames += n;
	if (mc->timed_count)
		timed_keys(mc);
}

static void *run(void *user)
{
	machine_t *mc = user;
	command_t c;

	boot(mc, true);
	mc->clock_start = now_seconds();
	mc->clock_frames = 0;

	for (;;)
	{
		while (take(mc, &c))
		{
			if (c.kind == CMD_QUIT)
			{
				free(c.path);
				return NULL;
			}
			handle(mc, &c);
			free(c.path);
		}
		if (!mc->power)
		{
			if (mc->audio)
				audio_pause(mc->audio, true);
			sleep_ms(10);
			continue;
		}
		if (mc->paused && mc->playing)
		{
			if (mc->audio)
				audio_pause(mc->audio, true);
			publish(mc, false);
			sleep_ms(10);
			mc->clock_start = now_seconds();
			mc->clock_frames = 0;
			continue;
		}

		size_t n = BLOCK;
		if (mc->audio)
		{
			size_t space = audio_space(mc->audio);
			if (space < n)
				n = space;
		}
		else
		{
			double due = (now_seconds() - mc->clock_start) * mc->rate - (double)mc->clock_frames;
			if (due < (double)n)
				n = due < 0 ? 0 : (size_t)due;
		}
		if (n == 0)
		{
			/* a full queue on a paused device would wait forever: after a
			 * pause or a power-off the device is what must move first */
			if (mc->audio && mc->started)
				audio_pause(mc->audio, false);
			sleep_ms(1);
			continue;
		}

		for (;;)
		{
			uint8_t bytes[1024];
			int which;
			size_t got = midi_io_read(mc->midi, &which, bytes, sizeof(bytes));
			if (!got)
				break;
			scemu_midi_write(mc->m, which ? SCEMU_MIDI_IN_B : SCEMU_MIDI_IN_A, bytes, got, 0);
		}
		if (mc->playing)
		{
			feed_events(mc, n);
			mc->pos += n;
			if (mc->pos >= mc->end_frame)
				mc->playing = false;
		}
		render_block(mc, n);
		publish(mc, false);
	}
}

/* ---------------------------------------------------------------- the front */

static void midi_out(const uint8_t *bytes, size_t count, void *user)
{
	machine_t *mc = user;
	midi_io_write(mc->midi, MIDI_IO_OUT, bytes, count);
}

machine_t *machine_start(const machine_options_t *o, char *err, size_t err_size)
{
	machine_t *mc = calloc(1, sizeof(*mc));
	if (!mc)
		return NULL;
	mc->opt = *o;
	if (!scplay_roms_load(&mc->roms, o->model, o->rom, o->exe_dir, err, err_size))
	{
		free(mc);
		return NULL;
	}
	mc->m = scemu_create(mc->roms.model, &mc->roms.roms, NULL);
	if (!mc->m)
	{
		snprintf(err, err_size, "%s", scemu_error(NULL));
		scplay_roms_free(&mc->roms);
		free(mc);
		return NULL;
	}
	mc->rate = scemu_sample_rate(mc->m);
	session_init(&mc->session, mc->m, mc->roms.model_name, mc->roms.hash, o->no_cache, o->keep_settings);
	if (!o->no_audio)
	{
		char aerr[256];
		mc->audio = audio_open(mc->rate, aerr, sizeof(aerr));
		if (!mc->audio)
			fprintf(stderr, "scgui: no audio (%s), running silently\n", aerr);
		else
			snprintf(mc->audio_driver, sizeof(mc->audio_driver), "%s", audio_driver(mc->audio));
	}
	mc->gain = 0.75f * 0.75f;
	mc->reset = MACHINE_RESET_GS;
	mc->midi = midi_io_open("scgui");
	scemu_set_midi_out(mc->m, midi_out, mc);
	pthread_mutex_init(&mc->lock, NULL);
	if (pthread_create(&mc->thread, NULL, run, mc) != 0)
	{
		snprintf(err, err_size, "cannot start the machine's thread");
		machine_stop(mc);
		return NULL;
	}
	return mc;
}

void machine_stop(machine_t *mc)
{
	if (!mc)
		return;
	if (mc->thread)
	{
		command_t c = { CMD_QUIT, 0, 0, 0, 0, NULL };
		post(mc, c);
		pthread_join(mc->thread, NULL);
	}
	if (mc->power)
		session_save_settings(&mc->session);
	unload_song(mc);
	audio_close(mc->audio);
	midi_io_close(mc->midi);
	session_free(&mc->session);
	scemu_destroy(mc->m);
	scplay_roms_free(&mc->roms);
	pthread_mutex_destroy(&mc->lock);
	free(mc);
}

scemu_model_t machine_model(const machine_t *mc) { return mc->roms.model; }
const char *machine_model_label(const machine_t *mc) { return scplay_model_label(mc->roms.model); }
const char *machine_audio_driver(const machine_t *mc) { return mc->audio ? mc->audio_driver : "none"; }

void machine_play(machine_t *mc, const char *path)
{
	command_t c = { CMD_PLAY, 0, 0, 0, 0, strdup(path) };
	post(mc, c);
}

void machine_pause(machine_t *mc, bool paused)
{
	command_t c = { CMD_PAUSE, paused, 0, 0, 0, NULL };
	post(mc, c);
}

void machine_stop_song(machine_t *mc)
{
	command_t c = { CMD_STOP, 0, 0, 0, 0, NULL };
	post(mc, c);
}

void machine_button(machine_t *mc, scemu_button_t b, bool down)
{
	command_t c = { CMD_BUTTON, b, down, 0, 0, NULL };
	post(mc, c);
}

void machine_button_after(machine_t *mc, scemu_button_t b, bool down, unsigned ms)
{
	command_t c = { CMD_BUTTON, b, down, (int)(ms ? ms : 1), 0, NULL };
	post(mc, c);
}

void machine_power(machine_t *mc, bool on)
{
	command_t c = { CMD_POWER, on, 0, 0, 0, NULL };
	post(mc, c);
}

void machine_set_gain(machine_t *mc, float gain)
{
	command_t c = { CMD_GAIN, 0, 0, 0, gain, NULL };
	post(mc, c);
}

const char *const machine_reset_names[MACHINE_RESET_COUNT] = {
	"nothing", "GM System On", "GS Reset", "GM2 System On", "SC-88 Mode Set, single module", "SC-88 Mode Set, double module"
};

void machine_set_reset(machine_t *mc, machine_reset_t reset)
{
	command_t c = { CMD_RESET, reset, 0, 0, 0, NULL };
	post(mc, c);
}

void machine_set_map(machine_t *mc, scemu_map_t map)
{
	command_t c = { CMD_MAP, map, 0, 0, 0, NULL };
	post(mc, c);
}

void machine_midi_input(machine_t *mc, int which, int client, int port)
{
	command_t c = { CMD_MIDI_IN, which, client, port, 0, NULL };
	post(mc, c);
}

void machine_midi_output(machine_t *mc, int which, int client, int port)
{
	command_t c = { CMD_MIDI_OUT, which, client, port, 0, NULL };
	post(mc, c);
}
