#include <string.h>
#include "h8500.h"
#include "h8500_jit.h"
#include "state.h"

#define FLAG_C 0x0001u
#define FLAG_V 0x0002u
#define FLAG_Z 0x0004u
#define FLAG_N 0x0008u
#define SR_T   0x8000u
#define SR_I   0x0700u
#define SR_MASK 0x870fu

#define IO(a) ((a) - H8500_IO_BASE)

enum
{
	VEC_INVALID = 2,
	VEC_DIVZERO = 3,
	VEC_TRAPV = 4,
	VEC_NMI = 11,
	VEC_TRAPA = 16
};

static void wdt_write16(h8500_t *cpu, uint16_t data);

#define mem_read8 h8500_mem_read8
#define mem_write8 h8500_mem_write8
#define mem_read16 h8500_mem_read16
#define mem_write16 h8500_mem_write16
#define peripherals_tick h8500_peripherals_tick
#define irq_select h8500_irq_select
#define exception h8500_exception
#define take_interrupt h8500_take_interrupt
#define exec_one h8500_exec_one

const uint8_t h8500_cyc_src[10] = { 2, 5, 5, 6, 5, 6, 5, 6, 3, 4 };
const uint8_t h8500_cyc_rmw[10] = { 2, 7, 7, 8, 7, 8, 7, 8, 3, 4 };
#define cyc_src h8500_cyc_src
#define cyc_rmw h8500_cyc_rmw

/* ------------------------------------------------------------------ */
/* memory                                                             */
/* ------------------------------------------------------------------ */

/* Returns 1 when the access lies in a mapped region: *ptr is then the byte
 * for a read, or NULL for a write into a read-only region. */
static int region_find(h8500_t *cpu, uint32_t addr, int len, uint8_t **ptr)
{
	int i;
	for (i = 0; i < cpu->region_count; i++)
	{
		const h8500_region_t *r = &cpu->regions[i];
		uint32_t off = addr - r->base;
		if (off < r->size && off + (uint32_t)len <= r->size)
		{
			*ptr = r->data + off;
			return r->writable ? 1 : 2;
		}
	}
	*ptr = NULL;
	return 0;
}

/* the register file, and the on-chip RAM below it where the part has one */
static int is_internal(h8500_t *cpu, uint32_t addr)
{
	const h8500_variant_t *v = cpu->var;
	return addr - v->ram_base < (uint32_t)(v->ram_size + v->io_size);
}

static int is_iram(h8500_t *cpu, uint32_t addr)
{
	return addr < cpu->var->io_base;
}

uint8_t h8500_mem_read8(h8500_t *cpu, uint32_t addr)
{
	uint8_t *p;
	addr &= cpu->var->addr_mask;
	if (is_internal(cpu, addr))
		return is_iram(cpu, addr) ? cpu->iram[addr - cpu->var->ram_base]
		                          : h8500_io_read(cpu, (uint16_t)addr);
	if (region_find(cpu, addr, 1, &p))
		return *p;
	return cpu->bus.read8 ? cpu->bus.read8(cpu->bus.user, addr) : 0xff;
}

void h8500_mem_write8(h8500_t *cpu, uint32_t addr, uint8_t data)
{
	uint8_t *p;
	addr &= cpu->var->addr_mask;
	if (is_internal(cpu, addr))
	{
		if (is_iram(cpu, addr))
			cpu->iram[addr - cpu->var->ram_base] = data;
		else
			h8500_io_write(cpu, (uint16_t)addr, data);
		return;
	}
	switch (region_find(cpu, addr, 1, &p))
	{
	case 1: *p = data; return;
	case 2: return;
	default: break;
	}
	if (cpu->bus.write8)
		cpu->bus.write8(cpu->bus.user, addr, data);
}

uint16_t h8500_mem_read16(h8500_t *cpu, uint32_t addr)
{
	uint8_t *p;
	addr &= cpu->var->addr_mask;
	if (addr & 1)
		return (uint16_t)((mem_read8(cpu, addr) << 8) | mem_read8(cpu, addr + 1));
	if (is_internal(cpu, addr))
	{
		if (is_iram(cpu, addr))
			return (uint16_t)((cpu->iram[addr - cpu->var->ram_base] << 8) |
			                  cpu->iram[addr + 1 - cpu->var->ram_base]);
		return (uint16_t)((h8500_io_read(cpu, (uint16_t)addr) << 8) |
		                  h8500_io_read(cpu, (uint16_t)(addr + 1)));
	}
	if (region_find(cpu, addr, 2, &p))
		return (uint16_t)((p[0] << 8) | p[1]);
	if (cpu->var->bus8)
		return (uint16_t)((mem_read8(cpu, addr) << 8) | mem_read8(cpu, addr + 1));
	if (cpu->bus.read16)
		return cpu->bus.read16(cpu->bus.user, addr);
	return 0xffff;
}

void h8500_mem_write16(h8500_t *cpu, uint32_t addr, uint16_t data)
{
	uint8_t *p;
	addr &= cpu->var->addr_mask;
	if (addr & 1)
	{
		mem_write8(cpu, addr, (uint8_t)(data >> 8));
		mem_write8(cpu, addr + 1, (uint8_t)data);
		return;
	}
	if (is_internal(cpu, addr))
	{
		if (is_iram(cpu, addr))
		{
			cpu->iram[addr - cpu->var->ram_base] = (uint8_t)(data >> 8);
			cpu->iram[addr + 1 - cpu->var->ram_base] = (uint8_t)data;
		}
		else if (cpu->var->wdt_reg >= 0 &&
		         addr == (uint32_t)(cpu->var->io_base + cpu->var->wdt_reg))
			wdt_write16(cpu, data);
		else
		{
			h8500_io_write(cpu, (uint16_t)addr, (uint8_t)(data >> 8));
			h8500_io_write(cpu, (uint16_t)(addr + 1), (uint8_t)data);
		}
		return;
	}
	switch (region_find(cpu, addr, 2, &p))
	{
	case 1:
		p[0] = (uint8_t)(data >> 8);
		p[1] = (uint8_t)data;
		return;
	case 2:
		return;
	default:
		break;
	}
	if (cpu->var->bus8)
	{
		mem_write8(cpu, addr, (uint8_t)(data >> 8));
		mem_write8(cpu, addr + 1, (uint8_t)data);
		return;
	}
	if (cpu->bus.write16)
		cpu->bus.write16(cpu->bus.user, addr, data);
}

static uint8_t fetch8(h8500_t *cpu)
{
	uint8_t v = mem_read8(cpu, ((uint32_t)cpu->cp << 16) | cpu->pc);
	cpu->pc++;
	return v;
}

static uint16_t fetch16(h8500_t *cpu)
{
	uint16_t hi = fetch8(cpu);
	return (uint16_t)((hi << 8) | fetch8(cpu));
}

static void push16(h8500_t *cpu, uint16_t v)
{
	cpu->r[7] = (uint16_t)(cpu->r[7] - 2);
	mem_write16(cpu, ((uint32_t)cpu->tp << 16) | cpu->r[7], v);
}

static uint16_t pop16(h8500_t *cpu)
{
	uint16_t v = mem_read16(cpu, ((uint32_t)cpu->tp << 16) | cpu->r[7]);
	cpu->r[7] = (uint16_t)(cpu->r[7] + 2);
	return v;
}

/* ------------------------------------------------------------------ */
/* peripherals                                                        */
/* ------------------------------------------------------------------ */

enum
{
	R_P1DDR = 0x00, R_P2DDR, R_P1DR, R_P2DR,
	R_P3DDR = 0x04, R_P4DDR, R_P3DR, R_P4DR,
	R_P5DDR = 0x08, R_P6DDR, R_P5DR, R_P6DR,
	R_P8DDR = 0x0d, R_P7DR, R_P8DR,
	R_ADDR0 = 0x10,
	R_ADCSR = 0x18, R_ADCR,
	R_FRT1  = 0x20,
	R_FRT2  = 0x30,
	R_TMRCR = 0x40, R_TMRCSR, R_TCORA, R_TCORB, R_TCNT,
	R_SCI1  = 0x48,
	R_SCI2  = 0x50,
	R_IPRA  = 0x80, R_IPRB, R_IPRC, R_IPRD,
	R_DTEA  = 0x88,
	R_WDT   = 0x90,
	R_NMICR = 0x9c, R_IRQCR
};

/* H8/532 register file, offsets from FF80 */
enum
{
	M_P1DDR = 0x00, M_P2DDR, M_P1DR, M_P2DR,
	M_P4DDR = 0x05, M_P3DR, M_P4DR,
	M_P5DDR = 0x08, M_P6DDR, M_P5DR, M_P6DR,
	M_P7DDR = 0x0c, M_P7DR = 0x0e, M_P8DR,
	M_FRT1  = 0x10,
	M_FRT2  = 0x20,
	M_FRT3  = 0x30,
	M_TMRCR = 0x50,
	M_SCI1  = 0x58,
	M_ADDR0 = 0x60,
	M_WDT   = 0x6c,
	M_IPRA  = 0x70,
	M_DTEA  = 0x74,
	M_P1CR  = 0x7c,
	M_P9DDR = 0x7e, M_P9DR
};

/* FRT sub-offsets */
enum { F_TCR = 0, F_TCSR, F_FRCH, F_FRCL, F_OCRAH, F_OCRAL, F_OCRBH, F_OCRBL, F_ICRH, F_ICRL };
/* 8-bit timer sub-offsets */
enum { T_TCR = 0, T_TCSR, T_TCORA, T_TCORB, T_TCNT };
/* SCI sub-offsets */
enum { S_SMR = 0, S_BRR, S_SCR, S_TDR, S_SSR, S_RDR };
/* A/D sub-offsets */
enum { A_ADDR0 = 0, A_ADCSR = 8, A_ADCR };
/* watchdog sub-offsets */
enum { W_TCSR = 0, W_TCNT };

/* which module owns a byte of the register file */
enum
{
	IOK_PLAIN = 0, IOK_PORT_DDR, IOK_PORT_DR,
	IOK_FRT, IOK_TMR, IOK_SCI, IOK_ADC, IOK_WDT, IOK_IPR, IOK_CTL
};

/* SSR bits */
#define SSR_TDRE 0x80
#define SSR_RDRF 0x40
#define SSR_ORER 0x20
#define SSR_FER  0x10
#define SSR_PER  0x08
#define SSR_TEND 0x04

static const h8500_variant_t variant_h8510 =
{
	.addr_mask = 0xffffffu,
	.io_base = 0xfe80, .io_size = 0x180,
	.ram_base = 0xfe80, .ram_size = 0,
	.bus8 = false,

	.port_count = 8,
	.port_ddr = { -1, R_P1DDR, R_P2DDR, R_P3DDR, R_P4DDR, R_P5DDR, R_P6DDR, -1, R_P8DDR, -1 },
	.port_dr  = { -1, R_P1DR, R_P2DR, R_P3DR, R_P4DR, R_P5DR, R_P6DR, R_P7DR, R_P8DR, -1 },
	.port_ddr_fixed = { 0, 0, 0, 0, 0, 0, 0, 0xff, 0, 0 },
	.port_ddr_reset = { 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0, 0x00, 0 },
	.port_mask = { 0, 0, 0, 0, 0, 0, 0, 0xf0, 0, 0 },

	.frt_count = 2,
	.frt_reg = { R_FRT1, R_FRT2 },
	.frt_vector = { 40, 44 },
	.frt_temp = false,

	.tmr_reg = R_TMRCR, .tmr_vector = 48,

	.sci_count = 2,
	.sci_reg = { R_SCI1, R_SCI2 },
	.sci_vector = { 52, 56 },

	.adc_reg = R_ADDR0, .adc_vector = 60,

	.wdt_reg = R_WDT, .wdt_vector = 33,

	.ipr_reg = R_IPRA,

	.irq_pin_count = 4,
	.irq_vector = { 32, 36, 37, 38 },
	.ctl_reg = { R_NMICR, R_IRQCR },
	.ctl_write = { 0x01, 0x0f },
	.ctl_read = { 0xfe, 0xf0 },
	.irq_ctl = 1, .irq_ctl_shift = 0,
	.nmi_ctl = 0, .nmi_ctl_bit = 0,

	.vector_slot =
	{
		-1, -1, -1, -1, -1, -1, -1, -1,
		-1, -1, -1, -1, -1, -1, -1, -1,
		-1, -1, -1, -1, -1, -1, -1, -1,
		-1, -1, -1, -1, -1, -1, -1, -1,
		 0,  0, -1, -1,  1,  1,  1, -1,
		 2,  2,  2,  2,  3,  3,  3,  3,
		 4,  4,  4, -1,  5,  5,  5, -1,
		 6,  6,  6, -1,  7, -1, -1, -1
	}
};

