#include <string.h>
#include "sub_hle.h"

#define BIT(x, n) (((x) >> (n)) & 1)

static const uint8_t SOURCE_FLAGS[SUB_SOURCES] = { 0x00, 0x10, 0x20 };

#define TX_RING_START 0x24
#define TX_RING_END 0xc0
#define TX_WRITE_PTR 0xd5
#define TX_READ_PTR 0xd4

typedef struct exclusive_block { uint8_t address; uint8_t code; bool paged; } exclusive_block_t;

static const exclusive_block_t BLOCKS[] =
{
	{ 0x00, 0x20, true }, { 0x08, 0x28, true },
	{ 0x20, 0x40, true }, { 0x28, 0x48, true },
	{ 0x21, 0x50, true }, { 0x29, 0x58, true },
	{ 0x40, 0x60, true }, { 0x48, 0x68, true },
	{ 0x50, 0x70, true }, { 0x58, 0x78, true },
	{ 0x41, 0x80, true }, { 0x49, 0x88, true },
	{ 0x51, 0x90, true }, { 0x59, 0x98, true },
	{ 0x22, 0xa0, true }, { 0x2a, 0xa8, true },
	{ 0x23, 0xb0, false }, { 0x24, 0xb1, false }, { 0x25, 0xb2, false },
	{ 0x26, 0xb3, false }, { 0x27, 0xb4, false },
	{ 0x2b, 0xb8, true },
};

static void set_int(sub_hle_t *sub, bool state)
{
	sub->int_state = state;
	if (sub->irq)
		sub->irq(sub->user, state);
}

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
	sub->sem = 0x80;
	sub->spcon = 0;
	sub->pa = sub->pa_dir = sub->pb = sub->pb_dir = 0;
	set_int(sub, false);

	memset(sub->src, 0, sizeof(sub->src));
	sub->queue_head = sub->queue_count = 0;
	sub->busy = false;
	sub->deliver_frames = 0;

	sub->dpram[TX_READ_PTR] = sub->dpram[TX_WRITE_PTR] = TX_RING_START;
	sub->tx_rd = TX_RING_START;
	sub->tx_left = 0;
	sub->tx_end = TX_RING_START;
	sub->tx_running = false;
	sub->tx_frames = 0;
}

/* ---------------------------------------------------------------- */
/* system bus interface                                             */

static uint8_t port_r(const sub_hle_t *sub)
{
	switch ((sub->spcon >> 2) & 7)
	{
	case 0: return sub->pa;
	case 1: return sub->pa_dir;
	case 2: return sub->pb;
	case 3: return sub->pb_dir;
	case 4:
	{
		uint8_t keys = 0xff;
		for (int row = 0; row < 4; row++)
			if (BIT(sub->pb, row))
				keys &= sub->keys[row];
		return keys;
	}
	default: return 0xff;
	}
}

static void port_w(sub_hle_t *sub, uint8_t data)
{
	switch ((sub->spcon >> 2) & 7)
	{
	case 0: sub->pa = data; break;
	case 1: sub->pa_dir = data; break;
	case 2: sub->pb = data; break;
	case 3: sub->pb_dir = data; break;
	default: break;
	}
}

void sub_hle_reset_w(sub_hle_t *sub, bool state)
{
	if (sub->in_reset == !state)
		return;
	sub->in_reset = !state;
	if (sub->in_reset)
		sub_hle_reset(sub);
}

uint8_t sub_hle_read(sub_hle_t *sub, uint32_t offset)
{
	offset &= 0xff;
	if (offset < 0xd8)
	{
		sub->flags[offset >> 3] &= (uint8_t)~(1u << (offset & 7));
		return sub->dpram[offset];
	}
	if (offset < 0xdc)
		return sub->ipcm[offset & 3];
	if (offset < 0xe0)
	{
		const uint8_t data = sub->ipcer[offset & 3];
		sub->ipcer[offset & 3] = 0;
		if (offset == 0xdc && sub->int_state)
		{
			set_int(sub, false);
			sub->deliver_frames = SUB_DELIVER_FRAMES;
		}
		return data;
	}
	if (offset < 0xfb)
		return sub->flags[offset - 0xe0];
	if (offset == 0xfd)
		return sub->sem;
	if (offset == 0xfe)
		return port_r(sub);
	if (offset == 0xff)
		return sub->spcon;
	return 0xff;
}

