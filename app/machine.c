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
	CMD_LOAD, CMD_START, CMD_PAUSE, CMD_STOP, CMD_UNLOAD, CMD_SEEK, CMD_TEMPO, CMD_BUTTON, CMD_DIAL, CMD_POWER,
	CMD_GAIN, CMD_AUDIO, CMD_MIDI_IN, CMD_MIDI_OUT, CMD_RESET, CMD_MAP, CMD_MODEL, CMD_SEND, CMD_ANIMATE, CMD_QUIT
} command_kind_t;

typedef struct command
{
	command_kind_t kind;
	int a, b, c;
	float f;
	uint64_t u;
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
	bool song_started;        /* the loaded song has been started at least once */
	bool chase_pending;       /* the position moved: what the song set before it is owed to the parts */
	uint64_t pos, end_frame, lead;   /* the position on the song's clock: its frames, the lead included */
	double pos_frac;          /* the part of a song frame left over by the tempo factor */
	double tempo_factor;
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
	SCEMU_MODEL_SC55, SCEMU_MODEL_SC55MK2, SCEMU_MODEL_SC88, SCEMU_MODEL_SC88VL, SCEMU_MODEL_SC88PRO,
	SCEMU_MODEL_SC8820, SCEMU_MODEL_SC8850
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
	s.booting = booting || unit_booting(mc->unit);
	s.loaded = mc->have_smf;
	s.playing = mc->playing;
	s.paused = mc->paused;
	s.finished = mc->have_smf && !mc->playing && mc->pos >= mc->end_frame;
	s.ended = mc->have_smf && mc->song_started && mc->pos >= (uint64_t)mc->smf.last_frame + mc->lead;
	s.position = (double)mc->pos / mc->rate;
	s.length = mc->have_smf ? (double)mc->end_frame / mc->rate : 0;
	s.frame = mc->pos;
	if (mc->have_smf)
	{
		uint64_t tick = smf_tick_at_frame(&mc->smf, mc->pos > mc->lead ? mc->pos - mc->lead : 0, mc->rate);
		s.bar = smf_bar_at_tick(&mc->smf, tick);
		s.bars = smf_bar_at_tick(&mc->smf, mc->smf.last_tick);
		s.tempo = 6e7 / smf_tempo_at_tick(&mc->smf, tick);
	}
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
	    || s.loaded != mc->state.loaded || s.ended != mc->state.ended || s.bar != mc->state.bar
	    || s.bars != mc->state.bars || s.tempo != mc->state.tempo
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

/* A song asked for while the firmware is still animating its boot: the rest of
 * the boot runs at once, so the song starts on a machine that is listening. */
static void finish_boot(machine_t *mc)
{
	if (!unit_booting(mc->unit))
		return;
	unit_boot_finish(mc->unit);
	publish(mc, false);
}

static void unload_song(machine_t *mc)
{
	if (mc->have_smf)
		smf_free(&mc->smf);
	mc->have_smf = mc->playing = mc->paused = mc->song_started = mc->chase_pending = false;
	mc->pos = mc->end_frame = 0;
	mc->pos_frac = 0;
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
	mc->lead = machine_reset_message(mc->reset, &msg_size) ? mc->rate / 4 : 0;
	mc->end_frame = (uint64_t)mc->smf.last_frame + mc->lead + (uint64_t)(mc->opt.tail * mc->rate);
	set_song(mc, session_base_name(path), mc->smf.name);
}

/* The song's title on the module's display, as the Sound Brush sends it: the Sound Canvas
 * display message with the sequence name the file spells at its first tick. */
static void send_title(machine_t *mc)
{
	if (!mc->smf.raw_name[0])
		return;
	uint8_t msg[8 + 32 + 2] = { 0xf0, 0x41, 0x10, 0x45, 0x12, 0x10, 0x00, 0x00 };
	unsigned sum = 0x10;
	for (int n = 0; n < 32; n++)
	{
		uint8_t c = (uint8_t)mc->smf.raw_name[n] & 0x7f;
		msg[8 + n] = c < 0x20 ? ' ' : c;
		sum += msg[8 + n];
	}
	msg[40] = (uint8_t)((128 - sum % 128) % 128);
	msg[41] = 0xf7;
	send_both(mc, msg, sizeof(msg));
}

/* the first event at or after a position on the song's clock */
static size_t event_at(const machine_t *mc, uint64_t pos)
{
	size_t lo = 0, hi = mc->smf.count;
	while (lo < hi)
	{
		size_t mid = lo + (hi - lo) / 2;
		if ((uint64_t)mc->smf.events[mid].frame + mc->lead < pos)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

/* an event to the machine `offset` frames into the block, and to the song outputs */
static void emit(machine_t *mc, int port, const uint8_t *bytes, size_t len, uint32_t offset)
{
	scemu_midi_write(unit_machine(mc->unit), port, bytes, len, offset);
	midi_io_write(mc->midi, MIDI_IO_SONG_A + port, bytes, len);
}

static void emit_event(machine_t *mc, const smf_event_t *e, uint32_t offset)
{
	if (e->tempo_change || (e->port != SMF_PORT_UNSET && e->port >= midi_ports(mc)))
		return;
	int port = e->port == SMF_PORT_UNSET ? SCEMU_MIDI_IN_A : e->port;
	if (e->status[0] == 0xf0 && e->bytes)
	{
		emit(mc, port, e->status, 1, offset);
		emit(mc, port, e->bytes, e->length - 1u, offset);
	}
	else if (e->bytes)
		emit(mc, port, e->bytes, e->length, offset);
	else
		emit(mc, port, e->status, e->length, offset);
}

/* the events up to `until` on the song's clock; the tempo factor stretches their place in the block */
static void feed_events(machine_t *mc, uint64_t until)
{
	while (mc->next_event < mc->smf.count && mc->smf.events[mc->next_event].frame + mc->lead < until)
	{
		const smf_event_t *e = &mc->smf.events[mc->next_event++];
		uint64_t at = e->frame + mc->lead;
		double ahead = at > mc->pos ? (double)(at - mc->pos) / mc->tempo_factor : 0;
		emit_event(mc, e, (uint32_t)ahead);
	}
}

/* What the song has set before the position, sent at once, so a song joined part way
 * through sounds as it would from the start: every system exclusive in order, then per
 * channel the bank and program, each controller's last value, the last registered or
 * non-registered parameter with its data, the pitch bend and the channel pressure.  Notes
 * are left out, as the Sound Brush's MIDI Update leaves them out. */
static void chase(machine_t *mc)
{
	int ports = midi_ports(mc);
	int16_t cc[SMF_PORTS][16][128], prog[SMF_PORTS][16], bend[SMF_PORTS][16], press[SMF_PORTS][16];
	int16_t param[SMF_PORTS][16][2], data[SMF_PORTS][16][2];
	int8_t param_kind[SMF_PORTS][16];   /* 0 none, 1 RPN, 2 NRPN, the last selected */
	memset(cc, 0xff, sizeof(cc));
	memset(prog, 0xff, sizeof(prog));
	memset(bend, 0xff, sizeof(bend));
	memset(press, 0xff, sizeof(press));
	memset(param, 0xff, sizeof(param));
	memset(data, 0xff, sizeof(data));
	memset(param_kind, 0, sizeof(param_kind));
	for (size_t n = 0; n < mc->next_event; n++)
	{
		const smf_event_t *e = &mc->smf.events[n];
		if (e->tempo_change || (e->port != SMF_PORT_UNSET && e->port >= ports))
			continue;
		int port = e->port == SMF_PORT_UNSET ? SCEMU_MIDI_IN_A : e->port;
		if (e->bytes)
		{
			emit_event(mc, e, 0);
			continue;
		}
		int kind = e->status[0] & 0xf0, ch = e->status[0] & 0x0f;
		if (kind == 0xb0)
		{
			int number = e->status[1], value = e->status[2];
			if (number == 101 || number == 100 || number == 99 || number == 98)
			{
				int rpn = number >= 100 ? 1 : 2;
				if (param_kind[port][ch] != rpn)
				{
					param[port][ch][0] = param[port][ch][1] = -1;
					data[port][ch][0] = data[port][ch][1] = -1;
				}
				param_kind[port][ch] = (int8_t)rpn;
				param[port][ch][number & 1] = (int16_t)value;   /* 101 and 99 are the MSB */
				data[port][ch][0] = data[port][ch][1] = -1;
			}
			else if (number == 6 || number == 38)
				data[port][ch][number == 6 ? 0 : 1] = (int16_t)value;
			else
				cc[port][ch][number] = (int16_t)value;
		}
		else if (kind == 0xc0)
			prog[port][ch] = e->status[1];
		else if (kind == 0xe0)
			bend[port][ch] = (int16_t)(e->status[1] | (e->status[2] << 7));
		else if (kind == 0xd0)
			press[port][ch] = e->status[1];
	}
	for (int port = 0; port < ports; port++)
		for (int ch = 0; ch < 16; ch++)
		{
			uint8_t msg[3] = { (uint8_t)(0xb0 | ch), 0, 0 };
			if (cc[port][ch][0] >= 0)
			{
				msg[1] = 0; msg[2] = (uint8_t)cc[port][ch][0];
				emit(mc, port, msg, 3, 0);
			}
			if (cc[port][ch][32] >= 0)
			{
				msg[1] = 32; msg[2] = (uint8_t)cc[port][ch][32];
				emit(mc, port, msg, 3, 0);
			}
			if (prog[port][ch] >= 0)
			{
				uint8_t pc[2] = { (uint8_t)(0xc0 | ch), (uint8_t)prog[port][ch] };
				emit(mc, port, pc, 2, 0);
			}
			for (int number = 1; number < 128; number++)
				if (number != 32 && cc[port][ch][number] >= 0)
				{
					msg[1] = (uint8_t)number; msg[2] = (uint8_t)cc[port][ch][number];
					emit(mc, port, msg, 3, 0);
				}
			if (param_kind[port][ch] && (data[port][ch][0] >= 0 || data[port][ch][1] >= 0))
			{
				uint8_t msb = param_kind[port][ch] == 1 ? 101 : 99;
				for (int half = 0; half < 2; half++)
					if (param[port][ch][half] >= 0)
					{
						msg[1] = (uint8_t)(msb - half); msg[2] = (uint8_t)param[port][ch][half];
						emit(mc, port, msg, 3, 0);
					}
				for (int half = 0; half < 2; half++)
					if (data[port][ch][half] >= 0)
					{
						msg[1] = half ? 38 : 6; msg[2] = (uint8_t)data[port][ch][half];
						emit(mc, port, msg, 3, 0);
					}
			}
			if (bend[port][ch] >= 0)
			{
				uint8_t pb[3] = { (uint8_t)(0xe0 | ch), (uint8_t)(bend[port][ch] & 0x7f), (uint8_t)(bend[port][ch] >> 7) };
				emit(mc, port, pb, 3, 0);
			}
			if (press[port][ch] >= 0)
			{
				uint8_t cp[2] = { (uint8_t)(0xd0 | ch), (uint8_t)press[port][ch] };
				emit(mc, port, cp, 2, 0);
			}
		}
	mc->chase_pending = false;
}

static void quiet(machine_t *mc);

/* the loaded song from a frame of its clock: the reset and the title first, and the
 * chase owed when it is not the start */
static void start_song(machine_t *mc, uint64_t frame)
{
	if (!mc->have_smf)
		return;
	if (mc->playing && !mc->paused)
		quiet(mc);
	size_t msg_size;
	const uint8_t *msg = machine_reset_message(mc->reset, &msg_size);
	if (msg)
		send_both(mc, msg, msg_size);
	send_title(mc);
	uint64_t end = (uint64_t)mc->smf.last_frame + mc->lead;
	mc->pos = frame < end ? frame : end;
	mc->pos_frac = 0;
	mc->next_event = event_at(mc, mc->pos);
	mc->chase_pending = mc->pos > mc->lead;
	mc->playing = mc->song_started = true;
	mc->paused = false;
}

/* to the start of a bar, the song playing or not: what sounds is silenced, and the parts
 * are brought up to the new place when the song plays on */
static void seek_bar(machine_t *mc, uint32_t bar)
{
	if (!mc->have_smf)
		return;
	uint64_t last = smf_bar_at_tick(&mc->smf, mc->smf.last_tick);
	uint64_t tick = bar > last ? mc->smf.last_tick : smf_tick_of_bar(&mc->smf, bar);
	uint64_t frame = smf_frame_at_tick(&mc->smf, tick, mc->rate) + mc->lead;
	if (mc->playing && !mc->paused)
		quiet(mc);
	mc->pos = frame;
	mc->pos_frac = 0;
	mc->next_event = event_at(mc, mc->pos);
	mc->chase_pending = true;
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
	case CMD_LOAD:
		if (unit_power_on(mc->unit))
		{
			finish_boot(mc);
			load_song(mc, c->path);
		}
		break;
	case CMD_START:
		if (unit_power_on(mc->unit) && mc->have_smf)
		{
			finish_boot(mc);
			start_song(mc, c->u);
		}
		break;
	case CMD_PAUSE:
		if (unit_power_on(mc->unit) && c->a && mc->playing && !mc->paused)
			quiet(mc);
		mc->paused = c->a != 0;
		break;
	case CMD_STOP:
		if (unit_power_on(mc->unit) && mc->playing && !mc->paused)
			quiet(mc);
		mc->playing = mc->paused = false;
		break;
	case CMD_UNLOAD:
		if (unit_power_on(mc->unit) && mc->playing && !mc->paused)
			quiet(mc);
		unload_song(mc);
		set_song(mc, "", "");
		break;
	case CMD_SEEK:
		if (unit_power_on(mc->unit))
			seek_bar(mc, (uint32_t)c->a);
		break;
	case CMD_TEMPO:
		mc->tempo_factor = c->f < 0.01f ? 0.01 : c->f > 20 ? 20 : c->f;
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
	case CMD_ANIMATE:
		mc->opt.boot_animation = c->a != 0;
		unit_set_boot_live(mc->unit, mc->opt.boot_animation);
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
			if (mc->chase_pending)
				chase(mc);
			double advance = n * mc->tempo_factor + mc->pos_frac;
			uint64_t whole = (uint64_t)advance;
			mc->pos_frac = advance - (double)whole;
			feed_events(mc, mc->pos + whole);
			mc->pos += whole;
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
	unit_set_boot_live(mc->unit, o->boot_animation);
	open_audio(mc);
	mc->gain = 0.75f * 0.75f;
	mc->reset = MACHINE_RESET_GS;
	mc->tempo_factor = 1;
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
		command_t c = { CMD_QUIT, 0, 0, 0, 0, 0, NULL };
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

void machine_load(machine_t *mc, const char *path)
{
	command_t c = { CMD_LOAD, 0, 0, 0, 0, 0, strdup(path) };
	post(mc, c);
}

void machine_start_song(machine_t *mc, uint64_t frame)
{
	command_t c = { CMD_START, 0, 0, 0, 0, frame, NULL };
	post(mc, c);
}

void machine_play(machine_t *mc, const char *path)
{
	machine_load(mc, path);
	machine_start_song(mc, 0);
}

void machine_pause(machine_t *mc, bool paused)
{
	command_t c = { CMD_PAUSE, paused, 0, 0, 0, 0, NULL };
	post(mc, c);
}

void machine_stop_song(machine_t *mc)
{
	command_t c = { CMD_STOP, 0, 0, 0, 0, 0, NULL };
	post(mc, c);
}

void machine_unload(machine_t *mc)
{
	command_t c = { CMD_UNLOAD, 0, 0, 0, 0, 0, NULL };
	post(mc, c);
}

void machine_seek_bar(machine_t *mc, uint32_t bar)
{
	command_t c = { CMD_SEEK, (int)bar, 0, 0, 0, 0, NULL };
	post(mc, c);
}

void machine_set_tempo(machine_t *mc, double factor)
{
	command_t c = { CMD_TEMPO, 0, 0, 0, (float)factor, 0, NULL };
	post(mc, c);
}

void machine_button(machine_t *mc, scemu_button_t b, bool down)
{
	command_t c = { CMD_BUTTON, b, down, 0, 0, 0, NULL };
	post(mc, c);
}

void machine_dial(machine_t *mc, int steps)
{
	command_t c = { CMD_DIAL, steps, 0, 0, 0, 0, NULL };
	post(mc, c);
}

void machine_button_after(machine_t *mc, scemu_button_t b, bool down, unsigned ms)
{
	command_t c = { CMD_BUTTON, b, down, (int)(ms ? ms : 1), 0, 0, NULL };
	post(mc, c);
}

void machine_power(machine_t *mc, bool on)
{
	command_t c = { CMD_POWER, on, 0, 0, 0, 0, NULL };
	post(mc, c);
}

void machine_set_gain(machine_t *mc, float gain)
{
	command_t c = { CMD_GAIN, 0, 0, 0, gain, 0, NULL };
	post(mc, c);
}

void machine_set_boot_animation(machine_t *mc, bool on)
{
	command_t c = { CMD_ANIMATE, on, 0, 0, 0, 0, NULL };
	post(mc, c);
}

void machine_set_model(machine_t *mc, scemu_model_t model)
{
	command_t c = { CMD_MODEL, (int)model, -1, 0, 0, 0, NULL };
	post(mc, c);
}

void machine_set_computer_switch(machine_t *mc, scemu_computer_switch_t sw)
{
	scemu_model_t model = machine_model(mc);
	int row = machine_system_index(model);
	if (row < 0)
		return;
	command_t c = { CMD_MODEL, (int)model, row, (int)sw, 0, 0, NULL };
	post(mc, c);
}


const char *const machine_reset_names[MACHINE_RESET_COUNT] = {
	"nothing", "GM System On", "GS Reset", "GM2 System On", "SC-88 Mode Set, single module", "SC-88 Mode Set, double module"
};

void machine_set_reset(machine_t *mc, machine_reset_t reset)
{
	command_t c = { CMD_RESET, reset, 0, 0, 0, 0, NULL };
	post(mc, c);
}

void machine_send(machine_t *mc, const uint8_t *bytes, size_t count)
{
	char *copy = malloc(count ? count : 1);
	if (!copy)
		return;
	memcpy(copy, bytes, count);
	command_t c = { CMD_SEND, (int)count, 0, 0, 0, 0, copy };
	post(mc, c);
}

void machine_set_map(machine_t *mc, scemu_map_t map)
{
	command_t c = { CMD_MAP, map, 0, 0, 0, 0, NULL };
	post(mc, c);
}

void machine_set_audio(machine_t *mc, int device, unsigned block, unsigned rate)
{
	command_t c = { CMD_AUDIO, device, (int)block, (int)rate, 0, 0, NULL };
	post(mc, c);
}

void machine_midi_input(machine_t *mc, int which, int id)
{
	command_t c = { CMD_MIDI_IN, which, id, 0, 0, 0, NULL };
	post(mc, c);
}

void machine_midi_output(machine_t *mc, int which, int id)
{
	command_t c = { CMD_MIDI_OUT, which, id, 0, 0, 0, NULL };
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
