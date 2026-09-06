#include <stdlib.h>
#include <string.h>
#include "sc55mk2.h"
#include "state_chunks.h"

#define TAG STATE_TAG

enum
{
	CHUNK_BOARD, CHUNK_SRAM, CHUNK_CPU, CHUNK_GP, CHUNK_GP_ERAM, CHUNK_LCD, CHUNK_SUB,
	CHUNK_MIDI, CHUNK_COUNT
};

static const uint32_t CHUNK_TAG[CHUNK_COUNT] =
{
	TAG('B', 'R', 'D', '3'), TAG('S', 'R', 'A', 'M'), TAG('C', 'P', 'U', ' '), TAG('G', 'P', ' ', ' '),
	TAG('G', 'E', 'R', 'M'), TAG('L', 'C', 'D', ' '), TAG('S', 'U', 'B', '3'), TAG('M', 'I', 'D', 'I')
};

/* ---------------------------------------------------------------- the chunks */

static void put_board(state_writer_t *w, const sc55mk2_t *b)
{
	put32(w, b->cpu_quarter_cycles);
	put8(w, (uint8_t)b->gp_pair);
	put8(w, b->sys_control);
	put8(w, b->int_enable);
	put8(w, b->int_trigger);
	put_bool(w, b->lcd_powered);
	put32(w, (uint32_t)b->factory);
	put32(w, b->factory_frames);
	put64(w, b->gp_written);
	put32(w, (uint32_t)b->computer_switch);
	put64(w, b->frame);
	put32(w, b->midi.drops);
}

static void get_board(state_reader_t *r, sc55mk2_t *b)
{
	b->cpu_quarter_cycles = get32(r);
	b->gp_pair = get8(r);
	b->sys_control = get8(r);
	b->int_enable = get8(r);
	b->int_trigger = get8(r);
	b->lcd_powered = get_bool(r);
	b->factory = (sc55mk2_factory_t)get32(r);
	b->factory_frames = get32(r);
	b->gp_written = get64(r);
	b->computer_switch = (scemu_computer_switch_t)get32(r);
	b->frame = get64(r);
	b->midi.drops = get32(r);
}

static void put_gp(state_writer_t *w, const gp_t *g)
{
	for (int n = 0; n < GP_SLOTS; n++)
	{
		const gp_slot_t *s = &g->slots[n];
		put_u32s(w, s->wide, 6);
		put_u16s(w, s->narrow, 12);
		put_i32(w, s->filter_low);
		put_i32(w, s->filter_band);
		put8(w, s->prefetch);
		put_bool(w, s->crossed);
	}
	put32(w, g->key_mask);
	put32(w, g->key_mask_pending);
	put_bool(w, g->key_mask_dirty);
	put32(w, g->write_latch);
	put32(w, g->read_latch);
	put32(w, g->rom_address);
	put8(w, g->rom_byte);
	put8(w, g->output_config);
	put8(w, g->slot_config);
	put8(w, g->selected_slot);
	put8(w, g->irq_slot);
	put_bool(w, g->irq_pending);
	put16(w, g->frame_counter);
	put_bool(w, g->first_frame);
	put_i32s(w, g->mix, 2);
	put_i32s(w, g->send, 2);
	put_i32s(w, g->returns, 6);
	for (int n = 0; n < 2; n++)
		put_i32s(w, g->sample[n], 2);
}

static void get_gp(state_reader_t *r, gp_t *g)
{
	for (int n = 0; n < GP_SLOTS; n++)
	{
		gp_slot_t *s = &g->slots[n];
		get_u32s(r, s->wide, 6);
		get_u16s(r, s->narrow, 12);
		s->filter_low = get_i32(r);
		s->filter_band = get_i32(r);
		s->prefetch = get8(r);
		s->crossed = get_bool(r);
	}
	g->key_mask = get32(r);
	g->key_mask_pending = get32(r);
	g->key_mask_dirty = get_bool(r);
	g->write_latch = get32(r);
	g->read_latch = get32(r);
	g->rom_address = get32(r);
	g->rom_byte = get8(r);
	g->output_config = get8(r);
	g->slot_config = get8(r);
	g->selected_slot = get8(r);
	g->irq_slot = get8(r);
	g->irq_pending = get_bool(r);
	g->frame_counter = get16(r);
	g->first_frame = get_bool(r);
	get_i32s(r, g->mix, 2);
	get_i32s(r, g->send, 2);
	get_i32s(r, g->returns, 6);
	for (int n = 0; n < 2; n++)
		get_i32s(r, g->sample[n], 2);
}

static void put_lcd(state_writer_t *w, const lcd_t *l)
{
	put_bytes(w, l->out.ddram, 80);
	put_bytes(w, l->out.cgram, 64);
	put_bool(w, l->out.display_on);
	put8(w, l->address);
	put_bool(w, l->cgram_mode);
	put_bool(w, l->increment);
	put_bool(w, l->shift);
	put_bool(w, l->two_line);
	put8(w, l->display_shift);
}

static void get_lcd(state_reader_t *r, lcd_t *l)
{
	get_bytes(r, l->out.ddram, 80);
	get_bytes(r, l->out.cgram, 64);
	l->out.display_on = get_bool(r);
	l->out.changed = true;
	l->address = get8(r);
	l->cgram_mode = get_bool(r);
	l->increment = get_bool(r);
	l->shift = get_bool(r);
	l->two_line = get_bool(r);
	l->display_shift = get8(r);
}