static const h8500_variant_t variant_h8532 =
{
	.addr_mask = 0x0fffffu,
	.io_base = 0xff80, .io_size = 0x80,
	.ram_base = 0xfb80, .ram_size = 0x400,
	.bus8 = true,

	.port_count = 9,
	.port_ddr = { -1, M_P1DDR, M_P2DDR, -1, M_P4DDR, M_P5DDR, M_P6DDR, M_P7DDR, -1, M_P9DDR },
	.port_dr  = { -1, M_P1DR, M_P2DR, M_P3DR, M_P4DR, M_P5DR, M_P6DR, M_P7DR, M_P8DR, M_P9DR },
	.port_ddr_fixed = { 0, 0, 0, 0xff, 0, 0, 0, 0, 0x00, 0 },
	.port_ddr_reset = { 0, 0xff, 0xff, 0, 0xff, 0xff, 0xff, 0xff, 0, 0xff },
	.port_mask = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },

	.frt_count = 3,
	.frt_reg = { M_FRT1, M_FRT2, M_FRT3 },
	.frt_vector = { 36, 40, 44 },
	.frt_temp = true,

	.tmr_reg = M_TMRCR, .tmr_vector = 48,

	.sci_count = 1,
	.sci_reg = { M_SCI1 },
	.sci_vector = { 52 },

	.adc_reg = M_ADDR0, .adc_vector = 56,

	.wdt_reg = M_WDT, .wdt_vector = 32,

	.ipr_reg = M_IPRA,

	.irq_pin_count = 2,
	.irq_vector = { 32, 33 },
	.ctl_reg = { M_P1CR, -1 },
	.ctl_write = { 0x78, 0 },
	.ctl_read = { 0x87, 0 },
	.irq_ctl = 0, .irq_ctl_shift = 5,
	.nmi_ctl = 0, .nmi_ctl_bit = 4,

	.vector_slot =
	{
		-1, -1, -1, -1, -1, -1, -1, -1,
		-1, -1, -1, -1, -1, -1, -1, -1,
		-1, -1, -1, -1, -1, -1, -1, -1,
		-1, -1, -1, -1, -1, -1, -1, -1,
		 0,  1, -1, -1,  2,  2,  2,  2,
		 3,  3,  3,  3,  4,  4,  4,  4,
		 5,  5,  5, -1,  6,  6,  6, -1,
		 7, -1, -1, -1, -1, -1, -1, -1
	}
};

const h8500_variant_t *h8500_variant(int model)
{
	return model == H8500_H8532 ? &variant_h8532 : &variant_h8510;
}

static void irq_raise(h8500_t *cpu, int vector)
{
	cpu->irq_pend[vector >> 5] |= 1u << (vector & 31);
}

static void irq_clear(h8500_t *cpu, int vector)
{
	cpu->irq_pend[vector >> 5] &= ~(1u << (vector & 31));
}

static int irq_priority(h8500_t *cpu, int vector)
{
	int slot;
	if (vector == VEC_NMI)
		return 8;
	if (vector < 0 || vector >= 64)
		return 0;
	slot = cpu->var->vector_slot[vector];
	if (slot < 0)
		return 0;
	return (cpu->io[cpu->var->ipr_reg + (slot >> 1)] >> ((slot & 1) ? 0 : 4)) & 7;
}

static uint8_t port_ddr(h8500_t *cpu, int port)
{
	if (cpu->var->port_ddr[port] < 0)
		return cpu->var->port_ddr_fixed[port];
	return cpu->io[cpu->var->port_ddr[port]];
}

static void port_update(h8500_t *cpu, int port)
{
	uint8_t ddr = port_ddr(cpu, port);
	uint8_t dr = cpu->io[cpu->var->port_dr[port]];
	uint8_t mask = cpu->var->port_mask[port];
	uint8_t data = (uint8_t)((dr | (uint8_t)~ddr) & (uint8_t)~mask);
	uint16_t out;
	ddr = (uint8_t)(ddr & (uint8_t)~mask);
	out = (uint16_t)((ddr << 8) | data);
	if (cpu->port_out[port] != out)
	{
		cpu->port_out[port] = out;
		if (cpu->bus.write_port)
			cpu->bus.write_port(cpu->bus.user, port, data, ddr);
	}
}

static uint8_t port_read(h8500_t *cpu, int port)
{
	uint8_t ddr = port_ddr(cpu, port);
	uint8_t mask = cpu->var->port_mask[port];
	uint8_t res = (uint8_t)(mask | (cpu->io[cpu->var->port_dr[port]] & ddr));
	if ((uint8_t)(ddr & ~mask) != (uint8_t)~mask)
	{
		uint8_t pins = cpu->bus.read_port ? cpu->bus.read_port(cpu->bus.user, port) : 0xff;
		res |= (uint8_t)(pins & ~ddr);
	}
	return res;
}

/* ---- serial ---- */

static uint32_t sci_char_cycles(h8500_t *cpu, int ch)
{
	int base = cpu->var->sci_reg[ch];
	uint8_t smr = cpu->io[base + S_SMR];
	uint32_t d = 32u * ((uint32_t)cpu->io[base + S_BRR] + 1u);
	uint32_t cks = smr & 3u;
	uint32_t bits;
	while (cks--)
		d *= 4u;
	if (smr & 0x80)
		return d;                       /* clocked synchronous: 8 bits, no framing */
	bits = 10;
	if (smr & 0x40)
		bits--;
	if (smr & 0x20)
		bits++;
	if (smr & 0x08)
		bits++;
	return bits * d;
}

static void sci_tx_start(h8500_t *cpu, int ch)
{
	int base = cpu->var->sci_reg[ch];
	int vec = cpu->var->sci_vector[ch];
	if (cpu->sci[ch].tx_busy)
		return;
	if (!(cpu->io[base + S_SCR] & 0x20))
		return;
	if (cpu->io[base + S_SSR] & SSR_TDRE)
		return;
	cpu->sci[ch].tx_shift = cpu->io[base + S_TDR];
	cpu->sci[ch].tx_busy = true;
	cpu->sci[ch].tx_timer = sci_char_cycles(cpu, ch);
	cpu->io[base + S_SSR] |= SSR_TDRE;
	if (cpu->io[base + S_SCR] & 0x80)
		irq_raise(cpu, vec + 2);
}

static void sci_update(h8500_t *cpu, int ch, uint32_t cycles)
{
	int base = cpu->var->sci_reg[ch];
	int vec = cpu->var->sci_vector[ch];
	uint8_t scr = cpu->io[base + S_SCR];

	if (cpu->sci[ch].tx_busy)
	{
		if (cpu->sci[ch].tx_timer <= cycles)
		{
			cpu->sci[ch].tx_timer = 0;
			cpu->sci[ch].tx_busy = false;
			if (cpu->bus.sci_tx)
				cpu->bus.sci_tx(cpu->bus.user, ch, cpu->sci[ch].tx_shift);
			if (!(cpu->io[base + S_SSR] & SSR_TDRE))
			{
				sci_tx_start(cpu, ch);
			}
			else
			{
				cpu->io[base + S_SSR] |= SSR_TEND;
				if (scr & 0x04)
					irq_raise(cpu, vec + 3);
			}
		}
		else
		{
			cpu->sci[ch].tx_timer -= cycles;
		}
	}

	if (cpu->sci[ch].rx_pending && (scr & 0x10))
	{
		cpu->sci[ch].rx_pending = false;
		if (cpu->io[base + S_SSR] & SSR_RDRF)
		{
			cpu->io[base + S_SSR] |= SSR_ORER;
			if (scr & 0x40)
				irq_raise(cpu, vec);
		}
		else
		{
			cpu->io[base + S_RDR] = cpu->sci[ch].rx_byte;
			cpu->io[base + S_SSR] |= SSR_RDRF;
			if (scr & 0x40)
				irq_raise(cpu, vec + 1);
		}
	}
}

/* ---- free-running timers ---- */

static void frt_update(h8500_t *cpu, int n, uint32_t cycles)
{
	int base = cpu->var->frt_reg[n];
	int vec = cpu->var->frt_vector[n];
	uint8_t tcr = cpu->io[base + F_TCR];
	uint8_t tcsr = cpu->io[base + F_TCSR];
	uint32_t shift, steps;
	uint16_t ocra, ocrb, cnt;
	int cclra = (tcsr & 0x01) != 0;

	switch (tcr & 3)
	{
	case 0: shift = 2; break;
	case 1: shift = 3; break;
	case 2: shift = 5; break;
	default: return;
	}

	cpu->frt_prescale[n] += cycles;
	steps = cpu->frt_prescale[n] >> shift;
	cpu->frt_prescale[n] &= (1u << shift) - 1u;
	if (!steps)
		return;

	ocra = (uint16_t)((cpu->io[base + F_OCRAH] << 8) | cpu->io[base + F_OCRAL]);
	ocrb = (uint16_t)((cpu->io[base + F_OCRBH] << 8) | cpu->io[base + F_OCRBL]);
	cnt = cpu->frt_count[n];

	while (steps--)
	{
		int wrapped = 0;
		cnt = (uint16_t)(cnt + 1);
		if (cnt == (uint16_t)(ocra + 1))
		{
			tcsr |= 0x20;
			if (tcr & 0x20)
				irq_raise(cpu, vec + 1);
			if (cclra)
			{
				cnt = 0;
				wrapped = 1;
			}
		}
		if (cnt == (uint16_t)(ocrb + 1))
		{
			tcsr |= 0x40;
			if (tcr & 0x40)
				irq_raise(cpu, vec + 2);
		}
		if (cnt == 0 && !wrapped)
		{
			tcsr |= 0x10;
			if (tcr & 0x10)
				irq_raise(cpu, vec + 3);
		}
	}

	cpu->frt_count[n] = cnt;
	cpu->io[base + F_TCSR] = tcsr;
}

/* ---- 8-bit timer ---- */

static void tmr_update(h8500_t *cpu, uint32_t cycles)
{
	static const uint8_t shifts[4] = { 0, 3, 6, 10 };
	int base = cpu->var->tmr_reg;
	int vec = cpu->var->tmr_vector;
	uint8_t tcr = cpu->io[base + T_TCR];
	uint8_t tcsr = cpu->io[base + T_TCSR];
	uint32_t shift, steps;
	uint8_t cnt, cora, corb;
	int clr = (tcr >> 3) & 3;

	if ((tcr & 7) == 0 || (tcr & 7) > 3)
		return;
	shift = shifts[tcr & 3];

	cpu->tmr_prescale += cycles;
	steps = cpu->tmr_prescale >> shift;
	cpu->tmr_prescale &= (1u << shift) - 1u;
	if (!steps)
		return;

	cora = cpu->io[base + T_TCORA];
	corb = cpu->io[base + T_TCORB];
	cnt = cpu->tmr_count;

	while (steps--)
	{
		int wrapped = 0;
		cnt = (uint8_t)(cnt + 1);
		if (cnt == (uint8_t)(cora + 1))
		{
			if (!(tcsr & 0x40))
			{
				tcsr |= 0x40;
				if (tcr & 0x40)
					irq_raise(cpu, vec);
			}
			if (clr == 1)
			{
				cnt = 0;
				wrapped = 1;
			}
		}
		if (cnt == (uint8_t)(corb + 1))
		{
			if (!(tcsr & 0x80))
			{
				tcsr |= 0x80;
				if (tcr & 0x80)
					irq_raise(cpu, vec + 1);
			}
			if (clr == 2)
			{
				cnt = 0;
				wrapped = 1;
			}
		}
		if (cnt == 0 && !wrapped)
		{
			if (!(tcsr & 0x20))
			{
				tcsr |= 0x20;
				if (tcr & 0x20)
					irq_raise(cpu, vec + 2);
			}
		}
	}

	cpu->tmr_count = cnt;
	cpu->io[base + T_TCSR] = tcsr;
}

/* ---- watchdog ---- */

static void wdt_update(h8500_t *cpu, uint32_t cycles)
{
	static const uint8_t shifts[8] = { 1, 5, 6, 7, 8, 9, 11, 12 };
	int base = cpu->var->wdt_reg;
	uint8_t tcsr;
	uint32_t shift, steps;

	if (base < 0)
		return;
	tcsr = cpu->io[base + W_TCSR];
	if (!(tcsr & 0x20))
	{
		cpu->io[base + W_TCNT] = 0;
		return;
	}
	shift = shifts[tcsr & 7];

	cpu->wdt_prescale += cycles;
	steps = cpu->wdt_prescale >> shift;
	cpu->wdt_prescale &= (1u << shift) - 1u;

	while (steps--)
	{
		cpu->io[base + W_TCNT]++;
		if (cpu->io[base + W_TCNT] == 0)
		{
			if (tcsr & 0x40)
			{
				h8500_reset(cpu);
				return;
			}
			if (!(cpu->io[base + W_TCSR] & 0x80))
			{
				cpu->io[base + W_TCSR] |= 0x80;
				irq_raise(cpu, cpu->var->wdt_vector);
			}
		}
	}
}

/* ---- A/D ---- */

