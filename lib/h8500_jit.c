#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include "h8500_jit.h"
#include "jit.h"

#if SLJIT_64BIT_ARCHITECTURE

#define FLAG_C 0x1
#define FLAG_V 0x2
#define FLAG_Z 0x4
#define FLAG_N 0x8
#define FLAGS_ALL 0xf

#define BLOCK_MAX_INSNS 48
#define SLOT_CHUNK 1024
#define PAGE_SHIFT 12
#define PAGE_COUNT (1u << (24 - PAGE_SHIFT))

typedef sljit_sw (SLJIT_FUNC *block_fn)(h8500_t *cpu, sljit_sw pending, sljit_sw limit);

typedef struct slot
{
	void *target;
	uint32_t key;
} slot_t;

typedef struct slot_chunk
{
	slot_t slots[SLOT_CHUNK];
	struct slot_chunk *next;
	uint32_t used;
} slot_chunk_t;

typedef struct block
{
	uint32_t key;
	block_fn fn;
	void *body;
	void *code;
	size_t size;
} block_t;

struct h8500_jit
{
	jit_alloc_t *alloc;
	block_t *table;
	uint32_t table_size;
	uint32_t count;
	size_t code_size;
	slot_chunk_t *chunks;
	void *exit_code;
	void *exit_body;
	uint8_t *rd8[PAGE_COUNT];
	uint8_t *rd16[PAGE_COUNT];
	uint8_t *wr8[PAGE_COUNT];
	uint8_t *wr16[PAGE_COUNT];
	uint32_t limit_base;
	uint32_t io_base, io_size, addr_mask;
	uint64_t stat_blocks_run;
	uint64_t stat_fallbacks;
	uint64_t stat_interrupts;
	uint64_t stat_flushes;
};

#define KEY_NONE 0xffffffffu

/* ------------------------------------------------------------------ */
/* the dispatcher                                                     */
/* ------------------------------------------------------------------ */

static void recompute(h8500_t *cpu)
{
	int level;
	uint32_t h = h8500_tick_horizon(cpu);
	uint64_t left = cpu->jit_deadline > cpu->cycles ? cpu->jit_deadline - cpu->cycles : 0;
	cpu->irq_ready = h8500_irq_select(cpu, &level) >= 0;
	if (left < h)
		h = (uint32_t)left;
	cpu->jit->limit_base = h;
	cpu->jit_limit = cpu->irq_ready ? 0 : h;
}

static void flush(h8500_t *cpu)
{
	uint32_t p = cpu->jit_pending;
	cpu->jit_pending = 0;
	if (p)
	{
		cpu->cycles += p;
		h8500_peripherals_tick(cpu, p);
	}
	recompute(cpu);
}

static void quick_irq(h8500_t *cpu)
{
	int level;
	cpu->irq_ready = (cpu->irq_pend[0] | cpu->irq_pend[1] | cpu->irq_pend[2]) != 0
		&& h8500_irq_select(cpu, &level) >= 0;
	cpu->jit_limit = cpu->irq_ready ? 0 : cpu->jit->limit_base;
}

static void fallback(h8500_t *cpu)
{
	int cyc;
	flush(cpu);
	cyc = h8500_exec_one(cpu);
	if (cyc < 1)
		cyc = 1;
	cpu->jit_pending = (uint32_t)cyc;
	recompute(cpu);
	cpu->jit->stat_fallbacks++;
}

static block_t *lookup(struct h8500_jit *j, uint32_t key)
{
	uint32_t i = (key * 2654435761u) >> 8;
	for (;;)
	{
		block_t *b = &j->table[i & (j->table_size - 1)];
		if (b->key == key || b->key == KEY_NONE)
			return b;
		i++;
	}
}

static bool grow(struct h8500_jit *j)
{
	uint32_t n = j->table_size * 2, i;
	block_t *t = malloc(n * sizeof(*t));
	block_t *old = j->table;
	uint32_t old_size = j->table_size;
	if (!t)
		return false;
	for (i = 0; i < n; i++)
		t[i].key = KEY_NONE;
	j->table = t;
	j->table_size = n;
	for (i = 0; i < old_size; i++)
		if (old[i].key != KEY_NONE)
			*lookup(j, old[i].key) = old[i];
	free(old);
	return true;
}

static block_t *translate(h8500_t *cpu, uint32_t key);

int h8500_jit_run(h8500_t *cpu, int cycles)
{
	struct h8500_jit *j = cpu->jit;
	uint64_t start = cpu->cycles;

	cpu->jit_pending = 0;
	cpu->jit_deadline = start + (uint64_t)cycles;
	recompute(cpu);

	for (;;)
	{
		if (cpu->jit_pending >= j->limit_base)
		{
			flush(cpu);
			j->stat_flushes++;
		}
		else
			quick_irq(cpu);
		if (cpu->cycles + cpu->jit_pending >= cpu->jit_deadline)
			break;

		if (!cpu->no_irq && cpu->irq_ready)
		{
			int level;
			int vector = h8500_irq_select(cpu, &level);
			flush(cpu);
			h8500_take_interrupt(cpu, vector, level);
			cpu->jit_pending = 20;
			recompute(cpu);
			j->stat_interrupts++;
			continue;
		}
		cpu->no_irq = false;

		if (cpu->sleeping)
		{
			uint32_t n = cpu->jit_limit > cpu->jit_pending ? cpu->jit_limit - cpu->jit_pending : 1;
			cpu->jit_pending += n;
			continue;
		}

		{
			uint32_t key = ((uint32_t)cpu->cp << 16) | cpu->pc;
			block_t *b = lookup(j, key);
			if (b->key == KEY_NONE)
				b = translate(cpu, key);
			if (b && b->fn)
			{
				cpu->jit_pending = (uint32_t)b->fn(cpu, (sljit_sw)cpu->jit_pending, (sljit_sw)cpu->jit_limit);
				j->stat_blocks_run++;
			}
			else
				fallback(cpu);
		}
	}

	flush(cpu);
	return (int)(cpu->cycles - start);
}

/* ------------------------------------------------------------------ */
/* helpers the translated code calls                                  */
/* ------------------------------------------------------------------ */

/* the register file, wherever the part keeps it and through whatever page
 * the access reached it */
static int is_internal(h8500_t *cpu, uint32_t addr)
{
	addr &= cpu->var->addr_mask;
	return addr - cpu->var->io_base < cpu->var->io_size;
}

static sljit_sw SLJIT_FUNC hread8(h8500_t *cpu, sljit_sw addr)
{
	if (is_internal(cpu, (uint32_t)addr))
		flush(cpu);
	return h8500_mem_read8(cpu, (uint32_t)addr);
}

static sljit_sw SLJIT_FUNC hread16(h8500_t *cpu, sljit_sw addr)
{
	uint32_t a = (uint32_t)addr;
	if (is_internal(cpu, a) || is_internal(cpu, a + 1))
		flush(cpu);
	return h8500_mem_read16(cpu, a);
}

static void SLJIT_FUNC hwrite8(h8500_t *cpu, sljit_sw addr, sljit_sw data)
{
	uint32_t a = (uint32_t)addr;
	if (is_internal(cpu, a))
	{
		flush(cpu);
		h8500_mem_write8(cpu, a, (uint8_t)data);
		recompute(cpu);
		return;
	}
	h8500_mem_write8(cpu, a, (uint8_t)data);
}

static void SLJIT_FUNC hwrite16(h8500_t *cpu, sljit_sw addr, sljit_sw data)
{
	uint32_t a = (uint32_t)addr;
	if (is_internal(cpu, a) || is_internal(cpu, a + 1))
	{
		flush(cpu);
		h8500_mem_write16(cpu, a, (uint16_t)data);
		recompute(cpu);
		return;
	}
	h8500_mem_write16(cpu, a, (uint16_t)data);
}

static void SLJIT_FUNC hfallback(h8500_t *cpu)
{
	fallback(cpu);
}

static void SLJIT_FUNC hdivzero(h8500_t *cpu, sljit_sw start_pc)
{
	flush(cpu);
	cpu->sr &= (uint16_t)~(FLAG_N | FLAG_V | FLAG_C);
	cpu->sr |= FLAG_Z;
	h8500_exception(cpu, 3, (uint16_t)start_pc, -1);
	recompute(cpu);
}

/* ------------------------------------------------------------------ */
/* decoding                                                           */
/* ------------------------------------------------------------------ */

enum { EM_REG = 0, EM_IND, EM_D8, EM_D16, EM_PREDEC, EM_POSTINC, EM_ABS8, EM_ABS16, EM_IMM8, EM_IMM16, EM_NONE };

enum
{
	K_FALLBACK = 0,
	K_NOP, K_SCB, K_LDM, K_STM, K_PJSR24, K_PJMP24, K_BSR, K_UNLK, K_JMP16, K_JSR16,
	K_PRTD, K_PRTS, K_PJMPR, K_PJSRR, K_JMPR, K_JSRR, K_JMPD, K_JSRD,
	K_RTD, K_LINK, K_RTS, K_SLEEP, K_BCC, K_CMPE, K_CMPI, K_MOVE, K_MOVI, K_MOVL, K_MOVS, K_MOVF, K_MOVFS,
	K_G_CMPIMM, K_G_MOVIMM, K_G_ADDQ, K_G_SWAP, K_G_EXTS, K_G_EXTU, K_G_CLR, K_G_NEG, K_G_NOT, K_G_TST, K_G_TAS,
	K_G_SHIFT, K_G_ADD, K_G_ADDS, K_G_SUB, K_G_SUBS, K_G_LOGIC, K_G_CMP, K_G_BITR, K_G_BTSTR, K_G_MOVTO,
	K_G_XCH, K_G_MOVFROM, K_G_ADDX, K_G_SUBX, K_G_MULXU, K_G_DIVXU, K_G_BITI,
	K_G_LDC, K_G_STC, K_G_CRIMM, K_TRAPA, K_RTE
};

typedef struct insn
{
	uint32_t addr;
	uint16_t pc;
	uint16_t next;
	uint8_t len;
	uint8_t kind;
	uint8_t b0, op, b1;
	uint8_t mode, sz, reg, step, ea;
	uint8_t ext[2];
	uint16_t imm;
	int16_t disp;
	int cyc;
	uint8_t fw, fr;
	bool term;
	bool byte_stack;
} insn_t;

typedef struct decoder
{
	const uint8_t *base;
	uint32_t region_base;
	uint32_t region_size;
	uint8_t cp;
	uint16_t pc;
	bool overrun;
} decoder_t;

static uint8_t dfetch8(decoder_t *d)
{
	uint32_t a = ((uint32_t)d->cp << 16) | d->pc;
	uint32_t off = a - d->region_base;
	d->pc++;
	if (off >= d->region_size)
	{
		d->overrun = true;
		return 0;
	}
	return d->base[off];
}

static uint16_t dfetch16(decoder_t *d)
{
	uint16_t hi = dfetch8(d);
	return (uint16_t)((hi << 8) | dfetch8(d));
}

static bool decode_ea(decoder_t *d, insn_t *i, uint8_t ea)
{
	i->ea = ea;
	i->sz = (uint8_t)((ea >> 3) & 1);
	i->reg = (uint8_t)(ea & 7);
	i->step = (uint8_t)(i->sz ? 2 : 1);
	i->byte_stack = (ea == 0xb7 || ea == 0xc7);
	if (i->byte_stack)
		i->step = 2;
	switch (ea)
	{
	case 0x04: i->mode = EM_IMM8; i->ext[0] = dfetch8(d); return true;
	case 0x0c: i->mode = EM_IMM16; i->ext[0] = dfetch8(d); i->ext[1] = dfetch8(d); return true;
	case 0x05: case 0x0d: i->mode = EM_ABS8; i->ext[0] = dfetch8(d); return true;
	case 0x15: case 0x1d: i->mode = EM_ABS16; i->ext[0] = dfetch8(d); i->ext[1] = dfetch8(d); return true;
	default: break;
	}
	switch (ea & 0xf0)
	{
	case 0xa0: i->mode = EM_REG; return true;
	case 0xb0: i->mode = EM_PREDEC; return true;
	case 0xc0: i->mode = EM_POSTINC; return true;
	case 0xd0: i->mode = EM_IND; return true;
	case 0xe0: i->mode = EM_D8; i->ext[0] = dfetch8(d); return true;
	case 0xf0: i->mode = EM_D16; i->ext[0] = dfetch8(d); i->ext[1] = dfetch8(d); return true;
	default: return false;
	}
}

static void set(insn_t *i, int kind, int cyc, int fw, int fr)
{
	i->kind = (uint8_t)kind;
	i->cyc = cyc;
	i->fw = (uint8_t)fw;
	i->fr = (uint8_t)fr;
}