static void put_sub(state_writer_t *w, const sub55_hle_t *s)
{
	put_bytes(w, s->dpram, SUB55_DPRAM_SIZE);
	put_bytes(w, s->flags, SUB55_FLAG_BYTES);
	put8(w, s->reason);
	put8(w, s->sem);
	put8(w, s->p0);
	put8(w, s->p0_dir);
	put_bytes(w, s->keys, 4);
	put_bool(w, s->started);
	for (int n = 0; n < SUB55_SOURCES; n++)
	{
		put8(w, s->src[n].status);
		put_bytes(w, s->src[n].data, 2);
		put8(w, s->src[n].count);
		put8(w, s->src[n].sense);
	}
	for (int n = 0; n < SUB55_CHANNELS; n++)
	{
		put16(w, s->ch[n].count);
		for (uint16_t i = 0; i < s->ch[n].count; i++)
			put8(w, s->ch[n].fifo[(s->ch[n].head + i) % SUB55_FIFO_SIZE]);
		put8(w, s->ch[n].last_status);
	}
	put8(w, s->tx_count);
	for (uint8_t i = 0; i < s->tx_count; i++)
		put8(w, s->tx[(s->tx_head + i) % SUB55_TX_SIZE]);
	put8(w, s->tx_status);
	put32(w, s->tx_frames);
	put32(w, s->tick_frames);
	put8(w, s->sense_div);
	put_bool(w, s->sensing);
}

static void get_sub(state_reader_t *r, sub55_hle_t *s)
{
	get_bytes(r, s->dpram, SUB55_DPRAM_SIZE);
	get_bytes(r, s->flags, SUB55_FLAG_BYTES);
	s->reason = get8(r);
	s->sem = get8(r);
	s->p0 = get8(r);
	s->p0_dir = get8(r);
	get_bytes(r, s->keys, 4);
	s->started = get_bool(r);
	for (int n = 0; n < SUB55_SOURCES; n++)
	{
		s->src[n].status = get8(r);
		get_bytes(r, s->src[n].data, 2);
		s->src[n].count = get8(r);
		s->src[n].sense = get8(r);
	}
	for (int n = 0; n < SUB55_CHANNELS; n++)
	{
		const uint16_t count = get16(r);
		if (count > SUB55_FIFO_SIZE)
		{
			r->ok = false;
			return;
		}
		s->ch[n].head = 0;
		s->ch[n].count = count;
		for (uint16_t i = 0; i < count; i++)
			s->ch[n].fifo[i] = get8(r);
		s->ch[n].last_status = get8(r);
	}
	const uint8_t tx_count = get8(r);
	if (tx_count > SUB55_TX_SIZE)
	{
		r->ok = false;
		return;
	}
	s->tx_head = 0;
	s->tx_count = tx_count;
	for (uint8_t i = 0; i < tx_count; i++)
		s->tx[i] = get8(r);
	s->tx_status = get8(r);
	s->tx_frames = get32(r);
	s->tick_frames = get32(r);
	s->sense_div = get8(r);
	s->sensing = get_bool(r);
}

/* ---------------------------------------------------------------- the whole */

static size_t write_state(const sc55mk2_t *b, uint8_t *out)
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
		case CHUNK_SRAM:    put_bytes(&w, b->sram, SC55MK2_SRAM_SIZE); break;
		case CHUNK_CPU:     state_put_h8500(&w, &b->cpu); break;
		case CHUNK_GP:      put_gp(&w, &b->gp); break;
		case CHUNK_GP_ERAM: put_u16s(&w, b->gp.eram, GP_ERAM_SIZE); break;
		case CHUNK_LCD:     put_lcd(&w, &b->lcd); break;
		case CHUNK_SUB:     put_sub(&w, &b->sub); break;
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

size_t sc55mk2_state_size(const sc55mk2_t *b)
{
	return write_state(b, NULL);
}

size_t sc55mk2_state_save(const sc55mk2_t *b, void *buffer, size_t size)
{
	const size_t need = write_state(b, NULL);
	if (size < need)
		return 0;
	return write_state(b, buffer);
}

static bool read_chunk(int which, state_reader_t *r, sc55mk2_t *b)
{
	switch (which)
	{
	case CHUNK_BOARD:   get_board(r, b); break;
	case CHUNK_SRAM:    get_bytes(r, b->sram, SC55MK2_SRAM_SIZE); break;
	case CHUNK_CPU:     state_get_h8500(r, &b->cpu); break;
	case CHUNK_GP:      get_gp(r, &b->gp); break;
	case CHUNK_GP_ERAM: get_u16s(r, b->gp.eram, GP_ERAM_SIZE); break;
	case CHUNK_LCD:     get_lcd(r, &b->lcd); break;
	case CHUNK_SUB:     get_sub(r, &b->sub); break;
	case CHUNK_MIDI:    state_get_midi(r, &b->midi); break;
	default: return false;
	}
	return r->ok && r->at == r->size;
}

bool sc55mk2_state_load(sc55mk2_t *b, const void *buffer, size_t size)
{
	state_reader_t r = { buffer, size, 0, true };
	state_header_t h;
	if (!get_header(&r, &h) || h.model != (uint32_t)b->model || h.rom_id != b->rom_id)
		return false;

	/* everything goes into a copy first, so a bad stream leaves the machine as it was */
	sc55mk2_t *t = malloc(sizeof(*t));
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
		b->sub.user = b;
		b->cpu.bus.user = b;
		h8500_set_irq(&b->cpu, H8500_IRQ0, b->gp.irq_pending);
		h8500_set_irq(&b->cpu, H8500_IRQ1, b->int_trigger != 0);
	}
	free(t);
	return ok;
}
