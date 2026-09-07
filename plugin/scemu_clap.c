/* scemu as a CLAP plugin: one instrument per model.
 *
 * An instance is one unit (play/unit.c) driven from the host's audio
 * thread through the pulled resampler (play/resample.c): host events carry
 * sample offsets, which scale by the machine's rate over the host's onto
 * scemu_midi_write; the machine's MIDI OUT comes back as note events.  The
 * unit boots when the host activates the instance, from the boot cache
 * when there is one.  Two stereo outputs (OUTPUT 1 and 2), MIDI IN A and B,
 * MIDI OUT; the volume, the map, the MIDI rate and the DAC rail as
 * parameters; the whole machine as the state.  The window (window.c) is
 * the front panel: what it does to the machine goes through a queue the
 * audio thread drains, what the machine shows comes back as the panel the
 * audio thread last saw, and the host's timer pumps it.
 *
 * Copyright (c) 2026 ian karlsson
 * SPDX-License-Identifier: BSD-3-Clause
 */
#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <clap/clap.h>
#include "scemu.h"
#include "unit.h"
#include "resample.h"
#include "roms.h"
#include "window.h"

#define VENDOR "SoundCarcass"
#define URL "https://github.com/superctr/sound-carcass"
#define PLUGIN_VERSION "0.1.0"
#define ID_PREFIX "net.superctr.scemu."

#define SOURCE_BLOCK 64            /* machine frames rendered per pull */
#define MIDI_OUT_ARENA 4096        /* sysex the machine sends within one process call */
#define STATE_MAGIC "SCEMUCL1"
#define COMMANDS 64                /* the window's hand on the machine, queued for the audio thread */
#define TIMER_MS 16

/* ---------------------------------------------------------------- the models */

typedef struct model_info
{
	scemu_model_t model;
	const char *name;          /* what the unit's ROM search wants */
	const char *id;
	const char *label;
	const char *description;
} model_info_t;

static const model_info_t model_infos[] = {
	{ SCEMU_MODEL_SC88PRO, "sc88pro", ID_PREFIX "sc88pro", "SC-88Pro",
	  "Roland SC-88Pro: the firmware and both chips emulated, the panel and display its own" },
	{ SCEMU_MODEL_SC88, "sc88", ID_PREFIX "sc88", "SC-88", "Roland SC-88" },
	{ SCEMU_MODEL_SC88VL, "sc88vl", ID_PREFIX "sc88vl", "SC-88VL", "Roland SC-88VL" },
	{ SCEMU_MODEL_SC8850, "sc8850", ID_PREFIX "sc8850", "SC-8850", "Roland SC-8850" },
	{ SCEMU_MODEL_SC55MK2, "sc55mk2", ID_PREFIX "sc55mk2", "SC-55mkII", "Roland SC-55mkII" },
};
#define MODEL_COUNT ((int)(sizeof model_infos / sizeof model_infos[0]))

static const char *const features[] = { CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_SYNTHESIZER,
                                        CLAP_PLUGIN_FEATURE_STEREO, NULL };
static clap_plugin_descriptor_t descriptors[MODEL_COUNT];
static char plugin_dir[1024];    /* where the host loaded us from: ROMs are looked for beside it */

/* ---------------------------------------------------------------- parameters */

enum { PARAM_VOLUME, PARAM_MAP, PARAM_MIDI_RATE, PARAM_RAIL, PARAM_COUNT };

static const char *const map_names[] = { "the song's own", "SC-55", "SC-88", "SC-88Pro", "SC-8850" };
static const char *const midi_rate_names[] = { "31250 baud (MIDI)", "38400 baud (computer port)", "unlimited" };
static const uint32_t midi_rates[] = { 31250, 38400, 0 };

static const struct
{
	const char *name;
	double min, max, def;
	uint32_t flags;
} param_infos[PARAM_COUNT] = {
	[PARAM_VOLUME] = { "Volume", 0, 1, 0.75, CLAP_PARAM_IS_AUTOMATABLE },
	[PARAM_MAP] = { "Instrument map", 0, 4, 0, CLAP_PARAM_IS_STEPPED | CLAP_PARAM_IS_ENUM | CLAP_PARAM_IS_AUTOMATABLE },
	[PARAM_MIDI_RATE] = { "MIDI input speed", 0, 2, 0, CLAP_PARAM_IS_STEPPED | CLAP_PARAM_IS_ENUM },
	[PARAM_RAIL] = { "Output headroom (DAC rail bits)", 24, 29, 24, CLAP_PARAM_IS_STEPPED },
};

/* ---------------------------------------------------------------- the instance */

enum { CMD_KEY, CMD_KEY_AFTER, CMD_DIAL };

typedef struct command
{
	uint8_t kind;
	bool down;
	scemu_button_t button;
	int steps;
	unsigned ms;
} command_t;