static void decode_general(decoder_t *d, insn_t *i)
{
	uint8_t op = dfetch8(d);
	int mode = i->mode;
	int cyc = h8500_cyc_src[mode];
	int rmw = h8500_cyc_rmw[mode];
	i->op = op;

	switch (op)
	{
	case 0x04: case 0x06:
		i->imm = dfetch8(d);
		set(i, op == 0x04 ? K_G_CMPIMM : K_G_MOVIMM, rmw + 1, op == 0x04 ? FLAGS_ALL : (FLAG_N | FLAG_Z | FLAG_V), 0);
		return;
	case 0x05: case 0x07:
		i->imm = dfetch16(d);
		set(i, op == 0x05 ? K_G_CMPIMM : K_G_MOVIMM, rmw + 2, op == 0x05 ? FLAGS_ALL : (FLAG_N | FLAG_Z | FLAG_V), 0);
		return;
	case 0x08: case 0x09: case 0x0c: case 0x0d:
		set(i, K_G_ADDQ, rmw, FLAGS_ALL, 0);
		return;
	case 0x10:
		if (mode != EM_REG) return;
		set(i, K_G_SWAP, 4, FLAG_N | FLAG_Z | FLAG_V, 0);
		return;
	case 0x11:
		if (mode != EM_REG) return;
		set(i, K_G_EXTS, 3, FLAGS_ALL, 0);
		return;
	case 0x12:
		if (mode != EM_REG) return;
		set(i, K_G_EXTU, 3, FLAGS_ALL, 0);
		return;
	case 0x13: set(i, K_G_CLR, rmw, FLAGS_ALL, 0); return;
	case 0x14: set(i, K_G_NEG, rmw, FLAGS_ALL, 0); return;
	case 0x15: set(i, K_G_NOT, rmw, FLAG_N | FLAG_Z | FLAG_V, 0); return;
	case 0x16: set(i, K_G_TST, cyc, FLAGS_ALL, 0); return;
	case 0x17: set(i, K_G_TAS, rmw, FLAGS_ALL, 0); return;
	case 0x18: case 0x19: case 0x1a: case 0x1b:
	case 0x1c: case 0x1d: case 0x1e: case 0x1f:
		set(i, K_G_SHIFT, rmw, FLAGS_ALL, (op >= 0x1e) ? FLAG_C : 0);
		return;
	default:
		break;
	}

	switch (op & 0xf8)
	{
	case 0x20: set(i, K_G_ADD, cyc, FLAGS_ALL, 0); return;
	case 0x28: set(i, K_G_ADDS, cyc, 0, 0); return;
	case 0x30: set(i, K_G_SUB, cyc, FLAGS_ALL, 0); return;
	case 0x38: set(i, K_G_SUBS, cyc, 0, 0); return;
	case 0x40: case 0x50: case 0x60: set(i, K_G_LOGIC, cyc, FLAG_N | FLAG_Z | FLAG_V, 0); return;
	case 0x70: set(i, K_G_CMP, cyc, FLAGS_ALL, 0); return;
	case 0x48: case 0x58: case 0x68:
		if (mode == EM_IMM8 || mode == EM_IMM16)
			set(i, K_G_CRIMM, cyc, FLAGS_ALL, FLAGS_ALL);
		else
			set(i, K_G_BITR, rmw, FLAG_Z, 0);
		return;
	case 0x78: set(i, K_G_BTSTR, cyc, FLAG_Z, 0); return;
	case 0x80: set(i, K_G_MOVFROM, cyc, FLAG_N | FLAG_Z | FLAG_V, 0); return;
	case 0x88: set(i, K_G_LDC, cyc, FLAGS_ALL, FLAGS_ALL); return;
	case 0x98: set(i, K_G_STC, rmw, 0, FLAGS_ALL); return;
	case 0x90:
		if (mode == EM_REG)
			set(i, K_G_XCH, 4, 0, 0);
		else
			set(i, K_G_MOVTO, rmw, FLAG_N | FLAG_Z | FLAG_V, 0);
		return;
	case 0xa0: set(i, K_G_ADDX, cyc, FLAGS_ALL, FLAG_Z | FLAG_C); return;
	case 0xb0: set(i, K_G_SUBX, cyc, FLAGS_ALL, FLAG_Z | FLAG_C); return;
	case 0xa8: set(i, K_G_MULXU, i->sz ? 24 : 14, FLAGS_ALL, 0); return;
	case 0xb8: set(i, K_G_DIVXU, i->sz ? 26 : 20, FLAGS_ALL, FLAGS_ALL); return;
	default: break;
	}

	switch (op & 0xf0)
	{
	case 0xc0: case 0xd0: case 0xe0: set(i, K_G_BITI, rmw, FLAG_Z, 0); return;
	case 0xf0: set(i, K_G_BITI, cyc, FLAG_Z, 0); return;
	default: break;
	}
}

static const uint8_t cond_reads[16] =
{
	0, 0, FLAG_C | FLAG_Z, FLAG_C | FLAG_Z, FLAG_C, FLAG_C, FLAG_Z, FLAG_Z,
	FLAG_V, FLAG_V, FLAG_N, FLAG_N, FLAG_N | FLAG_V, FLAG_N | FLAG_V,
	FLAG_N | FLAG_V | FLAG_Z, FLAG_N | FLAG_V | FLAG_Z
};

static void decode(decoder_t *d, insn_t *i)
{
	uint8_t b0;
	memset(i, 0, sizeof(*i));
	i->pc = d->pc;
	i->addr = ((uint32_t)d->cp << 16) | d->pc;
	i->mode = EM_NONE;
	b0 = dfetch8(d);
	i->b0 = b0;
	i->kind = K_FALLBACK;
	i->fw = FLAGS_ALL;
	i->fr = FLAGS_ALL;

	switch (b0)
	{
	case 0x00: set(i, K_NOP, 2, 0, 0); break;
	case 0x01: case 0x06: case 0x07:
		i->b1 = dfetch8(d);
		if ((i->b1 & 0xf8) != 0xb8)
			break;
		i->reg = (uint8_t)(i->b1 & 7);
		i->disp = (int8_t)dfetch8(d);
		set(i, K_SCB, 8, 0, FLAGS_ALL);
		break;
	case 0x02: i->b1 = dfetch8(d); set(i, K_LDM, 6, 0, 0); break;
	case 0x12: i->b1 = dfetch8(d); set(i, K_STM, 6, 0, 0); break;
	case 0x03: i->b1 = dfetch8(d); i->imm = dfetch16(d); set(i, K_PJSR24, 15, 0, 0); break;
	case 0x08:
		i->b1 = dfetch8(d);
		if ((i->b1 & 0xf0) == 0x10)
			set(i, K_TRAPA, 22, 0, FLAGS_ALL);
		break;
	case 0x0a: set(i, K_RTE, 15, FLAGS_ALL, 0); break;
	case 0x13: i->b1 = dfetch8(d); i->imm = dfetch16(d); set(i, K_PJMP24, 9, 0, 0); break;
	case 0x0e: i->disp = (int8_t)dfetch8(d); set(i, K_BSR, 9, 0, 0); break;
	case 0x1e: i->disp = (int16_t)dfetch16(d); set(i, K_BSR, 9, 0, 0); break;
	case 0x0f: set(i, K_UNLK, 5, 0, 0); break;
	case 0x10: i->imm = dfetch16(d); set(i, K_JMP16, 7, 0, 0); break;
	case 0x18: i->imm = dfetch16(d); set(i, K_JSR16, 9, 0, 0); break;
	case 0x11:
	{
		uint8_t b1 = dfetch8(d);
		i->b1 = b1;
		i->reg = (uint8_t)(b1 & 7);
		switch (b1)
		{
		case 0x14: i->disp = (int8_t)dfetch8(d); set(i, K_PRTD, 13, 0, 0); break;
		case 0x1c: i->disp = (int16_t)dfetch16(d); set(i, K_PRTD, 13, 0, 0); break;
		case 0x19: set(i, K_PRTS, 12, 0, 0); break;
		default:
			switch (b1 & 0xf8)
			{
			case 0xc0: set(i, K_PJMPR, 8, 0, 0); break;
			case 0xc8: set(i, K_PJSRR, 13, 0, 0); break;
			case 0xd0: set(i, K_JMPR, 6, 0, 0); break;
			case 0xd8: set(i, K_JSRR, 9, 0, 0); break;
			case 0xe0: i->disp = (int8_t)dfetch8(d); set(i, K_JMPD, 7, 0, 0); break;
			case 0xe8: i->disp = (int8_t)dfetch8(d); set(i, K_JSRD, 9, 0, 0); break;
			case 0xf0: i->disp = (int16_t)dfetch16(d); set(i, K_JMPD, 8, 0, 0); break;
			case 0xf8: i->disp = (int16_t)dfetch16(d); set(i, K_JSRD, 10, 0, 0); break;
			default: break;
			}
			break;
		}
		break;
	}
	case 0x14: i->disp = (int8_t)dfetch8(d); set(i, K_RTD, 9, 0, 0); break;
	case 0x1c: i->disp = (int16_t)dfetch16(d); set(i, K_RTD, 9, 0, 0); break;
	case 0x17: i->disp = (int8_t)dfetch8(d); set(i, K_LINK, 6, 0, 0); break;
	case 0x1f: i->disp = (int16_t)dfetch16(d); set(i, K_LINK, 7, 0, 0); break;
	case 0x19: set(i, K_RTS, 8, 0, 0); break;
	case 0x1a: set(i, K_SLEEP, 2, 0, 0); break;
	default:
		if (b0 >= 0x20 && b0 <= 0x2f)
		{
			i->disp = (int8_t)dfetch8(d);
			set(i, K_BCC, 7, 0, FLAGS_ALL);
		}
		else if (b0 >= 0x30 && b0 <= 0x3f)
		{
			i->disp = (int16_t)dfetch16(d);
			set(i, K_BCC, 7, 0, FLAGS_ALL);
		}
		else if (b0 >= 0x40 && b0 <= 0x5f)
		{
			i->reg = (uint8_t)(b0 & 7);
			switch (b0 & 0xf8)
			{
			case 0x40: i->imm = dfetch8(d); set(i, K_CMPE, 2, FLAGS_ALL, 0); break;
			case 0x48: i->imm = dfetch16(d); set(i, K_CMPI, 3, FLAGS_ALL, 0); break;
			case 0x50: i->imm = dfetch8(d); set(i, K_MOVE, 2, FLAG_N | FLAG_Z | FLAG_V, 0); break;
			default:   i->imm = dfetch16(d); set(i, K_MOVI, 3, FLAG_N | FLAG_Z | FLAG_V, 0); break;
			}
		}
		else if (b0 >= 0x60 && b0 <= 0x7f)
		{
			i->reg = (uint8_t)(b0 & 7);
			i->sz = (uint8_t)((b0 >> 3) & 1);
			i->ext[0] = dfetch8(d);
			set(i, b0 < 0x70 ? K_MOVL : K_MOVS, 5, FLAG_N | FLAG_Z | FLAG_V, 0);
		}
		else if (b0 >= 0x80 && b0 <= 0x9f)
		{
			i->reg = (uint8_t)(b0 & 7);
			i->sz = (uint8_t)((b0 >> 3) & 1);
			i->disp = (int8_t)dfetch8(d);
			set(i, b0 < 0x90 ? K_MOVF : K_MOVFS, 5, FLAG_N | FLAG_Z | FLAG_V, 0);
		}
		else if (b0 == 0x04 || b0 == 0x05 || b0 == 0x0c || b0 == 0x0d || b0 == 0x15 || b0 == 0x1d || b0 >= 0xa0)
		{
			if (decode_ea(d, i, b0))
				decode_general(d, i);
		}
		break;
	}

	if (i->kind == K_BCC)
		i->fr = cond_reads[b0 & 15];

	switch (i->kind)
	{
	case K_FALLBACK: case K_PJSR24: case K_PJMP24: case K_BSR: case K_JMP16: case K_JSR16:
	case K_PRTD: case K_PRTS: case K_PJMPR: case K_PJSRR: case K_JMPR: case K_JSRR: case K_JMPD: case K_JSRD:
	case K_RTD: case K_RTS: case K_SLEEP: case K_TRAPA: case K_RTE: case K_G_LDC: case K_G_CRIMM:
	case K_BCC: case K_SCB:
		i->term = true;
		break;
	default:
		break;
	}
	if (i->kind == K_FALLBACK)
	{
		i->fw = FLAGS_ALL;
		i->fr = FLAGS_ALL;
	}
	i->next = d->pc;
	i->len = (uint8_t)(d->pc - i->pc);
}

/* ------------------------------------------------------------------ */
/* emission                                                           */
/* ------------------------------------------------------------------ */

