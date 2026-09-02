#ifndef SCEMU_LSP_H
#define SCEMU_LSP_H

#include <stdint.h>
#include <stdbool.h>
#include "jit.h"

/* Roland/Boss LSP (MB87837): the insertion-effect DSP, one 384-word program pass per sample. */

#define LSP_PROGRAM_SIZE 384
#define LSP_PROGRAM_BASE 0x080
#define LSP_ADDRESS_MASK 0x1ff
#define LSP_IRAM_SIZE 0x80
#define LSP_ERAM_SIZE 0x10000

struct lsp;
typedef void (*lsp_sample_fn)(struct lsp *lsp);

typedef struct lsp_state
{
	int32_t acc[2];
	int32_t history[2][3];
	int32_t eram_read;
	uint8_t prev_offset;
	int32_t multiplier[2];
	uint16_t eram_base[2];
	bool eram_tap2[2];
	int32_t eram_latch;
	uint16_t tap;
	uint8_t buffer_pos;
	uint16_t eram_pos;
} lsp_state_t;

typedef struct lsp
{
	uint32_t program[LSP_PROGRAM_SIZE];
	int32_t iram[LSP_IRAM_SIZE];
	int32_t *eram;
	uint8_t eram_cmd[16];
	lsp_state_t state;
	uint16_t configuration;
	bool running;
	bool dirty;

	int32_t serial_in[2];
	int32_t serial_out[2];
	int32_t audio_out;

	uint32_t host_data;
	uint32_t host_read;
	uint16_t host_address;

	jit_alloc_t *jit;
	jit_code_t code[2];
	int live;
	lsp_sample_fn sample;
} lsp_t;

bool lsp_init(lsp_t *lsp, jit_alloc_t *jit);
void lsp_release(lsp_t *lsp);
void lsp_reset(lsp_t *lsp);

uint8_t lsp_host_read(lsp_t *lsp, uint32_t offset);
void lsp_host_write(lsp_t *lsp, uint32_t offset, uint8_t data);

void lsp_serial_write(lsp_t *lsp, int channel, int32_t sample);
int32_t lsp_serial_read(const lsp_t *lsp, int channel);
void lsp_run_sample(lsp_t *lsp);

#endif
