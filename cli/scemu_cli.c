/* scemu-cli: headless renderer.  Loads a ROM directory, boots the machine,
 * plays a Standard MIDI File into it and writes the output as a WAV file. */
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

/* ---------------------------------------------------------------- files */

static void *load_file(const char *path, size_t *size_out)
{
	FILE *fp = fopen(path, "rb");
	if (!fp)
		return NULL;
	fseek(fp, 0, SEEK_END);
	long size = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	if (size < 0)
	{
		fclose(fp);
		return NULL;
	}
	void *data = malloc((size_t)size + 1);
	size_t got = fread(data, 1, (size_t)size, fp);
	fclose(fp);
	if (got != (size_t)size)
	{
		free(data);
		return NULL;
	}
	*size_out = (size_t)size;
	return data;
}

static void *load_rom(const char *dir, const rom_file_t *f)
{
	char path[1024];
	size_t size;
	snprintf(path, sizeof(path), "%s/%s", dir, f->name);
	void *data = load_file(path, &size);
	if (!data)
	{
		fprintf(stderr, "cannot read %s\n", path);
		return NULL;
	}
	if (size != f->size)
	{
		fprintf(stderr, "%s: expected %zu bytes, got %zu\n", path, f->size, size);
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
	{
		int32_t v = stereo[n] >> 8;
		if (v > 32767) v = 32767;
		if (v < -32768) v = -32768;
		write_u16(fp, (uint16_t)(int16_t)v);
	}
	fclose(fp);
	return 1;
}

/* ---------------------------------------------------------------- Standard MIDI File */

typedef struct midi_event
{
	uint64_t tick;
	uint32_t order;
	uint32_t frame;
	uint16_t length;
	uint8_t tempo_change;
	const uint8_t *bytes;
	uint8_t status[3];
	uint32_t tempo;
} midi_event_t;

typedef struct smf
{
	midi_event_t *events;
	size_t count, capacity;
	uint16_t division;
} smf_t;

static uint32_t read_be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static uint16_t read_be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

static uint32_t read_vlq(const uint8_t **p, const uint8_t *end)
{
	uint32_t v = 0;
	while (*p < end)
	{
		uint8_t c = *(*p)++;
		v = (v << 7) | (c & 0x7f);
		if (!(c & 0x80))
			break;
	}
	return v;
}

static midi_event_t *smf_add(smf_t *s)
{
	if (s->count == s->capacity)
	{
		s->capacity = s->capacity ? s->capacity * 2 : 1024;
		s->events = realloc(s->events, s->capacity * sizeof(midi_event_t));
	}
	midi_event_t *e = &s->events[s->count];
	memset(e, 0, sizeof(*e));
	e->order = (uint32_t)s->count++;
	return e;
}

static int smf_parse_track(smf_t *s, const uint8_t *p, const uint8_t *end)
{
	uint64_t tick = 0;
	uint8_t running = 0;
	while (p < end)
	{
		tick += read_vlq(&p, end);
		if (p >= end)
			break;
		uint8_t status = *p;
		if (status == 0xff)
		{
			p++;
			uint8_t type = *p++;
			uint32_t length = read_vlq(&p, end);
			if (type == 0x51 && length == 3)
			{
				midi_event_t *e = smf_add(s);
				e->tick = tick;
				e->tempo_change = 1;
				e->tempo = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
			}
			if (type == 0x2f)
				break;
			p += length;
		}
		else if (status == 0xf0 || status == 0xf7)
		{
			p++;
			uint32_t length = read_vlq(&p, end);
			midi_event_t *e = smf_add(s);
			e->tick = tick;
			if (status == 0xf0)
			{
				e->status[0] = 0xf0;
				e->bytes = p;
				e->length = (uint16_t)(length + 1);
			}
			else
			{
				e->bytes = p;
				e->length = (uint16_t)length;
			}
			p += length;
		}
		else
		{
			if (status & 0x80)
			{
				running = status;
				p++;
			}
			else
				status = running;
			if (!status)
				return 0;
			int data = (status >= 0xc0 && status < 0xe0) ? 1 : 2;
			midi_event_t *e = smf_add(s);
			e->tick = tick;
			e->status[0] = status;
			for (int n = 0; n < data && p < end; n++)
				e->status[1 + n] = *p++;
			e->length = (uint16_t)(1 + data);
		}
	}
	return 1;
}

static int compare_events(const void *a, const void *b)
{
	const midi_event_t *x = a, *y = b;
	if (x->tick != y->tick)
		return x->tick < y->tick ? -1 : 1;
	return x->order < y->order ? -1 : x->order > y->order;
}

static int smf_load(smf_t *s, const uint8_t *data, size_t size, uint32_t rate)
{
	memset(s, 0, sizeof(*s));
	if (size < 14 || memcmp(data, "MThd", 4) != 0)
		return 0;
	uint16_t tracks = read_be16(data + 10);
	s->division = read_be16(data + 12);
	if (s->division & 0x8000)
		return 0;
	const uint8_t *p = data + 8 + read_be32(data + 4);
	for (uint16_t t = 0; t < tracks && p + 8 <= data + size; t++)
	{
		if (memcmp(p, "MTrk", 4) != 0)
			return 0;
		uint32_t length = read_be32(p + 4);
		const uint8_t *end = p + 8 + length;
		if (end > data + size)
			end = data + size;
		if (!smf_parse_track(s, p + 8, end))
			return 0;
		p = end;
	}
	qsort(s->events, s->count, sizeof(midi_event_t), compare_events);

	double seconds = 0;
	uint64_t last_tick = 0;
	uint32_t tempo = 500000;
	for (size_t n = 0; n < s->count; n++)
	{
		midi_event_t *e = &s->events[n];
		seconds += (double)(e->tick - last_tick) * tempo / 1e6 / s->division;
		last_tick = e->tick;
		e->frame = (uint32_t)(seconds * rate);
		if (e->tempo_change)
			tempo = e->tempo;
	}
	return 1;
}

/* ---------------------------------------------------------------- main */

static void usage(const char *argv0)
{
	fprintf(stderr, "usage: %s <sc88|sc88pro> <romdir> <out.wav> [--midi file.mid] [--seconds N] [--tail N] [--state boot.state] [--map sc55|sc88|sc88pro] [--midi-rate BAUD] [--no-jit]\n", argv0);
}

int main(int argc, char **argv)
{
	if (argc < 4)
	{
		usage(argv[0]);
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

	const char *midi_path = NULL, *state_path = NULL;
	double seconds = 0, tail = 2.0;
	scemu_config_t config = { 0 };
	scemu_map_t map = SCEMU_MAP_NATIVE;
	uint32_t midi_rate = 31250;
	for (int n = 4; n < argc; n++)
	{
		if (!strcmp(argv[n], "--midi") && n + 1 < argc)
			midi_path = argv[++n];
		else if (!strcmp(argv[n], "--seconds") && n + 1 < argc)
			seconds = atof(argv[++n]);
		else if (!strcmp(argv[n], "--tail") && n + 1 < argc)
			tail = atof(argv[++n]);
		else if (!strcmp(argv[n], "--state") && n + 1 < argc)
			state_path = argv[++n];
		else if (!strcmp(argv[n], "--no-jit"))
			config.h8500_interpreter = true;
		else if (!strcmp(argv[n], "--midi-rate") && n + 1 < argc)
			midi_rate = (uint32_t)atoi(argv[++n]);
		else if (!strcmp(argv[n], "--map") && n + 1 < argc)
		{
			const char *name = argv[++n];
			map = !strcmp(name, "sc55") ? SCEMU_MAP_SC55 : !strcmp(name, "sc88") ? SCEMU_MAP_SC88
					: !strcmp(name, "sc88pro") ? SCEMU_MAP_SC88PRO : SCEMU_MAP_NATIVE;
			if (map == SCEMU_MAP_NATIVE)
			{
				usage(argv[0]);
				return 2;
			}
		}
		else
		{
			usage(argv[0]);
			return 2;
		}
	}

	scemu_roms_t roms = { 0 };
	roms.program_rom = load_rom(argv[2], &set->program);
	roms.program_rom_size = set->program.size;
	roms.wave_rom_count = set->wave_count;
	for (int n = 0; n < set->wave_count; n++)
	{
		roms.wave_rom[n] = load_rom(argv[2], &set->wave[n]);
		roms.wave_rom_size[n] = set->wave[n].size;
	}
	if (!roms.program_rom)
		return 1;
	for (int n = 0; n < set->wave_count; n++)
		if (!roms.wave_rom[n])
			return 1;

	scemu_t *m = scemu_create(set->model, &roms, &config);
	if (!m)
	{
		fprintf(stderr, "scemu_create: %s\n", scemu_error(NULL));
		return 1;
	}
	uint32_t rate = scemu_sample_rate(m);

	bool booted = false;
	if (state_path)
	{
		size_t size;
		void *state = load_file(state_path, &size);
		if (state)
		{
			booted = scemu_state_load(m, state, size);
			free(state);
			if (!booted)
				fprintf(stderr, "%s: not a state for this machine, booting instead\n", state_path);
		}
	}
	if (!booted)
	{
		uint64_t boot_frames = scemu_boot(m);
		fprintf(stderr, "booted in %llu frames (%.2f s)\n", (unsigned long long)boot_frames, (double)boot_frames / rate);
		if (state_path)
		{
			size_t size = scemu_state_size(m);
			void *state = malloc(size);
			if (scemu_state_save(m, state, size) == size)
			{
				FILE *fp = fopen(state_path, "wb");
				if (fp)
				{
					fwrite(state, 1, size, fp);
					fclose(fp);
				}
			}
			free(state);
		}
	}

	scemu_set_map(m, map);
	scemu_set_midi_rate(m, midi_rate);

	smf_t smf = { 0 };
	uint8_t *midi_data = NULL;
	if (midi_path)
	{
		size_t size;
		midi_data = load_file(midi_path, &size);
		if (!midi_data || !smf_load(&smf, midi_data, size, rate))
		{
			fprintf(stderr, "%s: cannot read as a Standard MIDI File\n", midi_path);
			return 1;
		}
		if (seconds <= 0 && smf.count)
			seconds = (double)smf.events[smf.count - 1].frame / rate + tail;
	}
	if (seconds <= 0)
		seconds = 5.0;

	size_t frames = (size_t)(seconds * rate);
	int32_t *buf = calloc(frames * 2, sizeof(int32_t));
	const size_t block = 256;
	size_t next_event = 0;
	for (size_t done = 0; done < frames; done += block)
	{
		size_t n = frames - done < block ? frames - done : block;
		while (next_event < smf.count && smf.events[next_event].frame < done + n)
		{
			const midi_event_t *e = &smf.events[next_event++];
			uint32_t offset = e->frame > done ? (uint32_t)(e->frame - done) : 0;
			if (e->tempo_change)
				continue;
			if (e->status[0] == 0xf0 && e->bytes)
			{
				scemu_midi_write(m, SCEMU_MIDI_IN_A, e->status, 1, offset);
				scemu_midi_write(m, SCEMU_MIDI_IN_A, e->bytes, e->length - 1, offset);
			}
			else if (e->bytes)
				scemu_midi_write(m, SCEMU_MIDI_IN_A, e->bytes, e->length, offset);
			else
				scemu_midi_write(m, SCEMU_MIDI_IN_A, e->status, e->length, offset);
		}
		int32_t *const out[2] = { buf + done * 2, NULL };
		scemu_render(m, out, n);
	}

	if (!write_wav(argv[3], buf, frames, rate))
	{
		fprintf(stderr, "cannot write %s\n", argv[3]);
		return 1;
	}
	fprintf(stderr, "wrote %zu frames to %s\n", frames, argv[3]);

	free(buf);
	free(smf.events);
	free(midi_data);
	scemu_destroy(m);
	free((void *)roms.program_rom);
	for (int n = 0; n < set->wave_count; n++)
		free((void *)roms.wave_rom[n]);
	return 0;
}
