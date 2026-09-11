#ifndef SCEMU_SC55_H
#define SCEMU_SC55_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "scemu.h"
#include "h8500.h"
#include "gp.h"
#include "lcd.h"
#include "i8251.h"
#include "midi_queue.h"
#include "jit.h"

/* The SC-55 board.  One DAC word pair is the unit of time, as on the SC-55mkII: the GP-2's
 * own frame is 625 of its 20 MHz clocks and leaves two pairs behind, so a frame here is half
 * of one — the H8 runs its share of cycles and every second frame the chip runs and both
 * pairs are ready. */

#define SC55_GP_CLOCK 20000000u
#define SC55_SAMPLE_RATE 64000u
/* phi is 10 MHz and a chip frame is 625 clocks of 20 MHz, so a frame here is 156.25 phi
 * cycles: 625 quarters of one */
#define SC55_H8_QUARTER_CYCLES_PER_FRAME 625u

#define SC55_INTERNAL_ROM_SIZE 0x8000
#define SC55_PROGRAM_ROM_SIZE 0x40000
#define SC55_WAVE_ROM_SIZE 0x100000
#define SC55_SRAM_SIZE 0x8000
#define SC55_MIDI_PORTS 2

/* the display and the audio powered and no GP-2 register write for 0.4 s: the boot pauses
   264 ms between setting up the voices and the effect network */
#define SC55_IDLE_FRAMES 25600u

/* the gate array reports an LCD bus cycle done this many frames after it starts, 47 us */
#define SC55_LCD_DONE_FRAMES 3u

typedef struct sc55
{
	scemu_model_t model;
	jit_alloc_t jit;
	uint64_t rom_id;

	uint8_t *internal_rom;
	uint8_t *program_rom;
	uint8_t *wave_rom;
	size_t wave_rom_size;
	uint8_t sram[SC55_SRAM_SIZE];

	h8500_t cpu;
	uint32_t cpu_quarter_cycles;
	gp_t gp;
	int gp_pair;

	lcd_t lcd;
	bool lcd_powered;      /* the controller's own display-on bit, before the unit's rail */
	i8251_t usart;
	bool rxrdy;

	/* the I/O gate array */
	uint8_t scan;          /* SC7-SC0, the low byte of the last access to 0F000-0F0FF */
	uint8_t int_mask;      /* 0F107: bit n masks source n+1 */
	uint8_t int_pending;   /* bit n: source n+1 latched, waiting for 0F106 */
	uint8_t lcd_done;      /* frames until the LCD cycle's source 1, 0 when none is due */
	uint8_t keys[3];       /* the matrix returns of each row, active low */

	uint64_t gp_written;   /* the frame of the last write to 0E000-0E03F */

	uint64_t frame;
	midi_queue_t midi;
	scemu_midi_out_fn midi_out;
	void *midi_out_user;
} sc55_t;

const char *sc55_validate_roms(scemu_model_t model, const scemu_roms_t *roms);
bool sc55_init(sc55_t *b, scemu_model_t model, const scemu_roms_t *roms, const scemu_config_t *config);
void sc55_release(sc55_t *b);
void sc55_reset(sc55_t *b);
void sc55_run_frame(sc55_t *b);
bool sc55_idle(const sc55_t *b);
void sc55_button(sc55_t *b, scemu_button_t button, bool down);

void sc55_queue_midi(sc55_t *b, int port, uint8_t byte, uint32_t frame_offset);

/* the whole machine at a frame boundary as a byte stream (state_sc55.c) */
size_t sc55_state_size(const sc55_t *b);
size_t sc55_state_save(const sc55_t *b, void *buffer, size_t size);
bool sc55_state_load(sc55_t *b, const void *buffer, size_t size);

#endif
