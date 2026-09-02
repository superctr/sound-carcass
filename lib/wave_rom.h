#ifndef SCEMU_WAVE_ROM_H
#define SCEMU_WAVE_ROM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "scemu.h"

/* Assembles the XP's wave address space from the dumped chips: the board's
 * address line swap and the per-model chip layout. */
size_t wave_rom_size(scemu_model_t model);
uint32_t wave_rom_chip_size(scemu_model_t model);
bool wave_rom_build(scemu_model_t model, const scemu_roms_t *roms, uint8_t *out, size_t out_size);

#endif
