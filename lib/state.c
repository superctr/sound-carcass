#include <stdlib.h>
#include <string.h>
#include "sc88.h"

/* The machine at a frame boundary as a byte stream: a header naming the model and the ROM set, then
   tagged chunks, every field written little-endian at a fixed width, so a state does not depend on the
   host's layout, endianness or build.  Unknown chunks are skipped, missing ones refused. */

#define STATE_MAGIC "SCEMUST1"
#define STATE_HEADER_SIZE 36

#define TAG(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

enum
{
	CHUNK_BOARD, CHUNK_SRAM, CHUNK_CPU, CHUNK_XP, CHUNK_XP_ERAM, CHUNK_LSP, CHUNK_LSP_ERAM,
	CHUNK_GATE_ARRAY, CHUNK_LCD, CHUNK_SUB, CHUNK_MIDI, CHUNK_COUNT
};

static const uint32_t CHUNK_TAG[CHUNK_COUNT] =
{
	TAG('B', 'R', 'D', ' '), TAG('S', 'R', 'A', 'M'), TAG('C', 'P', 'U', ' '), TAG('X', 'P', ' ', ' '),
	TAG('X', 'E', 'R', 'M'), TAG('L', 'S', 'P', ' '), TAG('L', 'E', 'R', 'M'), TAG('G', 'A', ' ', ' '),
	TAG('L', 'C', 'D', ' '), TAG('S', 'U', 'B', ' '), TAG('M', 'I', 'D', 'I')
};

/* ---------------------------------------------------------------- the stream */

typedef struct writer
{
	uint8_t *p;
	size_t size;
} writer_t;

static void put8(writer_t *w, uint8_t v)
{
	if (w->p)
		w->p[w->size] = v;
	w->size++;
}

static void put16(writer_t *w, uint16_t v) { put8(w, (uint8_t)v); put8(w, (uint8_t)(v >> 8)); }
static void put32(writer_t *w, uint32_t v) { put16(w, (uint16_t)v); put16(w, (uint16_t)(v >> 16)); }
static void put64(writer_t *w, uint64_t v) { put32(w, (uint32_t)v); put32(w, (uint32_t)(v >> 32)); }
static void put_i32(writer_t *w, int32_t v) { put32(w, (uint32_t)v); }
static void put_bool(writer_t *w, bool v) { put8(w, v ? 1 : 0); }

static void put_bytes(writer_t *w, const void *data, size_t n)
{
	if (w->p)
		memcpy(w->p + w->size, data, n);
	w->size += n;
}

static void put_u16s(writer_t *w, const uint16_t *v, size_t n) { for (size_t i = 0; i < n; i++) put16(w, v[i]); }
static void put_u32s(writer_t *w, const uint32_t *v, size_t n) { for (size_t i = 0; i < n; i++) put32(w, v[i]); }
static void put_i32s(writer_t *w, const int32_t *v, size_t n) { for (size_t i = 0; i < n; i++) put_i32(w, v[i]); }

typedef struct reader
{
	const uint8_t *p;
	size_t size, at;
	bool ok;
} reader_t;

static uint8_t get8(reader_t *r)
{
	if (r->at >= r->size)
	{
		r->ok = false;
		return 0;
	}
	return r->p[r->at++];
}

static uint16_t get16(reader_t *r) { uint16_t lo = get8(r); return (uint16_t)(lo | (get8(r) << 8)); }
static uint32_t get32(reader_t *r) { uint32_t lo = get16(r); return lo | ((uint32_t)get16(r) << 16); }
static uint64_t get64(reader_t *r) { uint64_t lo = get32(r); return lo | ((uint64_t)get32(r) << 32); }
static int32_t get_i32(reader_t *r) { return (int32_t)get32(r); }
static bool get_bool(reader_t *r) { return get8(r) != 0; }

