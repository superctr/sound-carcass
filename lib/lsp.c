#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "lsp.h"

static const int32_t IMMEDIATE[5] = { 0, 1 << 7, 1 << 12, 1 << 17, 1 << 22 };

static inline int32_t narrow(int32_t v) { return (int32_t)((uint32_t)v << 8) >> 8; }

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
	memset(lsp->iram, 0, sizeof(lsp->iram));
	memset(lsp->eram, 0, LSP_ERAM_SIZE * sizeof(int32_t));
	memset(lsp->eram_cmd, 0, sizeof(lsp->eram_cmd));
	memset(&lsp->state, 0, sizeof(lsp->state));
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
		lsp->iram[address] = narrow((int32_t)data);
	else if (lsp->program[address - LSP_PROGRAM_BASE] != (data & 0xffffff))
	{
		lsp->program[address - LSP_PROGRAM_BASE] = data & 0xffffff;
		lsp->dirty = true;
	}
}

static void configure(lsp_t *lsp, uint16_t word)
{
	lsp->configuration = word;
	if ((word >> 12) & 1)
		lsp->running = false;
	else if (!lsp->running)
	{
		lsp->running = true;
		lsp->restart = true;
	}
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

/* ------------------------------------------------------------------ decode */

#define LSP_BLOCKS (LSP_ADDRESS_MASK + 1)

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

static lsp_insn_t block_insn(const lsp_t *lsp, int pc)
{
	return decode(pc >= LSP_PROGRAM_BASE ? lsp->program[pc - LSP_PROGRAM_BASE] : 0);
}

static bool is_special(const lsp_insn_t *s) { return s->present && s->opcode >= LSP_OP_SPECIAL_A; }
static int insn_slot(const lsp_insn_t *s) { return s->offset & 0x1f; }
static bool insn_replace(const lsp_insn_t *s) { return (s->offset >> 5) & 1; }

static bool is_jump(const lsp_insn_t *s)
{
	const int slot = insn_slot(s);
	if (!is_special(s) || slot < LSP_SLOT_JUMP_NEGATIVE || slot > LSP_SLOT_JUMP)
		return false;
	return !(slot == LSP_SLOT_JUMP_NEGATIVE && !s->store);
}

#if (defined SLJIT_64BIT_ARCHITECTURE && SLJIT_64BIT_ARCHITECTURE)

/* ------------------------------------------------------------------ emit */

#define CELLW(field) ((sljit_sw)offsetof(lsp_t, field))
#define CELL(field) SLJIT_MEM1(SLJIT_S0), CELLW(field)

#define R_ERAM SLJIT_S1
#define R_ACC0 SLJIT_S2
#define R_ACC1 SLJIT_S3
#define R_SLOT SLJIT_S4
#define R_BUF  SLJIT_S5

#define IRAM_BASE ((sljit_sw)(offsetof(lsp_t, iram) / 4))
#define HIST_BASE ((sljit_sw)(offsetof(lsp_t, state.hist) / 4))
#define CMD_BASE  ((sljit_sw)offsetof(lsp_t, eram_cmd))

static void e_sext(jit_builder_t *b, sljit_s32 reg)
{
	sljit_emit_op1(b->c, SLJIT_MOV_S32, reg, 0, reg, 0);
}

static void e_mov(jit_builder_t *b, sljit_s32 dst, sljit_s32 src, sljit_sw srcw)
{
	sljit_emit_op1(b->c, SLJIT_MOV, dst, 0, src, srcw);
}

static void e_load(jit_builder_t *b, sljit_s32 dst, sljit_s32 mem, sljit_sw memw)
{
	sljit_emit_op1(b->c, SLJIT_MOV_S32, dst, 0, mem, memw);
}

static void e_store(jit_builder_t *b, sljit_s32 mem, sljit_sw memw, sljit_s32 src)
{
	sljit_emit_op1(b->c, SLJIT_MOV32, mem, memw, src, 0);
}

static void e_op2(jit_builder_t *b, sljit_s32 op, sljit_s32 dst, sljit_s32 a, sljit_s32 src, sljit_sw srcw)
{
	sljit_emit_op2(b->c, op, dst, 0, a, 0, src, srcw);
}

/* dst = s32(a + b) */
static void e_add(jit_builder_t *b, sljit_s32 dst, sljit_s32 a, sljit_s32 src, sljit_sw srcw)
{
	e_op2(b, SLJIT_ADD, dst, a, src, srcw);
	e_sext(b, dst);
}

/* reg = s32(reg << 8) >> 8 */
static void e_narrow(jit_builder_t *b, sljit_s32 reg)
{
	e_op2(b, SLJIT_SHL, reg, reg, SLJIT_IMM, 8);
	e_sext(b, reg);
	e_op2(b, SLJIT_ASHR, reg, reg, SLJIT_IMM, 8);
}

/* dst = s32((s64)src * coefficient >> shift) */
static void e_product(jit_builder_t *b, sljit_s32 dst, sljit_s32 src, int coefficient, int shift)
{
	e_op2(b, SLJIT_MUL, dst, src, SLJIT_IMM, coefficient);
	e_op2(b, SLJIT_ASHR, dst, dst, SLJIT_IMM, shift);
	e_sext(b, dst);
}

/* dst = ((offset + buffer_pos) & 0x7f) + the internal RAM word base */
static void e_iram_index(jit_builder_t *b, sljit_s32 dst, int offset)
{
	e_op2(b, SLJIT_ADD, dst, R_BUF, SLJIT_IMM, offset);
	e_op2(b, SLJIT_AND, dst, dst, SLJIT_IMM, 0x7f);
	e_op2(b, SLJIT_ADD, dst, dst, SLJIT_IMM, IRAM_BASE);
}

/* dst = ((slot + 1) & 3) + the history word base of accumulator `which` */
static void e_hist_index(jit_builder_t *b, sljit_s32 dst, int which)
{
	e_op2(b, SLJIT_ADD, dst, R_SLOT, SLJIT_IMM, 1);
	e_op2(b, SLJIT_AND, dst, dst, SLJIT_IMM, 3);
	e_op2(b, SLJIT_ADD, dst, dst, SLJIT_IMM, HIST_BASE + which * 4);
}

static void e_source(jit_builder_t *b, sljit_s32 dst, sljit_s32 tmp, int store)
{
	e_hist_index(b, tmp, store == 2 ? 1 : 0);
	e_load(b, dst, SLJIT_MEM2(SLJIT_S0, tmp), 2);
	if (store == 3)
		e_narrow(b, dst);
	else
		jit_clamp24(b, dst, tmp);
}

static void e_value(jit_builder_t *b, sljit_s32 dst, sljit_s32 tmp, int store)
{
	if (store)
		e_source(b, dst, tmp, store);
	else
		e_mov(b, dst, SLJIT_IMM, 0);
}

/* dst = eram_cmd[(index - back) & 15] */
static void e_ring_load(jit_builder_t *b, sljit_s32 dst, sljit_s32 index, int back)
{
	e_op2(b, SLJIT_SUB, SLJIT_R4, index, SLJIT_IMM, back);
	e_op2(b, SLJIT_AND, SLJIT_R4, SLJIT_R4, SLJIT_IMM, 15);
	e_op2(b, SLJIT_ADD, SLJIT_R4, SLJIT_R4, SLJIT_IMM, CMD_BASE);
	sljit_emit_op1(b->c, SLJIT_MOV_U8, dst, 0, SLJIT_MEM2(SLJIT_S0, SLJIT_R4), 0);
}

/* R2 = the sixteen-bit base the six ring entries below `delay` assemble */
static void e_ring_base(jit_builder_t *b, sljit_s32 index, int delay)
{
	struct sljit_jump *skip;
	e_mov(b, SLJIT_R2, SLJIT_IMM, 0);
	for (int n = 0; n < 5; n++)
	{
		e_ring_load(b, SLJIT_R3, index, delay - 1 - n);
		if (n)
			e_op2(b, SLJIT_SHL, SLJIT_R3, SLJIT_R3, SLJIT_IMM, n * 3);
		e_op2(b, SLJIT_ADD, SLJIT_R2, SLJIT_R2, SLJIT_R3, 0);
	}
	e_ring_load(b, SLJIT_R3, index, delay - 6);
	e_op2(b, SLJIT_AND, SLJIT_R1, SLJIT_R3, SLJIT_IMM, 1);
	skip = sljit_emit_cmp(b->c, SLJIT_EQUAL, SLJIT_R1, 0, SLJIT_IMM, 0);
	e_op2(b, SLJIT_SHL, SLJIT_R3, SLJIT_R3, SLJIT_IMM, 15);
	e_op2(b, SLJIT_ADD, SLJIT_R2, SLJIT_R2, SLJIT_R3, 0);
	sljit_set_label(skip, sljit_emit_label(b->c));
	e_op2(b, SLJIT_AND, SLJIT_R2, SLJIT_R2, SLJIT_IMM, 0xffff);
}

static void e_eram_clock(jit_builder_t *b, int command)
{
	struct sljit_jump *skip, *other, *done;

	e_op2(b, SLJIT_AND, SLJIT_R0, R_SLOT, SLJIT_IMM, 15);
	e_op2(b, SLJIT_ADD, SLJIT_R1, SLJIT_R0, SLJIT_IMM, CMD_BASE);
	sljit_emit_op1(b->c, SLJIT_MOV_U8, SLJIT_MEM2(SLJIT_S0, SLJIT_R1), 0, SLJIT_IMM, command);

	e_ring_load(b, SLJIT_R1, SLJIT_R0, 8);
	e_op2(b, SLJIT_LSHR, SLJIT_R1, SLJIT_R1, SLJIT_IMM, 1);
	e_op2(b, SLJIT_AND, SLJIT_R1, SLJIT_R1, SLJIT_IMM, 3);
	skip = sljit_emit_cmp(b->c, SLJIT_NOT_EQUAL, SLJIT_R1, 0, SLJIT_IMM, 3);
	e_ring_base(b, SLJIT_R0, 8);
	sljit_emit_op1(b->c, SLJIT_MOV_U16, CELL(state.eram_base[0]), SLJIT_R2, 0);
	sljit_emit_op1(b->c, SLJIT_MOV_U8, CELL(state.eram_tap2[0]), SLJIT_IMM, 0);
	sljit_set_label(skip, sljit_emit_label(b->c));

	e_ring_load(b, SLJIT_R1, SLJIT_R0, 12);
	e_op2(b, SLJIT_LSHR, SLJIT_R1, SLJIT_R1, SLJIT_IMM, 1);
	e_op2(b, SLJIT_AND, SLJIT_R1, SLJIT_R1, SLJIT_IMM, 3);
	other = sljit_emit_cmp(b->c, SLJIT_EQUAL, SLJIT_R1, 0, SLJIT_IMM, 2);
	skip = sljit_emit_cmp(b->c, SLJIT_NOT_EQUAL, SLJIT_R1, 0, SLJIT_IMM, 1);
	e_ring_base(b, SLJIT_R0, 12);
	sljit_emit_op1(b->c, SLJIT_MOV_U16, CELL(state.eram_base[1]), SLJIT_R2, 0);
	sljit_emit_op1(b->c, SLJIT_MOV_U8, CELL(state.eram_tap2[1]), SLJIT_IMM, 0);
	done = sljit_emit_jump(b->c, SLJIT_JUMP);

	sljit_set_label(other, sljit_emit_label(b->c));
	e_ring_load(b, SLJIT_R1, SLJIT_R0, 11);
	e_mov(b, SLJIT_R2, SLJIT_IMM, 0);
	other = sljit_emit_cmp(b->c, SLJIT_NOT_EQUAL, SLJIT_R1, 0, SLJIT_IMM, 2);
	e_mov(b, SLJIT_R2, SLJIT_IMM, 1);
	sljit_set_label(other, sljit_emit_label(b->c));
	sljit_emit_op1(b->c, SLJIT_MOV_U16, CELL(state.eram_base[1]), SLJIT_R2, 0);
	sljit_emit_op1(b->c, SLJIT_MOV_U8, CELL(state.eram_tap2[1]), SLJIT_IMM, 1);

	sljit_set_label(skip, sljit_emit_label(b->c));
	sljit_set_label(done, sljit_emit_label(b->c));
}

/* dst = (eram_pos + eram_base[read] + tap) & 0xffff */
static void e_eram_index(jit_builder_t *b, sljit_s32 dst, bool read)
{
	sljit_emit_op1(b->c, SLJIT_MOV_U16, dst, 0, CELL(state.eram_pos));
	sljit_emit_op1(b->c, SLJIT_MOV_U16, SLJIT_R2, 0, SLJIT_MEM1(SLJIT_S0),
		read ? CELLW(state.eram_base[1]) : CELLW(state.eram_base[0]));
	e_op2(b, SLJIT_ADD, dst, dst, SLJIT_R2, 0);
	if (read)
	{
		struct sljit_jump *skip;
		sljit_emit_op1(b->c, SLJIT_MOV_U8, SLJIT_R2, 0, CELL(state.eram_tap2[1]));
		skip = sljit_emit_cmp(b->c, SLJIT_EQUAL, SLJIT_R2, 0, SLJIT_IMM, 0);
		sljit_emit_op1(b->c, SLJIT_MOV_U16, SLJIT_R2, 0, CELL(state.tap));
		e_op2(b, SLJIT_ADD, dst, dst, SLJIT_R2, 0);
		sljit_set_label(skip, sljit_emit_label(b->c));
	}
	e_op2(b, SLJIT_AND, dst, dst, SLJIT_IMM, 0xffff);
}

static void e_multiply(jit_builder_t *b, const lsp_insn_t *s, sljit_s32 operand)
{
	const int code = s->coefficient;
	const sljit_s32 acc = (code & 0x10) ? R_ACC1 : R_ACC0;

	e_load(b, SLJIT_R1, SLJIT_MEM1(SLJIT_S0), (code & 2) ? CELLW(state.multiplier[1]) : CELLW(state.multiplier[0]));
	if (code & 0x40)
	{
		e_op2(b, SLJIT_AND, SLJIT_R1, SLJIT_R1, SLJIT_IMM, 0xffff);
		e_op2(b, SLJIT_LSHR, SLJIT_R1, SLJIT_R1, SLJIT_IMM, 9);
		e_op2(b, SLJIT_MUL, SLJIT_R2, operand, SLJIT_R1, 0);
		e_op2(b, SLJIT_ASHR, SLJIT_R2, SLJIT_R2, SLJIT_IMM, s->shift);
		e_op2(b, SLJIT_ASHR, SLJIT_R2, SLJIT_R2, SLJIT_IMM, 7);
	}
	else
	{
		e_op2(b, SLJIT_ASHR, SLJIT_R1, SLJIT_R1, SLJIT_IMM, 16);
		e_op2(b, SLJIT_MUL, SLJIT_R2, operand, SLJIT_R1, 0);
		e_op2(b, SLJIT_ASHR, SLJIT_R2, SLJIT_R2, SLJIT_IMM, s->shift);
	}
	e_sext(b, SLJIT_R2);

	if (code & 4)
	{
		sljit_emit_op2(b->c, SLJIT_SUB, SLJIT_R2, 0, SLJIT_IMM, 0, SLJIT_R2, 0);
		e_sext(b, SLJIT_R2);
	}

	if ((code & 8) && !(code & 0x40))
		e_mov(b, acc, SLJIT_R2, 0);
	else
	{
		e_mov(b, SLJIT_R3, acc, 0);
		jit_clamp24(b, SLJIT_R3, SLJIT_R4);
		e_add(b, acc, SLJIT_R3, SLJIT_R2, 0);
	}
}

static void e_normal(jit_builder_t *b, const lsp_insn_t *s)
{
	const bool immediate = s->offset >= 1 && s->offset <= 4;

	if (s->store)
	{
		e_iram_index(b, SLJIT_R3, s->offset);
		e_source(b, SLJIT_R0, SLJIT_R1, s->store);
		e_store(b, SLJIT_MEM2(SLJIT_S0, SLJIT_R3), 2, SLJIT_R0);
	}

	if (immediate)
		e_mov(b, SLJIT_R0, SLJIT_IMM, IMMEDIATE[s->offset]);
	else
	{
		if (!s->store)
			e_iram_index(b, SLJIT_R3, s->offset);
		e_load(b, SLJIT_R0, SLJIT_MEM2(SLJIT_S0, SLJIT_R3), 2);
	}

	if (s->opcode == LSP_OP_MUL)
	{
		e_multiply(b, s, SLJIT_R0);
		return;
	}

	e_product(b, SLJIT_R1, SLJIT_R0, (int8_t)s->coefficient, s->shift);
	switch (s->opcode)
	{
	case LSP_OP_MAC_A: e_add(b, R_ACC0, R_ACC0, SLJIT_R1, 0); break;
	case LSP_OP_SET_A: e_mov(b, R_ACC0, SLJIT_R1, 0); break;
	case LSP_OP_MAC_B: e_add(b, R_ACC1, R_ACC1, SLJIT_R1, 0); break;
	case LSP_OP_SET_B: e_mov(b, R_ACC1, SLJIT_R1, 0); break;
	default:
	{
		struct sljit_jump *positive = sljit_emit_cmp(b->c, SLJIT_SIG_GREATER_EQUAL, SLJIT_R1, 0, SLJIT_IMM, 0);
		sljit_emit_op2(b->c, SLJIT_SUB, SLJIT_R1, 0, SLJIT_IMM, 0, SLJIT_R1, 0);
		e_sext(b, SLJIT_R1);
		sljit_set_label(positive, sljit_emit_label(b->c));
		e_mov(b, R_ACC0, SLJIT_R1, 0);
		break;
	}
	}
}

static void e_jump_slot(jit_builder_t *b, const lsp_insn_t *s, int slot)
{
	struct sljit_jump *skip = NULL;

	if (slot != LSP_SLOT_JUMP)
	{
		if (!s->store)
		{
			if (slot == LSP_SLOT_JUMP_NEGATIVE)
				return;
		}
		else
		{
			e_source(b, SLJIT_R0, SLJIT_R1, s->store);
			skip = sljit_emit_cmp(b->c,
				slot == LSP_SLOT_JUMP_NEGATIVE ? SLJIT_SIG_GREATER_EQUAL : SLJIT_SIG_LESS,
				SLJIT_R0, 0, SLJIT_IMM, 0);
		}
	}

	sljit_emit_op1(b->c, SLJIT_MOV_U8, CELL(state.jump_delay), SLJIT_IMM, 2);
	if (skip)
		sljit_set_label(skip, sljit_emit_label(b->c));
}

static void e_eram_write_slot(jit_builder_t *b, const lsp_insn_t *s)
{
	struct sljit_jump *immediate, *done;
	const sljit_s32 acc = (s->opcode & 1) ? R_ACC1 : R_ACC0;

	if (s->store)
	{
		e_source(b, SLJIT_R0, SLJIT_R1, s->store);
		e_store(b, CELL(state.eram_latch), SLJIT_R0);
		return;
	}

	sljit_emit_op1(b->c, SLJIT_MOV_U8, SLJIT_R1, 0, CELL(state.prev_offset));
	e_op2(b, SLJIT_SUB, SLJIT_R2, SLJIT_R1, SLJIT_IMM, 1);
	immediate = sljit_emit_cmp(b->c, SLJIT_LESS, SLJIT_R2, 0, SLJIT_IMM, 4);

	e_op2(b, SLJIT_ADD, SLJIT_R3, R_BUF, SLJIT_R1, 0);
	e_op2(b, SLJIT_AND, SLJIT_R3, SLJIT_R3, SLJIT_IMM, 0x7f);
	e_op2(b, SLJIT_ADD, SLJIT_R3, SLJIT_R3, SLJIT_IMM, IRAM_BASE);
	e_load(b, SLJIT_R2, SLJIT_MEM2(SLJIT_S0, SLJIT_R3), 2);
	done = sljit_emit_jump(b->c, SLJIT_JUMP);

	sljit_set_label(immediate, sljit_emit_label(b->c));
	e_op2(b, SLJIT_MUL, SLJIT_R3, SLJIT_R1, SLJIT_IMM, 5);
	e_op2(b, SLJIT_ADD, SLJIT_R3, SLJIT_R3, SLJIT_IMM, 2);
	e_mov(b, SLJIT_R2, SLJIT_IMM, 1);
	e_op2(b, SLJIT_SHL, SLJIT_R2, SLJIT_R2, SLJIT_R3, 0);
	sljit_set_label(done, sljit_emit_label(b->c));

	e_op2(b, SLJIT_MUL, SLJIT_R2, SLJIT_R2, SLJIT_IMM, s->coefficient);
	e_op2(b, SLJIT_ASHR, SLJIT_R2, SLJIT_R2, SLJIT_IMM, 8);
	e_op2(b, SLJIT_ASHR, SLJIT_R2, SLJIT_R2, SLJIT_IMM, s->shift);
	e_sext(b, SLJIT_R2);

	if (insn_replace(s))
		e_mov(b, acc, SLJIT_R2, 0);
	else
		e_add(b, acc, acc, SLJIT_R2, 0);
}

static void e_tap_slot(jit_builder_t *b, const lsp_insn_t *s)
{
	if (s->store != 3)
		return;
	e_hist_index(b, SLJIT_R1, 0);
	e_load(b, SLJIT_R0, SLJIT_MEM2(SLJIT_S0, SLJIT_R1), 2);
	e_op2(b, SLJIT_ASHR, SLJIT_R2, SLJIT_R0, SLJIT_IMM, 10);
	sljit_emit_op1(b->c, SLJIT_MOV_U16, CELL(state.tap), SLJIT_R2, 0);
	e_op2(b, SLJIT_AND, SLJIT_R0, SLJIT_R0, SLJIT_IMM, 0x3ff);
	e_op2(b, SLJIT_SHL, SLJIT_R0, SLJIT_R0, SLJIT_IMM, 13);
	e_sext(b, SLJIT_R0);
	e_store(b, CELL(state.multiplier[0]), SLJIT_R0);
}

static void e_special(jit_builder_t *b, const lsp_insn_t *s)
{
	const int slot = insn_slot(s);
	const sljit_s32 acc = (s->opcode & 1) ? R_ACC1 : R_ACC0;
	struct sljit_jump *skip, *done;

	switch (slot)
	{
	case LSP_SLOT_JUMP_NEGATIVE:
	case LSP_SLOT_JUMP_POSITIVE:
	case LSP_SLOT_JUMP:
		e_jump_slot(b, s, slot);
		return;

	case LSP_SLOT_ERAM_WRITE:
		e_eram_write_slot(b, s);
		return;

	case LSP_SLOT_TAP:
		e_tap_slot(b, s);
		return;

	case LSP_SLOT_MULTIPLIER:
	case LSP_SLOT_MULTIPLIER + 1:
		e_value(b, SLJIT_R0, SLJIT_R1, s->store);
		e_store(b, SLJIT_MEM1(SLJIT_S0),
			(slot == LSP_SLOT_MULTIPLIER) ? CELLW(state.multiplier[0]) : CELLW(state.multiplier[1]), SLJIT_R0);
		return;

	case LSP_SLOT_AUDIO_OUT:
		e_value(b, SLJIT_R0, SLJIT_R1, s->store);
		e_store(b, CELL(audio_out), SLJIT_R0);
		skip = sljit_emit_cmp(b->c, SLJIT_SIG_GREATER_EQUAL, R_SLOT, 0, SLJIT_IMM, LSP_PROGRAM_SIZE / 2);
		e_store(b, CELL(serial_out[1]), SLJIT_R0);
		sljit_set_label(skip, sljit_emit_label(b->c));
		break;

	case LSP_SLOT_ERAM_READ:
	case LSP_SLOT_ERAM_READ + 1:
	case LSP_SLOT_ERAM_READ + 2:
	case LSP_SLOT_ERAM_READ + 3:
		if (s->store)
		{
			e_eram_index(b, SLJIT_R3, true);
			e_load(b, SLJIT_R0, SLJIT_MEM2(R_ERAM, SLJIT_R3), 2);
			e_op2(b, SLJIT_SHL, SLJIT_R0, SLJIT_R0, SLJIT_IMM, 4);
			e_sext(b, SLJIT_R0);
			e_store(b, CELL(state.eram_read), SLJIT_R0);
		}
		else
			e_load(b, SLJIT_R0, CELL(state.eram_read));
		break;

	case LSP_SLOT_AUDIO_IN:
		skip = sljit_emit_cmp(b->c, SLJIT_SIG_LESS, R_SLOT, 0, SLJIT_IMM, LSP_PROGRAM_SIZE / 2);
		e_load(b, SLJIT_R0, CELL(serial_in[0]));
		done = sljit_emit_jump(b->c, SLJIT_JUMP);
		sljit_set_label(skip, sljit_emit_label(b->c));
		e_load(b, SLJIT_R0, CELL(serial_in[1]));
		sljit_set_label(done, sljit_emit_label(b->c));
		break;

	default:
		return;
	}

	e_iram_index(b, SLJIT_R3, 0x60 + slot);
	e_store(b, SLJIT_MEM2(SLJIT_S0, SLJIT_R3), 2, SLJIT_R0);
	e_product(b, SLJIT_R1, SLJIT_R0, (int8_t)s->coefficient, s->shift);
	if (insn_replace(s))
		e_mov(b, acc, SLJIT_R1, 0);
	else
		e_add(b, acc, acc, SLJIT_R1, 0);
}

static void e_hist_push(jit_builder_t *b)
{
	e_op2(b, SLJIT_AND, SLJIT_R0, R_SLOT, SLJIT_IMM, 3);
	e_op2(b, SLJIT_ADD, SLJIT_R0, SLJIT_R0, SLJIT_IMM, HIST_BASE);
	e_store(b, SLJIT_MEM2(SLJIT_S0, SLJIT_R0), 2, R_ACC0);
	e_op2(b, SLJIT_ADD, SLJIT_R0, SLJIT_R0, SLJIT_IMM, 4);
	e_store(b, SLJIT_MEM2(SLJIT_S0, SLJIT_R0), 2, R_ACC1);
}

static void e_eram_commit(jit_builder_t *b)
{
	e_eram_index(b, SLJIT_R3, false);
	e_load(b, SLJIT_R0, CELL(state.eram_latch));
	e_op2(b, SLJIT_ASHR, SLJIT_R0, SLJIT_R0, SLJIT_IMM, 4);
	e_store(b, SLJIT_MEM2(R_ERAM, SLJIT_R3), 2, SLJIT_R0);
}

typedef struct lsp_patch
{
	struct sljit_jump *jump;
	int target;
} lsp_patch_t;

static bool compile_program(lsp_t *lsp)
{
	jit_builder_t b;
	jit_code_t *code = &lsp->code[lsp->live ^ 1];
	struct sljit_label **labels;
	lsp_patch_t *patches;
	int patch_count = 0;
	lsp_insn_t insn[LSP_BLOCKS];
	uint8_t delay_check[LSP_BLOCKS], delay_fire[LSP_BLOCKS];
	uint16_t fire_target[LSP_BLOCKS];

	for (int pc = 0; pc < LSP_BLOCKS; pc++)
		insn[pc] = block_insn(lsp, pc);
	for (int pc = 0; pc < LSP_BLOCKS; pc++)
	{
		const int back = (pc - 1) & LSP_ADDRESS_MASK;
		delay_fire[pc] = is_jump(&insn[back]);
		delay_check[pc] = delay_fire[pc] || is_jump(&insn[pc]);
		fire_target[pc] = (uint16_t)((insn[back].coefficient << 1) & LSP_ADDRESS_MASK);
	}

	labels = malloc(LSP_BLOCKS * sizeof *labels);
	patches = malloc((2 * LSP_BLOCKS + 4) * sizeof *patches);
	if (!labels || !patches)
	{
		free(labels);
		free(patches);
		return false;
	}

	jit_code_free(lsp->jit, code);
	if (!jit_begin(&b, lsp->jit, 5, 6))
	{
		free(labels);
		free(patches);
		return false;
	}

	sljit_emit_op1(b.c, SLJIT_MOV_P, R_ERAM, 0, CELL(eram));
	e_load(&b, R_ACC0, CELL(state.acc[0]));
	e_load(&b, R_ACC1, CELL(state.acc[1]));
	e_mov(&b, R_SLOT, SLJIT_IMM, 0);
	sljit_emit_op1(b.c, SLJIT_MOV_U8, R_BUF, 0, CELL(state.buffer_pos));
	sljit_emit_op1(b.c, SLJIT_MOV_U8, CELL(state.jump_delay), SLJIT_IMM, 0);
	patches[patch_count].jump = sljit_emit_jump(b.c, SLJIT_JUMP);
	patches[patch_count++].target = LSP_PROGRAM_BASE;

	for (int pc = 0; pc < LSP_BLOCKS; pc++)
	{
		const lsp_insn_t *s = &insn[pc];
		struct sljit_jump *finished, *skip;

		labels[pc] = sljit_emit_label(b.c);

		e_eram_clock(&b, s->command);
		if (is_special(s))
			e_special(&b, s);
		else if (s->present)
			e_normal(&b, s);
		e_hist_push(&b);
		sljit_emit_op1(b.c, SLJIT_MOV_U8, CELL(state.prev_offset), SLJIT_IMM, s->offset);
		if (is_special(s) && insn_slot(s) == LSP_SLOT_ERAM_WRITE && s->store)
			e_eram_commit(&b);

		e_op2(&b, SLJIT_ADD, R_SLOT, R_SLOT, SLJIT_IMM, 1);
		finished = sljit_emit_cmp(b.c, SLJIT_EQUAL, R_SLOT, 0, SLJIT_IMM, LSP_PROGRAM_SIZE);
		patches[patch_count].jump = finished;
		patches[patch_count++].target = -1;

		if (delay_check[pc])
		{
			sljit_emit_op1(b.c, SLJIT_MOV_U8, SLJIT_R0, 0, CELL(state.jump_delay));
			skip = sljit_emit_cmp(b.c, SLJIT_EQUAL, SLJIT_R0, 0, SLJIT_IMM, 0);
			e_op2(&b, SLJIT_SUB, SLJIT_R0, SLJIT_R0, SLJIT_IMM, 1);
			sljit_emit_op1(b.c, SLJIT_MOV_U8, CELL(state.jump_delay), SLJIT_R0, 0);
			if (delay_fire[pc])
			{
				patches[patch_count].jump = sljit_emit_cmp(b.c, SLJIT_EQUAL, SLJIT_R0, 0, SLJIT_IMM, 0);
				patches[patch_count++].target = fire_target[pc];
			}
			sljit_set_label(skip, sljit_emit_label(b.c));
		}

		if (pc == LSP_ADDRESS_MASK)
		{
			patches[patch_count].jump = sljit_emit_jump(b.c, SLJIT_JUMP);
			patches[patch_count++].target = 0;
		}
	}

	{
		struct sljit_label *done = sljit_emit_label(b.c);
		e_store(&b, CELL(state.acc[0]), R_ACC0);
		e_store(&b, CELL(state.acc[1]), R_ACC1);
		for (int n = 0; n < patch_count; n++)
			sljit_set_label(patches[n].jump, patches[n].target < 0 ? done : labels[patches[n].target]);
	}

	free(labels);
	free(patches);

	if (!jit_end(&b, lsp->jit, code))
		return false;
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
	if (lsp->restart)
	{
		lsp->restart = false;
		lsp->state.jump_delay = 0;
	}
	if (lsp->sample)
		lsp->sample(lsp);
	lsp->serial_out[0] = lsp->audio_out;
	lsp->state.buffer_pos = (lsp->state.buffer_pos - 1) & 0x7f;
	lsp->state.eram_pos--;
}
