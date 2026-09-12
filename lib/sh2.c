#include <string.h>
#include "sh2.h"
#include "state.h"
#include "sh2_jit.h"

#define SR_T 0x00000001u
#define SR_S 0x00000002u
#define SR_I 0x000000f0u
#define SR_Q 0x00000100u
#define SR_M 0x00000200u
#define SR_MASK 0x000003f3u

#define VEC_ILLEGAL 4
#define VEC_NMI 11

enum
{
	IO_SCI0 = 0x1a0, IO_SCI1 = 0x1b0,
	IO_TSTR = 0x240, IO_TSYR = 0x241,
	IO_MTU0 = 0x260, IO_MTU1 = 0x280, IO_MTU2 = 0x2a0,
	IO_IPRA = 0x348, IO_ICR = 0x358, IO_ISR = 0x35a,
	IO_PADRL = 0x382, IO_PAIORL = 0x386, IO_PACRL1 = 0x38c, IO_PACRL2 = 0x38e,
	IO_PBDR = 0x390, IO_PBIOR = 0x394, IO_PBCR1 = 0x398, IO_PBCR2 = 0x39a,
	IO_PEDR = 0x3b0, IO_PFDR = 0x3b3, IO_PEIOR = 0x3b4, IO_PECR1 = 0x3b8, IO_PECR2 = 0x3ba,
	IO_ADDRA = 0x420, IO_ADCSR = 0x428, IO_ADCR = 0x429,
	IO_WDT = 0x610, IO_RSTCSR = 0x612,
	IO_BSC = 0x620,
	IO_DMAOR = 0x6b0, IO_DMA0 = 0x6c0, IO_DMA1 = 0x6d0,
	IO_CCR = 0x740
};

enum
{
	SSR_TDRE = 0x80, SSR_RDRF = 0x40, SSR_ORER = 0x20,
	SSR_FER = 0x10, SSR_PER = 0x08, SSR_TEND = 0x04,
	SSR_MPB = 0x02, SSR_MPBT = 0x01
};

enum
{
	SCR_TIE = 0x80, SCR_RIE = 0x40, SCR_TE = 0x20, SCR_RE = 0x10,
	SCR_MPIE = 0x08, SCR_TEIE = 0x04
};

enum
{
	CHCR_DE = 0x0001, CHCR_TE = 0x0002, CHCR_IE = 0x0004, CHCR_TM = 0x0020
};

static const int sci_vector[2][4] =
{
	{ 128, 129, 130, 131 },
	{ 132, 133, 134, 135 }
};

static const int mtu_vector[3][6] =
{
	{ 88, 89, 90, 91, 92, -1 },
	{ 96, 97, -1, -1, 100, 101 },
	{ 104, 105, -1, -1, 108, 109 }
};

static const int dma_vector[2] = { 72, 76 };

static void dma_check(sh2_t *cpu, int n);
static int dma_activate(sh2_t *cpu, int vector);

/* ------------------------------------------------------------------ */
/* memory                                                             */
/* ------------------------------------------------------------------ */

static int region_find(sh2_t *cpu, uint32_t addr, uint32_t len, uint8_t **ptr)
{
	int i;
	for (i = 0; i < cpu->region_count; i++)
	{
		const sh2_region_t *r = &cpu->regions[i];
		uint32_t off = addr - r->base;
		if (off < r->size && off + len <= r->size)
		{
			if (r->bypass)
				break;
			*ptr = r->data + off;
			return r->writable ? 1 : 2;
		}
	}
	*ptr = NULL;
	return 0;
}

uint8_t sh2_mem_read8(sh2_t *cpu, uint32_t addr)
{
	uint8_t *p;
	if (addr >= SH2_IO_BASE)
	{
		if (addr < SH2_IO_END)
			return (uint8_t)sh2_io_read(cpu, addr, 1);
		if (addr >= SH2_RAM_BASE)
			return cpu->ram[addr - SH2_RAM_BASE];
		return 0;
	}
	if (region_find(cpu, addr, 1, &p))
		return *p;
	return cpu->bus.read8 ? cpu->bus.read8(cpu->bus.user, addr) : 0;
}

uint16_t sh2_mem_read16(sh2_t *cpu, uint32_t addr)
{
	uint8_t *p;
	addr &= ~1u;
	if (addr >= SH2_IO_BASE)
	{
		if (addr < SH2_IO_END)
			return (uint16_t)sh2_io_read(cpu, addr, 2);
		if (addr >= SH2_RAM_BASE)
		{
			uint32_t o = addr - SH2_RAM_BASE;
			return (uint16_t)((cpu->ram[o] << 8) | cpu->ram[o + 1]);
		}
		return 0;
	}
	if (region_find(cpu, addr, 2, &p))
		return (uint16_t)((p[0] << 8) | p[1]);
	return cpu->bus.read16 ? cpu->bus.read16(cpu->bus.user, addr) : 0;
}

uint32_t sh2_mem_read32(sh2_t *cpu, uint32_t addr)
{
	uint8_t *p;
	addr &= ~3u;
	if (addr >= SH2_IO_BASE)
	{
		if (addr < SH2_IO_END)
			return sh2_io_read(cpu, addr, 4);
		if (addr >= SH2_RAM_BASE)
		{
			uint32_t o = addr - SH2_RAM_BASE;
			return ((uint32_t)cpu->ram[o] << 24) | ((uint32_t)cpu->ram[o + 1] << 16)
				| ((uint32_t)cpu->ram[o + 2] << 8) | cpu->ram[o + 3];
		}
		return 0;
	}
	if (region_find(cpu, addr, 4, &p))
		return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
	if (cpu->bus.read32)
		return cpu->bus.read32(cpu->bus.user, addr);
	return ((uint32_t)sh2_mem_read16(cpu, addr) << 16) | sh2_mem_read16(cpu, addr + 2);
}

void sh2_mem_write8(sh2_t *cpu, uint32_t addr, uint8_t data)
{
	uint8_t *p;
	if (addr >= SH2_IO_BASE)
	{
		if (addr < SH2_IO_END)
			sh2_io_write(cpu, addr, 1, data);
		else if (addr >= SH2_RAM_BASE)
			cpu->ram[addr - SH2_RAM_BASE] = data;
		return;
	}
	switch (region_find(cpu, addr, 1, &p))
	{
	case 1: *p = data; return;
	case 2: break;
	default: break;
	}
	if (cpu->bus.write8)
		cpu->bus.write8(cpu->bus.user, addr, data);
}

void sh2_mem_write16(sh2_t *cpu, uint32_t addr, uint16_t data)
{
	uint8_t *p;
	addr &= ~1u;
	if (addr >= SH2_IO_BASE)
	{
		if (addr < SH2_IO_END)
			sh2_io_write(cpu, addr, 2, data);
		else if (addr >= SH2_RAM_BASE)
		{
			uint32_t o = addr - SH2_RAM_BASE;
			cpu->ram[o] = (uint8_t)(data >> 8);
			cpu->ram[o + 1] = (uint8_t)data;
		}
		return;
	}
	switch (region_find(cpu, addr, 2, &p))
	{
	case 1: p[0] = (uint8_t)(data >> 8); p[1] = (uint8_t)data; return;
	case 2: break;
	default: break;
	}
	if (cpu->bus.write16)
		cpu->bus.write16(cpu->bus.user, addr, data);
}

void sh2_mem_write32(sh2_t *cpu, uint32_t addr, uint32_t data)
{
	uint8_t *p;
	addr &= ~3u;
	if (addr >= SH2_IO_BASE)
	{
		if (addr < SH2_IO_END)
			sh2_io_write(cpu, addr, 4, data);
		else if (addr >= SH2_RAM_BASE)
		{
			uint32_t o = addr - SH2_RAM_BASE;
			cpu->ram[o] = (uint8_t)(data >> 24);
			cpu->ram[o + 1] = (uint8_t)(data >> 16);
			cpu->ram[o + 2] = (uint8_t)(data >> 8);
			cpu->ram[o + 3] = (uint8_t)data;
		}
		return;
	}
	switch (region_find(cpu, addr, 4, &p))
	{
	case 1:
		p[0] = (uint8_t)(data >> 24); p[1] = (uint8_t)(data >> 16);
		p[2] = (uint8_t)(data >> 8); p[3] = (uint8_t)data;
		return;
	case 2: break;
	default: break;
	}
	if (cpu->bus.write32)
	{
		cpu->bus.write32(cpu->bus.user, addr, data);
		return;
	}
	sh2_mem_write16(cpu, addr, (uint16_t)(data >> 16));
	sh2_mem_write16(cpu, addr + 2, (uint16_t)data);
}

/* ------------------------------------------------------------------ */
/* interrupt controller                                               */
/* ------------------------------------------------------------------ */

int sh2_vector_level(const sh2_t *cpu, int vector)
{
	switch (vector)
	{
	case VEC_NMI: return 16;
	case 64: return (cpu->ipr[0] >> 12) & 15;
	case 65: return (cpu->ipr[0] >> 8) & 15;
	case 66: return (cpu->ipr[0] >> 4) & 15;
	case 67: return cpu->ipr[0] & 15;
	case 70: return (cpu->ipr[1] >> 4) & 15;
	case 71: return cpu->ipr[1] & 15;
	case 72: return (cpu->ipr[2] >> 12) & 15;
	case 76: return (cpu->ipr[2] >> 8) & 15;
	case 88: case 89: case 90: case 91: return (cpu->ipr[3] >> 12) & 15;
	case 92: return (cpu->ipr[3] >> 8) & 15;
	case 96: case 97: return (cpu->ipr[3] >> 4) & 15;
	case 100: case 101: return cpu->ipr[3] & 15;
	case 104: case 105: return (cpu->ipr[4] >> 12) & 15;
	case 108: case 109: return (cpu->ipr[4] >> 8) & 15;
	case 128: case 129: case 130: case 131: return (cpu->ipr[5] >> 4) & 15;
	case 132: case 133: case 134: case 135: return cpu->ipr[5] & 15;
	case 136: return (cpu->ipr[6] >> 12) & 15;
	case 144: return (cpu->ipr[6] >> 4) & 15;
	case 148: return cpu->ipr[6] & 15;
	case 152: case 153: return (cpu->ipr[7] >> 12) & 15;
	default: return 0;
	}
}

static void intc_clear(sh2_t *cpu, int vector)
{
	cpu->pending[vector >> 5] &= ~(1u << (vector & 31));
}

static void intc_raise(sh2_t *cpu, int vector)
{
	if (dma_activate(cpu, vector))
	{
		intc_clear(cpu, vector);
		return;
	}
	cpu->pending[vector >> 5] |= 1u << (vector & 31);
}

static void intc_set(sh2_t *cpu, int vector, bool state)
{
	if (state)
		intc_raise(cpu, vector);
	else
		intc_clear(cpu, vector);
}

static void irq_pins_update(sh2_t *cpu)
{
	uint32_t bits = 0;
	if (cpu->isr & 0xf3u)
	{
		int i;
		for (i = 0; i < 8; i++)
			if (cpu->isr & (1u << (7 - i)))
				bits |= 1u << i;
	}
	cpu->pending[2] = (cpu->pending[2] & ~0xffu) | bits;
}

