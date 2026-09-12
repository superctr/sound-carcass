#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "lsp.h"
#include "state.h"

static const int32_t IMMEDIATE[5] = { 0, 1 << 7, 1 << 12, 1 << 17, 1 << 22 };

static inline int32_t narrow(int32_t v) { return (int32_t)((uint32_t)v << 8) >> 8; }

/* ------------------------------------------------------------------ decode */

typedef struct lsp_insn
{
	uint8_t opcode;
	uint8_t store;
	uint8_t command;
	uint8_t offset;
	uint8_t coefficient;
	uint8_t shift;
	bool present;
} lsp_insn_t;

typedef enum lsp_jump
{
	JUMP_NONE, JUMP_CONDITIONAL, JUMP_ALWAYS
} lsp_jump_t;

static lsp_insn_t decode(uint32_t word)
{
	lsp_insn_t s;
	s.opcode = (word >> 21) & 7;
	s.store = (word >> 19) & 3;
	s.command = (word >> 16) & 7;
	s.offset = (word >> 8) & 0x7f;
	s.coefficient = word & 0xff;
	s.shift = ((word >> 15) & 1) ? 5 : 7;
	s.present = word != 0;
	return s;
}

static bool is_special(const lsp_insn_t *s) { return s->present && s->opcode >= LSP_OP_SPECIAL_A; }
static int insn_slot(const lsp_insn_t *s) { return s->offset & 0x1f; }
static bool insn_replace(const lsp_insn_t *s) { return (s->offset >> 5) & 1; }
static bool is_immediate(int offset) { return offset >= 1 && offset <= 4; }

static lsp_jump_t jump_kind(const lsp_insn_t *s)
{
	if (!is_special(s))
		return JUMP_NONE;
	switch (insn_slot(s))
	{
	case LSP_SLOT_JUMP_NEGATIVE: return s->store ? JUMP_CONDITIONAL : JUMP_NONE;
	case LSP_SLOT_JUMP_POSITIVE: return s->store ? JUMP_CONDITIONAL : JUMP_ALWAYS;
	case LSP_SLOT_JUMP:          return JUMP_ALWAYS;
	default:                     return JUMP_NONE;
	}
}

static bool is_second_operand(const lsp_insn_t *s)
{
	return is_special(s) && insn_slot(s) == LSP_SLOT_ERAM_WRITE && !s->store;
}

static bool coefficient_is_free(const lsp_insn_t *s)
{
	return s->present && s->opcode != LSP_OP_MUL && jump_kind(s) == JUMP_NONE;
}

static void modifies(const lsp_insn_t *s, bool m[2])
{
	m[0] = m[1] = false;
	if (!s->present)
		return;
	if (!is_special(s))
	{
		if (s->opcode == LSP_OP_MUL)
			m[(s->coefficient >> 4) & 1] = true;
		else
			m[s->opcode == LSP_OP_MAC_B || s->opcode == LSP_OP_SET_B] = true;
		return;
	}
	switch (insn_slot(s))
	{
	case LSP_SLOT_ERAM_WRITE:
		if (!s->store)
			m[s->opcode & 1] = true;
		break;
	case LSP_SLOT_AUDIO_OUT:
	case LSP_SLOT_ERAM_READ:
	case LSP_SLOT_ERAM_READ + 1:
	case LSP_SLOT_ERAM_READ + 2:
	case LSP_SLOT_ERAM_READ + 3:
	case LSP_SLOT_AUDIO_IN:
		m[s->opcode & 1] = true;
		break;
	}
}

/* ------------------------------------------------------------------ host */

bool lsp_init(lsp_t *lsp, jit_alloc_t *jit)
{
	memset(lsp, 0, sizeof(*lsp));
	lsp->jit = jit;
	lsp->eram = calloc(LSP_ERAM_SIZE, sizeof(int32_t));
	return lsp->eram != NULL;
}

void lsp_release(lsp_t *lsp)
{
	jit_code_free(lsp->jit, &lsp->code[0]);
	jit_code_free(lsp->jit, &lsp->code[1]);
	lsp->sample = NULL;
	free(lsp->eram);
	lsp->eram = NULL;
}

void lsp_reset(lsp_t *lsp)
{
	memset(lsp->program, 0, sizeof(lsp->program));
	memset(lsp->iram_window, 0, sizeof(lsp->iram_window));
	memset(lsp->eram, 0, LSP_ERAM_SIZE * sizeof(int32_t));
	memset(lsp->eram_cmd, 0, sizeof(lsp->eram_cmd));
	memset(&lsp->state, 0, sizeof(lsp->state));
	memset(lsp->patched, 0, sizeof(lsp->patched));
	lsp->configuration = 0;
	lsp->running = false;
	lsp->dirty = true;
	lsp->serial_in[0] = lsp->serial_in[1] = 0;
	lsp->serial_out[0] = lsp->serial_out[1] = 0;
	lsp->audio_out = 0;
	lsp->host_data = 0;
	lsp->host_read = 0;
	lsp->host_address = 0;
}

static uint32_t internal_read(const lsp_t *lsp, uint32_t address)
{
	return (address < LSP_PROGRAM_BASE) ? (uint32_t)(lsp->iram[address] & 0xffffff) : lsp->program[address - LSP_PROGRAM_BASE];
}

static void internal_write(lsp_t *lsp, uint32_t address, uint32_t data)
{
	if (address < LSP_PROGRAM_BASE)
	{
		lsp->iram[address] = lsp->iram_mirror[address] = narrow((int32_t)data);
		return;
	}

	const int n = address - LSP_PROGRAM_BASE;
	const uint32_t old = lsp->program[n];
	const uint32_t word = data & 0xffffff;
	if (old == word)
		return;
	lsp->program[n] = word;

	const lsp_insn_t s = decode(old);
	if (lsp->running && ((old ^ word) & 0xffff00) == 0 && coefficient_is_free(&s))
	{
		lsp->patched[n] = 1;
		if (!lsp->cell[n])
			lsp->dirty = true;
	}
	else
	{
		lsp->patched[n] = 0;
		lsp->dirty = true;
	}
}

static void configure(lsp_t *lsp, uint16_t word)
{
	lsp->configuration = word;
	lsp->running = !((word >> 12) & 1);
}

uint8_t lsp_host_read(lsp_t *lsp, uint32_t offset)
{
	switch (offset & 0xf)
	{
	case LSP_HOST_ADDRESS_LOW:  return lsp->host_read & 0xff;
	case LSP_HOST_ADDRESS_HIGH: return (lsp->host_read >> 8) & 0xff;
	case LSP_HOST_DATA_LOW:     return (lsp->host_read >> 16) & 0xff;
	default:                    return 0;
	}
}

void lsp_host_write(lsp_t *lsp, uint32_t offset, uint8_t data)
{
	switch (offset & 0xf)
	{
	case LSP_HOST_ADDRESS_LOW:
		lsp->host_address = (uint16_t)((lsp->host_address & 0xff00) | data);
		internal_write(lsp, lsp->host_address & LSP_ADDRESS_MASK, lsp->host_data);
		break;
	case LSP_HOST_ADDRESS_HIGH:
		lsp->host_address = (uint16_t)((lsp->host_address & 0x00ff) | (data << 8));
		break;
	case LSP_HOST_DATA_LOW:
		lsp->host_data = (lsp->host_data & 0xffff00) | data;
		break;
	case LSP_HOST_DATA_MID:
		lsp->host_data = (lsp->host_data & 0xff00ff) | ((uint32_t)data << 8);
		break;
	case LSP_HOST_DATA_HIGH:
		lsp->host_data = (lsp->host_data & 0x00ffff) | ((uint32_t)data << 16);
		break;
	case LSP_HOST_CONFIGURE:
		configure(lsp, lsp->host_data & 0xffff);
		break;
	case LSP_HOST_READ_LOW:
		lsp->host_address = (uint16_t)((lsp->host_address & 0xff00) | data);
		lsp->host_read = internal_read(lsp, lsp->host_address & LSP_ADDRESS_MASK);
		break;
	case LSP_HOST_READ_HIGH:
		lsp->host_address = (uint16_t)((lsp->host_address & 0x00ff) | (data << 8));
		break;
	}
}

