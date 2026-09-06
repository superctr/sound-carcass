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
	case SCEMU_MODEL_SC8850:
		return 0x2000000;
	default:
		return 0;
	}
}

uint32_t wave_rom_chip_size(scemu_model_t model)
{
	return (model == SCEMU_MODEL_SC88 || model == SCEMU_MODEL_SC88VL) ? 0x200000 : 0x400000;
}

static const uint8_t ADDRESS_LINES_SC88[18] = { 0, 4, 2, 3, 1, 13, 7, 12, 5, 10, 16, 9, 6, 8, 14, 17, 11, 15 };
static const uint8_t ADDRESS_LINES_SC8850[18] = { 0, 4, 2, 3, 1, 8, 12, 6, 13, 11, 9, 16, 7, 5, 14, 17, 10, 15 };

static void unscramble(uint8_t *out, const uint8_t *in, size_t size, const uint8_t *address_lines)
{
	static const uint8_t data_lines[8] = { 2, 0, 4, 5, 7, 6, 3, 1 };
	uint32_t address_of[3][64];
	uint8_t data_of[256];

	for (int part = 0; part < 3; part++)
		for (int v = 0; v < 64; v++)
		{
			uint32_t address = 0;
			for (int b = 0; b < 6; b++)
				if ((v >> b) & 1)
					address |= 1u << address_lines[part * 6 + b];
			address_of[part][v] = address;
		}
	for (int v = 0; v < 256; v++)
	{
		uint8_t data = 0;
		for (int b = 0; b < 8; b++)
			if ((v >> data_lines[b]) & 1)
				data |= (uint8_t)(1u << b);
		data_of[v] = (uint8_t)data;
	}

	for (size_t i = 0; i < size; i++)
	{
		const size_t address = (i & ~(size_t)0x3ffff)
			| address_of[0][i & 63] | address_of[1][(i >> 6) & 63] | address_of[2][(i >> 12) & 63];
		out[i] = data_of[in[address]];
	}
}

bool wave_rom_build(scemu_model_t model, const scemu_roms_t *roms, uint8_t *out, size_t out_size)
{
	size_t offset = 0;
	const uint8_t *address_lines = model == SCEMU_MODEL_SC8850 ? ADDRESS_LINES_SC8850 : ADDRESS_LINES_SC88;
	if (out_size < wave_rom_size(model))
		return false;
	for (int n = 0; n < roms->wave_rom_count; n++)
	{
		if (offset + roms->wave_rom_size[n] > out_size)
			return false;
		unscramble(out + offset, roms->wave_rom[n], roms->wave_rom_size[n], address_lines);
		offset += roms->wave_rom_size[n];
	}
	return offset == wave_rom_size(model);
}
