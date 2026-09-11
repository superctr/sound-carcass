#include <stdlib.h>
#include <string.h>
#include "sc8850.h"
#include "state_chunks.h"

#define TAG STATE_TAG

enum
{
	CHUNK_BOARD, CHUNK_CPU, CHUNK_DRAM, CHUNK_XP1, CHUNK_XP1_ERAM, CHUNK_XP2, CHUNK_XP2_ERAM,
	CHUNK_LSP, CHUNK_LSP_ERAM, CHUNK_GATE_ARRAY, CHUNK_GLCD, CHUNK_MIDI, CHUNK_FLASH, CHUNK_UIPC,
	CHUNK_COUNT
};

static const uint32_t CHUNK_TAG[CHUNK_COUNT] =
{
	TAG('B', 'R', 'D', '2'), TAG('S', 'H', '2', ' '), TAG('D', 'R', 'A', 'M'),
	TAG('X', 'P', '1', ' '), TAG('X', 'E', 'R', '1'), TAG('X', 'P', '2', ' '), TAG('X', 'E', 'R', '2'),
	TAG('L', 'S', 'P', ' '), TAG('L', 'E', 'R', 'M'), TAG('G', 'A', '2', ' '), TAG('G', 'L', 'C', 'D'),
	TAG('M', 'I', 'D', 'I'), TAG('F', 'L', 'S', 'H'), TAG('U', 'I', 'P', 'C')
};

/* ---------------------------------------------------------------- the chunks */

static void put_board(state_writer_t *w, const sc8850_t *b)
{
	put32(w, b->cpu_overshoot);
	put_bool(w, b->mute);
	put32(w, (uint32_t)b->computer_switch);
	put64(w, b->frame);
	put64(w, b->tg_written);
	put32(w, b->midi.drops);
}



static void get_board(state_reader_t *r, sc8850_t *b)
{
	b->cpu_overshoot = get32(r);
	b->mute = get_bool(r);
	b->computer_switch = (scemu_computer_switch_t)get32(r);
	b->frame = get64(r);
	b->tg_written = get64(r);
	b->midi.drops = get32(r);
}




static void put_gate_array(state_writer_t *w, const sc8850_ga_t *g)
{
	put_bytes(w, g->regs, 0x100);
	put16(w, g->requests);
	put8(w, g->source);
	put8(w, g->event);
	put_bool(w, g->pending);
	put_i32(w, g->encoder);
	put_bytes(w, g->keys, 4);
	put_bytes(w, g->key_state, 4);
	put8(w, g->scan_position);
	put_bool(w, g->scan_encoder);
	put32(w, g->scan_frames);
	put32(w, g->tick_frames);
	put32(w, g->sequencer_frames);
	put32(w, g->leds);
}

static void get_gate_array(state_reader_t *r, sc8850_ga_t *g)
{
	get_bytes(r, g->regs, 0x100);
	g->requests = get16(r);
	g->source = get8(r);
	g->event = get8(r);
	g->pending = get_bool(r);
	g->encoder = get_i32(r);
	get_bytes(r, g->keys, 4);
	get_bytes(r, g->key_state, 4);
	g->scan_position = get8(r);
	g->scan_encoder = get_bool(r);
	g->scan_frames = get32(r);
	g->tick_frames = get32(r);
	g->sequencer_frames = get32(r);
	g->leds = get32(r);
}

static void put_glcd(state_writer_t *w, const glcd_t *g)
{
	put_bytes(w, g->vram, sizeof(g->vram));
	put8(w, g->command);
	put8(w, g->param);
	put_bool(w, g->display);
	put_bool(w, g->sleep);
	put8(w, g->m0);
	put8(w, g->m1);
	put8(w, g->m2);
	put8(w, g->ws);
	put8(w, g->iv);
	put8(w, g->wf);
	put8(w, g->fx);
	put8(w, g->fy);
	put16(w, g->cr);
	put16(w, g->tcr);
	put16(w, g->lf);
	put16(w, g->ap);
	put16(w, g->sad1);
	put16(w, g->sad2);
	put16(w, g->sad3);
	put16(w, g->sad4);
	put16(w, g->sl1);
	put16(w, g->sl2);
	put16(w, g->sag);
	put8(w, g->hdotscr);
	put8(w, g->mx);
	put8(w, g->dm1);
	put8(w, g->dm3);
	put8(w, g->ov);
	put8(w, g->fc);
	put8(w, g->fp);
	put8(w, g->crx);
	put8(w, g->cry);
	put8(w, g->cm);
	put16(w, g->csr);
	put8(w, g->csrdir);
	put32(w, g->flash_count);
	put32(w, g->flash_period);
	put32(w, g->flash_phase);
}

