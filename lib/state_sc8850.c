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

static void put_uipc(state_writer_t *w, const sc8850_uipc_t *u)
{
	put16(w, u->rx_head);
	put16(w, u->rx_count);
	for (uint16_t n = 0; n < u->rx_count; n++)
		put16(w, u->rx[(u->rx_head + n) % SC8850_UIPC_RX]);
	put_bytes(w, u->tx, sizeof(u->tx));
	put8(w, u->tx_count);
	put8(w, u->boot);
	put_bool(w, u->running);
	put_bool(w, u->host);
	put_bool(w, u->online);
	for (int n = 0; n < SC8850_MIDI_PORTS; n++)
	{
		const sc8850_usb_in_t *in = &u->in[n];
		put_bytes(w, in->msg, sizeof(in->msg));
		put8(w, in->count);
		put8(w, in->need);
		put8(w, in->cin);
		put8(w, in->status);
		put_bool(w, in->sysex);
	}
	put32(w, u->announce);
	put32(w, u->poll);
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

static void get_uipc(state_reader_t *r, sc8850_uipc_t *u)
{
	memset(u, 0, sizeof(*u));
	u->rx_head = get16(r);
	u->rx_count = get16(r);
	if (u->rx_count > SC8850_UIPC_RX || u->rx_head >= SC8850_UIPC_RX)
	{
		r->ok = false;
		return;
	}
	for (uint16_t n = 0; n < u->rx_count; n++)
		u->rx[(u->rx_head + n) % SC8850_UIPC_RX] = get16(r);
	get_bytes(r, u->tx, sizeof(u->tx));
	u->tx_count = get8(r);
	u->boot = get8(r);
	u->running = get_bool(r);
	u->host = get_bool(r);
	u->online = get_bool(r);
	for (int n = 0; n < SC8850_MIDI_PORTS; n++)
	{
		sc8850_usb_in_t *in = &u->in[n];
		get_bytes(r, in->msg, sizeof(in->msg));
		in->count = get8(r);
		in->need = get8(r);
		in->cin = get8(r);
		in->status = get8(r);
		in->sysex = get_bool(r);
	}
	u->announce = get32(r);
	u->poll = get32(r);
}

static void put_cpu(state_writer_t *w, const sh2_t *c)
{
	put_u32s(w, c->r, 16);
	put32(w, c->sr);
	put32(w, c->gbr);
	put32(w, c->vbr);
	put32(w, c->mach);
	put32(w, c->macl);
	put32(w, c->pr);
	put32(w, c->pc);
	put32(w, c->delay_target);
	put_bool(w, c->delay);
	put_bool(w, c->sleeping);

	put_u16s(w, c->ipr, 8);
	put16(w, c->icr);
	put16(w, c->isr);
	put_u32s(w, c->pending, 8);
	put8(w, c->irq_line);
	put_bool(w, c->nmi_line);
	put_bool(w, c->nmi_pending);

	for (int n = 0; n < 2; n++)
	{
		const sh2_sci_t *s = &c->sci[n];
		put8(w, s->smr);
		put8(w, s->brr);
		put8(w, s->scr);
		put8(w, s->tdr);
		put8(w, s->ssr);
		put8(w, s->rdr);
		put8(w, s->tx_shift);
		put32(w, s->tx_timer);
		put_bool(w, s->tx_busy);
		put8(w, s->rx_byte);
		put_bool(w, s->rx_pending);
		put_bool(w, s->int_rxi);
		put_bool(w, s->int_txi);
		put_bool(w, s->int_tei);
		put_bool(w, s->int_eri);
	}

	for (int n = 0; n < 3; n++)
	{
		const sh2_mtu_channel_t *m = &c->mtu[n];
		put8(w, m->tcr);
		put8(w, m->tmdr);
		put8(w, m->tiorh);
		put8(w, m->tiorl);
		put8(w, m->tier);
		put8(w, m->tsr);
		put16(w, m->tcnt);
		put_u16s(w, m->tgr, 4);
		put32(w, m->prescale);
		put_bool(w, m->active);
	}
	put8(w, c->tsyr);

	put8(w, c->wtcsr);
	put8(w, c->wtcnt);
	put8(w, c->rstcsr);
	put32(w, c->wdt_prescale);
	put_bool(w, c->wdt_int);

	put_u16s(w, c->addr_, 4);
	put8(w, c->adcsr);
	put8(w, c->adcr);
	put32(w, c->adc_timer);
	put_bool(w, c->adc_int);

	for (int n = 0; n < 2; n++)
	{
		const sh2_dma_channel_t *d = &c->dma[n];
		put32(w, d->sar);
		put32(w, d->dar);
		put32(w, d->chcr);
		put32(w, d->dmatcr);
		put32(w, d->count);
		put32(w, d->timer);
		put_bool(w, d->active);
		put_bool(w, d->int_te);
	}
	put16(w, c->dmaor);

	put16(w, c->padr);
	put16(w, c->paior);
	put16(w, c->pacr1);
	put16(w, c->pacr2);
	put16(w, c->pbdr);
	put16(w, c->pbior);
	put16(w, c->pbcr1);
	put16(w, c->pbcr2);
	put16(w, c->pedr);
	put16(w, c->peior);
	put16(w, c->pecr1);
	put16(w, c->pecr2);
	put_u16s(w, c->port_out, 3);

	put16(w, c->bcr1);
	put16(w, c->bcr2);
	put16(w, c->wcr1);
	put16(w, c->wcr2);
	put16(w, c->dcr);
	put16(w, c->rtcsr);
	put16(w, c->rtcnt);
	put16(w, c->rtcor);
	put16(w, c->ccr);

	put_bytes(w, c->ram, SH2_RAM_SIZE);
	put64(w, c->cycles);
}

static void get_cpu(state_reader_t *r, sh2_t *c)
{
	get_u32s(r, c->r, 16);
	c->sr = get32(r);
	c->gbr = get32(r);
	c->vbr = get32(r);
	c->mach = get32(r);
	c->macl = get32(r);
	c->pr = get32(r);
	c->pc = get32(r);
	c->delay_target = get32(r);
	c->delay = get_bool(r);
	c->sleeping = get_bool(r);

	get_u16s(r, c->ipr, 8);
	c->icr = get16(r);
	c->isr = get16(r);
	get_u32s(r, c->pending, 8);
	c->irq_line = get8(r);
	c->nmi_line = get_bool(r);
	c->nmi_pending = get_bool(r);

	for (int n = 0; n < 2; n++)
	{
		sh2_sci_t *s = &c->sci[n];
		s->smr = get8(r);
		s->brr = get8(r);
		s->scr = get8(r);
		s->tdr = get8(r);
		s->ssr = get8(r);
		s->rdr = get8(r);
		s->tx_shift = get8(r);
		s->tx_timer = get32(r);
		s->tx_busy = get_bool(r);
		s->rx_byte = get8(r);
		s->rx_pending = get_bool(r);
		s->int_rxi = get_bool(r);
		s->int_txi = get_bool(r);
		s->int_tei = get_bool(r);
		s->int_eri = get_bool(r);
	}

	for (int n = 0; n < 3; n++)
	{
		sh2_mtu_channel_t *m = &c->mtu[n];
		m->tcr = get8(r);
		m->tmdr = get8(r);
		m->tiorh = get8(r);
		m->tiorl = get8(r);
		m->tier = get8(r);
		m->tsr = get8(r);
		m->tcnt = get16(r);
		get_u16s(r, m->tgr, 4);
		m->prescale = get32(r);
		m->active = get_bool(r);
	}
	c->tsyr = get8(r);

	c->wtcsr = get8(r);
	c->wtcnt = get8(r);
	c->rstcsr = get8(r);
	c->wdt_prescale = get32(r);
	c->wdt_int = get_bool(r);

	get_u16s(r, c->addr_, 4);
	c->adcsr = get8(r);
	c->adcr = get8(r);
	c->adc_timer = get32(r);
	c->adc_int = get_bool(r);

	for (int n = 0; n < 2; n++)
	{
		sh2_dma_channel_t *d = &c->dma[n];
		d->sar = get32(r);
		d->dar = get32(r);
		d->chcr = get32(r);
		d->dmatcr = get32(r);
		d->count = get32(r);
		d->timer = get32(r);
		d->active = get_bool(r);
		d->int_te = get_bool(r);
	}
	c->dmaor = get16(r);

	c->padr = get16(r);
	c->paior = get16(r);
	c->pacr1 = get16(r);
	c->pacr2 = get16(r);
	c->pbdr = get16(r);
	c->pbior = get16(r);
	c->pbcr1 = get16(r);
	c->pbcr2 = get16(r);
	c->pedr = get16(r);
	c->peior = get16(r);
	c->pecr1 = get16(r);
	c->pecr2 = get16(r);
	get_u16s(r, c->port_out, 3);

	c->bcr1 = get16(r);
	c->bcr2 = get16(r);
	c->wcr1 = get16(r);
	c->wcr2 = get16(r);
	c->dcr = get16(r);
	c->rtcsr = get16(r);
	c->rtcnt = get16(r);
	c->rtcor = get16(r);
	c->ccr = get16(r);

	get_bytes(r, c->ram, SH2_RAM_SIZE);
	c->cycles = get64(r);
	c->irq_ready = false;
	c->jit_pending = 0;
	c->jit_limit = 0;
	c->jit_deadline = 0;
}

static void put_flash(state_writer_t *w, const flash_t *f)
{
	put8(w, f->mode);
	put8(w, f->pending);
	put8(w, f->status);
}

static void get_flash(state_reader_t *r, flash_t *f)
{
	f->mode = get8(r);
	f->pending = get8(r);
	f->status = get8(r);
	f->erased = false;
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
		case CHUNK_CPU:        put_cpu(&w, &b->cpu); break;
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
		case CHUNK_UIPC:       put_uipc(&w, &b->uipc); break;
		case CHUNK_FLASH:
			put_flash(&w, &b->program_flash);
			put_flash(&w, &b->tone_flash);
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
	case CHUNK_CPU:        get_cpu(r, &b->cpu); break;
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
	case CHUNK_UIPC:       get_uipc(r, &b->uipc); break;
	case CHUNK_FLASH:
		get_flash(r, &b->program_flash);
		get_flash(r, &b->tone_flash);
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
