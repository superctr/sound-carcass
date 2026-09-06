#include <string.h>
#include "midi_queue.h"

void midi_queue_init(midi_queue_t *q, int ports, uint32_t rate)
{
	memset(q, 0, sizeof(*q));
	q->ports = ports;
	q->baud = MIDI_QUEUE_DEFAULT_BAUD;
	q->byte_units = 10 * rate;
}

void midi_queue_reset(midi_queue_t *q)
{
	memset(q->head, 0, sizeof(q->head));
	memset(q->count, 0, sizeof(q->count));
	memset(q->credit, 0, sizeof(q->credit));
	q->drops = 0;
}

void midi_queue_push(midi_queue_t *q, int port, uint8_t byte, uint32_t frame)
{
	port &= q->ports - 1;
	if (q->count[port] >= MIDI_QUEUE_SIZE)
	{
		q->drops++;
		return;
	}
	const uint32_t slot = (q->head[port] + q->count[port]) % MIDI_QUEUE_SIZE;
	q->events[port][slot].frame = frame;
	q->events[port][slot].byte = byte;
	q->count[port]++;
}

void midi_queue_deliver(midi_queue_t *q, uint32_t frame, midi_queue_take_fn take, void *user)
{
	for (int port = 0; port < q->ports; port++)
	{
		if (q->credit[port] < q->byte_units)
			q->credit[port] += q->baud;
		while (q->count[port] && (!q->baud || q->credit[port] >= q->byte_units))
		{
			const midi_queue_event_t *e = &q->events[port][q->head[port]];
			if (e->frame > frame)
				break;
			if (!take(user, port, e->byte))
				break;
			q->head[port] = (q->head[port] + 1) % MIDI_QUEUE_SIZE;
			q->count[port]--;
			if (q->baud)
				q->credit[port] -= q->byte_units;
		}
	}
}
