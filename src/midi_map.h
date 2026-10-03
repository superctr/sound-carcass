#ifndef SCEMU_MIDI_MAP_H
#define SCEMU_MIDI_MAP_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Rewrites a MIDI stream so that every part plays from one instrument map
 * whatever the song asks for: bank select LSB values are replaced, a
 * program change on a part that has not had one gets it first, and a
 * reset (GS, GM, XG, SC-88 mode set) is followed by the selection and a
 * program change on all sixteen parts. */

#define MIDI_MAP_PORTS 2
#define MIDI_MAP_MAX_OUT 96

typedef struct midi_map_port
{
	uint8_t status;
	uint8_t data[2];
	uint8_t need;
	uint8_t have;
	bool in_sysex;
	uint8_t sysex[12];
	uint8_t sysex_len;
	uint16_t selected;
} midi_map_port_t;

typedef struct midi_map
{
	uint8_t map;
	midi_map_port_t port[MIDI_MAP_PORTS];
} midi_map_t;

void midi_map_reset(midi_map_t *f, uint8_t map);

/* The selection for all parts of a port; returns the number of bytes. */
size_t midi_map_preset(midi_map_t *f, int port, uint8_t *out);

/* One input byte in, the bytes to send out (0 to MIDI_MAP_MAX_OUT). */
size_t midi_map_filter(midi_map_t *f, int port, uint8_t byte, uint8_t *out);

#endif