void lsp_serial_write(lsp_t *lsp, int channel, int32_t sample)
{
	lsp->serial_in[channel & 1] = sample;
}

int32_t lsp_serial_read(const lsp_t *lsp, int channel)
{
	return lsp->serial_out[channel & 1];
}

#if (defined SLJIT_64BIT_ARCHITECTURE && SLJIT_64BIT_ARCHITECTURE)

/* ------------------------------------------------------------------ plan */

#define LSP_ADDRESSES (LSP_ADDRESS_MASK + 1)
#define RING 16
#define UNKNOWN (-1)
#define BLOCK_BUDGET 2048
#define HASH_SIZE 8192
#define MAX_RESTARTS 32

/* what is known about the machine on entry to a block, merged over its predecessors */
typedef struct lsp_ctx
{
	int8_t ring[RING];
	int8_t prev_offset;
	int8_t age[2];
	bool valid;
} lsp_ctx_t;

/* one block per (pc, slot, pending jump) the program can reach; slot UNKNOWN once a pc is
 * reachable at too many slots to unroll */
typedef struct lsp_block
{
	uint16_t pc;
	int16_t slot;
	int16_t pending;
	int16_t next[2];
	lsp_ctx_t in;
	struct sljit_label *label;
} lsp_block_t;

typedef struct lsp_patch
{
	struct sljit_jump *jump;
	int target;
} lsp_patch_t;

typedef struct lsp_stub
{
	struct sljit_jump *enter;
	struct sljit_label *back;
	sljit_s32 reg;
	sljit_s32 tmp;
} lsp_stub_t;

typedef struct lsp_compile
{
	lsp_insn_t insn[LSP_ADDRESSES];
	lsp_block_t blocks[BLOCK_BUDGET + 2];
	int16_t order[BLOCK_BUDGET + 2];
	int16_t stack[BLOCK_BUDGET + 2];
	uint32_t hash_key[HASH_SIZE];
	int16_t hash_value[HASH_SIZE];
	uint8_t unknown[LSP_ADDRESSES];
	uint8_t slots_seen[LSP_ADDRESSES][LSP_PROGRAM_SIZE / 8];
	lsp_patch_t patches[2 * (BLOCK_BUDGET + 2)];
	lsp_patch_t ends[BLOCK_BUDGET + 2];
	lsp_stub_t stubs[3 * (BLOCK_BUDGET + 2)];
	int count;
	int patch_count;
	int end_count;
	int stub_count;
} lsp_compile_t;

static uint32_t block_key(int pc, int slot, int pending)
{
	return (uint32_t)(pc | ((slot + 1) << 9) | ((pending + 1) << 19)) + 1;
}

static int find_block(lsp_compile_t *c, int pc, int slot, int pending, bool *added)
{
	const uint32_t key = block_key(pc, slot, pending);
	uint32_t h = (key * 2654435761u) >> 19;
	for (;;)
	{
		h &= HASH_SIZE - 1;
		if (c->hash_key[h] == key)
		{
			*added = false;
			return c->hash_value[h];
		}
		if (!c->hash_key[h])
			break;
		h++;
	}
	if (c->count > BLOCK_BUDGET)
		return -1;
	c->hash_key[h] = key;
	c->hash_value[h] = (int16_t)c->count;
	lsp_block_t *b = &c->blocks[c->count++];
	b->pc = (uint16_t)pc;
	b->slot = (int16_t)slot;
	b->pending = (int16_t)pending;
	b->next[0] = b->next[1] = -1;
	b->label = NULL;
	*added = true;
	return c->count - 1;
}

static bool explore(lsp_compile_t *c)
{
	int depth = 0;
	bool added;

	memset(c->hash_key, 0, sizeof(c->hash_key));
	c->count = 0;
	c->stack[depth++] = (int16_t)find_block(c, LSP_PROGRAM_BASE, 0, -1, &added);

	while (depth)
	{
		const int index = c->stack[--depth];
		const int pc = c->blocks[index].pc, slot = c->blocks[index].slot, pending = c->blocks[index].pending;
		const lsp_insn_t *s = &c->insn[pc];
		const lsp_jump_t kind = jump_kind(s);

		if (slot == LSP_PROGRAM_SIZE - 1)
			continue;

		for (int fired = 0; fired < 2; fired++)
		{
			int npc, npending, nslot, next;
			if (fired ? kind == JUMP_NONE : kind == JUMP_ALWAYS)
				continue;
			if (fired)
			{
				npc = (pc + 1) & LSP_ADDRESS_MASK;
				npending = (s->coefficient << 1) & LSP_ADDRESS_MASK;
			}
			else
			{
				npc = pending >= 0 ? pending : (pc + 1) & LSP_ADDRESS_MASK;
				npending = -1;
			}
			nslot = (slot < 0 || c->unknown[npc]) ? UNKNOWN : slot + 1;
			next = find_block(c, npc, nslot, npending, &added);
			if (next < 0)
				return false;
			c->blocks[index].next[fired] = (int16_t)next;
			if (added)
				c->stack[depth++] = (int16_t)next;
		}
	}
	return true;
}

static void plan_blocks(lsp_compile_t *c)
{
	memset(c->unknown, 0, sizeof(c->unknown));
	for (int restarts = 0; !explore(c); restarts++)
	{
		if (restarts >= MAX_RESTARTS)
		{
			memset(c->unknown, 1, sizeof(c->unknown));
			continue;
		}
		memset(c->slots_seen, 0, sizeof(c->slots_seen));
		for (int n = 0; n < c->count; n++)
			if (c->blocks[n].slot >= 0)
				c->slots_seen[c->blocks[n].pc][c->blocks[n].slot >> 3] |= (uint8_t)(1 << (c->blocks[n].slot & 7));
		int worst = 0, worst_count = -1;
		for (int pc = 0; pc < LSP_ADDRESSES; pc++)
		{
			int count = 0;
			for (int k = 0; k < LSP_PROGRAM_SIZE / 8; k++)
				for (uint8_t bits = c->slots_seen[pc][k]; bits; bits &= bits - 1)
					count++;
			if (count > worst_count)
			{
				worst = pc;
				worst_count = count;
			}
		}
		c->unknown[worst] = 1;
	}
}

static bool ring_known(const lsp_ctx_t *ctx, int from, int to)
{
	for (int k = from; k <= to; k++)
		if (ctx->ring[k] < 0)
			return false;
	return true;
}

static void ctx_after(const lsp_ctx_t *in, const lsp_insn_t *s, lsp_ctx_t *out)
{
	bool m[2];
	modifies(s, m);
	out->ring[0] = (int8_t)s->command;
	for (int k = 1; k < RING; k++)
		out->ring[k] = in->ring[k - 1];
	out->prev_offset = (int8_t)s->offset;
	for (int n = 0; n < 2; n++)
		out->age[n] = m[n] ? 0 : (int8_t)(in->age[n] < 2 ? in->age[n] + 1 : 2);
	out->valid = true;
}

