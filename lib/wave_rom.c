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
	case SCEMU_MODEL_SC8820:
		return 0x1800000;
	case SCEMU_MODEL_SC55MK2:
		return 0x400000;
	case SCEMU_MODEL_SC55:
		return 0x600000;
	default:
		return 0;
	}
}

/* the SC-8820 numbers its 24 banks straight through, with no chip select, so its two ROMs are one chip */
uint32_t wave_rom_chip_size(scemu_model_t model)
{
	if (model == SCEMU_MODEL_SC8820)
		return 0x1000000;
	return (model == SCEMU_MODEL_SC88 || model == SCEMU_MODEL_SC88VL) ? 0x200000 : 0x400000;
}

static const uint8_t ADDRESS_LINES_SC88[18] = { 0, 4, 2, 3, 1, 13, 7, 12, 5, 10, 16, 9, 6, 8, 14, 17, 11, 15 };
static const uint8_t ADDRESS_LINES_SC8850[18] = { 0, 4, 2, 3, 1, 8, 12, 6, 13, 11, 9, 16, 7, 5, 14, 17, 10, 15 };
/* the SC-55 carries the same permutation */
static const uint8_t ADDRESS_LINES_SC55MK2[20] = { 2, 0, 3, 4, 1, 9, 13, 10, 18, 17, 6, 15, 11, 16, 8, 5, 12, 7, 14, 19 };

#define UNSCRAMBLE_MAX_GROUPS 4

static void unscramble(uint8_t *out, const uint8_t *in, size_t size, const uint8_t *address_lines, int bits)
{
	static const uint8_t data_lines[8] = { 2, 0, 4, 5, 7, 6, 3, 1 };
	const int groups = (bits + 5) / 6;
	uint32_t address_of[UNSCRAMBLE_MAX_GROUPS][64];
	uint8_t data_of[256];

	for (int part = 0; part < groups; part++)
		for (int v = 0; v < 64; v++)
		{
			uint32_t address = 0;
			for (int b = 0; b < 6 && part * 6 + b < bits; b++)
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
		size_t address = i & ~(((size_t)1 << bits) - 1);
		for (int part = 0; part < groups; part++)
			address |= address_of[part][(i >> (part * 6)) & 63];
		out[i] = data_of[in[address]];
	}
}

/* a model's own raw chips, in board order; 0 ends the list */
static const uint32_t CHIPS_SC88[] = { 0x200000, 0x200000, 0x200000, 0x200000, 0 };
static const uint32_t CHIPS_SC88PRO[] = { 0x400000, 0x400000, 0x400000, 0x400000, 0x400000, 0 };
static const uint32_t CHIPS_VEGSPRO[] = { 0x800000, 0x800000, 0x400000, 0 };
static const uint32_t CHIPS_SC8850[] = { 0x1000000, 0x1000000, 0 };
static const uint32_t CHIPS_SC8820[] = { 0x1000000, 0x800000, 0 };
static const uint32_t CHIPS_SC55MK2[] = { 0x200000, 0x100000, 0 };
static const uint32_t CHIPS_SC55[] = { 0x100000, 0x100000, 0x100000, 0 };

static const uint32_t *own_chips(scemu_model_t model)
{
	switch (model)
	{
	case SCEMU_MODEL_SC88:
	case SCEMU_MODEL_SC88VL:
		return CHIPS_SC88;
	case SCEMU_MODEL_SC88PRO:
		return CHIPS_SC88PRO;
	case SCEMU_MODEL_VEGSPRO:
		return CHIPS_VEGSPRO;
	case SCEMU_MODEL_SC8850:
		return CHIPS_SC8850;
	case SCEMU_MODEL_SC8820:
		return CHIPS_SC8820;
	case SCEMU_MODEL_SC55MK2:
		return CHIPS_SC55MK2;
	case SCEMU_MODEL_SC55:
		return CHIPS_SC55;
	default:
		return NULL;
	}
}

static bool chips_match(const scemu_roms_t *roms, const uint32_t *chips)
{
	int n = 0;
	for (; chips[n]; n++)
		if (n >= roms->wave_rom_count || !roms->wave_rom[n] || roms->wave_rom_size[n] != chips[n])
			return false;
	return n == roms->wave_rom_count;
}

static bool later_set(scemu_model_t model)
{
	return model == SCEMU_MODEL_SC88PRO || model == SCEMU_MODEL_VEGSPRO;
}

/* the whole address space as one image; the SC-55s' images leave out the mirrors */
static size_t descrambled_size(scemu_model_t model)
{
	return model == SCEMU_MODEL_SC55MK2 || model == SCEMU_MODEL_SC55 ? 0x300000 : wave_rom_size(model);
}

