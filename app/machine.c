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
#include "unit.h"
#include "smf.h"
#include "midi_io.h"

#define BLOCK_MAX 1024
#define BLOCK_DEFAULT 256
#define QUEUE_SIZE 64
#define PENDING_MAX 256
#define PENDING_BYTES 65536

typedef enum command_kind
{
	CMD_PLAY, CMD_PAUSE, CMD_STOP, CMD_BUTTON, CMD_DIAL, CMD_POWER, CMD_GAIN, CMD_AUDIO, CMD_MIDI_IN, CMD_MIDI_OUT,
	CMD_RESET, CMD_MAP, CMD_MODEL, CMD_SEND, CMD_QUIT
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
	unit_t *unit;
	uint32_t rate;
	scplay_audio_t *audio;
	midi_io_t *midi;
	machine_options_t opt;
	size_t block;             /* frames per render, the audio device's buffer */

	pthread_t thread;
	pthread_mutex_t lock;
	command_t queue[QUEUE_SIZE];
	int q_head, q_count;
	machine_state_t state;
	machine_rom_info_t info;
	unsigned models;          /* a bit per scemu_model_t whose ROM set is there */
	int ports;                /* the port groups the running machine takes */

	/* the thread's own */
	smf_t smf;
	bool have_smf, playing, paused;
	uint64_t pos, end_frame, lead;
	size_t next_event;
	float gain;
	machine_reset_t reset;
	double clock_start;
	uint64_t clock_frames;
	bool started;
	/* MIDI from the host, stamped when polled, placed when rendered */
	struct { double t; uint8_t which; uint16_t len; uint32_t at; } pending[PENDING_MAX];
	uint8_t pending_bytes[PENDING_BYTES];
	int pending_count;
	uint32_t pending_used;
};

const scemu_model_t machine_systems[MACHINE_SYSTEMS] = {
	SCEMU_MODEL_SC55MK2, SCEMU_MODEL_SC88, SCEMU_MODEL_SC88VL, SCEMU_MODEL_SC88PRO,
	SCEMU_MODEL_SC8850
};

int machine_system_index(scemu_model_t model)
{
	for (int n = 0; n < MACHINE_SYSTEMS; n++)
		if (machine_systems[n] == model)
			return n;
	return -1;
}

/* the position the options hold for a model, MIDI for one with no row */
static scemu_computer_switch_t computer_of(const machine_t *mc, scemu_model_t model)
{
	int row = machine_system_index(model);
	return row < 0 ? SCEMU_COMPUTER_MIDI : mc->opt.computer[row];
}

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
	unit_panel_t panel;
	memset(&s, 0, sizeof(s));
	bool panel_changed = unit_panel(mc->unit, &panel);
	s.lcd = panel.lcd;
	s.glcd = panel.glcd;
	s.has_glcd = panel.has_glcd;
	s.leds = panel.leds;
	s.power = panel.power;
	s.booting = booting;
	s.playing = mc->playing;
	s.paused = mc->paused;
	s.finished = mc->have_smf && !mc->playing && mc->pos >= mc->end_frame;
	s.position = (double)mc->pos / mc->rate;
	s.length = mc->have_smf ? (double)mc->end_frame / mc->rate : 0;
	s.underruns = mc->audio ? audio_underruns(mc->audio) : 0;
	if (mc->audio)
	{
		snprintf(s.audio, sizeof(s.audio), "%s (%s), %u Hz", audio_device_name(mc->audio), audio_driver(mc->audio),
		         audio_device_rate(mc->audio));
		s.latency = audio_latency(mc->audio);
	}
	else
		snprintf(s.audio, sizeof(s.audio), "none");
	pthread_mutex_lock(&mc->lock);
	snprintf(s.song, sizeof(s.song), "%s", mc->state.song);
	snprintf(s.title, sizeof(s.title), "%s", mc->state.title);
	snprintf(s.error, sizeof(s.error), "%s", mc->state.error);
	s.generation = mc->state.generation;
	if (panel_changed || s.booting != mc->state.booting || s.playing != mc->state.playing || s.paused != mc->state.paused
	    || s.finished != mc->state.finished || s.position != mc->state.position || s.underruns != mc->state.underruns
	    || strcmp(s.audio, mc->state.audio) != 0 || s.latency != mc->state.latency)
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