static bool ctx_merge(lsp_ctx_t *dst, const lsp_ctx_t *src)
{
	bool changed = false;
	if (!dst->valid)
	{
		*dst = *src;
		return true;
	}
	for (int k = 0; k < RING; k++)
		if (dst->ring[k] != src->ring[k] && dst->ring[k] != UNKNOWN)
		{
			dst->ring[k] = UNKNOWN;
			changed = true;
		}
	if (dst->prev_offset != src->prev_offset && dst->prev_offset != UNKNOWN)
	{
		dst->prev_offset = UNKNOWN;
		changed = true;
	}
	for (int n = 0; n < 2; n++)
		if (src->age[n] < dst->age[n])
		{
			dst->age[n] = src->age[n];
			changed = true;
		}
	return changed;
}

static void plan_contexts(lsp_compile_t *c)
{
	lsp_ctx_t entry;
	bool changed;

	memset(&entry, UNKNOWN, sizeof(entry));
	entry.age[0] = entry.age[1] = 0;
	entry.valid = true;
	for (int n = 0; n < c->count; n++)
		c->blocks[n].in.valid = false;
	c->blocks[0].in = entry;

	do
	{
		changed = false;
		for (int n = 0; n < c->count; n++)
		{
			lsp_block_t *b = &c->blocks[n];
			lsp_ctx_t out;
			if (!b->in.valid)
				continue;
			ctx_after(&b->in, &c->insn[b->pc], &out);
			for (int k = 0; k < 2; k++)
				if (b->next[k] >= 0 && ctx_merge(&c->blocks[b->next[k]].in, &out))
					changed = true;
		}
	} while (changed);
}

static void plan_order(lsp_compile_t *c)
{
	int placed = 0;
	for (int n = 0; n < c->count; n++)
		c->blocks[n].label = NULL;
	for (int n = 0; n < c->count; n++)
	{
		int b = n;
		while (b >= 0 && !c->blocks[b].label)
		{
			c->blocks[b].label = (struct sljit_label *)1;
			c->order[placed++] = (int16_t)b;
			b = c->blocks[b].next[0] >= 0 ? c->blocks[b].next[0] : c->blocks[b].next[1];
		}
	}
}

/* ------------------------------------------------------------------ emit */

#define CELLW(field) ((sljit_sw)offsetof(lsp_t, field))
#define CELL(field) SLJIT_MEM1(SLJIT_S0), CELLW(field)

#define R_ERAM SLJIT_S1
#define R_IRAM SLJIT_S2

static const sljit_s32 ACC[2][3] = { { SLJIT_S3, SLJIT_S4, SLJIT_S5 }, { SLJIT_R4, SLJIT_R5, SLJIT_R6 } };

#define IRAM_STRIDE ((sljit_sw)(LSP_IRAM_SIZE * sizeof(int32_t)))
#define CMD_BASE ((sljit_sw)offsetof(lsp_t, eram_cmd))
#if (defined SLJIT_BIG_ENDIAN && SLJIT_BIG_ENDIAN)
#define COEFFICIENT_BYTE 3
#else
#define COEFFICIENT_BYTE 0
#endif

typedef struct lsp_regs
{
	sljit_s32 old[2];
	sljit_s32 new[2];
	sljit_s32 stored[2];
} lsp_regs_t;

typedef struct lsp_operand
{
	sljit_s32 r;
	sljit_sw w;
} lsp_operand_t;

typedef struct lsp_emitter
{
	jit_builder_t b;
	lsp_t *lsp;
	lsp_compile_t *c;
} lsp_emitter_t;

static lsp_regs_t block_regs(const lsp_block_t *blk)
{
	lsp_regs_t r;
	for (int n = 0; n < 2; n++)
	{
		if (blk->slot >= 0)
		{
			r.new[n] = r.stored[n] = ACC[n][blk->slot % 3];
			r.old[n] = ACC[n][(blk->slot + 2) % 3];
		}
		else
		{
			r.old[n] = r.new[n] = ACC[n][2];
			r.stored[n] = ACC[n][0];
		}
	}
	return r;
}

static void e_sext(lsp_emitter_t *e, sljit_s32 reg)
{
	sljit_emit_op1(e->b.c, SLJIT_MOV_S32, reg, 0, reg, 0);
}

static void e_mov(lsp_emitter_t *e, sljit_s32 dst, sljit_s32 src, sljit_sw srcw)
{
	sljit_emit_op1(e->b.c, SLJIT_MOV, dst, 0, src, srcw);
}

static void e_load(lsp_emitter_t *e, sljit_s32 dst, sljit_s32 mem, sljit_sw memw)
{
	sljit_emit_op1(e->b.c, SLJIT_MOV_S32, dst, 0, mem, memw);
}

static void e_store(lsp_emitter_t *e, sljit_s32 mem, sljit_sw memw, sljit_s32 src)
{
	sljit_emit_op1(e->b.c, SLJIT_MOV32, mem, memw, src, 0);
}

static void e_store_u8(lsp_emitter_t *e, sljit_sw cell, sljit_s32 src, sljit_sw srcw)
{
	sljit_emit_op1(e->b.c, SLJIT_MOV_U8, SLJIT_MEM1(SLJIT_S0), cell, src, srcw);
}

static void e_store_u16(lsp_emitter_t *e, sljit_sw cell, sljit_s32 src, sljit_sw srcw)
{
	sljit_emit_op1(e->b.c, SLJIT_MOV_U16, SLJIT_MEM1(SLJIT_S0), cell, src, srcw);
}

static void e_load_u8(lsp_emitter_t *e, sljit_s32 dst, sljit_s32 mem, sljit_sw memw)
{
	sljit_emit_op1(e->b.c, SLJIT_MOV_U8, dst, 0, mem, memw);
}

static void e_load_u16(lsp_emitter_t *e, sljit_s32 dst, sljit_sw cell)
{
	sljit_emit_op1(e->b.c, SLJIT_MOV_U16, dst, 0, SLJIT_MEM1(SLJIT_S0), cell);
}

static void e_op2(lsp_emitter_t *e, sljit_s32 op, sljit_s32 dst, sljit_s32 a, sljit_s32 src, sljit_sw srcw)
{
	sljit_emit_op2(e->b.c, op, dst, 0, a, 0, src, srcw);
}

static struct sljit_jump *e_cmp(lsp_emitter_t *e, sljit_s32 type, sljit_s32 a, sljit_s32 src, sljit_sw srcw)
{
	return sljit_emit_cmp(e->b.c, type, a, 0, src, srcw);
}

static struct sljit_jump *e_jump(lsp_emitter_t *e)
{
	return sljit_emit_jump(e->b.c, SLJIT_JUMP);
}

static void e_label(lsp_emitter_t *e, struct sljit_jump *jump)
{
	sljit_set_label(jump, sljit_emit_label(e->b.c));
}

/* dst = s32(a + b) */
static void e_add(lsp_emitter_t *e, sljit_s32 dst, sljit_s32 a, sljit_s32 src, sljit_sw srcw)
{
	e_op2(e, SLJIT_ADD, dst, a, src, srcw);
	e_sext(e, dst);
}

/* reg = s32(reg << 8) >> 8 */
static void e_narrow(lsp_emitter_t *e, sljit_s32 reg)
{
	e_op2(e, SLJIT_SHL, reg, reg, SLJIT_IMM, 8);
	e_sext(e, reg);
	e_op2(e, SLJIT_ASHR, reg, reg, SLJIT_IMM, 8);
}

/* reg = clamp(reg, -2^23, 2^23 - 1), the out-of-range case out of line */
static void e_saturate(lsp_emitter_t *e, sljit_s32 reg, sljit_s32 tmp)
{
	lsp_stub_t *stub = &e->c->stubs[e->c->stub_count++];
	e_op2(e, SLJIT_ADD, tmp, reg, SLJIT_IMM, 0x800000);
	sljit_emit_op2(e->b.c, SLJIT_LSHR | SLJIT_SET_Z, tmp, 0, tmp, 0, SLJIT_IMM, 24);
	stub->enter = sljit_emit_jump(e->b.c, SLJIT_NOT_ZERO);
	stub->back = sljit_emit_label(e->b.c);
	stub->reg = reg;
	stub->tmp = tmp;
}