int sh2_irq_select(sh2_t *cpu, int *out_level)
{
	int mask = (int)((cpu->sr & SR_I) >> 4);
	int best = -1, best_level = -1;
	int i, j;

	irq_pins_update(cpu);
	if (cpu->nmi_pending)
	{
		*out_level = 16;
		return VEC_NMI;
	}
	if (!(cpu->pending[0] | cpu->pending[1] | cpu->pending[2]
		| cpu->pending[3] | cpu->pending[4]))
	{
		*out_level = -1;
		return -1;
	}
	for (i = 0; i < 8; i++)
	{
		uint32_t p = cpu->pending[i];
		while (p)
		{
			int b = 0;
			uint32_t bit;
			for (j = 0; j < 32; j++)
				if (p & (1u << j)) { b = j; break; }
			bit = 1u << b;
			p &= ~bit;
			{
				int vect = i * 32 + b;
				int level = sh2_vector_level(cpu, vect);
				if (level > mask && level > best_level)
				{
					best = vect;
					best_level = level;
				}
			}
		}
	}
	*out_level = best_level;
	return best;
}

void sh2_exception(sh2_t *cpu, int vector, uint32_t ret_pc, int level)
{
	cpu->r[15] -= 4;
	sh2_mem_write32(cpu, cpu->r[15], cpu->sr);
	cpu->r[15] -= 4;
	sh2_mem_write32(cpu, cpu->r[15], ret_pc);
	if (level >= 0)
	{
		if (level > 15)
			cpu->sr |= SR_I;
		else
			cpu->sr = (cpu->sr & ~SR_I) | (uint32_t)(level << 4);
	}
	cpu->pc = sh2_mem_read32(cpu, cpu->vbr + (uint32_t)vector * 4);
	cpu->sleeping = false;
}

void sh2_take_interrupt(sh2_t *cpu, int vector, int level)
{
	if (vector == VEC_NMI)
		cpu->nmi_pending = false;
	else
	{
		intc_clear(cpu, vector);
		if (vector >= 64 && vector < 72)
		{
			const int line = vector - 64;
			if ((cpu->icr >> (7 - line)) & 1)
				cpu->isr &= (uint16_t)~(1u << (7 - line));
		}
	}
	sh2_exception(cpu, vector, cpu->pc, level);
}

void sh2_set_irq(sh2_t *cpu, int line, bool state)
{
	if (line == SH2_NMI)
	{
		if (cpu->nmi_line == state)
			return;
		cpu->nmi_line = state;
		if (state)
			cpu->nmi_pending = true;
		return;
	}
	if (line < 0 || line > 7)
		return;
	{
		uint8_t mask = (uint8_t)(1u << line);
		bool was = (cpu->irq_line & mask) != 0;
		bool edge = ((cpu->icr >> (7 - line)) & 1) != 0;
		if (state)
			cpu->irq_line |= mask;
		else
			cpu->irq_line &= (uint8_t)~mask;
		if (edge)
		{
			if (state && !was)
				cpu->isr |= (uint16_t)(1u << (7 - line));
		}
		else
		{
			if (state)
				cpu->isr |= (uint16_t)(1u << (7 - line));
			else
				cpu->isr &= (uint16_t)~(1u << (7 - line));
		}
		irq_pins_update(cpu);
	}
}

/* ------------------------------------------------------------------ */
/* I/O ports                                                          */
/* ------------------------------------------------------------------ */

static uint16_t port_pins(sh2_t *cpu, int port)
{
	return cpu->bus.read_port ? cpu->bus.read_port(cpu->bus.user, port) : 0;
}

static void port_update(sh2_t *cpu, int port, uint16_t dr, uint16_t ior)
{
	uint16_t v = (uint16_t)(dr & ior);
	if (cpu->port_out[port] == v)
		return;
	cpu->port_out[port] = v;
	if (cpu->bus.write_port)
		cpu->bus.write_port(cpu->bus.user, port, v, ior);
}

/* ------------------------------------------------------------------ */
/* serial                                                             */
/* ------------------------------------------------------------------ */

static uint32_t sci_bit_cycles(sh2_t *cpu, int ch)
{
	const sh2_sci_t *s = &cpu->sci[ch];
	uint32_t div = (s->smr & 0x80) ? 4u : 32u;
	return div * (1u << (2 * (s->smr & 3))) * ((uint32_t)s->brr + 1);
}

static uint32_t sci_char_cycles(sh2_t *cpu, int ch)
{
	const sh2_sci_t *s = &cpu->sci[ch];
	uint32_t bits;
	if (s->smr & 0x80)
		bits = 8;
	else
	{
		bits = 1 + ((s->smr & 0x40) ? 7u : 8u);
		if ((s->smr & 0x24) == 0x20)
			bits++;
		bits += (s->smr & 0x08) ? 2u : 1u;
	}
	return bits * sci_bit_cycles(cpu, ch);
}

static void sci_int(sh2_t *cpu, int ch, int which, bool state)
{
	bool *flag;
	switch (which)
	{
	case 0: flag = &cpu->sci[ch].int_eri; break;
	case 1: flag = &cpu->sci[ch].int_rxi; break;
	case 2: flag = &cpu->sci[ch].int_txi; break;
	default: flag = &cpu->sci[ch].int_tei; break;
	}
	if (!state && !*flag)
		return;
	*flag = state;
	intc_set(cpu, sci_vector[ch][which], state);
}

static void sci_ints(sh2_t *cpu, int ch)
{
	const sh2_sci_t *s = &cpu->sci[ch];
	sci_int(cpu, ch, 0, (s->scr & SCR_RIE) && (s->ssr & (SSR_ORER | SSR_FER | SSR_PER)));
	sci_int(cpu, ch, 1, (s->scr & SCR_RIE) && (s->ssr & SSR_RDRF));
	sci_int(cpu, ch, 2, (s->scr & SCR_TIE) && (s->ssr & SSR_TDRE));
	sci_int(cpu, ch, 3, (s->scr & SCR_TEIE) && (s->ssr & SSR_TEND));
}

static void sci_tx_start(sh2_t *cpu, int ch)
{
	sh2_sci_t *s = &cpu->sci[ch];
	if (!(s->scr & SCR_TE) || (s->ssr & SSR_TDRE) || s->tx_busy)
		return;
	s->tx_shift = s->tdr;
	s->tx_busy = true;
	s->tx_timer = sci_char_cycles(cpu, ch);
	s->ssr |= SSR_TDRE;
	sci_ints(cpu, ch);
}

static void sci_update(sh2_t *cpu, int ch, uint32_t cycles)
{
	sh2_sci_t *s = &cpu->sci[ch];

	if (s->tx_busy)
	{
		if (s->tx_timer > cycles)
		{
			s->tx_timer -= cycles;
		}
		else
		{
			s->tx_timer = 0;
			s->tx_busy = false;
			if (cpu->bus.sci_tx)
				cpu->bus.sci_tx(cpu->bus.user, ch, s->tx_shift);
			if (!(s->ssr & SSR_TDRE))
			{
				sci_tx_start(cpu, ch);
			}
			else
			{
				s->ssr |= SSR_TEND;
				sci_ints(cpu, ch);
			}
		}
	}

	if (s->rx_pending && (s->scr & SCR_RE))
	{
		s->rx_pending = false;
		if (!(s->ssr & SSR_RDRF))
		{
			s->rdr = s->rx_byte;
			s->ssr |= SSR_RDRF;
		}
		else
		{
			s->ssr |= SSR_ORER;
		}
		sci_ints(cpu, ch);
	}
}

void sh2_sci_rx(sh2_t *cpu, int channel, uint8_t byte)
{
	int ch = channel & 1;
	cpu->sci[ch].rx_byte = byte;
	cpu->sci[ch].rx_pending = true;
}

/* ------------------------------------------------------------------ */
/* multifunction timer                                                */
/* ------------------------------------------------------------------ */

static int mtu_shift(const sh2_t *cpu, int n)
{
	static const signed char sh0[8] = { 0, 2, 4, 6, -1, -1, -1, -1 };
	static const signed char sh1[8] = { 0, 2, 4, 6, -1, -1, 8, -1 };
	static const signed char sh2s[8] = { 0, 2, 4, 6, -1, -1, -1, 10 };
	int sel = cpu->mtu[n].tcr & 7;
	if (n == 0) return sh0[sel];
	if (n == 1) return sh1[sel];
	return sh2s[sel];
}

static int mtu_clear_source(const sh2_t *cpu, int n)
{
	int c = (cpu->mtu[n].tcr >> 5) & (n == 0 ? 7 : 3);
	if (c == 1 || c == 2)
		return c - 1;
	if (n == 0 && (c == 5 || c == 6))
		return c - 3;
	return -1;
}

static int mtu_tgr_count(int n)
{
	return n == 0 ? 4 : 2;
}

static void mtu_flag(sh2_t *cpu, int n, int i)
{
	sh2_mtu_channel_t *ch = &cpu->mtu[n];
	if (ch->tsr & (1u << i))
		return;
	ch->tsr |= (uint8_t)(1u << i);
	if ((ch->tier & (1u << i)) && mtu_vector[n][i] >= 0)
		intc_raise(cpu, mtu_vector[n][i]);
}

static void mtu_step(sh2_t *cpu, int n)
{
	sh2_mtu_channel_t *ch = &cpu->mtu[n];
	int clear = mtu_clear_source(cpu, n);
	int count = mtu_tgr_count(n);
	int i;

	ch->tcnt = (uint16_t)(ch->tcnt + 1);
	for (i = 0; i < count; i++)
		if (ch->tcnt == ch->tgr[i])
			mtu_flag(cpu, n, i);
	if (clear >= 0 && ch->tcnt == ch->tgr[clear])
	{
		ch->tcnt = 0;
		return;
	}
	if (ch->tcnt == 0)
	{
		if (!(ch->tsr & 0x10))
		{
			ch->tsr |= 0x10;
			if ((ch->tier & 0x10) && mtu_vector[n][4] >= 0)
				intc_raise(cpu, mtu_vector[n][4]);
		}
	}
}

static void mtu_update(sh2_t *cpu, int n, uint32_t cycles)
{
	sh2_mtu_channel_t *ch = &cpu->mtu[n];
	int shift;
	uint32_t steps;

	if (!ch->active)
		return;
	shift = mtu_shift(cpu, n);
	if (shift < 0)
		return;
	ch->prescale += cycles;
	steps = ch->prescale >> shift;
	ch->prescale &= (1u << shift) - 1;
	while (steps--)
		mtu_step(cpu, n);
}

/* ------------------------------------------------------------------ */
/* watchdog                                                           */
/* ------------------------------------------------------------------ */

static int wdt_shift(const sh2_t *cpu)
{
	static const uint8_t shifts[8] = { 1, 6, 7, 8, 9, 10, 12, 13 };
	return shifts[cpu->wtcsr & 7];
}

static void wdt_interrupt(sh2_t *cpu)
{
	bool state = (cpu->wtcsr & 0xc0) == 0x80;
	if (state == cpu->wdt_int)
		return;
	cpu->wdt_int = state;
	intc_set(cpu, 152, state);
}

