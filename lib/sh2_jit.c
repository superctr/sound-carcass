#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include "sh2_jit.h"
#include "jit.h"

#if SLJIT_64BIT_ARCHITECTURE

#define SR_T 0x00000001u
#define SR_S 0x00000002u
#define SR_Q 0x00000100u
#define SR_M 0x00000200u
#define SR_MASK 0x000003f3u

#define BLOCK_MAX_INSNS 64
#define SLOT_CHUNK 1024
/* what the cache holds before it is thrown away and built again: the firmware
   settles well inside this, a run that has left the firmware does not */
#define CACHE_CODE_MAX (128u << 20)
#define CACHE_BLOCK_MAX 262144u
#define PAGE_SHIFT 16
#define PAGE_SIZE (1u << PAGE_SHIFT)
#define PAGE_COUNT (1u << (32 - PAGE_SHIFT))

typedef sljit_sw (SLJIT_FUNC *block_fn)(sh2_t *cpu, sljit_sw pending, sljit_sw limit);

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

struct sh2_jit
{
	jit_alloc_t *alloc;
	block_t *table;
	uint32_t table_size;
	uint32_t count;
	size_t code_size;
	slot_chunk_t *chunks;
	void *exit_code;
	void *exit_body;
	uint8_t **rd;
	uint8_t **wr;
	uint32_t limit_base;
	uint64_t stat_blocks_run;
	uint64_t stat_fallbacks;
	uint64_t stat_interrupts;
	uint64_t stat_flushes;
};

#define KEY_NONE 0xffffffffu

/* ------------------------------------------------------------------ */
/* the dispatcher                                                     */
/* ------------------------------------------------------------------ */

static bool irq_waiting(const sh2_t *cpu)
{
	return cpu->nmi_pending
		|| (cpu->pending[0] | cpu->pending[1] | cpu->pending[2] | cpu->pending[3] | cpu->pending[4]) != 0
		|| (cpu->isr & 0xf3u) != 0;
}

static void recompute(sh2_t *cpu)
{
	int level;
	uint32_t h = sh2_tick_horizon(cpu);
	uint64_t left = cpu->jit_deadline > cpu->cycles ? cpu->jit_deadline - cpu->cycles : 0;
	cpu->irq_ready = irq_waiting(cpu) && sh2_irq_select(cpu, &level) >= 0;
	if (left < h)
		h = (uint32_t)left;
	cpu->jit->limit_base = h;
	cpu->jit_limit = cpu->irq_ready ? 0 : h;
}

static void tick_now(sh2_t *cpu)
{
	uint32_t p = cpu->jit_pending;
	if (p)
	{
		cpu->jit_pending = 0;
		cpu->cycles += p;
		sh2_peripherals_tick(cpu, p);
	}
}

static void flush(sh2_t *cpu)
{
	tick_now(cpu);
	recompute(cpu);
}

static void quick_irq(sh2_t *cpu)
{
	int level;
	cpu->irq_ready = irq_waiting(cpu) && sh2_irq_select(cpu, &level) >= 0;
	cpu->jit_limit = cpu->irq_ready ? 0 : cpu->jit->limit_base;
}

/* one interpreted instruction, a delayed branch together with its slot */
static void fallback(sh2_t *cpu)
{
	int cyc;
	flush(cpu);
	cyc = sh2_exec_one(cpu);
	if (cpu->delay)
		cyc += sh2_exec_one(cpu);
	if (cyc < 1)
		cyc = 1;
	cpu->jit_pending = (uint32_t)cyc;
	recompute(cpu);
	cpu->jit->stat_fallbacks++;
}

static block_t *lookup(struct sh2_jit *j, uint32_t key)
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

static bool grow(struct sh2_jit *j)
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

static block_t *translate(sh2_t *cpu, uint32_t key);

