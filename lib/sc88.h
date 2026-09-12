#ifndef SCEMU_SC88_H
#define SCEMU_SC88_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "scemu.h"
#include "h8500.h"
#include "xp.h"
#include "lsp.h"
#include "lcd.h"
#include "sub_hle.h"
#include "midi_queue.h"
#include "jit.h"

/* The SC-88 family board.  One frame of the XP's sample clock is the unit of
 * time: the H8 runs its share of cycles, the sub-CPU and gate array tick, the
 * XP produces a sample and, on the Pro, clocks the LSP once through the serial
 * link. */

#define SC88_SAMPLE_RATE 32000u
#define SC88_XP_CLOCK 24576000u
#define SC88_XP_CLOCKS_PER_FRAME 768u
#define SC88_H8_CLOCK 10000000u
#define SC88_H8_HALF_CYCLES_PER_FRAME 625u

#define SC88_SRAM_SIZE 0x10000
#define SC88_MIDI_PORTS 2

/* uPD65622 gate array: interrupt aggregator on IRQ0, LED driver, LCD command/data FIFO. */
typedef struct sc88_ga
{
	uint8_t regs[0x100];
	uint8_t int_pending;
	uint8_t int_mask;
	uint16_t leds;
	uint8_t lcd_fifo[13];
	uint8_t lcd_fifo_count;
	bool lcd_command_pending;
	uint32_t lcd_busy_frames;
	lcd_t *lcd;
	void (*irq)(void *user, bool state);
	void *user;
} sc88_ga_t;

void sc88_ga_init(sc88_ga_t *ga, lcd_t *lcd, void (*irq)(void *user, bool state), void *user);
void sc88_ga_reset(sc88_ga_t *ga);
uint8_t sc88_ga_read(sc88_ga_t *ga, uint32_t offset);
void sc88_ga_write(sc88_ga_t *ga, uint32_t offset, uint8_t data);
void sc88_ga_frame(sc88_ga_t *ga);

typedef struct sc88_map
{
	uint32_t sram_base;
	uint32_t sram_alias[2];
	uint32_t xp_base;
	uint32_t sub_base;
	uint32_t ga_base;
	uint32_t lsp_base;
} sc88_map_t;

typedef struct sc88
{
	scemu_model_t model;
	sc88_map_t map;
	jit_alloc_t jit;
	uint64_t rom_id;

	uint8_t *program_rom;
	size_t program_rom_size;
	uint8_t *wave_rom;
	size_t wave_rom_size;
	uint8_t sram[SC88_SRAM_SIZE];

	h8500_t cpu;
	uint32_t cpu_half_cycles;
	xp_t xp;
	lsp_t lsp;
	bool has_lsp;
	bool has_panel;
	sc88_ga_t ga;
	lcd_t lcd;
	sub_hle_t sub;

	bool xp_int;
	bool mute;
	bool lcd_lit;          /* the SC-88VL's LCDBL line, its P6-0; the glass is dark without it */
	bool lcd_powered;      /* the controller's own display-on bit, before that line */
	bool lsp_mute;
	scemu_computer_switch_t computer_switch;

	uint64_t frame;
	midi_queue_t midi;
	scemu_midi_out_fn midi_out;
	void *midi_out_user;
} sc88_t;

const char *sc88_validate_roms(scemu_model_t model, const scemu_roms_t *roms);
bool sc88_init(sc88_t *b, scemu_model_t model, const scemu_roms_t *roms, const scemu_config_t *config);
void sc88_release(sc88_t *b);
void sc88_reset(sc88_t *b);
void sc88_run_frame(sc88_t *b);
bool sc88_idle(const sc88_t *b);

void sc88_queue_midi(sc88_t *b, int port, uint8_t byte, uint32_t frame_offset);
void sc88_deliver_midi(sc88_t *b);

void sc88_button(sc88_t *b, scemu_button_t button, bool down);
void sc88_display_restored(sc88_t *b);

#endif