static void e_stubs(lsp_emitter_t *e)
{
	for (int n = 0; n < e->c->stub_count; n++)
	{
		const lsp_stub_t *stub = &e->c->stubs[n];
		e_label(e, stub->enter);
		e_op2(e, SLJIT_ASHR, stub->tmp, stub->reg, SLJIT_IMM, 31);
		e_op2(e, SLJIT_XOR, stub->reg, stub->tmp, SLJIT_IMM, 0x7fffff);
		sljit_set_label(e_jump(e), stub->back);
	}
}

/* dst = the value a store of the given kind writes, read from the register holding it */
static void e_source(lsp_emitter_t *e, sljit_s32 dst, sljit_s32 tmp, sljit_s32 reg, int store)
{
	e_mov(e, dst, reg, 0);
	if (store == 3)
		e_narrow(e, dst);
	else
		e_saturate(e, dst, tmp);
}

static void e_value(lsp_emitter_t *e, sljit_s32 dst, sljit_s32 tmp, const lsp_regs_t *r, int store)
{
	if (store)
		e_source(e, dst, tmp, r->stored[store == 2], store);
	else
		e_mov(e, dst, SLJIT_IMM, 0);
}

/* dst = s32((s64)src * coefficient >> shift) */
static void e_product(lsp_emitter_t *e, sljit_s32 dst, sljit_s32 src, lsp_operand_t coefficient, int shift)
{
	e_op2(e, SLJIT_MUL, dst, src, coefficient.r, coefficient.w);
	e_op2(e, SLJIT_ASHR, dst, dst, SLJIT_IMM, shift);
	e_sext(e, dst);
}

static lsp_operand_t e_coefficient(lsp_emitter_t *e, int pc, sljit_s32 reg, bool as_unsigned)
{
	const lsp_insn_t *s = &e->c->insn[pc];
	lsp_operand_t operand;
	if (pc >= LSP_PROGRAM_BASE && e->lsp->patched[pc - LSP_PROGRAM_BASE])
	{
		sljit_emit_op1(e->b.c, as_unsigned ? SLJIT_MOV_U8 : SLJIT_MOV_S8, reg, 0, SLJIT_MEM1(SLJIT_S0),
			CELLW(program) + (sljit_sw)(pc - LSP_PROGRAM_BASE) * 4 + COEFFICIENT_BYTE);
		operand.r = reg;
		operand.w = 0;
	}
	else
	{
		operand.r = SLJIT_IMM;
		operand.w = as_unsigned ? s->coefficient : (int8_t)s->coefficient;
	}
	return operand;
}

static void e_iram_store(lsp_emitter_t *e, int offset, sljit_s32 reg)
{
	const sljit_sw at = (sljit_sw)offset * 4;
	e_store(e, SLJIT_MEM1(R_IRAM), at - IRAM_STRIDE, reg);
	e_store(e, SLJIT_MEM1(R_IRAM), at, reg);
	e_store(e, SLJIT_MEM1(R_IRAM), at + IRAM_STRIDE, reg);
}

/* the accumulator pipeline advances one slot: in an unrolled block the registers rotate by
 * naming, elsewhere the values shift */
static void e_commit(lsp_emitter_t *e, const lsp_block_t *blk, const lsp_regs_t *r, const bool m[2])
{
	for (int n = 0; n < 2; n++)
	{
		if (blk->in.age[n] >= 2)
			continue;
		if (blk->slot >= 0)
		{
			if (!m[n])
				e_mov(e, r->new[n], r->old[n], 0);
		}
		else
		{
			e_mov(e, ACC[n][0], ACC[n][1], 0);
			e_mov(e, ACC[n][1], ACC[n][2], 0);
		}
	}
}

/* ---- the external RAM command history */

/* dst = the command `back` slots before this one (back >= 1); index holds slot & 15 when the slot is
 * not known */
static void e_ring_entry(lsp_emitter_t *e, const lsp_block_t *blk, sljit_s32 dst, int back, sljit_s32 index)
{
	const int value = blk->in.ring[back - 1];
	if (value >= 0)
	{
		e_mov(e, dst, SLJIT_IMM, value);
		return;
	}
	if (blk->slot >= 0)
	{
		e_load_u8(e, dst, SLJIT_MEM1(SLJIT_S0), CMD_BASE + ((blk->slot - back) & 15));
		return;
	}
	e_op2(e, SLJIT_SUB, SLJIT_R0, index, SLJIT_IMM, back);
	e_op2(e, SLJIT_AND, SLJIT_R0, SLJIT_R0, SLJIT_IMM, 15);
	e_op2(e, SLJIT_ADD, SLJIT_R0, SLJIT_R0, SLJIT_IMM, CMD_BASE);
	e_load_u8(e, dst, SLJIT_MEM2(SLJIT_S0, SLJIT_R0), 0);
}

static uint16_t ring_base_value(const lsp_ctx_t *in, int delay)
{
	uint32_t base = 0;
	for (int n = 0; n < 5; n++)
		base += (uint32_t)in->ring[delay - 2 - n] << (n * 3);
	if (in->ring[delay - 7] & 1)
		base += (uint32_t)in->ring[delay - 7] << 15;
	return (uint16_t)base;
}

/* R2 = the base the six commands below `delay` assemble */
static void e_ring_base(lsp_emitter_t *e, const lsp_block_t *blk, int delay, sljit_s32 index)
{
	struct sljit_jump *skip;
	e_mov(e, SLJIT_R2, SLJIT_IMM, 0);
	for (int n = 0; n < 5; n++)
	{
		e_ring_entry(e, blk, SLJIT_R1, delay - 1 - n, index);
		if (n)
			e_op2(e, SLJIT_SHL, SLJIT_R1, SLJIT_R1, SLJIT_IMM, n * 3);
		e_op2(e, SLJIT_ADD, SLJIT_R2, SLJIT_R2, SLJIT_R1, 0);
	}
	e_ring_entry(e, blk, SLJIT_R1, delay - 6, index);
	e_op2(e, SLJIT_AND, SLJIT_R0, SLJIT_R1, SLJIT_IMM, 1);
	skip = e_cmp(e, SLJIT_EQUAL, SLJIT_R0, SLJIT_IMM, 0);
	e_op2(e, SLJIT_SHL, SLJIT_R1, SLJIT_R1, SLJIT_IMM, 15);
	e_op2(e, SLJIT_ADD, SLJIT_R2, SLJIT_R2, SLJIT_R1, 0);
	e_label(e, skip);
	e_op2(e, SLJIT_AND, SLJIT_R2, SLJIT_R2, SLJIT_IMM, 0xffff);
}

static void e_base_from_ring(lsp_emitter_t *e, const lsp_block_t *blk, int read, int delay, sljit_s32 index)
{
	const sljit_sw base = read ? CELLW(state.eram_base[1]) : CELLW(state.eram_base[0]);
	const sljit_sw tap2 = read ? CELLW(state.eram_tap2[1]) : CELLW(state.eram_tap2[0]);
	if (ring_known(&blk->in, delay - 7, delay - 2))
		e_store_u16(e, base, SLJIT_IMM, ring_base_value(&blk->in, delay));
	else
	{
		e_ring_base(e, blk, delay, index);
		e_store_u16(e, base, SLJIT_R2, 0);
	}
	e_store_u8(e, tap2, SLJIT_IMM, 0);
}

