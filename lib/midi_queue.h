#ifndef SCEMU_MIDI_QUEUE_H
#define SCEMU_MIDI_QUEUE_H

#include <stdint.h>
#include <stdbool.h>

/* The MIDI bytes a host has handed a board, held per port until their frame,
 * and delivered one byte per wire time per port from then on. */

#define MIDI_QUEUE_PORTS 4
#define MIDI_QUEUE_SIZE 16384
#define MIDI_QUEUE_DEFAULT_BAUD 31250
#define MIDI_QUEUE_BYTE_UNITS (10 * 32000)

typedef struct midi_queue_event
{
	uint32_t frame;
	uint8_t byte;
} midi_queue_event_t;

typedef struct midi_queue
{
	int ports;
	midi_queue_event_t events[MIDI_QUEUE_PORTS][MIDI_QUEUE_SIZE];
	uint32_t head[MIDI_QUEUE_PORTS];
	uint32_t count[MIDI_QUEUE_PORTS];
	uint32_t credit[MIDI_QUEUE_PORTS];
	uint32_t baud;
	uint32_t drops;
} midi_queue_t;

/* a byte the board can take now, or false to leave it queued */
typedef bool (*midi_queue_take_fn)(void *user, int port, uint8_t byte);

void midi_queue_init(midi_queue_t *q, int ports);
void midi_queue_reset(midi_queue_t *q);
void midi_queue_push(midi_queue_t *q, int port, uint8_t byte, uint32_t frame);
void midi_queue_deliver(midi_queue_t *q, uint32_t frame, midi_queue_take_fn take, void *user);

#endif