static void wdt_update(sh2_t *cpu, uint32_t cycles)
{
	int shift;
	uint32_t steps;

	if (!(cpu->wtcsr & 0x20))
		return;
	shift = wdt_shift(cpu);
	cpu->wdt_prescale += cycles;
	steps = cpu->wdt_prescale >> shift;
	cpu->wdt_prescale &= (1u << shift) - 1;
	while (steps--)
	{
		cpu->wtcnt++;
		if (cpu->wtcnt == 0)
		{
			if (cpu->wtcsr & 0x40)
				cpu->rstcsr |= 0x80;
			else
				cpu->wtcsr |= 0x80;
			wdt_interrupt(cpu);
		}
	}
}

/* ------------------------------------------------------------------ */
/* A/D                                                                */
/* ------------------------------------------------------------------ */

static void adc_interrupt(sh2_t *cpu)
{
	bool state = (cpu->adcsr & 0xc0) == 0xc0;
	if (state == cpu->adc_int)
		return;
	cpu->adc_int = state;
	intc_set(cpu, 136, state);
}

static uint32_t adc_conv_cycles(const sh2_t *cpu)
{
	uint32_t one = (cpu->adcsr & 0x08) ? 134u : 266u;
	uint32_t n = (cpu->adcsr & 0x10) ? (uint32_t)(cpu->adcsr & 3) + 1u : 1u;
	return one * n;
}

static uint16_t adc_sample(sh2_t *cpu, int channel)
{
	uint16_t v = cpu->bus.read_adc ? cpu->bus.read_adc(cpu->bus.user, channel) : 0;
	return (uint16_t)((v & 0x3ff) << 6);
}

static void adc_update(sh2_t *cpu, uint32_t cycles)
{
	if (!cpu->adc_timer)
		return;
	if (cpu->adc_timer > cycles)
	{
		cpu->adc_timer -= cycles;
		return;
	}
	cpu->adc_timer = 0;
	{
		int last = cpu->adcsr & 3;
		if (cpu->adcsr & 0x10)
		{
			int c;
			for (c = 0; c <= last; c++)
				cpu->addr_[c] = adc_sample(cpu, c);
			cpu->adc_timer = adc_conv_cycles(cpu);
		}
		else
		{
			cpu->addr_[last] = adc_sample(cpu, last);
			cpu->adcsr &= (uint8_t)~0x20;
		}
		cpu->adcsr |= 0x80;
		adc_interrupt(cpu);
	}
}

/* ------------------------------------------------------------------ */
/* DMA                                                                */
/* ------------------------------------------------------------------ */

enum
{
	DMA_SRC_EXT_DUAL = 0,
	DMA_SRC_EXT_SINGLE,
	DMA_SRC_AUTO,
	DMA_SRC_ONCHIP,
	DMA_SRC_PROHIBITED
};

static int dma_source_type(uint32_t chcr)
{
	switch ((chcr >> 8) & 15)
	{
	case 0: return DMA_SRC_EXT_DUAL;
	case 2: case 3: return DMA_SRC_EXT_SINGLE;
	case 4: return DMA_SRC_AUTO;
	case 6: case 7: case 8:
	case 11: case 12: case 13:
	case 14: case 15: return DMA_SRC_ONCHIP;
	default: return DMA_SRC_PROHIBITED;
	}
}

static int dma_source_vector(uint32_t chcr)
{
	switch ((chcr >> 8) & 15)
	{
	case 6: return 88;
	case 7: return 96;
	case 8: return 104;
	case 11: return 136;
	case 12: return 130;
	case 13: return 129;
	case 14: return 134;
	case 15: return 133;
	default: return -1;
	}
}

static bool dma_allowed(const sh2_t *cpu)
{
	return (cpu->dmaor & 1) != 0 && (cpu->dmaor & 6) == 0;
}

static void dma_check(sh2_t *cpu, int n)
{
	sh2_dma_channel_t *d = &cpu->dma[n];
	uint32_t sm, dm, ts;

	if (!dma_allowed(cpu) || !(d->chcr & CHCR_DE) || (d->chcr & CHCR_TE))
	{
		d->active = false;
		d->timer = 0;
		return;
	}
	if (d->active)
		return;

	dm = (d->chcr >> 14) & 3;
	sm = (d->chcr >> 12) & 3;
	ts = (d->chcr >> 3) & 3;
	if (dm == 3 || sm == 3 || ts == 3)
		return;

	if (d->dmatcr == 0)
		d->count = 0x10000;
	else
		d->count = d->dmatcr;
	d->active = true;

	{
		uint32_t unit = 1u << ts;
		if (unit > 1)
		{
			d->sar &= ~(unit - 1);
			d->dar &= ~(unit - 1);
		}
	}

	if (dma_source_type(d->chcr) != DMA_SRC_ONCHIP || (d->chcr & CHCR_TM))
		d->timer = 2;
	else
		d->timer = 0;
}

static int dma_activate(sh2_t *cpu, int vector)
{
	int n;
	if (!dma_allowed(cpu))
		return 0;
	for (n = 0; n < 2; n++)
	{
		sh2_dma_channel_t *d = &cpu->dma[n];
		if (dma_source_type(d->chcr) != DMA_SRC_ONCHIP || dma_source_vector(d->chcr) != vector)
			continue;
		if (!d->active)
			dma_check(cpu, n);
		if (!d->active)
			continue;
		d->timer = 2;
		return 1;
	}
	return 0;
}

static void dma_transfer(sh2_t *cpu, int n)
{
	sh2_dma_channel_t *d = &cpu->dma[n];
	uint32_t unit = 1u << ((d->chcr >> 3) & 3);
	uint32_t sm = (d->chcr >> 12) & 3;
	uint32_t dm = (d->chcr >> 14) & 3;

	if (sm == 2) d->sar -= unit;
	if (dm == 2) d->dar -= unit;

	switch (unit)
	{
	case 1: sh2_mem_write8(cpu, d->dar, sh2_mem_read8(cpu, d->sar)); break;
	case 2: sh2_mem_write16(cpu, d->dar, sh2_mem_read16(cpu, d->sar)); break;
	default: sh2_mem_write32(cpu, d->dar, sh2_mem_read32(cpu, d->sar)); break;
	}

	if (sm == 1) d->sar += unit;
	if (dm == 1) d->dar += unit;

	d->count--;
	d->dmatcr = d->count & 0xffff;

	if (d->count == 0)
	{
		d->chcr |= CHCR_TE;
		d->active = false;
		d->timer = 0;
		if (d->chcr & CHCR_IE)
		{
			d->int_te = true;
			intc_raise(cpu, dma_vector[n]);
		}
		return;
	}
	if (dma_source_type(d->chcr) != DMA_SRC_ONCHIP || (d->chcr & CHCR_TM))
		d->timer = 2;
	else
		d->timer = 0;
}

static void dma_update(sh2_t *cpu, int n, uint32_t cycles)
{
	sh2_dma_channel_t *d = &cpu->dma[n];
	while (d->active && d->timer && cycles)
	{
		if (d->timer > cycles)
		{
			d->timer -= cycles;
			return;
		}
		cycles -= d->timer;
		d->timer = 0;
		if (!dma_allowed(cpu))
		{
			d->active = false;
			return;
		}
		dma_transfer(cpu, n);
	}
}

/* ------------------------------------------------------------------ */
/* the peripheral clock                                               */
/* ------------------------------------------------------------------ */

void sh2_peripherals_tick(sh2_t *cpu, uint32_t cycles)
{
	if (!cycles)
		return;
	if (cpu->mtu[0].active) mtu_update(cpu, 0, cycles);
	if (cpu->mtu[1].active) mtu_update(cpu, 1, cycles);
	if (cpu->mtu[2].active) mtu_update(cpu, 2, cycles);
	if (cpu->wtcsr & 0x20) wdt_update(cpu, cycles);
	if (cpu->adc_timer) adc_update(cpu, cycles);
	if (cpu->dma[0].active) dma_update(cpu, 0, cycles);
	if (cpu->dma[1].active) dma_update(cpu, 1, cycles);
	if (cpu->sci[0].tx_busy || cpu->sci[0].rx_pending) sci_update(cpu, 0, cycles);
	if (cpu->sci[1].tx_busy || cpu->sci[1].rx_pending) sci_update(cpu, 1, cycles);
}

static uint32_t mtu_horizon(sh2_t *cpu, int n)
{
	sh2_mtu_channel_t *ch = &cpu->mtu[n];
	int shift = mtu_shift(cpu, n);
	int clear, count, i;
	uint32_t best, d;

	if (!ch->active || shift < 0)
		return UINT32_MAX;
	clear = mtu_clear_source(cpu, n);
	count = mtu_tgr_count(n);
	best = (uint32_t)((uint16_t)(0 - ch->tcnt));
	if (!best) best = 0x10000;
	for (i = 0; i < count; i++)
	{
		d = (uint32_t)((uint16_t)(ch->tgr[i] - ch->tcnt));
		if (!d) d = 0x10000;
		if (d < best) best = d;
	}
	if (clear >= 0)
	{
		d = (uint32_t)((uint16_t)(ch->tgr[clear] - ch->tcnt));
		if (!d) d = 0x10000;
		if (d < best) best = d;
	}
	return (best << shift) - ch->prescale;
}

static uint32_t wdt_horizon(sh2_t *cpu)
{
	if (!(cpu->wtcsr & 0x20))
		return UINT32_MAX;
	return ((256u - cpu->wtcnt) << wdt_shift(cpu)) - cpu->wdt_prescale;
}

static uint32_t sci_horizon(sh2_t *cpu, int ch)
{
	sh2_sci_t *s = &cpu->sci[ch];
	if (s->rx_pending && (s->scr & SCR_RE))
		return 1;
	if (s->tx_busy)
		return s->tx_timer ? s->tx_timer : 1;
	return UINT32_MAX;
}

uint32_t sh2_tick_horizon(sh2_t *cpu)
{
	uint32_t h = mtu_horizon(cpu, 0), d;
	d = mtu_horizon(cpu, 1); if (d < h) h = d;
	d = mtu_horizon(cpu, 2); if (d < h) h = d;
	d = wdt_horizon(cpu); if (d < h) h = d;
	d = cpu->adc_timer ? cpu->adc_timer : UINT32_MAX; if (d < h) h = d;
	d = (cpu->dma[0].active && cpu->dma[0].timer) ? cpu->dma[0].timer : UINT32_MAX; if (d < h) h = d;
	d = (cpu->dma[1].active && cpu->dma[1].timer) ? cpu->dma[1].timer : UINT32_MAX; if (d < h) h = d;
	d = sci_horizon(cpu, 0); if (d < h) h = d;
	d = sci_horizon(cpu, 1); if (d < h) h = d;
	return h ? h : 1;
}

/* ------------------------------------------------------------------ */
/* the on-chip register block                                         */
/* ------------------------------------------------------------------ */

static const uint16_t mtu_base[3] = { IO_MTU0, IO_MTU1, IO_MTU2 };

static int mtu_decode(uint32_t off, int *reg)
{
	int n;
	for (n = 0; n < 3; n++)
	{
		uint32_t o = off - mtu_base[n];
		if (o < 0x10)
		{
			*reg = (int)o;
			return n;
		}
	}
	return -1;
}