void sub_hle_write(sub_hle_t *sub, uint32_t offset, uint8_t data)
{
	offset &= 0xff;
	if (offset < 0xd8)
	{
		sub->dpram[offset] = data;
		sub->flags[offset >> 3] |= (uint8_t)(1u << (offset & 7));
		if (offset == TX_WRITE_PTR && !sub->tx_running)
		{
			sub->tx_running = true;
			sub->tx_frames = 1;
		}
		return;
	}

	if (offset < 0xdc)
	{
		sub->ipcm[offset & 3] = data;
		if (offset == 0xd8)
			sub->sem &= 0x7f;
	}
	else if (offset == 0xfd)
	{
		const uint8_t bit = (uint8_t)(1u << (data & 7));
		if (BIT(data, 7))
			sub->sem |= bit;
		else
			sub->sem &= (uint8_t)~bit;
		if (!sub->busy && sub->queue_count)
			sub->deliver_frames = SUB_DELIVER_FRAMES;
	}
	else if (offset == 0xfe)
		port_w(sub, data);
	else if (offset == 0xff)
		sub->spcon = data;
}

/* ---------------------------------------------------------------- */
/* IPC messages to the main CPU                                     */

static void deliver(sub_hle_t *sub)
{
	if (sub->busy || !sub->queue_count)
		return;

	sub_message_t *m = &sub->queue[sub->queue_head];
	const int src = (m->flags >> 4) & 3;
	if (m->block_size)
	{
		if (BIT(sub->sem, src))
		{
			sub->deliver_frames = SUB_BLOCK_RETRY_FRAMES;
			return;
		}
		memcpy(&sub->dpram[0], m->block, m->block_size);
		sub->sem |= (uint8_t)(1u << src);
	}

	sub->ipcer[0] = m->code;
	sub->ipcer[1] = m->flags;
	sub->ipcer[2] = m->d1;
	sub->ipcer[3] = m->d2;
	sub->queue_head = (uint8_t)((sub->queue_head + 1) % SUB_QUEUE_SIZE);
	sub->queue_count--;
	sub->busy = true;
	set_int(sub, true);
}

bool sub_hle_ready(const sub_hle_t *sub)
{
	return sub->queue_count + (SUB_MAX_EXCLUSIVE + SUB_BLOCK_SIZE - 1) / SUB_BLOCK_SIZE <= SUB_QUEUE_SIZE;
}

static void queue(sub_hle_t *sub, uint8_t code, uint8_t flags, uint8_t d1, uint8_t d2,
		const uint8_t *block, uint8_t block_size)
{
	if (sub->queue_count >= SUB_QUEUE_SIZE)
	{
		sub->queue_drops++;
		return;
	}

	sub_message_t *m = &sub->queue[(sub->queue_head + sub->queue_count) % SUB_QUEUE_SIZE];
	m->code = code;
	m->flags = flags;
	m->d1 = d1;
	m->d2 = d2;
	m->block_size = block_size;
	if (block_size)
		memcpy(m->block, block, block_size);
	sub->queue_count++;

	if (!sub->busy)
		deliver(sub);
}

/* ---------------------------------------------------------------- */
/* MIDI input parsing                                               */

/* s->sysex holds the bytes after F0, ending with F7 */
static void send_sysex(sub_hle_t *sub, int src, const sub_source_t *s)
{
	const uint8_t *x = s->sysex;
	const uint16_t size = s->sysex_size;
	uint8_t code;
	uint8_t chan = 0;
	uint8_t request = 0;

	if (size >= 8 && x[0] == 0x41 && x[2] == 0x42 && (x[3] == 0x12 || x[3] == 0x11))
	{
		const exclusive_block_t *block = NULL;
		for (size_t i = 0; i < sizeof(BLOCKS) / sizeof(BLOCKS[0]); i++)
			if (BLOCKS[i].address == x[4] && (BLOCKS[i].paged || (x[5] & 0xf0) == 0))
				block = &BLOCKS[i];
		if (!block)
			return;
		code = (uint8_t)(block->code + (block->paged ? (x[5] >> 4) : 0));
		chan = x[5] & 0x0f;
		request = (x[3] == 0x11) ? 0x80 : 0x00;
	}
	else if (size >= 5 && x[0] == 0x7e)
		code = 0xee;
	else if (size >= 5 && x[0] == 0x7f)
		code = 0xef;
	else
		return;

	if (size > SUB_MAX_EXCLUSIVE)
	{
		sub->sysex_drops++;
		return;
	}

	const uint16_t total = (uint16_t)(size - 1);
	for (uint16_t pos = 0; pos < total; pos += SUB_BLOCK_SIZE)
	{
		const uint16_t left = (uint16_t)(total - pos);
		const uint8_t len = (uint8_t)(left < SUB_BLOCK_SIZE ? left : SUB_BLOCK_SIZE);
		const bool more = pos + len < total;
		queue(sub, pos ? 0xe7 : code,
				(uint8_t)(SOURCE_FLAGS[src] | 0x80 | (more ? 0x40 : 0) | chan),
				(uint8_t)(len | (pos ? 0 : request)), 0,
				&x[pos], len);
	}
}