#define CPU SLJIT_S0
#define PENDING SLJIT_S1
#define LIMIT SLJIT_S2
#define EA SLJIT_S3
#define VAL SLJIT_S4
#define VAL2 SLJIT_S5

#define CELL(field) SLJIT_MEM1(CPU), (sljit_sw)offsetof(h8500_t, field)
#define REG16(n) SLJIT_MEM1(CPU), (sljit_sw)(offsetof(h8500_t, r) + 2 * (n))
#if (defined SLJIT_LITTLE_ENDIAN && SLJIT_LITTLE_ENDIAN)
#define REG8_OFF(n) ((sljit_sw)(offsetof(h8500_t, r) + 2 * (n)))
#else
#define REG8_OFF(n) ((sljit_sw)(offsetof(h8500_t, r) + 2 * (n) + 1))
#endif
#define REG8(n) SLJIT_MEM1(CPU), REG8_OFF(n)
#define REG_OFF(n, sz) ((sz) ? (sljit_sw)(offsetof(h8500_t, r) + 2 * (n)) : REG8_OFF(n))

typedef struct exit_site
{
	struct sljit_jump *jump;
	uint16_t pc;
} exit_site_t;

typedef struct emitter
{
	struct sljit_compiler *c;
	struct h8500_jit *j;
	uint8_t cp;
	exit_site_t exits[BLOCK_MAX_INSNS * 4 + 8];
	int exit_count;
	bool ea_committed;
	bool failed;
} emitter_t;

static void op1(emitter_t *e, sljit_s32 op, sljit_s32 d, sljit_sw dw, sljit_s32 s, sljit_sw sw)
{
	sljit_emit_op1(e->c, op, d, dw, s, sw);
}

static void op2(emitter_t *e, sljit_s32 op, sljit_s32 d, sljit_sw dw, sljit_s32 a, sljit_sw aw, sljit_s32 b, sljit_sw bw)
{
	sljit_emit_op2(e->c, op, d, dw, a, aw, b, bw);
}

static void movi(emitter_t *e, sljit_s32 r, sljit_sw v)
{
	sljit_emit_op1(e->c, SLJIT_MOV, r, 0, SLJIT_IMM, v);
}

static void movr(emitter_t *e, sljit_s32 d, sljit_s32 s)
{
	sljit_emit_op1(e->c, SLJIT_MOV, d, 0, s, 0);
}

static struct sljit_label *label(emitter_t *e)
{
	return sljit_emit_label(e->c);
}

static void bind(emitter_t *e, struct sljit_jump *j)
{
	sljit_set_label(j, sljit_emit_label(e->c));
}

/* the block leaves for the dispatcher with pc = target */
static void exit_to(emitter_t *e, struct sljit_jump *j, uint16_t pc)
{
	if (e->exit_count >= (int)(sizeof(e->exits) / sizeof(e->exits[0])))
	{
		e->failed = true;
		return;
	}
	e->exits[e->exit_count].jump = j;
	e->exits[e->exit_count].pc = pc;
	e->exit_count++;
}

static void emit_return(emitter_t *e)
{
	sljit_emit_return(e->c, SLJIT_MOV, PENDING, 0);
}

static void emit_exit_now(emitter_t *e, uint16_t pc)
{
	op1(e, SLJIT_MOV_U16, CELL(pc), SLJIT_IMM, pc);
	emit_return(e);
}

/* the boundary between two instructions: account the cycles, leave if the
 * budget or the peripheral horizon is reached */
static void boundary(emitter_t *e, int cyc, uint16_t next)
{
	op2(e, SLJIT_ADD, PENDING, 0, PENDING, 0, SLJIT_IMM, cyc);
	exit_to(e, sljit_emit_cmp(e->c, SLJIT_SIG_GREATER_EQUAL, PENDING, 0, LIMIT, 0), next);
}

static slot_t *new_slot(struct h8500_jit *j, uint32_t key)
{
	slot_chunk_t *ch = j->chunks;
	slot_t *s;
	if (!ch || ch->used == SLOT_CHUNK)
	{
		ch = calloc(1, sizeof(*ch));
		if (!ch)
			return NULL;
		ch->next = j->chunks;
		j->chunks = ch;
	}
	s = &ch->slots[ch->used++];
	s->key = key;
	s->target = j->exit_body;
	{
		block_t *b = lookup(j, key);
		if (b->key == key && b->body)
			s->target = b->body;
	}
	return s;
}

/* control passes to another block: through its slot if it is translated,
 * to the dispatcher otherwise */
static void chain(emitter_t *e, uint8_t cp, uint16_t pc)
{
	slot_t *s = new_slot(e->j, ((uint32_t)cp << 16) | pc);
	if (!s)
	{
		e->failed = true;
		return;
	}
	op1(e, SLJIT_MOV_U16, CELL(pc), SLJIT_IMM, pc);
	movi(e, SLJIT_R0, (sljit_sw)&s->target);
	sljit_emit_ijump(e->c, SLJIT_JUMP, SLJIT_MEM1(SLJIT_R0), 0);
}

static void call_pre(emitter_t *e)
{
	op1(e, SLJIT_MOV_U32, CELL(jit_pending), PENDING, 0);
	movr(e, SLJIT_R0, CPU);
}

static void call_post(emitter_t *e)
{
	op1(e, SLJIT_MOV_U32, PENDING, 0, CELL(jit_pending));
	op1(e, SLJIT_MOV_U32, LIMIT, 0, CELL(jit_limit));
}

static void call1(emitter_t *e, void *fn)
{
	call_pre(e);
	sljit_emit_icall(e->c, SLJIT_CALL, SLJIT_ARGS1V(P), SLJIT_IMM, (sljit_sw)(uintptr_t)fn);
	call_post(e);
}

/* ---- memory ---- */

/* value at the 24-bit address in EA into dst (zero-extended) */
static void mem_read(emitter_t *e, int sz, sljit_s32 dst)
{
	struct sljit_jump *slow1, *slow2, *slow3 = NULL, *done;
	uint8_t **table = sz ? e->j->rd16 : e->j->rd8;

	op2(e, SLJIT_SUB, SLJIT_R1, 0, EA, 0, SLJIT_IMM, sz ? e->j->io_base - 1 : e->j->io_base);
	slow1 = sljit_emit_cmp(e->c, SLJIT_LESS, SLJIT_R1, 0, SLJIT_IMM, sz ? e->j->io_size + 1 : e->j->io_size);
	if (sz)
	{
		op2(e, SLJIT_AND, SLJIT_R1, 0, EA, 0, SLJIT_IMM, 1);
		slow3 = sljit_emit_cmp(e->c, SLJIT_NOT_EQUAL, SLJIT_R1, 0, SLJIT_IMM, 0);
	}
	op2(e, SLJIT_LSHR, SLJIT_R1, 0, EA, 0, SLJIT_IMM, PAGE_SHIFT);
	movi(e, SLJIT_R2, (sljit_sw)table);
	op1(e, SLJIT_MOV_P, SLJIT_R1, 0, SLJIT_MEM2(SLJIT_R2, SLJIT_R1), 3);
	slow2 = sljit_emit_cmp(e->c, SLJIT_EQUAL, SLJIT_R1, 0, SLJIT_IMM, 0);
	if (sz)
	{
		op1(e, SLJIT_MOV_U16, dst, 0, SLJIT_MEM2(SLJIT_R1, EA), 0);
		op1(e, SLJIT_REV_U16, dst, 0, dst, 0);
	}
	else
		op1(e, SLJIT_MOV_U8, dst, 0, SLJIT_MEM2(SLJIT_R1, EA), 0);
	done = sljit_emit_jump(e->c, SLJIT_JUMP);

	bind(e, slow1);
	bind(e, slow2);
	if (slow3)
		bind(e, slow3);
	call_pre(e);
	movr(e, SLJIT_R1, EA);
	sljit_emit_icall(e->c, SLJIT_CALL, SLJIT_ARGS2(W, P, W), SLJIT_IMM, (sljit_sw)(uintptr_t)(sz ? (void *)hread16 : (void *)hread8));
	call_post(e);
	if (dst != SLJIT_R0)
		movr(e, dst, SLJIT_R0);
	bind(e, done);
}

/* src (zero-extended) to the 24-bit address in EA */
static void mem_write(emitter_t *e, int sz, sljit_s32 src)
{
	struct sljit_jump *slow1, *slow2, *slow3 = NULL, *done;
	uint8_t **table = sz ? e->j->wr16 : e->j->wr8;

	op2(e, SLJIT_SUB, SLJIT_R1, 0, EA, 0, SLJIT_IMM, sz ? e->j->io_base - 1 : e->j->io_base);
	slow1 = sljit_emit_cmp(e->c, SLJIT_LESS, SLJIT_R1, 0, SLJIT_IMM, sz ? e->j->io_size + 1 : e->j->io_size);
	if (sz)
	{
		op2(e, SLJIT_AND, SLJIT_R1, 0, EA, 0, SLJIT_IMM, 1);
		slow3 = sljit_emit_cmp(e->c, SLJIT_NOT_EQUAL, SLJIT_R1, 0, SLJIT_IMM, 0);
	}
	op2(e, SLJIT_LSHR, SLJIT_R1, 0, EA, 0, SLJIT_IMM, PAGE_SHIFT);
	movi(e, SLJIT_R2, (sljit_sw)table);
	op1(e, SLJIT_MOV_P, SLJIT_R1, 0, SLJIT_MEM2(SLJIT_R2, SLJIT_R1), 3);
	slow2 = sljit_emit_cmp(e->c, SLJIT_EQUAL, SLJIT_R1, 0, SLJIT_IMM, 0);
	if (sz)
	{
		op1(e, SLJIT_REV_U16, SLJIT_R2, 0, src, 0);
		op1(e, SLJIT_MOV_U16, SLJIT_MEM2(SLJIT_R1, EA), 0, SLJIT_R2, 0);
	}
	else
		op1(e, SLJIT_MOV_U8, SLJIT_MEM2(SLJIT_R1, EA), 0, src, 0);
	done = sljit_emit_jump(e->c, SLJIT_JUMP);

	bind(e, slow1);
	bind(e, slow2);
	if (slow3)
		bind(e, slow3);
	if (src != SLJIT_R2)
		movr(e, SLJIT_R2, src);
	call_pre(e);
	movr(e, SLJIT_R1, EA);
	sljit_emit_icall(e->c, SLJIT_CALL, SLJIT_ARGS3V(P, W, W), SLJIT_IMM, (sljit_sw)(uintptr_t)(sz ? (void *)hwrite16 : (void *)hwrite8));
	call_post(e);
	bind(e, done);
}

static sljit_sw page_offset(int reg)
{
	if (reg < 4)
		return (sljit_sw)offsetof(h8500_t, dp);
	if (reg < 6)
		return (sljit_sw)offsetof(h8500_t, ep);
	return (sljit_sw)offsetof(h8500_t, tp);
}

/* EA = page:offset of the operand; register and immediate modes address 0 */
static void ea_address(emitter_t *e, const insn_t *i)
{
	e->ea_committed = false;
	switch (i->mode)
	{
	case EM_ABS8:
		op1(e, SLJIT_MOV_U8, EA, 0, CELL(br));
		op2(e, SLJIT_SHL, EA, 0, EA, 0, SLJIT_IMM, 8);
		op2(e, SLJIT_OR, EA, 0, EA, 0, SLJIT_IMM, i->ext[0]);
		return;
	case EM_ABS16:
		op1(e, SLJIT_MOV_U8, EA, 0, CELL(dp));
		op2(e, SLJIT_SHL, EA, 0, EA, 0, SLJIT_IMM, 16);
		op2(e, SLJIT_OR, EA, 0, EA, 0, SLJIT_IMM, (i->ext[0] << 8) | i->ext[1]);
		return;
	case EM_IND: case EM_POSTINC: case EM_PREDEC: case EM_D8: case EM_D16:
	{
		sljit_sw adj = 0;
		if (i->mode == EM_PREDEC)
			adj = -(sljit_sw)i->step;
		else if (i->mode == EM_D8)
			adj = (int8_t)i->ext[0];
		else if (i->mode == EM_D16)
			adj = (int16_t)((i->ext[0] << 8) | i->ext[1]);
		op1(e, SLJIT_MOV_U16, SLJIT_R0, 0, REG16(i->reg));
		if (adj)
		{
			op2(e, SLJIT_ADD, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, adj);
			op2(e, SLJIT_AND, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, 0xffff);
		}
		op1(e, SLJIT_MOV_U8, EA, 0, SLJIT_MEM1(CPU), page_offset(i->reg));
		op2(e, SLJIT_SHL, EA, 0, EA, 0, SLJIT_IMM, 16);
		op2(e, SLJIT_OR, EA, 0, EA, 0, SLJIT_R0, 0);
		return;
	}
	default:
		movi(e, EA, 0);
		return;
	}
}

