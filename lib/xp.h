#ifndef SCEMU_XP_H
#define SCEMU_XP_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "jit.h"

/* Roland XP (MBCS30109): 64-voice PCM engine with a 256-slot effect DSP, one frame per sample. */

#define XP_VOICES 64
#define XP_BUS_COUNT 64
#define XP_OUTPUT_WORDS 8
#define XP_REGS 0x2000
#define XP_DSP_SLOTS 256
#define XP_IRAM_SIZE 256
#define XP_ERAM_SIZE 0x10000

enum
{
	XP_PAGE_CONTROL = 0x00, XP_PAGE_ADDRESS = 0x01, XP_PAGE_LOOP = 0x02, XP_PAGE_END = 0x03,
	XP_PAGE_RESO_TARGET = 0x11, XP_PAGE_PITCH_TARGET = 0x12, XP_PAGE_TVF_TARGET = 0x13,
	XP_PAGE_TVA2_TARGET = 0x14, XP_PAGE_TVA1_TARGET = 0x15,
	XP_PAGE_RESO_CONTROL = 0x16, XP_PAGE_PITCH_CONTROL = 0x17, XP_PAGE_TVF_CONTROL = 0x18,
	XP_PAGE_TVA2_CONTROL = 0x19, XP_PAGE_TVA1_CONTROL = 0x1a,
	XP_PAGE_PITCH_SEED = 0x1b, XP_PAGE_TVF_SEED = 0x1c, XP_PAGE_TVA2_SEED = 0x1d, XP_PAGE_TVA1_SEED = 0x1e,
	XP_PAGE_FILTER = 0x20, XP_PAGE_RESO_SEED = 0x21
};

enum
{
	XP_CRAM_BASE = 0x2c00, XP_IRAM_BASE = 0x3000, XP_IRAM3_BASE = 0x3200, XP_IRAM3_TARGET_BASE = 0x3300,
	XP_PRAM_BASE = 0x3400, XP_RUN_MASK = 0x3900, XP_ROM_SELECT = 0x3908, XP_READBACK_LOW = 0x3910,
	XP_READBACK_HIGH = 0x3912, XP_HIGHEST_VOICE = 0x3914, XP_DSP_MODE = 0x3916, XP_IRQ_STATUS = 0x3918,
	XP_IRQ_ACK = 0x391a, XP_STATUS = 0x391c, XP_ROM_PAGE = 0x3920, XP_ROM_BANK = 0x3922,
	XP_SERIAL_CONFIG = 0x3924, XP_DSP_CONFIG = 0x3926, XP_IRAM3_RATE = 0x3928, XP_DIAG_SELECT = 0x3930,
	XP_SERIAL_FORMAT = 0x3932, XP_VOICE_SELECT = 0x3934, XP_SEND_BASE = 0x3a00, XP_ROM_WINDOW = 0x3c00
};

enum
{
	XP_IRQ_RESO_DONE = 0, XP_IRQ_TVF_DONE = 1, XP_IRQ_PITCH_DONE = 2, XP_IRQ_TVA2_DONE = 3,
	XP_IRQ_VOICE_DONE = 4, XP_IRQ_LOOP_REACHED = 5, XP_IRQ_LOOP_ALTERNATE = 6,
	XP_IRQ_FETCH_OVERLOAD = 7, XP_IRQ_MUTE_CHANGED = 8, XP_IRQ_REASONS = 16
};

typedef enum xp_law { XP_LAW_LINEAR, XP_LAW_EXPONENTIAL, XP_LAW_S_CURVE } xp_law_t;

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
	uint8_t done_reported;
	uint16_t sub_phase;
	int32_t predictor;
	int32_t amplitude;
	int32_t smooth;
	int32_t filter_low;
	int32_t filter_band;
} xp_voice_t;

/* One decoded program slot, the input of the schedule pass. */
typedef struct xp_slot
{
	uint8_t st;
	uint8_t word;
	uint8_t col;
	uint8_t ext;
	uint8_t eram_op;
	uint16_t eram_offset;
	uint16_t cram;
	int32_t coefficient;
	int32_t raw;
} xp_slot_t;

/* What the compiled frame keeps across frames. */
typedef struct xp_dsp_state
{
	int32_t acc;
	int32_t r;
	int32_t mem;
	int32_t input;
	int32_t latch;
	int32_t pend[2];
	int32_t gain;
	uint16_t cursor;
	int32_t serial_frame[2];
	int32_t serial_in;
} xp_dsp_state_t;

/* The schedule pass: what the pipeline holds at each slot, resolved from the program alone. */
typedef struct xp_sched
{
	uint8_t lands;
	uint8_t latch_fresh;
	uint8_t now_valid;
	uint8_t gain_load;
	uint8_t strobe;
} xp_sched_t;

struct xp;
typedef void (*xp_frame_fn)(struct xp *xp);

typedef struct xp
{
	xp_link_t link;
	const uint8_t *wave;
	size_t wave_size;
	uint32_t wave_chip_size;

	uint16_t regs[XP_REGS];
	xp_voice_t voices[XP_VOICES];
	int32_t bus[XP_BUS_COUNT];
	uint64_t bus_written;
	uint64_t run_mask;
	uint64_t run_pending;
	uint32_t read_latch;
	uint32_t frame_counter;
	uint64_t irq_pending[XP_IRQ_REASONS];
	uint16_t irq_event;
	bool irq_active;
	bool int_state;
	int32_t exp_table[257];

	xp_slot_t slots[XP_DSP_SLOTS];
	xp_sched_t sched[XP_DSP_SLOTS];
	xp_dsp_state_t dsp;
	int32_t iram[XP_IRAM_SIZE];
	int32_t *eram;
	uint8_t iram_ramping[XP_IRAM_SIZE];
	uint16_t iram_target[32];
	bool dsp_enabled;
	bool program_dirty;
	uint8_t serial_out_word[2];

	jit_alloc_t *jit;
	jit_code_t code[2];
	int live;
	xp_frame_fn frame;
} xp_t;

bool xp_init(xp_t *xp, const xp_link_t *link, jit_alloc_t *jit, const uint8_t *wave, size_t wave_size, uint32_t chip_size);
void xp_release(xp_t *xp);
void xp_reset(xp_t *xp);

/* The host window: offset is the word index in the 16 KB window. */
uint16_t xp_read(xp_t *xp, uint32_t offset);
void xp_write(xp_t *xp, uint32_t offset, uint16_t data, uint16_t mask);

void xp_set_serial_words(xp_t *xp, int left, int right);
void xp_run_frame(xp_t *xp);
int32_t xp_output(const xp_t *xp, int word);

/* the DSP alone on whatever is in the bus words, for the tests */
void xp_run_dsp(xp_t *xp);

/* the program as the schedule pass sees it, for the tests */
void xp_decode_program(xp_t *xp);
void xp_schedule(xp_t *xp);

#endif