typedef struct instance
{
	clap_plugin_t plugin;
	const clap_host_t *host;
	const clap_host_log_t *log;
	const clap_host_params_t *host_params;
	const clap_host_gui_t *host_gui;
	const clap_host_timer_support_t *host_timer;
	const model_info_t *info;
	unit_t *unit;              /* NULL when the ROM set is not there; rom_error says */
	char rom_error[256];
	uint32_t rate;             /* the machine's */
	double host_rate;
	int channels;              /* 2 or 4: the machine's output pairs through one converter */
	resample_t *rs;
	float *out;                /* a host block of converted frames, interleaved */
	uint32_t max_frames;
	bool active;
	/* the unit: the audio thread holds it through a process call, the main
	 * thread for a state load or a parameter flush */
	pthread_mutex_t lock;
	double params[PARAM_COUNT];
	float gain;
	uint64_t mframes, hframes; /* machine frames rendered, host frames delivered */
	double block_start;        /* the machine frame the current host block starts at */
	/* the source's staging: one pull's frames */
	int32_t raw[2][SOURCE_BLOCK * 2];
	float staged[SOURCE_BLOCK * 4];
	/* what the machine sends during a process call: it comes a byte at a
	 * time and leaves as messages */
	const clap_process_t *proc;
	uint8_t arena[MIDI_OUT_ARENA];
	size_t arena_used;
	uint8_t msg[MIDI_OUT_ARENA];
	size_t msg_len, msg_want;   /* the message being assembled and its length once known */
	uint8_t running;
	/* a state loaded before the unit was on: applied after the boot */
	uint8_t *pending_state;
	size_t pending_state_size;
	/* the window, pumped by the host's timer */
	window_t *window;
	clap_id timer;
	bool timer_on;
	_Atomic bool processing;
	/* what the window does to the machine: the main thread posts, the audio
	 * thread drains at the start of a process call (the main thread itself
	 * when the host is not processing) */
	command_t commands[COMMANDS];
	_Atomic uint32_t cmd_head, cmd_tail;
	/* the panel as the machine last showed it, for the window */
	pthread_mutex_t panel_lock;
	unit_panel_t panel;
	uint32_t panel_serial, panel_shown;
	/* the volume knob turned in the window: the host hears at the next process or flush */
	_Atomic bool knob_moved;
	double knob_value;
} instance_t;

static void logf_(instance_t *in, clap_log_severity severity, const char *fmt, ...)
{
	char msg[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof msg, fmt, ap);
	va_end(ap);
	if (in->log)
		in->log->log(in->host, severity, msg);
	else
		fprintf(stderr, "scemu: %s\n", msg);
}

static int midi_ports(const instance_t *in)
{
	return in->unit ? unit_midi_ports(in->unit) : 2;
}

static void apply_param(instance_t *in, int id, double value)
{
	if (id < 0 || id >= PARAM_COUNT)
		return;
	if (value < param_infos[id].min)
		value = param_infos[id].min;
	if (value > param_infos[id].max)
		value = param_infos[id].max;
	if (param_infos[id].flags & CLAP_PARAM_IS_STEPPED)
		value = (double)(long)(value + 0.5);
	in->params[id] = value;
	switch (id)
	{
	case PARAM_VOLUME:
		in->gain = (float)(value * value);   /* the players' knob law */
		break;
	case PARAM_MAP:
		if (in->unit)
			unit_set_map(in->unit, (scemu_map_t)(int)value);
		break;
	case PARAM_MIDI_RATE:
		if (in->unit)
			unit_set_midi_rate(in->unit, midi_rates[(int)value]);
		break;
	case PARAM_RAIL:
		if (in->unit)
			unit_set_dac_rail(in->unit, (int)value);
		break;
	}
}

static void apply_all_params(instance_t *in)
{
	for (int id = 0; id < PARAM_COUNT; id++)
		apply_param(in, id, in->params[id]);
}

/* ---------------------------------------------------------------- the window's side */

/* with the unit held: the panel for the window, when it changed or when asked */
static void publish_panel(instance_t *in, bool force)
{
	if (!in->unit)
		return;
	unit_panel_t p;
	bool changed = unit_panel(in->unit, &p);
	if (!changed && !force)
		return;
	pthread_mutex_lock(&in->panel_lock);
	in->panel = p;
	in->panel_serial++;
	pthread_mutex_unlock(&in->panel_lock);
}

/* with the unit held */
static void drain_commands(instance_t *in)
{
	uint32_t tail = atomic_load_explicit(&in->cmd_tail, memory_order_relaxed);
	uint32_t head = atomic_load_explicit(&in->cmd_head, memory_order_acquire);
	while (tail != head)
	{
		const command_t *c = &in->commands[tail % COMMANDS];
		if (in->unit)
			switch (c->kind)
			{
			case CMD_KEY: unit_key(in->unit, c->button, c->down); break;
			case CMD_KEY_AFTER: unit_key_after(in->unit, c->button, c->down, c->ms); break;
			case CMD_DIAL: unit_dial(in->unit, c->steps); break;
			}
		tail++;
	}
	atomic_store_explicit(&in->cmd_tail, tail, memory_order_release);
}

/* main thread */
static void post_command(instance_t *in, command_t c)
{
	uint32_t head = atomic_load_explicit(&in->cmd_head, memory_order_relaxed);
	uint32_t tail = atomic_load_explicit(&in->cmd_tail, memory_order_acquire);
	if (head - tail < COMMANDS)
	{
		in->commands[head % COMMANDS] = c;
		atomic_store_explicit(&in->cmd_head, head + 1, memory_order_release);
	}
	if (!atomic_load_explicit(&in->processing, memory_order_acquire))
	{
		pthread_mutex_lock(&in->lock);
		drain_commands(in);
		publish_panel(in, false);
		pthread_mutex_unlock(&in->lock);
	}
}

/* the knob the window turned, to the host, on the audio thread or in a flush */
static void report_knob(instance_t *in, const clap_output_events_t *out)
{
	if (!out || !atomic_load_explicit(&in->knob_moved, memory_order_acquire))
		return;
	atomic_store_explicit(&in->knob_moved, false, memory_order_relaxed);
	clap_event_param_gesture_t g;
	memset(&g, 0, sizeof g);
	g.header.size = sizeof g;
	g.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
	g.header.type = CLAP_EVENT_PARAM_GESTURE_BEGIN;
	g.param_id = PARAM_VOLUME;
	out->try_push(out, &g.header);
	clap_event_param_value_t e;
	memset(&e, 0, sizeof e);
	e.header.size = sizeof e;
	e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
	e.header.type = CLAP_EVENT_PARAM_VALUE;
	e.param_id = PARAM_VOLUME;
	e.note_id = -1;
	e.port_index = -1;
	e.channel = -1;
	e.key = -1;
	e.value = in->knob_value;
	out->try_push(out, &e.header);
	g.header.type = CLAP_EVENT_PARAM_GESTURE_END;
	out->try_push(out, &g.header);
}

/* ---------------------------------------------------------------- MIDI out */

