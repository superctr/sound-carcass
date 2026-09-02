#include <stdlib.h>
#include <string.h>
#include "lsp.h"

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

uint8_t lsp_host_read(lsp_t *lsp, uint32_t offset)
{
	switch (offset & 0xf)
	{
	case 0x00: return lsp->host_read & 0xff;
	case 0x01: return (lsp->host_read >> 8) & 0xff;
	case 0x02: return (lsp->host_read >> 16) & 0xff;
	default: return 0;
	}
}

void lsp_host_write(lsp_t *lsp, uint32_t offset, uint8_t data)
{
	(void)lsp;
	(void)offset;
	(void)data;
}

void lsp_serial_write(lsp_t *lsp, int channel, int32_t sample)
{
	lsp->serial_in[channel & 1] = sample;
}

int32_t lsp_serial_read(const lsp_t *lsp, int channel)
{
	return lsp->serial_out[channel & 1];
}

void lsp_run_sample(lsp_t *lsp)
{
	if (!lsp->running)
		return;
	if (lsp->dirty)
	{
		lsp->dirty = false;
		lsp->sample = NULL;
	}
	if (lsp->sample)
		lsp->sample(lsp);
}
