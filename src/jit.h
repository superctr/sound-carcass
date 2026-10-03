#ifndef SCEMU_JIT_H
#define SCEMU_JIT_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "scemu.h"
#include "sljitLir.h"

/* A compiled DSP program.  Two live per DSP: the one running and the one
 * being built, swapped at a frame boundary. */
typedef struct jit_code
{
	void *entry;
	size_t size;
} jit_code_t;

typedef struct jit_alloc
{
	const scemu_config_t *config;
} jit_alloc_t;

void jit_alloc_init(jit_alloc_t *a, const scemu_config_t *config);
void jit_code_free(jit_alloc_t *a, jit_code_t *code);

/* The allocator hooks sljit calls when the integrator supplies executable memory. */
void *scemu_exec_alloc(size_t size, void *allocator_data);
void scemu_exec_free(void *block, void *allocator_data);

/* The emitters' shared shape: a function of one pointer argument, the
 * device struct in S0, with 32-bit words addressed off it. */
typedef struct jit_builder
{
	struct sljit_compiler *c;
	bool failed;
} jit_builder_t;

bool jit_begin(jit_builder_t *b, jit_alloc_t *a, int scratches, int saveds);
void *jit_end(jit_builder_t *b, jit_alloc_t *a, jit_code_t *code);

#define JIT_DEVICE SLJIT_S0
#define JIT_WORD(offset) SLJIT_MEM1(JIT_DEVICE), (sljit_sw)(offset)

void jit_load_s32(jit_builder_t *b, sljit_s32 reg, size_t offset);
void jit_store_s32(jit_builder_t *b, sljit_s32 reg, size_t offset);
void jit_load_imm(jit_builder_t *b, sljit_s32 reg, sljit_sw value);

/* reg = clamp(reg, -2^23, 2^23 - 1) */
void jit_clamp24(jit_builder_t *b, sljit_s32 reg, sljit_s32 tmp);

/* reg = (reg * coefficient + 4096) >> 13, the XP's multiply */
void jit_product(jit_builder_t *b, sljit_s32 reg, sljit_s32 coefficient);

void jit_call1(jit_builder_t *b, void (*fn)(void *), sljit_s32 arg);

#endif