static void ea_commit(emitter_t *e, const insn_t *i)
{
	if (e->ea_committed)
		return;
	e->ea_committed = true;
	if (i->mode == EM_PREDEC || i->mode == EM_POSTINC)
	{
		op1(e, SLJIT_MOV_U16, SLJIT_R0, 0, REG16(i->reg));
		op2(e, SLJIT_ADD, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, i->mode == EM_PREDEC ? -(sljit_sw)i->step : (sljit_sw)i->step);
		op1(e, SLJIT_MOV_U16, REG16(i->reg), SLJIT_R0, 0);
	}
}

/* the operand, read the interpreter's way, zero-extended into dst (a saved register) */
static void ea_read(emitter_t *e, const insn_t *i, sljit_s32 dst)
{
	if (i->mode == EM_IMM8)
	{
		movi(e, dst, i->sz ? (sljit_sw)(uint16_t)(int16_t)(int8_t)i->ext[0] : (sljit_sw)i->ext[0]);
		return;
	}
	if (i->mode == EM_IMM16)
	{
		movi(e, dst, i->sz ? (sljit_sw)((i->ext[0] << 8) | i->ext[1]) : (sljit_sw)i->ext[1]);
		return;
	}
	if (i->mode == EM_REG)
	{
		op1(e, i->sz ? SLJIT_MOV_U16 : SLJIT_MOV_U8, dst, 0, SLJIT_MEM1(CPU), REG_OFF(i->reg, i->sz));
		return;
	}
	if (i->byte_stack && !i->sz)
	{
		mem_read(e, 1, dst);
		op2(e, SLJIT_AND, dst, 0, dst, 0, SLJIT_IMM, 0xff);
	}
	else
		mem_read(e, i->sz, dst);
	ea_commit(e, i);
}

/* src (a register or immediate holding a zero-extended value) to the operand */
static void ea_write(emitter_t *e, const insn_t *i, sljit_s32 src, sljit_sw srcw)
{
	if (i->mode == EM_REG)
	{
		op1(e, i->sz ? SLJIT_MOV_U16 : SLJIT_MOV_U8, SLJIT_MEM1(CPU), REG_OFF(i->reg, i->sz), src, srcw);
		return;
	}
	if (src == SLJIT_IMM)
	{
		movi(e, SLJIT_R3, srcw);
		src = SLJIT_R3;
	}
	if (i->byte_stack && !i->sz)
	{
		op2(e, SLJIT_AND, SLJIT_R3, 0, src, 0, SLJIT_IMM, 0xff);
		mem_write(e, 1, SLJIT_R3);
	}
	else
		mem_write(e, i->sz, src);
	ea_commit(e, i);
}

/* ---- flags ---- */

/* N and Z from the zero-extended value in v of the given width; V cleared,
 * and C too when clear_c; the other bits of SR kept.  Clobbers R3-R5. */
static void flags_nz(emitter_t *e, sljit_s32 v, int width, int clear_c)
{
	op2(e, SLJIT_LSHR, SLJIT_R3, 0, v, 0, SLJIT_IMM, width - 4);
	op2(e, SLJIT_AND, SLJIT_R3, 0, SLJIT_R3, 0, SLJIT_IMM, FLAG_N);
	op2(e, SLJIT_SUB, SLJIT_R4, 0, v, 0, SLJIT_IMM, 1);
	op2(e, SLJIT_LSHR, SLJIT_R4, 0, SLJIT_R4, 0, SLJIT_IMM, 61);
	op2(e, SLJIT_AND, SLJIT_R4, 0, SLJIT_R4, 0, SLJIT_IMM, FLAG_Z);
	op2(e, SLJIT_OR, SLJIT_R3, 0, SLJIT_R3, 0, SLJIT_R4, 0);
	op1(e, SLJIT_MOV_U16, SLJIT_R5, 0, CELL(sr));
	op2(e, SLJIT_AND, SLJIT_R5, 0, SLJIT_R5, 0, SLJIT_IMM, clear_c ? 0xfff0 : 0xfff1);
	op2(e, SLJIT_OR, SLJIT_R5, 0, SLJIT_R5, 0, SLJIT_R3, 0);
	op1(e, SLJIT_MOV_U16, CELL(sr), SLJIT_R5, 0);
}

/* all four flags of d ± s (R0 = d, VAL = s, R1 = the untruncated result),
 * as the interpreter's do_add/do_sub.  Clobbers R2-R5. */
static void flags_arith(emitter_t *e, int width, int sub, int keep_z)
{
	op2(e, SLJIT_LSHR, SLJIT_R2, 0, SLJIT_R1, 0, SLJIT_IMM, width);
	op2(e, SLJIT_AND, SLJIT_R2, 0, SLJIT_R2, 0, SLJIT_IMM, FLAG_C);
	op2(e, SLJIT_LSHR, SLJIT_R3, 0, SLJIT_R1, 0, SLJIT_IMM, width - 4);
	op2(e, SLJIT_AND, SLJIT_R3, 0, SLJIT_R3, 0, SLJIT_IMM, FLAG_N);
	op2(e, SLJIT_OR, SLJIT_R2, 0, SLJIT_R2, 0, SLJIT_R3, 0);
	op2(e, SLJIT_AND, SLJIT_R3, 0, SLJIT_R1, 0, SLJIT_IMM, width == 16 ? 0xffff : 0xff);
	op2(e, SLJIT_SUB, SLJIT_R3, 0, SLJIT_R3, 0, SLJIT_IMM, 1);
	op2(e, SLJIT_LSHR, SLJIT_R3, 0, SLJIT_R3, 0, SLJIT_IMM, 61);
	op2(e, SLJIT_AND, SLJIT_R3, 0, SLJIT_R3, 0, SLJIT_IMM, FLAG_Z);
	if (keep_z)
	{
		op1(e, SLJIT_MOV_U16, SLJIT_R4, 0, CELL(sr));
		op2(e, SLJIT_AND, SLJIT_R3, 0, SLJIT_R3, 0, SLJIT_R4, 0);
	}
	op2(e, SLJIT_OR, SLJIT_R2, 0, SLJIT_R2, 0, SLJIT_R3, 0);
	op2(e, SLJIT_XOR, SLJIT_R3, 0, SLJIT_R0, 0, SLJIT_R1, 0);
	op2(e, SLJIT_XOR, SLJIT_R4, 0, SLJIT_R0, 0, VAL, 0);
	if (!sub)
		op2(e, SLJIT_XOR, SLJIT_R4, 0, SLJIT_R4, 0, SLJIT_IMM, -1);
	op2(e, SLJIT_AND, SLJIT_R3, 0, SLJIT_R3, 0, SLJIT_R4, 0);
	op2(e, SLJIT_LSHR, SLJIT_R3, 0, SLJIT_R3, 0, SLJIT_IMM, width - 2);
	op2(e, SLJIT_AND, SLJIT_R3, 0, SLJIT_R3, 0, SLJIT_IMM, FLAG_V);
	op2(e, SLJIT_OR, SLJIT_R2, 0, SLJIT_R2, 0, SLJIT_R3, 0);
	op1(e, SLJIT_MOV_U16, SLJIT_R3, 0, CELL(sr));
	op2(e, SLJIT_AND, SLJIT_R3, 0, SLJIT_R3, 0, SLJIT_IMM, 0xfff0);
	op2(e, SLJIT_OR, SLJIT_R3, 0, SLJIT_R3, 0, SLJIT_R2, 0);
	op1(e, SLJIT_MOV_U16, CELL(sr), SLJIT_R3, 0);
}

static void sr_clear(emitter_t *e, int mask)
{
	op1(e, SLJIT_MOV_U16, SLJIT_R3, 0, CELL(sr));
	op2(e, SLJIT_AND, SLJIT_R3, 0, SLJIT_R3, 0, SLJIT_IMM, 0xffff & ~mask);
	op1(e, SLJIT_MOV_U16, CELL(sr), SLJIT_R3, 0);
}

static void sr_set_clear(emitter_t *e, int set_mask, int clear_mask)
{
	op1(e, SLJIT_MOV_U16, SLJIT_R3, 0, CELL(sr));
	op2(e, SLJIT_AND, SLJIT_R3, 0, SLJIT_R3, 0, SLJIT_IMM, 0xffff & ~clear_mask);
	op2(e, SLJIT_OR, SLJIT_R3, 0, SLJIT_R3, 0, SLJIT_IMM, set_mask);
	op1(e, SLJIT_MOV_U16, CELL(sr), SLJIT_R3, 0);
}

/* R0 = 1 when the condition holds, 0 otherwise.  Clobbers R1. */
static void condition(emitter_t *e, int cc)
{
	op1(e, SLJIT_MOV_U16, SLJIT_R0, 0, CELL(sr));
	switch (cc)
	{
	case 0x0: movi(e, SLJIT_R0, 1); return;
	case 0x1: movi(e, SLJIT_R0, 0); return;
	case 0x2: case 0x3:
		op2(e, SLJIT_AND, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, FLAG_C | FLAG_Z);
		break;
	case 0x4: case 0x5:
		op2(e, SLJIT_AND, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, FLAG_C);
		break;
	case 0x6: case 0x7:
		op2(e, SLJIT_AND, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, FLAG_Z);
		break;
	case 0x8: case 0x9:
		op2(e, SLJIT_AND, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, FLAG_V);
		break;
	case 0xa: case 0xb:
		op2(e, SLJIT_AND, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, FLAG_N);
		break;
	case 0xc: case 0xd:
		op2(e, SLJIT_LSHR, SLJIT_R1, 0, SLJIT_R0, 0, SLJIT_IMM, 2);
		op2(e, SLJIT_XOR, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0);
		op2(e, SLJIT_AND, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, FLAG_V);
		break;
	default:
		op2(e, SLJIT_LSHR, SLJIT_R1, 0, SLJIT_R0, 0, SLJIT_IMM, 2);
		op2(e, SLJIT_XOR, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_R0, 0);
		op2(e, SLJIT_AND, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_IMM, FLAG_V);
		op2(e, SLJIT_AND, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, FLAG_Z);
		op2(e, SLJIT_OR, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0);
		break;
	}
	/* the odd conditions hold when the bits are set, the even ones when clear */
	op2(e, SLJIT_SUB, SLJIT_R0, 0, SLJIT_IMM, 0, SLJIT_R0, 0);
	op2(e, SLJIT_LSHR, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, 63);
	if (!(cc & 1))
		op2(e, SLJIT_XOR, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, 1);
}

/* ---- stack ---- */

static void push16(emitter_t *e, sljit_s32 src)
{
	op1(e, SLJIT_MOV_U16, SLJIT_R0, 0, REG16(7));
	op2(e, SLJIT_SUB, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, 2);
	op2(e, SLJIT_AND, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, 0xffff);
	op1(e, SLJIT_MOV_U16, REG16(7), SLJIT_R0, 0);
	op1(e, SLJIT_MOV_U8, EA, 0, CELL(tp));
	op2(e, SLJIT_SHL, EA, 0, EA, 0, SLJIT_IMM, 16);
	op2(e, SLJIT_OR, EA, 0, EA, 0, SLJIT_R0, 0);
	mem_write(e, 1, src);
}

static void pop16(emitter_t *e, sljit_s32 dst)
{
	op1(e, SLJIT_MOV_U8, EA, 0, CELL(tp));
	op2(e, SLJIT_SHL, EA, 0, EA, 0, SLJIT_IMM, 16);
	op1(e, SLJIT_MOV_U16, SLJIT_R0, 0, REG16(7));
	op2(e, SLJIT_OR, EA, 0, EA, 0, SLJIT_R0, 0);
	mem_read(e, 1, dst);
	op1(e, SLJIT_MOV_U16, SLJIT_R0, 0, REG16(7));
	op2(e, SLJIT_ADD, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, 2);
	op1(e, SLJIT_MOV_U16, REG16(7), SLJIT_R0, 0);
}

/* ---- the instructions ---- */

static void computed_exit(emitter_t *e, sljit_s32 pc_src, sljit_sw pc_srcw, int cyc)
{
	op1(e, SLJIT_MOV_U16, CELL(pc), pc_src, pc_srcw);
	op2(e, SLJIT_ADD, PENDING, 0, PENDING, 0, SLJIT_IMM, cyc);
	emit_return(e);
}

static void direct_exit(emitter_t *e, uint8_t cp, uint16_t pc, int cyc)
{
	op2(e, SLJIT_ADD, PENDING, 0, PENDING, 0, SLJIT_IMM, cyc);
	exit_to(e, sljit_emit_cmp(e->c, SLJIT_SIG_GREATER_EQUAL, PENDING, 0, LIMIT, 0), pc);
	chain(e, cp, pc);
}