static void get_bytes(reader_t *r, void *data, size_t n)
{
	if (r->at + n > r->size)
	{
		r->ok = false;
		memset(data, 0, n);
		r->at = r->size;
		return;
	}
	memcpy(data, r->p + r->at, n);
	r->at += n;
}

static void get_u16s(reader_t *r, uint16_t *v, size_t n) { for (size_t i = 0; i < n; i++) v[i] = get16(r); }
static void get_u32s(reader_t *r, uint32_t *v, size_t n) { for (size_t i = 0; i < n; i++) v[i] = get32(r); }
static void get_i32s(reader_t *r, int32_t *v, size_t n) { for (size_t i = 0; i < n; i++) v[i] = get_i32(r); }

/* ---------------------------------------------------------------- the chunks */

static void put_board(writer_t *w, const sc88_t *b)
{
	put32(w, b->cpu_half_cycles);
	put_bool(w, b->xp_int);
	put_bool(w, b->mute);
	put_bool(w, b->lsp_mute);
	put32(w, (uint32_t)b->computer_switch);
	put64(w, b->frame);
	put32(w, b->midi_drops);
}

static void get_board(reader_t *r, sc88_t *b)
{
	b->cpu_half_cycles = get32(r);
	b->xp_int = get_bool(r);
	b->mute = get_bool(r);
	b->lsp_mute = get_bool(r);
	b->computer_switch = (scemu_computer_switch_t)get32(r);
	b->frame = get64(r);
	b->midi_drops = get32(r);
}

static void put_cpu(writer_t *w, const h8500_t *c)
{
	put16(w, c->pc);
	put16(w, c->sr);
	put8(w, c->cp);
	put8(w, c->dp);
	put8(w, c->ep);
	put8(w, c->tp);
	put8(w, c->br);
	put_u16s(w, c->r, 8);
	put_bool(w, c->sleeping);
	put8(w, c->irq_lines);
	put8(w, c->irq_req);
	put_u32s(w, c->irq_pend, 3);
	put_bool(w, c->no_irq);
	put_bytes(w, c->io, H8500_IO_SIZE);
	put_u16s(w, c->frt_count, 2);
	put_u32s(w, c->frt_prescale, 2);
	put8(w, c->tmr_count);
	put32(w, c->tmr_prescale);
	put32(w, c->wdt_prescale);
	put32(w, c->adc_busy);
	put8(w, c->adc_channel);
	for (int n = 0; n < 2; n++)
	{
		put8(w, c->sci[n].rx_byte);
		put_bool(w, c->sci[n].rx_pending);
		put32(w, c->sci[n].tx_timer);
		put8(w, c->sci[n].tx_shift);
		put8(w, c->sci[n].ssr_read);
		put_bool(w, c->sci[n].tx_busy);
	}
	put_u16s(w, c->port_out, 9);
	put64(w, c->cycles);
}

static void get_cpu(reader_t *r, h8500_t *c)
{
	c->pc = get16(r);
	c->sr = get16(r);
	c->cp = get8(r);
	c->dp = get8(r);
	c->ep = get8(r);
	c->tp = get8(r);
	c->br = get8(r);
	get_u16s(r, c->r, 8);
	c->sleeping = get_bool(r);
	c->irq_lines = get8(r);
	c->irq_req = get8(r);
	get_u32s(r, c->irq_pend, 3);
	c->no_irq = get_bool(r);
	get_bytes(r, c->io, H8500_IO_SIZE);
	get_u16s(r, c->frt_count, 2);
	get_u32s(r, c->frt_prescale, 2);
	c->tmr_count = get8(r);
	c->tmr_prescale = get32(r);
	c->wdt_prescale = get32(r);
	c->adc_busy = get32(r);
	c->adc_channel = get8(r);
	for (int n = 0; n < 2; n++)
	{
		c->sci[n].rx_byte = get8(r);
		c->sci[n].rx_pending = get_bool(r);
		c->sci[n].tx_timer = get32(r);
		c->sci[n].tx_shift = get8(r);
		c->sci[n].ssr_read = get8(r);
		c->sci[n].tx_busy = get_bool(r);
	}
	get_u16s(r, c->port_out, 9);
	c->cycles = get64(r);
	c->irq_ready = false;
	c->jit_pending = 0;
	c->jit_limit = 0;
	c->jit_deadline = 0;
}