static void e_second_tap(lsp_emitter_t *e, const lsp_block_t *blk, int delay, sljit_s32 index)
{
	const int first = blk->in.ring[delay - 2];
	if (first >= 0)
		e_store_u16(e, CELLW(state.eram_base[1]), SLJIT_IMM, first == 2);
	else
	{
		struct sljit_jump *other;
		e_ring_entry(e, blk, SLJIT_R1, delay - 1, index);
		e_mov(e, SLJIT_R2, SLJIT_IMM, 0);
		other = e_cmp(e, SLJIT_NOT_EQUAL, SLJIT_R1, SLJIT_IMM, 2);
		e_mov(e, SLJIT_R2, SLJIT_IMM, 1);
		e_label(e, other);
		e_store_u16(e, CELLW(state.eram_base[1]), SLJIT_R2, 0);
	}
	e_store_u8(e, CELLW(state.eram_tap2[1]), SLJIT_IMM, 1);
}

static void e_present(lsp_emitter_t *e, const lsp_block_t *blk, int read, sljit_s32 index)
{
	const int delay = read ? 12 : 8;
	const int opener = blk->in.ring[delay - 1];
	struct sljit_jump *skip, *other, *done;

	if (opener >= 0)
	{
		const int kind = (opener >> 1) & 3;
		if (read ? (kind != 1 && kind != 2) : kind != 3)
			return;
		if (kind == 2)
			e_second_tap(e, blk, delay, index);
		else
			e_base_from_ring(e, blk, read, delay, index);
		return;
	}

	e_ring_entry(e, blk, SLJIT_R1, delay, index);
	e_op2(e, SLJIT_LSHR, SLJIT_R1, SLJIT_R1, SLJIT_IMM, 1);
	e_op2(e, SLJIT_AND, SLJIT_R1, SLJIT_R1, SLJIT_IMM, 3);
	if (!read)
	{
		skip = e_cmp(e, SLJIT_NOT_EQUAL, SLJIT_R1, SLJIT_IMM, 3);
		e_base_from_ring(e, blk, read, delay, index);
		e_label(e, skip);
		return;
	}
	other = e_cmp(e, SLJIT_EQUAL, SLJIT_R1, SLJIT_IMM, 2);
	skip = e_cmp(e, SLJIT_NOT_EQUAL, SLJIT_R1, SLJIT_IMM, 1);
	e_base_from_ring(e, blk, read, delay, index);
	done = e_jump(e);
	e_label(e, other);
	e_second_tap(e, blk, delay, index);
	e_label(e, skip);
	e_label(e, done);
}

static void e_clock(lsp_emitter_t *e, const lsp_block_t *blk, int command)
{
	sljit_s32 index = -1;
	const bool writes = blk->slot < 0 || !ring_known(&blk->in, 0, RING - 1);

	if (blk->slot < 0)
	{
		e_load_u16(e, SLJIT_R3, CELLW(state.slot));
		e_op2(e, SLJIT_AND, SLJIT_R3, SLJIT_R3, SLJIT_IMM, 15);
		index = SLJIT_R3;
	}
	if (writes)
	{
		if (blk->slot >= 0)
			e_store_u8(e, CMD_BASE + (blk->slot & 15), SLJIT_IMM, command);
		else
		{
			e_op2(e, SLJIT_ADD, SLJIT_R0, SLJIT_R3, SLJIT_IMM, CMD_BASE);
			sljit_emit_op1(e->b.c, SLJIT_MOV_U8, SLJIT_MEM2(SLJIT_S0, SLJIT_R0), 0, SLJIT_IMM, command);
		}
	}
	e_present(e, blk, 0, index);
	e_present(e, blk, 1, index);
}

/* the whole history as it stands after this block, written where the slot counter indexes it */
static void e_materialize_ring(lsp_emitter_t *e, const lsp_block_t *blk, int command)
{
	uint8_t after[RING], mem[RING];
	sljit_sw words[2] = { 0, 0 };
	after[0] = (uint8_t)command;
	for (int k = 1; k < RING; k++)
		after[k] = (uint8_t)blk->in.ring[k - 1];
	for (int i = 0; i < RING; i++)
		mem[i] = after[(blk->slot - i) & 15];
	for (int i = 0; i < RING; i++)
	{
#if (defined SLJIT_BIG_ENDIAN && SLJIT_BIG_ENDIAN)
		words[i / 8] |= (sljit_sw)mem[i] << (8 * (7 - (i & 7)));
#else
		words[i / 8] |= (sljit_sw)mem[i] << (8 * (i & 7));
#endif
	}
	sljit_emit_op1(e->b.c, SLJIT_MOV, SLJIT_MEM1(SLJIT_S0), CMD_BASE, SLJIT_IMM, words[0]);
	sljit_emit_op1(e->b.c, SLJIT_MOV, SLJIT_MEM1(SLJIT_S0), CMD_BASE + 8, SLJIT_IMM, words[1]);
}

/* dst = (eram_pos + eram_base[read] + tap) & 0xffff */
static void e_eram_index(lsp_emitter_t *e, sljit_s32 dst, bool read)
{
	e_load_u16(e, dst, CELLW(state.eram_pos));
	e_load_u16(e, SLJIT_R2, read ? CELLW(state.eram_base[1]) : CELLW(state.eram_base[0]));
	e_op2(e, SLJIT_ADD, dst, dst, SLJIT_R2, 0);
	if (read)
	{
		struct sljit_jump *skip;
		e_load_u8(e, SLJIT_R2, SLJIT_MEM1(SLJIT_S0), CELLW(state.eram_tap2[1]));
		skip = e_cmp(e, SLJIT_EQUAL, SLJIT_R2, SLJIT_IMM, 0);
		e_load_u16(e, SLJIT_R2, CELLW(state.tap));
		e_op2(e, SLJIT_ADD, dst, dst, SLJIT_R2, 0);
		e_label(e, skip);
	}
	e_op2(e, SLJIT_AND, dst, dst, SLJIT_IMM, 0xffff);
}

/* ---- the instructions */

static void e_multiply(lsp_emitter_t *e, const lsp_insn_t *s, sljit_s32 operand, const lsp_regs_t *r)
{
	const int code = s->coefficient;
	const int n = (code >> 4) & 1;

	e_load(e, SLJIT_R1, SLJIT_MEM1(SLJIT_S0), (code & 2) ? CELLW(state.multiplier[1]) : CELLW(state.multiplier[0]));
	if (code & 0x40)
	{
		e_op2(e, SLJIT_AND, SLJIT_R1, SLJIT_R1, SLJIT_IMM, 0xffff);
		e_op2(e, SLJIT_LSHR, SLJIT_R1, SLJIT_R1, SLJIT_IMM, 9);
		e_op2(e, SLJIT_MUL, SLJIT_R2, operand, SLJIT_R1, 0);
		e_op2(e, SLJIT_ASHR, SLJIT_R2, SLJIT_R2, SLJIT_IMM, s->shift);
		e_op2(e, SLJIT_ASHR, SLJIT_R2, SLJIT_R2, SLJIT_IMM, 7);
	}
	else
	{
		e_op2(e, SLJIT_ASHR, SLJIT_R1, SLJIT_R1, SLJIT_IMM, 16);
		e_op2(e, SLJIT_MUL, SLJIT_R2, operand, SLJIT_R1, 0);
		e_op2(e, SLJIT_ASHR, SLJIT_R2, SLJIT_R2, SLJIT_IMM, s->shift);
	}
	e_sext(e, SLJIT_R2);

	if (code & 4)
	{
		e_op2(e, SLJIT_SUB, SLJIT_R2, SLJIT_IMM, SLJIT_R2, 0);
		e_sext(e, SLJIT_R2);
	}

	if ((code & 8) && !(code & 0x40))
		e_mov(e, r->new[n], SLJIT_R2, 0);
	else
	{
		e_mov(e, SLJIT_R3, r->old[n], 0);
		e_saturate(e, SLJIT_R3, SLJIT_R1);
		e_add(e, r->new[n], SLJIT_R3, SLJIT_R2, 0);
	}
}

