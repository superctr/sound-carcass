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
 *
 *     # the rear COMPUTER switch, one position per system
 *     computer_sc88pro = midi
 *
 *     # audio: the output device, its buffer, the knob and the DSP rail
 *     audio_device = "HDA Intel PCH: ALC295 Analog"
 *     audio_block = 256
 *     volume = 0.75
 *     dac_rail = 24
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
 * fixed set of words or numbers (model, size, reset, map, midi_rate, dac_rail,
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

/* The systems whose rear COMPUTER switch the file remembers, in the order of
 * the computer[] rows: the keys are computer_sc88, computer_sc88vl,
 * computer_sc88pro and computer_sc8850. */
#define CONFIG_SYSTEMS 4

typedef struct scgui_config
{
	char model[16];          /* "sc88pro", "sc88", "sc88vl", "sc8850"; "" = the best set found */
	char rom[1024];          /* --rom: a zip or directory; "" = search the usual places */
	int size;                /* the window size: 4 the small panel, 8 twice as large */
	char audio_device[128];  /* PortAudio device name; "" = the default */
	int audio_block;         /* frames per device buffer: 64..1024 */
	char midi[5][80];        /* host MIDI device names tied to MIDI IN A, MIDI IN B, MIDI OUT, Song to A, Song to B; "" = none */
	char reset[24];          /* what precedes each song: "none", "gm", "gs", "gm2", "sc88-single", "sc88-double" */
	char map[16];            /* instrument map override: "native", "sc55", "sc88", "sc88pro" */
	int midi_rate;           /* baud of the MIDI input: 31250, 38400, 0 */
	char computer[CONFIG_SYSTEMS][8];  /* the rear switch of each system: "midi", "pc1", "pc2", "mac" ("usb" on the SC-8850) */
	bool keep_settings;      /* keep the machine's own settings memory across sessions */
	float volume;            /* the knob, 0..1 */
	int dac_rail;            /* the DSP output rail in bits: 24, the unit's, or 29, the wide one */
	float tail;              /* seconds after a song's last event */
} scgui_config_t;

/* The keys are the field names, the five MIDI ports being midi_in_a,
 * midi_in_b, midi_out, song_a and song_b and the four computer switches
 * computer_sc88, computer_sc88vl, computer_sc88pro and computer_sc8850. */

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