static void emit_fallback(emitter_t *e, const insn_t *i)
{
	op1(e, SLJIT_MOV_U16, CELL(pc), SLJIT_IMM, i->pc);
	call1(e, (void *)hfallback);
	emit_return(e);
}

static void load_reg(emitter_t *e, sljit_s32 dst, int reg, int sz)
{
	op1(e, sz ? SLJIT_MOV_U16 : SLJIT_MOV_U8, dst, 0, SLJIT_MEM1(CPU), REG_OFF(reg, sz));
}

static void store_reg(emitter_t *e, int reg, int sz, sljit_s32 src, sljit_sw srcw)
{
	op1(e, sz ? SLJIT_MOV_U16 : SLJIT_MOV_U8, SLJIT_MEM1(CPU), REG_OFF(reg, sz), src, srcw);
}

static void emit_scb(emitter_t *e, const insn_t *i)
{
	struct sljit_jump *skip = NULL, *ended;
	uint16_t target = (uint16_t)(i->next + i->disp);

	if (i->b0 != 0x01)
	{
		op1(e, SLJIT_MOV_U16, SLJIT_R0, 0, CELL(sr));
		op2(e, SLJIT_AND, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, FLAG_Z);
		skip = sljit_emit_cmp(e->c, i->b0 == 0x06 ? SLJIT_EQUAL : SLJIT_NOT_EQUAL, SLJIT_R0, 0, SLJIT_IMM, 0);
	}
	op1(e, SLJIT_MOV_U16, SLJIT_R0, 0, REG16(i->reg));
	op2(e, SLJIT_SUB, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, 1);
	op2(e, SLJIT_AND, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, 0xffff);
	op1(e, SLJIT_MOV_U16, REG16(i->reg), SLJIT_R0, 0);
	ended = sljit_emit_cmp(e->c, SLJIT_EQUAL, SLJIT_R0, 0, SLJIT_IMM, 0xffff);
	direct_exit(e, e->cp, target, 8);

	bind(e, ended);
	direct_exit(e, e->cp, i->next, 4);
	if (skip)
	{
		bind(e, skip);
		direct_exit(e, e->cp, i->next, 3);
	}
}

static void emit_ldm(emitter_t *e, const insn_t *i)
{
	int n, count = 0;
	op1(e, SLJIT_MOV_U16, VAL, 0, REG16(7));
	for (n = 0; n < 8; n++)
	{
		if (!(i->b1 & (1u << n)))
			continue;
		count++;
		op1(e, SLJIT_MOV_U8, EA, 0, CELL(tp));
		op2(e, SLJIT_SHL, EA, 0, EA, 0, SLJIT_IMM, 16);
		op2(e, SLJIT_OR, EA, 0, EA, 0, VAL, 0);
		mem_read(e, 1, VAL2);
		if (n != 7)
			op1(e, SLJIT_MOV_U16, REG16(n), VAL2, 0);
		op2(e, SLJIT_ADD, VAL, 0, VAL, 0, SLJIT_IMM, 2);
		op2(e, SLJIT_AND, VAL, 0, VAL, 0, SLJIT_IMM, 0xffff);
	}
	op1(e, SLJIT_MOV_U16, REG16(7), VAL, 0);
	boundary(e, 6 + 4 * count, i->next);
}

static void emit_stm(emitter_t *e, const insn_t *i)
{
	int n, count = 0;
	op1(e, SLJIT_MOV_U16, VAL, 0, REG16(7));
	for (n = 7; n >= 0; n--)
	{
		if (!(i->b1 & (1u << n)))
			continue;
		count++;
		op2(e, SLJIT_SUB, VAL, 0, VAL, 0, SLJIT_IMM, 2);
		op2(e, SLJIT_AND, VAL, 0, VAL, 0, SLJIT_IMM, 0xffff);
		op1(e, SLJIT_MOV_U8, EA, 0, CELL(tp));
		op2(e, SLJIT_SHL, EA, 0, EA, 0, SLJIT_IMM, 16);
		op2(e, SLJIT_OR, EA, 0, EA, 0, VAL, 0);
		op1(e, SLJIT_MOV_U16, VAL2, 0, REG16(n));
		if (n == 7)
		{
			op2(e, SLJIT_SUB, VAL2, 0, VAL2, 0, SLJIT_IMM, 2);
			op2(e, SLJIT_AND, VAL2, 0, VAL2, 0, SLJIT_IMM, 0xffff);
		}
		mem_write(e, 1, VAL2);
	}
	op1(e, SLJIT_MOV_U16, REG16(7), VAL, 0);
	boundary(e, 6 + 3 * count, i->next);
}

static void emit_bcc(emitter_t *e, const insn_t *i)
{
	int cc = i->b0 & 15;
	uint16_t target = (uint16_t)(i->next + i->disp);
	struct sljit_jump *not_taken;
	if (cc == 0)
	{
		direct_exit(e, e->cp, target, 7);
		return;
	}
	if (cc == 1)
	{
		direct_exit(e, e->cp, i->next, 3);
		return;
	}
	condition(e, cc);
	not_taken = sljit_emit_cmp(e->c, SLJIT_EQUAL, SLJIT_R0, 0, SLJIT_IMM, 0);
	direct_exit(e, e->cp, target, 7);
	bind(e, not_taken);
	direct_exit(e, e->cp, i->next, 3);
}

/* Z from the bit test of VAL against the mask in R1 */
static void bit_z(emitter_t *e)
{
	op2(e, SLJIT_AND, SLJIT_R2, 0, VAL, 0, SLJIT_R1, 0);
	op2(e, SLJIT_SUB, SLJIT_R2, 0, SLJIT_R2, 0, SLJIT_IMM, 1);
	op2(e, SLJIT_LSHR, SLJIT_R2, 0, SLJIT_R2, 0, SLJIT_IMM, 61);
	op2(e, SLJIT_AND, SLJIT_R2, 0, SLJIT_R2, 0, SLJIT_IMM, FLAG_Z);
	op1(e, SLJIT_MOV_U16, SLJIT_R3, 0, CELL(sr));
	op2(e, SLJIT_AND, SLJIT_R3, 0, SLJIT_R3, 0, SLJIT_IMM, 0xffff & ~FLAG_Z);
	op2(e, SLJIT_OR, SLJIT_R3, 0, SLJIT_R3, 0, SLJIT_R2, 0);
	op1(e, SLJIT_MOV_U16, CELL(sr), SLJIT_R3, 0);
}

/* set / clear / not / test of the bit whose mask is in R1, on VAL */
static void bit_apply(emitter_t *e, const insn_t *i, int op, int need)
{
	if (need)
		bit_z(e);
	switch (op)
	{
	case 0: op2(e, SLJIT_OR, VAL, 0, VAL, 0, SLJIT_R1, 0); break;
	case 1:
		op2(e, SLJIT_XOR, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_IMM, -1);
		op2(e, SLJIT_AND, VAL, 0, VAL, 0, SLJIT_R1, 0);
		break;
	case 2: op2(e, SLJIT_XOR, VAL, 0, VAL, 0, SLJIT_R1, 0); break;
	default: return;
	}
	ea_write(e, i, VAL, 0);
}

static void emit_shift(emitter_t *e, const insn_t *i, int need)
{
	int w = i->sz ? 16 : 8, msb = w - 1;
	sljit_sw mask = i->sz ? 0xffff : 0xff;
	ea_read(e, i, VAL);
	/* R1 = result, R2 = carry, R3 = V */
	movi(e, SLJIT_R3, 0);
	switch (i->op)
	{
	case 0x18:
		op2(e, SLJIT_SHL, SLJIT_R1, 0, VAL, 0, SLJIT_IMM, 1);
		op2(e, SLJIT_LSHR, SLJIT_R2, 0, VAL, 0, SLJIT_IMM, msb);
		op2(e, SLJIT_XOR, SLJIT_R3, 0, VAL, 0, SLJIT_R1, 0);
		op2(e, SLJIT_LSHR, SLJIT_R3, 0, SLJIT_R3, 0, SLJIT_IMM, msb);
		op2(e, SLJIT_AND, SLJIT_R3, 0, SLJIT_R3, 0, SLJIT_IMM, 1);
		break;
	case 0x19:
		op2(e, SLJIT_SHL, SLJIT_R1, 0, VAL, 0, SLJIT_IMM, 64 - w);
		op2(e, SLJIT_ASHR, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_IMM, 65 - w);
		op2(e, SLJIT_AND, SLJIT_R2, 0, VAL, 0, SLJIT_IMM, 1);
		break;
	case 0x1a:
		op2(e, SLJIT_SHL, SLJIT_R1, 0, VAL, 0, SLJIT_IMM, 1);
		op2(e, SLJIT_LSHR, SLJIT_R2, 0, VAL, 0, SLJIT_IMM, msb);
		break;
	case 0x1b:
		op2(e, SLJIT_LSHR, SLJIT_R1, 0, VAL, 0, SLJIT_IMM, 1);
		op2(e, SLJIT_AND, SLJIT_R2, 0, VAL, 0, SLJIT_IMM, 1);
		break;
	case 0x1c:
		op2(e, SLJIT_LSHR, SLJIT_R2, 0, VAL, 0, SLJIT_IMM, msb);
		op2(e, SLJIT_SHL, SLJIT_R1, 0, VAL, 0, SLJIT_IMM, 1);
		op2(e, SLJIT_OR, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_R2, 0);
		break;
	case 0x1d:
		op2(e, SLJIT_AND, SLJIT_R2, 0, VAL, 0, SLJIT_IMM, 1);
		op2(e, SLJIT_LSHR, SLJIT_R1, 0, VAL, 0, SLJIT_IMM, 1);
		op2(e, SLJIT_SHL, SLJIT_R0, 0, SLJIT_R2, 0, SLJIT_IMM, msb);
		op2(e, SLJIT_OR, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_R0, 0);
		break;
	case 0x1e:
		op1(e, SLJIT_MOV_U16, SLJIT_R0, 0, CELL(sr));
		op2(e, SLJIT_AND, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, FLAG_C);
		op2(e, SLJIT_LSHR, SLJIT_R2, 0, VAL, 0, SLJIT_IMM, msb);
		op2(e, SLJIT_SHL, SLJIT_R1, 0, VAL, 0, SLJIT_IMM, 1);
		op2(e, SLJIT_OR, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_R0, 0);
		break;
	default:
		op1(e, SLJIT_MOV_U16, SLJIT_R0, 0, CELL(sr));
		op2(e, SLJIT_AND, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, FLAG_C);
		op2(e, SLJIT_SHL, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, msb);
		op2(e, SLJIT_AND, SLJIT_R2, 0, VAL, 0, SLJIT_IMM, 1);
		op2(e, SLJIT_LSHR, SLJIT_R1, 0, VAL, 0, SLJIT_IMM, 1);
		op2(e, SLJIT_OR, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_R0, 0);
		break;
	}
	op2(e, SLJIT_AND, VAL2, 0, SLJIT_R1, 0, SLJIT_IMM, mask);
	op2(e, SLJIT_SHL, SLJIT_R3, 0, SLJIT_R3, 0, SLJIT_IMM, 1);
	op2(e, SLJIT_AND, SLJIT_R2, 0, SLJIT_R2, 0, SLJIT_IMM, 1);
	op2(e, SLJIT_OR, VAL, 0, SLJIT_R2, 0, SLJIT_R3, 0);
	ea_write(e, i, VAL2, 0);
	if (need)
	{
		flags_nz(e, VAL2, w, 1);
		op1(e, SLJIT_MOV_U16, SLJIT_R3, 0, CELL(sr));
		op2(e, SLJIT_OR, SLJIT_R3, 0, SLJIT_R3, 0, VAL, 0);
		if (i->op == 0x1b)
			op2(e, SLJIT_AND, SLJIT_R3, 0, SLJIT_R3, 0, SLJIT_IMM, 0xffff & ~FLAG_N);
		op1(e, SLJIT_MOV_U16, CELL(sr), SLJIT_R3, 0);
	}
}