static uint8_t sci_read(sh2_t *cpu, int ch, int reg)
{
	sh2_sci_t *s = &cpu->sci[ch];
	switch (reg)
	{
	case 0: return s->smr;
	case 1: return s->brr;
	case 2: return s->scr;
	case 3: return s->tdr;
	case 4: return s->ssr;
	default: return s->rdr;
	}
}

static void sci_write(sh2_t *cpu, int ch, int reg, uint8_t data)
{
	sh2_sci_t *s = &cpu->sci[ch];
	switch (reg)
	{
	case 0: s->smr = data; break;
	case 1: s->brr = data; break;
	case 2:
		s->scr = data;
		if (!(data & SCR_TE))
			s->ssr |= (uint8_t)(SSR_TEND | SSR_TDRE);
		sci_ints(cpu, ch);
		break;
	case 3: s->tdr = data; break;
	case 4:
	{
		uint8_t old = s->ssr;
		uint8_t clearable = (uint8_t)(SSR_TDRE | SSR_RDRF | SSR_ORER | SSR_FER | SSR_PER);
		uint8_t held = (uint8_t)(old & (data | (uint8_t)~clearable) & (uint8_t)~SSR_MPBT);
		s->ssr = (uint8_t)(held | (data & SSR_MPBT));
		if ((old & SSR_TDRE) && !(s->ssr & SSR_TDRE))
		{
			s->ssr &= (uint8_t)~SSR_TEND;
			sci_tx_start(cpu, ch);
		}
		if (!(s->scr & SCR_TE))
			s->ssr |= (uint8_t)(SSR_TEND | SSR_TDRE);
		sci_ints(cpu, ch);
		break;
	}
	default: break;
	}
}

static void wdt_write16(sh2_t *cpu, uint16_t data)
{
	if ((data >> 8) == 0x5a)
	{
		cpu->wtcnt = (uint8_t)data;
		cpu->wdt_prescale = 0;
	}
	else if ((data >> 8) == 0xa5)
	{
		cpu->wtcsr = (uint8_t)((cpu->wtcsr & data & 0x80) | (data & 0x7f) | 0x18);
		if (!(cpu->wtcsr & 0x20))
		{
			cpu->wtcnt = 0;
			cpu->wdt_prescale = 0;
		}
		wdt_interrupt(cpu);
	}
}

static int io_reg8(sh2_t *cpu, uint32_t off, uint8_t *out)
{
	int reg, n;

	if (off - IO_SCI0 < 6) { *out = sci_read(cpu, 0, (int)(off - IO_SCI0)); return 1; }
	if (off - IO_SCI1 < 6) { *out = sci_read(cpu, 1, (int)(off - IO_SCI1)); return 1; }
	if (off == IO_TSTR)
	{
		*out = (uint8_t)(cpu->mtu[0].active | (cpu->mtu[1].active << 1) | (cpu->mtu[2].active << 2));
		return 1;
	}
	if (off == IO_TSYR) { *out = (uint8_t)(cpu->tsyr & 7); return 1; }

	n = mtu_decode(off, &reg);
	if (n > 0 && reg >= 0x0c)
		n = -1;
	if (n >= 0)
	{
		sh2_mtu_channel_t *ch = &cpu->mtu[n];
		switch (reg)
		{
		case 0: *out = (uint8_t)(n ? (ch->tcr & 0x7f) : ch->tcr); return 1;
		case 1: *out = (uint8_t)((n ? (ch->tmdr & 0x0f) : ch->tmdr) | 0xc0); return 1;
		case 2: *out = ch->tiorh; return 1;
		case 3: *out = (uint8_t)(n ? 0 : ch->tiorl); return 1;
		case 4: *out = (uint8_t)(ch->tier | 0x40); return 1;
		case 5:
			*out = (uint8_t)(n == 0 ? ((ch->tsr & 0x1f) | 0xc0) : ((ch->tsr & 0xb3) | 0x40));
			return 1;
		default: return 0;
		}
	}

	if (off == IO_PFDR) { *out = (uint8_t)port_pins(cpu, SH2_PORT_F); return 1; }
	if (off == IO_ADCSR) { *out = cpu->adcsr; return 1; }
	if (off == IO_ADCR) { *out = cpu->adcr; return 1; }
	if (off == IO_WDT) { *out = cpu->wtcsr; return 1; }
	if (off == IO_WDT + 1) { *out = cpu->wtcnt; return 1; }
	if (off == IO_RSTCSR + 1) { *out = cpu->rstcsr; return 1; }
	return 0;
}

static int io_wreg8(sh2_t *cpu, uint32_t off, uint8_t data)
{
	int reg, n;

	if (off - IO_SCI0 < 6) { sci_write(cpu, 0, (int)(off - IO_SCI0), data); return 1; }
	if (off - IO_SCI1 < 6) { sci_write(cpu, 1, (int)(off - IO_SCI1), data); return 1; }
	if (off == IO_TSTR)
	{
		cpu->mtu[0].active = (data & 1) != 0;
		cpu->mtu[1].active = (data & 2) != 0;
		cpu->mtu[2].active = (data & 4) != 0;
		return 1;
	}
	if (off == IO_TSYR) { cpu->tsyr = data; return 1; }

	n = mtu_decode(off, &reg);
	if (n > 0 && reg >= 0x0c)
		n = -1;
	if (n >= 0)
	{
		sh2_mtu_channel_t *ch = &cpu->mtu[n];
		switch (reg)
		{
		case 0:
		{
			int shift;
			ch->tcr = data;
			shift = mtu_shift(cpu, n);
			ch->prescale = (shift >= 0) ? (ch->prescale & ((1u << shift) - 1)) : 0;
			return 1;
		}
		case 1: ch->tmdr = data; return 1;
		case 2: ch->tiorh = data; return 1;
		case 3: if (!n) ch->tiorl = data; return 1;
		case 4: ch->tier = data; return 1;
		case 5:
		{
			uint8_t mask = (uint8_t)(n == 0 ? 0x1f : 0x33);
			ch->tsr = (uint8_t)((data & (uint8_t)~mask) | (ch->tsr & data & mask) | (ch->tsr & 0x80));
			return 1;
		}
		default: return 0;
		}
	}

	if (off == IO_ADCSR)
	{
		bool was = (cpu->adcsr & 0x20) != 0;
		cpu->adcsr = (uint8_t)((cpu->adcsr & data & 0x80) | (data & 0x7f));
		adc_interrupt(cpu);
		if ((cpu->adcsr & 0x20) && !was)
			cpu->adc_timer = adc_conv_cycles(cpu);
		else if (!(cpu->adcsr & 0x20))
			cpu->adc_timer = 0;
		return 1;
	}
	if (off == IO_ADCR) { cpu->adcr = (uint8_t)(data | 0x7f); return 1; }
	if (off == IO_PFDR) return 1;
	return 0;
}

static int io_reg16(sh2_t *cpu, uint32_t off, uint16_t *out)
{
	static const uint16_t ipr_read[8] = { 0xffff, 0xff00, 0xffff, 0xffff, 0xff00, 0x00ff, 0xf0ff, 0xf000 };
	int reg, n;

	if (off - IO_IPRA < 16 && !(off & 1))
	{
		int i = (int)((off - IO_IPRA) >> 1);
		*out = (uint16_t)(cpu->ipr[i] & ipr_read[i]);
		return 1;
	}
	n = mtu_decode(off, &reg);
	if (n >= 0 && reg >= 6 && !(reg & 1) && !(n > 0 && reg >= 0x0c))
	{
		sh2_mtu_channel_t *ch = &cpu->mtu[n];
		*out = (reg == 6) ? ch->tcnt : ch->tgr[(reg - 8) >> 1];
		return 1;
	}

	switch (off)
	{
	case IO_ICR:
		*out = (uint16_t)((cpu->icr & 0x01f3)
			| ((cpu->nmi_line == ((cpu->icr & 0x0100) != 0)) ? 0x8000u : 0u));
		return 1;
	case IO_ISR: *out = (uint16_t)(cpu->isr & 0xf3); return 1;
	case IO_PADRL:
		*out = (uint16_t)(((cpu->padr & cpu->paior) | (port_pins(cpu, SH2_PORT_A) & ~cpu->paior)) & 0x83ff);
		return 1;
	case IO_PAIORL: *out = cpu->paior; return 1;
	case IO_PACRL1: *out = cpu->pacr1; return 1;
	case IO_PACRL2: *out = cpu->pacr2; return 1;
	case IO_PBDR:
		*out = (uint16_t)(((cpu->pbdr & cpu->pbior) | (port_pins(cpu, SH2_PORT_B) & ~cpu->pbior)) & 0x03ff);
		return 1;
	case IO_PBIOR: *out = cpu->pbior; return 1;
	case IO_PBCR1: *out = cpu->pbcr1; return 1;
	case IO_PBCR2: *out = cpu->pbcr2; return 1;
	case IO_PEDR:
		*out = (uint16_t)((cpu->pedr & cpu->peior) | (port_pins(cpu, SH2_PORT_E) & ~cpu->peior));
		return 1;
	case IO_PEIOR: *out = cpu->peior; return 1;
	case IO_PECR1: *out = cpu->pecr1; return 1;
	case IO_PECR2: *out = cpu->pecr2; return 1;
	case IO_ADDRA: *out = cpu->addr_[0]; return 1;
	case IO_ADDRA + 2: *out = cpu->addr_[1]; return 1;
	case IO_ADDRA + 4: *out = cpu->addr_[2]; return 1;
	case IO_ADDRA + 6: *out = cpu->addr_[3]; return 1;
	case IO_BSC + 0: *out = (uint16_t)((cpu->bcr1 & 0x010f) | 0x2000); return 1;
	case IO_BSC + 2: *out = cpu->bcr2; return 1;
	case IO_BSC + 4: *out = cpu->wcr1; return 1;
	case IO_BSC + 6: *out = (uint16_t)(cpu->wcr2 & 0x03ff); return 1;
	case IO_BSC + 10: *out = (uint16_t)(cpu->dcr & (uint16_t)~0x48u); return 1;
	case IO_BSC + 12: *out = (uint16_t)(cpu->rtcsr & 0x7f); return 1;
	case IO_BSC + 14: *out = (uint16_t)(cpu->rtcnt & 0xff); return 1;
	case IO_BSC + 16: *out = (uint16_t)(cpu->rtcor & 0xff); return 1;
	case IO_DMAOR: *out = (uint16_t)(cpu->dmaor & 7); return 1;
	case IO_CCR: *out = (uint16_t)(cpu->ccr & 0x1f); return 1;
	default: return 0;
	}
}