static uint32_t adc_conv_cycles(h8500_t *cpu, int first)
{
	if (cpu->io[cpu->var->adc_reg + A_ADCSR] & 0x08)
		return first ? 134u : 128u;
	return first ? 266u : 256u;
}

static void adc_start(h8500_t *cpu)
{
	uint8_t adcsr = cpu->io[cpu->var->adc_reg + A_ADCSR];
	cpu->adc_channel = (uint8_t)((adcsr & 0x10) ? (adcsr & 4) : (adcsr & 7));
	cpu->adc_busy = adc_conv_cycles(cpu, 1);
}

static void adc_update(h8500_t *cpu, uint32_t cycles)
{
	int base = cpu->var->adc_reg;
	uint8_t adcsr;
	int scan, endch;
	uint16_t v;

	if (!cpu->adc_busy)
		return;
	if (cpu->adc_busy > cycles)
	{
		cpu->adc_busy -= cycles;
		return;
	}
	cpu->adc_busy = 0;

	adcsr = cpu->io[base + A_ADCSR];
	scan = (adcsr & 0x10) != 0;
	endch = adcsr & 7;

	v = cpu->bus.read_adc ? cpu->bus.read_adc(cpu->bus.user, cpu->adc_channel) : 0;
	cpu->io[base + A_ADDR0 + ((cpu->adc_channel & 3) << 1)] = (uint8_t)(v >> 2);
	cpu->io[base + A_ADDR0 + ((cpu->adc_channel & 3) << 1) + 1] = (uint8_t)((v << 6) & 0xff);

	if (!scan)
	{
		cpu->io[base + A_ADCSR] = (uint8_t)((adcsr | 0x80) & ~0x20u);
		if (adcsr & 0x40)
			irq_raise(cpu, cpu->var->adc_vector);
		return;
	}

	if (cpu->adc_channel == (uint8_t)endch)
	{
		cpu->io[base + A_ADCSR] |= 0x80;
		if (adcsr & 0x40)
			irq_raise(cpu, cpu->var->adc_vector);
		cpu->adc_channel = (uint8_t)(adcsr & 4);
	}
	else
	{
		cpu->adc_channel = (uint8_t)((cpu->adc_channel + 1) & 7);
	}
	cpu->adc_busy = adc_conv_cycles(cpu, 0);
}

void h8500_peripherals_tick(h8500_t *cpu, uint32_t cycles)
{
	int n;
	if (!cycles)
		return;
	for (n = 0; n < cpu->var->frt_count; n++)
		frt_update(cpu, n, cycles);
	tmr_update(cpu, cycles);
	wdt_update(cpu, cycles);
	adc_update(cpu, cycles);
	for (n = 0; n < cpu->var->sci_count; n++)
		sci_update(cpu, n, cycles);
}

static uint32_t frt_horizon(h8500_t *cpu, int n)
{
	int base = cpu->var->frt_reg[n];
	uint8_t tcr = cpu->io[base + F_TCR];
	uint32_t shift, d, best;
	uint16_t ocra, ocrb, cnt;

	switch (tcr & 3)
	{
	case 0: shift = 2; break;
	case 1: shift = 3; break;
	case 2: shift = 5; break;
	default: return UINT32_MAX;
	}
	ocra = (uint16_t)((cpu->io[base + F_OCRAH] << 8) | cpu->io[base + F_OCRAL]);
	ocrb = (uint16_t)((cpu->io[base + F_OCRBH] << 8) | cpu->io[base + F_OCRBL]);
	cnt = cpu->frt_count[n];

	best = (uint32_t)((uint16_t)(ocra + 1 - cnt));
	if (!best) best = 0x10000;
	d = (uint32_t)((uint16_t)(ocrb + 1 - cnt));
	if (!d) d = 0x10000;
	if (d < best) best = d;
	d = (uint32_t)((uint16_t)(0 - cnt));
	if (!d) d = 0x10000;
	if (d < best) best = d;
	return (best << shift) - cpu->frt_prescale[n];
}

static uint32_t tmr_horizon(h8500_t *cpu)
{
	static const uint8_t shifts[4] = { 0, 3, 6, 10 };
	int base = cpu->var->tmr_reg;
	uint8_t tcr = cpu->io[base + T_TCR];
	uint32_t shift, d, best;
	uint8_t cnt = cpu->tmr_count;

	if ((tcr & 7) == 0 || (tcr & 7) > 3)
		return UINT32_MAX;
	shift = shifts[tcr & 3];
	best = (uint32_t)((uint8_t)(cpu->io[base + T_TCORA] + 1 - cnt));
	if (!best) best = 0x100;
	d = (uint32_t)((uint8_t)(cpu->io[base + T_TCORB] + 1 - cnt));
	if (!d) d = 0x100;
	if (d < best) best = d;
	d = (uint32_t)((uint8_t)(0 - cnt));
	if (!d) d = 0x100;
	if (d < best) best = d;
	return (best << shift) - cpu->tmr_prescale;
}

static uint32_t wdt_horizon(h8500_t *cpu)
{
	static const uint8_t shifts[8] = { 1, 5, 6, 7, 8, 9, 11, 12 };
	int base = cpu->var->wdt_reg;
	uint8_t tcsr;
	uint32_t shift;
	if (base < 0)
		return UINT32_MAX;
	tcsr = cpu->io[base + W_TCSR];
	shift = shifts[tcsr & 7];
	if (!(tcsr & 0x20))
		return UINT32_MAX;
	return ((256u - cpu->io[base + W_TCNT]) << shift) - cpu->wdt_prescale;
}

static uint32_t sci_horizon(h8500_t *cpu, int ch)
{
	int base = cpu->var->sci_reg[ch];
	uint32_t h = UINT32_MAX;
	if (cpu->sci[ch].rx_pending && (cpu->io[base + S_SCR] & 0x10))
		return 1;
	if (cpu->sci[ch].tx_busy)
		h = cpu->sci[ch].tx_timer ? cpu->sci[ch].tx_timer : 1;
	return h;
}

/* Cycles that can pass before a peripheral does anything but count:
 * a compare match, an overflow, a conversion or a character completing.
 * Ticking fewer cycles than this, in any number of pieces, leaves the
 * peripherals in the same state as ticking them at once. */
uint32_t h8500_tick_horizon(h8500_t *cpu)
{
	uint32_t h = UINT32_MAX, d;
	int n;
	for (n = 0; n < cpu->var->frt_count; n++)
	{
		d = frt_horizon(cpu, n); if (d < h) h = d;
	}
	d = tmr_horizon(cpu); if (d < h) h = d;
	d = wdt_horizon(cpu); if (d < h) h = d;
	d = cpu->adc_busy ? cpu->adc_busy : UINT32_MAX; if (d < h) h = d;
	for (n = 0; n < cpu->var->sci_count; n++)
	{
		d = sci_horizon(cpu, n); if (d < h) h = d;
	}
	return h ? h : 1;
}

/* ---- register file ---- */

static uint8_t frt_read(h8500_t *cpu, int n, int sub)
{
	int base = cpu->var->frt_reg[n];
	switch (sub)
	{
	case F_FRCH:
		if (cpu->var->frt_temp)
			cpu->frt_temp[n] = (uint8_t)cpu->frt_count[n];
		return (uint8_t)(cpu->frt_count[n] >> 8);
	case F_FRCL:
		return cpu->var->frt_temp ? cpu->frt_temp[n] : (uint8_t)cpu->frt_count[n];
	case F_ICRH:
		if (cpu->var->frt_temp)
			cpu->frt_temp[n] = 0;
		return 0;
	case F_ICRL:
		return cpu->var->frt_temp ? cpu->frt_temp[n] : 0;
	default:
		return cpu->io[base + sub];
	}
}

static void frt_write(h8500_t *cpu, int n, int sub, uint8_t data)
{
	int base = cpu->var->frt_reg[n];
	switch (sub)
	{
	case F_TCSR:
	{
		uint8_t old = cpu->io[base + sub];
		cpu->io[base + sub] = (uint8_t)((data & 0x0f) | (old & data & 0xf0));
		return;
	}
	case F_FRCH:
		if (cpu->var->frt_temp)
			cpu->frt_temp[n] = data;
		else
			cpu->frt_count[n] = (uint16_t)((data << 8) | (cpu->frt_count[n] & 0xff));
		return;
	case F_FRCL:
		if (cpu->var->frt_temp)
			cpu->frt_count[n] = (uint16_t)((cpu->frt_temp[n] << 8) | data);
		else
			cpu->frt_count[n] = (uint16_t)((cpu->frt_count[n] & 0xff00) | data);
		return;
	case F_OCRAH: case F_OCRBH:
		if (cpu->var->frt_temp)
			cpu->frt_temp[n] = data;
		else
			cpu->io[base + sub] = data;
		return;
	case F_OCRAL: case F_OCRBL:
		if (cpu->var->frt_temp)
			cpu->io[base + sub - 1] = cpu->frt_temp[n];
		cpu->io[base + sub] = data;
		return;
	case F_ICRH: case F_ICRL:
		return;
	default:
		cpu->io[base + sub] = data;
		return;
	}
}

static uint8_t irq_enable(h8500_t *cpu)
{
	const h8500_variant_t *v = cpu->var;
	return (uint8_t)((cpu->io[v->ctl_reg[v->irq_ctl]] >> v->irq_ctl_shift) &
	                 ((1u << v->irq_pin_count) - 1u));
}

static void irq_ctl_update(h8500_t *cpu)
{
	uint8_t en = irq_enable(cpu);
	int i;
	for (i = 0; i < cpu->var->irq_pin_count; i++)
	{
		if (!(en & (1u << i)))
		{
			cpu->irq_req &= (uint8_t)~(1u << i);
			irq_clear(cpu, cpu->var->irq_vector[i]);
		}
	}
	if ((en & 1) && (cpu->irq_lines & 1))
	{
		cpu->irq_req |= 1;
		irq_raise(cpu, cpu->var->irq_vector[0]);
	}
	else if (!(cpu->irq_lines & 1))
	{
		cpu->irq_req &= (uint8_t)~1u;
		irq_clear(cpu, cpu->var->irq_vector[0]);
	}
}

uint8_t h8500_io_read(h8500_t *cpu, uint16_t address)
{
	const h8500_variant_t *v = cpu->var;
	uint32_t o = (uint32_t)(address - v->io_base);
	int unit;

	if (o >= v->io_size)
		return 0xff;
	unit = cpu->io_unit[o];

	switch (cpu->io_kind[o])
	{
	case IOK_PORT_DDR:
		return 0xff;
	case IOK_PORT_DR:
		return port_read(cpu, unit);
	case IOK_FRT:
		return frt_read(cpu, unit, (int)(o - v->frt_reg[unit]));
	case IOK_TMR:
		if (o - v->tmr_reg == T_TCNT)
			return cpu->tmr_count;
		break;
	case IOK_SCI:
		if (o - v->sci_reg[unit] == S_SSR)
		{
			cpu->sci[unit].ssr_read = cpu->io[o];
			return cpu->io[o];
		}
		break;
	case IOK_WDT:
		if (o - (uint32_t)v->wdt_reg == W_TCSR)
			return (uint8_t)(cpu->io[o] | 0x18);
		break;
	case IOK_CTL:
		return (uint8_t)(v->ctl_read[unit] | cpu->io[o]);
	default:
		break;
	}
	return cpu->io[o];
}

