/* scplay: a terminal player for the scemu Sound Canvas emulator.
 *
 * Copyright (c) 2026 ian karlsson
 * SPDX-License-Identifier: BSD-3-Clause
 */
#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "scemu.h"
#include "audio.h"
#include "roms.h"
#include "smf.h"
#include "tui.h"
#include "session.h"

#define BLOCK 256
#define BUTTON_HOLD_FRAMES 1600

static volatile sig_atomic_t g_quit;

static void on_signal(int sig)
{
	(void)sig;
	g_quit = 1;
}

/* ---------------------------------------------------------------- wav */

typedef struct wav
{
	FILE *fp;
	uint64_t frames;
	uint32_t rate;
} wav_t;

static void wav_u32(FILE *fp, uint32_t v) { uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) }; fwrite(b, 1, 4, fp); }
static void wav_u16(FILE *fp, uint16_t v) { uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) }; fwrite(b, 1, 2, fp); }

static int wav_open(wav_t *w, const char *path, uint32_t rate)
{
	memset(w, 0, sizeof(*w));
	w->fp = fopen(path, "wb");
	if (!w->fp)
		return 0;
	w->rate = rate;
	fwrite("RIFF", 1, 4, w->fp); wav_u32(w->fp, 0); fwrite("WAVE", 1, 4, w->fp);
	fwrite("fmt ", 1, 4, w->fp); wav_u32(w->fp, 16); wav_u16(w->fp, 1); wav_u16(w->fp, 2);
	wav_u32(w->fp, rate); wav_u32(w->fp, rate * 4); wav_u16(w->fp, 4); wav_u16(w->fp, 16);
	fwrite("data", 1, 4, w->fp); wav_u32(w->fp, 0);
	return 1;
}

static void wav_write(wav_t *w, const int16_t *stereo, size_t frames)
{
	if (!w->fp)
		return;
	fwrite(stereo, sizeof(int16_t) * 2, frames, w->fp);
	w->frames += frames;
}

static void wav_close(wav_t *w)
{
	if (!w->fp)
		return;
	uint32_t data_size = (uint32_t)(w->frames * 4);
	fseek(w->fp, 4, SEEK_SET);
	wav_u32(w->fp, 36 + data_size);
	fseek(w->fp, 40, SEEK_SET);
	wav_u32(w->fp, data_size);
	fclose(w->fp);
	w->fp = NULL;
}

/* ---------------------------------------------------------------- helpers */

static void to_s16(const int32_t *in, int16_t *out, size_t samples)
{
	for (size_t n = 0; n < samples; n++)
	{
		int32_t v = (in[n] + 128) >> 8;
		if (v > 32767) v = 32767;
		if (v < -32768) v = -32768;
		out[n] = (int16_t)v;
	}
}

