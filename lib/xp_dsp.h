#ifndef SCEMU_XP_DSP_H
#define SCEMU_XP_DSP_H

#include <stdint.h>
#include <stdbool.h>
#include "jit.h"

#define XP_DSP_SLOTS 256
#define XP_IRAM_SIZE 256
#define XP_ERAM_SIZE 0x10000

struct xp;

/* One decoded program slot, the input of the schedule pass. */
typedef struct xp_dsp_slot
{
	uint8_t st;
	uint8_t word;
	uint8_t col;
	uint8_t ext;
	uint8_t eram_op;
	bool read_bypass;
	uint16_t eram_offset;
	uint16_t cram;
	int32_t coefficient;
	int32_t raw;
} xp_dsp_slot_t;

/* The data the compiled frame reads and writes.  Everything the schedule pass
 * can resolve from program structure is folded into the code; only this is
 * live across frames. */
typedef struct xp_dsp_state
{
	int32_t acc;
	int32_t product;
	int32_t r;
	int32_t mem;
	int32_t latch;
	int32_t gain;
	uint16_t fraction;
	uint16_t cursor;
	int32_t serial_frame[2];
} xp_dsp_state_t;

typedef void (*xp_dsp_frame_fn)(struct xp *xp);

typedef struct xp_dsp
{
	xp_dsp_slot_t slots[XP_DSP_SLOTS];
	xp_dsp_state_t state;
	int32_t iram[XP_IRAM_SIZE];
	int32_t *eram;
	uint8_t iram_ramping[XP_IRAM_SIZE];
	bool enabled;
	bool dirty;
	jit_code_t code[2];
	int live;
	xp_dsp_frame_fn frame;
} xp_dsp_t;

void xp_dsp_init(xp_dsp_t *d, int32_t *eram);
void xp_dsp_reset(xp_dsp_t *d);
void xp_dsp_decode(xp_dsp_t *d, const uint16_t *pram, const uint16_t *cram);
bool xp_dsp_compile(xp_dsp_t *d, jit_alloc_t *a);
void xp_dsp_release(xp_dsp_t *d, jit_alloc_t *a);

#endif