static void push_message(instance_t *in, int port, const uint8_t *bytes, size_t count)
{
	const clap_process_t *p = in->proc;
	if (!p || !p->out_events || !count)
		return;
	double t = ((double)in->mframes - in->block_start) * in->host_rate / in->rate;
	uint32_t time = t <= 0 ? 0 : (uint32_t)t;
	if (p->frames_count && time >= p->frames_count)
		time = p->frames_count - 1;
	if (bytes[0] == 0xf0)
	{
		if (in->arena_used + count > sizeof in->arena)
			return;
		clap_event_midi_sysex_t e;
		memset(&e, 0, sizeof e);
		e.header.size = sizeof e;
		e.header.time = time;
		e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
		e.header.type = CLAP_EVENT_MIDI_SYSEX;
		e.port_index = (uint16_t)port;
		memcpy(in->arena + in->arena_used, bytes, count);
		e.buffer = in->arena + in->arena_used;
		e.size = (uint32_t)count;
		in->arena_used += count;
		p->out_events->try_push(p->out_events, &e.header);
		return;
	}
	clap_event_midi_t e;
	memset(&e, 0, sizeof e);
	e.header.size = sizeof e;
	e.header.time = time;
	e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
	e.header.type = CLAP_EVENT_MIDI;
	e.port_index = (uint16_t)port;
	memcpy(e.data, bytes, count > 3 ? 3 : count);
	p->out_events->try_push(p->out_events, &e.header);
}

static size_t midi_length(uint8_t status)
{
	switch (status & 0xf0)
	{
	case 0xc0: case 0xd0: return 2;
	case 0xf0:
		switch (status)
		{
		case 0xf1: case 0xf3: return 2;
		case 0xf2: return 3;
		default: return 1;
		}
	default: return 3;
	}
}

/* the machine's MIDI OUT, a byte at a time: running status is resolved,
 * a sysex is collected up to its F7, real-time bytes pass in between */
static void midi_out(int port, const uint8_t *bytes, size_t count, void *user)
{
	instance_t *in = user;
	for (size_t k = 0; k < count; k++)
	{
		uint8_t b = bytes[k];
		if (b >= 0xf8)
		{
			push_message(in, port, &b, 1);
			continue;
		}
		if (b == 0xf7)
		{
			if (in->msg_len && in->msg[0] == 0xf0 && in->msg_len < sizeof in->msg)
			{
				in->msg[in->msg_len++] = b;
				push_message(in, port, in->msg, in->msg_len);
			}
			in->msg_len = in->msg_want = 0;
			continue;
		}
		if (b & 0x80)
		{
			in->msg_len = 0;
			in->msg[in->msg_len++] = b;
			in->msg_want = b == 0xf0 ? 0 : midi_length(b);
			in->running = b < 0xf0 ? b : 0;
		}
		else if (!in->msg_len)
		{
			if (!in->running)
				continue;
			in->msg[in->msg_len++] = in->running;
			in->msg_want = midi_length(in->running);
			if (in->msg_len < sizeof in->msg)
				in->msg[in->msg_len++] = b;
		}
		else if (in->msg_len < sizeof in->msg)
			in->msg[in->msg_len++] = b;
		if (in->msg_want && in->msg_len >= in->msg_want)
		{
			push_message(in, port, in->msg, in->msg_len);
			in->msg_len = 0;
		}
	}
}

/* ---------------------------------------------------------------- rendering */

/* the resampler's source: a pull of machine frames, both pairs interleaved,
 * scaled to float by the 24-bit full scale, the knob and the model's trim */
static size_t source(void *user, const float **frames, size_t want)
{
	instance_t *in = user;
	size_t n = SOURCE_BLOCK;
	if (want && want < n)
		n = want;
	int32_t *const out[2] = { in->raw[0], in->channels == 4 ? in->raw[1] : NULL };
	unit_render(in->unit, out, n);
	const float scale = in->gain * unit_output_trim(in->info->model) * (1.0f / 8388608);
	if (in->channels == 4)
		for (size_t k = 0; k < n; k++)
		{
			in->staged[4 * k] = (float)in->raw[0][2 * k] * scale;
			in->staged[4 * k + 1] = (float)in->raw[0][2 * k + 1] * scale;
			in->staged[4 * k + 2] = (float)in->raw[1][2 * k] * scale;
			in->staged[4 * k + 3] = (float)in->raw[1][2 * k + 1] * scale;
		}
	else
		for (size_t k = 0; k < 2 * n; k++)
			in->staged[k] = (float)in->raw[0][k] * scale;
	in->mframes += n;
	*frames = in->staged;
	return n;
}

/* a host offset onto the machine's clock, relative to its next render */
static uint32_t machine_offset(const instance_t *in, uint32_t time)
{
	double at = in->block_start + (double)time * in->rate / in->host_rate - (double)in->mframes;
	return at <= 0 ? 0 : (uint32_t)(at + 0.5);
}

static void take_event(instance_t *in, const clap_event_header_t *h)
{
	if (h->space_id != CLAP_CORE_EVENT_SPACE_ID)
		return;
	scemu_t *m = in->unit ? unit_machine(in->unit) : NULL;
	switch (h->type)
	{
	case CLAP_EVENT_PARAM_VALUE:
	{
		const clap_event_param_value_t *e = (const clap_event_param_value_t *)h;
		apply_param(in, (int)e->param_id, e->value);
		break;
	}
	case CLAP_EVENT_MIDI:
	{
		const clap_event_midi_t *e = (const clap_event_midi_t *)h;
		if (m && e->port_index < midi_ports(in))
			scemu_midi_write(m, e->port_index, e->data, midi_length(e->data[0]), machine_offset(in, h->time));
		break;
	}
	case CLAP_EVENT_MIDI_SYSEX:
	{
		const clap_event_midi_sysex_t *e = (const clap_event_midi_sysex_t *)h;
		if (m && e->port_index < midi_ports(in) && e->buffer)
			scemu_midi_write(m, e->port_index, e->buffer, e->size, machine_offset(in, h->time));
		break;
	}
	case CLAP_EVENT_NOTE_ON:
	case CLAP_EVENT_NOTE_OFF:
	case CLAP_EVENT_NOTE_CHOKE:
	{
		/* the MIDI dialect is what the ports ask for; a host that sends
		 * note events anyway gets them as channel messages */
		const clap_event_note_t *e = (const clap_event_note_t *)h;
		if (!m || e->port_index >= midi_ports(in) || e->channel < 0 || e->key < 0)
			break;
		int port = e->port_index < 0 ? 0 : e->port_index;
		int v = (int)(e->velocity * 127 + 0.5);
		uint8_t msg[3] = { (uint8_t)((h->type == CLAP_EVENT_NOTE_ON ? 0x90 : 0x80) | (e->channel & 15)),
		                   (uint8_t)(e->key & 127), (uint8_t)(h->type == CLAP_EVENT_NOTE_ON ? (v ? v : 1) : 64) };
		scemu_midi_write(m, port, msg, 3, machine_offset(in, h->time));
		break;
	}
	default:
		break;
	}
}

