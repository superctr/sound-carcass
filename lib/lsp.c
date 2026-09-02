#include <stdlib.h>
#include <string.h>
#include "lsp.h"

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
	else
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

static bool compile_program(lsp_t *lsp)
{
	jit_builder_t b;
	jit_code_t *code = &lsp->code[lsp->live ^ 1];
	jit_code_free(lsp->jit, code);
	if (!jit_begin(&b, lsp->jit, 4, 2))
		return false;
	if (!jit_end(&b, lsp->jit, code))
		return false;
	lsp->live ^= 1;
	lsp->sample = (lsp_sample_fn)code->entry;
	return true;
}

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