static void put_xp(writer_t *w, const xp_t *x)
{
	put_u16s(w, x->regs, XP_REGS);
	for (int n = 0; n < XP_VOICES; n++)
	{
		put8(w, x->voices[n].phase);
		put8(w, x->voices[n].format);
		put8(w, x->voices[n].fade_entry);
		put32(w, x->voices[n].start);
	}
	put_bytes(w, x->still, XP_VOICES);
	put64(w, x->run_mask);
	put64(w, x->run_pending);
	put32(w, x->read_latch);
	put16(w, x->write_latch);
	put32(w, x->frame_counter);
	put16(w, x->irq_event);
	put_bool(w, x->irq_active);
	put_bool(w, x->irq_frame_used);
	put_bool(w, x->int_state);
	const xp_dsp_state_t *d = &x->dsp;
	put_i32(w, d->acc);
	put_i32(w, d->product);
	put_i32(w, d->r);
	put_i32(w, d->input);
	put_i32(w, d->now);
	put_i32(w, d->latch);
	put_i32(w, d->gain);
	put_i32s(w, d->pend, 2);
	put8(w, d->now_valid);
	put16(w, d->cursor);
	put_i32(w, d->port_a_in);
	put_i32(w, d->port_b_in);
	put_i32s(w, d->port_a_return, XP_STROBES);
	put_i32s(w, d->port_b_pair, 2);
	put_i32(w, d->cycle);
	put_i32(w, d->land_valid);
	put_i32(w, d->strobe_a);
	put_i32(w, d->strobe_bcd);
	put_i32(w, d->position);
	put_i32s(w, x->iram, XP_IRAM_CELLS);
	put_bytes(w, x->iram_ramping, 64);
	put8(w, x->parity);
	for (int port = 0; port < XP_OUTPUT_PORTS; port++)
		put_i32s(w, x->port_word[port], 2);
	put_i32s(w, x->port_a_out, XP_STROBES);
}

static void get_xp(reader_t *r, xp_t *x)
{
	get_u16s(r, x->regs, XP_REGS);
	for (int n = 0; n < XP_VOICES; n++)
	{
		x->voices[n].phase = get8(r);
		x->voices[n].format = get8(r);
		x->voices[n].fade_entry = get8(r);
		x->voices[n].start = get32(r);
	}
	get_bytes(r, x->still, XP_VOICES);
	x->run_mask = get64(r);
	x->run_pending = get64(r);
	x->read_latch = get32(r);
	x->write_latch = get16(r);
	x->frame_counter = get32(r);
	x->irq_event = get16(r);
	x->irq_active = get_bool(r);
	x->irq_frame_used = get_bool(r);
	x->int_state = get_bool(r);
	xp_dsp_state_t *d = &x->dsp;
	d->acc = get_i32(r);
	d->product = get_i32(r);
	d->r = get_i32(r);
	d->input = get_i32(r);
	d->now = get_i32(r);
	d->latch = get_i32(r);
	d->gain = get_i32(r);
	get_i32s(r, d->pend, 2);
	d->now_valid = get8(r);
	d->cursor = get16(r);
	d->port_a_in = get_i32(r);
	d->port_b_in = get_i32(r);
	get_i32s(r, d->port_a_return, XP_STROBES);
	get_i32s(r, d->port_b_pair, 2);
	d->cycle = get_i32(r);
	d->land_valid = get_i32(r);
	d->strobe_a = get_i32(r);
	d->strobe_bcd = get_i32(r);
	d->position = get_i32(r);
	get_i32s(r, x->iram, XP_IRAM_CELLS);
	get_bytes(r, x->iram_ramping, 64);
	x->parity = get8(r);
	for (int port = 0; port < XP_OUTPUT_PORTS; port++)
		get_i32s(r, x->port_word[port], 2);
	get_i32s(r, x->port_a_out, XP_STROBES);
	x->named_words = 0;
	x->sends_dirty = true;
	x->program_dirty = true;
	memset(x->live, 0, sizeof(x->live));
}