static void silence(const clap_process_t *p)
{
	for (uint32_t port = 0; port < p->audio_outputs_count; port++)
	{
		clap_audio_buffer_t *b = &p->audio_outputs[port];
		for (uint32_t ch = 0; ch < b->channel_count; ch++)
		{
			if (b->data32 && b->data32[ch])
				memset(b->data32[ch], 0, p->frames_count * sizeof(float));
			if (b->data64 && b->data64[ch])
				memset(b->data64[ch], 0, p->frames_count * sizeof(double));
		}
		b->constant_mask = 0;
	}
}

static clap_process_status plugin_process(const clap_plugin_t *plugin, const clap_process_t *p)
{
	instance_t *in = plugin->plugin_data;
	silence(p);
	if (!in->unit || !in->rs || !in->out)
	{
		/* parameters still move, so the host's view stays right */
		uint32_t n = p->in_events ? p->in_events->size(p->in_events) : 0;
		for (uint32_t k = 0; k < n; k++)
		{
			const clap_event_header_t *h = p->in_events->get(p->in_events, k);
			if (h->space_id == CLAP_CORE_EVENT_SPACE_ID && h->type == CLAP_EVENT_PARAM_VALUE)
				take_event(in, h);
		}
		return CLAP_PROCESS_CONTINUE;
	}
	if (pthread_mutex_trylock(&in->lock) != 0)
		return CLAP_PROCESS_CONTINUE;   /* the main thread is inside the unit: a block of silence */

	in->block_start = (double)in->hframes * in->rate / in->host_rate;
	in->proc = p;
	in->arena_used = 0;
	drain_commands(in);
	report_knob(in, p->out_events);
	uint32_t n = p->in_events ? p->in_events->size(p->in_events) : 0;
	for (uint32_t k = 0; k < n; k++)
		take_event(in, p->in_events->get(p->in_events, k));

	uint32_t frames = p->frames_count;
	if (frames > in->max_frames)
		frames = in->max_frames;
	size_t made = resample_read(in->rs, in->out, frames);
	for (uint32_t port = 0; port < p->audio_outputs_count && port < 2; port++)
	{
		clap_audio_buffer_t *b = &p->audio_outputs[port];
		if (!b->data32 || b->channel_count < 2)
			continue;
		int base = (int)port * 2;
		if (port == 1 && in->channels < 4)
			break;   /* one output pair on this model: OUTPUT 2 stays silent */
		for (size_t k = 0; k < made; k++)
		{
			b->data32[0][k] = in->out[k * in->channels + base];
			b->data32[1][k] = in->out[k * in->channels + base + 1];
		}
	}
	in->hframes += frames;
	in->proc = NULL;
	publish_panel(in, false);
	pthread_mutex_unlock(&in->lock);
	return CLAP_PROCESS_CONTINUE;
}

/* ---------------------------------------------------------------- lifetime */

static bool plugin_init(const clap_plugin_t *plugin)
{
	instance_t *in = plugin->plugin_data;
	in->log = in->host->get_extension(in->host, CLAP_EXT_LOG);
	in->host_params = in->host->get_extension(in->host, CLAP_EXT_PARAMS);
	in->host_gui = in->host->get_extension(in->host, CLAP_EXT_GUI);
	in->host_timer = in->host->get_extension(in->host, CLAP_EXT_TIMER_SUPPORT);
	const char *rom_path = getenv("SCEMU_ROMS");
	in->unit = unit_open(in->info->name, rom_path, plugin_dir[0] ? plugin_dir : NULL, false, false,
	                     in->rom_error, sizeof in->rom_error);
	if (!in->unit)
	{
		logf_(in, CLAP_LOG_WARNING, "%s: %s (put the ROM set beside the plugin, in ~/.mame/roms, or name it in SCEMU_ROMS)",
		      in->info->label, in->rom_error);
		return true;   /* the instance stands, silent, and its window will say why */
	}
	in->rate = unit_rate(in->unit);
	in->channels = scemu_output_count(unit_machine(in->unit)) > 1 ? 4 : 2;
	unit_set_midi_out(in->unit, midi_out, in);
	apply_all_params(in);
	return true;
}

static void gui_destroy(const clap_plugin_t *plugin);

static void plugin_destroy(const clap_plugin_t *plugin)
{
	instance_t *in = plugin->plugin_data;
	if (in->window)
		gui_destroy(plugin);
	resample_close(in->rs);
	free(in->out);
	unit_close(in->unit);
	free(in->pending_state);
	pthread_mutex_destroy(&in->lock);
	pthread_mutex_destroy(&in->panel_lock);
	free(in);
}

static void apply_pending_state(instance_t *in)
{
	if (!in->pending_state || !in->unit || !unit_power_on(in->unit))
		return;
	if (!scemu_state_load(unit_machine(in->unit), in->pending_state, in->pending_state_size))
		logf_(in, CLAP_LOG_WARNING, "%s: the saved machine state does not fit this ROM set; starting fresh",
		      in->info->label);
	free(in->pending_state);
	in->pending_state = NULL;
	in->pending_state_size = 0;
}