void h8500_io_write(h8500_t *cpu, uint16_t address, uint8_t data)
{
	const h8500_variant_t *v = cpu->var;
	uint32_t o = (uint32_t)(address - v->io_base);
	int unit;

	if (o >= v->io_size)
		return;
	unit = cpu->io_unit[o];

	switch (cpu->io_kind[o])
	{
	case IOK_PORT_DDR: case IOK_PORT_DR:
		cpu->io[o] = data;
		port_update(cpu, unit);
		return;

	case IOK_FRT:
		frt_write(cpu, unit, (int)(o - v->frt_reg[unit]), data);
		return;

	case IOK_TMR:
		switch (o - v->tmr_reg)
		{
		case T_TCR:
		{
			uint8_t old = cpu->io[o];
			uint8_t tcsr = cpu->io[v->tmr_reg + T_TCSR];
			cpu->io[o] = data;
			if ((data & 0x40) && !(old & 0x40) && (tcsr & 0x40))
				irq_raise(cpu, v->tmr_vector);
			if ((data & 0x80) && !(old & 0x80) && (tcsr & 0x80))
				irq_raise(cpu, v->tmr_vector + 1);
			if ((data & 0x20) && !(old & 0x20) && (tcsr & 0x20))
				irq_raise(cpu, v->tmr_vector + 2);
			return;
		}
		case T_TCSR:
		{
			uint8_t nv = (uint8_t)(cpu->io[o] & (data | 0x1f));
			cpu->io[o] = (uint8_t)((nv & 0xf0) | (data & 0x0f));
			return;
		}
		case T_TCNT:
			cpu->tmr_count = data;
			return;
		default:
			break;
		}
		break;

	case IOK_SCI:
		switch (o - v->sci_reg[unit])
		{
		case S_SSR:
		{
			int base = v->sci_reg[unit];
			int vec = v->sci_vector[unit];
			uint8_t old = cpu->io[o];
			uint8_t seen = cpu->sci[unit].ssr_read;
			uint8_t nv = old;

			if ((cpu->io[base + S_SCR] & 0x20) && (seen & SSR_TDRE) && !(data & SSR_TDRE))
				nv &= (uint8_t)~(SSR_TDRE | SSR_TEND);
			if ((seen & SSR_RDRF) && !(data & SSR_RDRF)) nv &= (uint8_t)~SSR_RDRF;
			if ((seen & SSR_ORER) && !(data & SSR_ORER)) nv &= (uint8_t)~SSR_ORER;
			if ((seen & SSR_FER)  && !(data & SSR_FER))  nv &= (uint8_t)~SSR_FER;
			if ((seen & SSR_PER)  && !(data & SSR_PER))  nv &= (uint8_t)~SSR_PER;
			nv = (uint8_t)((nv & 0xfe) | (data & 1));
			cpu->io[o] = nv;
			cpu->sci[unit].ssr_read &= nv;
			if (!(nv & (SSR_ORER | SSR_FER | SSR_PER)))
				irq_clear(cpu, vec);
			if (!(nv & SSR_RDRF))
				irq_clear(cpu, vec + 1);
			if (!(nv & SSR_TDRE))
				irq_clear(cpu, vec + 2);
			if (!(nv & SSR_TEND))
				irq_clear(cpu, vec + 3);
			if (!(nv & SSR_TDRE))
				sci_tx_start(cpu, unit);
			return;
		}
		case S_SCR:
		{
			int base = v->sci_reg[unit];
			int vec = v->sci_vector[unit];
			uint8_t old = cpu->io[o];
			uint8_t ssr;
			cpu->io[o] = data;
			ssr = cpu->io[base + S_SSR];
			if ((data & 0x80) && !(old & 0x80) && (ssr & SSR_TDRE))
				irq_raise(cpu, vec + 2);
			if ((data & 0x04) && !(old & 0x04) && (ssr & SSR_TEND))
				irq_raise(cpu, vec + 3);
			if ((data & 0x40) && !(old & 0x40))
			{
				if (ssr & SSR_RDRF)
					irq_raise(cpu, vec + 1);
				if (ssr & (SSR_ORER | SSR_FER | SSR_PER))
					irq_raise(cpu, vec);
			}
			if (!(data & 0x40))
			{
				irq_clear(cpu, vec);
				irq_clear(cpu, vec + 1);
			}
			if (!(data & 0x80))
				irq_clear(cpu, vec + 2);
			if (!(data & 0x04))
				irq_clear(cpu, vec + 3);
			return;
		}
		case S_RDR:
			return;
		default:
			break;
		}
		break;

	case IOK_ADC:
		if (o - v->adc_reg == A_ADCSR)
		{
			uint8_t old = cpu->io[o];
			uint8_t nv = (uint8_t)((data & 0x7f) | (old & data & 0x80));
			cpu->io[o] = nv;
			if ((nv & 0x20) && !(old & 0x20))
				adc_start(cpu);
			else if (!(nv & 0x20))
				cpu->adc_busy = 0;
			return;
		}
		break;

	case IOK_WDT:
		return;

	case IOK_IPR:
		cpu->io[o] = (uint8_t)(data & 0x77);
		return;

	case IOK_CTL:
		cpu->io[o] = (uint8_t)(data & v->ctl_write[unit]);
		if (unit == v->irq_ctl)
			irq_ctl_update(cpu);
		return;

	default:
		break;
	}
	cpu->io[o] = data;
}

/* The watchdog register pair takes 16-bit writes only, with a password in
 * the high byte. */
static void wdt_write16(h8500_t *cpu, uint16_t data)
{
	int base = cpu->var->wdt_reg;
	if ((data >> 8) == 0xa5)
	{
		uint8_t old = cpu->io[base + W_TCSR];
		cpu->io[base + W_TCSR] = (uint8_t)((old & data & 0x80) | (data & 0x7f));
		if (!(old & 0x20) && (cpu->io[base + W_TCSR] & 0x20))
			cpu->wdt_prescale = 0;
	}
	else if ((data >> 8) == 0x5a)
	{
		if (cpu->io[base + W_TCSR] & 0x20)
		{
			cpu->io[base + W_TCNT] = (uint8_t)data;
			cpu->wdt_prescale = 0;
		}
	}
}

/* ------------------------------------------------------------------ */
/* interrupts                                                         */
/* ------------------------------------------------------------------ */

