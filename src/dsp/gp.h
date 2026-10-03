#ifndef SCEMU_GP_H
#define SCEMU_GP_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Roland GP PCM gate array: GP-2 (TC24SC201AF) and GP-4 (TC6116AF), 28 DPCM voices and a
   reverb and chorus network over an external 16K x 16 effect RAM, one frame per sample pair. */

#define GP_SLOTS 32
#define GP_ERAM_SIZE 0x4000

typedef struct gp_link
{
	void (*irq)(void *user, bool state);
	void *user;
} gp_link_t;

/* one slot: the register file the host reads and writes, and the working state its registers
   only mirror */
typedef struct gp_slot
{
	uint32_t wide[6];
	uint16_t narrow[12];
	int32_t filter_low;
	int32_t filter_band;
	uint8_t prefetch;
	bool crossed;
} gp_slot_t;

typedef struct gp
{
	gp_link_t link;
	const uint8_t *wave;
	size_t wave_size;
	bool gp4;

	gp_slot_t slots[GP_SLOTS];
	uint16_t eram[GP_ERAM_SIZE];

	uint32_t key_mask;
	uint32_t key_mask_pending;
	bool key_mask_dirty;

	uint32_t write_latch;
	uint32_t read_latch;
	uint32_t rom_address;
	uint8_t rom_byte;

	uint8_t output_config;
	uint8_t slot_config;
	uint8_t selected_slot;

	uint8_t irq_slot;
	bool irq_pending;

	uint16_t frame_counter;
	bool first_frame;
	int32_t mix[2];
	int32_t send[2];
	int32_t returns[6];
	int32_t sample[2][2];
} gp_t;

/* the wave ROM is the flat 24-bit space the chip presents; the board owns the chip-select
   decoding that fills it */
void gp_init(gp_t *gp, const gp_link_t *link, bool gp4, const uint8_t *wave, size_t wave_size);
void gp_reset(gp_t *gp);

/* the board must run the frames the elapsed clocks are worth before a bus access */
uint8_t gp_read(gp_t *gp, uint8_t offset);
void gp_write(gp_t *gp, uint8_t offset, uint8_t data);

/* a frame is gp_frame_clocks() long and leaves gp_pairs() sample pairs, two in double rate */
void gp_run_frame(gp_t *gp);
uint32_t gp_frame_clocks(const gp_t *gp);
int gp_pairs(const gp_t *gp);
int32_t gp_output(const gp_t *gp, int pair, int channel);
uint32_t gp_output_rate(const gp_t *gp, uint32_t clock);

/* the register file by position, for the tests */
uint32_t gp_wide(const gp_t *gp, int slot, int index);
uint16_t gp_narrow(const gp_t *gp, int slot, int index);

struct state_registry;
void gp_state(gp_t *gp, struct state_registry *reg);

#endif