static bool plugin_activate(const clap_plugin_t *plugin, double sample_rate, uint32_t min_frames, uint32_t max_frames)
{
	instance_t *in = plugin->plugin_data;
	(void)min_frames;
	in->host_rate = sample_rate;
	in->max_frames = max_frames;
	in->active = true;
	if (!in->unit)
		return true;
	pthread_mutex_lock(&in->lock);
	if (!unit_power_on(in->unit))
	{
		unit_boot(in->unit, true, NULL, NULL);
		apply_all_params(in);
		apply_pending_state(in);
	}
	publish_panel(in, true);
	resample_close(in->rs);
	free(in->out);
	in->rs = resample_open(in->rate, (uint32_t)(sample_rate + 0.5), in->channels, source, in);
	in->out = malloc((size_t)max_frames * in->channels * sizeof(float));
	in->hframes = in->mframes = 0;
	pthread_mutex_unlock(&in->lock);
	if (!in->rs || !in->out)
	{
		logf_(in, CLAP_LOG_ERROR, "%s: cannot convert %u Hz to %.0f Hz", in->info->label, in->rate, sample_rate);
		return false;
	}
	return true;
}

static void plugin_deactivate(const clap_plugin_t *plugin)
{
	instance_t *in = plugin->plugin_data;
	in->active = false;
	/* the unit stays on: the machine keeps its state and the next activation is instant */
}

static bool plugin_start_processing(const clap_plugin_t *plugin)
{
	instance_t *in = plugin->plugin_data;
	atomic_store_explicit(&in->processing, true, memory_order_release);
	return true;
}

static void plugin_stop_processing(const clap_plugin_t *plugin)
{
	instance_t *in = plugin->plugin_data;
	atomic_store_explicit(&in->processing, false, memory_order_release);
}

static void plugin_reset(const clap_plugin_t *plugin)
{
	instance_t *in = plugin->plugin_data;
	if (!in->unit)
		return;
	pthread_mutex_lock(&in->lock);
	for (int ch = 0; ch < 16; ch++)
	{
		uint8_t off[6] = { (uint8_t)(0xb0 | ch), 0x7b, 0x00, (uint8_t)(0xb0 | ch), 0x78, 0x00 };
		unit_send(in->unit, off, sizeof off);
	}
	if (in->rs)
		resample_reset(in->rs);
	pthread_mutex_unlock(&in->lock);
}

static void plugin_on_main_thread(const clap_plugin_t *plugin) { (void)plugin; }

/* ---------------------------------------------------------------- audio ports */

static uint32_t audio_ports_count(const clap_plugin_t *plugin, bool is_input)
{
	(void)plugin;
	return is_input ? 0 : 2;
}

static bool audio_ports_get(const clap_plugin_t *plugin, uint32_t index, bool is_input, clap_audio_port_info_t *info)
{
	(void)plugin;
	if (is_input || index > 1)
		return false;
	info->id = index;
	snprintf(info->name, sizeof info->name, "OUTPUT %u", index + 1);
	info->flags = index == 0 ? CLAP_AUDIO_PORT_IS_MAIN : 0;
	info->channel_count = 2;
	info->port_type = CLAP_PORT_STEREO;
	info->in_place_pair = CLAP_INVALID_ID;
	return true;
}

static const clap_plugin_audio_ports_t audio_ports_ext = { audio_ports_count, audio_ports_get };

/* ---------------------------------------------------------------- note ports */

static uint32_t note_ports_count(const clap_plugin_t *plugin, bool is_input)
{
	instance_t *in = plugin->plugin_data;
	return is_input ? (uint32_t)midi_ports(in) : 1;
}

static bool note_ports_get(const clap_plugin_t *plugin, uint32_t index, bool is_input, clap_note_port_info_t *info)
{
	instance_t *in = plugin->plugin_data;
	if (index >= note_ports_count(plugin, is_input))
		return false;
	info->id = index;
	info->supported_dialects = CLAP_NOTE_DIALECT_MIDI;
	info->preferred_dialect = CLAP_NOTE_DIALECT_MIDI;
	if (is_input)
		snprintf(info->name, sizeof info->name, "MIDI IN %c", 'A' + (int)index);
	else
		snprintf(info->name, sizeof info->name, "MIDI OUT");
	(void)in;
	return true;
}

static const clap_plugin_note_ports_t note_ports_ext = { note_ports_count, note_ports_get };

/* ---------------------------------------------------------------- parameters */

static uint32_t params_count(const clap_plugin_t *plugin) { (void)plugin; return PARAM_COUNT; }

static bool params_get_info(const clap_plugin_t *plugin, uint32_t index, clap_param_info_t *info)
{
	(void)plugin;
	if (index >= PARAM_COUNT)
		return false;
	memset(info, 0, sizeof *info);
	info->id = index;
	info->flags = param_infos[index].flags;
	snprintf(info->name, sizeof info->name, "%s", param_infos[index].name);
	info->min_value = param_infos[index].min;
	info->max_value = param_infos[index].max;
	info->default_value = param_infos[index].def;
	return true;
}

static bool params_get_value(const clap_plugin_t *plugin, clap_id id, double *value)
{
	instance_t *in = plugin->plugin_data;
	if (id >= PARAM_COUNT)
		return false;
	*value = in->params[id];
	return true;
}

static bool params_value_to_text(const clap_plugin_t *plugin, clap_id id, double value, char *out, uint32_t size)
{
	(void)plugin;
	int v = (int)(value + 0.5);
	switch (id)
	{
	case PARAM_VOLUME:
		snprintf(out, size, "%d %%", (int)(value * 100 + 0.5));
		return true;
	case PARAM_MAP:
		snprintf(out, size, "%s", map_names[v < 0 ? 0 : v > 4 ? 4 : v]);
		return true;
	case PARAM_MIDI_RATE:
		snprintf(out, size, "%s", midi_rate_names[v < 0 ? 0 : v > 2 ? 2 : v]);
		return true;
	case PARAM_RAIL:
		snprintf(out, size, v <= 24 ? "%d bits (the unit's)" : "%d bits (+%d dB)", v, (v - 24) * 6);
		return true;
	default:
		return false;
	}
}