int h8500_irq_select(h8500_t *cpu, int *out_level)
{
	int mask = (cpu->sr & SR_I) >> 8;
	int best = -1, best_level = -1;
	int i, j;
	for (i = 0; i < 3; i++)
	{
		uint32_t p = cpu->irq_pend[i];
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
				int level = irq_priority(cpu, vect);
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

void h8500_exception(h8500_t *cpu, int vector, uint16_t ret_pc, int level)
{
	uint32_t va = (uint32_t)vector * 4;
	push16(cpu, ret_pc);
	push16(cpu, cpu->cp);
	push16(cpu, cpu->sr);
	cpu->sr &= (uint16_t)~SR_T;
	if (level >= 0)
	{
		cpu->sr &= (uint16_t)~SR_I;
		cpu->sr |= (uint16_t)((level > 7 ? 7 : level) << 8);
	}
	cpu->cp = (uint8_t)mem_read16(cpu, va);
	cpu->pc = mem_read16(cpu, va + 2);
}

void h8500_take_interrupt(h8500_t *cpu, int vector, int level)
{
	int i;
	for (i = 0; i < cpu->var->irq_pin_count; i++)
	{
		if (vector == cpu->var->irq_vector[i])
		{
			if (i != 0)
				cpu->irq_req &= (uint8_t)~(1u << i);
			if (!(cpu->irq_req & (1u << i)))
				irq_clear(cpu, vector);
			break;
		}
	}
	if (i == cpu->var->irq_pin_count)
		irq_clear(cpu, vector);
	cpu->sleeping = false;
	exception(cpu, vector, cpu->pc, level);
	cpu->no_irq = true;
}

void h8500_set_irq(h8500_t *cpu, int line, bool state)
{
	const h8500_variant_t *v = cpu->var;
	uint8_t mask, en;
	if (line == H8500_NMI)
	{
		uint8_t edge = (uint8_t)((cpu->io[v->ctl_reg[v->nmi_ctl]] >> v->nmi_ctl_bit) & 1);
		bool trigger = edge
			? (!state && (cpu->irq_lines & 0x80) != 0)
			: (state && (cpu->irq_lines & 0x80) == 0);
		if (state)
			cpu->irq_lines |= 0x80;
		else
			cpu->irq_lines &= (uint8_t)~0x80;
		if (trigger)
			irq_raise(cpu, VEC_NMI);
		return;
	}
	if (line < 0 || line >= v->irq_pin_count)
		return;
	mask = (uint8_t)(1u << line);
	en = irq_enable(cpu);
	{
		bool was = (cpu->irq_lines & mask) != 0;
		if (state)
			cpu->irq_lines |= mask;
		else
			cpu->irq_lines &= (uint8_t)~mask;
		if (line == 0)
		{
			if (state && (en & 1))
			{
				cpu->irq_req |= 1;
				irq_raise(cpu, v->irq_vector[0]);
			}
			else
			{
				cpu->irq_req &= (uint8_t)~1u;
				irq_clear(cpu, v->irq_vector[0]);
			}
		}
		else if (state && !was && (en & mask))
		{
			cpu->irq_req |= mask;
			irq_raise(cpu, v->irq_vector[line]);
		}
	}
}

void h8500_sci_rx(h8500_t *cpu, int channel, uint8_t byte)
{
	int ch = channel & 1;
	cpu->sci[ch].rx_byte = byte;
	cpu->sci[ch].rx_pending = true;
}

/* ------------------------------------------------------------------ */
/* flags                                                              */
/* ------------------------------------------------------------------ */

static void set_nz8(h8500_t *cpu, uint8_t r)
{
	cpu->sr &= (uint16_t)~(FLAG_N | FLAG_Z);
	if (r & 0x80) cpu->sr |= FLAG_N;
	if (!r) cpu->sr |= FLAG_Z;
}

static void set_nz16(h8500_t *cpu, uint16_t r)
{
	cpu->sr &= (uint16_t)~(FLAG_N | FLAG_Z);
	if (r & 0x8000) cpu->sr |= FLAG_N;
	if (!r) cpu->sr |= FLAG_Z;
}

static uint8_t do_add8(h8500_t *cpu, uint8_t d, uint8_t s, unsigned c, int keep_z)
{
	unsigned t = (unsigned)d + s + c;
	uint8_t r = (uint8_t)t;
	uint16_t z = (uint16_t)(cpu->sr & FLAG_Z);
	cpu->sr &= (uint16_t)~(FLAG_N | FLAG_Z | FLAG_V | FLAG_C);
	if (r & 0x80) cpu->sr |= FLAG_N;
	if (!r && (!keep_z || z)) cpu->sr |= FLAG_Z;
	if ((~(d ^ s) & (d ^ r)) & 0x80) cpu->sr |= FLAG_V;
	if (t & 0x100) cpu->sr |= FLAG_C;
	return r;
}

static uint16_t do_add16(h8500_t *cpu, uint16_t d, uint16_t s, unsigned c, int keep_z)
{
	uint32_t t = (uint32_t)d + s + c;
	uint16_t r = (uint16_t)t;
	uint16_t z = (uint16_t)(cpu->sr & FLAG_Z);
	cpu->sr &= (uint16_t)~(FLAG_N | FLAG_Z | FLAG_V | FLAG_C);
	if (r & 0x8000) cpu->sr |= FLAG_N;
	if (!r && (!keep_z || z)) cpu->sr |= FLAG_Z;
	if ((~(d ^ s) & (d ^ r)) & 0x8000) cpu->sr |= FLAG_V;
	if (t & 0x10000) cpu->sr |= FLAG_C;
	return r;
}

static uint8_t do_sub8(h8500_t *cpu, uint8_t d, uint8_t s, unsigned c, int keep_z)
{
	unsigned t = (unsigned)d - s - c;
	uint8_t r = (uint8_t)t;
	uint16_t z = (uint16_t)(cpu->sr & FLAG_Z);
	cpu->sr &= (uint16_t)~(FLAG_N | FLAG_Z | FLAG_V | FLAG_C);
	if (r & 0x80) cpu->sr |= FLAG_N;
	if (!r && (!keep_z || z)) cpu->sr |= FLAG_Z;
	if (((d ^ s) & (d ^ r)) & 0x80) cpu->sr |= FLAG_V;
	if (t & 0x100) cpu->sr |= FLAG_C;
	return r;
}

static uint16_t do_sub16(h8500_t *cpu, uint16_t d, uint16_t s, unsigned c, int keep_z)
{
	uint32_t t = (uint32_t)d - s - c;
	uint16_t r = (uint16_t)t;
	uint16_t z = (uint16_t)(cpu->sr & FLAG_Z);
	cpu->sr &= (uint16_t)~(FLAG_N | FLAG_Z | FLAG_V | FLAG_C);
	if (r & 0x8000) cpu->sr |= FLAG_N;
	if (!r && (!keep_z || z)) cpu->sr |= FLAG_Z;
	if (((d ^ s) & (d ^ r)) & 0x8000) cpu->sr |= FLAG_V;
	if (t & 0x10000) cpu->sr |= FLAG_C;
	return r;
}

static int cond_true(h8500_t *cpu, int cc)
{
	unsigned n = (cpu->sr & FLAG_N) != 0;
	unsigned z = (cpu->sr & FLAG_Z) != 0;
	unsigned v = (cpu->sr & FLAG_V) != 0;
	unsigned c = (cpu->sr & FLAG_C) != 0;
	switch (cc)
	{
	case 0x0: return 1;
	case 0x1: return 0;
	case 0x2: return !(c | z);
	case 0x3: return (c | z) != 0;
	case 0x4: return !c;
	case 0x5: return c != 0;
	case 0x6: return !z;
	case 0x7: return z != 0;
	case 0x8: return !v;
	case 0x9: return v != 0;
	case 0xa: return !n;
	case 0xb: return n != 0;
	case 0xc: return (n ^ v) == 0;
	case 0xd: return (n ^ v) != 0;
	case 0xe: return (z | (n ^ v)) == 0;
	default:  return (z | (n ^ v)) != 0;
	}
}

/* ------------------------------------------------------------------ */
/* effective addresses                                                */
/* ------------------------------------------------------------------ */

typedef struct
{
	uint8_t ea;
	uint8_t ext[2];
	uint8_t sz;
	uint8_t mode;
	uint8_t reg;
	uint8_t step;
	uint8_t committed;
	uint32_t addr;
} ea_t;

enum { EM_REG = 0, EM_IND, EM_D8, EM_D16, EM_PREDEC, EM_POSTINC, EM_ABS8, EM_ABS16, EM_IMM8, EM_IMM16 };

static int ea_is_byte_stack(const ea_t *e)
{
	return e->ea == 0xb7 || e->ea == 0xc7;
}

static uint32_t ea_page(h8500_t *cpu, int reg)
{
	if (reg < 4)
		return (uint32_t)cpu->dp << 16;
	if (reg < 6)
		return (uint32_t)cpu->ep << 16;
	return (uint32_t)cpu->tp << 16;
}

static int ea_decode(h8500_t *cpu, ea_t *e, uint8_t ea)
{
	e->ea = ea;
	e->sz = (uint8_t)((ea >> 3) & 1);
	e->reg = (uint8_t)(ea & 7);
	e->committed = 0;
	e->addr = 0;
	e->step = (uint8_t)(e->sz ? 2 : 1);

	switch (ea)
	{
	case 0x04: e->mode = EM_IMM8;  e->ext[0] = fetch8(cpu); return 1;
	case 0x0c: e->mode = EM_IMM16; e->ext[0] = fetch8(cpu); e->ext[1] = fetch8(cpu); return 1;
	case 0x05: case 0x0d:
		e->mode = EM_ABS8;
		e->ext[0] = fetch8(cpu);
		e->addr = (uint32_t)((cpu->br << 8) | e->ext[0]);
		return 1;
	case 0x15: case 0x1d:
		e->mode = EM_ABS16;
		e->ext[0] = fetch8(cpu);
		e->ext[1] = fetch8(cpu);
		e->addr = ((uint32_t)cpu->dp << 16) | (uint32_t)((e->ext[0] << 8) | e->ext[1]);
		return 1;
	default:
		break;
	}

	switch (ea & 0xf0)
	{
	case 0xa0: case 0xb0:
		if ((ea & 0xf0) == 0xa0)
		{
			e->mode = EM_REG;
			return 1;
		}
		e->mode = EM_PREDEC;
		if (ea_is_byte_stack(e))
			e->step = 2;
		e->addr = ea_page(cpu, e->reg) |
			(uint32_t)((cpu->r[e->reg] - e->step) & 0xffff);
		return 1;
	case 0xc0:
		e->mode = EM_POSTINC;
		if (ea_is_byte_stack(e))
			e->step = 2;
		e->addr = ea_page(cpu, e->reg) | cpu->r[e->reg];
		return 1;
	case 0xd0:
		e->mode = EM_IND;
		e->addr = ea_page(cpu, e->reg) | cpu->r[e->reg];
		return 1;
	case 0xe0:
		e->mode = EM_D8;
		e->ext[0] = fetch8(cpu);
		e->addr = ea_page(cpu, e->reg) |
			(uint32_t)((cpu->r[e->reg] + (int8_t)e->ext[0]) & 0xffff);
		return 1;
	case 0xf0:
		e->mode = EM_D16;
		e->ext[0] = fetch8(cpu);
		e->ext[1] = fetch8(cpu);
		e->addr = ea_page(cpu, e->reg) |
			(uint32_t)((cpu->r[e->reg] + (int16_t)((e->ext[0] << 8) | e->ext[1])) & 0xffff);
		return 1;
	default:
		return 0;
	}
}

static void ea_commit(h8500_t *cpu, ea_t *e)
{
	if (e->committed)
		return;
	e->committed = 1;
	if (e->mode == EM_PREDEC)
		cpu->r[e->reg] = (uint16_t)(cpu->r[e->reg] - e->step);
	else if (e->mode == EM_POSTINC)
		cpu->r[e->reg] = (uint16_t)(cpu->r[e->reg] + e->step);
}

static uint8_t ea_read8(h8500_t *cpu, ea_t *e)
{
	uint8_t v;
	if (e->mode == EM_IMM8)
		return e->ext[0];
	if (e->mode == EM_IMM16)
		return e->ext[1];
	if (e->mode == EM_REG)
		return (uint8_t)cpu->r[e->reg];
	if (ea_is_byte_stack(e))
	{
		v = (uint8_t)mem_read16(cpu, e->addr);
		ea_commit(cpu, e);
		return v;
	}
	v = mem_read8(cpu, e->addr);
	ea_commit(cpu, e);
	return v;
}

static uint16_t ea_read16(h8500_t *cpu, ea_t *e)
{
	uint16_t v;
	if (e->mode == EM_IMM16)
		return (uint16_t)((e->ext[0] << 8) | e->ext[1]);
	if (e->mode == EM_IMM8)
		return (uint16_t)(int16_t)(int8_t)e->ext[0];
	if (e->mode == EM_REG)
		return cpu->r[e->reg];
	v = mem_read16(cpu, e->addr);
	ea_commit(cpu, e);
	return v;
}

static void ea_write8(h8500_t *cpu, ea_t *e, uint8_t v)
{
	if (e->mode == EM_REG)
	{
		cpu->r[e->reg] = (uint16_t)((cpu->r[e->reg] & 0xff00) | v);
		return;
	}
	if (ea_is_byte_stack(e))
	{
		mem_write16(cpu, e->addr, v);
		ea_commit(cpu, e);
		return;
	}
	mem_write8(cpu, e->addr, v);
	ea_commit(cpu, e);
}

static void ea_write16(h8500_t *cpu, ea_t *e, uint16_t v)
{
	if (e->mode == EM_REG)
	{
		cpu->r[e->reg] = v;
		return;
	}
	mem_write16(cpu, e->addr, v);
	ea_commit(cpu, e);
}

/* ------------------------------------------------------------------ */
/* control registers                                                  */
/* ------------------------------------------------------------------ */

static uint16_t cr_read(h8500_t *cpu, int c)
{
	switch (c)
	{
	case 0: return (uint16_t)(cpu->sr & SR_MASK);
	case 1: return (uint16_t)(cpu->sr & 0x0f);
	case 3: return cpu->br;
	case 4: return cpu->ep;
	case 5: return cpu->dp;
	case 7: return cpu->tp;
	default: return 0;
	}
}

static void cr_write(h8500_t *cpu, int c, uint16_t v)
{
	switch (c)
	{
	case 0: cpu->sr = (uint16_t)(v & SR_MASK); break;
	case 1: cpu->sr = (uint16_t)((cpu->sr & 0xfff0) | (v & 0x0f)); break;
	case 3: cpu->br = (uint8_t)v; break;
	case 4: cpu->ep = (uint8_t)v; break;
	case 5: cpu->dp = (uint8_t)v; break;
	case 7: cpu->tp = (uint8_t)v; break;
	default: break;
	}
}

/* ------------------------------------------------------------------ */
/* shifts and bit operations                                          */
/* ------------------------------------------------------------------ */

static void do_shift(h8500_t *cpu, ea_t *e, int op)
{
	unsigned c_in = (cpu->sr & FLAG_C) ? 1u : 0u;
	unsigned carry = 0, v = 0, clear_v = 1;

	if (e->sz)
	{
		uint16_t d = ea_read16(cpu, e), r = 0;
		switch (op)
		{
		case 0x18: r = (uint16_t)(d << 1); carry = (d >> 15) & 1;
			v = ((d ^ r) >> 15) & 1; clear_v = 0; break;
		case 0x19: r = (uint16_t)((int16_t)d >> 1); carry = d & 1; break;
		case 0x1a: r = (uint16_t)(d << 1); carry = (d >> 15) & 1; break;
		case 0x1b: r = (uint16_t)(d >> 1); carry = d & 1; break;
		case 0x1c: carry = (d >> 15) & 1; r = (uint16_t)((d << 1) | carry); break;
		case 0x1d: carry = d & 1; r = (uint16_t)((d >> 1) | (carry << 15)); break;
		case 0x1e: carry = (d >> 15) & 1; r = (uint16_t)((d << 1) | c_in); break;
		default:   carry = d & 1; r = (uint16_t)((d >> 1) | (c_in << 15)); break;
		}
		ea_write16(cpu, e, r);
		set_nz16(cpu, r);
		if (op == 0x1b)
			cpu->sr &= (uint16_t)~FLAG_N;
	}
	else
	{
		uint8_t d = ea_read8(cpu, e), r = 0;
		switch (op)
		{
		case 0x18: r = (uint8_t)(d << 1); carry = (d >> 7) & 1;
			v = ((d ^ r) >> 7) & 1; clear_v = 0; break;
		case 0x19: r = (uint8_t)((int8_t)d >> 1); carry = d & 1; break;
		case 0x1a: r = (uint8_t)(d << 1); carry = (d >> 7) & 1; break;
		case 0x1b: r = (uint8_t)(d >> 1); carry = d & 1; break;
		case 0x1c: carry = (d >> 7) & 1; r = (uint8_t)((d << 1) | carry); break;
		case 0x1d: carry = d & 1; r = (uint8_t)((d >> 1) | (carry << 7)); break;
		case 0x1e: carry = (d >> 7) & 1; r = (uint8_t)((d << 1) | c_in); break;
		default:   carry = d & 1; r = (uint8_t)((d >> 1) | (c_in << 7)); break;
		}
		ea_write8(cpu, e, r);
		set_nz8(cpu, r);
		if (op == 0x1b)
			cpu->sr &= (uint16_t)~FLAG_N;
	}

	cpu->sr &= (uint16_t)~(FLAG_V | FLAG_C);
	if (!clear_v && v)
		cpu->sr |= FLAG_V;
	if (carry)
		cpu->sr |= FLAG_C;
}

static void do_bitop(h8500_t *cpu, ea_t *e, int op, int bit, int bitreg)
{
	uint32_t m, d;

	if (e->sz)
		d = ea_read16(cpu, e);
	else
		d = ea_read8(cpu, e);

	if (bitreg >= 0)
		bit = cpu->r[bitreg];
	m = 1u << (bit & 15);

	if (d & m)
		cpu->sr &= (uint16_t)~FLAG_Z;
	else
		cpu->sr |= FLAG_Z;

	switch (op)
	{
	case 0: d |= m; break;
	case 1: d &= ~m; break;
	case 2: d ^= m; break;
	default: return;
	}

	if (e->sz)
		ea_write16(cpu, e, (uint16_t)d);
	else
		ea_write8(cpu, e, (uint8_t)d);
}

/* ------------------------------------------------------------------ */
/* the general (EA-first) instruction group                           */
/* ------------------------------------------------------------------ */

static int exec_general(h8500_t *cpu, ea_t *e, uint16_t start_pc)
{
	uint8_t op = fetch8(cpu);
	int mode = e->mode;
	int cyc = cyc_src[mode];
	int d;

	switch (op)
	{
	case 0x00:
	{
		uint8_t op2 = fetch8(cpu);
		if ((op2 & 0xf8) == 0x80)
		{
			uint8_t v = ea_read8(cpu, e);
			cpu->r[op2 & 7] = (uint16_t)((cpu->r[op2 & 7] & 0xff00) | v);
			return cyc + 6;
		}
		if ((op2 & 0xf8) == 0x90)
		{
			ea_write8(cpu, e, (uint8_t)cpu->r[op2 & 7]);
			return cyc + 6;
		}
		if ((op2 & 0xf0) == 0xa0 || (op2 & 0xf0) == 0xb0)
		{
			int rd = op2 & 7;
			uint8_t s = ea_read8(cpu, e);
			uint8_t dd = (uint8_t)cpu->r[rd];
			unsigned c = (cpu->sr & FLAG_C) ? 1u : 0u;
			unsigned res;
			uint16_t z = (uint16_t)(cpu->sr & FLAG_Z);
			if ((op2 & 0xf0) == 0xa0)
			{
				res = (dd & 0x0f) + (s & 0x0f) + c;
				if (res >= 0x0a)
					res += 0x06;
				res += (dd & 0xf0) + (s & 0xf0);
				if (res >= 0xa0)
					res += 0x60;
			}
			else
			{
				int t = (int)(dd & 0x0f) - (int)(s & 0x0f) - (int)c;
				if (t < 0)
					t -= 0x06;
				t += (int)(dd & 0xf0) - (int)(s & 0xf0);
				if (t < 0)
					t -= 0x60;
				res = (unsigned)t & 0x1ff;
			}
			cpu->sr &= (uint16_t)~FLAG_C;
			if (res & 0x100)
				cpu->sr |= FLAG_C;
			cpu->r[rd] = (uint16_t)((cpu->r[rd] & 0xff00) | (res & 0xff));
			cpu->sr &= (uint16_t)~FLAG_Z;
			if (!(res & 0xff) && z)
				cpu->sr |= FLAG_Z;
			return 4;
		}
		return -1;
	}

	case 0x04: case 0x06:
	{
		uint8_t imm = fetch8(cpu);
		if (e->sz)
		{
			uint16_t s = (uint16_t)(int16_t)(int8_t)imm;
			if (op == 0x04)
				do_sub16(cpu, ea_read16(cpu, e), s, 0, 0);
			else
			{
				ea_write16(cpu, e, s);
				set_nz16(cpu, s);
				cpu->sr &= (uint16_t)~FLAG_V;
			}
		}
		else
		{
			if (op == 0x04)
				do_sub8(cpu, ea_read8(cpu, e), imm, 0, 0);
			else
			{
				ea_write8(cpu, e, imm);
				set_nz8(cpu, imm);
				cpu->sr &= (uint16_t)~FLAG_V;
			}
		}
		return cyc_rmw[mode] + 1;
	}

	case 0x05: case 0x07:
	{
		uint16_t imm = fetch16(cpu);
		if (e->sz)
		{
			if (op == 0x05)
				do_sub16(cpu, ea_read16(cpu, e), imm, 0, 0);
			else
			{
				ea_write16(cpu, e, imm);
				set_nz16(cpu, imm);
				cpu->sr &= (uint16_t)~FLAG_V;
			}
		}
		else
		{
			if (op == 0x05)
				do_sub8(cpu, ea_read8(cpu, e), (uint8_t)imm, 0, 0);
			else
			{
				ea_write8(cpu, e, (uint8_t)imm);
				set_nz8(cpu, (uint8_t)imm);
				cpu->sr &= (uint16_t)~FLAG_V;
			}
		}
		return cyc_rmw[mode] + 2;
	}

	case 0x08: case 0x09: case 0x0c: case 0x0d:
	{
		int add = (op == 0x08) ? 1 : (op == 0x09) ? 2 : (op == 0x0c) ? -1 : -2;
		if (e->sz)
			ea_write16(cpu, e, do_add16(cpu, ea_read16(cpu, e), (uint16_t)add, 0, 0));
		else
			ea_write8(cpu, e, do_add8(cpu, ea_read8(cpu, e), (uint8_t)add, 0, 0));
		return cyc_rmw[mode];
	}

	case 0x10:                                       /* SWAP Rd */
		if (mode != EM_REG)
			return -1;
		{
			uint16_t v = cpu->r[e->reg];
			v = (uint16_t)((v >> 8) | (v << 8));
			cpu->r[e->reg] = v;
			set_nz16(cpu, v);
			cpu->sr &= (uint16_t)~FLAG_V;
		}
		return 4;

	case 0x11:                                       /* EXTS Rd */
		if (mode != EM_REG)
			return -1;
		{
			uint16_t v = (uint16_t)(int16_t)(int8_t)cpu->r[e->reg];
			cpu->r[e->reg] = v;
			set_nz16(cpu, v);
			cpu->sr &= (uint16_t)~(FLAG_V | FLAG_C);
		}
		return 3;

	case 0x12:                                       /* EXTU Rd */
		if (mode != EM_REG)
			return -1;
		{
			uint16_t v = (uint16_t)(cpu->r[e->reg] & 0xff);
			cpu->r[e->reg] = v;
			set_nz16(cpu, v);
			cpu->sr &= (uint16_t)~(FLAG_N | FLAG_V | FLAG_C);
		}
		return 3;

	case 0x13:                                       /* CLR */
		if (e->sz)
			ea_write16(cpu, e, 0);
		else
			ea_write8(cpu, e, 0);
		cpu->sr &= (uint16_t)~(FLAG_N | FLAG_V | FLAG_C);
		cpu->sr |= FLAG_Z;
		return cyc_rmw[mode];

	case 0x14:                                       /* NEG */
		if (e->sz)
			ea_write16(cpu, e, do_sub16(cpu, 0, ea_read16(cpu, e), 0, 0));
		else
			ea_write8(cpu, e, do_sub8(cpu, 0, ea_read8(cpu, e), 0, 0));
		return cyc_rmw[mode];

	case 0x15:                                       /* NOT */
		if (e->sz)
		{
			uint16_t r = (uint16_t)~ea_read16(cpu, e);
			ea_write16(cpu, e, r);
			set_nz16(cpu, r);
		}
		else
		{
			uint8_t r = (uint8_t)~ea_read8(cpu, e);
			ea_write8(cpu, e, r);
			set_nz8(cpu, r);
		}
		cpu->sr &= (uint16_t)~FLAG_V;
		return cyc_rmw[mode];

	case 0x16:                                       /* TST */
		if (e->sz)
			set_nz16(cpu, ea_read16(cpu, e));
		else
			set_nz8(cpu, ea_read8(cpu, e));
		cpu->sr &= (uint16_t)~(FLAG_V | FLAG_C);
		return cyc;

	case 0x17:                                       /* TAS */
	{
		uint8_t v = ea_read8(cpu, e);
		set_nz8(cpu, v);
		cpu->sr &= (uint16_t)~(FLAG_V | FLAG_C);
		ea_write8(cpu, e, (uint8_t)(v | 0x80));
		return cyc_rmw[mode];
	}

	case 0x18: case 0x19: case 0x1a: case 0x1b:
	case 0x1c: case 0x1d: case 0x1e: case 0x1f:
		do_shift(cpu, e, op);
		return cyc_rmw[mode];

	default:
		break;
	}

	d = op & 7;

	/* the oracle fixes the order in which the two operands are fetched; it is
	 * observable when the EA register is also the operand register */
	switch (op & 0xf8)
	{
	case 0x20:                                       /* ADD:G */
		if (e->sz)
		{
			uint16_t s = ea_read16(cpu, e);
			cpu->r[d] = do_add16(cpu, cpu->r[d], s, 0, 0);
		}
		else
		{
			uint8_t s = ea_read8(cpu, e);
			cpu->r[d] = (uint16_t)((cpu->r[d] & 0xff00) |
				do_add8(cpu, (uint8_t)cpu->r[d], s, 0, 0));
		}
		return cyc;

	case 0x28:                                       /* ADDS */
	{
		uint16_t s = e->sz ? ea_read16(cpu, e)
				   : (uint16_t)(int16_t)(int8_t)ea_read8(cpu, e);
		cpu->r[d] = (uint16_t)(cpu->r[d] + s);
		return cyc;
	}

	case 0x30:                                       /* SUB */
		if (e->sz)
		{
			uint16_t s = ea_read16(cpu, e);
			cpu->r[d] = do_sub16(cpu, cpu->r[d], s, 0, 0);
		}
		else
		{
			uint8_t s = ea_read8(cpu, e);
			cpu->r[d] = (uint16_t)((cpu->r[d] & 0xff00) |
				do_sub8(cpu, (uint8_t)cpu->r[d], s, 0, 0));
		}
		return cyc;

	case 0x38:                                       /* SUBS */
	{
		uint16_t s = e->sz ? ea_read16(cpu, e)
				   : (uint16_t)(int16_t)(int8_t)ea_read8(cpu, e);
		cpu->r[d] = (uint16_t)(cpu->r[d] - s);
		return cyc;
	}

	case 0x40: case 0x50: case 0x60:                 /* OR / AND / XOR */
		if (e->sz)
		{
			uint16_t s = ea_read16(cpu, e);
			uint16_t r = (op & 0xf8) == 0x40 ? (uint16_t)(cpu->r[d] | s)
				   : (op & 0xf8) == 0x50 ? (uint16_t)(cpu->r[d] & s)
							 : (uint16_t)(cpu->r[d] ^ s);
			cpu->r[d] = r;
			set_nz16(cpu, r);
		}
		else
		{
			uint8_t s = ea_read8(cpu, e);
			uint8_t dv = (uint8_t)cpu->r[d];
			uint8_t r = (op & 0xf8) == 0x40 ? (uint8_t)(dv | s)
				  : (op & 0xf8) == 0x50 ? (uint8_t)(dv & s)
							: (uint8_t)(dv ^ s);
			cpu->r[d] = (uint16_t)((cpu->r[d] & 0xff00) | r);
			set_nz8(cpu, r);
		}
		cpu->sr &= (uint16_t)~FLAG_V;
		return cyc;

	case 0x70:                                       /* CMP:G <EAs>,Rd */
		if (e->sz)
		{
			uint16_t dv = cpu->r[d];
			do_sub16(cpu, dv, ea_read16(cpu, e), 0, 0);
		}
		else
		{
			uint8_t dv = (uint8_t)cpu->r[d];
			do_sub8(cpu, dv, ea_read8(cpu, e), 0, 0);
		}
		return cyc;

	case 0x48: case 0x58: case 0x68:                 /* BSET/BCLR/BNOT Rs, or xxxC */
		if (mode == EM_IMM8 || mode == EM_IMM16)
		{
			int c = op & 7;
			uint16_t imm = (mode == EM_IMM16)
				? (uint16_t)((e->ext[0] << 8) | e->ext[1])
				: (uint16_t)e->ext[0];
			uint16_t cur, nv;
			cpu->no_irq = true;
			if (c == 2 || c == 6)
				return cyc;
			if (e->sz && c != 0)
				return cyc;
			cur = cr_read(cpu, c);
			switch (op & 0xf8)
			{
			case 0x48: nv = (uint16_t)(cur | imm); break;
			case 0x58: nv = (uint16_t)(cur & imm); break;
			default:   nv = (uint16_t)(cur ^ imm); break;
			}
			cr_write(cpu, c, nv);
			if (c != 0 && c != 1)
			{
				set_nz8(cpu, (uint8_t)cr_read(cpu, c));
				cpu->sr &= (uint16_t)~FLAG_V;
			}
			return cyc;
		}
		do_bitop(cpu, e, ((op & 0xf8) == 0x48) ? 0 : ((op & 0xf8) == 0x58) ? 1 : 2,
			 cpu->r[d], -1);
		return cyc_rmw[mode];

	case 0x78:                                       /* BTST Rs */
		do_bitop(cpu, e, 3, 0, d);
		return cyc;

	case 0x80:                                       /* MOV:G <EAs>,Rd */
		if (e->sz)
		{
			uint16_t v = ea_read16(cpu, e);
			cpu->r[d] = v;
			set_nz16(cpu, v);
		}
		else
		{
			uint8_t v = ea_read8(cpu, e);
			cpu->r[d] = (uint16_t)((cpu->r[d] & 0xff00) | v);
			set_nz8(cpu, v);
		}
		cpu->sr &= (uint16_t)~FLAG_V;
		return cyc;

	case 0x88:                                       /* LDC <EAs>,CR */
	{
		int c = op & 7;
		cpu->no_irq = true;
		if (e->sz)
		{
			uint16_t v;
			if (c != 0 && c != 3 && c != 4 && c != 5)
				return cyc;
			v = ea_read16(cpu, e);
			if (c == 0)
				cpu->sr = (uint16_t)(v & SR_MASK);
			else
				cr_write(cpu, c, (uint16_t)(v & 0xff));
		}
		else if (c == 4 && ea_is_byte_stack(e))
		{
			uint16_t v = mem_read16(cpu, e->addr);
			ea_commit(cpu, e);
			cpu->ep = (uint8_t)(v >> 8);
			cpu->dp = (uint8_t)v;
		}
		else
		{
			uint8_t v = ea_read8(cpu, e);
			if (c != 0 && c != 2 && c != 6)
				cr_write(cpu, c, v);
		}
		return cyc;
	}

	case 0x90:                                       /* MOV:G Rs,<EAd> / XCH */
		if (mode == EM_REG)
		{
			uint16_t t = cpu->r[e->reg];
			cpu->r[e->reg] = cpu->r[d];
			cpu->r[d] = t;
			return 4;
		}
		if (e->sz)
		{
			uint16_t v = cpu->r[d];
			ea_write16(cpu, e, v);
			set_nz16(cpu, v);
		}
		else
		{
			uint8_t v = (uint8_t)cpu->r[d];
			ea_write8(cpu, e, v);
			set_nz8(cpu, v);
		}
		cpu->sr &= (uint16_t)~FLAG_V;
		return cyc_rmw[mode];

	case 0x98:                                       /* STC CR,<EAd> */
	{
		int c = op & 7;
		if (e->sz)
		{
			if (c == 0)
				ea_write16(cpu, e, (uint16_t)(cpu->sr & SR_MASK));
			else if (c == 3 || c == 4 || c == 5)
			{
				uint8_t v = (uint8_t)cr_read(cpu, c);
				ea_write16(cpu, e, (uint16_t)((v << 8) | v));
			}
		}
		else if (c == 4 && ea_is_byte_stack(e))
		{
			mem_write16(cpu, e->addr, (uint16_t)((cpu->ep << 8) | cpu->dp));
			ea_commit(cpu, e);
		}
		else
		{
			ea_write8(cpu, e, (uint8_t)cr_read(cpu, c));
		}
		return cyc_rmw[mode];
	}

	case 0xa0:                                       /* ADDX */
		if (e->sz)
		{
			uint16_t s = ea_read16(cpu, e);
			cpu->r[d] = do_add16(cpu, cpu->r[d], s, (cpu->sr & FLAG_C) ? 1u : 0u, 1);
		}
		else
		{
			uint8_t s = ea_read8(cpu, e);
			cpu->r[d] = (uint16_t)((cpu->r[d] & 0xff00) |
				do_add8(cpu, (uint8_t)cpu->r[d], s, (cpu->sr & FLAG_C) ? 1u : 0u, 1));
		}
		return cyc;

	case 0xb0:                                       /* SUBX */
		if (e->sz)
		{
			uint16_t s = ea_read16(cpu, e);
			cpu->r[d] = do_sub16(cpu, cpu->r[d], s, (cpu->sr & FLAG_C) ? 1u : 0u, 1);
		}
		else
		{
			uint8_t s = ea_read8(cpu, e);
			cpu->r[d] = (uint16_t)((cpu->r[d] & 0xff00) |
				do_sub8(cpu, (uint8_t)cpu->r[d], s, (cpu->sr & FLAG_C) ? 1u : 0u, 1));
		}
		return cyc;

	case 0xa8:                                       /* MULXU */
		if (e->sz)
		{
			uint16_t s = ea_read16(cpu, e);
			uint32_t r = (uint32_t)cpu->r[d] * s;
			cpu->r[d] = (uint16_t)(r >> 16);
			cpu->r[(d + 1) & 7] = (uint16_t)r;
			cpu->sr &= (uint16_t)~(FLAG_N | FLAG_Z | FLAG_V | FLAG_C);
			if (r & 0x80000000u) cpu->sr |= FLAG_N;
			if (!r) cpu->sr |= FLAG_Z;
			return 24;
		}
		else
		{
			uint8_t s = ea_read8(cpu, e);
			uint16_t r = (uint16_t)((cpu->r[d] & 0xff) * s);
			cpu->r[d] = r;
			set_nz16(cpu, r);
			cpu->sr &= (uint16_t)~(FLAG_V | FLAG_C);
			return 14;
		}

	case 0xb8:                                       /* DIVXU */
		if (e->sz)
		{
			uint32_t dividend = ((uint32_t)cpu->r[d] << 16) | cpu->r[(d + 1) & 7];
			uint16_t s = ea_read16(cpu, e);
			if (!s)
			{
				cpu->sr &= (uint16_t)~(FLAG_N | FLAG_V | FLAG_C);
				cpu->sr |= FLAG_Z;
				exception(cpu, VEC_DIVZERO, start_pc, -1);
				return 22;
			}
			if ((dividend >> 16) >= s)
			{
				cpu->sr &= (uint16_t)~(FLAG_N | FLAG_Z | FLAG_C);
				cpu->sr |= FLAG_V;
				return 26;
			}
			{
				uint16_t q = (uint16_t)(dividend / s);
				uint16_t rem = (uint16_t)(dividend % s);
				cpu->r[d] = rem;
				cpu->r[(d + 1) & 7] = q;
				cpu->sr &= (uint16_t)~FLAG_C;
				set_nz16(cpu, q);
				cpu->sr &= (uint16_t)~FLAG_V;
				return 26;
			}
		}
		else
		{
			uint16_t dividend = cpu->r[d];
			uint8_t s = ea_read8(cpu, e);
			if (!s)
			{
				cpu->sr &= (uint16_t)~(FLAG_N | FLAG_V | FLAG_C);
				cpu->sr |= FLAG_Z;
				exception(cpu, VEC_DIVZERO, start_pc, -1);
				return 16;
			}
			if ((dividend >> 8) >= s)
			{
				cpu->sr &= (uint16_t)~(FLAG_N | FLAG_Z | FLAG_C);
				cpu->sr |= FLAG_V;
				return 20;
			}
			{
				uint8_t q = (uint8_t)(dividend / s);
				uint8_t rem = (uint8_t)(dividend % s);
				cpu->r[d] = (uint16_t)((rem << 8) | q);
				cpu->sr &= (uint16_t)~FLAG_C;
				set_nz8(cpu, q);
				cpu->sr &= (uint16_t)~FLAG_V;
				return 20;
			}
		}

	default:
		break;
	}

	switch (op & 0xf0)
	{
	case 0xc0: do_bitop(cpu, e, 0, op & 15, -1); return cyc_rmw[mode];
	case 0xd0: do_bitop(cpu, e, 1, op & 15, -1); return cyc_rmw[mode];
	case 0xe0: do_bitop(cpu, e, 2, op & 15, -1); return cyc_rmw[mode];
	case 0xf0: do_bitop(cpu, e, 3, op & 15, -1); return cyc;
	default: break;
	}

	return -1;
}

/* ------------------------------------------------------------------ */
/* the special (opcode-first) instruction group                       */
/* ------------------------------------------------------------------ */

static int exec_11(h8500_t *cpu, uint16_t start_pc)
{
	uint8_t b = fetch8(cpu);
	uint16_t target;
	int n = b & 7;
	(void)start_pc;

	switch (b)
	{
	case 0x14:                                       /* PRTD #xx:8 */
	{
		int8_t imm = (int8_t)fetch8(cpu);
		cpu->cp = (uint8_t)pop16(cpu);
		cpu->pc = pop16(cpu);
		cpu->r[7] = (uint16_t)(cpu->r[7] + imm);
		return 13;
	}
	case 0x1c:                                       /* PRTD #xx:16 */
	{
		int16_t imm = (int16_t)fetch16(cpu);
		cpu->cp = (uint8_t)pop16(cpu);
		cpu->pc = pop16(cpu);
		cpu->r[7] = (uint16_t)(cpu->r[7] + imm);
		return 13;
	}
	case 0x19:                                       /* PRTS */
		cpu->cp = (uint8_t)pop16(cpu);
		cpu->pc = pop16(cpu);
		return 12;
	default:
		break;
	}

	switch (b & 0xf8)
	{
	case 0xc0:                                       /* PJMP @Rn */
		cpu->cp = (uint8_t)cpu->r[n];
		cpu->pc = cpu->r[(n + 1) & 7];
		return 8;
	case 0xc8:                                       /* PJSR @Rn */
	{
		uint8_t ncp = (uint8_t)cpu->r[n];
		uint16_t npc = cpu->r[(n + 1) & 7];
		push16(cpu, cpu->pc);
		push16(cpu, cpu->cp);
		cpu->cp = ncp;
		cpu->pc = npc;
		return 13;
	}
	case 0xd0:                                       /* JMP @Rn */
		cpu->pc = cpu->r[n];
		return 6;
	case 0xd8:                                       /* JSR @Rn */
		target = cpu->r[n];
		push16(cpu, cpu->pc);
		cpu->pc = target;
		return 9;
	case 0xe0:                                       /* JMP @(d:8,Rn) */
		target = (uint16_t)(cpu->r[n] + (int8_t)fetch8(cpu));
		cpu->pc = target;
		return 7;
	case 0xe8:                                       /* JSR @(d:8,Rn) */
		target = (uint16_t)(cpu->r[n] + (int8_t)fetch8(cpu));
		push16(cpu, cpu->pc);
		cpu->pc = target;
		return 9;
	case 0xf0:                                       /* JMP @(d:16,Rn) */
		target = (uint16_t)(cpu->r[n] + (int16_t)fetch16(cpu));
		cpu->pc = target;
		return 8;
	case 0xf8:                                       /* JSR @(d:16,Rn) */
		target = (uint16_t)(cpu->r[n] + (int16_t)fetch16(cpu));
		push16(cpu, cpu->pc);
		cpu->pc = target;
		return 10;
	default:
		break;
	}

	return -1;
}

int h8500_exec_one(h8500_t *cpu)
{
	uint16_t start_pc = cpu->pc;
	uint8_t b0 = fetch8(cpu);
	ea_t e;
	int cyc;

	switch (b0)
	{
	case 0x00:                                       /* NOP */
		return 2;

	case 0x01: case 0x06: case 0x07:                 /* SCB/F, SCB/NE, SCB/EQ */
	{
		uint8_t b1 = fetch8(cpu);
		int8_t disp;
		int n;
		if ((b1 & 0xf8) != 0xb8)
			break;
		n = b1 & 7;
		disp = (int8_t)fetch8(cpu);
		if (b0 == 0x06 && !(cpu->sr & FLAG_Z))
			return 3;
		if (b0 == 0x07 && (cpu->sr & FLAG_Z))
			return 3;
		cpu->r[n] = (uint16_t)(cpu->r[n] - 1);
		if (cpu->r[n] == 0xffff)
			return 4;
		cpu->pc = (uint16_t)(cpu->pc + disp);
		return 8;
	}

	case 0x02:                                       /* LDM */
	{
		uint8_t list = fetch8(cpu);
		uint16_t sp = cpu->r[7];
		int n, count = 0;
		for (n = 0; n < 8; n++)
		{
			if (!(list & (1u << n)))
				continue;
			count++;
			{
				uint16_t v = mem_read16(cpu, ((uint32_t)cpu->tp << 16) | sp);
				if (n != 7)
					cpu->r[n] = v;
			}
			sp = (uint16_t)(sp + 2);
		}
		cpu->r[7] = sp;
		return 6 + 4 * count;
	}

	case 0x12:                                       /* STM */
	{
		uint8_t list = fetch8(cpu);
		uint16_t sp = cpu->r[7];
		int n, count = 0;
		for (n = 7; n >= 0; n--)
		{
			if (!(list & (1u << n)))
				continue;
			count++;
			sp = (uint16_t)(sp - 2);
			mem_write16(cpu, ((uint32_t)cpu->tp << 16) | sp,
				(n == 7) ? (uint16_t)(cpu->r[7] - 2) : cpu->r[n]);
		}
		cpu->r[7] = sp;
		return 6 + 3 * count;
	}

	case 0x03:                                       /* PJSR @aa:24 */
	{
		uint8_t page = fetch8(cpu);
		uint16_t addr = fetch16(cpu);
		push16(cpu, cpu->pc);
		push16(cpu, cpu->cp);
		cpu->cp = page;
		cpu->pc = addr;
		return 15;
	}

	case 0x13:                                       /* PJMP @aa:24 */
	{
		uint8_t page = fetch8(cpu);
		uint16_t addr = fetch16(cpu);
		cpu->cp = page;
		cpu->pc = addr;
		return 9;
	}

	case 0x08:                                       /* TRAPA #VEC */
	{
		uint8_t b1 = fetch8(cpu);
		if ((b1 & 0xf0) != 0x10)
			break;
		exception(cpu, VEC_TRAPA + (b1 & 15), cpu->pc, -1);
		return 22;
	}

	case 0x09:                                       /* TRAP/VS */
		if (cpu->sr & FLAG_V)
		{
			exception(cpu, VEC_TRAPV, cpu->pc, -1);
			return 23;
		}
		return 3;

	case 0x0a:                                       /* RTE */
		cpu->sr = (uint16_t)(pop16(cpu) & SR_MASK);
		cpu->cp = (uint8_t)pop16(cpu);
		cpu->pc = pop16(cpu);
		cpu->no_irq = true;
		return 15;

	case 0x0e:                                       /* BSR d:8 */
	{
		int8_t disp = (int8_t)fetch8(cpu);
		push16(cpu, cpu->pc);
		cpu->pc = (uint16_t)(cpu->pc + disp);
		return 9;
	}

	case 0x1e:                                       /* BSR d:16 */
	{
		int16_t disp = (int16_t)fetch16(cpu);
		push16(cpu, cpu->pc);
		cpu->pc = (uint16_t)(cpu->pc + disp);
		return 9;
	}

	case 0x0f:                                       /* UNLK */
		cpu->r[7] = cpu->r[6];
		cpu->r[6] = pop16(cpu);
		return 5;

	case 0x10:                                       /* JMP @aa:16 */
		cpu->pc = fetch16(cpu);
		return 7;

	case 0x18:                                       /* JSR @aa:16 */
	{
		uint16_t addr = fetch16(cpu);
		push16(cpu, cpu->pc);
		cpu->pc = addr;
		return 9;
	}

	case 0x11:
		cyc = exec_11(cpu, start_pc);
		if (cyc < 0)
			break;
		return cyc;

	case 0x14:                                       /* RTD #xx:8 */
	{
		int8_t imm = (int8_t)fetch8(cpu);
		cpu->pc = pop16(cpu);
		cpu->r[7] = (uint16_t)(cpu->r[7] + imm);
		return 9;
	}

	case 0x1c:                                       /* RTD #xx:16 */
	{
		int16_t imm = (int16_t)fetch16(cpu);
		cpu->pc = pop16(cpu);
		cpu->r[7] = (uint16_t)(cpu->r[7] + imm);
		return 9;
	}

	case 0x17:                                       /* LINK FP,#xx:8 */
	{
		int8_t imm = (int8_t)fetch8(cpu);
		push16(cpu, cpu->r[6]);
		cpu->r[6] = cpu->r[7];
		cpu->r[7] = (uint16_t)(cpu->r[7] + imm);
		return 6;
	}

	case 0x1f:                                       /* LINK FP,#xx:16 */
	{
		int16_t imm = (int16_t)fetch16(cpu);
		push16(cpu, cpu->r[6]);
		cpu->r[6] = cpu->r[7];
		cpu->r[7] = (uint16_t)(cpu->r[7] + imm);
		return 7;
	}

	case 0x19:                                       /* RTS */
		cpu->pc = pop16(cpu);
		return 8;

	case 0x1a:                                       /* SLEEP */
		cpu->sleeping = true;
		return 2;

	default:
		break;
	}

	if (b0 >= 0x20 && b0 <= 0x2f)                    /* Bcc d:8 */
	{
		int8_t disp = (int8_t)fetch8(cpu);
		if (!cond_true(cpu, b0 & 15))
			return 3;
		cpu->pc = (uint16_t)(cpu->pc + disp);
		return 7;
	}

	if (b0 >= 0x30 && b0 <= 0x3f)                    /* Bcc d:16 */
	{
		int16_t disp = (int16_t)fetch16(cpu);
		if (!cond_true(cpu, b0 & 15))
			return 3;
		cpu->pc = (uint16_t)(cpu->pc + disp);
		return 7;
	}

	if (b0 >= 0x40 && b0 <= 0x9f)
	{
		int n = b0 & 7;
		switch (b0 & 0xf8)
		{
		case 0x40:                                   /* CMP:E #xx:8,Rd */
			do_sub8(cpu, (uint8_t)cpu->r[n], fetch8(cpu), 0, 0);
			return 2;
		case 0x48:                                   /* CMP:I #xx:16,Rd */
			do_sub16(cpu, cpu->r[n], fetch16(cpu), 0, 0);
			return 3;
		case 0x50:                                   /* MOV:E #xx:8,Rd */
		{
			uint8_t v = fetch8(cpu);
			cpu->r[n] = (uint16_t)((cpu->r[n] & 0xff00) | v);
			set_nz8(cpu, v);
			cpu->sr &= (uint16_t)~FLAG_V;
			return 2;
		}
		case 0x58:                                   /* MOV:I #xx:16,Rd */
		{
			uint16_t v = fetch16(cpu);
			cpu->r[n] = v;
			set_nz16(cpu, v);
			cpu->sr &= (uint16_t)~FLAG_V;
			return 3;
		}
		default:
			break;
		}

		if (b0 >= 0x60 && b0 <= 0x7f)                /* MOV:L / MOV:S @aa:8 */
		{
			int sz = (b0 >> 3) & 1;
			uint32_t addr = (uint32_t)((cpu->br << 8) | fetch8(cpu));
			if (b0 < 0x70)
			{
				if (sz)
				{
					uint16_t v = mem_read16(cpu, addr);
					cpu->r[n] = v;
					set_nz16(cpu, v);
				}
				else
				{
					uint8_t v = mem_read8(cpu, addr);
					cpu->r[n] = (uint16_t)((cpu->r[n] & 0xff00) | v);
					set_nz8(cpu, v);
				}
			}
			else
			{
				if (sz)
				{
					uint16_t v = cpu->r[n];
					mem_write16(cpu, addr, v);
					set_nz16(cpu, v);
				}
				else
				{
					uint8_t v = (uint8_t)cpu->r[n];
					mem_write8(cpu, addr, v);
					set_nz8(cpu, v);
				}
			}
			cpu->sr &= (uint16_t)~FLAG_V;
			return 5;
		}

		/* 0x80-0x9f: MOV:F @(d:8,R6) */
		{
			int sz = (b0 >> 3) & 1;
			int8_t disp = (int8_t)fetch8(cpu);
			uint32_t addr = ((uint32_t)cpu->tp << 16) |
				(uint32_t)((cpu->r[6] + disp) & 0xffff);
			if (b0 < 0x90)
			{
				if (sz)
				{
					uint16_t v = mem_read16(cpu, addr);
					cpu->r[n] = v;
					set_nz16(cpu, v);
				}
				else
				{
					uint8_t v = mem_read8(cpu, addr);
					cpu->r[n] = (uint16_t)((cpu->r[n] & 0xff00) | v);
					set_nz8(cpu, v);
				}
			}
			else
			{
				if (sz)
				{
					uint16_t v = cpu->r[n];
					mem_write16(cpu, addr, v);
					set_nz16(cpu, v);
				}
				else
				{
					uint8_t v = (uint8_t)cpu->r[n];
					mem_write8(cpu, addr, v);
					set_nz8(cpu, v);
				}
			}
			cpu->sr &= (uint16_t)~FLAG_V;
			return 5;
		}
	}

	if (b0 == 0x04 || b0 == 0x05 || b0 == 0x0c || b0 == 0x0d ||
	    b0 == 0x15 || b0 == 0x1d || b0 >= 0xa0)
	{
		if (ea_decode(cpu, &e, b0))
		{
			cyc = exec_general(cpu, &e, start_pc);
			if (cyc >= 0)
				return cyc;
		}
	}

	cpu->pc = start_pc;
	exception(cpu, VEC_INVALID, start_pc, -1);
	return 22;
}

/* ------------------------------------------------------------------ */
/* public interface                                                   */
/* ------------------------------------------------------------------ */

static void io_map(h8500_t *cpu, int base, int len, int kind, int unit)
{
	int i;
	for (i = 0; i < len; i++)
	{
		cpu->io_kind[base + i] = (uint8_t)kind;
		cpu->io_unit[base + i] = (uint8_t)unit;
	}
}

static void io_build(h8500_t *cpu)
{
	const h8500_variant_t *v = cpu->var;
	int i;

	memset(cpu->io_kind, IOK_PLAIN, sizeof(cpu->io_kind));
	memset(cpu->io_unit, 0, sizeof(cpu->io_unit));

	for (i = 1; i <= v->port_count; i++)
	{
		if (v->port_ddr[i] >= 0)
			io_map(cpu, v->port_ddr[i], 1, IOK_PORT_DDR, i);
		if (v->port_dr[i] >= 0)
			io_map(cpu, v->port_dr[i], 1, IOK_PORT_DR, i);
	}
	for (i = 0; i < v->frt_count; i++)
		io_map(cpu, v->frt_reg[i], 10, IOK_FRT, i);
	io_map(cpu, v->tmr_reg, 5, IOK_TMR, 0);
	for (i = 0; i < v->sci_count; i++)
		io_map(cpu, v->sci_reg[i], 6, IOK_SCI, i);
	io_map(cpu, v->adc_reg, 10, IOK_ADC, 0);
	if (v->wdt_reg >= 0)
		io_map(cpu, v->wdt_reg, 2, IOK_WDT, 0);
	io_map(cpu, v->ipr_reg, 4, IOK_IPR, 0);
	for (i = 0; i < 2; i++)
		if (v->ctl_reg[i] >= 0)
			io_map(cpu, v->ctl_reg[i], 1, IOK_CTL, i);
}

void h8500_init(h8500_t *cpu, const h8500_bus_t *bus)
{
	h8500_init_model(cpu, bus, H8500_H8510);
}

void h8500_init_model(h8500_t *cpu, const h8500_bus_t *bus, int model)
{
	memset(cpu, 0, sizeof(*cpu));
	cpu->bus = *bus;
	cpu->var = h8500_variant(model);
	io_build(cpu);
}

void h8500_map(h8500_t *cpu, uint32_t base, uint32_t size, uint8_t *data, bool writable)
{
	h8500_region_t *r;
	if (cpu->region_count >= H8500_MAX_REGIONS)
		return;
	r = &cpu->regions[cpu->region_count++];
	r->base = base;
	r->size = size;
	r->data = data;
	r->writable = writable;
}

void h8500_reset(h8500_t *cpu)
{
	const h8500_variant_t *v = cpu->var;
	int i;

	cpu->sr = 0x0700;
	cpu->cp = cpu->dp = cpu->ep = cpu->tp = cpu->br = 0;
	cpu->sleeping = false;
	cpu->no_irq = false;
	cpu->irq_req = 0;
	cpu->irq_pend[0] = cpu->irq_pend[1] = cpu->irq_pend[2] = 0;

	memset(cpu->io, 0, sizeof(cpu->io));
	for (i = 1; i <= v->port_count; i++)
		if (v->port_ddr[i] >= 0)
			cpu->io[v->port_ddr[i]] = v->port_ddr_reset[i];
	cpu->io[v->tmr_reg + T_TCSR] = 0x10;
	cpu->io[v->tmr_reg + T_TCORA] = cpu->io[v->tmr_reg + T_TCORB] = 0xff;
	for (i = 0; i < v->frt_count; i++)
	{
		int b = v->frt_reg[i];
		cpu->io[b + F_OCRAH] = cpu->io[b + F_OCRAL] = 0xff;
		cpu->io[b + F_OCRBH] = cpu->io[b + F_OCRBL] = 0xff;
	}
	for (i = 0; i < v->sci_count; i++)
	{
		int b = v->sci_reg[i];
		cpu->io[b + S_SSR] = 0x84;
		cpu->io[b + S_BRR] = 0xff;
		cpu->io[b + S_TDR] = 0xff;
	}

	memset(cpu->frt_count, 0, sizeof(cpu->frt_count));
	memset(cpu->frt_prescale, 0, sizeof(cpu->frt_prescale));
	memset(cpu->frt_temp, 0, sizeof(cpu->frt_temp));
	cpu->tmr_count = 0;
	cpu->tmr_prescale = 0;
	cpu->wdt_prescale = 0;
	cpu->adc_busy = 0;
	cpu->adc_channel = 0;
	memset(cpu->sci, 0, sizeof(cpu->sci));

	for (i = 1; i <= v->port_count; i++)
	{
		cpu->port_out[i] = 0xffff;
		port_update(cpu, i);
	}

	cpu->cp = (uint8_t)mem_read16(cpu, 0);
	cpu->pc = mem_read16(cpu, 2);
}

int h8500_step(h8500_t *cpu)
{
	int cycles;

	if (!cpu->no_irq)
	{
		int level;
		int vector = irq_select(cpu, &level);
		if (vector >= 0)
		{
			take_interrupt(cpu, vector, level);
			cycles = 20;
			cpu->cycles += (uint64_t)cycles;
			peripherals_tick(cpu, (uint32_t)cycles);
			return cycles;
		}
	}
	cpu->no_irq = false;

	if (cpu->sleeping)
	{
		cpu->cycles++;
		peripherals_tick(cpu, 1);
		return 1;
	}

	cycles = exec_one(cpu);
	if (cycles < 1)
		cycles = 1;
	cpu->cycles += (uint64_t)cycles;
	peripherals_tick(cpu, (uint32_t)cycles);
	return cycles;
}

int h8500_run(h8500_t *cpu, int cycles)
{
	int ran = 0;
#ifdef SCEMU_H8500_JIT
	if (cpu->jit && cpu->jit_enabled)
		return h8500_jit_run(cpu, cycles);
#endif
	while (ran < cycles)
		ran += h8500_step(cpu);
	return ran;
}

/* ---------------------------------------------------------------- the state */

static bool state_restored(void *user)
{
	h8500_t *cpu = user;
	cpu->irq_ready = false;
	cpu->jit_pending = 0;
	cpu->jit_limit = 0;
	cpu->jit_deadline = 0;
	return true;
}

void h8500_state(h8500_t *cpu, state_registry_t *reg)
{
	state_var(reg, cpu->pc);
	state_var(reg, cpu->sr);
	state_var(reg, cpu->cp);
	state_var(reg, cpu->dp);
	state_var(reg, cpu->ep);
	state_var(reg, cpu->tp);
	state_var(reg, cpu->br);
	state_array(reg, cpu->r);
	state_bool(reg, cpu->sleeping);
	state_var(reg, cpu->irq_lines);
	state_var(reg, cpu->irq_req);
	state_array(reg, cpu->irq_pend);
	state_bool(reg, cpu->no_irq);
	state_array(reg, cpu->io);
	state_block(reg, cpu->frt_count, (size_t)cpu->var->frt_count);
	state_block(reg, cpu->frt_prescale, (size_t)cpu->var->frt_count);
	if (cpu->var->frt_temp)
		state_block(reg, cpu->frt_temp, (size_t)cpu->var->frt_count);
	state_var(reg, cpu->tmr_count);
	state_var(reg, cpu->tmr_prescale);
	state_var(reg, cpu->wdt_prescale);
	state_var(reg, cpu->adc_busy);
	state_var(reg, cpu->adc_channel);
	state_field(reg, cpu->sci, 2, rx_byte);
	state_bool_field(reg, cpu->sci, 2, rx_pending);
	state_field(reg, cpu->sci, 2, tx_timer);
	state_field(reg, cpu->sci, 2, tx_shift);
	state_field(reg, cpu->sci, 2, ssr_read);
	state_bool_field(reg, cpu->sci, 2, tx_busy);
	state_block(reg, cpu->port_out, (size_t)cpu->var->port_count + 1);
	if (cpu->var->ram_size)
		state_block(reg, cpu->iram, cpu->var->ram_size);
	state_var(reg, cpu->cycles);
	state_after_load(reg, state_restored, cpu);
}
