#ifndef SCEMU_XP_H
#define SCEMU_XP_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "jit.h"

/* Roland XP (MBCS30109): 64-voice PCM engine with a 288-slot effect DSP, one frame per sample. */

#define XP_VOICES 64
#define XP_BUS_COUNT 64
#define XP_OUTPUT_PORTS 3
#define XP_OUTPUTS (XP_OUTPUT_PORTS * 2)
#define XP_REGS 0x2000
#define XP_DSP_SLOTS 288
#define XP_DSP_BUDGET 256
#define XP_IRAM_CELLS 192
#define XP_ERAM_SIZE 0x10000
#define XP_STROBES 16

enum
{
	XP_PAGE_CONTROL = 0x00, XP_PAGE_ADDRESS = 0x01, XP_PAGE_LOOP = 0x02, XP_PAGE_END = 0x03, XP_PAGE_EXPONENTS = 0x04,
	XP_PAGE_PREDICTOR = 0x0c, XP_PAGE_INCREMENT = 0x0d, XP_PAGE_PHASE = 0x0e, XP_PAGE_SERVICE = 0x10,
	XP_PAGE_RESO_TARGET = 0x11, XP_PAGE_PITCH_TARGET = 0x12, XP_PAGE_TVF_TARGET = 0x13,
	XP_PAGE_TVA2_TARGET = 0x14, XP_PAGE_TVA1_TARGET = 0x15,
	XP_PAGE_RESO_CONTROL = 0x16, XP_PAGE_PITCH_CONTROL = 0x17, XP_PAGE_TVF_CONTROL = 0x18,
	XP_PAGE_TVA2_CONTROL = 0x19, XP_PAGE_TVA1_CONTROL = 0x1a,
	XP_PAGE_PITCH_SEED = 0x1b, XP_PAGE_TVF_SEED = 0x1c, XP_PAGE_TVA2_SEED = 0x1d, XP_PAGE_TVA1_SEED = 0x1e,
	XP_PAGE_FILTER = 0x20, XP_PAGE_RESO_SEED = 0x21, XP_PAGE_CUTOFF = 0x22, XP_PAGE_AMPLITUDE = 0x23,
	XP_PAGE_PITCH_STEP = 0x24, XP_PAGE_TVF_STEP = 0x25, XP_PAGE_TVA1_STEP = 0x26, XP_PAGE_SMOOTH = 0x27,
	XP_PAGE_FILTER_BAND = 0x28, XP_PAGE_FILTER_LOW = 0x29, XP_PAGE_OUTPUT = 0x2a
};

enum
{
	XP_CRAM_BASE = 0x2c00, XP_IRAM_BASE = 0x3000, XP_IRAM3_BASE = 0x3200, XP_IRAM3_TARGET_BASE = 0x3300,
	XP_PRAM_BASE = 0x3400, XP_RUN_MASK = 0x3900, XP_ROM_SELECT = 0x3908, XP_READBACK_LOW = 0x3910,
	XP_READBACK_HIGH = 0x3912, XP_HIGHEST_VOICE = 0x3914, XP_DSP_MODE = 0x3916, XP_IRQ_STATUS = 0x3918,
	XP_IRQ_ACK = 0x391a, XP_STATUS = 0x391c, XP_ROM_PAGE = 0x3920, XP_ROM_BANK = 0x3922,
	XP_SERIAL_CONFIG = 0x3924, XP_DSP_CONFIG = 0x3926, XP_IRAM3_RATE = 0x3928, XP_DIAG_SELECT = 0x3930,
	XP_SERIAL_FORMAT = 0x3932, XP_VOICE_SELECT = 0x3934, XP_VOICE_WINDOW = 0x3940, XP_VOICE_WINDOW_END = 0x39f0,
	XP_SEND_WINDOW = 0x39f8, XP_SEND_BASE = 0x3a00, XP_ROM_WINDOW = 0x3c00
};

enum
{
	XP_IRQ_RESO_DONE = 0, XP_IRQ_TVF_DONE = 1, XP_IRQ_PITCH_DONE = 2, XP_IRQ_TVA2_DONE = 3,
	XP_IRQ_VOICE_DONE = 4, XP_IRQ_LOOP_REACHED = 5, XP_IRQ_LOOP_ALTERNATE = 6,
	XP_IRQ_FETCH_OVERLOAD = 7, XP_IRQ_MUTE_CHANGED = 8, XP_IRQ_REASONS = 16
};

enum { XP_IDLE, XP_PRELOAD, XP_INITIALIZE, XP_STARTING, XP_RUNNING };

/* the serial ports the `ext=2` strobes clock out on; port A, the six-line bus of the `ext=1` strobes, is below */
enum { XP_PORT_B, XP_PORT_C, XP_PORT_D };

typedef struct xp_link
{
	void (*irq)(void *user, bool state);
	/* a word clocked out on port B, C or D: port * 2 + half, the word with the bits the wire does not carry cleared */
	void (*port_out)(void *user, int port, int32_t word);
	/* port A's input: the word taken at the frame's k-th `ext=1` strobe */
	int32_t (*port_a_in)(void *user, int strobe);
	/* port B's input: the word taken at the k-th three-strobe group */
	int32_t (*port_b_in)(void *user, int group);
	void *user;
} xp_link_t;