static double now_seconds(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

/* ---------------------------------------------------------------- panel */

typedef struct panel
{
	scemu_t *m;
	int hold[SCEMU_BUTTON_COUNT];
} panel_t;

static void panel_press(panel_t *p, scemu_button_t button)
{
	if (!p->hold[button])
		scemu_button(p->m, button, true);
	p->hold[button] = BUTTON_HOLD_FRAMES;
}

static void panel_advance(panel_t *p, int frames)
{
	for (int n = 0; n < SCEMU_BUTTON_COUNT; n++)
		if (p->hold[n] > 0)
		{
			p->hold[n] -= frames;
			if (p->hold[n] <= 0)
			{
				p->hold[n] = 0;
				scemu_button(p->m, (scemu_button_t)n, false);
			}
		}
}

static int button_for_key(int key)
{
	switch (key)
	{
	case TUI_KEY_LEFT:  return SCEMU_BUTTON_PART_LEFT;
	case TUI_KEY_RIGHT: return SCEMU_BUTTON_PART_RIGHT;
	case TUI_KEY_DOWN:  return SCEMU_BUTTON_INSTRUMENT_LEFT;
	case TUI_KEY_UP:    return SCEMU_BUTTON_INSTRUMENT_RIGHT;
	case '-':           return SCEMU_BUTTON_LEVEL_LEFT;
	case '=':           return SCEMU_BUTTON_LEVEL_RIGHT;
	case '[':           return SCEMU_BUTTON_PAN_LEFT;
	case ']':           return SCEMU_BUTTON_PAN_RIGHT;
	case ';':           return SCEMU_BUTTON_REVERB_LEFT;
	case '\'':          return SCEMU_BUTTON_REVERB_RIGHT;
	case ',':           return SCEMU_BUTTON_CHORUS_LEFT;
	case '.':           return SCEMU_BUTTON_CHORUS_RIGHT;
	case 'k':           return SCEMU_BUTTON_KEY_SHIFT_LEFT;
	case 'l':           return SCEMU_BUTTON_KEY_SHIFT_RIGHT;
	case 'n':           return SCEMU_BUTTON_MIDI_CH_LEFT;
	case 'm':           return SCEMU_BUTTON_MIDI_CH_RIGHT;
	case 'a':           return SCEMU_BUTTON_ALL;
	case 'x':           return SCEMU_BUTTON_MUTE;
	case '5':           return SCEMU_BUTTON_SC55_MAP;
	case '8':           return SCEMU_BUTTON_SC88_MAP;
	case 'u':           return SCEMU_BUTTON_USER_INST;
	case 's':           return SCEMU_BUTTON_SELECT;
	case 'v':           return SCEMU_BUTTON_PREVIEW;
	default:            return -1;
	}
}

/* ---------------------------------------------------------------- options */

typedef struct options
{
	const char *midi;
	const char *model;
	const char *wav;
	const char *rom;
	const char *audio_device;
	unsigned audio_block;
	double tail;
	int port;
	bool no_audio;
	bool no_cache;
	bool keep_settings;
	bool hold;
	scemu_map_t map;
	uint32_t midi_rate;
} options_t;

static void usage(FILE *fp)
{
	fprintf(fp,
	        "usage: scplay [options] song.mid\n"
	        "\n"
	        "  --model sc88|sc88pro|sc88vl   machine to emulate (default sc88pro)\n"
	        "  --rom PATH                    a zip or a directory holding the ROM images (any names)\n"
	        "  --wav FILE                    also write what is played, 16-bit stereo 32 kHz\n"
	        "  --no-audio                    render as fast as the host allows, no sound card\n"
	        "  --audio-device NAME           play on the output device whose name holds NAME\n"
	        "                                (--audio-device list prints them) instead of the default\n"
	        "  --audio-block N               the device's buffer in frames (default 256, 8 ms)\n"
	        "  --no-cache                    boot the firmware instead of loading a cached state\n"
	        "  --keep-settings               start from, and save, the settings memory of the\n"
	        "                                last --keep-settings run instead of factory settings\n"
	        "  --tail N                      seconds to keep running after the last event (default 4)\n"
	        "  --port 0|1                    MIDI IN for tracks that name no port (default A);\n"
	        "                                a track's port event routes it to A (0) or B (1);\n"
	        "                                ports 2 and 3 (an SC-8850's C and D) are not played\n"
	        "  --map sc55|sc88|sc88pro       play every part from that instrument map, whatever\n"
	        "                                the song selects (the SC-88 has no SC-88Pro map)\n"
	        "  --midi-rate BAUD              speed of the MIDI input: 31250 (the cable, default),\n"
	        "                                38400 (the computer port), 0 for no limit\n"
	        "  --hold                        wait for a key when the song ends\n"
	        "  --help\n");
}

static int parse_options(int argc, char **argv, options_t *o)
{
	memset(o, 0, sizeof(*o));
	o->tail = 4.0;
	o->midi_rate = 31250;
	o->audio_block = 256;
	for (int n = 1; n < argc; n++)
	{
		const char *a = argv[n];
		if (!strcmp(a, "--help") || !strcmp(a, "-h"))
		{
			usage(stdout);
			return 0;
		}
		else if (!strcmp(a, "--model") && n + 1 < argc)
			o->model = argv[++n];
		else if (!strcmp(a, "--rom") && n + 1 < argc)
			o->rom = argv[++n];
		else if (!strcmp(a, "--wav") && n + 1 < argc)
			o->wav = argv[++n];
		else if (!strcmp(a, "--tail") && n + 1 < argc)
			o->tail = atof(argv[++n]);
		else if (!strcmp(a, "--port") && n + 1 < argc)
			o->port = atoi(argv[++n]) ? SCEMU_MIDI_IN_B : SCEMU_MIDI_IN_A;
		else if (!strcmp(a, "--map") && n + 1 < argc)
		{
			const char *name = argv[++n];
			o->map = !strcmp(name, "sc55") ? SCEMU_MAP_SC55 : !strcmp(name, "sc88") ? SCEMU_MAP_SC88
					: !strcmp(name, "sc88pro") ? SCEMU_MAP_SC88PRO : SCEMU_MAP_NATIVE;
			if (o->map == SCEMU_MAP_NATIVE)
			{
				fprintf(stderr, "scplay: --map takes sc55, sc88 or sc88pro\n");
				return -1;
			}
		}
		else if (!strcmp(a, "--midi-rate") && n + 1 < argc)
			o->midi_rate = (uint32_t)atoi(argv[++n]);
		else if (!strcmp(a, "--audio-device") && n + 1 < argc)
			o->audio_device = argv[++n];
		else if (!strcmp(a, "--audio-block") && n + 1 < argc)
			o->audio_block = (unsigned)atoi(argv[++n]);
		else if (!strcmp(a, "--no-audio"))
			o->no_audio = true;
		else if (!strcmp(a, "--no-cache"))
			o->no_cache = true;
		else if (!strcmp(a, "--keep-settings"))
			o->keep_settings = true;
		else if (!strcmp(a, "--hold"))
			o->hold = true;
		else if (a[0] == '-' && a[1])
		{
			fprintf(stderr, "scplay: unknown option %s\n", a);
			return -1;
		}
		else if (!o->midi)
			o->midi = a;
		else
		{
			fprintf(stderr, "scplay: more than one song given\n");
			return -1;
		}
	}
	if (o->audio_device && !strcmp(o->audio_device, "list"))
	{
		audio_device_info_t devices[64];
		int count = audio_list(devices, 64);
		for (int n = 0; n < count; n++)
			printf("%s%s\n", devices[n].name, devices[n].is_default ? " (default)" : "");
		return 0;
	}
	if (!o->midi)
	{
		usage(stderr);
		return -1;
	}
	return 1;
}

/* the first output device whose name holds `name`, any case; -1 for none */
static int find_audio_device(const char *name)
{
	audio_device_info_t devices[64];
	int count = audio_list(devices, 64);
	for (int n = 0; n < count; n++)
	{
		const char *hay = devices[n].name, *needle = name;
		for (const char *h = hay; *h; h++)
		{
			size_t k = 0;
			while (needle[k] && h[k] && tolower((unsigned char)h[k]) == tolower((unsigned char)needle[k]))
				k++;
			if (!needle[k])
				return devices[n].index;
		}
	}
	return -1;
}

/* ---------------------------------------------------------------- main */

typedef struct boot_progress
{
	tui_t *tui;
	tui_state_t *st;
	scemu_t *m;
	uint32_t rate;
	double next_draw;
} boot_progress_t;

static bool boot_progress(void *user, uint64_t frames)
{
	boot_progress_t *b = user;
	int key;
	while ((key = tui_key(b->tui)) != TUI_KEY_NONE)
		if (key == 'q' || key == TUI_KEY_ESC)
			g_quit = 1;
	double t = now_seconds();
	if (b->tui && t >= b->next_draw)
	{
		b->next_draw = t + 1.0 / 30;
		b->st->status = "▪ booting the firmware";
		b->st->elapsed = (double)frames / b->rate;
		b->st->total = 8.0;
		b->st->leds = scemu_leds(b->m);
		tui_draw(b->tui, b->st);
	}
	return !g_quit;
}

int main(int argc, char **argv)
{
	options_t opt;
	int rc = parse_options(argc, argv, &opt);
	if (rc <= 0)
		return rc == 0 ? 0 : 2;

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	char exe_dir[PATH_MAX];
	session_exe_directory(argv[0], exe_dir, sizeof(exe_dir));

	char err[512];
	scplay_roms_t roms;
	if (!scplay_roms_load(&roms, opt.model, opt.rom, exe_dir, err, sizeof(err)))
	{
		fprintf(stderr, "scplay: %s\n", err);
		if (strncmp(err, "unknown model", 13) != 0)
			fprintf(stderr, "scplay: put the dumps, zipped or loose and under any names, beside"
			                " the program or in ~/.mame/roms, or give --rom\n");
		return 1;
	}
	if (!opt.model && roms.model != SCEMU_MODEL_SC88PRO)
		fprintf(stderr, "scplay: no SC-88Pro ROM set found, using the %s\n",
		        scplay_model_label(roms.model));

	scemu_t *m = scemu_create(roms.model, &roms.roms, NULL);
	if (!m)
	{
		fprintf(stderr, "scplay: %s\n", scemu_error(NULL));
		scplay_roms_free(&roms);
		return 1;
	}
	uint32_t rate = scemu_sample_rate(m);

	smf_t smf;
	if (!smf_load(&smf, opt.midi, rate))
	{
		fprintf(stderr, "scplay: %s: not a Standard MIDI File\n", opt.midi);
		scemu_destroy(m);
		scplay_roms_free(&roms);
		return 1;
	}

	tui_t *tui = tui_open();
	panel_t panel = { m, { 0 } };

	tui_state_t st;
	memset(&st, 0, sizeof(st));
	st.model = scplay_model_label(roms.model);
	st.song = session_base_name(opt.midi);
	st.title = smf.name;
	st.lcd = scemu_lcd(m);
	st.has_efx_led = roms.model == SCEMU_MODEL_SC88PRO;
	st.eq_label = roms.model != SCEMU_MODEL_SC88PRO;

	int32_t raw[BLOCK * 2];
	int16_t pcm[BLOCK * 2];
	int32_t *const out[2] = { raw, NULL };

	/* ------------------------------------------------------------ boot */

	session_t session;
	session_init(&session, m, roms.model_name, roms.hash, opt.no_cache, opt.keep_settings);

	boot_progress_t progress = { tui, &st, m, rate, 0 };
	double boot_start = now_seconds();
	session_boot(&session, true, boot_progress, &progress);
	bool from_cache = session.from_cache;
	uint64_t boot_frames = session.boot_frames;
	double boot_seconds = now_seconds() - boot_start;

	/* ------------------------------------------------------------ play */

	wav_t wav;
	memset(&wav, 0, sizeof(wav));
	if (opt.wav && !wav_open(&wav, opt.wav, rate))
	{
		fprintf(stderr, "scplay: cannot write %s\n", opt.wav);
		g_quit = 1;
	}

	scemu_set_map(m, opt.map);
	scemu_set_midi_rate(m, opt.midi_rate);

	scplay_audio_t *audio = NULL;
	if (!opt.no_audio && !g_quit)
	{
		int device = -1;
		if (opt.audio_device)
		{
			device = find_audio_device(opt.audio_device);
			if (device < 0)
				fprintf(stderr, "scplay: no output device named like %s, using the default\n", opt.audio_device);
		}
		audio = audio_open(rate, device, opt.audio_block, err, sizeof(err));
		if (!audio)
			fprintf(stderr, "scplay: no audio (%s), rendering silently\n", err);
	}

	uint64_t end_frame = (uint64_t)smf.last_frame + (uint64_t)(opt.tail * rate);
	uint64_t pos = 0;
	size_t next_event = 0;
	bool paused = false, started = false;
	double next_draw = 0, play_start = now_seconds();
	uint64_t rendered = 0;

	while (!g_quit && pos < end_frame)
	{
		int key;
		bool toggled_pause = false;
		while ((key = tui_key(tui)) != TUI_KEY_NONE)
		{
			if (key == 'q' || key == TUI_KEY_ESC)
				g_quit = 1;
			else if (key == ' ')
			{
				paused = !paused;
				toggled_pause = true;
			}
			else if (key == '?')
				st.show_keys = !st.show_keys;
			else
			{
				int b = button_for_key(key);
				if (b >= 0)
					panel_press(&panel, (scemu_button_t)b);
			}
		}
		if (toggled_pause && audio)
			audio_pause(audio, paused);

		size_t n = BLOCK;
		if (paused)
			n = 0;
		else if (audio)
		{
			size_t space = audio_space(audio);
			if (space < n)
				n = space;
		}
		if (n > end_frame - pos)
			n = (size_t)(end_frame - pos);

		if (n)
		{
			while (next_event < smf.count && smf.events[next_event].frame < pos + n)
			{
				const smf_event_t *e = &smf.events[next_event++];
				uint32_t offset = e->frame > pos ? (uint32_t)(e->frame - pos) : 0;
				if (e->tempo_change)
					continue;
				if (e->port != SMF_PORT_UNSET && e->port > 1)
					continue;
				int port = e->port == SMF_PORT_UNSET ? opt.port : e->port ? SCEMU_MIDI_IN_B : SCEMU_MIDI_IN_A;
				if (e->status[0] == 0xf0 && e->bytes)
				{
					scemu_midi_write(m, port, e->status, 1, offset);
					scemu_midi_write(m, port, e->bytes, e->length - 1u, offset);
				}
				else if (e->bytes)
					scemu_midi_write(m, port, e->bytes, e->length, offset);
				else
					scemu_midi_write(m, port, e->status, e->length, offset);
			}

			scemu_render(m, out, n);
			panel_advance(&panel, (int)n);
			to_s16(raw, pcm, n * 2);
			if (audio)
				audio_push(audio, pcm, n);
			wav_write(&wav, pcm, n);
			pos += n;
			rendered += n;
			if (audio && !started && audio_space(audio) == 0)
			{
				audio_pause(audio, false);
				started = true;
			}
		}

		double t = now_seconds();
		if (tui && t >= next_draw)
		{
			next_draw = t + 1.0 / 30;
			st.status = NULL;
			st.paused = paused;
			st.leds = scemu_leds(m);
			st.elapsed = (audio && started ? (double)audio_played(audio) : (double)pos) / rate;
			st.total = (double)end_frame / rate;
			st.underruns = audio ? audio_underruns(audio) : 0;
			st.speed = t > play_start ? (double)rendered / rate / (t - play_start) : 0;
			tui_draw(tui, &st);
		}

		if (!n && !g_quit)
		{
			struct timespec ts = { 0, paused ? 20000000 : 2000000 };
			nanosleep(&ts, NULL);
		}
	}

	if (audio && !g_quit)
	{
		if (!started)
			audio_pause(audio, false);
		audio_drain(audio);
	}

	if (tui && opt.hold && !g_quit)
	{
		st.status = "■ done — press a key";
		tui_draw(tui, &st);
		while (tui_key(tui) == TUI_KEY_NONE && !g_quit)
		{
			struct timespec ts = { 0, 30000000 };
			nanosleep(&ts, NULL);
		}
	}

	tui_close(tui);
	audio_close(audio);
	wav_close(&wav);

	bool nvram_written = session_save_settings(&session);
	session_free(&session);

	double play_seconds = now_seconds() - play_start;
	if (from_cache)
		fprintf(stderr, "scplay: %s, boot restored from the cache in %.2f s\n",
		        scplay_model_label(roms.model), boot_seconds);
	else
		fprintf(stderr, "scplay: %s, booted in %.2f s (%.2f s of machine time)\n",
		        scplay_model_label(roms.model), boot_seconds, (double)boot_frames / rate);
	fprintf(stderr, "scplay: played %.2f s of music in %.2f s (%.1f× real time)\n",
	        (double)rendered / rate, play_seconds,
	        play_seconds > 0 ? (double)rendered / rate / play_seconds : 0);
	if (opt.wav)
		fprintf(stderr, "scplay: wrote %s\n", opt.wav);
	if (nvram_written)
		fprintf(stderr, "scplay: saved the settings memory\n");

	smf_free(&smf);
	scemu_destroy(m);
	scplay_roms_free(&roms);
	return 0;
}