static void emit_divxu(emitter_t *e, const insn_t *i)
{
	int d = i->op & 7;
	struct sljit_jump *zero, *ovf, *done1, *done2;

	if (i->sz)
	{
		op1(e, SLJIT_MOV_U16, VAL2, 0, REG16(d));
		op2(e, SLJIT_SHL, VAL2, 0, VAL2, 0, SLJIT_IMM, 16);
		op1(e, SLJIT_MOV_U16, SLJIT_R0, 0, REG16((d + 1) & 7));
		op2(e, SLJIT_OR, VAL2, 0, VAL2, 0, SLJIT_R0, 0);
	}
	else
		op1(e, SLJIT_MOV_U16, VAL2, 0, REG16(d));
	ea_read(e, i, VAL);
	zero = sljit_emit_cmp(e->c, SLJIT_EQUAL, VAL, 0, SLJIT_IMM, 0);
	op2(e, SLJIT_LSHR, SLJIT_R0, 0, VAL2, 0, SLJIT_IMM, i->sz ? 16 : 8);
	ovf = sljit_emit_cmp(e->c, SLJIT_GREATER_EQUAL, SLJIT_R0, 0, VAL, 0);
	movr(e, SLJIT_R0, VAL2);
	movr(e, SLJIT_R1, VAL);
	sljit_emit_op0(e->c, SLJIT_DIVMOD_U32);
	if (i->sz)
	{
		op1(e, SLJIT_MOV_U16, REG16(d), SLJIT_R1, 0);
		op1(e, SLJIT_MOV_U16, REG16((d + 1) & 7), SLJIT_R0, 0);
		op2(e, SLJIT_AND, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, 0xffff);
		flags_nz(e, SLJIT_R0, 16, 1);
	}
	else
	{
		op2(e, SLJIT_AND, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, 0xff);
		op2(e, SLJIT_SHL, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_IMM, 8);
		op2(e, SLJIT_OR, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_R0, 0);
		op1(e, SLJIT_MOV_U16, REG16(d), SLJIT_R1, 0);
		flags_nz(e, SLJIT_R0, 8, 1);
	}
	done1 = sljit_emit_jump(e->c, SLJIT_JUMP);

	bind(e, ovf);
	sr_set_clear(e, FLAG_V, FLAG_N | FLAG_Z | FLAG_C);
	done2 = sljit_emit_jump(e->c, SLJIT_JUMP);

	bind(e, zero);
	call_pre(e);
	movi(e, SLJIT_R1, i->pc);
	sljit_emit_icall(e->c, SLJIT_CALL, SLJIT_ARGS2V(P, W), SLJIT_IMM, (sljit_sw)(uintptr_t)hdivzero);
	call_post(e);
	op2(e, SLJIT_ADD, PENDING, 0, PENDING, 0, SLJIT_IMM, i->sz ? 22 : 16);
	emit_return(e);

	bind(e, done1);
	bind(e, done2);
	boundary(e, i->cyc, i->next);
}

static sljit_sw cr_offset(int c)
{
	switch (c)
	{
	case 3: return (sljit_sw)offsetof(h8500_t, br);
	case 4: return (sljit_sw)offsetof(h8500_t, ep);
	case 5: return (sljit_sw)offsetof(h8500_t, dp);
	default: return (sljit_sw)offsetof(h8500_t, tp);
	}
}

/* the control register's value as the interpreter's cr_read, into dst */
static void cr_read(emitter_t *e, int c, sljit_s32 dst)
{
	switch (c)
	{
	case 0:
		op1(e, SLJIT_MOV_U16, dst, 0, CELL(sr));
		op2(e, SLJIT_AND, dst, 0, dst, 0, SLJIT_IMM, 0x870f);
		break;
	case 1:
		op1(e, SLJIT_MOV_U16, dst, 0, CELL(sr));
		op2(e, SLJIT_AND, dst, 0, dst, 0, SLJIT_IMM, 0x0f);
		break;
	case 3: case 4: case 5: case 7:
		op1(e, SLJIT_MOV_U8, dst, 0, SLJIT_MEM1(CPU), cr_offset(c));
		break;
	default:
		movi(e, dst, 0);
		break;
	}
}

static void cr_write(emitter_t *e, int c, sljit_s32 src)
{
	switch (c)
	{
	case 0:
		op2(e, SLJIT_AND, src, 0, src, 0, SLJIT_IMM, 0x870f);
		op1(e, SLJIT_MOV_U16, CELL(sr), src, 0);
		break;
	case 1:
		op1(e, SLJIT_MOV_U16, SLJIT_R2, 0, CELL(sr));
		op2(e, SLJIT_AND, SLJIT_R2, 0, SLJIT_R2, 0, SLJIT_IMM, 0xfff0);
		op2(e, SLJIT_AND, src, 0, src, 0, SLJIT_IMM, 0x0f);
		op2(e, SLJIT_OR, SLJIT_R2, 0, SLJIT_R2, 0, src, 0);
		op1(e, SLJIT_MOV_U16, CELL(sr), SLJIT_R2, 0);
		break;
	case 3: case 4: case 5: case 7:
		op1(e, SLJIT_MOV_U8, SLJIT_MEM1(CPU), cr_offset(c), src, 0);
		break;
	default:
		break;
	}
}

static void emit_ldc(emitter_t *e, const insn_t *i)
{
	int c = i->op & 7;
	if (i->sz)
	{
		if (c == 0 || c == 3 || c == 4 || c == 5)
		{
			ea_read(e, i, VAL);
			if (c == 0)
				cr_write(e, 0, VAL);
			else
			{
				op2(e, SLJIT_AND, VAL, 0, VAL, 0, SLJIT_IMM, 0xff);
				cr_write(e, c, VAL);
			}
		}
	}
	else if (c == 4 && i->byte_stack)
	{
		mem_read(e, 1, VAL);
		ea_commit(e, i);
		op2(e, SLJIT_LSHR, SLJIT_R0, 0, VAL, 0, SLJIT_IMM, 8);
		op1(e, SLJIT_MOV_U8, CELL(ep), SLJIT_R0, 0);
		op1(e, SLJIT_MOV_U8, CELL(dp), VAL, 0);
	}
	else
	{
		ea_read(e, i, VAL);
		if (c != 0 && c != 2 && c != 6)
			cr_write(e, c, VAL);
	}
	op1(e, SLJIT_MOV_U8, CELL(no_irq), SLJIT_IMM, 1);
	op2(e, SLJIT_ADD, PENDING, 0, PENDING, 0, SLJIT_IMM, i->cyc);
	emit_exit_now(e, i->next);
}

static void emit_stc(emitter_t *e, const insn_t *i)
{
	int c = i->op & 7;
	if (i->sz)
	{
		if (c == 0)
		{
			cr_read(e, 0, VAL);
			ea_write(e, i, VAL, 0);
		}
		else if (c == 3 || c == 4 || c == 5)
		{
			cr_read(e, c, VAL);
			op2(e, SLJIT_SHL, SLJIT_R0, 0, VAL, 0, SLJIT_IMM, 8);
			op2(e, SLJIT_OR, VAL, 0, VAL, 0, SLJIT_R0, 0);
			ea_write(e, i, VAL, 0);
		}
	}
	else if (c == 4 && i->byte_stack)
	{
		op1(e, SLJIT_MOV_U8, VAL, 0, CELL(ep));
		op2(e, SLJIT_SHL, VAL, 0, VAL, 0, SLJIT_IMM, 8);
		op1(e, SLJIT_MOV_U8, SLJIT_R0, 0, CELL(dp));
		op2(e, SLJIT_OR, VAL, 0, VAL, 0, SLJIT_R0, 0);
		mem_write(e, 1, VAL);
		ea_commit(e, i);
	}
	else
	{
		cr_read(e, c, VAL);
		op2(e, SLJIT_AND, VAL, 0, VAL, 0, SLJIT_IMM, 0xff);
		ea_write(e, i, VAL, 0);
	}
	boundary(e, i->cyc, i->next);
}

static void emit_crimm(emitter_t *e, const insn_t *i)
{
	int c = i->op & 7;
	sljit_sw imm = (i->mode == EM_IMM16) ? (sljit_sw)((i->ext[0] << 8) | i->ext[1]) : (sljit_sw)i->ext[0];
	op1(e, SLJIT_MOV_U8, CELL(no_irq), SLJIT_IMM, 1);
	if (c != 2 && c != 6 && !(i->sz && c != 0))
	{
		cr_read(e, c, SLJIT_R0);
		op2(e, (i->op & 0xf8) == 0x48 ? SLJIT_OR : (i->op & 0xf8) == 0x58 ? SLJIT_AND : SLJIT_XOR,
		    SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, imm);
		cr_write(e, c, SLJIT_R0);
		if (c != 0 && c != 1)
		{
			cr_read(e, c, SLJIT_R0);
			flags_nz(e, SLJIT_R0, 8, 0);
		}
	}
	op2(e, SLJIT_ADD, PENDING, 0, PENDING, 0, SLJIT_IMM, i->cyc);
	emit_exit_now(e, i->next);
}

static void emit_trapa(emitter_t *e, const insn_t *i)
{
	sljit_sw va = (sljit_sw)(16 + (i->b1 & 15)) * 4;
	movi(e, SLJIT_R3, i->next);
	push16(e, SLJIT_R3);
	op1(e, SLJIT_MOV_U8, SLJIT_R3, 0, CELL(cp));
	push16(e, SLJIT_R3);
	op1(e, SLJIT_MOV_U16, SLJIT_R3, 0, CELL(sr));
	push16(e, SLJIT_R3);
	op1(e, SLJIT_MOV_U16, SLJIT_R3, 0, CELL(sr));
	op2(e, SLJIT_AND, SLJIT_R3, 0, SLJIT_R3, 0, SLJIT_IMM, 0x7fff);
	op1(e, SLJIT_MOV_U16, CELL(sr), SLJIT_R3, 0);
	movi(e, EA, va);
	mem_read(e, 1, VAL);
	op1(e, SLJIT_MOV_U8, CELL(cp), VAL, 0);
	movi(e, EA, va + 2);
	mem_read(e, 1, VAL);
	computed_exit(e, VAL, 0, 22);
}

static void emit_rte(emitter_t *e, const insn_t *i)
{
	(void)i;
	pop16(e, VAL);
	cr_write(e, 0, VAL);
	pop16(e, VAL);
	op1(e, SLJIT_MOV_U8, CELL(cp), VAL, 0);
	pop16(e, VAL);
	op1(e, SLJIT_MOV_U8, CELL(no_irq), SLJIT_IMM, 1);
	computed_exit(e, VAL, 0, 15);
}

