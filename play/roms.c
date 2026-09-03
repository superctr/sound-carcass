/* scplay: ROM discovery.  A model's images are looked for in a zip or a
 * directory named after the model, beside the executable and in ~/.mame/roms.
 *
 * Copyright (c) 2026 ian karlsson
 * SPDX-License-Identifier: BSD-3-Clause
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <zlib.h>
#include "roms.h"

typedef struct rom_file
{
	const char *name;
	size_t size;
} rom_file_t;

typedef struct model_def
{
	const char *name;
	const char *label;
	scemu_model_t model;
	rom_file_t program;
	rom_file_t wave[SCEMU_MAX_WAVE_ROMS];
	int wave_count;
	const char *wave_from;   /* another model's set to fall back on for the waves */
} model_def_t;

static const rom_file_t WAVE_SC88[4] =
{
	{ "sc88-pcm-ic-325.ic14", 0x200000 }, { "sc88-pcm-ic-326.ic8", 0x200000 },
	{ "sc88-pcm-ic-327.ic7", 0x200000 },  { "sc88-pcm-ic-328.ic6", 0x200000 },
};

static const model_def_t MODELS[] =
{
	{ "sc88pro", "SC-88Pro", SCEMU_MODEL_SC88PRO,
	  { "roland_sc88pro-1.04.ic26", 0x100000 },
	  { { "roland-r01017834-378.ic20", 0x400000 }, { "roland-r01017845-379.ic21", 0x400000 },
	    { "roland-r01124778-519.ic22", 0x400000 }, { "roland-r01124789-520.ic23", 0x400000 },
	    { "roland-r01124790-521.ic24", 0x400000 } }, 5, NULL },
	{ "sc88", "SC-88", SCEMU_MODEL_SC88,
	  { "roland_sc88-control-1.04.ic17", 0x80000 },
	  { { 0 } }, 4, "sc88" },
	{ "sc88vl", "SC-88VL", SCEMU_MODEL_SC88VL,
	  { "roland_sc88_vl-1.04.ic29", 0x80000 },
	  { { 0 } }, 4, "sc88" },
};

#define MODEL_COUNT ((int)(sizeof(MODELS) / sizeof(MODELS[0])))

static const rom_file_t *model_wave(const model_def_t *d, int n)
{
	return d->wave_from ? &WAVE_SC88[n] : &d->wave[n];
}

/* ---------------------------------------------------------------- zip */

typedef struct zip_entry
{
	char name[128];
	uint32_t method;
	uint32_t compressed, uncompressed;
	uint32_t local_offset;
} zip_entry_t;

