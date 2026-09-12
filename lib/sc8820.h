#ifndef SCEMU_SC8820_H
#define SCEMU_SC8820_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "scemu.h"
#include "sh2.h"
#include "xp.h"
#include "lsp.h"
#include "flash.h"
#include "uipc.h"
#include "midi_queue.h"
#include "jit.h"

/* The SC-8820 board: the SC-8850's SH-2 and flash with one XP, which clocks
 * the LSP on its port B, no gate array and no display.  One frame of the
 * chip's sample clock is the unit of time: the SH-2 runs its 882 cycles, the
 * panel is sampled, the chip produces its sample.  The panel hangs off the
 * SH-2's own port pins and the USB controller's mailboxes raise IRQ1 and
 * IRQ2. */

#define SC8820_SAMPLE_RATE 32000u
#define SC8820_SH2_CYCLES_PER_FRAME 882u

#define SC8820_BOOT_ROM_SIZE 0x10000
#define SC8820_PROGRAM_ROM_SIZE 0x200000
#define SC8820_DRAM_SIZE 0x80000
#define SC8820_MIDI_PORTS 4
#define SC8820_FLASH_DEVICE 0x00d0
#define SC8820_IDLE_FRAMES 6400u
#define SC8820_PANEL_ROWS 3

/* The panel: an LED matrix of three rows on PE15, PE14 and PA15 (a row is on
 * while its pin is low) by four columns on PE1, PE0, PE3, PE2, the POWER lamp
 * on PE4 (lit while low), the INST MAP key on PE6 and the volume knob's push
 * on PE7 (low while pressed).  A row's columns are sampled at the end of a
 * frame in which the ports were not written, so the picture is what the eye
 * sees of the scan and not the moment between two rows. */
typedef struct sc8820_panel
{
	uint16_t pe;
	uint16_t pa;
	bool written;
	uint8_t column[SC8820_PANEL_ROWS];
	bool map_key;
	bool preview_key;
	uint32_t leds;
} sc8820_panel_t;

typedef struct sc8820
{
	jit_alloc_t jit;
	uint64_t rom_id;

	uint8_t *boot_rom;
	uint8_t *program_rom;
	uint8_t *wave_rom;
	size_t wave_rom_size;
	uint8_t dram[SC8820_DRAM_SIZE];

	sh2_t cpu;
	uint32_t cpu_overshoot;
	xp_t xp;
	lsp_t lsp;
	flash_t flash;
	uipc_t uipc;
	sc8820_panel_t panel;

	bool mute;
	scemu_computer_switch_t computer_switch;

	uint64_t frame;
	uint64_t tg_written;
	bool code_flush_pending;
	midi_queue_t midi;
	scemu_midi_out_fn midi_out;
	void *midi_out_user;
} sc8820_t;

const char *sc8820_validate_roms(scemu_model_t model, const scemu_roms_t *roms);
bool sc8820_init(sc8820_t *b, scemu_model_t model, const scemu_roms_t *roms, const scemu_config_t *config);
void sc8820_release(sc8820_t *b);
void sc8820_reset(sc8820_t *b);
void sc8820_run_frame(sc8820_t *b);
bool sc8820_idle(const sc8820_t *b);

void sc8820_queue_midi(sc8820_t *b, int port, uint8_t byte, uint32_t frame_offset);
void sc8820_deliver_midi(sc8820_t *b);
void sc8820_button(sc8820_t *b, scemu_button_t button, bool down);

#endif
