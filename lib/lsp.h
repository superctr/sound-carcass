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

enum
{
	LSP_SLOT_JUMP_NEGATIVE = 0x0d, LSP_SLOT_JUMP_POSITIVE = 0x0e, LSP_SLOT_JUMP = 0x0f,
	LSP_SLOT_ERAM_WRITE = 0x10, LSP_SLOT_TAP = 0x13, LSP_SLOT_MULTIPLIER = 0x14,
	LSP_SLOT_AUDIO_OUT = 0x18, LSP_SLOT_ERAM_READ = 0x1a, LSP_SLOT_AUDIO_IN = 0x1e
};

enum
{
	LSP_HOST_ADDRESS_LOW = 0x00, LSP_HOST_ADDRESS_HIGH = 0x01, LSP_HOST_DATA_LOW = 0x02,
	LSP_HOST_DATA_MID = 0x03, LSP_HOST_DATA_HIGH = 0x04, LSP_HOST_CONFIGURE = 0x06,
	LSP_HOST_READ_LOW = 0x08, LSP_HOST_READ_HIGH = 0x09
};

enum
{
	LSP_OP_MAC_A = 0, LSP_OP_SET_A = 1, LSP_OP_MAC_B = 2, LSP_OP_SET_B = 3,
	LSP_OP_MUL = 4, LSP_OP_ABS = 5, LSP_OP_SPECIAL_A = 6, LSP_OP_SPECIAL_B = 7
};

struct lsp;
typedef void (*lsp_sample_fn)(struct lsp *lsp);

/* hist[n] is a four-deep ring of accumulator n, indexed by the slot counter. */
typedef struct lsp_state
{
	int32_t acc[2];
	int32_t hist[2][4];
	int32_t eram_read;
	int32_t multiplier[2];
	int32_t eram_latch;
	uint16_t eram_base[2];
	uint16_t tap;
	uint16_t eram_pos;
	uint8_t eram_tap2[2];
	uint8_t prev_offset;
	uint8_t buffer_pos;
	uint8_t jump_delay;
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
	bool restart;
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
