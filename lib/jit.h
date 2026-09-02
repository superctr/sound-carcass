#ifndef SCEMU_JIT_H
#define SCEMU_JIT_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "scemu.h"

/* A compiled DSP program.  Two live at a time per DSP: the one running and the
 * one being built, swapped at a frame boundary. */
typedef struct jit_code
{
	void *entry;
	void *block;
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

#endif
