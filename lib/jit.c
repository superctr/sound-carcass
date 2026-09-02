#include "jit.h"
#include "sljitLir.h"

void jit_alloc_init(jit_alloc_t *a, const scemu_config_t *config)
{
	a->config = config;
}

void jit_code_free(jit_alloc_t *a, jit_code_t *code)
{
	if (code->block)
		sljit_free_code(code->block, (void *)a->config);
	code->entry = NULL;
	code->block = NULL;
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