static bool params_text_to_value(const clap_plugin_t *plugin, clap_id id, const char *text, double *value)
{
	(void)plugin;
	if (id >= PARAM_COUNT)
		return false;
	const char *const *names = id == PARAM_MAP ? map_names : id == PARAM_MIDI_RATE ? midi_rate_names : NULL;
	if (names)
	{
		int count = id == PARAM_MAP ? 5 : 3;
		for (int n = 0; n < count; n++)
			if (!strcmp(names[n], text))
			{
				*value = n;
				return true;
			}
	}
	char *end;
	double v = strtod(text, &end);
	if (end == text)
		return false;
	*value = id == PARAM_VOLUME ? v / 100 : v;
	return true;
}

static void params_flush(const clap_plugin_t *plugin, const clap_input_events_t *events, const clap_output_events_t *out)
{
	instance_t *in = plugin->plugin_data;
	report_knob(in, out);
	uint32_t n = events ? events->size(events) : 0;
	if (!n)
		return;
	pthread_mutex_lock(&in->lock);
	for (uint32_t k = 0; k < n; k++)
	{
		const clap_event_header_t *h = events->get(events, k);
		if (h->space_id == CLAP_CORE_EVENT_SPACE_ID && h->type == CLAP_EVENT_PARAM_VALUE)
			take_event(in, h);
	}
	pthread_mutex_unlock(&in->lock);
}

static const clap_plugin_params_t params_ext = {
	params_count, params_get_info, params_get_value, params_value_to_text, params_text_to_value, params_flush
};

/* ---------------------------------------------------------------- state */

static bool write_all(const clap_ostream_t *s, const void *data, size_t size)
{
	const uint8_t *p = data;
	while (size)
	{
		int64_t n = s->write(s, p, size);
		if (n <= 0)
			return false;
		p += n;
		size -= (size_t)n;
	}
	return true;
}

static bool read_all(const clap_istream_t *s, void *data, size_t size)
{
	uint8_t *p = data;
	while (size)
	{
		int64_t n = s->read(s, p, size);
		if (n <= 0)
			return false;
		p += n;
		size -= (size_t)n;
	}
	return true;
}

static void put_u32(uint8_t *p, uint32_t v) { for (int n = 0; n < 4; n++) p[n] = (uint8_t)(v >> (8 * n)); }
static uint32_t get_u32(const uint8_t *p) { uint32_t v = 0; for (int n = 3; n >= 0; n--) v = (v << 8) | p[n]; return v; }
static void put_f64(uint8_t *p, double v) { uint64_t u; memcpy(&u, &v, 8); for (int n = 0; n < 8; n++) p[n] = (uint8_t)(u >> (8 * n)); }
static double get_f64(const uint8_t *p) { uint64_t u = 0; for (int n = 7; n >= 0; n--) u = (u << 8) | p[n]; double v; memcpy(&v, &u, 8); return v; }

/* the magic, the model, the parameters, then the machine's own state stream */
static bool state_save(const clap_plugin_t *plugin, const clap_ostream_t *stream)
{
	instance_t *in = plugin->plugin_data;
	uint8_t head[8 + 4 + 4 + 8 * PARAM_COUNT + 4];
	memcpy(head, STATE_MAGIC, 8);
	put_u32(head + 8, (uint32_t)in->info->model);
	put_u32(head + 12, PARAM_COUNT);
	for (int n = 0; n < PARAM_COUNT; n++)
		put_f64(head + 16 + 8 * n, in->params[n]);
	uint8_t *state = NULL;
	size_t size = 0;
	if (in->unit && unit_power_on(in->unit))
	{
		pthread_mutex_lock(&in->lock);
		size = scemu_state_size(unit_machine(in->unit));
		state = malloc(size ? size : 1);
		if (state)
			size = scemu_state_save(unit_machine(in->unit), state, size);
		pthread_mutex_unlock(&in->lock);
	}
	else if (in->pending_state)
	{
		size = in->pending_state_size;
		state = malloc(size);
		if (state)
			memcpy(state, in->pending_state, size);
	}
	put_u32(head + 16 + 8 * PARAM_COUNT, (uint32_t)size);
	bool ok = write_all(stream, head, sizeof head) && (!size || write_all(stream, state, size));
	free(state);
	return ok;
}

static bool state_load(const clap_plugin_t *plugin, const clap_istream_t *stream)
{
	instance_t *in = plugin->plugin_data;
	uint8_t head[16];
	if (!read_all(stream, head, sizeof head) || memcmp(head, STATE_MAGIC, 8) != 0)
		return false;
	if (get_u32(head + 8) != (uint32_t)in->info->model)
		return false;
	uint32_t count = get_u32(head + 12);
	if (count > 64)
		return false;
	double values[64];
	uint8_t raw[8 * 64 + 4];
	if (!read_all(stream, raw, 8 * count + 4))
		return false;
	for (uint32_t n = 0; n < count; n++)
		values[n] = get_f64(raw + 8 * n);
	uint32_t size = get_u32(raw + 8 * count);
	uint8_t *state = NULL;
	if (size)
	{
		state = malloc(size);
		if (!state || !read_all(stream, state, size))
		{
			free(state);
			return false;
		}
	}
	pthread_mutex_lock(&in->lock);
	for (uint32_t n = 0; n < count && n < PARAM_COUNT; n++)
		apply_param(in, (int)n, values[n]);
	free(in->pending_state);
	in->pending_state = state;
	in->pending_state_size = size;
	apply_pending_state(in);
	publish_panel(in, true);
	pthread_mutex_unlock(&in->lock);
	if (in->host_params)
		in->host_params->rescan(in->host, CLAP_PARAM_RESCAN_VALUES);
	return true;
}

