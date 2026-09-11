#include <stdlib.h>
#include <string.h>
#include "sc8820.h"
#include "state_chunks.h"

#define TAG STATE_TAG

enum
{
	CHUNK_BOARD, CHUNK_CPU, CHUNK_DRAM, CHUNK_XP, CHUNK_XP_ERAM, CHUNK_LSP, CHUNK_LSP_ERAM,
	CHUNK_MIDI, CHUNK_FLASH, CHUNK_UIPC, CHUNK_PANEL,
	CHUNK_COUNT
};

static const uint32_t CHUNK_TAG[CHUNK_COUNT] =
{
	TAG('B', 'R', 'D', '2'), TAG('S', 'H', '2', ' '), TAG('D', 'R', 'A', 'M'),
	TAG('X', 'P', '1', ' '), TAG('X', 'E', 'R', '1'), TAG('L', 'S', 'P', ' '), TAG('L', 'E', 'R', 'M'),
	TAG('M', 'I', 'D', 'I'), TAG('F', 'L', 'S', 'H'), TAG('U', 'I', 'P', 'C'), TAG('P', 'N', 'L', '2')
};

/* ---------------------------------------------------------------- the chunks */

static void put_board(state_writer_t *w, const sc8820_t *b)
{
	put32(w, b->cpu_overshoot);
	put_bool(w, b->mute);
	put32(w, (uint32_t)b->computer_switch);
	put64(w, b->frame);
	put64(w, b->tg_written);
	put32(w, b->midi.drops);
}

static void get_board(state_reader_t *r, sc8820_t *b)
{
	b->cpu_overshoot = get32(r);
	b->mute = get_bool(r);
	b->computer_switch = (scemu_computer_switch_t)get32(r);
	b->frame = get64(r);
	b->tg_written = get64(r);
	b->midi.drops = get32(r);
}

static void put_panel(state_writer_t *w, const sc8820_panel_t *p)
{
	put16(w, p->pe);
	put16(w, p->pa);
	put_bool(w, p->written);
	put_bytes(w, p->column, SC8820_PANEL_ROWS);
	put_bool(w, p->map_key);
	put_bool(w, p->preview_key);
	put32(w, p->leds);
}

static void get_panel(state_reader_t *r, sc8820_panel_t *p)
{
	p->pe = get16(r);
	p->pa = get16(r);
	p->written = get_bool(r);
	get_bytes(r, p->column, SC8820_PANEL_ROWS);
	p->map_key = get_bool(r);
	p->preview_key = get_bool(r);
	p->leds = get32(r);
}

/* ---------------------------------------------------------------- the whole */

static size_t write_state(const sc8820_t *b, uint8_t *out)
{
	state_writer_t w = { out, 0 };
	put_bytes(&w, STATE_MAGIC, 8);
	put32(&w, (uint32_t)SCEMU_MODEL_SC8820);
	put32(&w, 0);
	put64(&w, b->rom_id);
	put64(&w, b->frame);
	put32(&w, CHUNK_COUNT);

	for (int which = 0; which < CHUNK_COUNT; which++)
	{
		put32(&w, CHUNK_TAG[which]);
		const size_t size_at = w.size;
		put32(&w, 0);
		const size_t start = w.size;
		switch (which)
		{
		case CHUNK_BOARD:    put_board(&w, b); break;
		case CHUNK_CPU:      state_put_sh2(&w, &b->cpu); break;
		case CHUNK_DRAM:     put_bytes(&w, b->dram, SC8820_DRAM_SIZE); break;
		case CHUNK_XP:       state_put_xp(&w, &b->xp); break;
		case CHUNK_XP_ERAM:  put_i32s(&w, b->xp.eram, XP_ERAM_SIZE); break;
		case CHUNK_LSP:      state_put_lsp(&w, &b->lsp); break;
		case CHUNK_LSP_ERAM: put_i32s(&w, b->lsp.eram, LSP_ERAM_SIZE); break;
		case CHUNK_MIDI:     state_put_midi(&w, &b->midi); break;
		case CHUNK_FLASH:    state_put_flash(&w, &b->flash); break;
		case CHUNK_UIPC:     state_put_uipc(&w, &b->uipc); break;
		case CHUNK_PANEL:    put_panel(&w, &b->panel); break;
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

size_t sc8820_state_size(const sc8820_t *b)
{
	return write_state(b, NULL);
}

size_t sc8820_state_save(const sc8820_t *b, void *buffer, size_t size)
{
	const size_t need = write_state(b, NULL);
	if (size < need)
		return 0;
	return write_state(b, buffer);
}

static bool read_chunk(int which, state_reader_t *r, sc8820_t *b)
{
	switch (which)
	{
	case CHUNK_BOARD:    get_board(r, b); break;
	case CHUNK_CPU:      state_get_sh2(r, &b->cpu); break;
	case CHUNK_DRAM:     get_bytes(r, b->dram, SC8820_DRAM_SIZE); break;
	case CHUNK_XP:       state_get_xp(r, &b->xp); break;
	case CHUNK_XP_ERAM:  get_i32s(r, b->xp.eram, XP_ERAM_SIZE); break;
	case CHUNK_LSP:      state_get_lsp(r, &b->lsp); break;
	case CHUNK_LSP_ERAM: get_i32s(r, b->lsp.eram, LSP_ERAM_SIZE); break;
	case CHUNK_MIDI:     state_get_midi(r, &b->midi); break;
	case CHUNK_FLASH:    state_get_flash(r, &b->flash); break;
	case CHUNK_UIPC:     state_get_uipc(r, &b->uipc); break;
	case CHUNK_PANEL:    get_panel(r, &b->panel); break;
	default: return false;
	}
	return r->ok && r->at == r->size;
}

bool sc8820_state_load(sc8820_t *b, const void *buffer, size_t size)
{
	state_reader_t r = { buffer, size, 0, true };
	state_header_t h;
	if (!get_header(&r, &h) || h.model != (uint32_t)SCEMU_MODEL_SC8820 || h.rom_id != b->rom_id)
		return false;

	sc8820_t *t = malloc(sizeof(*t));
	if (!t)
		return false;
	memcpy(t, b, sizeof(*t));
	int32_t *xp_eram = malloc(XP_ERAM_SIZE * sizeof(int32_t));
	int32_t *lsp_eram = malloc(LSP_ERAM_SIZE * sizeof(int32_t));
	t->xp.eram = xp_eram;
	t->lsp.eram = lsp_eram;
	bool ok = xp_eram && lsp_eram;

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
	ok = ok && seen == (1u << CHUNK_COUNT) - 1;

	if (ok)
	{
		t->xp.eram = b->xp.eram;
		t->lsp.eram = b->lsp.eram;
		memcpy(b, t, sizeof(*b));
		memcpy(b->xp.eram, xp_eram, XP_ERAM_SIZE * sizeof(int32_t));
		memcpy(b->lsp.eram, lsp_eram, LSP_ERAM_SIZE * sizeof(int32_t));
		b->xp.jit = &b->jit;
		b->lsp.jit = &b->jit;
		for (int n = 0; n < b->cpu.region_count; n++)
			if (b->cpu.regions[n].data == b->flash.data)
				b->cpu.regions[n].bypass = !flash_in_array(&b->flash);
		sh2_jit_flush(&b->cpu);
	}
	free(lsp_eram);
	free(xp_eram);
	free(t);
	return ok;
}
