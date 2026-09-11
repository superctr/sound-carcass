#include <string.h>
#include "state_chunks.h"

/* The chunks the SH-2 boards share: the core with its on-chip RAM, a flash chip's mode, and the
 * USB controller's mailboxes. */

void state_put_sh2(state_writer_t *w, const sh2_t *c)
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

void state_get_sh2(state_reader_t *r, sh2_t *c)
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

void state_put_flash(state_writer_t *w, const flash_t *f)
{
	put8(w, f->mode);
	put8(w, f->pending);
	put8(w, f->status);
}

void state_get_flash(state_reader_t *r, flash_t *f)
{
	f->mode = get8(r);
	f->pending = get8(r);
	f->status = get8(r);
	f->erased = false;
}

void state_put_uipc(state_writer_t *w, const uipc_t *u)
{
	put16(w, u->rx_head);
	put16(w, u->rx_count);
	for (uint16_t n = 0; n < u->rx_count; n++)
		put16(w, u->rx[(u->rx_head + n) % UIPC_RX]);
	put_bytes(w, u->tx, sizeof(u->tx));
	put8(w, u->tx_count);
	put8(w, u->boot);
	put_bool(w, u->running);
	put_bool(w, u->host);
	put_bool(w, u->online);
	for (int n = 0; n < UIPC_PORTS; n++)
	{
		const uipc_in_t *in = &u->in[n];
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

void state_get_uipc(state_reader_t *r, uipc_t *u)
{
	const uipc_link_t link = u->link;
	const bool reports_switch = u->reports_switch;
	memset(u, 0, sizeof(*u));
	u->link = link;
	u->reports_switch = reports_switch;
	u->rx_head = get16(r);
	u->rx_count = get16(r);
	if (u->rx_count > UIPC_RX || u->rx_head >= UIPC_RX)
	{
		r->ok = false;
		return;
	}
	for (uint16_t n = 0; n < u->rx_count; n++)
		u->rx[(u->rx_head + n) % UIPC_RX] = get16(r);
	get_bytes(r, u->tx, sizeof(u->tx));
	u->tx_count = get8(r);
	u->boot = get8(r);
	u->running = get_bool(r);
	u->host = get_bool(r);
	u->online = get_bool(r);
	for (int n = 0; n < UIPC_PORTS; n++)
	{
		uipc_in_t *in = &u->in[n];
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