static void e_normal(lsp_emitter_t *e, const lsp_block_t *blk, const lsp_insn_t *s, const lsp_regs_t *r, const bool m[2])
{
	const bool immediate = is_immediate(s->offset);
	lsp_operand_t coefficient;

	if (s->store)
	{
		e_source(e, SLJIT_R0, SLJIT_R1, r->stored[s->store == 2], s->store);
		e_iram_store(e, s->offset, SLJIT_R0);
	}
	if (immediate)
		e_mov(e, SLJIT_R0, SLJIT_IMM, IMMEDIATE[s->offset]);
	else if (!s->store)
		e_load(e, SLJIT_R0, SLJIT_MEM1(R_IRAM), (sljit_sw)s->offset * 4);

	e_commit(e, blk, r, m);

	if (s->opcode == LSP_OP_MUL)
	{
		e_multiply(e, s, SLJIT_R0, r);
		return;
	}

	coefficient = e_coefficient(e, blk->pc, SLJIT_R1, false);
	if (coefficient.r == SLJIT_IMM && coefficient.w == 0)
	{
		const int n = s->opcode == LSP_OP_MAC_B || s->opcode == LSP_OP_SET_B;
		if (s->opcode == LSP_OP_MAC_A || s->opcode == LSP_OP_MAC_B)
		{
			if (r->new[n] != r->old[n])
				e_mov(e, r->new[n], r->old[n], 0);
		}
		else
			e_mov(e, r->new[n], SLJIT_IMM, 0);
		return;
	}
	if (immediate && coefficient.r == SLJIT_IMM)
	{
		coefficient.w = (int32_t)(((int64_t)IMMEDIATE[s->offset] * coefficient.w) >> s->shift);
		if (s->opcode == LSP_OP_ABS && coefficient.w < 0)
			coefficient.w = (int32_t)(0u - (uint32_t)coefficient.w);
		switch (s->opcode)
		{
		case LSP_OP_MAC_A: e_add(e, r->new[0], r->old[0], SLJIT_IMM, coefficient.w); break;
		case LSP_OP_MAC_B: e_add(e, r->new[1], r->old[1], SLJIT_IMM, coefficient.w); break;
		case LSP_OP_SET_B: e_mov(e, r->new[1], SLJIT_IMM, coefficient.w); break;
		default:           e_mov(e, r->new[0], SLJIT_IMM, coefficient.w); break;
		}
		return;
	}

	e_product(e, SLJIT_R1, SLJIT_R0, coefficient, s->shift);
	switch (s->opcode)
	{
	case LSP_OP_MAC_A: e_add(e, r->new[0], r->old[0], SLJIT_R1, 0); break;
	case LSP_OP_SET_A: e_mov(e, r->new[0], SLJIT_R1, 0); break;
	case LSP_OP_MAC_B: e_add(e, r->new[1], r->old[1], SLJIT_R1, 0); break;
	case LSP_OP_SET_B: e_mov(e, r->new[1], SLJIT_R1, 0); break;
	default:
	{
		struct sljit_jump *positive = e_cmp(e, SLJIT_SIG_GREATER_EQUAL, SLJIT_R1, SLJIT_IMM, 0);
		e_op2(e, SLJIT_SUB, SLJIT_R1, SLJIT_IMM, SLJIT_R1, 0);
		e_sext(e, SLJIT_R1);
		e_label(e, positive);
		e_mov(e, r->new[0], SLJIT_R1, 0);
		break;
	}
	}
}

/* R2 = the second-operand form's product: the previous slot's operand times the unsigned coefficient */
static void e_second_operand(lsp_emitter_t *e, const lsp_block_t *blk, const lsp_insn_t *s)
{
	const int prev = blk->in.prev_offset;
	lsp_operand_t coefficient = e_coefficient(e, blk->pc, SLJIT_R1, true);

	if (prev >= 0 && is_immediate(prev) && coefficient.r == SLJIT_IMM)
	{
		e_mov(e, SLJIT_R2, SLJIT_IMM, (int32_t)((((int64_t)IMMEDIATE[prev] * coefficient.w) >> 8) >> s->shift));
		return;
	}
	if (prev >= 0)
	{
		if (is_immediate(prev))
			e_mov(e, SLJIT_R2, SLJIT_IMM, IMMEDIATE[prev]);
		else
			e_load(e, SLJIT_R2, SLJIT_MEM1(R_IRAM), (sljit_sw)prev * 4);
	}
	else
	{
		struct sljit_jump *immediate, *done;
		e_load_u8(e, SLJIT_R0, SLJIT_MEM1(SLJIT_S0), CELLW(state.prev_offset));
		e_op2(e, SLJIT_SUB, SLJIT_R2, SLJIT_R0, SLJIT_IMM, 1);
		immediate = e_cmp(e, SLJIT_LESS, SLJIT_R2, SLJIT_IMM, 4);
		e_load(e, SLJIT_R2, SLJIT_MEM2(R_IRAM, SLJIT_R0), 2);
		done = e_jump(e);
		e_label(e, immediate);
		e_op2(e, SLJIT_MUL, SLJIT_R3, SLJIT_R0, SLJIT_IMM, 5);
		e_op2(e, SLJIT_ADD, SLJIT_R3, SLJIT_R3, SLJIT_IMM, 2);
		e_mov(e, SLJIT_R2, SLJIT_IMM, 1);
		e_op2(e, SLJIT_SHL, SLJIT_R2, SLJIT_R2, SLJIT_R3, 0);
		e_label(e, done);
	}
	e_op2(e, SLJIT_MUL, SLJIT_R2, SLJIT_R2, coefficient.r, coefficient.w);
	e_op2(e, SLJIT_ASHR, SLJIT_R2, SLJIT_R2, SLJIT_IMM, 8);
	e_op2(e, SLJIT_ASHR, SLJIT_R2, SLJIT_R2, SLJIT_IMM, s->shift);
	e_sext(e, SLJIT_R2);
}

static void e_tap_slot(lsp_emitter_t *e, const lsp_regs_t *r)
{
	e_mov(e, SLJIT_R0, r->stored[0], 0);
	e_op2(e, SLJIT_ASHR, SLJIT_R2, SLJIT_R0, SLJIT_IMM, 10);
	e_store_u16(e, CELLW(state.tap), SLJIT_R2, 0);
	e_op2(e, SLJIT_AND, SLJIT_R0, SLJIT_R0, SLJIT_IMM, 0x3ff);
	e_op2(e, SLJIT_SHL, SLJIT_R0, SLJIT_R0, SLJIT_IMM, 13);
	e_sext(e, SLJIT_R0);
	e_store(e, CELL(state.multiplier[0]), SLJIT_R0);
}

/* the branch condition a block ends on, tested against R0; 0 when it has one successor */
static sljit_s32 e_jump_slot(lsp_emitter_t *e, const lsp_insn_t *s, const lsp_regs_t *r)
{
	const bool negative = insn_slot(s) == LSP_SLOT_JUMP_NEGATIVE;
	if (jump_kind(s) != JUMP_CONDITIONAL)
		return 0;
	if (s->store == 3)
	{
		e_op2(e, SLJIT_AND, SLJIT_R0, r->stored[0], SLJIT_IMM, 0x800000);
		return negative ? SLJIT_NOT_EQUAL : SLJIT_EQUAL;
	}
	e_mov(e, SLJIT_R0, r->stored[s->store == 2], 0);
	return negative ? SLJIT_SIG_LESS : SLJIT_SIG_GREATER_EQUAL;
}

