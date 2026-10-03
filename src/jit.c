#include "jit.h"

void jit_alloc_init(jit_alloc_t *a, const scemu_config_t *config)
{
	a->config = config;
}

void jit_code_free(jit_alloc_t *a, jit_code_t *code)
{
	if (code->entry)
		sljit_free_code(code->entry, (void *)a->config);
	code->entry = NULL;
	code->size = 0;
}

void *scemu_exec_alloc(size_t size, void *allocator_data)
{
	const scemu_config_t *config = allocator_data;
	if (!config || !config->exec_alloc)
		return NULL;
	return config->exec_alloc(size, config->user);
}

void scemu_exec_free(void *block, void *allocator_data)
{
	const scemu_config_t *config = allocator_data;
	if (config && config->exec_free)
		config->exec_free(block, config->user);
}

bool jit_begin(jit_builder_t *b, jit_alloc_t *a, int scratches, int saveds)
{
	b->failed = false;
	b->c = sljit_create_compiler(NULL);
	if (!b->c)
	{
		b->failed = true;
		return false;
	}
	sljit_emit_enter(b->c, 0, SLJIT_ARGS1V(P), scratches, saveds, 0);
	return true;
}

void *jit_end(jit_builder_t *b, jit_alloc_t *a, jit_code_t *code)
{
	void *entry = NULL;
	if (!b->failed)
	{
		sljit_emit_return_void(b->c);
		entry = sljit_generate_code(b->c, 0, (void *)a->config);
	}
	if (b->c)
	{
		if (entry)
			code->size = sljit_get_generated_code_size(b->c);
		sljit_free_compiler(b->c);
	}
	b->c = NULL;
	code->entry = entry;
	if (!entry)
		code->size = 0;
	return entry;
}

void jit_load_s32(jit_builder_t *b, sljit_s32 reg, size_t offset)
{
	sljit_emit_op1(b->c, SLJIT_MOV_S32, reg, 0, JIT_WORD(offset));
}

void jit_store_s32(jit_builder_t *b, sljit_s32 reg, size_t offset)
{
	sljit_emit_op1(b->c, SLJIT_MOV32, JIT_WORD(offset), reg, 0);
}

void jit_load_imm(jit_builder_t *b, sljit_s32 reg, sljit_sw value)
{
	sljit_emit_op1(b->c, SLJIT_MOV, reg, 0, SLJIT_IMM, value);
}

void jit_clamp24(jit_builder_t *b, sljit_s32 reg, sljit_s32 tmp)
{
	struct sljit_jump *below, *above, *done;
	struct sljit_label *saturate;
	sljit_emit_op1(b->c, SLJIT_MOV, tmp, 0, SLJIT_IMM, -0x800000);
	below = sljit_emit_cmp(b->c, SLJIT_SIG_LESS, reg, 0, tmp, 0);
	sljit_emit_op1(b->c, SLJIT_MOV, tmp, 0, SLJIT_IMM, 0x7fffff);
	above = sljit_emit_cmp(b->c, SLJIT_SIG_GREATER, reg, 0, tmp, 0);
	done = sljit_emit_jump(b->c, SLJIT_JUMP);
	saturate = sljit_emit_label(b->c);
	sljit_set_label(below, saturate);
	sljit_set_label(above, saturate);
	sljit_emit_op1(b->c, SLJIT_MOV, reg, 0, tmp, 0);
	sljit_set_label(done, sljit_emit_label(b->c));
}

void jit_product(jit_builder_t *b, sljit_s32 reg, sljit_s32 coefficient)
{
	sljit_emit_op2(b->c, SLJIT_MUL, reg, 0, reg, 0, coefficient, 0);
	sljit_emit_op2(b->c, SLJIT_ADD, reg, 0, reg, 0, SLJIT_IMM, 0x1000);
	sljit_emit_op2(b->c, SLJIT_ASHR, reg, 0, reg, 0, SLJIT_IMM, 13);
}

void jit_call1(jit_builder_t *b, void (*fn)(void *), sljit_s32 arg)
{
	if (arg != SLJIT_R0)
		sljit_emit_op1(b->c, SLJIT_MOV, SLJIT_R0, 0, arg, 0);
	sljit_emit_icall(b->c, SLJIT_CALL, SLJIT_ARGS1V(P), SLJIT_IMM, SLJIT_FUNC_ADDR(fn));
}