static void put_lsp(writer_t *w, const lsp_t *l)
{
	put_u32s(w, l->program, LSP_PROGRAM_SIZE);
	put_i32s(w, l->iram, LSP_IRAM_SIZE);
	put_bytes(w, l->eram_cmd, 16);
	const lsp_state_t *s = &l->state;
	put_i32s(w, s->acc, 2);
	for (int n = 0; n < 2; n++)
		put_i32s(w, s->hist[n], 3);
	put_i32(w, s->eram_read);
	put_i32s(w, s->multiplier, 2);
	put_i32(w, s->eram_latch);
	put_u16s(w, s->eram_base, 2);
	put16(w, s->tap);
	put16(w, s->eram_pos);
	put16(w, s->slot);
	put_bytes(w, s->eram_tap2, 2);
	put8(w, s->prev_offset);
	put8(w, s->buffer_pos);
	put16(w, l->configuration);
	put_bool(w, l->running);
	put_bytes(w, l->patched, LSP_PROGRAM_SIZE);
	put_i32s(w, l->serial_in, 2);
	put_i32s(w, l->serial_out, 2);
	put_i32(w, l->audio_out);
	put32(w, l->host_data);
	put32(w, l->host_read);
	put16(w, l->host_address);
}

static void get_lsp(reader_t *r, lsp_t *l)
{
	get_u32s(r, l->program, LSP_PROGRAM_SIZE);
	memset(l->iram_window, 0, sizeof(l->iram_window));
	get_i32s(r, l->iram, LSP_IRAM_SIZE);
	memcpy(l->iram_mirror, l->iram, sizeof(l->iram_mirror));
	get_bytes(r, l->eram_cmd, 16);
	lsp_state_t *s = &l->state;
	get_i32s(r, s->acc, 2);
	for (int n = 0; n < 2; n++)
		get_i32s(r, s->hist[n], 3);
	s->eram_read = get_i32(r);
	get_i32s(r, s->multiplier, 2);
	s->eram_latch = get_i32(r);
	get_u16s(r, s->eram_base, 2);
	s->tap = get16(r);
	s->eram_pos = get16(r);
	s->slot = get16(r);
	get_bytes(r, s->eram_tap2, 2);
	s->prev_offset = get8(r);
	s->buffer_pos = get8(r);
	l->configuration = get16(r);
	l->running = get_bool(r);
	get_bytes(r, l->patched, LSP_PROGRAM_SIZE);
	get_i32s(r, l->serial_in, 2);
	get_i32s(r, l->serial_out, 2);
	l->audio_out = get_i32(r);
	l->host_data = get32(r);
	l->host_read = get32(r);
	l->host_address = get16(r);
	l->dirty = true;
}

static void put_gate_array(writer_t *w, const gate_array_t *g)
{
	put_bytes(w, g->regs, 0x100);
	put8(w, g->int_pending);
	put8(w, g->int_mask);
	put16(w, g->leds);
	put_bytes(w, g->lcd_fifo, 13);
	put8(w, g->lcd_fifo_count);
	put_bool(w, g->lcd_command_pending);
	put32(w, g->lcd_busy_frames);
}

static void get_gate_array(reader_t *r, gate_array_t *g)
{
	get_bytes(r, g->regs, 0x100);
	g->int_pending = get8(r);
	g->int_mask = get8(r);
	g->leds = get16(r);
	get_bytes(r, g->lcd_fifo, 13);
	g->lcd_fifo_count = get8(r);
	g->lcd_command_pending = get_bool(r);
	g->lcd_busy_frames = get32(r);
}

