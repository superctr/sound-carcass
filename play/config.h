/* The players' settings file: one `key = value` per line, read by a re2c
 * lexer and written back so that it round-trips.
 *
 * The file is UTF-8 text.  A `#` starts a comment that runs to the end of the
 * line, blank lines are allowed, and a line is a lowercase key, an `=`, and a
 * value:
 *
 *     # the machine: which module, where its ROMs are, and what it remembers
 *     model = "sc88pro"
 *     rom = "/home/me/roms/sc88pro.zip"
 *     map = "native"
 *     keep_settings = no
 *     boot_animation = no
 *
 *     # the rear COMPUTER switch, one position per system
 *     computer_sc88pro = midi
 *
 *     # audio: the output device, its rate and buffer, and the knob
 *     audio_device = "HDA Intel PCH: ALC295 Analog"
 *     audio_rate = 0
 *     audio_block = 256
 *     volume = 0.75
 *
 * A value is an integer, a decimal, a boolean (`yes`/`no`, `true`/`false`,
 * `on`/`off`, in any case), or a string.  A string is double-quoted and takes
 * the escapes `\"`, `\\`, `\n`, `\t`, `\r`; any other escaped character stands
 * for itself.  A bare word -- anything with no space, `#`, `=` or `"` in it --
 * is taken as a string too, for files written by hand.  The writer always
 * quotes, and writes booleans as `yes` and `no`.  The file is text: a NUL byte
 * in it ends the read there.
 *
 * Nothing in the file stops the load.  An unknown key, a line that is not
 * `key = value`, and a value a key cannot take are each reported in `err` with
 * their line number, and every other line is still taken -- so an older
 * program reads a file written by a newer one.  A value a key cannot take
 * leaves that key alone, except that a number outside its range (audio_block,
 * volume, tail) is clamped to the range: a range has a nearest usable value, a
 * fixed set of words or numbers (model, size, reset, map, audio_rate, midi_rate,
 * the computer switches) has none.  A string longer than its field is truncated, and said
 * to be.
 *
 * Copyright (c) 2026 ian karlsson
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef SCPLAY_CONFIG_H
#define SCPLAY_CONFIG_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The systems, one row each for the rear COMPUTER switch, oldest machine
 * first -- the keys are computer_sc55mk2, computer_sc88, computer_sc88vl,
 * computer_sc88pro, computer_sc8820 and computer_sc8850, and app/machine.h's
 * machine_systems[] is the same order.  The SC-55 has no such switch: its
 * row stays at midi and the file keeps no key for it. */
enum
{
	CONFIG_ROW_SC55, CONFIG_ROW_SC55MK2, CONFIG_ROW_SC88, CONFIG_ROW_SC88VL, CONFIG_ROW_SC88PRO,
	CONFIG_ROW_SC8820, CONFIG_ROW_SC8850, CONFIG_SYSTEMS
};

typedef struct scgui_config
{
	char model[16];          /* "sc88pro", "sc88", "sc88vl", "sc8850", "sc8820", "sc55mk2", "sc55"; "" = the best set found */
	char rom[1024];          /* --rom: a zip or directory; "" = search the usual places */
	int size;                /* the window size: 4 the small panel, 8 twice as large */
	char audio_device[128];  /* PortAudio device name; "" = the default */
	int audio_rate;          /* the rate to ask the device for: 0 the machine's own, or 32000, 44100, 48000 */
	int audio_block;         /* frames per device buffer: 64..1024 */
	char midi[9][80];        /* host MIDI device names tied to MIDI IN A-D, MIDI OUT, Song to A-D; "" = none */
	char reset[24];          /* what precedes each song: "none", "gm", "gs", "gm2", "sc88-single", "sc88-double" */
	char map[16];            /* instrument map override: "native", "sc55", "sc88", "sc88pro" */
	int midi_rate;           /* baud of the MIDI input: 31250, 38400, 0 */
	char computer[CONFIG_SYSTEMS][8];  /* the rear switch of each system: "midi", "pc1", "pc2", "mac" ("usb" on the SC-8850 and the SC-8820, whose pc2 is its Mac position) */
	bool keep_settings;      /* keep the machine's own settings memory across sessions */
	bool boot_animation;     /* boot the firmware in the machine's own time, so the display animates */
	bool swap_buttons;       /* the pointer's right and middle buttons change places on the panel */
	float volume;            /* the knob, 0..1 */
	float tail;              /* seconds after a song's last event */
	bool sb55_window;        /* the Sound Brush's window is open */
	int sb55_interval;       /* its seconds between songs, 0..99 (SET + PAUSE) */
	bool sb55_auto_play;     /* it plays when a disk goes in (SET + PLAY) */
	bool sb55_auto_rewind;   /* STOP returns to the start of the song (SET + STOP) */
} scgui_config_t;

/* The keys are the field names, the five MIDI ports being midi_in_a,
 * midi_in_b, midi_out, song_a and song_b and the computer switches
 * computer_sc88, computer_sc88vl, computer_sc88pro, computer_sc8850,
 * computer_sc8820 and computer_sc55mk2. */

void config_defaults(scgui_config_t *c);

/* $XDG_CONFIG_HOME/scemu/scgui.conf, else ~/.config/scemu/scgui.conf, with the
 * directories made; false without a home, or when the path does not fit. */
bool config_path(char *out, size_t size);

/* Starts from the defaults, so the config is complete whatever the file holds.
 * A missing file leaves the defaults and returns true; an unreadable or partly
 * bad one still returns true with the complaints in err (empty when there were
 * none); false only when nothing could be read at all. */
bool config_load(scgui_config_t *c, const char *path, char *err, size_t err_size);

/* Writes every key, grouped, through a temp file in the same directory. */
bool config_save(const scgui_config_t *c, const char *path);

#ifdef __cplusplus
}
#endif

#endif