static void emit_insn(emitter_t *e, const insn_t *i, int need)
{
	int w = i->sz ? 16 : 8;
	sljit_sw mask = i->sz ? 0xffff : 0xff;
	int d = i->op & 7;

	switch (i->kind)
	{
	case K_FALLBACK:
		emit_fallback(e, i);
		return;
	case K_NOP:
		boundary(e, 2, i->next);
		return;
	case K_SCB:
		emit_scb(e, i);
		return;
	case K_LDM:
		emit_ldm(e, i);
		return;
	case K_STM:
		emit_stm(e, i);
		return;
	case K_PJSR24:
		movi(e, SLJIT_R3, i->next);
		push16(e, SLJIT_R3);
		op1(e, SLJIT_MOV_U8, SLJIT_R3, 0, CELL(cp));
		push16(e, SLJIT_R3);
		op1(e, SLJIT_MOV_U8, CELL(cp), SLJIT_IMM, i->b1);
		direct_exit(e, i->b1, i->imm, 15);
		return;
	case K_PJMP24:
		op1(e, SLJIT_MOV_U8, CELL(cp), SLJIT_IMM, i->b1);
		direct_exit(e, i->b1, i->imm, 9);
		return;
	case K_BSR:
		movi(e, SLJIT_R3, i->next);
		push16(e, SLJIT_R3);
		direct_exit(e, e->cp, (uint16_t)(i->next + i->disp), 9);
		return;
	case K_UNLK:
		op1(e, SLJIT_MOV_U16, SLJIT_R0, 0, REG16(6));
		op1(e, SLJIT_MOV_U16, REG16(7), SLJIT_R0, 0);
		pop16(e, VAL);
		op1(e, SLJIT_MOV_U16, REG16(6), VAL, 0);
		boundary(e, 5, i->next);
		return;
	case K_JMP16:
		direct_exit(e, e->cp, i->imm, 7);
		return;
	case K_JSR16:
		movi(e, SLJIT_R3, i->next);
		push16(e, SLJIT_R3);
		direct_exit(e, e->cp, i->imm, 9);
		return;
	case K_PRTD:
	case K_PRTS:
		pop16(e, VAL);
		op1(e, SLJIT_MOV_U8, CELL(cp), VAL, 0);
		pop16(e, VAL);
		if (i->kind == K_PRTD)
		{
			op1(e, SLJIT_MOV_U16, SLJIT_R0, 0, REG16(7));
			op2(e, SLJIT_ADD, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, i->disp);
			op1(e, SLJIT_MOV_U16, REG16(7), SLJIT_R0, 0);
		}
		computed_exit(e, VAL, 0, i->cyc);
		return;
	case K_PJMPR:
		op1(e, SLJIT_MOV_U8, CELL(cp), REG8(i->reg));
		computed_exit(e, REG16((i->reg + 1) & 7), i->cyc);
		return;
	case K_PJSRR:
		op1(e, SLJIT_MOV_U16, VAL, 0, REG16(i->reg));
		op1(e, SLJIT_MOV_U16, VAL2, 0, REG16((i->reg + 1) & 7));
		movi(e, SLJIT_R3, i->next);
		push16(e, SLJIT_R3);
		op1(e, SLJIT_MOV_U8, SLJIT_R3, 0, CELL(cp));
		push16(e, SLJIT_R3);
		op1(e, SLJIT_MOV_U8, CELL(cp), VAL, 0);
		computed_exit(e, VAL2, 0, i->cyc);
		return;
	case K_JMPR:
		computed_exit(e, REG16(i->reg), i->cyc);
		return;
	case K_JSRR:
		op1(e, SLJIT_MOV_U16, VAL, 0, REG16(i->reg));
		movi(e, SLJIT_R3, i->next);
		push16(e, SLJIT_R3);
		computed_exit(e, VAL, 0, i->cyc);
		return;
	case K_JMPD:
	case K_JSRD:
		op1(e, SLJIT_MOV_U16, VAL, 0, REG16(i->reg));
		op2(e, SLJIT_ADD, VAL, 0, VAL, 0, SLJIT_IMM, i->disp);
		op2(e, SLJIT_AND, VAL, 0, VAL, 0, SLJIT_IMM, 0xffff);
		if (i->kind == K_JSRD)
		{
			movi(e, SLJIT_R3, i->next);
			push16(e, SLJIT_R3);
		}
		computed_exit(e, VAL, 0, i->cyc);
		return;
	case K_RTD:
		pop16(e, VAL);
		op1(e, SLJIT_MOV_U16, SLJIT_R0, 0, REG16(7));
		op2(e, SLJIT_ADD, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, i->disp);
		op1(e, SLJIT_MOV_U16, REG16(7), SLJIT_R0, 0);
		computed_exit(e, VAL, 0, 9);
		return;
	case K_LINK:
		op1(e, SLJIT_MOV_U16, SLJIT_R3, 0, REG16(6));
		push16(e, SLJIT_R3);
		op1(e, SLJIT_MOV_U16, SLJIT_R0, 0, REG16(7));
		op1(e, SLJIT_MOV_U16, REG16(6), SLJIT_R0, 0);
		op2(e, SLJIT_ADD, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, i->disp);
		op1(e, SLJIT_MOV_U16, REG16(7), SLJIT_R0, 0);
		boundary(e, i->cyc, i->next);
		return;
	case K_RTS:
		pop16(e, VAL);
		computed_exit(e, VAL, 0, 8);
		return;
	case K_SLEEP:
		op1(e, SLJIT_MOV_U8, CELL(sleeping), SLJIT_IMM, 1);
		op2(e, SLJIT_ADD, PENDING, 0, PENDING, 0, SLJIT_IMM, 2);
		emit_exit_now(e, i->next);
		return;
	case K_BCC:
		emit_bcc(e, i);
		return;
	case K_TRAPA:
		emit_trapa(e, i);
		return;
	case K_RTE:
		emit_rte(e, i);
		return;
	case K_CMPE:
	case K_CMPI:
		if (need)
		{
			load_reg(e, SLJIT_R0, i->reg, i->kind == K_CMPI);
			movi(e, VAL, i->imm);
			op2(e, SLJIT_SUB, SLJIT_R1, 0, SLJIT_R0, 0, VAL, 0);
			flags_arith(e, i->kind == K_CMPI ? 16 : 8, 1, 0);
		}
		boundary(e, i->cyc, i->next);
		return;
	case K_MOVE:
	case K_MOVI:
		store_reg(e, i->reg, i->kind == K_MOVI, SLJIT_IMM, i->imm);
		if (need)
		{
			movi(e, SLJIT_R0, i->imm);
			flags_nz(e, SLJIT_R0, i->kind == K_MOVI ? 16 : 8, 0);
		}
		boundary(e, i->cyc, i->next);
		return;
	case K_MOVL:
	case K_MOVS:
		op1(e, SLJIT_MOV_U8, EA, 0, CELL(br));
		op2(e, SLJIT_SHL, EA, 0, EA, 0, SLJIT_IMM, 8);
		op2(e, SLJIT_OR, EA, 0, EA, 0, SLJIT_IMM, i->ext[0]);
		if (i->kind == K_MOVL)
		{
			mem_read(e, i->sz, VAL);
			store_reg(e, i->reg, i->sz, VAL, 0);
		}
		else
		{
			load_reg(e, VAL, i->reg, i->sz);
			mem_write(e, i->sz, VAL);
		}
		if (need)
			flags_nz(e, VAL, w, 0);
		boundary(e, 5, i->next);
		return;
	case K_MOVF:
	case K_MOVFS:
		op1(e, SLJIT_MOV_U16, SLJIT_R0, 0, REG16(6));
		op2(e, SLJIT_ADD, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, i->disp);
		op2(e, SLJIT_AND, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, 0xffff);
		op1(e, SLJIT_MOV_U8, EA, 0, CELL(tp));
		op2(e, SLJIT_SHL, EA, 0, EA, 0, SLJIT_IMM, 16);
		op2(e, SLJIT_OR, EA, 0, EA, 0, SLJIT_R0, 0);
		if (i->kind == K_MOVF)
		{
			mem_read(e, i->sz, VAL);
			store_reg(e, i->reg, i->sz, VAL, 0);
		}
		else
		{
			load_reg(e, VAL, i->reg, i->sz);
			mem_write(e, i->sz, VAL);
		}
		if (need)
			flags_nz(e, VAL, w, 0);
		boundary(e, 5, i->next);
		return;
	default:
		break;
	}

	ea_address(e, i);
	switch (i->kind)
	{
	case K_G_CMPIMM:
	{
		sljit_sw s;
		if (i->sz)
			s = (i->op == 0x04) ? (sljit_sw)(uint16_t)(int16_t)(int8_t)i->imm : (sljit_sw)i->imm;
		else
			s = i->imm & 0xff;
		ea_read(e, i, VAL2);
		if (need)
		{
			movr(e, SLJIT_R0, VAL2);
			movi(e, VAL, s);
			op2(e, SLJIT_SUB, SLJIT_R1, 0, SLJIT_R0, 0, VAL, 0);
			flags_arith(e, w, 1, 0);
		}
		break;
	}
	case K_G_MOVIMM:
	{
		sljit_sw s;
		if (i->sz)
			s = (i->op == 0x06) ? (sljit_sw)(uint16_t)(int16_t)(int8_t)i->imm : (sljit_sw)i->imm;
		else
			s = i->imm & 0xff;
		ea_write(e, i, SLJIT_IMM, s);
		if (need)
		{
			movi(e, SLJIT_R0, s);
			flags_nz(e, SLJIT_R0, w, 0);
		}
		break;
	}
	case K_G_ADDQ:
	{
		int add = (i->op == 0x08) ? 1 : (i->op == 0x09) ? 2 : (i->op == 0x0c) ? -1 : -2;
		ea_read(e, i, VAL2);
		movr(e, SLJIT_R0, VAL2);
		movi(e, VAL, add & mask);
		op2(e, SLJIT_ADD, SLJIT_R1, 0, SLJIT_R0, 0, VAL, 0);
		op2(e, SLJIT_AND, VAL2, 0, SLJIT_R1, 0, SLJIT_IMM, mask);
		if (need)
			flags_arith(e, w, 0, 0);
		ea_write(e, i, VAL2, 0);
		break;
	}
	case K_G_SWAP:
		op1(e, SLJIT_MOV_U16, SLJIT_R0, 0, REG16(i->reg));
		op2(e, SLJIT_LSHR, SLJIT_R1, 0, SLJIT_R0, 0, SLJIT_IMM, 8);
		op2(e, SLJIT_SHL, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, 8);
		op2(e, SLJIT_OR, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_R0, 0);
		op2(e, SLJIT_AND, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_IMM, 0xffff);
		op1(e, SLJIT_MOV_U16, REG16(i->reg), SLJIT_R1, 0);
		if (need)
			flags_nz(e, SLJIT_R1, 16, 0);
		break;
	case K_G_EXTS:
		op1(e, SLJIT_MOV_S8, SLJIT_R0, 0, REG8(i->reg));
		op2(e, SLJIT_AND, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, 0xffff);
		op1(e, SLJIT_MOV_U16, REG16(i->reg), SLJIT_R0, 0);
		if (need)
			flags_nz(e, SLJIT_R0, 16, 1);
		break;
	case K_G_EXTU:
		op1(e, SLJIT_MOV_U8, SLJIT_R0, 0, REG8(i->reg));
		op1(e, SLJIT_MOV_U16, REG16(i->reg), SLJIT_R0, 0);
		if (need)
			flags_nz(e, SLJIT_R0, 16, 1);
		break;
	case K_G_CLR:
		ea_write(e, i, SLJIT_IMM, 0);
		if (need)
			sr_set_clear(e, FLAG_Z, FLAG_N | FLAG_V | FLAG_C);
		break;
	case K_G_NEG:
		ea_read(e, i, VAL);
		movi(e, SLJIT_R0, 0);
		op2(e, SLJIT_SUB, SLJIT_R1, 0, SLJIT_R0, 0, VAL, 0);
		op2(e, SLJIT_AND, VAL2, 0, SLJIT_R1, 0, SLJIT_IMM, mask);
		if (need)
			flags_arith(e, w, 1, 0);
		ea_write(e, i, VAL2, 0);
		break;
	case K_G_NOT:
		ea_read(e, i, VAL);
		op2(e, SLJIT_XOR, VAL2, 0, VAL, 0, SLJIT_IMM, -1);
		op2(e, SLJIT_AND, VAL2, 0, VAL2, 0, SLJIT_IMM, mask);
		ea_write(e, i, VAL2, 0);
		if (need)
			flags_nz(e, VAL2, w, 0);
		break;
	case K_G_TST:
		ea_read(e, i, VAL);
		if (need)
			flags_nz(e, VAL, w, 1);
		break;
	case K_G_TAS:
		ea_read(e, i, VAL);
		if (need)
			flags_nz(e, VAL, 8, 1);
		op2(e, SLJIT_OR, VAL, 0, VAL, 0, SLJIT_IMM, 0x80);
		ea_write(e, i, VAL, 0);
		break;
	case K_G_SHIFT:
		emit_shift(e, i, need);
		break;
	case K_G_ADD:
	case K_G_SUB:
		ea_read(e, i, VAL);
		load_reg(e, SLJIT_R0, d, i->sz);
		op2(e, i->kind == K_G_ADD ? SLJIT_ADD : SLJIT_SUB, SLJIT_R1, 0, SLJIT_R0, 0, VAL, 0);
		store_reg(e, d, i->sz, SLJIT_R1, 0);
		if (need)
			flags_arith(e, w, i->kind == K_G_SUB, 0);
		break;
	case K_G_ADDS:
	case K_G_SUBS:
		ea_read(e, i, VAL);
		if (!i->sz)
		{
			op1(e, SLJIT_MOV_S8, VAL, 0, VAL, 0);
			op2(e, SLJIT_AND, VAL, 0, VAL, 0, SLJIT_IMM, 0xffff);
		}
		op1(e, SLJIT_MOV_U16, SLJIT_R0, 0, REG16(d));
		op2(e, i->kind == K_G_ADDS ? SLJIT_ADD : SLJIT_SUB, SLJIT_R0, 0, SLJIT_R0, 0, VAL, 0);
		op1(e, SLJIT_MOV_U16, REG16(d), SLJIT_R0, 0);
		break;
	case K_G_LOGIC:
		ea_read(e, i, VAL);
		load_reg(e, SLJIT_R0, d, i->sz);
		op2(e, (i->op & 0xf8) == 0x40 ? SLJIT_OR : (i->op & 0xf8) == 0x50 ? SLJIT_AND : SLJIT_XOR,
		    SLJIT_R1, 0, SLJIT_R0, 0, VAL, 0);
		store_reg(e, d, i->sz, SLJIT_R1, 0);
		if (need)
			flags_nz(e, SLJIT_R1, w, 0);
		break;
	case K_G_CMP:
		load_reg(e, VAL2, d, i->sz);
		ea_read(e, i, VAL);
		if (need)
		{
			movr(e, SLJIT_R0, VAL2);
			op2(e, SLJIT_SUB, SLJIT_R1, 0, SLJIT_R0, 0, VAL, 0);
			flags_arith(e, w, 1, 0);
		}
		break;
	case K_G_BITR:
		op1(e, SLJIT_MOV_U16, VAL2, 0, REG16(d));
		ea_read(e, i, VAL);
		op2(e, SLJIT_AND, VAL2, 0, VAL2, 0, SLJIT_IMM, 15);
		movi(e, SLJIT_R1, 1);
		op2(e, SLJIT_SHL, SLJIT_R1, 0, SLJIT_R1, 0, VAL2, 0);
		bit_apply(e, i, (i->op & 0xf8) == 0x48 ? 0 : (i->op & 0xf8) == 0x58 ? 1 : 2, need);
		break;
	case K_G_BTSTR:
		ea_read(e, i, VAL);
		op1(e, SLJIT_MOV_U16, VAL2, 0, REG16(d));
		op2(e, SLJIT_AND, VAL2, 0, VAL2, 0, SLJIT_IMM, 15);
		movi(e, SLJIT_R1, 1);
		op2(e, SLJIT_SHL, SLJIT_R1, 0, SLJIT_R1, 0, VAL2, 0);
		if (need)
			bit_z(e);
		break;
	case K_G_BITI:
		ea_read(e, i, VAL);
		movi(e, SLJIT_R1, 1 << (i->op & 15));
		bit_apply(e, i, (i->op >> 4) - 0xc, need);
		break;
	case K_G_MOVFROM:
		ea_read(e, i, VAL);
		store_reg(e, d, i->sz, VAL, 0);
		if (need)
			flags_nz(e, VAL, w, 0);
		break;
	case K_G_MOVTO:
		load_reg(e, VAL, d, i->sz);
		ea_write(e, i, VAL, 0);
		if (need)
			flags_nz(e, VAL, w, 0);
		break;
	case K_G_XCH:
		op1(e, SLJIT_MOV_U16, SLJIT_R0, 0, REG16(i->reg));
		op1(e, SLJIT_MOV_U16, SLJIT_R1, 0, REG16(d));
		op1(e, SLJIT_MOV_U16, REG16(i->reg), SLJIT_R1, 0);
		op1(e, SLJIT_MOV_U16, REG16(d), SLJIT_R0, 0);
		break;
	case K_G_ADDX:
	case K_G_SUBX:
		ea_read(e, i, VAL);
		load_reg(e, SLJIT_R0, d, i->sz);
		op1(e, SLJIT_MOV_U16, SLJIT_R2, 0, CELL(sr));
		op2(e, SLJIT_AND, SLJIT_R2, 0, SLJIT_R2, 0, SLJIT_IMM, FLAG_C);
		if (i->kind == K_G_ADDX)
		{
			op2(e, SLJIT_ADD, SLJIT_R1, 0, SLJIT_R0, 0, VAL, 0);
			op2(e, SLJIT_ADD, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_R2, 0);
		}
		else
		{
			op2(e, SLJIT_SUB, SLJIT_R1, 0, SLJIT_R0, 0, VAL, 0);
			op2(e, SLJIT_SUB, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_R2, 0);
		}
		store_reg(e, d, i->sz, SLJIT_R1, 0);
		if (need)
			flags_arith(e, w, i->kind == K_G_SUBX, 1);
		break;
	case K_G_MULXU:
		ea_read(e, i, VAL);
		load_reg(e, SLJIT_R0, d, i->sz);
		op2(e, SLJIT_MUL, SLJIT_R1, 0, SLJIT_R0, 0, VAL, 0);
		if (i->sz)
		{
			op2(e, SLJIT_LSHR, SLJIT_R2, 0, SLJIT_R1, 0, SLJIT_IMM, 16);
			op1(e, SLJIT_MOV_U16, REG16(d), SLJIT_R2, 0);
			op1(e, SLJIT_MOV_U16, REG16((d + 1) & 7), SLJIT_R1, 0);
			if (need)
				flags_nz(e, SLJIT_R1, 32, 1);
		}
		else
		{
			op1(e, SLJIT_MOV_U16, REG16(d), SLJIT_R1, 0);
			if (need)
				flags_nz(e, SLJIT_R1, 16, 1);
		}
		break;
	case K_G_DIVXU:
		emit_divxu(e, i);
		return;
	case K_G_LDC:
		emit_ldc(e, i);
		return;
	case K_G_STC:
		emit_stc(e, i);
		return;
	case K_G_CRIMM:
		emit_crimm(e, i);
		return;
	default:
		e->failed = true;
		return;
	}
	boundary(e, i->cyc, i->next);
}