static const clap_plugin_state_t state_ext = { state_save, state_load };

/* ---------------------------------------------------------------- the window */

static void act_key(void *user, scemu_button_t b, bool down)
{
	post_command(user, (command_t){ .kind = CMD_KEY, .button = b, .down = down });
}

static void act_key_after(void *user, scemu_button_t b, bool down, unsigned ms)
{
	post_command(user, (command_t){ .kind = CMD_KEY_AFTER, .button = b, .down = down, .ms = ms });
}

static void act_dial(void *user, int steps)
{
	post_command(user, (command_t){ .kind = CMD_DIAL, .steps = steps });
}

/* the hard power switch: off stands the machine still; on is a cold boot,
 * the keys held, here on the main thread with the audio thread silent
 * meanwhile */
static void act_power(void *user, bool on)
{
	instance_t *in = user;
	if (!in->unit)
		return;
	pthread_mutex_lock(&in->lock);
	unit_power(in->unit, on);
	if (on)
	{
		apply_all_params(in);
		apply_pending_state(in);
	}
	publish_panel(in, true);
	pthread_mutex_unlock(&in->lock);
	if (on && in->window)
		window_boot_done(in->window);
}

static void act_knob(void *user, float turn)
{
	instance_t *in = user;
	in->params[PARAM_VOLUME] = turn;
	in->gain = turn * turn;
	in->knob_value = turn;
	atomic_store_explicit(&in->knob_moved, true, memory_order_release);
	if (in->host_params && in->host_params->request_flush)
		in->host_params->request_flush(in->host);
}

static void act_element(void *user, panel_element_t e, double x, double y) {}
static void act_combo_menu(void *user, panel_element_t e, double x, double y) {}

static void act_closed(void *user)
{
	instance_t *in = user;
	if (in->host_gui && in->host_gui->closed)
		in->host_gui->closed(in->host, false);
}

static const window_actions_t window_actions = {
	{ act_key, act_key_after, act_dial, act_power, act_knob, act_element, act_combo_menu }, act_closed
};

static void on_timer(const clap_plugin_t *plugin, clap_id timer_id)
{
	instance_t *in = plugin->plugin_data;
	if (!in->window || timer_id != in->timer)
		return;
	pthread_mutex_lock(&in->panel_lock);
	if (in->panel_shown != in->panel_serial)
	{
		unit_panel_t p = in->panel;
		in->panel_shown = in->panel_serial;
		pthread_mutex_unlock(&in->panel_lock);
		window_set_panel(in->window, &p);
	}
	else
		pthread_mutex_unlock(&in->panel_lock);
	window_set_knob(in->window, (float)in->params[PARAM_VOLUME]);
	window_tick(in->window);
}

static const clap_plugin_timer_support_t timer_ext = { on_timer };

static bool gui_is_api_supported(const clap_plugin_t *plugin, const char *api, bool is_floating)
{
	return !strcmp(api, window_api());
}

static bool gui_get_preferred_api(const clap_plugin_t *plugin, const char **api, bool *is_floating)
{
	*api = window_api();
	*is_floating = false;
	return true;
}

static bool gui_create(const clap_plugin_t *plugin, const char *api, bool is_floating)
{
	instance_t *in = plugin->plugin_data;
	if (strcmp(api, window_api()) != 0 || in->window)
		return false;
	if (!in->host_timer || !in->host_timer->register_timer)
	{
		logf_(in, CLAP_LOG_WARNING, "%s: the host has no timers, so no window", in->info->label);
		return false;
	}
	char err[128];
	in->window = window_create(in->info->model, 1.0, &window_actions, in, err, sizeof err);
	if (!in->window)
	{
		logf_(in, CLAP_LOG_WARNING, "%s: no window: %s", in->info->label, err);
		return false;
	}
	if (!in->unit)
	{
		char text[512];
		snprintf(text, sizeof text, "%s\n\n%s\n\nPut the ROM set beside the plugin or in .mame/roms under your home, or name it in SCEMU_ROMS, and load the plugin again.",
		         in->info->label, in->rom_error);
		window_set_message(in->window, text);
	}
	else
	{
		pthread_mutex_lock(&in->lock);
		publish_panel(in, true);
		pthread_mutex_unlock(&in->lock);
	}
	in->timer_on = in->host_timer->register_timer(in->host, TIMER_MS, &in->timer);
	if (!in->timer_on)
		logf_(in, CLAP_LOG_WARNING, "%s: the host gave no timer: the window will not move", in->info->label);
	return true;
}

static void gui_destroy(const clap_plugin_t *plugin)
{
	instance_t *in = plugin->plugin_data;
	if (in->timer_on)
		in->host_timer->unregister_timer(in->host, in->timer);
	in->timer_on = false;
	window_destroy(in->window);
	in->window = NULL;
}

static bool gui_set_scale(const clap_plugin_t *plugin, double scale)
{
	instance_t *in = plugin->plugin_data;
	return in->window && window_set_scale(in->window, scale);
}

static bool gui_get_size(const clap_plugin_t *plugin, uint32_t *width, uint32_t *height)
{
	instance_t *in = plugin->plugin_data;
	if (!in->window)
		return false;
	window_size(in->window, width, height);
	return true;
}

static bool gui_can_resize(const clap_plugin_t *plugin) { return true; }

static bool gui_get_resize_hints(const clap_plugin_t *plugin, clap_gui_resize_hints_t *hints)
{
	instance_t *in = plugin->plugin_data;
	if (!in->window)
		return false;
	uint32_t w, h;
	window_size(in->window, &w, &h);
	hints->can_resize_horizontally = true;
	hints->can_resize_vertically = true;
	hints->preserve_aspect_ratio = true;
	hints->aspect_ratio_width = w;
	hints->aspect_ratio_height = h;
	return true;
}

