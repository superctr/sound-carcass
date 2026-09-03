/* scgui: the machine on its own thread.
 *
 * The emulator, its audio output and the song being played live on one
 * thread; the window talks to it through commands and reads a snapshot of
 * what the panel should show.  Nothing here knows about the toolkit.
 *
 * Copyright (c) 2026 ian karlsson
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef SCGUI_MACHINE_H
#define SCGUI_MACHINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "scemu.h"

typedef struct machine machine_t;

typedef struct machine_options
{
	const char *model;        /* NULL: the best ROM set found */
	const char *rom;          /* --rom, or NULL */
	const char *exe_dir;
	scemu_map_t map;
	uint32_t midi_rate;
	double tail;              /* seconds after a song's last event */
	bool keep_settings, no_cache, no_audio;
} machine_options_t;

typedef struct machine_state
{
	scemu_lcd_t lcd;
	uint32_t leds;
	bool power, booting, playing, paused, finished;
	double position, length;  /* seconds into the song, and its length with the tail */
	char song[256];           /* the file's name, empty when nothing is loaded */
	char title[128];          /* the file's title, UTF-8 */
	uint32_t underruns;
	uint64_t generation;      /* counts up on every change of the above */
} machine_state_t;

/* Loads the ROMs, creates the machine and starts its thread, which boots it
 * (from the cache when it can).  NULL with a message in err on failure. */
machine_t *machine_start(const machine_options_t *o, char *err, size_t err_size);
void machine_stop(machine_t *mc);

scemu_model_t machine_model(const machine_t *mc);
const char *machine_model_label(const machine_t *mc);
const char *machine_audio_driver(const machine_t *mc);

void machine_snapshot(machine_t *mc, machine_state_t *out);

/* Commands; all return at once. */
void machine_play(machine_t *mc, const char *path);    /* a fresh machine, then the song */
void machine_pause(machine_t *mc, bool paused);
void machine_stop_song(machine_t *mc);
void machine_button(machine_t *mc, scemu_button_t b, bool down);
void machine_power(machine_t *mc, bool on);            /* off: silence; on: reset and boot, keys held */
void machine_set_gain(machine_t *mc, float gain);       /* 0..1, applied to the output */

/* Host MIDI ports (midi_io.h): connect one of the machine's inputs (MIDI IN
 * A, B) to a source, or one of its outputs (MIDI OUT, the song's ports A
 * and B for a real unit playing along) to a destination; client -1
 * disconnects. */
void machine_midi_input(machine_t *mc, int which, int client, int port);
void machine_midi_output(machine_t *mc, int which, int client, int port);

#endif
