#ifndef SCEMU_SC88_H
#define SCEMU_SC88_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "scemu.h"
#include "h8500.h"
#include "xp.h"
#include "lsp.h"
#include "gate_array.h"
#include "lcd.h"
#include "sub_hle.h"
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
#define SC88_MIDI_QUEUE_SIZE 16384

typedef struct sc88_midi_event
{
	uint32_t frame;
	uint8_t port;
	uint8_t byte;
} sc88_midi_event_t;

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
	gate_array_t ga;
	lcd_t lcd;
	sub_hle_t sub;

	bool xp_int;
	bool mute;
	bool lsp_mute;
	scemu_computer_switch_t computer_switch;

	uint64_t frame;
	sc88_midi_event_t midi_queue[SC88_MIDI_PORTS][SC88_MIDI_QUEUE_SIZE];
	uint32_t midi_head[SC88_MIDI_PORTS], midi_count[SC88_MIDI_PORTS];
	uint32_t midi_drops;
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

/* The whole machine at a frame boundary: the struct with its pointers put back on load, then the ERAMs. */
size_t sc88_state_size(const sc88_t *b);
size_t sc88_state_save(const sc88_t *b, void *buffer, size_t size);
bool sc88_state_load(sc88_t *b, const void *buffer, size_t size);
void sc88_button(sc88_t *b, scemu_button_t button, bool down);

#endif