static bool descrambled_size_ok(scemu_model_t model, size_t size)
{
	if (size == descrambled_size(model))
		return true;
	return later_set(model) && (size == wave_rom_size(SCEMU_MODEL_SC8850) || size == wave_rom_size(SCEMU_MODEL_SC8820));
}

const char *wave_rom_validate(scemu_model_t model, const scemu_roms_t *roms)
{
	const uint32_t *chips = own_chips(model);
	if (!chips)
		return "unknown model";
	if (roms->wave_rom_count == 1)
	{
		if (!roms->wave_rom[0])
			return "missing wave ROM";
		if (!descrambled_size_ok(model, roms->wave_rom_size[0]))
			return "descrambled wave ROM has the wrong size";
		return NULL;
	}
	if (chips_match(roms, chips))
		return NULL;
	if (later_set(model) && (chips_match(roms, CHIPS_SC8850) || chips_match(roms, CHIPS_SC8820)))
		return NULL;
	return "wave ROMs do not make a set";
}

static bool build_descrambled(scemu_model_t model, const uint8_t *in, uint8_t *out)
{
	switch (model)
	{
	case SCEMU_MODEL_SC55MK2:
		memcpy(out, in, 0x300000);
		memcpy(out + 0x300000, in + 0x200000, 0x100000);
		return true;
	case SCEMU_MODEL_SC55:
		for (int n = 0; n < 3; n++)
		{
			memcpy(out + (size_t)n * 0x200000, in + (size_t)n * 0x100000, 0x100000);
			memcpy(out + (size_t)n * 0x200000 + 0x100000, in + (size_t)n * 0x100000, 0x100000);
		}
		return true;
	default:
		memcpy(out, in, wave_rom_size(model));
		return true;
	}
}

/* chip select 1 is 2 MB wide and IC16 has no A20 */
static void build_sc55mk2(const scemu_roms_t *roms, uint8_t *out)
{
	unscramble(out, roms->wave_rom[0], 0x200000, ADDRESS_LINES_SC55MK2, 20);
	unscramble(out + 0x200000, roms->wave_rom[1], 0x100000, ADDRESS_LINES_SC55MK2, 20);
	memcpy(out + 0x300000, out + 0x200000, 0x100000);
}

/* each of the three 1 MB parts has a 2 MB chip select of its own and no A20 */
static void build_sc55(const scemu_roms_t *roms, uint8_t *out)
{
	for (int n = 0; n < 3; n++)
	{
		uint8_t *chip = out + (size_t)n * 0x200000;
		unscramble(chip, roms->wave_rom[n], 0x100000, ADDRESS_LINES_SC55MK2, 20);
		memcpy(chip + 0x100000, chip, 0x100000);
	}
}

/* the later sets begin with the SC-88Pro's twenty regions, on the SC-8850's wiring */
static void build_from_later(const scemu_roms_t *roms, uint8_t *out, size_t size)
{
	size_t offset = 0;
	for (int n = 0; n < roms->wave_rom_count && offset < size; n++)
	{
		size_t take = roms->wave_rom_size[n] < size - offset ? roms->wave_rom_size[n] : size - offset;
		unscramble(out + offset, roms->wave_rom[n], take, ADDRESS_LINES_SC8850, 18);
		offset += take;
	}
}

bool wave_rom_build(scemu_model_t model, const scemu_roms_t *roms, uint8_t *out, size_t out_size)
{
	const size_t size = wave_rom_size(model);
	if (out_size < size || wave_rom_validate(model, roms))
		return false;
	if (roms->wave_rom_count == 1)
		return build_descrambled(model, roms->wave_rom[0], out);
	if (model == SCEMU_MODEL_SC55MK2)
		build_sc55mk2(roms, out);
	else if (model == SCEMU_MODEL_SC55)
		build_sc55(roms, out);
	else if (later_set(model) && !chips_match(roms, own_chips(model)))
		build_from_later(roms, out, size);
	else
	{
		const uint8_t *address_lines = model == SCEMU_MODEL_SC8850 || model == SCEMU_MODEL_SC8820 ? ADDRESS_LINES_SC8850 : ADDRESS_LINES_SC88;
		size_t offset = 0;
		for (int n = 0; n < roms->wave_rom_count; n++)
		{
			unscramble(out + offset, roms->wave_rom[n], roms->wave_rom_size[n], address_lines, 18);
			offset += roms->wave_rom_size[n];
		}
	}
	return true;
}