static int io_wreg16(sh2_t *cpu, uint32_t off, uint16_t data)
{
	int reg, n;

	if (off - IO_IPRA < 16 && !(off & 1))
	{
		cpu->ipr[(off - IO_IPRA) >> 1] = data;
		return 1;
	}
	n = mtu_decode(off, &reg);
	if (n >= 0 && reg >= 6 && !(reg & 1) && !(n > 0 && reg >= 0x0c))
	{
		sh2_mtu_channel_t *ch = &cpu->mtu[n];
		if (reg == 6)
			ch->tcnt = data;
		else
			ch->tgr[(reg - 8) >> 1] = data;
		return 1;
	}

	switch (off)
	{
	case IO_ICR: cpu->icr = data; irq_pins_update(cpu); return 1;
	case IO_ISR:
	{
		uint16_t old = cpu->isr;
		cpu->isr = (uint16_t)((old & ~cpu->icr) | (old & data & cpu->icr));
		irq_pins_update(cpu);
		return 1;
	}
	case IO_PADRL: cpu->padr = (uint16_t)(data & 0x83ff); port_update(cpu, SH2_PORT_A, cpu->padr, cpu->paior); return 1;
	case IO_PAIORL: cpu->paior = (uint16_t)(data & 0x83ff); port_update(cpu, SH2_PORT_A, cpu->padr, cpu->paior); return 1;
	case IO_PACRL1: cpu->pacr1 = (uint16_t)(data & 0x400f); return 1;
	case IO_PACRL2: cpu->pacr2 = (uint16_t)(data & 0xfd75); return 1;
	case IO_PBDR: cpu->pbdr = data; port_update(cpu, SH2_PORT_B, cpu->pbdr, cpu->pbior); return 1;
	case IO_PBIOR: cpu->pbior = (uint16_t)(data & 0x03fc); port_update(cpu, SH2_PORT_B, cpu->pbdr, cpu->pbior); return 1;
	case IO_PBCR1: cpu->pbcr1 = (uint16_t)(data & 0x000f); return 1;
	case IO_PBCR2: cpu->pbcr2 = (uint16_t)(data & 0xfff5); return 1;
	case IO_PEDR: cpu->pedr = data; port_update(cpu, SH2_PORT_E, cpu->pedr, cpu->peior); return 1;
	case IO_PEIOR: cpu->peior = data; port_update(cpu, SH2_PORT_E, cpu->pedr, cpu->peior); return 1;
	case IO_PECR1: cpu->pecr1 = (uint16_t)(data & 0xf000); return 1;
	case IO_PECR2: cpu->pecr2 = (uint16_t)(data & 0x55ff); return 1;
	case IO_WDT: wdt_write16(cpu, data); return 1;
	case IO_RSTCSR:
		if ((data >> 8) == 0x5a)
			cpu->rstcsr &= (uint8_t)~0x80;
		else if ((data >> 8) == 0xa5)
			cpu->rstcsr = (uint8_t)((data & 0x40) | 0x1f);
		return 1;
	case IO_BSC + 0: cpu->bcr1 = data; return 1;
	case IO_BSC + 2: cpu->bcr2 = data; return 1;
	case IO_BSC + 4: cpu->wcr1 = data; return 1;
	case IO_BSC + 6: cpu->wcr2 = data; return 1;
	case IO_BSC + 10: cpu->dcr = data; return 1;
	case IO_BSC + 12: cpu->rtcsr = data; return 1;
	case IO_BSC + 14: cpu->rtcnt = data; return 1;
	case IO_BSC + 16: cpu->rtcor = data; return 1;
	case IO_DMAOR:
		cpu->dmaor = (uint16_t)((data & (uint16_t)~6u) | (cpu->dmaor & data & 6u));
		dma_check(cpu, 0);
		dma_check(cpu, 1);
		return 1;
	case IO_CCR: cpu->ccr = data; return 1;
	case IO_ADDRA: case IO_ADDRA + 2: case IO_ADDRA + 4: case IO_ADDRA + 6:
		return 1;
	default: return 0;
	}
}

static int io_reg32(sh2_t *cpu, uint32_t off, uint32_t *out)
{
	int n;
	for (n = 0; n < 2; n++)
	{
		uint32_t base = n ? IO_DMA1 : IO_DMA0;
		if (off - base < 0x10 && !(off & 3))
		{
			const sh2_dma_channel_t *d = &cpu->dma[n];
			switch (off - base)
			{
			case 0: *out = d->sar; return 1;
			case 4: *out = d->dar; return 1;
			case 8: *out = d->dmatcr & 0xffffu; return 1;
			default: *out = d->chcr & 0x000bff7fu; return 1;
			}
		}
	}
	return 0;
}

static int io_wreg32(sh2_t *cpu, uint32_t off, uint32_t data)
{
	int n;
	for (n = 0; n < 2; n++)
	{
		uint32_t base = n ? IO_DMA1 : IO_DMA0;
		if (off - base < 0x10 && !(off & 3))
		{
			sh2_dma_channel_t *d = &cpu->dma[n];
			switch (off - base)
			{
			case 0: d->sar = data; return 1;
			case 4: d->dar = data; return 1;
			case 8: d->dmatcr = data & 0xffffu; return 1;
			default:
				d->chcr = data;
				if (!(data & CHCR_TE))
					d->int_te = false;
				dma_check(cpu, n);
				return 1;
			}
		}
	}
	return 0;
}

static uint8_t io_read8(sh2_t *cpu, uint32_t off)
{
	uint8_t b;
	uint16_t w;
	uint32_t l;
	if (io_reg8(cpu, off, &b))
		return b;
	if (io_reg16(cpu, off & ~1u, &w))
		return (off & 1) ? (uint8_t)w : (uint8_t)(w >> 8);
	if (io_reg32(cpu, off & ~3u, &l))
		return (uint8_t)(l >> (8 * (3 - (off & 3))));
	return 0;
}

static void io_write8(sh2_t *cpu, uint32_t off, uint8_t data)
{
	uint16_t w;
	uint32_t l;
	if (io_wreg8(cpu, off, data))
		return;
	if (io_reg16(cpu, off & ~1u, &w))
	{
		w = (off & 1) ? (uint16_t)((w & 0xff00) | data) : (uint16_t)((w & 0x00ff) | (data << 8));
		io_wreg16(cpu, off & ~1u, w);
		return;
	}
	if (io_reg32(cpu, off & ~3u, &l))
	{
		int s = 8 * (3 - (int)(off & 3));
		l = (l & ~(0xffu << s)) | ((uint32_t)data << s);
		io_wreg32(cpu, off & ~3u, l);
	}
}

static uint16_t io_read16(sh2_t *cpu, uint32_t off)
{
	uint16_t w;
	uint32_t l;
	if (io_reg16(cpu, off, &w))
		return w;
	if (io_reg32(cpu, off & ~3u, &l))
		return (uint16_t)((off & 2) ? l : (l >> 16));
	return (uint16_t)((io_read8(cpu, off) << 8) | io_read8(cpu, off + 1));
}

static void io_write16(sh2_t *cpu, uint32_t off, uint16_t data)
{
	uint32_t l;
	if (io_wreg16(cpu, off, data))
		return;
	if (io_reg32(cpu, off & ~3u, &l))
	{
		l = (off & 2) ? ((l & 0xffff0000u) | data) : ((l & 0x0000ffffu) | ((uint32_t)data << 16));
		io_wreg32(cpu, off & ~3u, l);
		return;
	}
	io_write8(cpu, off, (uint8_t)(data >> 8));
	io_write8(cpu, off + 1, (uint8_t)data);
}

static uint32_t io_read32(sh2_t *cpu, uint32_t off)
{
	uint32_t l;
	if (io_reg32(cpu, off, &l))
		return l;
	return ((uint32_t)io_read16(cpu, off) << 16) | io_read16(cpu, off + 2);
}

static void io_write32(sh2_t *cpu, uint32_t off, uint32_t data)
{
	if (io_wreg32(cpu, off, data))
		return;
	io_write16(cpu, off, (uint16_t)(data >> 16));
	io_write16(cpu, off + 2, (uint16_t)data);
}

uint32_t sh2_io_read(sh2_t *cpu, uint32_t address, int width)
{
	uint32_t off = address - SH2_IO_BASE;
	if (off >= 0x800)
		return 0;
	switch (width)
	{
	case 1: return io_read8(cpu, off);
	case 2: return io_read16(cpu, off);
	default: return io_read32(cpu, off);
	}
}

void sh2_io_write(sh2_t *cpu, uint32_t address, int width, uint32_t data)
{
	uint32_t off = address - SH2_IO_BASE;
	if (off >= 0x800)
		return;
	switch (width)
	{
	case 1: io_write8(cpu, off, (uint8_t)data); break;
	case 2: io_write16(cpu, off, (uint16_t)data); break;
	default: io_write32(cpu, off, data); break;
	}
}

/* ------------------------------------------------------------------ */
/* instruction execution                                              */
/* ------------------------------------------------------------------ */

static void branch(sh2_t *cpu, uint32_t target)
{
	cpu->delay_target = target;
	cpu->delay = true;
}

static int illegal(sh2_t *cpu)
{
	cpu->r[15] -= 4;
	sh2_mem_write32(cpu, cpu->r[15], cpu->sr);
	cpu->r[15] -= 4;
	sh2_mem_write32(cpu, cpu->r[15], cpu->pc - 2);
	cpu->pc = sh2_mem_read32(cpu, cpu->vbr + VEC_ILLEGAL * 4);
	return 5;
}

void sh2_mac_l(sh2_t *cpu, uint32_t a, uint32_t b)
{
	int64_t prod = (int64_t)(int32_t)a * (int64_t)(int32_t)b;
	uint64_t sum = (((uint64_t)cpu->mach << 32) | cpu->macl) + (uint64_t)prod;

	if (cpu->sr & SR_S)
	{
		if (sum > 0x00007fffffffffffULL && sum < 0xffff800000000000ULL)
			sum = ((a ^ b) & 0x80000000u) ? 0xffff800000000000ULL : 0x00007fffffffffffULL;
	}
	cpu->mach = (uint32_t)(sum >> 32);
	cpu->macl = (uint32_t)sum;
}

void sh2_mac_w(sh2_t *cpu, uint16_t a, uint16_t b)
{
	int32_t prod = (int32_t)(int16_t)a * (int32_t)(int16_t)b;
	if (cpu->sr & SR_S)
	{
		int64_t sum = (int64_t)(int32_t)cpu->macl + prod;
		if (sum > 0x7fffffffLL)
		{
			cpu->macl = 0x7fffffffu;
			cpu->mach |= 1;
		}
		else if (sum < -0x80000000LL)
		{
			cpu->macl = 0x80000000u;
			cpu->mach |= 1;
		}
		else
		{
			cpu->macl = (uint32_t)sum;
		}
	}
	else
	{
		int64_t mac = (int64_t)(((uint64_t)cpu->mach << 32) | cpu->macl);
		mac += prod;
		cpu->mach = (uint32_t)((uint64_t)mac >> 32);
		cpu->macl = (uint32_t)mac;
	}
}