static sljit_s32 e_special(lsp_emitter_t *e, const lsp_block_t *blk, const lsp_insn_t *s, const lsp_regs_t *r, const bool m[2])
{
	const int slot = insn_slot(s);
	const int n = s->opcode & 1;
	lsp_operand_t coefficient;
	sljit_s32 condition = 0;
	struct sljit_jump *skip, *done;

	switch (slot)
	{
	case LSP_SLOT_JUMP_NEGATIVE:
	case LSP_SLOT_JUMP_POSITIVE:
	case LSP_SLOT_JUMP:
		condition = e_jump_slot(e, s, r);
		e_commit(e, blk, r, m);
		return condition;

	case LSP_SLOT_ERAM_WRITE:
		if (s->store)
		{
			e_source(e, SLJIT_R0, SLJIT_R1, r->stored[s->store == 2], s->store);
			e_store(e, CELL(state.eram_latch), SLJIT_R0);
			e_commit(e, blk, r, m);
			e_eram_index(e, SLJIT_R3, false);
			e_op2(e, SLJIT_ASHR, SLJIT_R0, SLJIT_R0, SLJIT_IMM, 4);
			e_store(e, SLJIT_MEM2(R_ERAM, SLJIT_R3), 2, SLJIT_R0);
			return 0;
		}
		e_second_operand(e, blk, s);
		e_commit(e, blk, r, m);
		if (insn_replace(s))
			e_mov(e, r->new[n], SLJIT_R2, 0);
		else
			e_add(e, r->new[n], r->old[n], SLJIT_R2, 0);
		return 0;

	case LSP_SLOT_TAP:
		if (s->store == 3)
			e_tap_slot(e, r);
		e_commit(e, blk, r, m);
		return 0;

	case LSP_SLOT_MULTIPLIER:
	case LSP_SLOT_MULTIPLIER + 1:
		e_value(e, SLJIT_R0, SLJIT_R1, r, s->store);
		e_store(e, SLJIT_MEM1(SLJIT_S0),
			(slot == LSP_SLOT_MULTIPLIER) ? CELLW(state.multiplier[0]) : CELLW(state.multiplier[1]), SLJIT_R0);
		e_commit(e, blk, r, m);
		return 0;

	case LSP_SLOT_AUDIO_OUT:
		e_value(e, SLJIT_R0, SLJIT_R1, r, s->store);
		e_store(e, CELL(audio_out), SLJIT_R0);
		if (blk->slot >= 0)
		{
			if (blk->slot < LSP_PROGRAM_SIZE / 2)
				e_store(e, CELL(serial_out[1]), SLJIT_R0);
		}
		else
		{
			e_load_u16(e, SLJIT_R1, CELLW(state.slot));
			skip = e_cmp(e, SLJIT_SIG_GREATER_EQUAL, SLJIT_R1, SLJIT_IMM, LSP_PROGRAM_SIZE / 2);
			e_store(e, CELL(serial_out[1]), SLJIT_R0);
			e_label(e, skip);
		}
		break;

	case LSP_SLOT_ERAM_READ:
	case LSP_SLOT_ERAM_READ + 1:
	case LSP_SLOT_ERAM_READ + 2:
	case LSP_SLOT_ERAM_READ + 3:
		if (s->store)
		{
			e_eram_index(e, SLJIT_R3, true);
			e_load(e, SLJIT_R0, SLJIT_MEM2(R_ERAM, SLJIT_R3), 2);
			e_op2(e, SLJIT_SHL, SLJIT_R0, SLJIT_R0, SLJIT_IMM, 4);
			e_sext(e, SLJIT_R0);
			e_store(e, CELL(state.eram_read), SLJIT_R0);
		}
		else
			e_load(e, SLJIT_R0, CELL(state.eram_read));
		break;

	case LSP_SLOT_AUDIO_IN:
		if (blk->slot >= 0)
			e_load(e, SLJIT_R0, SLJIT_MEM1(SLJIT_S0),
				blk->slot < LSP_PROGRAM_SIZE / 2 ? CELLW(serial_in[1]) : CELLW(serial_in[0]));
		else
		{
			e_load_u16(e, SLJIT_R1, CELLW(state.slot));
			skip = e_cmp(e, SLJIT_SIG_LESS, SLJIT_R1, SLJIT_IMM, LSP_PROGRAM_SIZE / 2);
			e_load(e, SLJIT_R0, CELL(serial_in[0]));
			done = e_jump(e);
			e_label(e, skip);
			e_load(e, SLJIT_R0, CELL(serial_in[1]));
			e_label(e, done);
		}
		break;

	default:
		e_commit(e, blk, r, m);
		return 0;
	}

	e_iram_store(e, 0x60 + slot, SLJIT_R0);
	coefficient = e_coefficient(e, blk->pc, SLJIT_R1, false);
	e_product(e, SLJIT_R1, SLJIT_R0, coefficient, s->shift);
	e_commit(e, blk, r, m);
	if (insn_replace(s))
		e_mov(e, r->new[n], SLJIT_R1, 0);
	else
		e_add(e, r->new[n], r->old[n], SLJIT_R1, 0);
	return 0;
}

/* ---- blocks */

/* the accumulator registers of an unrolled block at slot n, put into the order the shifting
 * form and the epilogue expect */
static void e_canonical(lsp_emitter_t *e, const lsp_block_t *blk, const lsp_ctx_t *out)
{
	for (int n = 0; n < 2; n++)
	{
		const sljit_s32 *a = ACC[n];
		if (out->age[n] >= 2)
			continue;
		switch (blk->slot % 3)
		{
		case 0:
			e_mov(e, SLJIT_R1, a[1], 0);
			e_mov(e, a[1], a[2], 0);
			e_mov(e, a[2], a[0], 0);
			e_mov(e, a[0], SLJIT_R1, 0);
			break;
		case 1:
			e_mov(e, SLJIT_R1, a[2], 0);
			e_mov(e, a[2], a[1], 0);
			e_mov(e, a[1], a[0], 0);
			e_mov(e, a[0], SLJIT_R1, 0);
			break;
		}
	}
}

static void add_patch(lsp_compile_t *c, struct sljit_jump *jump, int target)
{
	c->patches[c->patch_count].jump = jump;
	c->patches[c->patch_count++].target = target;
}

static void e_block(lsp_emitter_t *e, int index, int following)
{
	lsp_compile_t *c = e->c;
	lsp_block_t *blk = &c->blocks[index];
	const lsp_insn_t *s = &c->insn[blk->pc];
	const lsp_regs_t r = block_regs(blk);
	const bool known = blk->slot >= 0;
	lsp_ctx_t out;
	bool m[2];
	sljit_s32 condition = 0;
	bool successor_unknown_slot = false, successor_unknown_ring = false, successor_needs_prev = false;
	int fall;

	blk->label = sljit_emit_label(e->b.c);
	modifies(s, m);
	ctx_after(&blk->in, s, &out);

	e_clock(e, blk, s->command);
	if (is_special(s))
		condition = e_special(e, blk, s, &r, m);
	else if (s->present)
		e_normal(e, blk, s, &r, m);
	else
		e_commit(e, blk, &r, m);

	for (int k = 0; k < 2; k++)
	{
		const lsp_block_t *next = blk->next[k] >= 0 ? &c->blocks[blk->next[k]] : NULL;
		if (!next)
			continue;
		successor_unknown_slot |= next->slot < 0;
		successor_unknown_ring |= !ring_known(&next->in, 0, RING - 1);
		successor_needs_prev |= next->in.prev_offset < 0 && is_second_operand(&c->insn[next->pc]);
	}

	if (known && ring_known(&blk->in, 0, RING - 1) && (successor_unknown_ring || successor_unknown_slot))
		e_materialize_ring(e, blk, s->command);
	if (successor_needs_prev)
		e_store_u8(e, CELLW(state.prev_offset), SLJIT_IMM, s->offset);
	if (known && successor_unknown_slot)
	{
		e_store_u16(e, CELLW(state.slot), SLJIT_IMM, blk->slot + 1);
		e_canonical(e, blk, &out);
	}

	if (known && blk->slot == LSP_PROGRAM_SIZE - 1)
	{
		if (ring_known(&blk->in, 0, RING - 1))
			e_materialize_ring(e, blk, s->command);
		e_store_u8(e, CELLW(state.prev_offset), SLJIT_IMM, s->offset);
		add_patch(c, e_jump(e), -1);
		return;
	}

	if (!known)
	{
		e_load_u16(e, SLJIT_R1, CELLW(state.slot));
		e_op2(e, SLJIT_ADD, SLJIT_R1, SLJIT_R1, SLJIT_IMM, 1);
		e_store_u16(e, CELLW(state.slot), SLJIT_R1, 0);
		c->ends[c->end_count].jump = e_cmp(e, SLJIT_EQUAL, SLJIT_R1, SLJIT_IMM, LSP_PROGRAM_SIZE);
		c->ends[c->end_count++].target = s->offset;
	}

	fall = blk->next[0] >= 0 ? blk->next[0] : blk->next[1];
	if (blk->next[0] >= 0 && blk->next[1] >= 0)
		add_patch(c, e_cmp(e, condition, SLJIT_R0, SLJIT_IMM, 0), blk->next[1]);
	if (fall >= 0 && fall != following)
		add_patch(c, e_jump(e), fall);
}