static void set_error(machine_t *mc, const char *text)
{
	pthread_mutex_lock(&mc->lock);
	snprintf(mc->state.error, sizeof(mc->state.error), "%s", text ? text : "");
	mc->state.generation++;
	pthread_mutex_unlock(&mc->lock);
}

/* the machine's own thread looks for the other models' ROMs: the catalog
 * behind it is not shared with another thread's load */
static void set_rom_info(machine_t *mc)
{
	unsigned models = scplay_roms_available(mc->opt.rom, mc->opt.exe_dir);
	pthread_mutex_lock(&mc->lock);
	mc->models = models;
	mc->info.model = unit_model(mc->unit);
	mc->info.label = unit_model_label(mc->unit);
	snprintf(mc->info.version, sizeof(mc->info.version), "%s", unit_rom_version(mc->unit));
	snprintf(mc->info.origin, sizeof(mc->info.origin), "%s", unit_rom_origin(mc->unit));
	mc->info.rate = mc->rate;
	mc->ports = unit_midi_ports(mc->unit);
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
	if (frames % (BLOCK_DEFAULT * 16) == 0)
		publish(mc, true);
	return true;
}

static void boot(machine_t *mc, bool use_cache)
{
	publish(mc, true);
	unit_boot(mc->unit, use_cache, boot_progress, mc);
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

const uint8_t *machine_reset_message(machine_reset_t reset, size_t *size)
{
	static const uint8_t gm_on[] = { 0xf0, 0x7e, 0x7f, 0x09, 0x01, 0xf7 };
	static const uint8_t gm2_on[] = { 0xf0, 0x7e, 0x7f, 0x09, 0x03, 0xf7 };
	static const uint8_t gs_reset[] = { 0xf0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7f, 0x00, 0x41, 0xf7 };
	static const uint8_t mode_single[] = { 0xf0, 0x41, 0x10, 0x42, 0x12, 0x00, 0x00, 0x7f, 0x00, 0x01, 0xf7 };
	static const uint8_t mode_double[] = { 0xf0, 0x41, 0x10, 0x42, 0x12, 0x00, 0x00, 0x7f, 0x01, 0x00, 0xf7 };
	switch (reset)
	{
	case MACHINE_RESET_GM: *size = sizeof(gm_on); return gm_on;
	case MACHINE_RESET_GS: *size = sizeof(gs_reset); return gs_reset;
	case MACHINE_RESET_GM2: *size = sizeof(gm2_on); return gm2_on;
	case MACHINE_RESET_SC88_SINGLE: *size = sizeof(mode_single); return mode_single;
	case MACHINE_RESET_SC88_DOUBLE: *size = sizeof(mode_double); return mode_double;
	default: *size = 0; return NULL;
	}
}

static int midi_ports(const machine_t *mc)
{
	return unit_midi_ports(mc->unit);
}

/* to every port group of the machine, and to the song outputs with it */
static void send_both(machine_t *mc, const uint8_t *msg, size_t size)
{
	unit_send(mc->unit, msg, size);
	for (int port = 0; port < midi_ports(mc); port++)
		midi_io_write(mc->midi, MIDI_IO_SONG_A + port, msg, size);
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
	size_t msg_size;
	const uint8_t *msg = machine_reset_message(mc->reset, &msg_size);
	mc->lead = 0;
	if (msg)
	{
		send_both(mc, msg, msg_size);
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
		if (e->tempo_change || (e->port != SMF_PORT_UNSET && e->port >= midi_ports(mc)))
			continue;
		int port = e->port == SMF_PORT_UNSET ? SCEMU_MIDI_IN_A : e->port;
		int song = MIDI_IO_SONG_A + port;
		if (e->status[0] == 0xf0 && e->bytes)
		{
			scemu_midi_write(unit_machine(mc->unit), port, e->status, 1, offset);
			scemu_midi_write(unit_machine(mc->unit), port, e->bytes, e->length - 1u, offset);
			midi_io_write(mc->midi, song, e->status, 1);
			midi_io_write(mc->midi, song, e->bytes, e->length - 1u);
		}
		else if (e->bytes)
		{
			scemu_midi_write(unit_machine(mc->unit), port, e->bytes, e->length, offset);
			midi_io_write(mc->midi, song, e->bytes, e->length);
		}
		else
		{
			scemu_midi_write(unit_machine(mc->unit), port, e->status, e->length, offset);
			midi_io_write(mc->midi, song, e->status, e->length);
		}
	}
}

static void render_block(machine_t *mc, size_t n);
static void midi_out(int port, const uint8_t *bytes, size_t count, void *user);

/* every note and every sound off on every part of both ports, and time for
 * the firmware to act on it */
static void quiet(machine_t *mc)
{
	/* a stopped stream (a paused song) would never drain the ring */
	if (mc->audio)
		audio_pause(mc->audio, false);
	for (int port = 0; port < midi_ports(mc); port++)
		for (int ch = 0; ch < 16; ch++)
		{
			uint8_t off[6] = { (uint8_t)(0xb0 | ch), 0x7b, 0x00, (uint8_t)(0xb0 | ch), 0x78, 0x00 };
			scemu_midi_write(unit_machine(mc->unit), port, off, sizeof(off), 0);
			midi_io_write(mc->midi, MIDI_IO_SONG_A + port, off, sizeof(off));
		}
	for (size_t done = 0; done < mc->rate / 8; done += mc->block)
	{
		while (mc->audio && audio_space(mc->audio) < mc->block)
			sleep_ms(1);
		render_block(mc, mc->block);
	}
}

static void open_audio(machine_t *mc)
{
	char err[256];
	size_t block = mc->opt.audio_block ? mc->opt.audio_block : BLOCK_DEFAULT;
	if (block > BLOCK_MAX)
		block = BLOCK_MAX;
	mc->block = block;
	mc->started = false;
	if (mc->opt.no_audio)
		return;
	mc->audio = audio_open(mc->rate, mc->opt.audio_rate, mc->opt.audio_device, block, err, sizeof(err));
	if (!mc->audio)
		fprintf(stderr, "scgui: no audio (%s), running silently\n", err);
}

/* Another machine in place of this one -- another model, or the same one with
 * its rear switch in another position, which the firmware only reads at boot.
 * The unit loads the new ROMs before the old instance goes, so a set that is
 * not there leaves the old one playing. */
static void switch_machine(machine_t *mc, scemu_model_t model)
{
	char err[256];
	if (!unit_prepare(mc->unit, model, computer_of(mc, model), err, sizeof(err)))
	{
		if (err[0])
			set_error(mc, err);
		return;
	}
	bool was_on = unit_power_on(mc->unit);
	if (was_on && mc->have_smf)
		quiet(mc);
	unload_song(mc);
	set_song(mc, "", "");
	publish(mc, was_on);
	mc->pending_count = 0;   /* stamped against the clock of the machine that goes */
	mc->pending_used = 0;
	unit_replace(mc->unit, boot_progress, mc);
	midi_io_set_groups(mc->midi, midi_ports(mc));
	uint32_t was_rate = mc->rate;
	mc->rate = unit_rate(mc->unit);
	if (mc->rate != was_rate)   /* the stream was opened around the old machine's rate */
	{
		audio_close(mc->audio);
		mc->audio = NULL;
		open_audio(mc);
	}
	set_error(mc, "");
	set_rom_info(mc);
	publish(mc, false);
	mc->clock_start = now_seconds();
	mc->clock_frames = 0;
}

static void handle(machine_t *mc, const command_t *c)
{
	switch (c->kind)
	{
	case CMD_PLAY:
		if (unit_power_on(mc->unit))
			load_song(mc, c->path);
		break;
	case CMD_PAUSE:
		if (unit_power_on(mc->unit) && c->a && !mc->paused)
			quiet(mc);
		mc->paused = c->a != 0;
		break;
	case CMD_STOP:
		if (unit_power_on(mc->unit) && mc->have_smf)
			quiet(mc);
		unload_song(mc);
		set_song(mc, "", "");
		break;
	case CMD_BUTTON:
		if (c->c > 0)
			unit_key_after(mc->unit, (scemu_button_t)c->a, c->b != 0, (unsigned)c->c);
		else
			unit_key(mc->unit, (scemu_button_t)c->a, c->b != 0);
		break;
	case CMD_DIAL:
		unit_dial(mc->unit, c->a);
		break;
	case CMD_POWER:
		if (c->a && !unit_power_on(mc->unit))
		{
			publish(mc, true);
			unit_power(mc->unit, true);
			publish(mc, false);
		}
		else if (!c->a && unit_power_on(mc->unit))
		{
			unit_power(mc->unit, false);
			unload_song(mc);
			set_song(mc, "", "");
			publish(mc, false);
		}
		break;
	case CMD_GAIN:
		mc->gain = c->f;
		break;
	case CMD_AUDIO:
		audio_close(mc->audio);
		mc->audio = NULL;
		mc->opt.audio_device = c->a;
		if (c->b > 0)
			mc->opt.audio_block = (unsigned)c->b;
		mc->opt.audio_rate = (unsigned)c->c;
		open_audio(mc);
		mc->clock_start = now_seconds();
		mc->clock_frames = 0;
		break;
	case CMD_MIDI_IN:
		midi_io_connect_input(mc->midi, c->a, c->b);
		break;
	case CMD_MIDI_OUT:
		midi_io_connect_output(mc->midi, c->a, c->b);
		break;
	case CMD_RESET:
		mc->reset = (machine_reset_t)c->a;
		break;
	case CMD_SEND:
		if (unit_power_on(mc->unit))
			send_both(mc, (const uint8_t *)c->path, (size_t)c->a);
		break;
	case CMD_MAP:
		unit_set_map(mc->unit, (scemu_map_t)c->a);
		break;
	case CMD_MODEL:
		if (c->b >= 0)
			mc->opt.computer[c->b] = (scemu_computer_switch_t)c->c;
		switch_machine(mc, (scemu_model_t)c->a);
		break;
	case CMD_QUIT:
		break;
	}
}

static void to_s16(const int32_t *in, int16_t *out, size_t samples, float gain)
{
	const float scale = gain * (1.0f / 256);
	for (size_t n = 0; n < samples; n++)
	{
		float v = (float)in[n] * scale;
		out[n] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
	}
}

static void render_block(machine_t *mc, size_t n)
{
	int32_t raw[BLOCK_MAX * 2];
	int16_t pcm[BLOCK_MAX * 2];
	int32_t *const out[2] = { raw, NULL };
	unit_render(mc->unit, out, n);
	to_s16(raw, pcm, n * 2, mc->gain * unit_output_trim(unit_model(mc->unit)));
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
}

/* Whatever the host's ports hold, stamped with the time it was seen; the
 * machine thread looks every millisecond or so, so the stamp is close to
 * the arrival. */
static void poll_midi(machine_t *mc, bool keep)
{
	for (;;)
	{
		uint8_t bytes[1024];
		int which;
		size_t got = midi_io_read(mc->midi, &which, bytes, sizeof(bytes));
		if (!got)
			break;
		if (!keep || mc->pending_count >= PENDING_MAX || mc->pending_used + got > PENDING_BYTES)
			continue;
		mc->pending[mc->pending_count].t = now_seconds();
		mc->pending[mc->pending_count].which = (uint8_t)which;
		mc->pending[mc->pending_count].len = (uint16_t)got;
		mc->pending[mc->pending_count].at = mc->pending_used;
		memcpy(mc->pending_bytes + mc->pending_used, bytes, got);
		mc->pending_used += (uint32_t)got;
		mc->pending_count++;
	}
}

/* Each message goes to the frame a fixed time after it was seen -- a block
 * and two milliseconds, which is later than the start of the block about
 * to be rendered however long ago the message arrived -- so the delay from
 * the host is the same for all of them instead of a whole block's worth
 * of jitter. */
static void deliver_midi(machine_t *mc)
{
	if (!mc->pending_count)
		return;
	double now = now_seconds();
	double delay = (double)mc->block / mc->rate + 0.002;
	for (int n = 0; n < mc->pending_count; n++)
	{
		double at = (delay - (now - mc->pending[n].t)) * mc->rate;
		uint32_t offset = at > 0 ? (uint32_t)(at + 0.5) : 0;
		scemu_midi_write(unit_machine(mc->unit), mc->pending[n].which, mc->pending_bytes + mc->pending[n].at,
		                 mc->pending[n].len, offset);
	}
	mc->pending_count = 0;
	mc->pending_used = 0;
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
		if (!unit_power_on(mc->unit))
		{
			if (mc->audio)
				audio_pause(mc->audio, true);
			poll_midi(mc, false);
			sleep_ms(10);
			continue;
		}
		if (mc->paused && mc->playing)
		{
			if (mc->audio)
				audio_pause(mc->audio, true);
			poll_midi(mc, false);
			publish(mc, false);
			sleep_ms(10);
			mc->clock_start = now_seconds();
			mc->clock_frames = 0;
			continue;
		}

		poll_midi(mc, true);
		size_t n = mc->block;
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

		deliver_midi(mc);
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

static void midi_out(int port, const uint8_t *bytes, size_t count, void *user)
{
	machine_t *mc = user;
	(void)port;
	midi_io_write(mc->midi, MIDI_IO_OUT, bytes, count);
}

machine_t *machine_start(const machine_options_t *o, char *err, size_t err_size)
{
	machine_t *mc = calloc(1, sizeof(*mc));
	if (!mc)
		return NULL;
	mc->opt = *o;
	mc->unit = unit_open(o->model, o->rom, o->exe_dir, o->no_cache, o->keep_settings, err, err_size);
	if (!mc->unit)
	{
		free(mc);
		return NULL;
	}
	/* the rear switch is the one the options hold for the model the unit picked */
	unit_set_computer_switch(mc->unit, computer_of(mc, unit_model(mc->unit)));
	mc->rate = unit_rate(mc->unit);
	unit_set_map(mc->unit, o->map);
	unit_set_midi_rate(mc->unit, o->midi_rate);
	open_audio(mc);
	mc->gain = 0.75f * 0.75f;
	mc->reset = MACHINE_RESET_GS;
	mc->midi = midi_io_open("scgui", midi_ports(mc));
	unit_set_midi_out(mc->unit, midi_out, mc);
	pthread_mutex_init(&mc->lock, NULL);
	set_rom_info(mc);
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
	unload_song(mc);
	audio_close(mc->audio);
	midi_io_close(mc->midi);
	unit_close(mc->unit);
	pthread_mutex_destroy(&mc->lock);
	free(mc);
}

scemu_model_t machine_model(machine_t *mc)
{
	pthread_mutex_lock(&mc->lock);
	scemu_model_t model = mc->info.model;
	pthread_mutex_unlock(&mc->lock);
	return model;
}

const char *machine_model_label(machine_t *mc)
{
	pthread_mutex_lock(&mc->lock);
	const char *label = mc->info.label;
	pthread_mutex_unlock(&mc->lock);
	return label;
}

void machine_rom_info(machine_t *mc, machine_rom_info_t *out)
{
	pthread_mutex_lock(&mc->lock);
	*out = mc->info;
	pthread_mutex_unlock(&mc->lock);
}

unsigned machine_models_available(machine_t *mc)
{
	pthread_mutex_lock(&mc->lock);
	unsigned models = mc->models;
	pthread_mutex_unlock(&mc->lock);
	return models;
}

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

void machine_dial(machine_t *mc, int steps)
{
	command_t c = { CMD_DIAL, steps, 0, 0, 0, NULL };
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

void machine_set_model(machine_t *mc, scemu_model_t model)
{
	command_t c = { CMD_MODEL, (int)model, -1, 0, 0, NULL };
	post(mc, c);
}

void machine_set_computer_switch(machine_t *mc, scemu_computer_switch_t sw)
{
	scemu_model_t model = machine_model(mc);
	int row = machine_system_index(model);
	if (row < 0)
		return;
	command_t c = { CMD_MODEL, (int)model, row, (int)sw, 0, NULL };
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

void machine_send(machine_t *mc, const uint8_t *bytes, size_t count)
{
	char *copy = malloc(count ? count : 1);
	if (!copy)
		return;
	memcpy(copy, bytes, count);
	command_t c = { CMD_SEND, (int)count, 0, 0, 0, copy };
	post(mc, c);
}

void machine_set_map(machine_t *mc, scemu_map_t map)
{
	command_t c = { CMD_MAP, map, 0, 0, 0, NULL };
	post(mc, c);
}

void machine_set_audio(machine_t *mc, int device, unsigned block, unsigned rate)
{
	command_t c = { CMD_AUDIO, device, (int)block, (int)rate, 0, NULL };
	post(mc, c);
}

void machine_midi_input(machine_t *mc, int which, int id)
{
	command_t c = { CMD_MIDI_IN, which, id, 0, 0, NULL };
	post(mc, c);
}

void machine_midi_output(machine_t *mc, int which, int id)
{
	command_t c = { CMD_MIDI_OUT, which, id, 0, 0, NULL };
	post(mc, c);
}

int machine_midi_list(machine_t *mc, struct midi_port_info *out, int max)
{
	return midi_io_list(mc->midi, out, max);
}

void machine_midi_rescan(machine_t *mc)
{
	midi_io_rescan(mc->midi);
}

int machine_midi_ports(machine_t *mc)
{
	pthread_mutex_lock(&mc->lock);
	int ports = mc->ports;
	pthread_mutex_unlock(&mc->lock);
	return ports;
}
