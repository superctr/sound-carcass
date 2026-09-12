#ifndef SCEMU_SC55MK2_H
#define SCEMU_SC55MK2_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "scemu.h"
#include "h8500.h"
#include "gp.h"
#include "lcd.h"
#include "sub55_hle.h"
#include "midi_queue.h"
#include "jit.h"

/* The SC-55mkII board.  One DAC word pair is the unit of time: the GP-4's own
 * frame is 725 of its clocks and leaves two pairs behind, so a frame here is
 * half of one — the H8 runs its share of cycles, the sub-CPU HLE ticks, and
 * every second frame the chip runs and both pairs are ready. */

#define SC55MK2_GP_CLOCK 24000000u
#define SC55MK2_SAMPLE_RATE 66206u
/* phi is 12 MHz and a chip frame is 725 clocks of 24 MHz, so a frame here is
 * 181.25 phi cycles: 725 quarters of one */
#define SC55MK2_H8_QUARTER_CYCLES_PER_FRAME 725u

#define SC55MK2_INTERNAL_ROM_SIZE 0x8000
#define SC55MK2_PROGRAM_ROM_SIZE 0x80000
#define SC55MK2_WAVE_ROM_SIZE 0x400000
#define SC55MK2_SRAM_SIZE 0x8000
#define SC55MK2_MIDI_PORTS 2

/* the analog mute released and no GP-4 register write for 0.2 s */
#define SC55MK2_IDLE_FRAMES 13200u

/* how long a key the machine presses itself is held, 0.1 s */
#define SC55MK2_KEY_FRAMES 6620u

/* what the factory setup is waiting for, from the reset that started it */
typedef enum sc55mk2_factory
{
	SC55MK2_FACTORY_NONE,
	SC55MK2_FACTORY_PROMPT,   /* holding INSTRUMENT < and >, waiting for `Init All,  Sure?` */
	SC55MK2_FACTORY_CONFIRM,  /* ALL held */
	SC55MK2_FACTORY_REBUILD,  /* waiting for the rebuild to take the machine off idle */
	SC55MK2_FACTORY_SETTLE    /* and for it to come back */
} sc55mk2_factory_t;

typedef struct sc55mk2
{
	scemu_model_t model;
	jit_alloc_t jit;
	uint64_t rom_id;

	uint8_t *internal_rom;
	uint8_t *program_rom;
	uint8_t *wave_rom;
	size_t wave_rom_size;
	uint8_t sram[SC55MK2_SRAM_SIZE];

	h8500_t cpu;
	uint32_t cpu_quarter_cycles;
	gp_t gp;
	int gp_pair;

	lcd_t lcd;
	bool lcd_powered;      /* the controller's own display-on bit, before the unit's rail */
	sub55_hle_t sub;

	/* the GP-4's second register file at 0E400-0E407 */
	uint8_t sys_control;   /* 0E401: bit 0 the LCD rail, 1 the analog mute, 4-2 the mux */
	uint8_t int_enable;    /* 0E402 written: bit n enables source n+1 */
	uint8_t int_trigger;   /* 0E402 read: the source waiting, cleared by the read */

	uint64_t gp_written;   /* the frame of the last write to 0E000-0E03F */
	sc55mk2_factory_t factory;
	uint32_t factory_frames;
	scemu_computer_switch_t computer_switch;

	uint64_t frame;
	midi_queue_t midi;
	scemu_midi_out_fn midi_out;
	void *midi_out_user;
} sc55mk2_t;

const char *sc55mk2_validate_roms(scemu_model_t model, const scemu_roms_t *roms);
bool sc55mk2_init(sc55mk2_t *b, scemu_model_t model, const scemu_roms_t *roms, const scemu_config_t *config);
void sc55mk2_release(sc55mk2_t *b);
void sc55mk2_reset(sc55mk2_t *b);
void sc55mk2_run_frame(sc55mk2_t *b);
bool sc55mk2_idle(const sc55mk2_t *b);
void sc55mk2_button(sc55mk2_t *b, scemu_button_t button, bool down);

void sc55mk2_queue_midi(sc55mk2_t *b, int port, uint8_t byte, uint32_t frame_offset);

#endif
