#include <stdlib.h>
#include <string.h>
#include "sc55.h"
#include "state_chunks.h"

#define TAG STATE_TAG

enum
{
	CHUNK_BOARD, CHUNK_SRAM, CHUNK_CPU, CHUNK_GP, CHUNK_GP_ERAM, CHUNK_LCD, CHUNK_USART,
	CHUNK_MIDI, CHUNK_COUNT
};

static const uint32_t CHUNK_TAG[CHUNK_COUNT] =
{
	TAG('B', 'R', 'D', '4'), TAG('S', 'R', 'A', 'M'), TAG('C', 'P', 'U', ' '), TAG('G', 'P', ' ', ' '),
	TAG('G', 'E', 'R', 'M'), TAG('L', 'C', 'D', ' '), TAG('U', 'S', 'R', 'T'), TAG('M', 'I', 'D', 'I')
};

/* ---------------------------------------------------------------- the chunks */

static void put_board(state_writer_t *w, const sc55_t *b)
{
	put32(w, b->cpu_quarter_cycles);
	put8(w, (uint8_t)b->gp_pair);
	put_bool(w, b->lcd_powered);
	put8(w, b->scan);
	put8(w, b->int_mask);
	put8(w, b->int_pending);
	put8(w, b->lcd_done);
	put_bytes(w, b->keys, 3);
	put64(w, b->gp_written);
	put64(w, b->frame);
	put32(w, b->midi.drops);
}

static void get_board(state_reader_t *r, sc55_t *b)
{
	b->cpu_quarter_cycles = get32(r);
	b->gp_pair = get8(r);
	b->lcd_powered = get_bool(r);
	b->scan = get8(r);
	b->int_mask = get8(r);
	b->int_pending = get8(r);
	b->lcd_done = get8(r);
	get_bytes(r, b->keys, 3);
	b->gp_written = get64(r);
	b->frame = get64(r);
	b->midi.drops = get32(r);
}

static void put_usart(state_writer_t *w, const sc55_t *b)
{
	put8(w, (uint8_t)b->usart.next);
	put8(w, b->usart.mode);
	put8(w, b->usart.command);
	put8(w, b->usart.status);
	put8(w, b->usart.rx_data);
	put_bool(w, b->rxrdy);
}

static void get_usart(state_reader_t *r, sc55_t *b)
{
	const uint8_t next = get8(r);
	if (next > I8251_NEXT_COMMAND)
		r->ok = false;
	b->usart.next = (i8251_next_t)next;
	b->usart.mode = get8(r);
	b->usart.command = get8(r);
	b->usart.status = get8(r);
	b->usart.rx_data = get8(r);
	b->rxrdy = get_bool(r);
}

/* ---------------------------------------------------------------- the whole */

static size_t write_state(const sc55_t *b, uint8_t *out)
{
	state_writer_t w = { out, 0 };
	state_header_t h = { .model = (uint32_t)b->model, .flags = 0, .chunk_count = CHUNK_COUNT,
			.rom_id = b->rom_id, .frame = b->frame };
	put_header(&w, &h);

	for (int which = 0; which < CHUNK_COUNT; which++)
	{
		put32(&w, CHUNK_TAG[which]);
		const size_t size_at = w.size;
		put32(&w, 0);
		const size_t start = w.size;
		switch (which)
		{
		case CHUNK_BOARD:   put_board(&w, b); break;
		case CHUNK_SRAM:    put_bytes(&w, b->sram, SC55_SRAM_SIZE); break;
		case CHUNK_CPU:     state_put_h8500(&w, &b->cpu); break;
		case CHUNK_GP:      state_put_gp(&w, &b->gp); break;
		case CHUNK_GP_ERAM: put_u16s(&w, b->gp.eram, GP_ERAM_SIZE); break;
		case CHUNK_LCD:     state_put_lcd(&w, &b->lcd); break;
		case CHUNK_USART:   put_usart(&w, b); break;
		case CHUNK_MIDI:    state_put_midi(&w, &b->midi); break;
		default: break;
		}
		if (out)
		{
			const uint32_t size = (uint32_t)(w.size - start);
			out[size_at] = (uint8_t)size;
			out[size_at + 1] = (uint8_t)(size >> 8);
			out[size_at + 2] = (uint8_t)(size >> 16);
			out[size_at + 3] = (uint8_t)(size >> 24);
		}
	}
	return w.size;
}

size_t sc55_state_size(const sc55_t *b)
{
	return write_state(b, NULL);
}

size_t sc55_state_save(const sc55_t *b, void *buffer, size_t size)
{
	const size_t need = write_state(b, NULL);
	if (size < need)
		return 0;
	return write_state(b, buffer);
}

static bool read_chunk(int which, state_reader_t *r, sc55_t *b)
{
	switch (which)
	{
	case CHUNK_BOARD:   get_board(r, b); break;
	case CHUNK_SRAM:    get_bytes(r, b->sram, SC55_SRAM_SIZE); break;
	case CHUNK_CPU:     state_get_h8500(r, &b->cpu); break;
	case CHUNK_GP:      state_get_gp(r, &b->gp); break;
	case CHUNK_GP_ERAM: get_u16s(r, b->gp.eram, GP_ERAM_SIZE); break;
	case CHUNK_LCD:     state_get_lcd(r, &b->lcd); break;
	case CHUNK_USART:   get_usart(r, b); break;
	case CHUNK_MIDI:    state_get_midi(r, &b->midi); break;
	default: return false;
	}
	return r->ok && r->at == r->size;
}

bool sc55_state_load(sc55_t *b, const void *buffer, size_t size)
{
	state_reader_t r = { buffer, size, 0, true };
	state_header_t h;
	if (!get_header(&r, &h) || h.model != (uint32_t)b->model || h.rom_id != b->rom_id)
		return false;

	/* everything goes into a copy first, so a bad stream leaves the machine as it was */
	sc55_t *t = malloc(sizeof(*t));
	if (!t)
		return false;
	memcpy(t, b, sizeof(*t));

	bool ok = true;
	uint32_t seen = 0;
	for (uint32_t n = 0; ok && n < h.chunk_count; n++)
	{
		const uint32_t tag = get32(&r);
		const uint32_t len = get32(&r);
		if (!r.ok || len > r.size - r.at)
		{
			ok = false;
			break;
		}
		int which = -1;
		for (int k = 0; k < CHUNK_COUNT; k++)
			if (CHUNK_TAG[k] == tag)
				which = k;
		if (which >= 0)
		{
			state_reader_t body = { r.p + r.at, len, 0, true };
			ok = read_chunk(which, &body, t);
			seen |= 1u << which;
		}
		r.at += len;
	}
	const uint32_t need = (1u << CHUNK_COUNT) - 1;
	ok = ok && (seen & need) == need;

	if (ok)
	{
		memcpy(b, t, sizeof(*b));
		b->gp.link.user = b;
		b->cpu.bus.user = b;
		h8500_set_irq(&b->cpu, H8500_IRQ0, b->gp.irq_pending);
		h8500_set_irq(&b->cpu, H8500_IRQ1, (b->int_pending & ~b->int_mask) != 0);
	}
	free(t);
	return ok;
}
