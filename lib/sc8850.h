#ifndef SCEMU_SC8850_H
#define SCEMU_SC8850_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "scemu.h"
#include "sh2.h"
#include "xp.h"
#include "lsp.h"
#include "glcd.h"
#include "flash.h"
#include "midi_queue.h"
#include "jit.h"

/* The SC-8850 board.  One frame of the XPs' sample clock is the unit of time:
 * the SH-2 runs its 882 cycles, the gate array ticks, the master XP produces
 * its sample and then the slave, which clocks the LSP once on its port C. */

#define SC8850_SAMPLE_RATE 32000u
#define SC8850_SH2_CLOCK 28224000u
#define SC8850_SH2_CYCLES_PER_FRAME 882u

#define SC8850_BOOT_ROM_SIZE 0x10000
#define SC8850_PROGRAM_ROM_SIZE 0x100000
#define SC8850_TONE_ROM_SIZE 0x200000
#define SC8850_DRAM_SIZE 0x80000
#define SC8850_MIDI_PORTS 4
#define SC8850_NVRAM_BASE 0xe0000u
#define SC8850_NVRAM_SIZE 0x20000u
#define SC8850_PROGRAM_FLASH_DEVICE 0x0050
#define SC8850_TONE_FLASH_DEVICE 0x00d0
#define SC8850_IDLE_FRAMES 6400u

/* TC160G22AF gate array: sixteen interrupt sources on IRQ2 and PA8, the switch matrix,
 * the value encoder, the LED drives. */
typedef struct sc8850_ga
{
	uint8_t regs[0x100];
	uint16_t requests;
	uint8_t source;
	uint8_t event;
	bool pending;
	int32_t encoder;
	uint8_t keys[4];
	uint8_t key_state[4];
	uint8_t scan_position;
	bool scan_encoder;
	uint32_t scan_frames;
	uint32_t tick_frames;
	uint32_t sequencer_frames;
	uint32_t leds;
} sc8850_ga_t;

typedef struct sc8850
{
	jit_alloc_t jit;
	uint64_t rom_id;

	uint8_t *boot_rom;
	uint8_t *program_rom;
	uint8_t *tone_rom;
	uint8_t *wave_rom;
	size_t wave_rom_size;
	uint8_t dram[SC8850_DRAM_SIZE];

	sh2_t cpu;
	uint32_t cpu_overshoot;
	xp_t master;
	xp_t slave;
	lsp_t lsp;
	glcd_t glcd;
	flash_t program_flash;
	flash_t tone_flash;
	sc8850_ga_t ga;
	uint32_t uipc_step;

	bool mute;
	scemu_computer_switch_t computer_switch;

	uint64_t frame;
	uint64_t tg_written;
	bool code_flush_pending;
	midi_queue_t midi;
	scemu_midi_out_fn midi_out;
	void *midi_out_user;
} sc8850_t;

const char *sc8850_validate_roms(scemu_model_t model, const scemu_roms_t *roms);
bool sc8850_init(sc8850_t *b, scemu_model_t model, const scemu_roms_t *roms, const scemu_config_t *config);
void sc8850_release(sc8850_t *b);
void sc8850_reset(sc8850_t *b);
void sc8850_run_frame(sc8850_t *b);
bool sc8850_idle(const sc8850_t *b);

void sc8850_queue_midi(sc8850_t *b, int port, uint8_t byte, uint32_t frame_offset);
void sc8850_deliver_midi(sc8850_t *b);
void sc8850_button(sc8850_t *b, scemu_button_t button, bool down);
void sc8850_dial(sc8850_t *b, int steps);

size_t sc8850_state_size(const sc8850_t *b);
size_t sc8850_state_save(const sc8850_t *b, void *buffer, size_t size);
bool sc8850_state_load(sc8850_t *b, const void *buffer, size_t size);

#endif
