#include <string.h>
#include "sub_hle.h"

void sub_hle_init(sub_hle_t *sub, void (*irq)(void *user, bool state), void (*midi_out)(void *user, uint8_t byte), void *user)
{
	memset(sub, 0, sizeof(*sub));
	sub->irq = irq;
	sub->midi_out = midi_out;
	sub->user = user;
	memset(sub->keys, 0xff, sizeof(sub->keys));
}

void sub_hle_reset(sub_hle_t *sub)
{
	memset(sub->dpram, 0, sizeof(sub->dpram));
	memset(sub->ipcm, 0, sizeof(sub->ipcm));
	memset(sub->ipcer, 0, sizeof(sub->ipcer));
	memset(sub->flags, 0, sizeof(sub->flags));
	sub->sem = 0;
	sub->spcon = 0;
	sub->pa = sub->pa_dir = sub->pb = sub->pb_dir = 0;
	sub->int_state = false;
	sub->in_reset = false;
	memset(sub->src, 0, sizeof(sub->src));
	sub->queue_head = sub->queue_count = 0;
	sub->busy = false;
	sub->deliver_frames = 0;
}

uint8_t sub_hle_read(sub_hle_t *sub, uint32_t offset)
{
	offset &= 0xff;
	if (offset < sizeof(sub->dpram))
		return sub->dpram[offset];
	return 0;
}

void sub_hle_write(sub_hle_t *sub, uint32_t offset, uint8_t data)
{
	offset &= 0xff;
	if (offset < sizeof(sub->dpram))
		sub->dpram[offset] = data;
}

void sub_hle_midi_byte(sub_hle_t *sub, int source, uint8_t byte)
{
	(void)sub;
	(void)source;
	(void)byte;
}

void sub_hle_set_key(sub_hle_t *sub, int row, int bit, bool down)
{
	if (down)
		sub->keys[row & 3] &= (uint8_t)~(1u << bit);
	else
		sub->keys[row & 3] |= (uint8_t)(1u << bit);
}

void sub_hle_frame(sub_hle_t *sub)
{
	if (sub->deliver_frames && !--sub->deliver_frames)
		sub->busy = false;
}