void sh2_div1(sh2_t *cpu, int m, int n)
{
	uint32_t old_q = cpu->sr & SR_Q;
	uint32_t tmp0;
	uint32_t q, mm;
	int tmp1;

	q = (cpu->r[n] & 0x80000000u) ? SR_Q : 0;
	cpu->r[n] = (cpu->r[n] << 1) | (cpu->sr & SR_T);
	mm = cpu->sr & SR_M;

	if (!old_q)
	{
		if (!mm)
		{
			tmp0 = cpu->r[n];
			cpu->r[n] -= cpu->r[m];
			tmp1 = cpu->r[n] > tmp0;
			q = q ? (tmp1 ? 0 : SR_Q) : (tmp1 ? SR_Q : 0);
		}
		else
		{
			tmp0 = cpu->r[n];
			cpu->r[n] += cpu->r[m];
			tmp1 = cpu->r[n] < tmp0;
			q = q ? (tmp1 ? SR_Q : 0) : (tmp1 ? 0 : SR_Q);
		}
	}
	else
	{
		if (!mm)
		{
			tmp0 = cpu->r[n];
			cpu->r[n] += cpu->r[m];
			tmp1 = cpu->r[n] < tmp0;
			q = q ? (tmp1 ? 0 : SR_Q) : (tmp1 ? SR_Q : 0);
		}
		else
		{
			tmp0 = cpu->r[n];
			cpu->r[n] -= cpu->r[m];
			tmp1 = cpu->r[n] > tmp0;
			q = q ? (tmp1 ? SR_Q : 0) : (tmp1 ? 0 : SR_Q);
		}
	}

	cpu->sr = (cpu->sr & ~(SR_Q | SR_T)) | q;
	if ((q != 0) == (mm != 0))
		cpu->sr |= SR_T;
}

static int exec_0000(sh2_t *cpu, uint16_t op)
{
	int n = (op >> 8) & 15;
	int m = (op >> 4) & 15;

	switch (op & 0x3f)
	{
	case 0x02: cpu->r[n] = cpu->sr; return 1;
	case 0x12: cpu->r[n] = cpu->gbr; return 1;
	case 0x22: cpu->r[n] = cpu->vbr; return 1;

	case 0x03: cpu->pr = cpu->pc + 2; branch(cpu, cpu->pc + 2 + cpu->r[n]); return 2;
	case 0x23: branch(cpu, cpu->pc + 2 + cpu->r[n]); return 2;

	case 0x04: case 0x14: case 0x24: case 0x34:
		sh2_mem_write8(cpu, cpu->r[n] + cpu->r[0], (uint8_t)cpu->r[m]); return 1;
	case 0x05: case 0x15: case 0x25: case 0x35:
		sh2_mem_write16(cpu, cpu->r[n] + cpu->r[0], (uint16_t)cpu->r[m]); return 1;
	case 0x06: case 0x16: case 0x26: case 0x36:
		sh2_mem_write32(cpu, cpu->r[n] + cpu->r[0], cpu->r[m]); return 1;
	case 0x07: case 0x17: case 0x27: case 0x37:
		cpu->macl = cpu->r[n] * cpu->r[m]; return 2;

	case 0x08: cpu->sr &= ~SR_T; return 1;
	case 0x18: cpu->sr |= SR_T; return 1;
	case 0x28: cpu->mach = cpu->macl = 0; return 1;

	case 0x09: return 1;
	case 0x19: cpu->sr &= ~(SR_M | SR_Q | SR_T); return 1;
	case 0x29: cpu->r[n] = cpu->sr & SR_T; return 1;

	case 0x0a: cpu->r[n] = cpu->mach; return 1;
	case 0x1a: cpu->r[n] = cpu->macl; return 1;
	case 0x2a: cpu->r[n] = cpu->pr; return 1;

	case 0x0b: branch(cpu, cpu->pr); return 2;
	case 0x1b: cpu->sleeping = true; return 3;
	case 0x2b:
	{
		uint32_t target = sh2_mem_read32(cpu, cpu->r[15]);
		cpu->r[15] += 4;
		cpu->sr = sh2_mem_read32(cpu, cpu->r[15]) & SR_MASK;
		cpu->r[15] += 4;
		branch(cpu, target);
		return 4;
	}

	case 0x0c: case 0x1c: case 0x2c: case 0x3c:
		cpu->r[n] = (uint32_t)(int8_t)sh2_mem_read8(cpu, cpu->r[m] + cpu->r[0]); return 1;
	case 0x0d: case 0x1d: case 0x2d: case 0x3d:
		cpu->r[n] = (uint32_t)(int16_t)sh2_mem_read16(cpu, cpu->r[m] + cpu->r[0]); return 1;
	case 0x0e: case 0x1e: case 0x2e: case 0x3e:
		cpu->r[n] = sh2_mem_read32(cpu, cpu->r[m] + cpu->r[0]); return 1;
	case 0x0f: case 0x1f: case 0x2f: case 0x3f:
	{
		uint32_t a, b;
		a = sh2_mem_read32(cpu, cpu->r[n]); cpu->r[n] += 4;
		b = sh2_mem_read32(cpu, cpu->r[m]); cpu->r[m] += 4;
		sh2_mac_l(cpu, a, b);
		return 3;
	}
	default: return illegal(cpu);
	}
}

static int exec_0100(sh2_t *cpu, uint16_t op)
{
	int n = (op >> 8) & 15;
	int m = (op >> 4) & 15;
	uint32_t v;

	switch (op & 0x3f)
	{
	case 0x00: cpu->sr = (cpu->sr & ~SR_T) | (cpu->r[n] >> 31); cpu->r[n] <<= 1; return 1;
	case 0x01: cpu->sr = (cpu->sr & ~SR_T) | (cpu->r[n] & 1); cpu->r[n] >>= 1; return 1;
	case 0x04:
		cpu->sr = (cpu->sr & ~SR_T) | (cpu->r[n] >> 31);
		cpu->r[n] = (cpu->r[n] << 1) | (cpu->r[n] >> 31);
		return 1;
	case 0x05:
		cpu->sr = (cpu->sr & ~SR_T) | (cpu->r[n] & 1);
		cpu->r[n] = (cpu->r[n] >> 1) | (cpu->r[n] << 31);
		return 1;
	case 0x20: cpu->sr = (cpu->sr & ~SR_T) | (cpu->r[n] >> 31); cpu->r[n] <<= 1; return 1;
	case 0x21:
		cpu->sr = (cpu->sr & ~SR_T) | (cpu->r[n] & 1);
		cpu->r[n] = (uint32_t)(((int32_t)cpu->r[n]) >> 1);
		return 1;
	case 0x24:
		v = cpu->sr & SR_T;
		cpu->sr = (cpu->sr & ~SR_T) | (cpu->r[n] >> 31);
		cpu->r[n] = (cpu->r[n] << 1) | v;
		return 1;
	case 0x25:
		v = cpu->sr & SR_T;
		cpu->sr = (cpu->sr & ~SR_T) | (cpu->r[n] & 1);
		cpu->r[n] = (cpu->r[n] >> 1) | (v << 31);
		return 1;
	case 0x08: cpu->r[n] <<= 2; return 1;
	case 0x09: cpu->r[n] >>= 2; return 1;
	case 0x18: cpu->r[n] <<= 8; return 1;
	case 0x19: cpu->r[n] >>= 8; return 1;
	case 0x28: cpu->r[n] <<= 16; return 1;
	case 0x29: cpu->r[n] >>= 16; return 1;

	case 0x10:
		cpu->r[n]--;
		cpu->sr = (cpu->sr & ~SR_T) | (cpu->r[n] == 0);
		return 1;
	case 0x11:
		cpu->sr = (cpu->sr & ~SR_T) | ((int32_t)cpu->r[n] >= 0);
		return 1;
	case 0x15:
		cpu->sr = (cpu->sr & ~SR_T) | ((int32_t)cpu->r[n] > 0);
		return 1;

	case 0x02: cpu->r[n] -= 4; sh2_mem_write32(cpu, cpu->r[n], cpu->mach); return 1;
	case 0x12: cpu->r[n] -= 4; sh2_mem_write32(cpu, cpu->r[n], cpu->macl); return 1;
	case 0x22: cpu->r[n] -= 4; sh2_mem_write32(cpu, cpu->r[n], cpu->pr); return 1;
	case 0x03: cpu->r[n] -= 4; sh2_mem_write32(cpu, cpu->r[n], cpu->sr); return 2;
	case 0x13: cpu->r[n] -= 4; sh2_mem_write32(cpu, cpu->r[n], cpu->gbr); return 2;
	case 0x23: cpu->r[n] -= 4; sh2_mem_write32(cpu, cpu->r[n], cpu->vbr); return 2;

	case 0x06: cpu->mach = sh2_mem_read32(cpu, cpu->r[n]); cpu->r[n] += 4; return 1;
	case 0x16: cpu->macl = sh2_mem_read32(cpu, cpu->r[n]); cpu->r[n] += 4; return 1;
	case 0x26: cpu->pr = sh2_mem_read32(cpu, cpu->r[n]); cpu->r[n] += 4; return 1;
	case 0x07: cpu->sr = sh2_mem_read32(cpu, cpu->r[n]) & SR_MASK; cpu->r[n] += 4; return 3;
	case 0x17: cpu->gbr = sh2_mem_read32(cpu, cpu->r[n]); cpu->r[n] += 4; return 3;
	case 0x27: cpu->vbr = sh2_mem_read32(cpu, cpu->r[n]); cpu->r[n] += 4; return 3;

	case 0x0a: cpu->mach = cpu->r[n]; return 1;
	case 0x1a: cpu->macl = cpu->r[n]; return 1;
	case 0x2a: cpu->pr = cpu->r[n]; return 1;
	case 0x0e: cpu->sr = cpu->r[n] & SR_MASK; return 1;
	case 0x1e: cpu->gbr = cpu->r[n]; return 1;
	case 0x2e: cpu->vbr = cpu->r[n]; return 1;

	case 0x0b: cpu->pr = cpu->pc + 2; branch(cpu, cpu->r[n]); return 2;
	case 0x2b: branch(cpu, cpu->r[n]); return 2;

	case 0x1b:
	{
		uint8_t b = sh2_mem_read8(cpu, cpu->r[n]);
		cpu->sr = (cpu->sr & ~SR_T) | (b == 0);
		sh2_mem_write8(cpu, cpu->r[n], (uint8_t)(b | 0x80));
		return 4;
	}

	case 0x0f: case 0x1f: case 0x2f: case 0x3f:
	{
		uint16_t a, b;
		a = sh2_mem_read16(cpu, cpu->r[n]); cpu->r[n] += 2;
		b = sh2_mem_read16(cpu, cpu->r[m]); cpu->r[m] += 2;
		sh2_mac_w(cpu, a, b);
		return 3;
	}
	default: return illegal(cpu);
	}
}

