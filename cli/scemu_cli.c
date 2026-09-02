/* scemu-cli: headless renderer.  Loads a ROM directory, boots the machine,
 * renders to a WAV file. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "scemu.h"

typedef struct rom_file
{
	const char *name;
	size_t size;
} rom_file_t;

typedef struct rom_set
{
	const char *name;
	scemu_model_t model;
	rom_file_t program;
	rom_file_t wave[8];
	int wave_count;
} rom_set_t;

static const rom_set_t ROM_SETS[] =
{
	{ "sc88", SCEMU_MODEL_SC88, { "roland_sc88-control-1.04.ic17", 0x80000 },
	  { { "sc88-pcm-ic-325.ic14", 0x200000 }, { "sc88-pcm-ic-326.ic8", 0x200000 },
	    { "sc88-pcm-ic-327.ic7", 0x200000 }, { "sc88-pcm-ic-328.ic6", 0x200000 } }, 4 },
	{ "sc88pro", SCEMU_MODEL_SC88PRO, { "roland_sc88pro-1.04.ic26", 0x100000 },
	  { { "roland-r01017834-378.ic20", 0x400000 }, { "roland-r01017845-379.ic21", 0x400000 },
	    { "roland-r01124778-519.ic22", 0x400000 }, { "roland-r01124789-520.ic23", 0x400000 },
	    { "roland-r01124790-521.ic24", 0x400000 } }, 5 },
};

static void *load_file(const char *dir, const rom_file_t *f)
{
	char path[1024];
	snprintf(path, sizeof(path), "%s/%s", dir, f->name);
	FILE *fp = fopen(path, "rb");
	if (!fp)
	{
		fprintf(stderr, "cannot open %s\n", path);
		return NULL;
	}
	void *data = malloc(f->size);
	size_t got = fread(data, 1, f->size, fp);
	fclose(fp);
	if (got != f->size)
	{
		fprintf(stderr, "%s: expected %zu bytes, read %zu\n", path, f->size, got);
		free(data);
		return NULL;
	}
	return data;
}

static void write_u32(FILE *fp, uint32_t v) { uint8_t b[4] = { v, v >> 8, v >> 16, v >> 24 }; fwrite(b, 1, 4, fp); }
static void write_u16(FILE *fp, uint16_t v) { uint8_t b[2] = { v, v >> 8 }; fwrite(b, 1, 2, fp); }

static int write_wav(const char *path, const int32_t *stereo, size_t frames, uint32_t rate)
{
	FILE *fp = fopen(path, "wb");
	if (!fp)
		return 0;
	uint32_t data_size = (uint32_t)(frames * 2 * 2);
	fwrite("RIFF", 1, 4, fp); write_u32(fp, 36 + data_size); fwrite("WAVE", 1, 4, fp);
	fwrite("fmt ", 1, 4, fp); write_u32(fp, 16); write_u16(fp, 1); write_u16(fp, 2);
	write_u32(fp, rate); write_u32(fp, rate * 4); write_u16(fp, 4); write_u16(fp, 16);
	fwrite("data", 1, 4, fp); write_u32(fp, data_size);
	for (size_t n = 0; n < frames * 2; n++)
		write_u16(fp, (uint16_t)(int16_t)(stereo[n] >> 8));
	fclose(fp);
	return 1;
}

int main(int argc, char **argv)
{
	if (argc < 4)
	{
		fprintf(stderr, "usage: %s <sc88|sc88pro> <romdir> <out.wav> [seconds]\n", argv[0]);
		return 2;
	}
	const rom_set_t *set = NULL;
	for (size_t n = 0; n < sizeof(ROM_SETS) / sizeof(ROM_SETS[0]); n++)
		if (!strcmp(argv[1], ROM_SETS[n].name))
			set = &ROM_SETS[n];
	if (!set)
	{
		fprintf(stderr, "unknown machine %s\n", argv[1]);
		return 2;
	}
	double seconds = argc > 4 ? atof(argv[4]) : 5.0;

	scemu_roms_t roms = { 0 };
	roms.program_rom = load_file(argv[2], &set->program);
	roms.program_rom_size = set->program.size;
	roms.wave_rom_count = set->wave_count;
	for (int n = 0; n < set->wave_count; n++)
	{
		roms.wave_rom[n] = load_file(argv[2], &set->wave[n]);
		roms.wave_rom_size[n] = set->wave[n].size;
	}

	scemu_t *m = scemu_create(set->model, &roms, NULL);
	if (!m)
	{
		fprintf(stderr, "scemu_create: %s\n", scemu_error(NULL));
		return 1;
	}
	uint64_t boot_frames = scemu_boot(m);
	fprintf(stderr, "booted in %llu frames\n", (unsigned long long)boot_frames);

	uint32_t rate = scemu_sample_rate(m);
	size_t frames = (size_t)(seconds * rate);
	int32_t *buf = calloc(frames * 2, sizeof(int32_t));
	int32_t *const out[2] = { buf, NULL };
	scemu_render(m, out, frames);
	if (!write_wav(argv[3], buf, frames, rate))
	{
		fprintf(stderr, "cannot write %s\n", argv[3]);
		return 1;
	}

	free(buf);
	scemu_destroy(m);
	free((void *)roms.program_rom);
	for (int n = 0; n < set->wave_count; n++)
		free((void *)roms.wave_rom[n]);
	return 0;
}