static void put_lcd(writer_t *w, const lcd_t *l)
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

static void get_lcd(reader_t *r, lcd_t *l)
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

static void put_sub(writer_t *w, const sub_hle_t *s)
{
	put_bytes(w, s->dpram, sizeof(s->dpram));
	put_bytes(w, s->ipcm, 4);
	put_bytes(w, s->ipcer, 4);
	put_bytes(w, s->flags, sizeof(s->flags));
	put8(w, s->sem);
	put8(w, s->spcon);
	put8(w, s->pa);
	put8(w, s->pa_dir);
	put8(w, s->pb);
	put8(w, s->pb_dir);
	put_bool(w, s->int_state);
	put_bool(w, s->in_reset);
	for (int n = 0; n < SUB_SOURCES; n++)
	{
		put8(w, s->src[n].status);
		put_bytes(w, s->src[n].data, 2);
		put8(w, s->src[n].count);
		put_bytes(w, s->src[n].sysex, SUB_SYSEX_MAX);
		put16(w, s->src[n].sysex_size);
		put_bool(w, s->src[n].in_sysex);
	}
	for (int n = 0; n < SUB_QUEUE_SIZE; n++)
	{
		const sub_message_t *m = &s->queue[n];
		put8(w, m->code);
		put8(w, m->flags);
		put8(w, m->d1);
		put8(w, m->d2);
		put8(w, m->block_size);
		put_bytes(w, m->block, SUB_BLOCK_SIZE);
	}
	put8(w, s->queue_head);
	put8(w, s->queue_count);
	put_bool(w, s->busy);
	put32(w, s->deliver_frames);
	put32(w, s->queue_drops);
	put32(w, s->sysex_drops);
	put8(w, s->tx_rd);
	put8(w, s->tx_left);
	put8(w, s->tx_end);
	put_bool(w, s->tx_running);
	put32(w, s->tx_frames);
	put_bytes(w, s->keys, 4);
}

static void get_sub(reader_t *r, sub_hle_t *s)
{
	get_bytes(r, s->dpram, sizeof(s->dpram));
	get_bytes(r, s->ipcm, 4);
	get_bytes(r, s->ipcer, 4);
	get_bytes(r, s->flags, sizeof(s->flags));
	s->sem = get8(r);
	s->spcon = get8(r);
	s->pa = get8(r);
	s->pa_dir = get8(r);
	s->pb = get8(r);
	s->pb_dir = get8(r);
	s->int_state = get_bool(r);
	s->in_reset = get_bool(r);
	for (int n = 0; n < SUB_SOURCES; n++)
	{
		s->src[n].status = get8(r);
		get_bytes(r, s->src[n].data, 2);
		s->src[n].count = get8(r);
		get_bytes(r, s->src[n].sysex, SUB_SYSEX_MAX);
		s->src[n].sysex_size = get16(r);
		s->src[n].in_sysex = get_bool(r);
	}
	for (int n = 0; n < SUB_QUEUE_SIZE; n++)
	{
		sub_message_t *m = &s->queue[n];
		m->code = get8(r);
		m->flags = get8(r);
		m->d1 = get8(r);
		m->d2 = get8(r);
		m->block_size = get8(r);
		get_bytes(r, m->block, SUB_BLOCK_SIZE);
	}
	s->queue_head = get8(r);
	s->queue_count = get8(r);
	s->busy = get_bool(r);
	s->deliver_frames = get32(r);
	s->queue_drops = get32(r);
	s->sysex_drops = get32(r);
	s->tx_rd = get8(r);
	s->tx_left = get8(r);
	s->tx_end = get8(r);
	s->tx_running = get_bool(r);
	s->tx_frames = get32(r);
	get_bytes(r, s->keys, 4);
}