static bool gui_adjust_size(const clap_plugin_t *plugin, uint32_t *width, uint32_t *height)
{
	instance_t *in = plugin->plugin_data;
	if (!in->window)
		return false;
	window_fit(in->window, width, height);
	return true;
}

static bool gui_set_size(const clap_plugin_t *plugin, uint32_t width, uint32_t height)
{
	instance_t *in = plugin->plugin_data;
	return in->window && window_set_size(in->window, width, height);
}

static bool gui_set_parent(const clap_plugin_t *plugin, const clap_window_t *window)
{
	instance_t *in = plugin->plugin_data;
	return in->window && !strcmp(window->api, window_api()) && window_set_parent(in->window, (uintptr_t)window->ptr);
}

static bool gui_set_transient(const clap_plugin_t *plugin, const clap_window_t *window)
{
	instance_t *in = plugin->plugin_data;
	return in->window && !strcmp(window->api, window_api()) && window_set_transient(in->window, (uintptr_t)window->ptr);
}

static void gui_suggest_title(const clap_plugin_t *plugin, const char *title)
{
	instance_t *in = plugin->plugin_data;
	if (in->window)
		window_set_title(in->window, title);
}

static bool gui_show(const clap_plugin_t *plugin)
{
	instance_t *in = plugin->plugin_data;
	return in->window && window_show(in->window);
}

static bool gui_hide(const clap_plugin_t *plugin)
{
	instance_t *in = plugin->plugin_data;
	if (!in->window)
		return false;
	window_hide(in->window);
	return true;
}

static const clap_plugin_gui_t gui_ext = {
	gui_is_api_supported, gui_get_preferred_api, gui_create, gui_destroy, gui_set_scale, gui_get_size,
	gui_can_resize, gui_get_resize_hints, gui_adjust_size, gui_set_size, gui_set_parent, gui_set_transient,
	gui_suggest_title, gui_show, gui_hide
};

/* ---------------------------------------------------------------- extensions */

static const void *plugin_get_extension(const clap_plugin_t *plugin, const char *id)
{
	(void)plugin;
	if (!strcmp(id, CLAP_EXT_AUDIO_PORTS))
		return &audio_ports_ext;
	if (!strcmp(id, CLAP_EXT_NOTE_PORTS))
		return &note_ports_ext;
	if (!strcmp(id, CLAP_EXT_PARAMS))
		return &params_ext;
	if (!strcmp(id, CLAP_EXT_STATE))
		return &state_ext;
	if (!strcmp(id, CLAP_EXT_GUI))
		return &gui_ext;
	if (!strcmp(id, CLAP_EXT_TIMER_SUPPORT))
		return &timer_ext;
	return NULL;
}

/* ---------------------------------------------------------------- the factory */

static uint32_t factory_count(const clap_plugin_factory_t *f) { (void)f; return MODEL_COUNT; }

static const clap_plugin_descriptor_t *factory_descriptor(const clap_plugin_factory_t *f, uint32_t index)
{
	(void)f;
	return index < (uint32_t)MODEL_COUNT ? &descriptors[index] : NULL;
}

static const clap_plugin_t *factory_create(const clap_plugin_factory_t *f, const clap_host_t *host, const char *id)
{
	(void)f;
	if (!clap_version_is_compatible(host->clap_version))
		return NULL;
	const model_info_t *info = NULL;
	for (int n = 0; n < MODEL_COUNT; n++)
		if (!strcmp(model_infos[n].id, id))
			info = &model_infos[n];
	if (!info)
		return NULL;
	instance_t *in = calloc(1, sizeof *in);
	if (!in)
		return NULL;
	in->host = host;
	in->info = info;
	pthread_mutex_init(&in->lock, NULL);
	pthread_mutex_init(&in->panel_lock, NULL);
	for (int n = 0; n < PARAM_COUNT; n++)
		in->params[n] = param_infos[n].def;
	in->gain = (float)(in->params[PARAM_VOLUME] * in->params[PARAM_VOLUME]);
	in->plugin.desc = &descriptors[info - model_infos];
	in->plugin.plugin_data = in;
	in->plugin.init = plugin_init;
	in->plugin.destroy = plugin_destroy;
	in->plugin.activate = plugin_activate;
	in->plugin.deactivate = plugin_deactivate;
	in->plugin.start_processing = plugin_start_processing;
	in->plugin.stop_processing = plugin_stop_processing;
	in->plugin.reset = plugin_reset;
	in->plugin.process = plugin_process;
	in->plugin.get_extension = plugin_get_extension;
	in->plugin.on_main_thread = plugin_on_main_thread;
	return &in->plugin;
}

static const clap_plugin_factory_t factory = { factory_count, factory_descriptor, factory_create };

/* ---------------------------------------------------------------- the entry */

static bool entry_init(const char *plugin_path)
{
	for (int n = 0; n < MODEL_COUNT; n++)
	{
		clap_plugin_descriptor_t *d = &descriptors[n];
		d->clap_version = (clap_version_t)CLAP_VERSION_INIT;
		d->id = model_infos[n].id;
		d->name = model_infos[n].label;
		d->vendor = VENDOR;
		d->url = URL;
		d->manual_url = URL;
		d->support_url = URL;
		d->version = PLUGIN_VERSION;
		d->description = model_infos[n].description;
		d->features = features;
	}
	plugin_dir[0] = 0;
	if (plugin_path)
	{
		snprintf(plugin_dir, sizeof plugin_dir, "%s", plugin_path);
		char *slash = strrchr(plugin_dir, '/');
		if (slash)
			*slash = 0;
		else
			plugin_dir[0] = 0;
	}
	return true;
}

static void entry_deinit(void) {}

static const void *entry_get_factory(const char *id)
{
	return strcmp(id, CLAP_PLUGIN_FACTORY_ID) == 0 ? &factory : NULL;
}

CLAP_EXPORT const clap_plugin_entry_t clap_entry = { CLAP_VERSION_INIT, entry_init, entry_deinit, entry_get_factory };
