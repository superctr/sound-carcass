/* scgui: MIDI ports on the host, through PortMidi.
 *
 * The program has ports of its own that other programs can connect to:
 * two inputs (the machine's MIDI IN A and B), one output (its MIDI OUT)
 * and two more outputs that carry the song being played, for a real unit
 * beside the emulated one.  Besides those, each of the five can be tied to
 * one of the host's devices, so a keyboard feeds an input or a synth hangs
 * off an output without any routing on the host.
 *
 * Copyright (c) 2026 ian karlsson
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef SCGUI_MIDI_IO_H
#define SCGUI_MIDI_IO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct midi_io midi_io_t;

enum { MIDI_IO_IN_A, MIDI_IO_IN_B, MIDI_IO_INPUT_COUNT };
enum { MIDI_IO_OUT, MIDI_IO_SONG_A, MIDI_IO_SONG_B, MIDI_IO_OUTPUT_COUNT };

typedef struct midi_port_info
{
	int in_id, out_id;          /* what the connect calls want, as a source and as a
	                               destination; -1 for a side it lacks; void after a rescan */
	char name[80];
	bool readable, writable;    /* can feed us / can be fed */
} midi_port_info_t;

midi_io_t *midi_io_open(const char *client_name);
void midi_io_close(midi_io_t *io);

/* The host's devices, ours left out; returns how many fit. */
int midi_io_list(midi_io_t *io, midi_port_info_t *out, int max);
/* Look for devices again (after plugging something in).  Ties to devices
 * that are still there are kept, by name; the ids in a previous list are
 * void. */
void midi_io_rescan(midi_io_t *io);

/* Tie one of our inputs to a device that feeds it, or one of our outputs
 * to a device it feeds; id < 0 unties. */
bool midi_io_connect_input(midi_io_t *io, int which, int id);
bool midi_io_connect_output(midi_io_t *io, int which, int id);

/* Pending bytes on one input, or 0; `which` says on which.  Whole
 * messages, but possibly several at once. */
size_t midi_io_read(midi_io_t *io, int *which, uint8_t *buf, size_t max);
void midi_io_write(midi_io_t *io, int which, const uint8_t *bytes, size_t count);

#endif