/* the queued MIDI bytes not yet delivered, from the head */
static void put_midi(writer_t *w, const sc88_t *b)
{
	for (int port = 0; port < SC88_MIDI_PORTS; port++)
	{
		put32(w, b->midi_credit[port]);
		put32(w, b->midi_count[port]);
		for (uint32_t n = 0; n < b->midi_count[port]; n++)
		{
			const sc88_midi_event_t *e = &b->midi_queue[port][(b->midi_head[port] + n) % SC88_MIDI_QUEUE_SIZE];
			put32(w, e->frame);
			put8(w, e->byte);
		}
	}
}

static void get_midi(reader_t *r, sc88_t *b)
{
	for (int port = 0; port < SC88_MIDI_PORTS; port++)
	{
		b->midi_credit[port] = get32(r);
		const uint32_t count = get32(r);
		if (count > SC88_MIDI_QUEUE_SIZE)
		{
			r->ok = false;
			return;
		}
		b->midi_head[port] = 0;
		b->midi_count[port] = count;
		for (uint32_t n = 0; n < count; n++)
		{
			b->midi_queue[port][n].frame = get32(r);
			b->midi_queue[port][n].port = (uint8_t)port;
			b->midi_queue[port][n].byte = get8(r);
		}
	}
}

/* ---------------------------------------------------------------- the whole */

static void chunk(writer_t *w, int which, writer_t *body)
{
	put32(w, CHUNK_TAG[which]);
	put32(w, (uint32_t)body->size);
	put_bytes(w, body->p, body->size);
}