static void e_ends(lsp_emitter_t *e)
{
	lsp_compile_t *c = e->c;
	for (int n = 0; n < c->end_count; n++)
	{
		if (!c->ends[n].jump)
			continue;
		e_label(e, c->ends[n].jump);
		for (int k = n + 1; k < c->end_count; k++)
			if (c->ends[k].jump && c->ends[k].target == c->ends[n].target)
			{
				sljit_set_label(c->ends[k].jump, sljit_emit_label(e->b.c));
				c->ends[k].jump = NULL;
			}
		e_store_u8(e, CELLW(state.prev_offset), SLJIT_IMM, c->ends[n].target);
		add_patch(c, e_jump(e), -1);
	}
}

static void e_prologue(lsp_emitter_t *e)
{
	sljit_emit_op1(e->b.c, SLJIT_MOV_P, R_ERAM, 0, CELL(eram));
	e_load_u8(e, SLJIT_R0, SLJIT_MEM1(SLJIT_S0), CELLW(state.buffer_pos));
	e_op2(e, SLJIT_SHL, SLJIT_R0, SLJIT_R0, SLJIT_IMM, 2);
	e_op2(e, SLJIT_ADD, R_IRAM, SLJIT_S0, SLJIT_R0, 0);
	e_op2(e, SLJIT_ADD, R_IRAM, R_IRAM, SLJIT_IMM, CELLW(iram));
	for (int n = 0; n < 2; n++)
		for (int k = 0; k < 3; k++)
			e_load(e, ACC[n][2 - k], SLJIT_MEM1(SLJIT_S0), CELLW(state.hist[n][k]));
}

static void e_epilogue(lsp_emitter_t *e)
{
	struct sljit_label *done = sljit_emit_label(e->b.c);
	for (int n = 0; n < e->c->patch_count; n++)
		if (e->c->patches[n].target < 0)
			sljit_set_label(e->c->patches[n].jump, done);
	for (int n = 0; n < 2; n++)
	{
		e_store(e, CELL(state.acc[n]), ACC[n][2]);
		for (int k = 0; k < 3; k++)
			e_store(e, SLJIT_MEM1(SLJIT_S0), CELLW(state.hist[n][k]), ACC[n][2 - k]);
	}
}

static bool compile_program(lsp_t *lsp)
{
	lsp_emitter_t e;
	jit_code_t *code = &lsp->code[lsp->live ^ 1];
	lsp_compile_t *c = malloc(sizeof(*c));

	if (!c)
		return false;
	e.lsp = lsp;
	e.c = c;
	c->patch_count = c->end_count = c->stub_count = 0;

	for (int pc = 0; pc < LSP_ADDRESSES; pc++)
		c->insn[pc] = decode(pc >= LSP_PROGRAM_BASE ? lsp->program[pc - LSP_PROGRAM_BASE] : 0);
	plan_blocks(c);
	plan_contexts(c);
	plan_order(c);

	jit_code_free(lsp->jit, code);
	if (!jit_begin(&e.b, lsp->jit, 7, 6))
	{
		free(c);
		return false;
	}

	e_prologue(&e);
	for (int n = 0; n < c->count; n++)
		e_block(&e, c->order[n], n + 1 < c->count ? c->order[n + 1] : -1);
	e_ends(&e);
	e_stubs(&e);
	e_epilogue(&e);
	for (int n = 0; n < c->patch_count; n++)
		if (c->patches[n].target >= 0)
			sljit_set_label(c->patches[n].jump, c->blocks[c->patches[n].target].label);

	lsp->blocks = (uint32_t)c->count;
	lsp->dynamic_blocks = 0;
	for (int n = 0; n < c->count; n++)
		lsp->dynamic_blocks += c->blocks[n].slot < 0;
	free(c);

	if (!jit_end(&e.b, lsp->jit, code))
		return false;
	memcpy(lsp->cell, lsp->patched, sizeof(lsp->cell));
	lsp->compiles++;
	lsp->live ^= 1;
	lsp->sample = (lsp_sample_fn)code->entry;
	return true;
}

#else

static bool compile_program(lsp_t *lsp)
{
	lsp->sample = NULL;
	return false;
}

#endif

void lsp_run_sample(lsp_t *lsp)
{
	if (!lsp->running)
		return;
	if (lsp->dirty)
	{
		lsp->dirty = false;
		compile_program(lsp);
	}
	if (lsp->sample)
		lsp->sample(lsp);
	lsp->serial_out[0] = lsp->audio_out;
	lsp->state.buffer_pos = (lsp->state.buffer_pos - 1) & 0x7f;
	lsp->state.eram_pos--;
}

/* ---------------------------------------------------------------- the state */

static bool state_restored(void *user)
{
	lsp_t *lsp = user;
	memset(lsp->iram_below, 0, sizeof(lsp->iram_below));
	memset(lsp->iram_above, 0, sizeof(lsp->iram_above));
	memcpy(lsp->iram_mirror, lsp->iram, sizeof(lsp->iram_mirror));
	lsp->dirty = true;
	return true;
}

void lsp_state(lsp_t *lsp, state_registry_t *reg)
{
	state_array(reg, lsp->program);
	state_array(reg, lsp->iram);
	state_array(reg, lsp->eram_cmd);

	lsp_state_t *s = &lsp->state;
	state_array(reg, s->acc);
	state_block(reg, &s->hist[0][0], 2 * 3);
	state_var(reg, s->eram_read);
	state_array(reg, s->multiplier);
	state_var(reg, s->eram_latch);
	state_array(reg, s->eram_base);
	state_var(reg, s->tap);
	state_var(reg, s->eram_pos);
	state_var(reg, s->slot);
	state_array(reg, s->eram_tap2);
	state_var(reg, s->prev_offset);
	state_var(reg, s->buffer_pos);

	state_var(reg, lsp->configuration);
	state_bool(reg, lsp->running);
	state_array(reg, lsp->patched);
	state_array(reg, lsp->serial_in);
	state_array(reg, lsp->serial_out);
	state_var(reg, lsp->audio_out);
	state_var(reg, lsp->host_data);
	state_var(reg, lsp->host_read);
	state_var(reg, lsp->host_address);
	state_block(reg, lsp->eram, LSP_ERAM_SIZE);
	state_after_load(reg, state_restored, lsp);
}