/* what a voice keeps outside its pages */
typedef struct xp_voice
{
	uint8_t phase;
	uint8_t format;
	uint8_t fade_entry;
	uint32_t start;
} xp_voice_t;

/* the multiply input a slot's col[5:4] selects, and what it selects instead when the function is 0 */
enum { XP_INPUT_PREVIOUS, XP_INPUT_ACC, XP_INPUT_R, XP_INPUT_LATCH };
enum { XP_SPECIAL_NOP, XP_SPECIAL_BRANCH, XP_SPECIAL_INDEXED_READ, XP_SPECIAL_PARALLEL };
enum { XP_LIVE_CRAM = 1, XP_LIVE_OFFSET = 2 };

/* One decoded program slot, the input of the schedule pass. */
typedef struct xp_slot
{
	uint8_t st;
	uint8_t word;
	uint8_t input;
	uint8_t function;
	uint8_t ext;
	uint8_t eram_op;
	uint8_t eram_second;
	uint16_t eram_offset;
	uint16_t cram;
	int32_t coefficient;
	int32_t raw;
} xp_slot_t;

/* The datapath registers that live across slots and frames. */
typedef struct xp_dsp_state
{
	int32_t acc;
	int32_t product;
	int32_t r;
	int32_t input;
	int32_t now;
	int32_t latch;
	int32_t gain;
	int32_t pend[2];
	uint8_t now_valid;
	uint16_t cursor;
	int32_t port_a_in;
	int32_t port_b_in;
	int32_t port_a_return[XP_STROBES];
	int32_t port_b_pair[2];
	int32_t cycle;
	int32_t land_valid;
	int32_t strobe_a;
	int32_t strobe_bcd;
	int32_t position;
} xp_dsp_state_t;

/* The schedule pass: what a straight-line frame holds at each slot, resolved from the program alone. */
typedef struct xp_sched
{
	uint8_t lands;
	uint8_t now_valid;
	uint8_t gain_load;
	uint8_t strobe_a;     /* the ordinal of an `ext=1` strobe, else 0xff */
	uint8_t strobe_bcd;   /* the ordinal of an `ext=2` strobe, else 0xff */
	uint8_t position;     /* the word position a strobe clocks out */
	uint8_t eram_rail;    /* an ERAM write saturates at these bits instead of 24; 0 = 24 */
	uint8_t latch_clamp;  /* an ERAM read is saturated to 24 bits as it lands */
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
	uint8_t still[XP_VOICES];
	uint64_t named_words;
	bool sends_dirty;
	uint64_t run_mask;
	uint64_t run_pending;
	uint32_t read_latch;
	uint16_t write_latch;
	uint32_t frame_counter;
	uint16_t irq_event;
	bool irq_active;
	bool irq_frame_used;
	bool int_state;
	int32_t exp_table[257];

	xp_slot_t slots[XP_DSP_SLOTS];
	xp_sched_t sched[XP_DSP_SLOTS];
	xp_dsp_state_t dsp;
	int32_t iram[XP_IRAM_CELLS];
	int32_t *eram;
	uint8_t iram_ramping[64];
	bool dsp_enabled;
	bool program_dirty;
	bool branching;
	uint8_t live[XP_DSP_SLOTS];   /* XP_LIVE_CRAM, XP_LIVE_OFFSET: what the code reads from the slot */
	uint16_t strobe_slots[XP_DSP_SLOTS];
	int strobe_count;
	uint32_t compiles;
	uint8_t parity;
	int rail_bits;
	uint64_t wide[4];             /* the words whose store saturates at the rail instead of 24 bits */
	int32_t port_word[XP_OUTPUT_PORTS][2];
	int32_t port_a_out[XP_STROBES];

	jit_alloc_t *jit;
	jit_code_t code[2];
	xp_frame_fn frame[2];
} xp_t;

bool xp_init(xp_t *xp, const xp_link_t *link, jit_alloc_t *jit, const uint8_t *wave, size_t wave_size, uint32_t chip_size);
void xp_release(xp_t *xp);
void xp_reset(xp_t *xp);

/* The host window: offset is the word index in the 16 KB window. */
uint16_t xp_read(xp_t *xp, uint32_t offset);
void xp_write(xp_t *xp, uint32_t offset, uint16_t data, uint16_t mask);

/* the saturation width of the words the DACs take, 24 (the chip's) to 29 bits */
void xp_set_rail(xp_t *xp, int bits);

void xp_run_frame(xp_t *xp);
/* a port's word for the stream: port * 2 + half */
int32_t xp_output(const xp_t *xp, int channel);
/* what port A clocked out at the last frame's k-th strobe */
int32_t xp_port_a_out(const xp_t *xp, int strobe);

/* the DSP words as the frame just run addressed them, and the mixer bank the next frame reads, for the tests */
int32_t xp_iram(const xp_t *xp, int word);
int32_t xp_bus_word(const xp_t *xp, int n);
void xp_set_bus_word(xp_t *xp, int n, int32_t value);
#define xp_run_dsp xp_run_frame

/* the program as the schedule pass sees it, for the tests */
void xp_decode_program(xp_t *xp);
void xp_schedule(xp_t *xp);

#endif
