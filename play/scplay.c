/* scplay: a terminal player for the scemu Sound Canvas emulator.
 *
 * Copyright (c) 2026 ian karlsson
 * SPDX-License-Identifier: BSD-3-Clause
 */
#define _POSIX_C_SOURCE 200809L

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

#define BLOCK 256
#define BOOT_LIMIT_SECONDS 20
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

static void exe_directory(const char *argv0, char *out, size_t size)
{
	char path[PATH_MAX];
	ssize_t got = readlink("/proc/self/exe", path, sizeof(path) - 1);
	if (got > 0)
		path[got] = 0;
	else
		snprintf(path, sizeof(path), "%s", argv0 ? argv0 : "");
	char *slash = strrchr(path, '/');
	if (slash)
		*slash = 0;
	else
		snprintf(path, sizeof(path), ".");
	snprintf(out, size, "%s", path);
}

static const char *base_name(const char *path)
{
	const char *slash = strrchr(path, '/');
	return slash ? slash + 1 : path;
}

static int make_dir(const char *path)
{
	if (mkdir(path, 0755) == 0 || errno == EEXIST)
		return 1;
	return 0;
}

static int cache_dir(char *out, size_t size)
{
	char dir[900];
	const char *xdg = getenv("XDG_CACHE_HOME");
	const char *home = getenv("HOME");
	if (xdg && *xdg)
		snprintf(dir, sizeof(dir), "%s", xdg);
	else if (home && *home)
	{
		snprintf(dir, sizeof(dir), "%s/.cache", home);
		if (!make_dir(dir))
			return 0;
	}
	else
		return 0;
	if (!make_dir(dir))
		return 0;
	snprintf(out, size, "%s/scemu", dir);
	return make_dir(out);
}

static int read_file_exact(const char *path, void *buffer, size_t size)
{
	FILE *fp = fopen(path, "rb");
	if (!fp)
		return 0;
	int ok = fread(buffer, 1, size, fp) == size && fgetc(fp) == EOF;
	fclose(fp);
	return ok;
}

static void write_file_atomic(const char *path, const void *data, size_t size)
{
	char tmp[PATH_MAX + 8];
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	FILE *fp = fopen(tmp, "wb");
	if (!fp)
		return;
	int ok = fwrite(data, 1, size, fp) == size;
	fclose(fp);
	if (ok)
		rename(tmp, path);
	else
		remove(tmp);
}

static bool load_cached_state(scemu_t *m, const char *path)
{
	FILE *fp = fopen(path, "rb");
	if (!fp)
		return false;
	fseek(fp, 0, SEEK_END);
	long size = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	if (size <= 0)
	{
		fclose(fp);
		return false;
	}
	void *buf = malloc((size_t)size);
	bool ok = buf && fread(buf, 1, (size_t)size, fp) == (size_t)size &&
	          scemu_state_load(m, buf, (size_t)size);
	free(buf);
	fclose(fp);
	return ok;
}

static void save_cached_state(const scemu_t *m, const char *path)
{
	size_t size = scemu_state_size(m);
	void *buf = malloc(size);
	if (!buf)
		return;
	if (scemu_state_save(m, buf, size) == size)
		write_file_atomic(path, buf, size);
	free(buf);
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
	        "  --rom PATH                    a zip or a directory holding the ROM images\n"
	        "  --wav FILE                    also write what is played, 16-bit stereo 32 kHz\n"
	        "  --no-audio                    render as fast as the host allows, no sound card\n"
	        "  --no-cache                    boot the firmware instead of loading a cached state\n"
	        "  --keep-settings               start from, and save, the settings memory of the\n"
	        "                                last --keep-settings run instead of factory settings\n"
	        "  --tail N                      seconds to keep running after the last event (default 4)\n"
	        "  --port 0|1                    MIDI IN for tracks that name no port (default A);\n"
	        "                                a track's port event routes it to A (even) or B (odd)\n"
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
	if (!o->midi)
	{
		usage(stderr);
		return -1;
	}
	return 1;
}

/* ---------------------------------------------------------------- main */

