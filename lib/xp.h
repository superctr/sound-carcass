#ifndef SCEMU_XP_H
#define SCEMU_XP_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "xp_dsp.h"

/* Roland XP (MBCS30109): 64-voice PCM engine with a 256-slot DSP, one frame per sample. */

#define XP_VOICES 64
#define XP_BUS_COUNT 64
#define XP_OUTPUT_WORDS 8
#define XP_REGS 0x2000

typedef struct xp_link
{
	void (*irq)(void *user, bool state);
	void (*serial_out)(void *user, int channel, int32_t word);
	int32_t (*serial_in)(void *user, int channel);
	void *user;
} xp_link_t;

typedef struct xp_ramp
{
	int32_t current;
	int32_t previous;
	int32_t target;
	int32_t step;
	int32_t accumulator;
	int32_t midpoint;
	uint16_t control;
	uint32_t counter;
	bool active;
} xp_ramp_t;

typedef struct xp_voice
{
	xp_ramp_t pitch, tvf, reso, tva1, tva2;
	uint32_t region;
	uint32_t start;
	uint32_t address;
	uint32_t loop;
	uint32_t end;
	uint8_t start_pending;
	uint8_t alternate;
	uint8_t reverse;
	uint8_t backward;
	uint8_t reading;
	uint8_t loop_reported;
	uint8_t done_reported;
	uint16_t sub_phase;
	int32_t predictor;
	int32_t filter_low;
	int32_t filter_band;
} xp_voice_t;

typedef struct xp
{
	xp_link_t link;
	const uint8_t *wave;
	size_t wave_size;

	uint16_t regs[XP_REGS];
	xp_voice_t voices[XP_VOICES];
	int32_t bus[XP_BUS_COUNT];
	uint64_t run_mask;
	uint32_t read_latch;
	uint32_t frame_counter;
	uint32_t noise;
	uint16_t irq_queue[XP_VOICES];
	uint8_t irq_head;
	uint8_t irq_count;
	bool int_state;
	int32_t exp_table[257];

	int32_t *eram;
	xp_dsp_t dsp;
	jit_alloc_t *jit;
	uint8_t serial_out_word[2];
} xp_t;

bool xp_init(xp_t *xp, const xp_link_t *link, jit_alloc_t *jit, const uint8_t *wave, size_t wave_size);
void xp_release(xp_t *xp);
void xp_reset(xp_t *xp);

uint16_t xp_read(xp_t *xp, uint32_t offset);
void xp_write(xp_t *xp, uint32_t offset, uint16_t data);

void xp_set_serial_words(xp_t *xp, int left, int right);
void xp_run_frame(xp_t *xp);
int32_t xp_output(const xp_t *xp, int word);

#endif