/* ------------------------------------------------------------------ */
/* blocks                                                             */
/* ------------------------------------------------------------------ */

static void patch_slots(struct h8500_jit *j, uint32_t key, void *body)
{
	slot_chunk_t *ch;
	for (ch = j->chunks; ch; ch = ch->next)
	{
		uint32_t n;
		for (n = 0; n < ch->used; n++)
			if (ch->slots[n].key == key)
				ch->slots[n].target = body;
	}
}

static block_t *translate(h8500_t *cpu, uint32_t key)
{
	struct h8500_jit *j = cpu->jit;
	block_t *b;
	insn_t insns[BLOCK_MAX_INSNS];
	uint8_t need[BLOCK_MAX_INSNS];
	int count = 0, n;
	decoder_t d;
	emitter_t e;
	struct sljit_label *body;
	const h8500_region_t *r = NULL;
	void *code;

	if (j->count * 2 >= j->table_size && !grow(j))
		return NULL;
	b = lookup(j, key);
	b->key = key;
	b->fn = NULL;
	b->body = NULL;
	b->code = NULL;
	b->size = 0;
	j->count++;

	for (n = 0; n < cpu->region_count; n++)
		if ((key & cpu->var->addr_mask) - cpu->regions[n].base < cpu->regions[n].size)
		{
			r = cpu->regions[n].writable ? NULL : &cpu->regions[n];
			break;
		}
	if (!r)
		return b;

	d.base = r->data;
	d.region_base = r->base + (key & ~cpu->var->addr_mask);
	d.region_size = r->size;
	d.cp = (uint8_t)(key >> 16);
	d.pc = (uint16_t)key;
	d.overrun = false;
	while (count < BLOCK_MAX_INSNS)
	{
		decode(&d, &insns[count]);
		if (d.overrun)
			break;
		count++;
		if (insns[count - 1].term || d.pc < insns[count - 1].pc)
			break;
	}
	if (!count || (count == 1 && insns[0].kind == K_FALLBACK))
		return b;

	for (n = 0; n < count; n++)
		need[n] = insns[n].fw;

	memset(&e, 0, sizeof(e));
	e.j = j;
	e.cp = d.cp;
	e.c = sljit_create_compiler(NULL);
	if (!e.c)
		return b;
	sljit_emit_enter(e.c, 0, SLJIT_ARGS3(W, P, W, W), 6, 6, 0);
	body = sljit_emit_label(e.c);
	for (n = 0; n < count && !e.failed; n++)
		emit_insn(&e, &insns[n], need[n]);
	if (!insns[count - 1].term && !e.failed)
		chain(&e, d.cp, insns[count - 1].next);
	for (n = 0; n < e.exit_count; n++)
	{
		bind(&e, e.exits[n].jump);
		emit_exit_now(&e, e.exits[n].pc);
	}
	if (e.failed)
	{
		sljit_free_compiler(e.c);
		return b;
	}
	code = sljit_generate_code(e.c, 0, (void *)j->alloc->config);
	if (code)
	{
		b->code = code;
		b->fn = (block_fn)code;
		b->body = (void *)sljit_get_label_addr(body);
		b->size = sljit_get_generated_code_size(e.c);
		j->code_size += b->size;
		patch_slots(j, key, b->body);
	}
	sljit_free_compiler(e.c);
	return b;
}

static bool make_exit_stub(struct h8500_jit *j)
{
	struct sljit_compiler *c = sljit_create_compiler(NULL);
	struct sljit_label *body;
	if (!c)
		return false;
	sljit_emit_enter(c, 0, SLJIT_ARGS3(W, P, W, W), 6, 6, 0);
	body = sljit_emit_label(c);
	sljit_emit_return(c, SLJIT_MOV, PENDING, 0);
	j->exit_code = sljit_generate_code(c, 0, (void *)j->alloc->config);
	if (j->exit_code)
		j->exit_body = (void *)sljit_get_label_addr(body);
	sljit_free_compiler(c);
	return j->exit_code != NULL;
}

/* One entry per 4 KB of the 24-bit space; a page the address mask folds
 * elsewhere takes its region from the folded address, and a page holding
 * on-chip RAM is left to the slow path (the register file is diverted
 * before the table). */
static void build_pages(h8500_t *cpu)
{
	struct h8500_jit *j = cpu->jit;
	const h8500_variant_t *v = cpu->var;
	uint32_t p;
	for (p = 0; p < PAGE_COUNT; p++)
	{
		uint32_t addr = (p << PAGE_SHIFT) & v->addr_mask;
		const h8500_region_t *r = NULL;
		int n;
		uint8_t *biased;
		j->rd8[p] = j->rd16[p] = j->wr8[p] = j->wr16[p] = NULL;
		if (v->ram_size && addr + (1u << PAGE_SHIFT) > v->ram_base && addr < v->io_base)
			continue;
		for (n = 0; n < cpu->region_count; n++)
			if (addr - cpu->regions[n].base < cpu->regions[n].size)
			{
				r = &cpu->regions[n];
				break;
			}
		if (!r)
			continue;
		biased = (uint8_t *)((uintptr_t)r->data - r->base - ((p << PAGE_SHIFT) - addr));
		if (r->base + r->size >= addr + (1u << PAGE_SHIFT))
		{
			j->rd8[p] = biased;
			if (r->writable)
				j->wr8[p] = biased;
		}
		if (r->base + r->size >= addr + (1u << PAGE_SHIFT) + 1)
		{
			j->rd16[p] = biased;
			if (r->writable)
				j->wr16[p] = biased;
		}
	}
}

bool h8500_jit_attach(h8500_t *cpu, struct jit_alloc *alloc)
{
	struct h8500_jit *j;
	uint32_t i;
	h8500_jit_detach(cpu);
	j = calloc(1, sizeof(*j));
	if (!j)
		return false;
	j->alloc = alloc;
	j->table_size = 4096;
	j->table = malloc(j->table_size * sizeof(*j->table));
	if (!j->table)
	{
		free(j);
		return false;
	}
	for (i = 0; i < j->table_size; i++)
		j->table[i].key = KEY_NONE;
	if (!make_exit_stub(j))
	{
		free(j->table);
		free(j);
		return false;
	}
	j->io_base = cpu->var->io_base;
	j->io_size = cpu->var->io_size;
	j->addr_mask = cpu->var->addr_mask;
	cpu->jit = j;
	build_pages(cpu);
	cpu->jit_enabled = true;
	return true;
}

void h8500_jit_flush(h8500_t *cpu)
{
	struct h8500_jit *j = cpu->jit;
	uint32_t i;
	if (!j)
		return;
	for (i = 0; i < j->table_size; i++)
	{
		if (j->table[i].key != KEY_NONE && j->table[i].code)
			sljit_free_code(j->table[i].code, (void *)j->alloc->config);
		j->table[i].key = KEY_NONE;
	}
	while (j->chunks)
	{
		slot_chunk_t *ch = j->chunks;
		j->chunks = ch->next;
		free(ch);
	}
	j->count = 0;
	j->code_size = 0;
}

void h8500_jit_detach(h8500_t *cpu)
{
	struct h8500_jit *j = cpu->jit;
	if (!j)
		return;
	h8500_jit_flush(cpu);
	if (j->exit_code)
		sljit_free_code(j->exit_code, (void *)j->alloc->config);
	free(j->table);
	free(j);
	cpu->jit = NULL;
	cpu->jit_enabled = false;
}

size_t h8500_jit_code_size(const h8500_t *cpu)
{
	return cpu->jit ? cpu->jit->code_size : 0;
}

size_t h8500_jit_block_count(const h8500_t *cpu)
{
	return cpu->jit ? cpu->jit->count : 0;
}

void h8500_jit_stats(const h8500_t *cpu, uint64_t out[4])
{
	out[0] = cpu->jit ? cpu->jit->stat_blocks_run : 0;
	out[1] = cpu->jit ? cpu->jit->stat_fallbacks : 0;
	out[2] = cpu->jit ? cpu->jit->stat_interrupts : 0;
	out[3] = cpu->jit ? cpu->jit->stat_flushes : 0;
}

#else

int h8500_jit_run(h8500_t *cpu, int cycles)
{
	int ran = 0;
	while (ran < cycles)
		ran += h8500_step(cpu);
	return ran;
}

bool h8500_jit_attach(h8500_t *cpu, struct jit_alloc *alloc) { (void)cpu; (void)alloc; return false; }
void h8500_jit_detach(h8500_t *cpu) { (void)cpu; }
void h8500_jit_flush(h8500_t *cpu) { (void)cpu; }
size_t h8500_jit_code_size(const h8500_t *cpu) { (void)cpu; return 0; }
size_t h8500_jit_block_count(const h8500_t *cpu) { (void)cpu; return 0; }
void h8500_jit_stats(const h8500_t *cpu, uint64_t out[4]) { (void)cpu; out[0] = out[1] = out[2] = out[3] = 0; }

#endif