int main(int argc, char **argv)
{
	options_t opt;
	int rc = parse_options(argc, argv, &opt);
	if (rc <= 0)
		return rc == 0 ? 0 : 2;

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	char exe_dir[PATH_MAX];
	exe_directory(argv[0], exe_dir, sizeof(exe_dir));

	char err[512];
	scplay_roms_t roms;
	if (!scplay_roms_load(&roms, opt.model, opt.rom, exe_dir, err, sizeof(err)))
	{
		fprintf(stderr, "scplay: %s\n", err);
		if (strncmp(err, "unknown model", 13) != 0)
			fprintf(stderr, "scplay: put sc88pro.zip (or sc88.zip, sc88vl.zip) beside the program"
			                " or in ~/.mame/roms, or give --rom\n");
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
	st.song = base_name(opt.midi);
	st.title = smf.name;
	st.lcd = scemu_lcd(m);
	st.has_efx_led = roms.model == SCEMU_MODEL_SC88PRO;
	st.eq_label = roms.model != SCEMU_MODEL_SC88PRO;

	int32_t raw[BLOCK * 2];
	int16_t pcm[BLOCK * 2];
	int32_t *const out[2] = { raw, NULL };

	/* ------------------------------------------------------------ boot */

	char cache_root[1024], state_file[PATH_MAX];
	char factory_file[PATH_MAX], settings_file[PATH_MAX];
	bool have_cache = !opt.no_cache && cache_dir(cache_root, sizeof(cache_root));
	size_t nvram_size = scemu_nvram_size(m);
	uint8_t *nvram = malloc(nvram_size);
	bool have_seed = false, seed_is_user = false;

	if (have_cache && nvram)
	{
		snprintf(factory_file, sizeof(factory_file), "%s/%s-factory.nvram", cache_root, roms.model_name);
		snprintf(settings_file, sizeof(settings_file), "%s/%s.nvram", cache_root, roms.model_name);
		if (opt.keep_settings && read_file_exact(settings_file, nvram, nvram_size) &&
		    scemu_nvram_set(m, nvram, nvram_size))
			have_seed = seed_is_user = true;
		else if (read_file_exact(factory_file, nvram, nvram_size) &&
		         scemu_nvram_set(m, nvram, nvram_size))
			have_seed = true;
		snprintf(state_file, sizeof(state_file), "%s/boot-%s-%016llx.state",
		         cache_root, roms.model_name, (unsigned long long)roms.hash);
	}

	/* The cached boot state is a machine that has just come up from the
	 * firmware's own factory initialisation, so it carries no settings from an
	 * earlier session; a machine seeded with the user's own settings memory is
	 * booted every time instead. */
	bool use_state = have_cache && !seed_is_user;

	double boot_start = now_seconds();
	bool from_cache = use_state && load_cached_state(m, state_file);
	uint64_t boot_frames = 0;

	if (!from_cache)
	{
		double next_draw = 0;
		bool released = false;
		while (!released && !g_quit && boot_frames < (uint64_t)BOOT_LIMIT_SECONDS * rate)
		{
			scemu_render(m, out, BLOCK);
			boot_frames += BLOCK;
			released = !scemu_muted(m);
			int key;
			while ((key = tui_key(tui)) != TUI_KEY_NONE)
				if (key == 'q' || key == TUI_KEY_ESC)
					g_quit = 1;
			double t = now_seconds();
			if (tui && t >= next_draw)
			{
				next_draw = t + 1.0 / 30;
				st.status = "▪ booting the firmware";
				st.elapsed = (double)boot_frames / rate;
				st.total = 8.0;
				st.leds = scemu_leds(m);
				tui_draw(tui, &st);
			}
		}
		/* A blank settings memory sends the firmware through a first power-on
		 * initialisation that takes a longer path than any later start.  Keep
		 * what that boot wrote — it is the firmware's own factory image, not
		 * anyone's settings — and let the next run, which starts from a machine
		 * that has been switched on before, be the one that is cached. */
		if (have_cache && released)
		{
			if (!have_seed && nvram && scemu_nvram_get(m, nvram, nvram_size) == nvram_size)
			{
				write_file_atomic(factory_file, nvram, nvram_size);
				have_seed = true;
			}
			else if (use_state)
				save_cached_state(m, state_file);
		}
	}
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
		audio = audio_open(rate, err, sizeof(err));
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
				int port = e->port == SMF_PORT_UNSET ? opt.port : (e->port & 1) ? SCEMU_MIDI_IN_B : SCEMU_MIDI_IN_A;
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

	/* Only on request: the machine keeps what this session left in it. */
	bool nvram_written = false;
	if (opt.keep_settings && have_cache && nvram &&
	    scemu_nvram_get(m, nvram, nvram_size) == nvram_size)
	{
		write_file_atomic(settings_file, nvram, nvram_size);
		nvram_written = true;
	}
	free(nvram);

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
