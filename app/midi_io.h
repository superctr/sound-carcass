/* scgui: MIDI ports on the host, through the ALSA sequencer.
 *
 * The program is a sequencer client with two input ports (the machine's
 * MIDI IN A and B), one output port (its MIDI OUT) and two more outputs
 * that carry the song being played, for a real unit beside the emulated
 * one.  Other clients may connect to them; this file also connects them to
 * a chosen port each.
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
	int client, port;
	char name[80];
	bool readable, writable;    /* can feed us / can be fed */
} midi_port_info_t;

/* The other clients' ports; returns how many fit. */
int midi_io_list(midi_port_info_t *out, int max);

midi_io_t *midi_io_open(const char *client_name);
void midi_io_close(midi_io_t *io);

/* Connect one of our inputs to a source, or one of our outputs to a
 * destination; client < 0 disconnects. */
bool midi_io_connect_input(midi_io_t *io, int which, int client, int port);
bool midi_io_connect_output(midi_io_t *io, int which, int client, int port);

/* One pending message's bytes, or 0; `which` says on which input. */
size_t midi_io_read(midi_io_t *io, int *which, uint8_t *buf, size_t max);
void midi_io_write(midi_io_t *io, int which, const uint8_t *bytes, size_t count);

#endif