static void get_glcd(state_reader_t *r, glcd_t *g)
{
	get_bytes(r, g->vram, sizeof(g->vram));
	g->command = get8(r);
	g->param = get8(r);
	g->display = get_bool(r);
	g->sleep = get_bool(r);
	g->m0 = get8(r);
	g->m1 = get8(r);
	g->m2 = get8(r);
	g->ws = get8(r);
	g->iv = get8(r);
	g->wf = get8(r);
	g->fx = get8(r);
	g->fy = get8(r);
	g->cr = get16(r);
	g->tcr = get16(r);
	g->lf = get16(r);
	g->ap = get16(r);
	g->sad1 = get16(r);
	g->sad2 = get16(r);
	g->sad3 = get16(r);
	g->sad4 = get16(r);
	g->sl1 = get16(r);
	g->sl2 = get16(r);
	g->sag = get16(r);
	g->hdotscr = get8(r);
	g->mx = get8(r);
	g->dm1 = get8(r);
	g->dm3 = get8(r);
	g->ov = get8(r);
	g->fc = get8(r);
	g->fp = get8(r);
	g->crx = get8(r);
	g->cry = get8(r);
	g->cm = get8(r);
	g->csr = get16(r);
	g->csrdir = get8(r);
	g->flash_count = get32(r);
	g->flash_period = get32(r);
	g->flash_phase = get32(r);
	g->dirty = true;
	memset(&g->out, 0, sizeof(g->out));
	glcd_out(g);
	g->out.changed = true;
}

/* ---------------------------------------------------------------- the whole */