int sh2_jit_run(sh2_t *cpu, int cycles)
{
	struct sh2_jit *j = cpu->jit;
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

		if (cpu->delay)
		{
			fallback(cpu);
			continue;
		}

		if (cpu->irq_ready)
		{
			int level;
			int vector = sh2_irq_select(cpu, &level);
			flush(cpu);
			sh2_take_interrupt(cpu, vector, level);
			cpu->jit_pending = 8;
			recompute(cpu);
			j->stat_interrupts++;
			continue;
		}

		if (cpu->sleeping)
		{
			uint32_t n = cpu->jit_limit > cpu->jit_pending ? cpu->jit_limit - cpu->jit_pending : 1;
			cpu->jit_pending += n;
			continue;
		}

		{
			block_t *b;
			if (j->count >= CACHE_BLOCK_MAX || j->code_size >= CACHE_CODE_MAX)
				sh2_jit_flush(cpu);
			b = lookup(j, cpu->pc);
			if (b->key == KEY_NONE)
				b = translate(cpu, cpu->pc);
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

static sljit_sw SLJIT_FUNC hread8(sh2_t *cpu, sljit_sw addr)
{
	uint8_t v;
	tick_now(cpu);
	v = sh2_mem_read8(cpu, (uint32_t)addr);
	recompute(cpu);
	return v;
}

static sljit_sw SLJIT_FUNC hread16(sh2_t *cpu, sljit_sw addr)
{
	uint16_t v;
	tick_now(cpu);
	v = sh2_mem_read16(cpu, (uint32_t)addr);
	recompute(cpu);
	return v;
}

static sljit_sw SLJIT_FUNC hread32(sh2_t *cpu, sljit_sw addr)
{
	uint32_t v;
	tick_now(cpu);
	v = sh2_mem_read32(cpu, (uint32_t)addr);
	recompute(cpu);
	return v;
}

static void SLJIT_FUNC hwrite8(sh2_t *cpu, sljit_sw addr, sljit_sw data)
{
	tick_now(cpu);
	sh2_mem_write8(cpu, (uint32_t)addr, (uint8_t)data);
	recompute(cpu);
}

static void SLJIT_FUNC hwrite16(sh2_t *cpu, sljit_sw addr, sljit_sw data)
{
	tick_now(cpu);
	sh2_mem_write16(cpu, (uint32_t)addr, (uint16_t)data);
	recompute(cpu);
}

static void SLJIT_FUNC hwrite32(sh2_t *cpu, sljit_sw addr, sljit_sw data)
{
	tick_now(cpu);
	sh2_mem_write32(cpu, (uint32_t)addr, (uint32_t)data);
	recompute(cpu);
}

static void SLJIT_FUNC hfallback(sh2_t *cpu)
{
	fallback(cpu);
}

static void SLJIT_FUNC hmacl(sh2_t *cpu, sljit_sw a, sljit_sw b)
{
	sh2_mac_l(cpu, (uint32_t)a, (uint32_t)b);
}

static void SLJIT_FUNC hmacw(sh2_t *cpu, sljit_sw a, sljit_sw b)
{
	sh2_mac_w(cpu, (uint16_t)a, (uint16_t)b);
}

static void SLJIT_FUNC hdiv1(sh2_t *cpu, sljit_sw m, sljit_sw n)
{
	sh2_div1(cpu, (int)m, (int)n);
}

/* ------------------------------------------------------------------ */
/* the regions the translator reads directly                          */
/* ------------------------------------------------------------------ */

static const sh2_region_t *region_of(const sh2_t *cpu, uint32_t addr, uint32_t len)
{
	int i;
	if (addr >= SH2_IO_BASE)
		return NULL;
	for (i = 0; i < cpu->region_count; i++)
	{
		const sh2_region_t *r = &cpu->regions[i];
		uint32_t off = addr - r->base;
		if (off < r->size && (uint64_t)off + len <= r->size)
			return r;
	}
	return NULL;
}

static bool fetch_code(const sh2_t *cpu, uint32_t addr, uint16_t *out)
{
	const sh2_region_t *r;
	const uint8_t *p;
	addr &= ~1u;
	r = region_of(cpu, addr, 2);
	if (!r || r->writable)
		return false;
	p = r->data + (addr - r->base);
	*out = (uint16_t)((p[0] << 8) | p[1]);
	return true;
}

static bool fetch_const(const sh2_t *cpu, uint32_t addr, int width, uint32_t *out)
{
	const sh2_region_t *r;
	const uint8_t *p;
	addr &= ~(uint32_t)(width - 1);
	r = region_of(cpu, addr, (uint32_t)width);
	if (!r || r->writable)
		return false;
	p = r->data + (addr - r->base);
	if (width == 2)
		*out = (uint32_t)((p[0] << 8) | p[1]);
	else
		*out = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
	return true;
}

/* ------------------------------------------------------------------ */
/* decoding                                                           */
/* ------------------------------------------------------------------ */

enum
{
	CLS_NORMAL = 0,
	CLS_BRANCH,
	CLS_COND,
	CLS_SR,
	CLS_NONE
};

static bool op_valid(uint16_t op)
{
	int low = op & 15, hi = (op >> 4) & 3;
	switch (op >> 12)
	{
	case 0x0:
		if (low < 2)
			return false;
		if (low == 2)
			return hi < 3;
		if (low == 3)
			return hi == 0 || hi == 2;
		if (low >= 8 && low <= 11)
			return hi < 3;
		return true;
	case 0x2:
		return low != 3;
	case 0x3:
		return low != 1 && low != 9;
	case 0x4:
		if (hi == 3)
			return low == 15;
		if (low == 12 || low == 13)
			return false;
		return !(hi == 1 && low == 4);
	case 0x8:
		switch ((op >> 8) & 15)
		{
		case 2: case 3: case 6: case 7: case 10: case 12: case 14: return false;
		default: return true;
		}
	default:
		return true;
	}
}

static int op_class(uint16_t op)
{
	if (!op_valid(op))
		return CLS_NONE;
	switch (op >> 12)
	{
	case 0x0:
		switch (op & 0x3f)
		{
		case 0x03: case 0x23: case 0x0b: return CLS_BRANCH;
		case 0x1b: case 0x2b: return CLS_NONE;
		default: return CLS_NORMAL;
		}
	case 0x4:
		switch (op & 0x3f)
		{
		case 0x0b: case 0x2b: return CLS_BRANCH;
		case 0x07: case 0x0e: return CLS_SR;
		default: return CLS_NORMAL;
		}
	case 0x8:
		switch ((op >> 8) & 15)
		{
		case 9: case 11: case 13: case 15: return CLS_COND;
		default: return CLS_NORMAL;
		}
	case 0xa: case 0xb:
		return CLS_BRANCH;
	case 0xc:
		return ((op >> 8) & 15) == 3 ? CLS_NONE : CLS_NORMAL;
	default:
		return CLS_NORMAL;
	}
}

/* the value cpu->pc carries while the instruction runs */
static bool op_uses_pc(uint16_t op)
{
	switch (op >> 12)
	{
	case 0x0:
		return (op & 0x3f) == 0x03 || (op & 0x3f) == 0x23;
	case 0x4:
		return (op & 0x3f) == 0x0b;
	case 0x8:
		switch ((op >> 8) & 15)
		{
		case 9: case 11: case 13: case 15: return true;
		default: return false;
		}
	case 0x9: case 0xa: case 0xb: case 0xd:
		return true;
	case 0xc:
		return ((op >> 8) & 15) == 3 || ((op >> 8) & 15) == 7;
	default:
		return false;
	}
}

static int cycles_0000(uint16_t op)
{
	switch (op & 0x3f)
	{
	case 0x07: case 0x17: case 0x27: case 0x37: return 2;
	case 0x0f: case 0x1f: case 0x2f: case 0x3f: return 3;
	default: return 1;
	}
}

static int cycles_0100(uint16_t op)
{
	switch (op & 0x3f)
	{
	case 0x03: case 0x13: case 0x23: return 2;
	case 0x07: case 0x17: case 0x27: return 3;
	case 0x1b: return 4;
	case 0x0f: case 0x1f: case 0x2f: case 0x3f: return 3;
	default: return 1;
	}
}

static int op_cycles(uint16_t op)
{
	switch (op >> 12)
	{
	case 0x0: return cycles_0000(op);
	case 0x3: return (op & 15) == 5 || (op & 15) == 13 ? 2 : 1;
	case 0x4: return cycles_0100(op);
	case 0xc:
		switch ((op >> 8) & 15)
		{
		case 12: case 13: case 14: case 15: return 3;
		default: return 1;
		}
	default: return 1;
	}
}

/* ------------------------------------------------------------------ */
/* emission                                                           */
/* ------------------------------------------------------------------ */

#define CPU SLJIT_S0
#define PENDING SLJIT_S1
#define LIMIT SLJIT_S2
#define ADDR SLJIT_S3
#define VAL SLJIT_S4
#define VAL2 SLJIT_S5

#define CELL(field) SLJIT_MEM1(CPU), (sljit_sw)offsetof(sh2_t, field)
#define RN(n) SLJIT_MEM1(CPU), (sljit_sw)(offsetof(sh2_t, r) + 4 * (n))

typedef struct exit_site
{
	struct sljit_jump *jump;
	uint32_t pc;
} exit_site_t;

typedef struct emitter
{
	struct sljit_compiler *c;
	struct sh2_jit *j;
	sh2_t *cpu;
	exit_site_t exits[BLOCK_MAX_INSNS * 4 + 8];
	int exit_count;
	struct sljit_jump *rets[BLOCK_MAX_INSNS * 4 + 8];
	int ret_count;
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

static void bind(emitter_t *e, struct sljit_jump *j)
{
	sljit_set_label(j, sljit_emit_label(e->c));
}

static void exit_to(emitter_t *e, struct sljit_jump *j, uint32_t pc)
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

/* every way out of a block meets at one epilogue */
static void emit_return(emitter_t *e)
{
	if (e->ret_count >= (int)(sizeof(e->rets) / sizeof(e->rets[0])))
	{
		e->failed = true;
		return;
	}
	e->rets[e->ret_count++] = sljit_emit_jump(e->c, SLJIT_JUMP);
}

static void emit_epilogue(emitter_t *e)
{
	struct sljit_label *l = sljit_emit_label(e->c);
	int n;
	for (n = 0; n < e->ret_count; n++)
		sljit_set_label(e->rets[n], l);
	sljit_emit_return(e->c, SLJIT_MOV, PENDING, 0);
}

static void emit_exit_now(emitter_t *e, uint32_t pc)
{
	op1(e, SLJIT_MOV_U32, CELL(pc), SLJIT_IMM, (sljit_sw)pc);
	emit_return(e);
}

/* the boundary between two instructions */
static void boundary(emitter_t *e, int cyc, uint32_t next)
{
	op2(e, SLJIT_ADD, PENDING, 0, PENDING, 0, SLJIT_IMM, cyc);
	exit_to(e, sljit_emit_cmp(e->c, SLJIT_SIG_GREATER_EQUAL, PENDING, 0, LIMIT, 0), next);
}

static slot_t *new_slot(struct sh2_jit *j, uint32_t key)
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
static void chain(emitter_t *e, uint32_t pc)
{
	slot_t *s = new_slot(e->j, pc);
	if (!s)
	{
		e->failed = true;
		return;
	}
	op1(e, SLJIT_MOV_U32, CELL(pc), SLJIT_IMM, (sljit_sw)pc);
	movi(e, SLJIT_R0, (sljit_sw)&s->target);
	sljit_emit_ijump(e->c, SLJIT_JUMP, SLJIT_MEM1(SLJIT_R0), 0);
}

static void direct_exit(emitter_t *e, uint32_t pc, int cyc)
{
	op2(e, SLJIT_ADD, PENDING, 0, PENDING, 0, SLJIT_IMM, cyc);
	exit_to(e, sljit_emit_cmp(e->c, SLJIT_SIG_GREATER_EQUAL, PENDING, 0, LIMIT, 0), pc);
	chain(e, pc);
}

/* a branch whose target is only known while the block runs, and the
 * instructions that change the interrupt mask, leave through the dispatcher */
static void hard_exit(emitter_t *e, uint32_t pc, int cyc)
{
	op2(e, SLJIT_ADD, PENDING, 0, PENDING, 0, SLJIT_IMM, cyc);
	emit_exit_now(e, pc);
}

static void dyn_exit(emitter_t *e, int cyc)
{
	op1(e, SLJIT_MOV_U32, SLJIT_R0, 0, CELL(delay_target));
	op1(e, SLJIT_MOV_U32, CELL(pc), SLJIT_R0, 0);
	op2(e, SLJIT_ADD, PENDING, 0, PENDING, 0, SLJIT_IMM, cyc);
	emit_return(e);
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

static void emit_fallback(emitter_t *e, uint32_t addr)
{
	op1(e, SLJIT_MOV_U32, CELL(pc), SLJIT_IMM, (sljit_sw)addr);
	call1(e, (void *)hfallback);
	emit_return(e);
}

/* ---- registers and the T bit ---- */

static void ld_u(emitter_t *e, sljit_s32 d, int n)
{
	op1(e, SLJIT_MOV_U32, d, 0, RN(n));
}

static void ld_s(emitter_t *e, sljit_s32 d, int n)
{
	op1(e, SLJIT_MOV_S32, d, 0, RN(n));
}

static void st_r(emitter_t *e, int n, sljit_s32 s)
{
	op1(e, SLJIT_MOV_U32, RN(n), s, 0);
}

/* sr = (sr & ~T) | (src & T); clobbers R1 and R2 */
static void set_t(emitter_t *e, sljit_s32 src)
{
	op1(e, SLJIT_MOV_U32, SLJIT_R1, 0, CELL(sr));
	op2(e, SLJIT_AND, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_IMM, ~(sljit_sw)1);
	op2(e, SLJIT_AND, SLJIT_R2, 0, src, 0, SLJIT_IMM, 1);
	op2(e, SLJIT_OR, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_R2, 0);
	op1(e, SLJIT_MOV_U32, CELL(sr), SLJIT_R1, 0);
}

static void set_t_flag(emitter_t *e, sljit_s32 type)
{
	sljit_emit_op_flags(e->c, SLJIT_MOV, SLJIT_R0, 0, type);
	set_t(e, SLJIT_R0);
}

static void get_t(emitter_t *e, sljit_s32 d)
{
	op1(e, SLJIT_MOV_U32, d, 0, CELL(sr));
	op2(e, SLJIT_AND, d, 0, d, 0, SLJIT_IMM, 1);
}

/* ---- memory ---- */

static void mem_slow(emitter_t *e, void *fn, sljit_s32 dst, sljit_s32 src, bool store)
{
	if (store && src != SLJIT_R2)
		movr(e, SLJIT_R2, src);
	call_pre(e);
	movr(e, SLJIT_R1, ADDR);
	if (store)
		sljit_emit_icall(e->c, SLJIT_CALL, SLJIT_ARGS3V(P, W, W), SLJIT_IMM, (sljit_sw)(uintptr_t)fn);
	else
		sljit_emit_icall(e->c, SLJIT_CALL, SLJIT_ARGS2(W, P, W), SLJIT_IMM, (sljit_sw)(uintptr_t)fn);
	call_post(e);
	if (!store && dst != SLJIT_R0)
		movr(e, dst, SLJIT_R0);
}

/* the value at ADDR into dst, zero extended; clobbers R0-R2 */
static void mem_read(emitter_t *e, int width, sljit_s32 dst)
{
	struct sljit_jump *onchip, *slow1, *slow2, *done1, *done2;
	void *fn = width == 1 ? (void *)hread8 : width == 2 ? (void *)hread16 : (void *)hread32;
	sljit_s32 load = width == 1 ? SLJIT_MOV_U8 : width == 2 ? SLJIT_MOV_U16 : SLJIT_MOV_U32;
	sljit_s32 rev = width == 2 ? SLJIT_REV_U16 : SLJIT_REV_U32;

	if (width > 1)
		op2(e, SLJIT_AND, ADDR, 0, ADDR, 0, SLJIT_IMM, ~(sljit_sw)(width - 1));
	onchip = sljit_emit_cmp(e->c, SLJIT_GREATER_EQUAL, ADDR, 0, SLJIT_IMM, SH2_IO_BASE);

	op2(e, SLJIT_LSHR, SLJIT_R1, 0, ADDR, 0, SLJIT_IMM, PAGE_SHIFT);
	movi(e, SLJIT_R2, (sljit_sw)(uintptr_t)e->j->rd);
	op1(e, SLJIT_MOV_P, SLJIT_R1, 0, SLJIT_MEM2(SLJIT_R2, SLJIT_R1), 3);
	slow1 = sljit_emit_cmp(e->c, SLJIT_EQUAL, SLJIT_R1, 0, SLJIT_IMM, 0);
	op1(e, load, dst, 0, SLJIT_MEM2(SLJIT_R1, ADDR), 0);
	if (width > 1)
		op1(e, rev, dst, 0, dst, 0);
	done1 = sljit_emit_jump(e->c, SLJIT_JUMP);

	bind(e, onchip);
	slow2 = sljit_emit_cmp(e->c, SLJIT_LESS, ADDR, 0, SLJIT_IMM, SH2_RAM_BASE);
	op2(e, SLJIT_AND, SLJIT_R1, 0, ADDR, 0, SLJIT_IMM, SH2_RAM_SIZE - 1);
	op2(e, SLJIT_ADD, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_IMM, (sljit_sw)offsetof(sh2_t, ram));
	op1(e, load, dst, 0, SLJIT_MEM2(CPU, SLJIT_R1), 0);
	if (width > 1)
		op1(e, rev, dst, 0, dst, 0);
	done2 = sljit_emit_jump(e->c, SLJIT_JUMP);

	bind(e, slow1);
	bind(e, slow2);
	mem_slow(e, fn, dst, 0, false);
	bind(e, done1);
	bind(e, done2);
}

/* src to ADDR; src must not be R1, R2 or R3 */
static void mem_write(emitter_t *e, int width, sljit_s32 src)
{
	struct sljit_jump *onchip, *slow1, *slow2, *done1, *done2;
	void *fn = width == 1 ? (void *)hwrite8 : width == 2 ? (void *)hwrite16 : (void *)hwrite32;
	sljit_s32 store = width == 1 ? SLJIT_MOV_U8 : width == 2 ? SLJIT_MOV_U16 : SLJIT_MOV_U32;
	sljit_s32 rev = width == 2 ? SLJIT_REV_U16 : SLJIT_REV_U32;

	if (width > 1)
		op2(e, SLJIT_AND, ADDR, 0, ADDR, 0, SLJIT_IMM, ~(sljit_sw)(width - 1));
	onchip = sljit_emit_cmp(e->c, SLJIT_GREATER_EQUAL, ADDR, 0, SLJIT_IMM, SH2_IO_BASE);

	op2(e, SLJIT_LSHR, SLJIT_R1, 0, ADDR, 0, SLJIT_IMM, PAGE_SHIFT);
	movi(e, SLJIT_R2, (sljit_sw)(uintptr_t)e->j->wr);
	op1(e, SLJIT_MOV_P, SLJIT_R1, 0, SLJIT_MEM2(SLJIT_R2, SLJIT_R1), 3);
	slow1 = sljit_emit_cmp(e->c, SLJIT_EQUAL, SLJIT_R1, 0, SLJIT_IMM, 0);
	if (width > 1)
	{
		op1(e, rev, SLJIT_R3, 0, src, 0);
		op1(e, store, SLJIT_MEM2(SLJIT_R1, ADDR), 0, SLJIT_R3, 0);
	}
	else
		op1(e, store, SLJIT_MEM2(SLJIT_R1, ADDR), 0, src, 0);
	done1 = sljit_emit_jump(e->c, SLJIT_JUMP);

	bind(e, onchip);
	slow2 = sljit_emit_cmp(e->c, SLJIT_LESS, ADDR, 0, SLJIT_IMM, SH2_RAM_BASE);
	op2(e, SLJIT_AND, SLJIT_R1, 0, ADDR, 0, SLJIT_IMM, SH2_RAM_SIZE - 1);
	op2(e, SLJIT_ADD, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_IMM, (sljit_sw)offsetof(sh2_t, ram));
	if (width > 1)
	{
		op1(e, rev, SLJIT_R3, 0, src, 0);
		op1(e, store, SLJIT_MEM2(CPU, SLJIT_R1), 0, SLJIT_R3, 0);
	}
	else
		op1(e, store, SLJIT_MEM2(CPU, SLJIT_R1), 0, src, 0);
	done2 = sljit_emit_jump(e->c, SLJIT_JUMP);

	bind(e, slow1);
	bind(e, slow2);
	mem_slow(e, fn, 0, src, true);
	bind(e, done1);
	bind(e, done2);
}

static void ea_reg(emitter_t *e, int n)
{
	op1(e, SLJIT_MOV_U32, ADDR, 0, RN(n));
}

static void ea_disp(emitter_t *e, int n, sljit_sw disp)
{
	op1(e, SLJIT_MOV_U32, ADDR, 0, RN(n));
	if (disp)
	{
		op2(e, SLJIT_ADD, ADDR, 0, ADDR, 0, SLJIT_IMM, disp);
		op1(e, SLJIT_MOV_U32, ADDR, 0, ADDR, 0);
	}
}

static void ea_index(emitter_t *e, int n)
{
	op1(e, SLJIT_MOV_U32, ADDR, 0, RN(n));
	op1(e, SLJIT_MOV_U32, SLJIT_R0, 0, RN(0));
	op2(e, SLJIT_ADD, ADDR, 0, ADDR, 0, SLJIT_R0, 0);
	op1(e, SLJIT_MOV_U32, ADDR, 0, ADDR, 0);
}

static void ea_gbr(emitter_t *e, sljit_sw disp)
{
	op1(e, SLJIT_MOV_U32, ADDR, 0, CELL(gbr));
	if (disp)
	{
		op2(e, SLJIT_ADD, ADDR, 0, ADDR, 0, SLJIT_IMM, disp);
		op1(e, SLJIT_MOV_U32, ADDR, 0, ADDR, 0);
	}
}

static void ea_gbr_index(emitter_t *e)
{
	op1(e, SLJIT_MOV_U32, ADDR, 0, CELL(gbr));
	op1(e, SLJIT_MOV_U32, SLJIT_R0, 0, RN(0));
	op2(e, SLJIT_ADD, ADDR, 0, ADDR, 0, SLJIT_R0, 0);
	op1(e, SLJIT_MOV_U32, ADDR, 0, ADDR, 0);
}

/* r[n] += delta, truncated, leaving the new value in R0 */
static void bump(emitter_t *e, int n, sljit_sw delta)
{
	op1(e, SLJIT_MOV_U32, SLJIT_R0, 0, RN(n));
	op2(e, SLJIT_ADD, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, delta);
	op1(e, SLJIT_MOV_U32, SLJIT_R0, 0, SLJIT_R0, 0);
	st_r(e, n, SLJIT_R0);
}

static void sign_extend(emitter_t *e, sljit_s32 r, int width)
{
	op1(e, width == 1 ? SLJIT_MOV_S8 : SLJIT_MOV_S16, r, 0, r, 0);
}

/* ---- the instructions ---- */

static void emit_load(emitter_t *e, int width, int n, bool sign)
{
	mem_read(e, width, VAL);
	if (sign && width < 4)
		sign_extend(e, VAL, width);
	st_r(e, n, VAL);
}

static void emit_cmp_t(emitter_t *e, int n, int m, sljit_s32 type, sljit_s32 set, bool sign)
{
	if (sign)
	{
		ld_s(e, SLJIT_R3, n);
		ld_s(e, SLJIT_R4, m);
	}
	else
	{
		ld_u(e, SLJIT_R3, n);
		ld_u(e, SLJIT_R4, m);
	}
	sljit_emit_op2u(e->c, SLJIT_SUB | set, SLJIT_R3, 0, SLJIT_R4, 0);
	set_t_flag(e, type);
}

static void emit_shift_t(emitter_t *e, int n, bool left, bool arith)
{
	if (arith && !left)
		ld_s(e, SLJIT_R0, n);
	else
		ld_u(e, SLJIT_R0, n);
	if (left)
		op2(e, SLJIT_LSHR, SLJIT_R3, 0, SLJIT_R0, 0, SLJIT_IMM, 31);
	else
		op2(e, SLJIT_AND, SLJIT_R3, 0, SLJIT_R0, 0, SLJIT_IMM, 1);
	op2(e, left ? SLJIT_SHL : (arith ? SLJIT_ASHR : SLJIT_LSHR), SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, 1);
	st_r(e, n, SLJIT_R0);
	set_t(e, SLJIT_R3);
}

static void emit_addc(emitter_t *e, int n, int m, bool sub)
{
	ld_u(e, SLJIT_R3, n);
	ld_u(e, SLJIT_R4, m);
	get_t(e, SLJIT_R5);
	if (sub)
	{
		op2(e, SLJIT_SUB, SLJIT_R3, 0, SLJIT_R3, 0, SLJIT_R4, 0);
		op2(e, SLJIT_SUB, SLJIT_R3, 0, SLJIT_R3, 0, SLJIT_R5, 0);
	}
	else
	{
		op2(e, SLJIT_ADD, SLJIT_R3, 0, SLJIT_R3, 0, SLJIT_R4, 0);
		op2(e, SLJIT_ADD, SLJIT_R3, 0, SLJIT_R3, 0, SLJIT_R5, 0);
	}
	st_r(e, n, SLJIT_R3);
	op2(e, SLJIT_LSHR, SLJIT_R3, 0, SLJIT_R3, 0, SLJIT_IMM, 32);
	set_t(e, SLJIT_R3);
}

static void emit_addv(emitter_t *e, int n, int m, bool sub)
{
	ld_u(e, SLJIT_R3, n);
	ld_u(e, SLJIT_R4, m);
	op2(e, sub ? SLJIT_SUB : SLJIT_ADD, SLJIT_R5, 0, SLJIT_R3, 0, SLJIT_R4, 0);
	st_r(e, n, SLJIT_R5);
	op2(e, SLJIT_XOR, SLJIT_R4, 0, SLJIT_R3, 0, SLJIT_R4, 0);
	if (!sub)
		op2(e, SLJIT_XOR, SLJIT_R4, 0, SLJIT_R4, 0, SLJIT_IMM, -1);
	op2(e, SLJIT_XOR, SLJIT_R5, 0, SLJIT_R3, 0, SLJIT_R5, 0);
	op2(e, SLJIT_AND, SLJIT_R5, 0, SLJIT_R5, 0, SLJIT_R4, 0);
	op2(e, SLJIT_LSHR, SLJIT_R5, 0, SLJIT_R5, 0, SLJIT_IMM, 31);
	set_t(e, SLJIT_R5);
}

static void emit_mac(emitter_t *e, int width, int n, int m)
{
	ea_reg(e, n);
	mem_read(e, width, VAL);
	bump(e, n, width);
	ea_reg(e, m);
	mem_read(e, width, VAL2);
	bump(e, m, width);
	call_pre(e);
	movr(e, SLJIT_R1, VAL);
	movr(e, SLJIT_R2, VAL2);
	sljit_emit_icall(e->c, SLJIT_CALL, SLJIT_ARGS3V(P, W, W), SLJIT_IMM,
		(sljit_sw)(uintptr_t)(width == 4 ? (void *)hmacl : (void *)hmacw));
	call_post(e);
}

static bool emit_0000(emitter_t *e, uint16_t op)
{
	int n = (op >> 8) & 15;
	int m = (op >> 4) & 15;

	switch (op & 0x3f)
	{
	case 0x02: op1(e, SLJIT_MOV_U32, SLJIT_R0, 0, CELL(sr)); st_r(e, n, SLJIT_R0); return true;
	case 0x12: op1(e, SLJIT_MOV_U32, SLJIT_R0, 0, CELL(gbr)); st_r(e, n, SLJIT_R0); return true;
	case 0x22: op1(e, SLJIT_MOV_U32, SLJIT_R0, 0, CELL(vbr)); st_r(e, n, SLJIT_R0); return true;

	case 0x04: case 0x14: case 0x24: case 0x34:
		ld_u(e, VAL, m); ea_index(e, n); mem_write(e, 1, VAL); return true;
	case 0x05: case 0x15: case 0x25: case 0x35:
		ld_u(e, VAL, m); ea_index(e, n); mem_write(e, 2, VAL); return true;
	case 0x06: case 0x16: case 0x26: case 0x36:
		ld_u(e, VAL, m); ea_index(e, n); mem_write(e, 4, VAL); return true;

	case 0x07: case 0x17: case 0x27: case 0x37:
		ld_u(e, SLJIT_R0, n);
		ld_u(e, SLJIT_R1, m);
		op2(e, SLJIT_MUL, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0);
		op1(e, SLJIT_MOV_U32, CELL(macl), SLJIT_R0, 0);
		return true;

	case 0x08:
		op1(e, SLJIT_MOV_U32, SLJIT_R0, 0, CELL(sr));
		op2(e, SLJIT_AND, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, ~(sljit_sw)SR_T);
		op1(e, SLJIT_MOV_U32, CELL(sr), SLJIT_R0, 0);
		return true;
	case 0x18:
		op1(e, SLJIT_MOV_U32, SLJIT_R0, 0, CELL(sr));
		op2(e, SLJIT_OR, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, SR_T);
		op1(e, SLJIT_MOV_U32, CELL(sr), SLJIT_R0, 0);
		return true;
	case 0x28:
		op1(e, SLJIT_MOV_U32, CELL(mach), SLJIT_IMM, 0);
		op1(e, SLJIT_MOV_U32, CELL(macl), SLJIT_IMM, 0);
		return true;

	case 0x09: return true;
	case 0x19:
		op1(e, SLJIT_MOV_U32, SLJIT_R0, 0, CELL(sr));
		op2(e, SLJIT_AND, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, ~(sljit_sw)(SR_M | SR_Q | SR_T));
		op1(e, SLJIT_MOV_U32, CELL(sr), SLJIT_R0, 0);
		return true;
	case 0x29:
		get_t(e, SLJIT_R0);
		st_r(e, n, SLJIT_R0);
		return true;

	case 0x0a: op1(e, SLJIT_MOV_U32, SLJIT_R0, 0, CELL(mach)); st_r(e, n, SLJIT_R0); return true;
	case 0x1a: op1(e, SLJIT_MOV_U32, SLJIT_R0, 0, CELL(macl)); st_r(e, n, SLJIT_R0); return true;
	case 0x2a: op1(e, SLJIT_MOV_U32, SLJIT_R0, 0, CELL(pr)); st_r(e, n, SLJIT_R0); return true;

	case 0x0c: case 0x1c: case 0x2c: case 0x3c:
		ea_index(e, m); emit_load(e, 1, n, true); return true;
	case 0x0d: case 0x1d: case 0x2d: case 0x3d:
		ea_index(e, m); emit_load(e, 2, n, true); return true;
	case 0x0e: case 0x1e: case 0x2e: case 0x3e:
		ea_index(e, m); emit_load(e, 4, n, false); return true;

	case 0x0f: case 0x1f: case 0x2f: case 0x3f:
		emit_mac(e, 4, n, m);
		return true;
	default:
		return false;
	}
}

static bool emit_0100(emitter_t *e, uint16_t op)
{
	int n = (op >> 8) & 15;
	int m = (op >> 4) & 15;

	switch (op & 0x3f)
	{
	case 0x00: case 0x20: emit_shift_t(e, n, true, false); return true;
	case 0x01: emit_shift_t(e, n, false, false); return true;
	case 0x21: emit_shift_t(e, n, false, true); return true;
	case 0x04:
		ld_u(e, SLJIT_R0, n);
		op2(e, SLJIT_LSHR, SLJIT_R3, 0, SLJIT_R0, 0, SLJIT_IMM, 31);
		op2(e, SLJIT_SHL, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, 1);
		op2(e, SLJIT_OR, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R3, 0);
		st_r(e, n, SLJIT_R0);
		set_t(e, SLJIT_R3);
		return true;
	case 0x05:
		ld_u(e, SLJIT_R0, n);
		op2(e, SLJIT_AND, SLJIT_R3, 0, SLJIT_R0, 0, SLJIT_IMM, 1);
		op2(e, SLJIT_LSHR, SLJIT_R4, 0, SLJIT_R0, 0, SLJIT_IMM, 1);
		op2(e, SLJIT_SHL, SLJIT_R5, 0, SLJIT_R3, 0, SLJIT_IMM, 31);
		op2(e, SLJIT_OR, SLJIT_R4, 0, SLJIT_R4, 0, SLJIT_R5, 0);
		st_r(e, n, SLJIT_R4);
		set_t(e, SLJIT_R3);
		return true;
	case 0x24:
		ld_u(e, SLJIT_R0, n);
		get_t(e, SLJIT_R4);
		op2(e, SLJIT_LSHR, SLJIT_R3, 0, SLJIT_R0, 0, SLJIT_IMM, 31);
		op2(e, SLJIT_SHL, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, 1);
		op2(e, SLJIT_OR, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R4, 0);
		st_r(e, n, SLJIT_R0);
		set_t(e, SLJIT_R3);
		return true;
	case 0x25:
		ld_u(e, SLJIT_R0, n);
		get_t(e, SLJIT_R4);
		op2(e, SLJIT_AND, SLJIT_R3, 0, SLJIT_R0, 0, SLJIT_IMM, 1);
		op2(e, SLJIT_LSHR, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, 1);
		op2(e, SLJIT_SHL, SLJIT_R4, 0, SLJIT_R4, 0, SLJIT_IMM, 31);
		op2(e, SLJIT_OR, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R4, 0);
		st_r(e, n, SLJIT_R0);
		set_t(e, SLJIT_R3);
		return true;

	case 0x08: case 0x18: case 0x28:
	{
		int sh = (op & 0x3f) == 0x08 ? 2 : (op & 0x3f) == 0x18 ? 8 : 16;
		ld_u(e, SLJIT_R0, n);
		op2(e, SLJIT_SHL, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, sh);
		st_r(e, n, SLJIT_R0);
		return true;
	}
	case 0x09: case 0x19: case 0x29:
	{
		int sh = (op & 0x3f) == 0x09 ? 2 : (op & 0x3f) == 0x19 ? 8 : 16;
		ld_u(e, SLJIT_R0, n);
		op2(e, SLJIT_LSHR, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, sh);
		st_r(e, n, SLJIT_R0);
		return true;
	}

	case 0x10:
		ld_u(e, SLJIT_R0, n);
		op2(e, SLJIT_SUB, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, 1);
		op1(e, SLJIT_MOV_U32, SLJIT_R0, 0, SLJIT_R0, 0);
		st_r(e, n, SLJIT_R0);
		sljit_emit_op2u(e->c, SLJIT_SUB | SLJIT_SET_Z, SLJIT_R0, 0, SLJIT_IMM, 0);
		set_t_flag(e, SLJIT_EQUAL);
		return true;
	case 0x11:
		ld_s(e, SLJIT_R3, n);
		sljit_emit_op2u(e->c, SLJIT_SUB | SLJIT_SET_SIG_LESS, SLJIT_R3, 0, SLJIT_IMM, 0);
		set_t_flag(e, SLJIT_SIG_GREATER_EQUAL);
		return true;
	case 0x15:
		ld_s(e, SLJIT_R3, n);
		sljit_emit_op2u(e->c, SLJIT_SUB | SLJIT_SET_SIG_GREATER, SLJIT_R3, 0, SLJIT_IMM, 0);
		set_t_flag(e, SLJIT_SIG_GREATER);
		return true;

	case 0x02: case 0x12: case 0x22: case 0x03: case 0x13: case 0x23:
	{
		size_t off = (op & 0x3f) == 0x02 ? offsetof(sh2_t, mach)
			: (op & 0x3f) == 0x12 ? offsetof(sh2_t, macl)
			: (op & 0x3f) == 0x22 ? offsetof(sh2_t, pr)
			: (op & 0x3f) == 0x03 ? offsetof(sh2_t, sr)
			: (op & 0x3f) == 0x13 ? offsetof(sh2_t, gbr)
			: offsetof(sh2_t, vbr);
		op1(e, SLJIT_MOV_U32, VAL, 0, SLJIT_MEM1(CPU), (sljit_sw)off);
		bump(e, n, -4);
		movr(e, ADDR, SLJIT_R0);
		mem_write(e, 4, VAL);
		return true;
	}
	case 0x06: case 0x16: case 0x26: case 0x07: case 0x17: case 0x27:
	{
		size_t off = (op & 0x3f) == 0x06 ? offsetof(sh2_t, mach)
			: (op & 0x3f) == 0x16 ? offsetof(sh2_t, macl)
			: (op & 0x3f) == 0x26 ? offsetof(sh2_t, pr)
			: (op & 0x3f) == 0x07 ? offsetof(sh2_t, sr)
			: (op & 0x3f) == 0x17 ? offsetof(sh2_t, gbr)
			: offsetof(sh2_t, vbr);
		ea_reg(e, n);
		mem_read(e, 4, VAL);
		if ((op & 0x3f) == 0x07)
			op2(e, SLJIT_AND, VAL, 0, VAL, 0, SLJIT_IMM, SR_MASK);
		op1(e, SLJIT_MOV_U32, SLJIT_MEM1(CPU), (sljit_sw)off, VAL, 0);
		bump(e, n, 4);
		return true;
	}

	case 0x0a: op1(e, SLJIT_MOV_U32, SLJIT_R0, 0, RN(n)); op1(e, SLJIT_MOV_U32, CELL(mach), SLJIT_R0, 0); return true;
	case 0x1a: op1(e, SLJIT_MOV_U32, SLJIT_R0, 0, RN(n)); op1(e, SLJIT_MOV_U32, CELL(macl), SLJIT_R0, 0); return true;
	case 0x2a: op1(e, SLJIT_MOV_U32, SLJIT_R0, 0, RN(n)); op1(e, SLJIT_MOV_U32, CELL(pr), SLJIT_R0, 0); return true;
	case 0x0e:
		ld_u(e, SLJIT_R0, n);
		op2(e, SLJIT_AND, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, SR_MASK);
		op1(e, SLJIT_MOV_U32, CELL(sr), SLJIT_R0, 0);
		return true;
	case 0x1e: op1(e, SLJIT_MOV_U32, SLJIT_R0, 0, RN(n)); op1(e, SLJIT_MOV_U32, CELL(gbr), SLJIT_R0, 0); return true;
	case 0x2e: op1(e, SLJIT_MOV_U32, SLJIT_R0, 0, RN(n)); op1(e, SLJIT_MOV_U32, CELL(vbr), SLJIT_R0, 0); return true;

	case 0x1b:
		ea_reg(e, n);
		mem_read(e, 1, VAL);
		sljit_emit_op2u(e->c, SLJIT_SUB | SLJIT_SET_Z, VAL, 0, SLJIT_IMM, 0);
		set_t_flag(e, SLJIT_EQUAL);
		op2(e, SLJIT_OR, VAL, 0, VAL, 0, SLJIT_IMM, 0x80);
		mem_write(e, 1, VAL);
		return true;

	case 0x0f: case 0x1f: case 0x2f: case 0x3f:
		emit_mac(e, 2, n, m);
		return true;
	default:
		return false;
	}
}

static bool emit_normal(emitter_t *e, uint16_t op, uint32_t pc, bool pc_known)
{
	int n = (op >> 8) & 15;
	int m = (op >> 4) & 15;

	switch (op >> 12)
	{
	case 0x0: return emit_0000(e, op);

	case 0x1:
		ld_u(e, VAL, m);
		ea_disp(e, n, (sljit_sw)((op & 15) * 4));
		mem_write(e, 4, VAL);
		return true;

	case 0x2:
		switch (op & 15)
		{
		case 0: ld_u(e, VAL, m); ea_reg(e, n); mem_write(e, 1, VAL); return true;
		case 1: ld_u(e, VAL, m); ea_reg(e, n); mem_write(e, 2, VAL); return true;
		case 2: ld_u(e, VAL, m); ea_reg(e, n); mem_write(e, 4, VAL); return true;
		case 4: case 5: case 6:
		{
			int w = (op & 15) == 4 ? 1 : (op & 15) == 5 ? 2 : 4;
			ld_u(e, VAL, m);
			bump(e, n, -w);
			movr(e, ADDR, SLJIT_R0);
			mem_write(e, w, VAL);
			return true;
		}
		case 7:
			ld_u(e, SLJIT_R3, n);
			ld_u(e, SLJIT_R4, m);
			op1(e, SLJIT_MOV_U32, SLJIT_R5, 0, CELL(sr));
			op2(e, SLJIT_AND, SLJIT_R5, 0, SLJIT_R5, 0, SLJIT_IMM, ~(sljit_sw)(SR_Q | SR_M | SR_T));
			op2(e, SLJIT_LSHR, SLJIT_R0, 0, SLJIT_R3, 0, SLJIT_IMM, 31);
			op2(e, SLJIT_SHL, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, 8);
			op2(e, SLJIT_OR, SLJIT_R5, 0, SLJIT_R5, 0, SLJIT_R0, 0);
			op2(e, SLJIT_LSHR, SLJIT_R0, 0, SLJIT_R4, 0, SLJIT_IMM, 31);
			op2(e, SLJIT_SHL, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, 9);
			op2(e, SLJIT_OR, SLJIT_R5, 0, SLJIT_R5, 0, SLJIT_R0, 0);
			op2(e, SLJIT_XOR, SLJIT_R0, 0, SLJIT_R3, 0, SLJIT_R4, 0);
			op2(e, SLJIT_LSHR, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, 31);
			op2(e, SLJIT_OR, SLJIT_R5, 0, SLJIT_R5, 0, SLJIT_R0, 0);
			op1(e, SLJIT_MOV_U32, CELL(sr), SLJIT_R5, 0);
			return true;
		case 8:
			ld_u(e, SLJIT_R3, n);
			ld_u(e, SLJIT_R4, m);
			sljit_emit_op2u(e->c, SLJIT_AND | SLJIT_SET_Z, SLJIT_R3, 0, SLJIT_R4, 0);
			set_t_flag(e, SLJIT_EQUAL);
			return true;
		case 9: case 10: case 11:
		{
			sljit_s32 o = (op & 15) == 9 ? SLJIT_AND : (op & 15) == 10 ? SLJIT_XOR : SLJIT_OR;
			ld_u(e, SLJIT_R0, n);
			ld_u(e, SLJIT_R1, m);
			op2(e, o, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0);
			st_r(e, n, SLJIT_R0);
			return true;
		}
		case 12:
		{
			static const sljit_sw mask[4] = { 0xff000000, 0x00ff0000, 0x0000ff00, 0x000000ff };
			int i;
			ld_u(e, SLJIT_R3, n);
			ld_u(e, SLJIT_R5, m);
			op2(e, SLJIT_XOR, SLJIT_R3, 0, SLJIT_R3, 0, SLJIT_R5, 0);
			movi(e, SLJIT_R4, 0);
			for (i = 0; i < 4; i++)
			{
				sljit_emit_op2u(e->c, SLJIT_AND | SLJIT_SET_Z, SLJIT_R3, 0, SLJIT_IMM, mask[i]);
				sljit_emit_op_flags(e->c, SLJIT_OR, SLJIT_R4, 0, SLJIT_EQUAL);
			}
			set_t(e, SLJIT_R4);
			return true;
		}
		case 13:
			ld_u(e, SLJIT_R0, m);
			op2(e, SLJIT_SHL, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, 16);
			ld_u(e, SLJIT_R1, n);
			op2(e, SLJIT_LSHR, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_IMM, 16);
			op2(e, SLJIT_OR, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0);
			st_r(e, n, SLJIT_R0);
			return true;
		case 14:
			ld_u(e, SLJIT_R0, n);
			op2(e, SLJIT_AND, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, 0xffff);
			ld_u(e, SLJIT_R1, m);
			op2(e, SLJIT_AND, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_IMM, 0xffff);
			op2(e, SLJIT_MUL, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0);
			op1(e, SLJIT_MOV_U32, CELL(macl), SLJIT_R0, 0);
			return true;
		case 15:
			ld_u(e, SLJIT_R0, n);
			op1(e, SLJIT_MOV_S16, SLJIT_R0, 0, SLJIT_R0, 0);
			ld_u(e, SLJIT_R1, m);
			op1(e, SLJIT_MOV_S16, SLJIT_R1, 0, SLJIT_R1, 0);
			op2(e, SLJIT_MUL, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0);
			op1(e, SLJIT_MOV_U32, CELL(macl), SLJIT_R0, 0);
			return true;
		default:
			return false;
		}

	case 0x3:
		switch (op & 15)
		{
		case 0: emit_cmp_t(e, n, m, SLJIT_EQUAL, SLJIT_SET_Z, false); return true;
		case 2: emit_cmp_t(e, n, m, SLJIT_GREATER_EQUAL, SLJIT_SET_LESS, false); return true;
		case 3: emit_cmp_t(e, n, m, SLJIT_SIG_GREATER_EQUAL, SLJIT_SET_SIG_LESS, true); return true;
		case 6: emit_cmp_t(e, n, m, SLJIT_GREATER, SLJIT_SET_GREATER, false); return true;
		case 7: emit_cmp_t(e, n, m, SLJIT_SIG_GREATER, SLJIT_SET_SIG_GREATER, true); return true;
		case 4:
			call_pre(e);
			movi(e, SLJIT_R1, m);
			movi(e, SLJIT_R2, n);
			sljit_emit_icall(e->c, SLJIT_CALL, SLJIT_ARGS3V(P, W, W), SLJIT_IMM, (sljit_sw)(uintptr_t)hdiv1);
			call_post(e);
			return true;
		case 5: case 13:
			if ((op & 15) == 5)
			{
				ld_u(e, SLJIT_R0, n);
				ld_u(e, SLJIT_R1, m);
			}
			else
			{
				ld_s(e, SLJIT_R0, n);
				ld_s(e, SLJIT_R1, m);
			}
			op2(e, SLJIT_MUL, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0);
			op1(e, SLJIT_MOV_U32, CELL(macl), SLJIT_R0, 0);
			op2(e, SLJIT_LSHR, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, 32);
			op1(e, SLJIT_MOV_U32, CELL(mach), SLJIT_R0, 0);
			return true;
		case 8: case 12:
			ld_u(e, SLJIT_R0, n);
			ld_u(e, SLJIT_R1, m);
			op2(e, (op & 15) == 8 ? SLJIT_SUB : SLJIT_ADD, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0);
			st_r(e, n, SLJIT_R0);
			return true;
		case 10: emit_addc(e, n, m, true); return true;
		case 14: emit_addc(e, n, m, false); return true;
		case 11: emit_addv(e, n, m, true); return true;
		case 15: emit_addv(e, n, m, false); return true;
		default:
			return false;
		}

	case 0x4: return emit_0100(e, op);

	case 0x5:
		ea_disp(e, m, (sljit_sw)((op & 15) * 4));
		emit_load(e, 4, n, false);
		return true;

	case 0x6:
		switch (op & 15)
		{
		case 0: ea_reg(e, m); emit_load(e, 1, n, true); return true;
		case 1: ea_reg(e, m); emit_load(e, 2, n, true); return true;
		case 2: ea_reg(e, m); emit_load(e, 4, n, false); return true;
		case 3: ld_u(e, SLJIT_R0, m); st_r(e, n, SLJIT_R0); return true;
		case 4: case 5: case 6:
		{
			int w = (op & 15) == 4 ? 1 : (op & 15) == 5 ? 2 : 4;
			ea_reg(e, m);
			emit_load(e, w, n, w != 4);
			if (n != m)
				bump(e, m, w);
			return true;
		}
		case 7:
			ld_u(e, SLJIT_R0, m);
			op2(e, SLJIT_XOR, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, -1);
			st_r(e, n, SLJIT_R0);
			return true;
		case 8:
			ld_u(e, SLJIT_R0, m);
			op2(e, SLJIT_AND, SLJIT_R1, 0, SLJIT_R0, 0, SLJIT_IMM, 0xff);
			op2(e, SLJIT_SHL, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_IMM, 8);
			op2(e, SLJIT_LSHR, SLJIT_R2, 0, SLJIT_R0, 0, SLJIT_IMM, 8);
			op2(e, SLJIT_AND, SLJIT_R2, 0, SLJIT_R2, 0, SLJIT_IMM, 0xff);
			op2(e, SLJIT_AND, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, 0xffff0000);
			op2(e, SLJIT_OR, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0);
			op2(e, SLJIT_OR, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R2, 0);
			st_r(e, n, SLJIT_R0);
			return true;
		case 9:
			ld_u(e, SLJIT_R0, m);
			op2(e, SLJIT_SHL, SLJIT_R1, 0, SLJIT_R0, 0, SLJIT_IMM, 16);
			op2(e, SLJIT_LSHR, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, 16);
			op2(e, SLJIT_OR, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0);
			st_r(e, n, SLJIT_R0);
			return true;
		case 10:
			ld_u(e, SLJIT_R4, m);
			get_t(e, SLJIT_R5);
			op2(e, SLJIT_SUB, SLJIT_R3, 0, SLJIT_IMM, 0, SLJIT_R4, 0);
			op2(e, SLJIT_SUB, SLJIT_R3, 0, SLJIT_R3, 0, SLJIT_R5, 0);
			st_r(e, n, SLJIT_R3);
			op2(e, SLJIT_LSHR, SLJIT_R3, 0, SLJIT_R3, 0, SLJIT_IMM, 32);
			set_t(e, SLJIT_R3);
			return true;
		case 11:
			ld_u(e, SLJIT_R0, m);
			op2(e, SLJIT_SUB, SLJIT_R0, 0, SLJIT_IMM, 0, SLJIT_R0, 0);
			st_r(e, n, SLJIT_R0);
			return true;
		case 12:
			ld_u(e, SLJIT_R0, m);
			op2(e, SLJIT_AND, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, 0xff);
			st_r(e, n, SLJIT_R0);
			return true;
		case 13:
			ld_u(e, SLJIT_R0, m);
			op2(e, SLJIT_AND, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, 0xffff);
			st_r(e, n, SLJIT_R0);
			return true;
		case 14:
			ld_u(e, SLJIT_R0, m);
			op1(e, SLJIT_MOV_S8, SLJIT_R0, 0, SLJIT_R0, 0);
			st_r(e, n, SLJIT_R0);
			return true;
		default:
			ld_u(e, SLJIT_R0, m);
			op1(e, SLJIT_MOV_S16, SLJIT_R0, 0, SLJIT_R0, 0);
			st_r(e, n, SLJIT_R0);
			return true;
		}

	case 0x7:
		ld_u(e, SLJIT_R0, n);
		op2(e, SLJIT_ADD, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, (sljit_sw)(int8_t)(op & 0xff));
		st_r(e, n, SLJIT_R0);
		return true;

	case 0x8:
		switch ((op >> 8) & 15)
		{
		case 0: ld_u(e, VAL, 0); ea_disp(e, m, (sljit_sw)(op & 15)); mem_write(e, 1, VAL); return true;
		case 1: ld_u(e, VAL, 0); ea_disp(e, m, (sljit_sw)((op & 15) * 2)); mem_write(e, 2, VAL); return true;
		case 4: ea_disp(e, m, (sljit_sw)(op & 15)); emit_load(e, 1, 0, true); return true;
		case 5: ea_disp(e, m, (sljit_sw)((op & 15) * 2)); emit_load(e, 2, 0, true); return true;
		case 8:
			ld_s(e, SLJIT_R3, 0);
			sljit_emit_op2u(e->c, SLJIT_SUB | SLJIT_SET_Z, SLJIT_R3, 0, SLJIT_IMM, (sljit_sw)(int8_t)(op & 0xff));
			set_t_flag(e, SLJIT_EQUAL);
			return true;
		default:
			return false;
		}

	case 0x9:
	{
		uint32_t a, v;
		if (!pc_known)
			return false;
		a = pc + 2 + (uint32_t)(op & 0xff) * 2;
		if (fetch_const(e->cpu, a, 2, &v))
			op1(e, SLJIT_MOV_U32, RN(n), SLJIT_IMM, (sljit_sw)(uint32_t)(int32_t)(int16_t)v);
		else
		{
			movi(e, ADDR, (sljit_sw)a);
			emit_load(e, 2, n, true);
		}
		return true;
	}

	case 0xc:
		switch ((op >> 8) & 15)
		{
		case 0: ld_u(e, VAL, 0); ea_gbr(e, (sljit_sw)(op & 0xff)); mem_write(e, 1, VAL); return true;
		case 1: ld_u(e, VAL, 0); ea_gbr(e, (sljit_sw)((op & 0xff) * 2)); mem_write(e, 2, VAL); return true;
		case 2: ld_u(e, VAL, 0); ea_gbr(e, (sljit_sw)((op & 0xff) * 4)); mem_write(e, 4, VAL); return true;
		case 4: ea_gbr(e, (sljit_sw)(op & 0xff)); emit_load(e, 1, 0, true); return true;
		case 5: ea_gbr(e, (sljit_sw)((op & 0xff) * 2)); emit_load(e, 2, 0, true); return true;
		case 6: ea_gbr(e, (sljit_sw)((op & 0xff) * 4)); emit_load(e, 4, 0, false); return true;
		case 7:
			if (!pc_known)
				return false;
			op1(e, SLJIT_MOV_U32, RN(0), SLJIT_IMM,
				(sljit_sw)(((pc + 2) & ~3u) + (uint32_t)(op & 0xff) * 4));
			return true;
		case 8:
			ld_u(e, SLJIT_R3, 0);
			sljit_emit_op2u(e->c, SLJIT_AND | SLJIT_SET_Z, SLJIT_R3, 0, SLJIT_IMM, (sljit_sw)(op & 0xff));
			set_t_flag(e, SLJIT_EQUAL);
			return true;
		case 9: case 10: case 11:
		{
			sljit_s32 o = ((op >> 8) & 15) == 9 ? SLJIT_AND : ((op >> 8) & 15) == 10 ? SLJIT_XOR : SLJIT_OR;
			ld_u(e, SLJIT_R0, 0);
			op2(e, o, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, (sljit_sw)(op & 0xff));
			st_r(e, 0, SLJIT_R0);
			return true;
		}
		case 12:
			ea_gbr_index(e);
			mem_read(e, 1, VAL);
			sljit_emit_op2u(e->c, SLJIT_AND | SLJIT_SET_Z, VAL, 0, SLJIT_IMM, (sljit_sw)(op & 0xff));
			set_t_flag(e, SLJIT_EQUAL);
			return true;
		default:
		{
			sljit_s32 o = ((op >> 8) & 15) == 13 ? SLJIT_AND : ((op >> 8) & 15) == 14 ? SLJIT_XOR : SLJIT_OR;
			ea_gbr_index(e);
			mem_read(e, 1, VAL);
			op2(e, o, VAL, 0, VAL, 0, SLJIT_IMM, (sljit_sw)(op & 0xff));
			op2(e, SLJIT_AND, VAL, 0, VAL, 0, SLJIT_IMM, 0xff);
			mem_write(e, 1, VAL);
			return true;
		}
		}

	case 0xd:
	{
		uint32_t a, v;
		if (!pc_known)
			return false;
		a = ((pc + 2) & ~3u) + (uint32_t)(op & 0xff) * 4;
		if (fetch_const(e->cpu, a, 4, &v))
			op1(e, SLJIT_MOV_U32, RN(n), SLJIT_IMM, (sljit_sw)v);
		else
		{
			movi(e, ADDR, (sljit_sw)a);
			emit_load(e, 4, n, false);
		}
		return true;
	}

	case 0xe:
		op1(e, SLJIT_MOV_U32, RN(n), SLJIT_IMM, (sljit_sw)(uint32_t)(int32_t)(int8_t)(op & 0xff));
		return true;

	default:
		return false;
	}
}

/* ---- control flow ---- */

static bool slot_ok(emitter_t *e, uint16_t slot, bool have_slot, bool pc_known)
{
	if (!have_slot)
		return false;
	if (op_class(slot) != CLS_NORMAL)
		return false;
	if (!pc_known && op_uses_pc(slot))
		return false;
	return true;
}

static bool emit_branch(emitter_t *e, uint16_t op, uint32_t addr, uint16_t slot, bool have_slot)
{
	uint32_t pc = addr + 2;
	int n = (op >> 8) & 15;
	uint32_t target = 0;
	bool stat = false;
	int cyc;

	switch (op >> 12)
	{
	case 0xa: case 0xb:
		target = pc + 2 + (uint32_t)((((int32_t)(op << 20)) >> 20) * 2);
		stat = true;
		break;
	default:
		break;
	}

	if (!slot_ok(e, slot, have_slot, stat))
		return false;

	if (stat)
		op1(e, SLJIT_MOV_U32, CELL(delay_target), SLJIT_IMM, (sljit_sw)target);
	else
	{
		switch (op >> 12)
		{
		case 0x0:
			if ((op & 0x3f) == 0x0b)
				op1(e, SLJIT_MOV_U32, VAL2, 0, CELL(pr));
			else
			{
				ld_u(e, VAL2, n);
				op2(e, SLJIT_ADD, VAL2, 0, VAL2, 0, SLJIT_IMM, (sljit_sw)(pc + 2));
				op1(e, SLJIT_MOV_U32, VAL2, 0, VAL2, 0);
			}
			break;
		default:
			ld_u(e, VAL2, n);
			break;
		}
		op1(e, SLJIT_MOV_U32, CELL(delay_target), VAL2, 0);
	}

	if ((op >> 12) == 0xb || ((op >> 12) == 0x0 && (op & 0x3f) == 0x03)
	    || ((op >> 12) == 0x4 && (op & 0x3f) == 0x0b))
		op1(e, SLJIT_MOV_U32, CELL(pr), SLJIT_IMM, (sljit_sw)(pc + 2));

	if (!emit_normal(e, slot, target, stat))
	{
		e->failed = true;
		return false;
	}

	cyc = 2 + op_cycles(slot);
	if (stat)
		direct_exit(e, target, cyc);
	else
		dyn_exit(e, cyc);
	return true;
}

static bool emit_cond(emitter_t *e, uint16_t op, uint32_t addr, uint16_t slot, bool have_slot)
{
	uint32_t pc = addr + 2;
	uint32_t target = pc + 2 + (uint32_t)((int32_t)(int8_t)(op & 0xff) * 2);
	int sel = (op >> 8) & 15;
	bool delayed = sel == 13 || sel == 15;
	bool on_set = sel == 9 || sel == 13;
	struct sljit_jump *not_taken;

	if (delayed && !slot_ok(e, slot, have_slot, true))
		return false;

	get_t(e, SLJIT_R0);
	not_taken = sljit_emit_cmp(e->c, on_set ? SLJIT_EQUAL : SLJIT_NOT_EQUAL, SLJIT_R0, 0, SLJIT_IMM, 0);
	if (delayed)
	{
		op1(e, SLJIT_MOV_U32, CELL(delay_target), SLJIT_IMM, (sljit_sw)target);
		if (!emit_normal(e, slot, target, true))
		{
			e->failed = true;
			return false;
		}
		direct_exit(e, target, 2 + op_cycles(slot));
	}
	else
		direct_exit(e, target, 3);
	bind(e, not_taken);
	boundary(e, 1, addr + 2);
	return true;
}

/* ------------------------------------------------------------------ */
/* blocks                                                             */
/* ------------------------------------------------------------------ */

static void patch_slots(struct sh2_jit *j, uint32_t key, void *body)
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

static block_t *translate(sh2_t *cpu, uint32_t key)
{
	struct sh2_jit *j = cpu->jit;
	block_t *b;
	emitter_t e;
	struct sljit_label *body;
	uint32_t pc = key;
	int count = 0, n;
	bool open = true;
	void *code;
	uint16_t op;

	if (j->count * 2 >= j->table_size && !grow(j))
		return NULL;
	b = lookup(j, key);
	b->key = key;
	b->fn = NULL;
	b->body = NULL;
	b->code = NULL;
	b->size = 0;
	j->count++;

	if (!fetch_code(cpu, key, &op))
		return b;

	memset(&e, 0, sizeof(e));
	e.j = j;
	e.cpu = cpu;
	e.c = sljit_create_compiler(NULL);
	if (!e.c)
		return b;
	sljit_emit_enter(e.c, 0, SLJIT_ARGS3(W, P, W, W), 6, 6, 0);
	body = sljit_emit_label(e.c);

	while (count < BLOCK_MAX_INSNS && !e.failed)
	{
		uint16_t slot = 0;
		bool have_slot;
		int cls;

		if (!fetch_code(cpu, pc, &op))
			break;
		cls = op_class(op);
		have_slot = fetch_code(cpu, pc + 2, &slot);

		if (cls == CLS_NONE)
		{
			if (count == 0)
				break;
			emit_fallback(&e, pc);
			open = false;
			break;
		}
		if (cls == CLS_BRANCH)
		{
			if (!emit_branch(&e, op, pc, slot, have_slot))
			{
				if (count == 0)
					break;
				emit_fallback(&e, pc);
			}
			open = false;
			break;
		}
		if (cls == CLS_COND)
		{
			if (!emit_cond(&e, op, pc, slot, have_slot))
			{
				if (count == 0)
					break;
				emit_fallback(&e, pc);
				open = false;
				break;
			}
			count++;
			pc += 2;
			continue;
		}
		if (!emit_normal(&e, op, pc + 2, true))
		{
			if (count == 0)
				break;
			emit_fallback(&e, pc);
			open = false;
			break;
		}
		count++;
		pc += 2;
		if (cls == CLS_SR)
		{
			hard_exit(&e, pc, op_cycles(op));
			open = false;
			break;
		}
		boundary(&e, op_cycles(op), pc);
	}

	if (count == 0 || e.failed)
	{
		sljit_free_compiler(e.c);
		return b;
	}
	if (open)
		chain(&e, pc);
	for (n = 0; n < e.exit_count; n++)
	{
		bind(&e, e.exits[n].jump);
		emit_exit_now(&e, e.exits[n].pc);
	}
	emit_epilogue(&e);
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

static bool make_exit_stub(struct sh2_jit *j)
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

static void build_pages(sh2_t *cpu)
{
	struct sh2_jit *j = cpu->jit;
	uint32_t p;
	for (p = 0; p < PAGE_COUNT; p++)
	{
		uint32_t base = p << PAGE_SHIFT;
		const sh2_region_t *r = NULL;
		int i;
		j->rd[p] = j->wr[p] = NULL;
		if ((uint64_t)base + PAGE_SIZE > SH2_IO_BASE)
			continue;
		for (i = 0; i < cpu->region_count; i++)
		{
			const sh2_region_t *c = &cpu->regions[i];
			uint32_t off = base - c->base;
			if (c->bypass)
				continue;
			if (off < c->size && (uint64_t)off + PAGE_SIZE <= c->size)
			{
				r = c;
				break;
			}
			if ((uint64_t)c->base < (uint64_t)base + PAGE_SIZE && (uint64_t)c->base + c->size > base)
				break;
		}
		if (!r)
			continue;
		j->rd[p] = (uint8_t *)((uintptr_t)r->data - r->base);
		if (r->writable)
			j->wr[p] = j->rd[p];
	}
}

bool sh2_jit_attach(sh2_t *cpu, struct jit_alloc *alloc)
{
	struct sh2_jit *j;
	uint32_t i;
	const char *env;
	sh2_jit_detach(cpu);
	j = calloc(1, sizeof(*j));
	if (!j)
		return false;
	j->alloc = alloc;
	j->table_size = 4096;
	j->table = malloc(j->table_size * sizeof(*j->table));
	j->rd = calloc(PAGE_COUNT, sizeof(*j->rd));
	j->wr = calloc(PAGE_COUNT, sizeof(*j->wr));
	if (!j->table || !j->rd || !j->wr)
	{
		free(j->table);
		free(j->rd);
		free(j->wr);
		free(j);
		return false;
	}
	for (i = 0; i < j->table_size; i++)
		j->table[i].key = KEY_NONE;
	if (!make_exit_stub(j))
	{
		free(j->table);
		free(j->rd);
		free(j->wr);
		free(j);
		return false;
	}
	cpu->jit = j;
	build_pages(cpu);
	env = getenv("SCEMU_SH2_JIT");
	cpu->jit_enabled = !(env && atoi(env) == 0);
	return true;
}

void sh2_jit_remap(sh2_t *cpu)
{
	struct sh2_jit *j = cpu->jit;
	int i;
	if (!j)
		return;
	for (i = 0; i < cpu->region_count; i++)
	{
		const sh2_region_t *c = &cpu->regions[i];
		uint32_t p;
		if ((c->base & (PAGE_SIZE - 1)) || (c->size & (PAGE_SIZE - 1)))
		{
			build_pages(cpu);
			return;
		}
		for (p = c->base >> PAGE_SHIFT; p < (c->base + c->size) >> PAGE_SHIFT; p++)
		{
			j->rd[p] = c->bypass ? NULL : (uint8_t *)((uintptr_t)c->data - c->base);
			j->wr[p] = c->writable ? j->rd[p] : NULL;
		}
	}
}

void sh2_jit_flush(sh2_t *cpu)
{
	struct sh2_jit *j = cpu->jit;
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
	build_pages(cpu);
}

void sh2_jit_detach(sh2_t *cpu)
{
	struct sh2_jit *j = cpu->jit;
	if (!j)
		return;
	sh2_jit_flush(cpu);
	if (j->exit_code)
		sljit_free_code(j->exit_code, (void *)j->alloc->config);
	free(j->table);
	free(j->rd);
	free(j->wr);
	free(j);
	cpu->jit = NULL;
	cpu->jit_enabled = false;
}

size_t sh2_jit_code_size(const sh2_t *cpu)
{
	return cpu->jit ? cpu->jit->code_size : 0;
}

size_t sh2_jit_block_count(const sh2_t *cpu)
{
	return cpu->jit ? cpu->jit->count : 0;
}

void sh2_jit_stats(const sh2_t *cpu, uint64_t out[4])
{
	out[0] = cpu->jit ? cpu->jit->stat_blocks_run : 0;
	out[1] = cpu->jit ? cpu->jit->stat_fallbacks : 0;
	out[2] = cpu->jit ? cpu->jit->stat_interrupts : 0;
	out[3] = cpu->jit ? cpu->jit->stat_flushes : 0;
}

#else

int sh2_jit_run(sh2_t *cpu, int cycles)
{
	int ran = 0;
	while (ran < cycles)
		ran += sh2_step(cpu);
	return ran;
}

bool sh2_jit_attach(sh2_t *cpu, struct jit_alloc *alloc) { (void)cpu; (void)alloc; return false; }
void sh2_jit_detach(sh2_t *cpu) { (void)cpu; }
void sh2_jit_flush(sh2_t *cpu) { (void)cpu; }
void sh2_jit_remap(sh2_t *cpu) { (void)cpu; }
size_t sh2_jit_code_size(const sh2_t *cpu) { (void)cpu; return 0; }
size_t sh2_jit_block_count(const sh2_t *cpu) { (void)cpu; return 0; }
void sh2_jit_stats(const sh2_t *cpu, uint64_t out[4]) { (void)cpu; out[0] = out[1] = out[2] = out[3] = 0; }

#endif