static int exec_one_op(sh2_t *cpu, uint16_t op)
{
	int n = (op >> 8) & 15;
	int m = (op >> 4) & 15;
	uint32_t a, b, r;

	switch (op >> 12)
	{
	case 0x0: return exec_0000(cpu, op);

	case 0x1:
		sh2_mem_write32(cpu, cpu->r[n] + (uint32_t)(op & 15) * 4, cpu->r[m]);
		return 1;

	case 0x2:
		switch (op & 15)
		{
		case 0: sh2_mem_write8(cpu, cpu->r[n], (uint8_t)cpu->r[m]); return 1;
		case 1: sh2_mem_write16(cpu, cpu->r[n], (uint16_t)cpu->r[m]); return 1;
		case 2: sh2_mem_write32(cpu, cpu->r[n], cpu->r[m]); return 1;
		case 4: a = cpu->r[m]; cpu->r[n] -= 1; sh2_mem_write8(cpu, cpu->r[n], (uint8_t)a); return 1;
		case 5: a = cpu->r[m]; cpu->r[n] -= 2; sh2_mem_write16(cpu, cpu->r[n], (uint16_t)a); return 1;
		case 6: a = cpu->r[m]; cpu->r[n] -= 4; sh2_mem_write32(cpu, cpu->r[n], a); return 1;
		case 7:
			cpu->sr &= ~(SR_Q | SR_M | SR_T);
			if (cpu->r[n] & 0x80000000u) cpu->sr |= SR_Q;
			if (cpu->r[m] & 0x80000000u) cpu->sr |= SR_M;
			if ((cpu->r[n] ^ cpu->r[m]) & 0x80000000u) cpu->sr |= SR_T;
			return 1;
		case 8: cpu->sr = (cpu->sr & ~SR_T) | ((cpu->r[n] & cpu->r[m]) == 0); return 1;
		case 9: cpu->r[n] &= cpu->r[m]; return 1;
		case 10: cpu->r[n] ^= cpu->r[m]; return 1;
		case 11: cpu->r[n] |= cpu->r[m]; return 1;
		case 12:
			r = cpu->r[n] ^ cpu->r[m];
			cpu->sr &= ~SR_T;
			if (!(r & 0xff000000u) || !(r & 0x00ff0000u) || !(r & 0x0000ff00u) || !(r & 0xffu))
				cpu->sr |= SR_T;
			return 1;
		case 13: cpu->r[n] = (cpu->r[m] << 16) | (cpu->r[n] >> 16); return 1;
		case 14: cpu->macl = (uint32_t)((cpu->r[n] & 0xffffu) * (cpu->r[m] & 0xffffu)); return 1;
		case 15: cpu->macl = (uint32_t)((int32_t)(int16_t)cpu->r[n] * (int32_t)(int16_t)cpu->r[m]); return 1;
		default: return illegal(cpu);
		}

	case 0x3:
		switch (op & 15)
		{
		case 0: cpu->sr = (cpu->sr & ~SR_T) | (cpu->r[n] == cpu->r[m]); return 1;
		case 2: cpu->sr = (cpu->sr & ~SR_T) | (cpu->r[n] >= cpu->r[m]); return 1;
		case 3: cpu->sr = (cpu->sr & ~SR_T) | ((int32_t)cpu->r[n] >= (int32_t)cpu->r[m]); return 1;
		case 4: sh2_div1(cpu, m, n); return 1;
		case 5:
		{
			uint64_t p = (uint64_t)cpu->r[n] * (uint64_t)cpu->r[m];
			cpu->mach = (uint32_t)(p >> 32); cpu->macl = (uint32_t)p;
			return 2;
		}
		case 6: cpu->sr = (cpu->sr & ~SR_T) | (cpu->r[n] > cpu->r[m]); return 1;
		case 7: cpu->sr = (cpu->sr & ~SR_T) | ((int32_t)cpu->r[n] > (int32_t)cpu->r[m]); return 1;
		case 8: cpu->r[n] -= cpu->r[m]; return 1;
		case 10:
		{
			uint64_t d = (uint64_t)cpu->r[n] - cpu->r[m] - (cpu->sr & SR_T);
			cpu->r[n] = (uint32_t)d;
			cpu->sr = (cpu->sr & ~SR_T) | (uint32_t)((d >> 32) & 1);
			return 1;
		}
		case 11:
			a = cpu->r[n]; b = cpu->r[m];
			r = a - b;
			cpu->sr &= ~SR_T;
			if (((a ^ b) & (a ^ r) & 0x80000000u) != 0)
				cpu->sr |= SR_T;
			cpu->r[n] = r;
			return 1;
		case 12: cpu->r[n] += cpu->r[m]; return 1;
		case 13:
		{
			int64_t p = (int64_t)(int32_t)cpu->r[n] * (int64_t)(int32_t)cpu->r[m];
			cpu->mach = (uint32_t)((uint64_t)p >> 32); cpu->macl = (uint32_t)p;
			return 2;
		}
		case 14:
		{
			uint64_t sum = (uint64_t)cpu->r[n] + cpu->r[m] + (cpu->sr & SR_T);
			cpu->r[n] = (uint32_t)sum;
			cpu->sr = (cpu->sr & ~SR_T) | (uint32_t)((sum >> 32) & 1);
			return 1;
		}
		case 15:
			a = cpu->r[n]; b = cpu->r[m];
			r = a + b;
			cpu->sr &= ~SR_T;
			if ((~(a ^ b) & (a ^ r) & 0x80000000u) != 0)
				cpu->sr |= SR_T;
			cpu->r[n] = r;
			return 1;
		default: return illegal(cpu);
		}

	case 0x4: return exec_0100(cpu, op);

	case 0x5:
		cpu->r[n] = sh2_mem_read32(cpu, cpu->r[m] + (uint32_t)(op & 15) * 4);
		return 1;

	case 0x6:
		switch (op & 15)
		{
		case 0: cpu->r[n] = (uint32_t)(int8_t)sh2_mem_read8(cpu, cpu->r[m]); return 1;
		case 1: cpu->r[n] = (uint32_t)(int16_t)sh2_mem_read16(cpu, cpu->r[m]); return 1;
		case 2: cpu->r[n] = sh2_mem_read32(cpu, cpu->r[m]); return 1;
		case 3: cpu->r[n] = cpu->r[m]; return 1;
		case 4:
			cpu->r[n] = (uint32_t)(int8_t)sh2_mem_read8(cpu, cpu->r[m]);
			if (n != m) cpu->r[m] += 1;
			return 1;
		case 5:
			cpu->r[n] = (uint32_t)(int16_t)sh2_mem_read16(cpu, cpu->r[m]);
			if (n != m) cpu->r[m] += 2;
			return 1;
		case 6:
			cpu->r[n] = sh2_mem_read32(cpu, cpu->r[m]);
			if (n != m) cpu->r[m] += 4;
			return 1;
		case 7: cpu->r[n] = ~cpu->r[m]; return 1;
		case 8:
			cpu->r[n] = (cpu->r[m] & 0xffff0000u) | ((cpu->r[m] & 0xffu) << 8) | ((cpu->r[m] >> 8) & 0xffu);
			return 1;
		case 9: cpu->r[n] = (cpu->r[m] << 16) | (cpu->r[m] >> 16); return 1;
		case 10:
		{
			uint64_t d = (uint64_t)0 - cpu->r[m] - (cpu->sr & SR_T);
			cpu->r[n] = (uint32_t)d;
			cpu->sr = (cpu->sr & ~SR_T) | (uint32_t)((d >> 32) & 1);
			return 1;
		}
		case 11: cpu->r[n] = 0 - cpu->r[m]; return 1;
		case 12: cpu->r[n] = cpu->r[m] & 0xffu; return 1;
		case 13: cpu->r[n] = cpu->r[m] & 0xffffu; return 1;
		case 14: cpu->r[n] = (uint32_t)(int8_t)cpu->r[m]; return 1;
		case 15: cpu->r[n] = (uint32_t)(int16_t)cpu->r[m]; return 1;
		default: return illegal(cpu);
		}

	case 0x7:
		cpu->r[n] += (uint32_t)(int8_t)(op & 0xff);
		return 1;

	case 0x8:
		switch ((op >> 8) & 15)
		{
		case 0: sh2_mem_write8(cpu, cpu->r[m] + (uint32_t)(op & 15), (uint8_t)cpu->r[0]); return 1;
		case 1: sh2_mem_write16(cpu, cpu->r[m] + (uint32_t)(op & 15) * 2, (uint16_t)cpu->r[0]); return 1;
		case 4: cpu->r[0] = (uint32_t)(int8_t)sh2_mem_read8(cpu, cpu->r[m] + (uint32_t)(op & 15)); return 1;
		case 5: cpu->r[0] = (uint32_t)(int16_t)sh2_mem_read16(cpu, cpu->r[m] + (uint32_t)(op & 15) * 2); return 1;
		case 8: cpu->sr = (cpu->sr & ~SR_T) | (cpu->r[0] == (uint32_t)(int8_t)(op & 0xff)); return 1;
		case 9:
			if (cpu->sr & SR_T) { cpu->pc = cpu->pc + 2 + (uint32_t)((int32_t)(int8_t)(op & 0xff) * 2); return 3; }
			return 1;
		case 11:
			if (!(cpu->sr & SR_T)) { cpu->pc = cpu->pc + 2 + (uint32_t)((int32_t)(int8_t)(op & 0xff) * 2); return 3; }
			return 1;
		case 13:
			if (cpu->sr & SR_T) { branch(cpu, cpu->pc + 2 + (uint32_t)((int32_t)(int8_t)(op & 0xff) * 2)); return 2; }
			return 1;
		case 15:
			if (!(cpu->sr & SR_T)) { branch(cpu, cpu->pc + 2 + (uint32_t)((int32_t)(int8_t)(op & 0xff) * 2)); return 2; }
			return 1;
		default: return illegal(cpu);
		}

	case 0x9:
		cpu->r[n] = (uint32_t)(int16_t)sh2_mem_read16(cpu, cpu->pc + 2 + (uint32_t)(op & 0xff) * 2);
		return 1;

	case 0xa:
		branch(cpu, cpu->pc + 2 + (uint32_t)(((int32_t)(op << 20) >> 20) * 2));
		return 2;

	case 0xb:
		cpu->pr = cpu->pc + 2;
		branch(cpu, cpu->pc + 2 + (uint32_t)(((int32_t)(op << 20) >> 20) * 2));
		return 2;

	case 0xc:
		switch ((op >> 8) & 15)
		{
		case 0: sh2_mem_write8(cpu, cpu->gbr + (uint32_t)(op & 0xff), (uint8_t)cpu->r[0]); return 1;
		case 1: sh2_mem_write16(cpu, cpu->gbr + (uint32_t)(op & 0xff) * 2, (uint16_t)cpu->r[0]); return 1;
		case 2: sh2_mem_write32(cpu, cpu->gbr + (uint32_t)(op & 0xff) * 4, cpu->r[0]); return 1;
		case 3:
			cpu->r[15] -= 4;
			sh2_mem_write32(cpu, cpu->r[15], cpu->sr);
			cpu->r[15] -= 4;
			sh2_mem_write32(cpu, cpu->r[15], cpu->pc);
			cpu->pc = sh2_mem_read32(cpu, cpu->vbr + (uint32_t)(op & 0xff) * 4);
			return 8;
		case 4: cpu->r[0] = (uint32_t)(int8_t)sh2_mem_read8(cpu, cpu->gbr + (uint32_t)(op & 0xff)); return 1;
		case 5: cpu->r[0] = (uint32_t)(int16_t)sh2_mem_read16(cpu, cpu->gbr + (uint32_t)(op & 0xff) * 2); return 1;
		case 6: cpu->r[0] = sh2_mem_read32(cpu, cpu->gbr + (uint32_t)(op & 0xff) * 4); return 1;
		case 7: cpu->r[0] = ((cpu->pc + 2) & ~3u) + (uint32_t)(op & 0xff) * 4; return 1;
		case 8: cpu->sr = (cpu->sr & ~SR_T) | ((cpu->r[0] & (uint32_t)(op & 0xff)) == 0); return 1;
		case 9: cpu->r[0] &= (uint32_t)(op & 0xff); return 1;
		case 10: cpu->r[0] ^= (uint32_t)(op & 0xff); return 1;
		case 11: cpu->r[0] |= (uint32_t)(op & 0xff); return 1;
		case 12:
			a = cpu->gbr + cpu->r[0];
			cpu->sr = (cpu->sr & ~SR_T) | ((sh2_mem_read8(cpu, a) & (uint8_t)op) == 0);
			return 3;
		case 13:
			a = cpu->gbr + cpu->r[0];
			sh2_mem_write8(cpu, a, (uint8_t)(sh2_mem_read8(cpu, a) & (uint8_t)op));
			return 3;
		case 14:
			a = cpu->gbr + cpu->r[0];
			sh2_mem_write8(cpu, a, (uint8_t)(sh2_mem_read8(cpu, a) ^ (uint8_t)op));
			return 3;
		default:
			a = cpu->gbr + cpu->r[0];
			sh2_mem_write8(cpu, a, (uint8_t)(sh2_mem_read8(cpu, a) | (uint8_t)op));
			return 3;
		}

	case 0xd:
		cpu->r[n] = sh2_mem_read32(cpu, ((cpu->pc + 2) & ~3u) + (uint32_t)(op & 0xff) * 4);
		return 1;

	case 0xe:
		cpu->r[n] = (uint32_t)(int8_t)(op & 0xff);
		return 1;

	default:
		return illegal(cpu);
	}
}