static size_t write_state(const sc88_t *b, uint8_t *out)
{
	writer_t w = { out, 0 };
	put_bytes(&w, STATE_MAGIC, 8);
	put32(&w, (uint32_t)b->model);
	put32(&w, b->has_lsp ? 1 : 0);
	put64(&w, b->rom_id);
	put64(&w, b->frame);
	put32(&w, b->has_lsp ? CHUNK_COUNT : CHUNK_COUNT - 2);

	for (int which = 0; which < CHUNK_COUNT; which++)
	{
		if (!b->has_lsp && (which == CHUNK_LSP || which == CHUNK_LSP_ERAM))
			continue;
		put32(&w, CHUNK_TAG[which]);
		const size_t size_at = w.size;
		put32(&w, 0);
		const size_t start = w.size;
		switch (which)
		{
		case CHUNK_BOARD:      put_board(&w, b); break;
		case CHUNK_SRAM:       put_bytes(&w, b->sram, SC88_SRAM_SIZE); break;
		case CHUNK_CPU:        put_cpu(&w, &b->cpu); break;
		case CHUNK_XP:         put_xp(&w, &b->xp); break;
		case CHUNK_XP_ERAM:    put_i32s(&w, b->xp.eram, XP_ERAM_SIZE); break;
		case CHUNK_LSP:        put_lsp(&w, &b->lsp); break;
		case CHUNK_LSP_ERAM:   put_i32s(&w, b->lsp.eram, LSP_ERAM_SIZE); break;
		case CHUNK_GATE_ARRAY: put_gate_array(&w, &b->ga); break;
		case CHUNK_LCD:        put_lcd(&w, &b->lcd); break;
		case CHUNK_SUB:        put_sub(&w, &b->sub); break;
		case CHUNK_MIDI:       put_midi(&w, b); break;
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

size_t sc88_state_size(const sc88_t *b)
{
	return write_state(b, NULL);
}

size_t sc88_state_save(const sc88_t *b, void *buffer, size_t size)
{
	const size_t need = write_state(b, NULL);
	if (size < need)
		return 0;
	return write_state(b, buffer);
}

typedef struct header
{
	uint32_t model, flags, chunk_count;
	uint64_t rom_id, frame;
} header_t;

static bool read_header(reader_t *r, header_t *h)
{
	char magic[8];
	get_bytes(r, magic, 8);
	h->model = get32(r);
	h->flags = get32(r);
	h->rom_id = get64(r);
	h->frame = get64(r);
	h->chunk_count = get32(r);
	return r->ok && memcmp(magic, STATE_MAGIC, 8) == 0;
}

bool sc88_state_info(const void *buffer, size_t size, scemu_model_t *model, uint64_t *rom_id, uint64_t *frame)
{
	reader_t r = { buffer, size, 0, true };
	header_t h;
	if (!read_header(&r, &h) || h.model >= SCEMU_MODEL_COUNT)
		return false;
	if (model)
		*model = (scemu_model_t)h.model;
	if (rom_id)
		*rom_id = h.rom_id;
	if (frame)
		*frame = h.frame;
	return true;
}

static bool read_chunk(int which, reader_t *r, sc88_t *b)
{
	switch (which)
	{
	case CHUNK_BOARD:      get_board(r, b); break;
	case CHUNK_SRAM:       get_bytes(r, b->sram, SC88_SRAM_SIZE); break;
	case CHUNK_CPU:        get_cpu(r, &b->cpu); break;
	case CHUNK_XP:         get_xp(r, &b->xp); break;
	case CHUNK_XP_ERAM:    get_i32s(r, b->xp.eram, XP_ERAM_SIZE); break;
	case CHUNK_LSP:        get_lsp(r, &b->lsp); break;
	case CHUNK_LSP_ERAM:   get_i32s(r, b->lsp.eram, LSP_ERAM_SIZE); break;
	case CHUNK_GATE_ARRAY: get_gate_array(r, &b->ga); break;
	case CHUNK_LCD:        get_lcd(r, &b->lcd); break;
	case CHUNK_SUB:        get_sub(r, &b->sub); break;
	case CHUNK_MIDI:       get_midi(r, b); break;
	default: return false;
	}
	return r->ok && r->at == r->size;
}

bool sc88_state_load(sc88_t *b, const void *buffer, size_t size)
{
	reader_t r = { buffer, size, 0, true };
	header_t h;
	if (!read_header(&r, &h) || h.model != (uint32_t)b->model || h.rom_id != b->rom_id
	    || (h.flags & 1) != (b->has_lsp ? 1u : 0u))
		return false;

	/* everything but the ERAMs goes into a copy first, so a bad stream leaves the machine as it was */
	sc88_t *t = malloc(sizeof(*t));
	int32_t *xp_eram = NULL, *lsp_eram = NULL;
	if (!t)
		return false;
	memcpy(t, b, sizeof(*t));
	xp_eram = malloc(XP_ERAM_SIZE * sizeof(int32_t));
	lsp_eram = b->has_lsp ? malloc(LSP_ERAM_SIZE * sizeof(int32_t)) : NULL;
	t->xp.eram = xp_eram;
	t->lsp.eram = lsp_eram;
	bool ok = xp_eram && (!b->has_lsp || lsp_eram);

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
			reader_t body = { r.p + r.at, len, 0, true };
			ok = read_chunk(which, &body, t);
			seen |= 1u << which;
		}
		r.at += len;
	}
	uint32_t need = (1u << CHUNK_COUNT) - 1;
	if (!b->has_lsp)
		need &= ~((1u << CHUNK_LSP) | (1u << CHUNK_LSP_ERAM));
	ok = ok && (seen & need) == need;

	if (ok)
	{
		t->xp.eram = b->xp.eram;
		t->lsp.eram = b->lsp.eram;
		memcpy(b, t, sizeof(*b));
		memcpy(b->xp.eram, xp_eram, XP_ERAM_SIZE * sizeof(int32_t));
		if (b->has_lsp)
			memcpy(b->lsp.eram, lsp_eram, LSP_ERAM_SIZE * sizeof(int32_t));
		b->xp.jit = &b->jit;
		b->lsp.jit = &b->jit;
		b->ga.user = b;
		b->sub.user = b;
		h8500_set_irq(&b->cpu, H8500_IRQ1, b->xp_int);
	}
	free(lsp_eram);
	free(xp_eram);
	free(t);
	return ok;
}
