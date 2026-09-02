#include <string.h>
#include "wave_rom.h"

size_t wave_rom_size(scemu_model_t model)
{
	switch (model)
	{
	case SCEMU_MODEL_SC88:
	case SCEMU_MODEL_SC88VL:
		return 0x800000;
	case SCEMU_MODEL_SC88PRO:
	case SCEMU_MODEL_VEGSPRO:
		return 0x1400000;
	default:
		return 0;
	}
}

bool wave_rom_build(scemu_model_t model, const scemu_roms_t *roms, uint8_t *out, size_t out_size)
{
	size_t offset = 0;
	if (out_size < wave_rom_size(model))
		return false;
	for (int n = 0; n < roms->wave_rom_count; n++)
	{
		if (offset + roms->wave_rom_size[n] > out_size)
			return false;
		memcpy(out + offset, roms->wave_rom[n], roms->wave_rom_size[n]);
		offset += roms->wave_rom_size[n];
	}
	return offset == wave_rom_size(model);
}
