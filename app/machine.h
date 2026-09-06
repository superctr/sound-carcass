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

/* The systems the players keep a rear COMPUTER switch for, oldest first, which
 * is also the order they are listed in.  It is the order of machine_options_t's
 * computer[] and of the settings file's rows (play/config.h's CONFIG_ROW_*), and
 * the three have to agree. */
#define MACHINE_SYSTEMS 5
extern const scemu_model_t machine_systems[MACHINE_SYSTEMS];
/* the row a model's switch lives in, or -1 for a model without one */
int machine_system_index(scemu_model_t model);

typedef struct machine_options
{
	const char *model;        /* NULL: the best ROM set found */
	const char *rom;          /* --rom, or NULL */
	const char *exe_dir;
	scemu_map_t map;
	uint32_t midi_rate;
	scemu_computer_switch_t computer[MACHINE_SYSTEMS];  /* the rear switch of each system */
	double tail;              /* seconds after a song's last event */
	bool keep_settings, no_cache, no_audio;
	int audio_device;         /* an index from audio_list (audio.h), or -1 for the default */
	unsigned audio_block;     /* the device's buffer in frames; 0 for the default */
	unsigned audio_rate;      /* the rate the device is asked for; 0 the machine's own */
} machine_options_t;

typedef struct machine_state
{
	scemu_lcd_t lcd;
	scemu_glcd_t glcd;
	bool has_glcd;            /* the machine's glass is the bitmap, not the character cells */
	uint32_t leds;
	bool power, booting, playing, paused, finished;
	double position, length;  /* seconds into the song, and its length with the tail */
	char song[256];           /* the file's name, empty when nothing is loaded */
	char title[128];          /* the file's title, UTF-8 */
	uint32_t underruns;
	char audio[192];          /* the output device, its host API and rate; "none" without one */
	double latency;           /* seconds from the machine to the jack */
	char error[256];          /* what went wrong last, empty when nothing did */
	uint64_t generation;      /* counts up on every change of the above */
} machine_state_t;

/* The machine that is running and where its ROMs came from. */
typedef struct machine_rom_info
{
	scemu_model_t model;
	const char *label;        /* "SC-88Pro" */
	char version[16];         /* the control ROM's version */
	char origin[512];
	uint32_t rate;            /* what the library renders at */
} machine_rom_info_t;

/* Loads the ROMs, creates the machine and starts its thread, which boots it
 * (from the cache when it can).  NULL with a message in err on failure. */
machine_t *machine_start(const machine_options_t *o, char *err, size_t err_size);
void machine_stop(machine_t *mc);

scemu_model_t machine_model(machine_t *mc);
const char *machine_model_label(machine_t *mc);
void machine_rom_info(machine_t *mc, machine_rom_info_t *out);
/* a bitmask by scemu_model_t of the models whose ROM set was there when the
 * machine last loaded one */
unsigned machine_models_available(machine_t *mc);

void machine_snapshot(machine_t *mc, machine_state_t *out);

/* Commands; all return at once. */
void machine_play(machine_t *mc, const char *path);    /* a fresh machine, then the song */
void machine_pause(machine_t *mc, bool paused);
void machine_stop_song(machine_t *mc);
void machine_button(machine_t *mc, scemu_button_t b, bool down);
void machine_dial(machine_t *mc, int steps);           /* the value dial, positive clockwise */
/* the same, ms of the machine's own time later (it stands still while the
 * machine boots), in the order posted; for the panel's key combinations */
void machine_button_after(machine_t *mc, scemu_button_t b, bool down, unsigned ms);
void machine_power(machine_t *mc, bool on);            /* off: silence; on: reset and boot, keys held */
void machine_set_gain(machine_t *mc, float gain);       /* 0..1, applied to the output */
void machine_set_dac_rail(machine_t *mc, int bits);     /* 24..29, kept across boots */
/* Another machine, in place: the song is unloaded, the old instance goes and
 * the new one boots from its cache.  On a failure the old one keeps running
 * and the snapshot's error says why. */
void machine_set_model(machine_t *mc, scemu_model_t model);
/* The rear COMPUTER switch of the running system.  The firmware reads the
 * ladder once at boot, so this replaces the machine the way a model change
 * does; the position is remembered for that system. */
void machine_set_computer_switch(machine_t *mc, scemu_computer_switch_t sw);
/* Reopen the output on another device (audio.h's index, -1 the default) with
 * another buffer -- 0 keeps the block -- at another rate, 0 the machine's own. */
void machine_set_audio(machine_t *mc, int device, unsigned block, unsigned rate);

/* What goes to the machine before every song, on both ports. */
typedef enum machine_reset
{
	MACHINE_RESET_NONE,
	MACHINE_RESET_GM,          /* GM System On */
	MACHINE_RESET_GS,          /* GS Reset (the default) */
	MACHINE_RESET_GM2,         /* GM2 System On */
	MACHINE_RESET_SC88_SINGLE, /* SC-88 System Mode Set, single module */
	MACHINE_RESET_SC88_DOUBLE, /* SC-88 System Mode Set, double module */
	MACHINE_RESET_COUNT
} machine_reset_t;
extern const char *const machine_reset_names[MACHINE_RESET_COUNT];

void machine_set_reset(machine_t *mc, machine_reset_t reset);
/* the message a reset kind stands for; NULL and 0 for MACHINE_RESET_NONE */
const uint8_t *machine_reset_message(machine_reset_t reset, size_t *size);
/* MIDI to both of the machine's inputs now, and to the song outputs with it */
void machine_send(machine_t *mc, const uint8_t *bytes, size_t count);
void machine_set_map(machine_t *mc, scemu_map_t map);   /* takes effect at once */

/* Host MIDI devices (midi_io.h): tie one of the machine's inputs (MIDI IN
 * A-D) to a device that feeds it, or one of its outputs (MIDI OUT, the
 * song's ports A-D for a real unit playing along) to a device it feeds;
 * id -1 unties.  The list and the rescan are synchronous. */
void machine_midi_input(machine_t *mc, int which, int id);
void machine_midi_output(machine_t *mc, int which, int id);
struct midi_port_info;
int machine_midi_list(machine_t *mc, struct midi_port_info *out, int max);
void machine_midi_rescan(machine_t *mc);
/* the port groups the running machine takes: 4 on an SC-8850 whose switch
 * is on USB, 2 on everything else; the ports above follow it */
int machine_midi_ports(machine_t *mc);

#endif