int sh2_exec_one(sh2_t *cpu)
{
	uint16_t op = sh2_mem_read16(cpu, cpu->pc);
	if (cpu->delay)
	{
		cpu->pc = cpu->delay_target;
		cpu->delay = false;
	}
	else
	{
		cpu->pc += 2;
	}
	return exec_one_op(cpu, op);
}

/* ------------------------------------------------------------------ */

void sh2_init(sh2_t *cpu, const sh2_bus_t *bus)
{
	memset(cpu, 0, sizeof(*cpu));
	cpu->bus = *bus;
}

void sh2_map(sh2_t *cpu, uint32_t base, uint32_t size, uint8_t *data, bool writable)
{
	sh2_region_t *r;
	if (cpu->region_count >= SH2_MAX_REGIONS)
		return;
	r = &cpu->regions[cpu->region_count++];
	r->base = base;
	r->size = size;
	r->data = data;
	r->writable = writable;
}

void sh2_reset(sh2_t *cpu)
{
	int i;

	memset(cpu->r, 0, sizeof(cpu->r));
	cpu->sr = SR_I;
	cpu->gbr = cpu->vbr = cpu->mach = cpu->macl = cpu->pr = 0;
	cpu->delay = false;
	cpu->delay_target = 0;
	cpu->sleeping = false;

	memset(cpu->ipr, 0, sizeof(cpu->ipr));
	cpu->icr = cpu->isr = 0;
	memset(cpu->pending, 0, sizeof(cpu->pending));
	cpu->irq_line = 0;
	cpu->nmi_line = cpu->nmi_pending = false;

	memset(cpu->sci, 0, sizeof(cpu->sci));
	for (i = 0; i < 2; i++)
	{
		cpu->sci[i].brr = 0xff;
		cpu->sci[i].tdr = 0xff;
		cpu->sci[i].ssr = (uint8_t)(SSR_TDRE | SSR_TEND);
	}

	memset(cpu->mtu, 0, sizeof(cpu->mtu));
	for (i = 0; i < 3; i++)
		cpu->mtu[i].tsr = 0x80;
	cpu->tsyr = 0;

	cpu->wtcsr = 0x18;
	cpu->wtcnt = 0;
	cpu->rstcsr = 0x1f;
	cpu->wdt_prescale = 0;
	cpu->wdt_int = false;

	memset(cpu->addr_, 0, sizeof(cpu->addr_));
	cpu->adcsr = 0;
	cpu->adcr = 0x7f;
	cpu->adc_timer = 0;
	cpu->adc_int = false;

	memset(cpu->dma, 0, sizeof(cpu->dma));
	cpu->dmaor = 0;

	cpu->padr = cpu->paior = cpu->pacr1 = cpu->pacr2 = 0;
	cpu->pbdr = cpu->pbior = cpu->pbcr1 = cpu->pbcr2 = 0;
	cpu->pedr = cpu->peior = cpu->pecr1 = cpu->pecr2 = 0;
	for (i = 0; i < 3; i++)
		cpu->port_out[i] = 0xffff;

	cpu->bcr1 = 0;
	cpu->bcr2 = 0xffff;
	cpu->wcr1 = 0xffff;
	cpu->wcr2 = 0x000f;
	cpu->dcr = cpu->rtcsr = cpu->rtcnt = cpu->rtcor = 0;
	cpu->ccr = 0;

	memset(cpu->ram, 0, sizeof(cpu->ram));
	cpu->cycles = 0;

	for (i = 0; i < 3; i++)
		port_update(cpu, i, 0, 0);

	cpu->pc = sh2_mem_read32(cpu, 0);
	cpu->r[15] = sh2_mem_read32(cpu, 4);
}

int sh2_step(sh2_t *cpu)
{
	int cycles;

	if (!cpu->delay)
	{
		int level;
		int vector = sh2_irq_select(cpu, &level);
		if (vector >= 0)
		{
			sh2_take_interrupt(cpu, vector, level);
			cycles = 8;
			cpu->cycles += (uint64_t)cycles;
			sh2_peripherals_tick(cpu, (uint32_t)cycles);
			return cycles;
		}
	}

	if (cpu->sleeping)
	{
		cpu->cycles++;
		sh2_peripherals_tick(cpu, 1);
		return 1;
	}

	cycles = sh2_exec_one(cpu);
	if (cpu->delay)
		cycles += sh2_exec_one(cpu);
	if (cycles < 1)
		cycles = 1;
	cpu->cycles += (uint64_t)cycles;
	sh2_peripherals_tick(cpu, (uint32_t)cycles);
	return cycles;
}

int sh2_run(sh2_t *cpu, int cycles)
{
	int ran = 0;
	if (cpu->jit && cpu->jit_enabled)
		return sh2_jit_run(cpu, cycles);
	while (ran < cycles)
		ran += sh2_step(cpu);
	return ran;
}

/* ---------------------------------------------------------------- the state */

static bool state_restored(void *user)
{
	sh2_t *cpu = user;
	cpu->irq_ready = false;
	cpu->jit_pending = 0;
	cpu->jit_limit = 0;
	cpu->jit_deadline = 0;
	return true;
}

void sh2_state(sh2_t *cpu, state_registry_t *reg)
{
	state_array(reg, cpu->r);
	state_var(reg, cpu->sr);
	state_var(reg, cpu->gbr);
	state_var(reg, cpu->vbr);
	state_var(reg, cpu->mach);
	state_var(reg, cpu->macl);
	state_var(reg, cpu->pr);
	state_var(reg, cpu->pc);
	state_var(reg, cpu->delay_target);
	state_bool(reg, cpu->delay);
	state_bool(reg, cpu->sleeping);

	state_array(reg, cpu->ipr);
	state_var(reg, cpu->icr);
	state_var(reg, cpu->isr);
	state_array(reg, cpu->pending);
	state_var(reg, cpu->irq_line);
	state_bool(reg, cpu->nmi_line);
	state_bool(reg, cpu->nmi_pending);

	state_field(reg, cpu->sci, 2, smr);
	state_field(reg, cpu->sci, 2, brr);
	state_field(reg, cpu->sci, 2, scr);
	state_field(reg, cpu->sci, 2, tdr);
	state_field(reg, cpu->sci, 2, ssr);
	state_field(reg, cpu->sci, 2, rdr);
	state_field(reg, cpu->sci, 2, tx_shift);
	state_field(reg, cpu->sci, 2, tx_timer);
	state_bool_field(reg, cpu->sci, 2, tx_busy);
	state_field(reg, cpu->sci, 2, rx_byte);
	state_bool_field(reg, cpu->sci, 2, rx_pending);
	state_bool_field(reg, cpu->sci, 2, int_rxi);
	state_bool_field(reg, cpu->sci, 2, int_txi);
	state_bool_field(reg, cpu->sci, 2, int_tei);
	state_bool_field(reg, cpu->sci, 2, int_eri);

	state_field(reg, cpu->mtu, 3, tcr);
	state_field(reg, cpu->mtu, 3, tmdr);
	state_field(reg, cpu->mtu, 3, tiorh);
	state_field(reg, cpu->mtu, 3, tiorl);
	state_field(reg, cpu->mtu, 3, tier);
	state_field(reg, cpu->mtu, 3, tsr);
	state_field(reg, cpu->mtu, 3, tcnt);
	for (int n = 0; n < 4; n++)
		state_field(reg, cpu->mtu, 3, tgr[n]);
	state_field(reg, cpu->mtu, 3, prescale);
	state_bool_field(reg, cpu->mtu, 3, active);
	state_var(reg, cpu->tsyr);

	state_var(reg, cpu->wtcsr);
	state_var(reg, cpu->wtcnt);
	state_var(reg, cpu->rstcsr);
	state_var(reg, cpu->wdt_prescale);
	state_bool(reg, cpu->wdt_int);

	state_array(reg, cpu->addr_);
	state_var(reg, cpu->adcsr);
	state_var(reg, cpu->adcr);
	state_var(reg, cpu->adc_timer);
	state_bool(reg, cpu->adc_int);

	state_field(reg, cpu->dma, 2, sar);
	state_field(reg, cpu->dma, 2, dar);
	state_field(reg, cpu->dma, 2, chcr);
	state_field(reg, cpu->dma, 2, dmatcr);
	state_field(reg, cpu->dma, 2, count);
	state_field(reg, cpu->dma, 2, timer);
	state_bool_field(reg, cpu->dma, 2, active);
	state_bool_field(reg, cpu->dma, 2, int_te);
	state_var(reg, cpu->dmaor);

	state_var(reg, cpu->padr);
	state_var(reg, cpu->paior);
	state_var(reg, cpu->pacr1);
	state_var(reg, cpu->pacr2);
	state_var(reg, cpu->pbdr);
	state_var(reg, cpu->pbior);
	state_var(reg, cpu->pbcr1);
	state_var(reg, cpu->pbcr2);
	state_var(reg, cpu->pedr);
	state_var(reg, cpu->peior);
	state_var(reg, cpu->pecr1);
	state_var(reg, cpu->pecr2);
	state_array(reg, cpu->port_out);

	state_var(reg, cpu->bcr1);
	state_var(reg, cpu->bcr2);
	state_var(reg, cpu->wcr1);
	state_var(reg, cpu->wcr2);
	state_var(reg, cpu->dcr);
	state_var(reg, cpu->rtcsr);
	state_var(reg, cpu->rtcnt);
	state_var(reg, cpu->rtcor);
	state_var(reg, cpu->ccr);

	state_array(reg, cpu->ram);
	state_var(reg, cpu->cycles);
	state_after_load(reg, state_restored, cpu);
}