static size_t write_state(const sc8850_t *b, uint8_t *out)
{
	state_writer_t w = { out, 0 };
	put_bytes(&w, STATE_MAGIC, 8);
	put32(&w, (uint32_t)SCEMU_MODEL_SC8850);
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
		case CHUNK_BOARD:      put_board(&w, b); break;
		case CHUNK_CPU:        state_put_sh2(&w, &b->cpu); break;
		case CHUNK_DRAM:       put_bytes(&w, b->dram, SC8850_DRAM_SIZE); break;
		case CHUNK_XP1:        state_put_xp(&w, &b->master); break;
		case CHUNK_XP1_ERAM:   put_i32s(&w, b->master.eram, XP_ERAM_SIZE); break;
		case CHUNK_XP2:        state_put_xp(&w, &b->slave); break;
		case CHUNK_XP2_ERAM:   put_i32s(&w, b->slave.eram, XP_ERAM_SIZE); break;
		case CHUNK_LSP:        state_put_lsp(&w, &b->lsp); break;
		case CHUNK_LSP_ERAM:   put_i32s(&w, b->lsp.eram, LSP_ERAM_SIZE); break;
		case CHUNK_GATE_ARRAY: put_gate_array(&w, &b->ga); break;
		case CHUNK_GLCD:       put_glcd(&w, &b->glcd); break;
		case CHUNK_MIDI:       state_put_midi(&w, &b->midi); break;
		case CHUNK_UIPC:       state_put_uipc(&w, &b->uipc); break;
		case CHUNK_FLASH:
			state_put_flash(&w, &b->program_flash);
			state_put_flash(&w, &b->tone_flash);
			put_bytes(&w, b->program_rom + SC8850_NVRAM_BASE, SC8850_NVRAM_SIZE);
			break;
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

size_t sc8850_state_size(const sc8850_t *b)
{
	return write_state(b, NULL);
}

size_t sc8850_state_save(const sc8850_t *b, void *buffer, size_t size)
{
	const size_t need = write_state(b, NULL);
	if (size < need)
		return 0;
	return write_state(b, buffer);
}

static bool read_chunk(int which, state_reader_t *r, sc8850_t *b, uint8_t *flash_blocks)
{
	switch (which)
	{
	case CHUNK_BOARD:      get_board(r, b); break;
	case CHUNK_CPU:        state_get_sh2(r, &b->cpu); break;
	case CHUNK_DRAM:       get_bytes(r, b->dram, SC8850_DRAM_SIZE); break;
	case CHUNK_XP1:        state_get_xp(r, &b->master); break;
	case CHUNK_XP1_ERAM:   get_i32s(r, b->master.eram, XP_ERAM_SIZE); break;
	case CHUNK_XP2:        state_get_xp(r, &b->slave); break;
	case CHUNK_XP2_ERAM:   get_i32s(r, b->slave.eram, XP_ERAM_SIZE); break;
	case CHUNK_LSP:        state_get_lsp(r, &b->lsp); break;
	case CHUNK_LSP_ERAM:   get_i32s(r, b->lsp.eram, LSP_ERAM_SIZE); break;
	case CHUNK_GATE_ARRAY: get_gate_array(r, &b->ga); break;
	case CHUNK_GLCD:       get_glcd(r, &b->glcd); break;
	case CHUNK_MIDI:       state_get_midi(r, &b->midi); break;
	case CHUNK_UIPC:       state_get_uipc(r, &b->uipc); break;
	case CHUNK_FLASH:
		state_get_flash(r, &b->program_flash);
		state_get_flash(r, &b->tone_flash);
		get_bytes(r, flash_blocks, SC8850_NVRAM_SIZE);
		break;
	default: return false;
	}
	return r->ok && r->at == r->size;
}

bool sc8850_state_load(sc8850_t *b, const void *buffer, size_t size)
{
	state_reader_t r = { buffer, size, 0, true };
	state_header_t h;
	if (!get_header(&r, &h) || h.model != (uint32_t)SCEMU_MODEL_SC8850 || h.rom_id != b->rom_id)
		return false;

	sc8850_t *t = malloc(sizeof(*t));
	if (!t)
		return false;
	memcpy(t, b, sizeof(*t));
	int32_t *master_eram = malloc(XP_ERAM_SIZE * sizeof(int32_t));
	int32_t *slave_eram = malloc(XP_ERAM_SIZE * sizeof(int32_t));
	int32_t *lsp_eram = malloc(LSP_ERAM_SIZE * sizeof(int32_t));
	uint8_t *flash_blocks = malloc(SC8850_NVRAM_SIZE);
	t->master.eram = master_eram;
	t->slave.eram = slave_eram;
	t->lsp.eram = lsp_eram;
	bool ok = master_eram && slave_eram && lsp_eram && flash_blocks;

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
			ok = read_chunk(which, &body, t, flash_blocks);
			seen |= 1u << which;
		}
		r.at += len;
	}
	ok = ok && seen == (1u << CHUNK_COUNT) - 1;

	if (ok)
	{
		t->master.eram = b->master.eram;
		t->slave.eram = b->slave.eram;
		t->lsp.eram = b->lsp.eram;
		memcpy(b, t, sizeof(*b));
		memcpy(b->master.eram, master_eram, XP_ERAM_SIZE * sizeof(int32_t));
		memcpy(b->slave.eram, slave_eram, XP_ERAM_SIZE * sizeof(int32_t));
		memcpy(b->lsp.eram, lsp_eram, LSP_ERAM_SIZE * sizeof(int32_t));
		b->master.jit = &b->jit;
		b->slave.jit = &b->jit;
		b->lsp.jit = &b->jit;
		memcpy(b->program_rom + SC8850_NVRAM_BASE, flash_blocks, SC8850_NVRAM_SIZE);
		for (int n = 0; n < b->cpu.region_count; n++)
		{
			if (b->cpu.regions[n].data == b->program_flash.data)
				b->cpu.regions[n].bypass = !flash_in_array(&b->program_flash);
			if (b->cpu.regions[n].data == b->tone_flash.data)
				b->cpu.regions[n].bypass = !flash_in_array(&b->tone_flash);
		}
		sh2_jit_flush(&b->cpu);
	}
	free(flash_blocks);
	free(lsp_eram);
	free(slave_eram);
	free(master_eram);
	free(t);
	return ok;
}