typedef struct zipfile
{
	uint8_t *data;
	size_t size;
	zip_entry_t *entries;
	int count;
} zipfile_t;

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static void *read_whole(const char *path, size_t *size_out)
{
	FILE *fp = fopen(path, "rb");
	if (!fp)
		return NULL;
	if (fseek(fp, 0, SEEK_END) != 0)
	{
		fclose(fp);
		return NULL;
	}
	long size = ftell(fp);
	if (size <= 0 || fseek(fp, 0, SEEK_SET) != 0)
	{
		fclose(fp);
		return NULL;
	}
	void *data = malloc((size_t)size);
	if (!data)
	{
		fclose(fp);
		return NULL;
	}
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

static void zip_close(zipfile_t *z)
{
	if (!z)
		return;
	free(z->entries);
	free(z->data);
	free(z);
}

static zipfile_t *zip_open(const char *path)
{
	size_t size;
	uint8_t *data = read_whole(path, &size);
	if (!data)
		return NULL;
	if (size < 22)
	{
		free(data);
		return NULL;
	}

	size_t scan = size > 66000 ? size - 66000 : 0;
	long eocd = -1;
	for (size_t n = size - 22 + 1; n-- > scan; )
		if (data[n] == 'P' && data[n + 1] == 'K' && data[n + 2] == 5 && data[n + 3] == 6)
		{
			eocd = (long)n;
			break;
		}
	if (eocd < 0)
	{
		free(data);
		return NULL;
	}

	int count = rd16(data + eocd + 10);
	uint32_t dir_offset = rd32(data + eocd + 16);
	if (count <= 0 || dir_offset >= size)
	{
		free(data);
		return NULL;
	}

	zipfile_t *z = calloc(1, sizeof(*z));
	if (!z)
	{
		free(data);
		return NULL;
	}
	z->data = data;
	z->size = size;
	z->entries = calloc((size_t)count, sizeof(zip_entry_t));
	if (!z->entries)
	{
		zip_close(z);
		return NULL;
	}

	const uint8_t *p = data + dir_offset;
	for (int n = 0; n < count; n++)
	{
		if ((size_t)(p - data) + 46 > size || rd32(p) != 0x02014b50)
			break;
		uint16_t name_len = rd16(p + 28), extra_len = rd16(p + 30), comment_len = rd16(p + 32);
		zip_entry_t *e = &z->entries[z->count];
		size_t copy = name_len < sizeof(e->name) - 1 ? name_len : sizeof(e->name) - 1;
		memcpy(e->name, p + 46, copy);
		e->name[copy] = 0;
		e->method = rd16(p + 10);
		e->compressed = rd32(p + 20);
		e->uncompressed = rd32(p + 24);
		e->local_offset = rd32(p + 42);
		z->count++;
		p += 46 + name_len + extra_len + comment_len;
	}
	return z;
}

static void *zip_extract(zipfile_t *z, const char *name, size_t *size_out)
{
	const zip_entry_t *e = NULL;
	for (int n = 0; n < z->count && !e; n++)
	{
		const char *base = strrchr(z->entries[n].name, '/');
		base = base ? base + 1 : z->entries[n].name;
		if (!strcmp(base, name))
			e = &z->entries[n];
	}
	if (!e || (size_t)e->local_offset + 30 > z->size)
		return NULL;

	const uint8_t *lh = z->data + e->local_offset;
	if (rd32(lh) != 0x04034b50)
		return NULL;
	size_t data_offset = e->local_offset + 30u + rd16(lh + 26) + rd16(lh + 28);
	if (data_offset + e->compressed > z->size)
		return NULL;

	uint8_t *out = malloc(e->uncompressed ? e->uncompressed : 1);
	if (!out)
		return NULL;

	if (e->method == 0)
	{
		if (e->compressed != e->uncompressed)
		{
			free(out);
			return NULL;
		}
		memcpy(out, z->data + data_offset, e->uncompressed);
	}
	else if (e->method == 8)
	{
		z_stream s;
		memset(&s, 0, sizeof(s));
		if (inflateInit2(&s, -MAX_WBITS) != Z_OK)
		{
			free(out);
			return NULL;
		}
		s.next_in = z->data + data_offset;
		s.avail_in = e->compressed;
		s.next_out = out;
		s.avail_out = e->uncompressed;
		int rc = inflate(&s, Z_FINISH);
		inflateEnd(&s);
		if ((rc != Z_STREAM_END && rc != Z_OK) || s.total_out != e->uncompressed)
		{
			free(out);
			return NULL;
		}
	}
	else
	{
		free(out);
		return NULL;
	}
	*size_out = e->uncompressed;
	return out;
}

/* ---------------------------------------------------------------- sources */

typedef struct rom_source
{
	char path[512];
	int is_zip;
	int tried;
	zipfile_t *zip;
} rom_source_t;

typedef struct source_list
{
	rom_source_t s[SCPLAY_MAX_SOURCES];
	int count;
} source_list_t;

static int is_dir(const char *path)
{
	struct stat st;
	return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int is_file(const char *path)
{
	struct stat st;
	return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static void source_add(source_list_t *list, const char *path, int zip)
{
	if (list->count >= SCPLAY_MAX_SOURCES)
		return;
	for (int n = 0; n < list->count; n++)
		if (!strcmp(list->s[n].path, path))
			return;
	rom_source_t *s = &list->s[list->count++];
	memset(s, 0, sizeof(*s));
	snprintf(s->path, sizeof(s->path), "%s", path);
	s->is_zip = zip;
}

static void source_add_named(source_list_t *list, const char *dir, const char *name)
{
	char path[512];
	snprintf(path, sizeof(path), "%s/%s.zip", dir, name);
	if (is_file(path))
		source_add(list, path, 1);
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	if (is_dir(path))
		source_add(list, path, 0);
}

static void source_free(source_list_t *list)
{
	for (int n = 0; n < list->count; n++)
		zip_close(list->s[n].zip);
	list->count = 0;
}

static void *source_read(rom_source_t *s, const rom_file_t *f, char *origin, size_t origin_size)
{
	void *data = NULL;
	size_t size = 0;
	if (s->is_zip)
	{
		if (!s->tried)
		{
			s->tried = 1;
			s->zip = zip_open(s->path);
		}
		if (s->zip)
			data = zip_extract(s->zip, f->name, &size);
	}
	else
	{
		char path[1024];
		snprintf(path, sizeof(path), "%s/%s", s->path, f->name);
		data = read_whole(path, &size);
	}
	if (!data)
		return NULL;
	if (size != f->size)
	{
		free(data);
		return NULL;
	}
	if (origin)
		snprintf(origin, origin_size, "%s", s->path);
	return data;
}

static void *find_rom(source_list_t *list, const rom_file_t *f, char *origin, size_t origin_size)
{
	for (int n = 0; n < list->count; n++)
	{
		void *data = source_read(&list->s[n], f, origin, origin_size);
		if (data)
			return data;
	}
	return NULL;
}

/* ---------------------------------------------------------------- loading */

static uint64_t hash_add(uint64_t h, const void *data, size_t size)
{
	const uint8_t *p = data;
	size_t words = size / 8;
	for (size_t n = 0; n < words; n++)
	{
		uint64_t v;
		memcpy(&v, p + n * 8, 8);
		h = (h ^ v) * 0x100000001b3ull;
	}
	for (size_t n = words * 8; n < size; n++)
		h = (h ^ p[n]) * 0x100000001b3ull;
	return h ^ (uint64_t)size;
}

static void build_sources(source_list_t *list, const model_def_t *d, const char *rom_path,
                          const char *exe_dir)
{
	const char *dirs[3];
	int dir_count = 0;
	char home_roms[512];
	const char *home = getenv("HOME");

	if (exe_dir && *exe_dir)
		dirs[dir_count++] = exe_dir;
	if (home)
	{
		snprintf(home_roms, sizeof(home_roms), "%s/.mame/roms", home);
		dirs[dir_count++] = home_roms;
	}

	if (rom_path && *rom_path)
	{
		if (is_dir(rom_path))
			source_add(list, rom_path, 0);
		else if (is_file(rom_path))
			source_add(list, rom_path, 1);
	}
	for (int n = 0; n < dir_count; n++)
		source_add_named(list, dirs[n], d->name);
	if (d->wave_from)
		for (int n = 0; n < dir_count; n++)
			source_add_named(list, dirs[n], d->wave_from);
	/* a bare directory holding the images, given as --rom, is already first */
	for (int n = 0; n < dir_count; n++)
		source_add(list, dirs[n], 0);
}

static int load_model(scplay_roms_t *out, const model_def_t *d, const char *rom_path,
                      const char *exe_dir, char *err, size_t err_size)
{
	source_list_t list;
	memset(&list, 0, sizeof(list));
	build_sources(&list, d, rom_path, exe_dir);

	memset(out, 0, sizeof(*out));
	out->model = d->model;
	out->model_name = d->name;

	void *program = find_rom(&list, &d->program, out->origin, sizeof(out->origin));
	if (!program)
	{
		snprintf(err, err_size, "%s: no control ROM %s", d->name, d->program.name);
		source_free(&list);
		return 0;
	}
	out->owned[out->owned_count++] = program;
	out->roms.program_rom = program;
	out->roms.program_rom_size = d->program.size;
	out->roms.wave_rom_count = d->wave_count;

	uint64_t h = 0xcbf29ce484222325ull;
	h = hash_add(h, program, d->program.size);

	for (int n = 0; n < d->wave_count; n++)
	{
		const rom_file_t *f = model_wave(d, n);
		void *wave = find_rom(&list, f, NULL, 0);
		if (!wave)
		{
			snprintf(err, err_size, "%s: no wave ROM %s", d->name, f->name);
			source_free(&list);
			scplay_roms_free(out);
			return 0;
		}
		out->owned[out->owned_count++] = wave;
		out->roms.wave_rom[n] = wave;
		out->roms.wave_rom_size[n] = f->size;
		h = hash_add(h, wave, f->size);
	}
	out->hash = h;
	source_free(&list);
	return 1;
}

int scplay_roms_load(scplay_roms_t *out, const char *model_name, const char *rom_path,
                     const char *exe_dir, char *err, size_t err_size)
{
	char first_err[256] = "no ROM set found";
	if (model_name)
	{
		for (int n = 0; n < MODEL_COUNT; n++)
			if (!strcmp(MODELS[n].name, model_name))
				return load_model(out, &MODELS[n], rom_path, exe_dir, err, err_size);
		snprintf(err, err_size, "unknown model %s", model_name);
		return 0;
	}
	for (int n = 0; n < MODEL_COUNT; n++)
	{
		char e[256];
		if (load_model(out, &MODELS[n], rom_path, exe_dir, e, sizeof(e)))
			return 1;
		if (n == 0)
			snprintf(first_err, sizeof(first_err), "%s", e);
	}
	snprintf(err, err_size, "%s (and none of the other models' sets either)", first_err);
	return 0;
}

void scplay_roms_free(scplay_roms_t *r)
{
	for (int n = 0; n < r->owned_count; n++)
		free(r->owned[n]);
	r->owned_count = 0;
	memset(&r->roms, 0, sizeof(r->roms));
}

const char *scplay_model_label(scemu_model_t model)
{
	for (int n = 0; n < MODEL_COUNT; n++)
		if (MODELS[n].model == model)
			return MODELS[n].label;
	return "?";
}