void sub_hle_midi_byte(sub_hle_t *sub, int source, uint8_t data)
{
	if (source < 0 || source >= SUB_SOURCES)
		return;

	sub_source_t *s = &sub->src[source];

	if (data >= 0xf8)
		return;

	if (data >= 0x80)
	{
		if (s->in_sysex)
		{
			if (data == 0xf7)
			{
				s->sysex[s->sysex_size++] = data;
				send_sysex(sub, source, s);
			}
			s->in_sysex = false;
			s->sysex_size = 0;
		}
		if (data == 0xf0)
		{
			s->in_sysex = true;
			s->status = 0;
		}
		else if (data < 0xf0)
		{
			s->status = data;
			s->count = 0;
		}
		else
			s->status = 0;
		return;
	}

	if (s->in_sysex)
	{
		if (s->sysex_size < SUB_MAX_EXCLUSIVE + 8)
			s->sysex[s->sysex_size++] = data;
		return;
	}
	if (s->status == 0)
		return;

	s->data[s->count++] = data;
	const int type = s->status >> 4;
	const int length = (type == 0xc || type == 0xd) ? 1 : 2;
	if (s->count < length)
		return;
	s->count = 0;

	queue(sub, (uint8_t)(type - 7), (uint8_t)((s->status & 0x0f) | SOURCE_FLAGS[source]),
			s->data[0], (length == 2) ? s->data[1] : 0, NULL, 0);
}

void sub_hle_set_key(sub_hle_t *sub, int row, int bit, bool down)
{
	if (down)
		sub->keys[row & 3] &= (uint8_t)~(1u << bit);
	else
		sub->keys[row & 3] |= (uint8_t)(1u << bit);
}

/* ---------------------------------------------------------------- */
/* MIDI OUT: bytes queued by the main CPU in the dual port RAM ring  */

static uint8_t ring_advance(uint8_t offset, uint8_t count)
{
	offset += count;
	if (offset >= TX_RING_END)
		offset -= TX_RING_END - TX_RING_START;
	return offset;
}

static void tx_step(sub_hle_t *sub)
{
	int guard = (TX_RING_END - TX_RING_START) / 4 + 1;
	while (!sub->tx_left)
	{
		if (sub->tx_rd == sub->dpram[TX_WRITE_PTR] || !guard--)
		{
			sub->tx_running = false;
			return;
		}
		sub->tx_left = sub->dpram[sub->tx_rd];
		sub->tx_end = ring_advance(sub->tx_rd, (uint8_t)((sub->tx_left + 4) & ~3));
		sub->tx_rd = ring_advance(sub->tx_rd, 1);
		if (!sub->tx_left)
			sub->dpram[TX_READ_PTR] = sub->tx_rd = sub->tx_end;
	}

	const uint8_t data = sub->dpram[sub->tx_rd];
	sub->tx_rd = ring_advance(sub->tx_rd, 1);
	if (!--sub->tx_left)
		sub->dpram[TX_READ_PTR] = sub->tx_rd = sub->tx_end;

	if (sub->midi_out)
		sub->midi_out(sub->user, data);

	sub->tx_running = true;
	sub->tx_frames = SUB_TX_BYTE_FRAMES;
}

void sub_hle_frame(sub_hle_t *sub)
{
	if (sub->deliver_frames && !--sub->deliver_frames)
	{
		sub->busy = false;
		deliver(sub);
	}

	if (sub->tx_running && sub->tx_frames && !--sub->tx_frames)
		tx_step(sub);
}
