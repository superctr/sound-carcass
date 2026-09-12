#ifndef SCEMU_INTERNAL_H
#define SCEMU_INTERNAL_H

#include "scemu.h"
#include "state.h"
#include "midi_queue.h"
#include "midi_map.h"
#include "sc88.h"
#include "sc8850.h"
#include "sc8820.h"
#include "sc55mk2.h"
#include "sc55.h"

/* What a board owes the API.  A board is one machine family's glue: its CPU, its memory map, its
 * gate array and display, wired to the chips; the API dispatches to whichever the model names. */
typedef struct board_ops
{
	const char *(*validate_roms)(scemu_model_t model, const scemu_roms_t *roms);
	bool (*init)(void *board, scemu_model_t model, const scemu_roms_t *roms, const scemu_config_t *config);
	void (*release)(void *board);
	void (*reset)(void *board);
	void (*run_frame)(void *board);
	bool (*idle)(const void *board);
	uint64_t (*frame)(const void *board);
	uint64_t (*rom_id)(const void *board);
	uint32_t (*sample_rate)(const void *board);

	int (*output_count)(const void *board);
	/* a DAC word of the frame just run, zero while the analog mute holds */
	int32_t (*output)(const void *board, int pair, int channel);

	midi_queue_t *(*midi)(void *board);
	void (*set_midi_out)(void *board, scemu_midi_out_fn fn, void *user);

	void (*button)(void *board, scemu_button_t button, bool down);
	void (*set_computer_switch)(void *board, scemu_computer_switch_t sw);
	uint32_t (*leds)(const void *board);
	scemu_lcd_t *(*lcd)(void *board);
	scemu_glcd_t *(*glcd)(void *board);
	void (*dial)(void *board, int steps);

	size_t (*nvram_size)(const void *board);
	size_t (*nvram_get)(const void *board, void *buffer, size_t size);
	bool (*nvram_set)(void *board, const void *buffer, size_t size);

	/* what the board and its chips keep, for state.c to write and read */
	void (*state_register)(void *board, state_registry_t *reg);
} board_ops_t;

extern const board_ops_t sc88_board_ops;
extern const board_ops_t sc8850_board_ops;
extern const board_ops_t sc8820_board_ops;
extern const board_ops_t sc55mk2_board_ops;
extern const board_ops_t sc55_board_ops;

struct scemu
{
	scemu_model_t model;
	scemu_config_t config;
	const char *error;
	const board_ops_t *ops;
	union
	{
		sc88_t sc88;
		sc8850_t sc8850;
		sc8820_t sc8820;
		sc55mk2_t sc55mk2;
		sc55_t sc55;
	} board;
	midi_map_t map;
};

#endif
