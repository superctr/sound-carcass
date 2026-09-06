/* scplay: ROM discovery.  Images are recognised by their CRC32 and size, so
 * neither file nor zip entry names matter: every zip and every loose file in
 * the search directories is looked at.
 *
 * Copyright (c) 2026 ian karlsson
 * SPDX-License-Identifier: BSD-3-Clause
 */
#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <zlib.h>
#include "roms.h"

#define PATH_LEN 512

/* ---------------------------------------------------------------- images */

enum
{
	SET_SC88_CTL, SET_SC88VL_CTL, SET_PRO_CTL, SET_8850_CTL,
	SET_SC88_WAVE, SET_PRO_WAVE, SET_8850_WAVE,
	SET_8850_BOOT, SET_8850_TONE,
	SET_NONE = -1
};

typedef struct rom_image
{
	uint32_t crc;
	uint32_t size;
	int set;
	int slot;             /* wave ROM board position */
	int rank;             /* control ROM version, the newest highest */
	const char *version;
	int word_swapped;     /* dumped byte-swapped; the library wants the 1.04 order */
	const char *role;
} rom_image_t;

static const rom_image_t IMAGES[] =
{
	{ 0x9e9c56f9, 0x080000, SET_SC88_CTL,   0, 102, "1.02", 1, "control ROM" },
	{ 0xcf953f71, 0x080000, SET_SC88_CTL,   0, 103, "1.03", 1, "control ROM" },
	{ 0x979b6c09, 0x080000, SET_SC88_CTL,   0, 104, "1.04", 0, "control ROM" },

	{ 0x66aa5762, 0x080000, SET_SC88VL_CTL, 0, 104, "1.04", 0, "control ROM" },

	{ 0xf9dd9e49, 0x200000, SET_SC88_WAVE,  0, 0, NULL, 0, "wave ROM ic14" },
	{ 0x05f939f2, 0x200000, SET_SC88_WAVE,  1, 0, NULL, 0, "wave ROM ic8" },
	{ 0xa6fc7393, 0x200000, SET_SC88_WAVE,  2, 0, NULL, 0, "wave ROM ic7" },
	{ 0x7bc514aa, 0x200000, SET_SC88_WAVE,  3, 0, NULL, 0, "wave ROM ic6" },

	{ 0x7b0d392d, 0x100000, SET_PRO_CTL,    0, 102, "1.02", 0, "control ROM" },
	{ 0x820824d2, 0x100000, SET_PRO_CTL,    0, 104, "1.04", 0, "control ROM" },

	{ 0x84dfea65, 0x400000, SET_PRO_WAVE,   0, 0, NULL, 0, "wave ROM ic20" },
	{ 0x60210227, 0x400000, SET_PRO_WAVE,   1, 0, NULL, 0, "wave ROM ic21" },
	{ 0x5f883ddd, 0x400000, SET_PRO_WAVE,   2, 0, NULL, 0, "wave ROM ic22" },
	{ 0xecb4dd39, 0x400000, SET_PRO_WAVE,   3, 0, NULL, 0, "wave ROM ic23" },
	{ 0x93541e95, 0x400000, SET_PRO_WAVE,   4, 0, NULL, 0, "wave ROM ic24" },

	{ 0x3ef69f93, 0x100000, SET_8850_CTL,   0, 100, "1.00", 0, "program flash ic9" },
	{ 0x4b2f36e3, 0x010000, SET_8850_BOOT,  0, 0, NULL, 0, "CPU ROM ic1" },
	{ 0x390faa62, 0x200000, SET_8850_TONE,  0, 0, NULL, 0, "tone flash ic10" },
	{ 0x2cfe5aa2, 0x1000000, SET_8850_WAVE, 0, 0, NULL, 0, "wave ROM ic53" },
	{ 0x623015b6, 0x1000000, SET_8850_WAVE, 1, 0, NULL, 0, "wave ROM ic54" },
};

#define IMAGE_COUNT ((int)(sizeof(IMAGES) / sizeof(IMAGES[0])))

typedef struct model_def
{
	const char *name;
	const char *label;
	scemu_model_t model;
	int control_set;
	int wave_set;
	int wave_count;
	int boot_set;         /* the SC-8850's CPU ROM and tone flash; SET_NONE elsewhere */
	int tone_set;
} model_def_t;

static const model_def_t MODELS[] =
{
	{ "sc88pro", "SC-88Pro", SCEMU_MODEL_SC88PRO, SET_PRO_CTL,    SET_PRO_WAVE,  5, SET_NONE, SET_NONE },
	{ "sc88",    "SC-88",    SCEMU_MODEL_SC88,    SET_SC88_CTL,   SET_SC88_WAVE, 4, SET_NONE, SET_NONE },
	{ "sc88vl",  "SC-88VL",  SCEMU_MODEL_SC88VL,  SET_SC88VL_CTL, SET_SC88_WAVE, 4, SET_NONE, SET_NONE },
	{ "sc8850",  "SC-8850",  SCEMU_MODEL_SC8850,  SET_8850_CTL,   SET_8850_WAVE, 2, SET_8850_BOOT, SET_8850_TONE },
};

#define MODEL_COUNT ((int)(sizeof(MODELS) / sizeof(MODELS[0])))

static int image_index(uint32_t crc, uint64_t size)
{
	for (int n = 0; n < IMAGE_COUNT; n++)
		if (IMAGES[n].crc == crc && IMAGES[n].size == size)
			return n;
	return -1;
}

static int size_is_known(uint64_t size)
{
	for (int n = 0; n < IMAGE_COUNT; n++)
		if (IMAGES[n].size == size)
			return 1;
	return 0;
}

/* ---------------------------------------------------------------- catalog */

typedef struct found
{
	char path[PATH_LEN];
	int present;
	int in_zip;
	uint32_t offset, csize, usize, method;
} found_t;

typedef struct catalog
{
	found_t found[IMAGE_COUNT];
	char rom_path[PATH_LEN];
	char exe_dir[PATH_LEN];
	int built;
} catalog_t;

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static void record(catalog_t *c, int index, const char *path, int in_zip,
                   uint32_t offset, uint32_t csize, uint32_t usize, uint32_t method)
{
	found_t *f = &c->found[index];
	if (f->present)
		return;
	f->present = 1;
	f->in_zip = in_zip;
	f->offset = offset;
	f->csize = csize;
	f->usize = usize;
	f->method = method;
	snprintf(f->path, sizeof(f->path), "%s", path);
}

/* ---------------------------------------------------------------- scanning */

static int scan_zip(catalog_t *c, const char *path)
{
	FILE *fp = fopen(path, "rb");
	if (!fp)
		return 0;
	long size = 0;
	if (fseek(fp, 0, SEEK_END) == 0)
		size = ftell(fp);
	if (size < 22)
	{
		fclose(fp);
		return 0;
	}

	long tail = size > 66000 ? 66000 : size;
	uint8_t *buf = malloc((size_t)tail);
	if (!buf)
	{
		fclose(fp);
		return 0;
	}
	if (fseek(fp, size - tail, SEEK_SET) != 0 || fread(buf, 1, (size_t)tail, fp) != (size_t)tail)
	{
		free(buf);
		fclose(fp);
		return 0;
	}
	long eocd = -1;
	for (long n = tail - 22; n >= 0; n--)
		if (buf[n] == 'P' && buf[n + 1] == 'K' && buf[n + 2] == 5 && buf[n + 3] == 6)
		{
			eocd = n;
			break;
		}
	if (eocd < 0)
	{
		free(buf);
		fclose(fp);
		return 0;
	}
	int count = rd16(buf + eocd + 10);
	uint32_t dir_size = rd32(buf + eocd + 12);
	uint32_t dir_offset = rd32(buf + eocd + 16);
	free(buf);

	if (count <= 0 || dir_size < 46 || (uint64_t)dir_offset + dir_size > (uint64_t)size)
	{
		fclose(fp);
		return 1;
	}
	uint8_t *dir = malloc(dir_size);
	if (!dir)
	{
		fclose(fp);
		return 1;
	}
	if (fseek(fp, (long)dir_offset, SEEK_SET) != 0 || fread(dir, 1, dir_size, fp) != dir_size)
	{
		free(dir);
		fclose(fp);
		return 1;
	}
	fclose(fp);

	uint32_t at = 0;
	for (int n = 0; n < count && at + 46 <= dir_size; n++)
	{
		const uint8_t *e = dir + at;
		if (rd32(e) != 0x02014b50)
			break;
		uint32_t method = rd16(e + 10);
		uint32_t crc = rd32(e + 16), csize = rd32(e + 20), usize = rd32(e + 24);
		uint32_t offset = rd32(e + 42);
		int index = image_index(crc, usize);
		if (index >= 0 && (method == 0 || method == 8))
			record(c, index, path, 1, offset, csize, usize, method);
		at += 46u + rd16(e + 28) + rd16(e + 30) + rd16(e + 32);
	}
	free(dir);
	return 1;
}

static int file_crc(const char *path, uint32_t *crc_out)
{
	FILE *fp = fopen(path, "rb");
	if (!fp)
		return 0;
	uint8_t buf[65536];
	uLong crc = crc32(0, NULL, 0);
	size_t got;
	while ((got = fread(buf, 1, sizeof(buf), fp)) > 0)
		crc = crc32(crc, buf, (uInt)got);
	int ok = !ferror(fp);
	fclose(fp);
	*crc_out = (uint32_t)crc;
	return ok;
}

static void scan_file(catalog_t *c, const char *path, uint64_t size)
{
	if (!size_is_known(size))
		return;
	uint32_t crc;
	if (!file_crc(path, &crc))
		return;
	int index = image_index(crc, size);
	if (index >= 0)
		record(c, index, path, 0, 0, 0, (uint32_t)size, 0);
}

static int name_ends_zip(const char *name)
{
	size_t len = strlen(name);
	return len > 4 && !strcasecmp(name + len - 4, ".zip");
}

static int name_compare(const void *a, const void *b)
{
	return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static void scan_dir(catalog_t *c, const char *path, int depth)
{
	DIR *d = opendir(path);
	if (!d)
		return;

	char **names = NULL;
	size_t count = 0, room = 0;
	const struct dirent *de;
	while ((de = readdir(d)))
	{
		if (de->d_name[0] == '.')
			continue;
		if (count == room)
		{
			size_t want = room ? room * 2 : 64;
			char **grown = realloc(names, want * sizeof(*names));
			if (!grown)
				break;
			names = grown;
			room = want;
		}
		names[count] = strdup(de->d_name);
		if (!names[count])
			break;
		count++;
	}
	closedir(d);
	qsort(names, count, sizeof(*names), name_compare);

	for (size_t n = 0; n < count; n++)
	{
		char child[PATH_LEN];
		struct stat st;
		snprintf(child, sizeof(child), "%s/%s", path, names[n]);
		if (stat(child, &st) == 0)
		{
			if (S_ISREG(st.st_mode))
			{
				if (name_ends_zip(names[n]))
					scan_zip(c, child);
				else
					scan_file(c, child, (uint64_t)st.st_size);
			}
			else if (S_ISDIR(st.st_mode) && depth > 0)
				scan_dir(c, child, depth - 1);
		}
		free(names[n]);
	}
	free(names);
}

static void scan_source(catalog_t *c, const char *path)
{
	struct stat st;
	if (stat(path, &st) != 0)
		return;
	if (S_ISDIR(st.st_mode))
		scan_dir(c, path, 1);
	else if (S_ISREG(st.st_mode) && !scan_zip(c, path))
		scan_file(c, path, (uint64_t)st.st_size);
}

static const catalog_t *catalog_get(const char *rom_path, const char *exe_dir)
{
	static catalog_t cache;
	const char *rom = rom_path ? rom_path : "";
	const char *exe = exe_dir ? exe_dir : "";

	if (cache.built && !strcmp(cache.rom_path, rom) && !strcmp(cache.exe_dir, exe))
		return &cache;

	memset(&cache, 0, sizeof(cache));
	snprintf(cache.rom_path, sizeof(cache.rom_path), "%s", rom);
	snprintf(cache.exe_dir, sizeof(cache.exe_dir), "%s", exe);
	cache.built = 1;

	if (*rom)
		scan_source(&cache, rom);
	if (*exe)
		scan_dir(&cache, exe, 1);
	const char *home = getenv("HOME");
	if (home)
	{
		char roms[PATH_LEN];
		snprintf(roms, sizeof(roms), "%s/.mame/roms", home);
		scan_dir(&cache, roms, 1);
	}
	return &cache;
}

/* ---------------------------------------------------------------- reading */

static void *read_whole(const char *path, size_t want)
{
	FILE *fp = fopen(path, "rb");
	if (!fp)
		return NULL;
	uint8_t *data = malloc(want);
	if (!data)
	{
		fclose(fp);
		return NULL;
	}
	size_t got = fread(data, 1, want, fp);
	int extra = fgetc(fp) != EOF;
	fclose(fp);
	if (got != want || extra)
	{
		free(data);
		return NULL;
	}
	return data;
}

static void *read_zip_entry(const found_t *f)
{
	FILE *fp = fopen(f->path, "rb");
	if (!fp)
		return NULL;
	uint8_t head[30];
	if (fseek(fp, (long)f->offset, SEEK_SET) != 0 || fread(head, 1, 30, fp) != 30
	    || rd32(head) != 0x04034b50)
	{
		fclose(fp);
		return NULL;
	}
	long data_offset = (long)f->offset + 30 + rd16(head + 26) + rd16(head + 28);
	if (fseek(fp, data_offset, SEEK_SET) != 0)
	{
		fclose(fp);
		return NULL;
	}

	uint8_t *out = malloc(f->usize);
	if (!out)
	{
		fclose(fp);
		return NULL;
	}
	if (f->method == 0)
	{
		if (f->csize != f->usize || fread(out, 1, f->usize, fp) != f->usize)
		{
			free(out);
			fclose(fp);
			return NULL;
		}
		fclose(fp);
		return out;
	}

	uint8_t *in = malloc(f->csize ? f->csize : 1);
	if (!in || fread(in, 1, f->csize, fp) != f->csize)
	{
		free(in);
		free(out);
		fclose(fp);
		return NULL;
	}
	fclose(fp);

	z_stream s;
	memset(&s, 0, sizeof(s));
	int rc = inflateInit2(&s, -MAX_WBITS);
	if (rc == Z_OK)
	{
		s.next_in = in;
		s.avail_in = f->csize;
		s.next_out = out;
		s.avail_out = f->usize;
		rc = inflate(&s, Z_FINISH);
		if (rc == Z_OK || rc == Z_STREAM_END)
			rc = s.total_out == f->usize ? Z_OK : Z_DATA_ERROR;
		inflateEnd(&s);
	}
	free(in);
	if (rc != Z_OK)
	{
		free(out);
		return NULL;
	}
	return out;
}

static void *image_read(const catalog_t *c, int index)
{
	const rom_image_t *img = &IMAGES[index];
	const found_t *f = &c->found[index];
	uint8_t *data = f->in_zip ? read_zip_entry(f) : read_whole(f->path, img->size);
	if (!data)
		return NULL;
	if (img->word_swapped)
		for (uint32_t n = 0; n + 1 < img->size; n += 2)
		{
			uint8_t t = data[n];
			data[n] = data[n + 1];
			data[n + 1] = t;
		}
	return data;
}

/* ---------------------------------------------------------------- choosing */

static int find_control(const catalog_t *c, int set)
{
	int best = -1;
	for (int n = 0; n < IMAGE_COUNT; n++)
		if (IMAGES[n].set == set && c->found[n].present
		    && (best < 0 || IMAGES[n].rank > IMAGES[best].rank))
			best = n;
	return best;
}

static int find_wave(const catalog_t *c, int set, int slot)
{
	for (int n = 0; n < IMAGE_COUNT; n++)
		if (IMAGES[n].set == set && IMAGES[n].slot == slot && c->found[n].present)
			return n;
	return -1;
}

static int missing_count(const catalog_t *c, const model_def_t *d)
{
	int missing = find_control(c, d->control_set) < 0;
	for (int n = 0; n < d->wave_count; n++)
		if (find_wave(c, d->wave_set, n) < 0)
			missing++;
	if (d->boot_set != SET_NONE && find_control(c, d->boot_set) < 0)
		missing++;
	if (d->tone_set != SET_NONE && find_control(c, d->tone_set) < 0)
		missing++;
	return missing;
}

static int set_complete(const catalog_t *c, const model_def_t *d)
{
	return missing_count(c, d) == 0;
}

static const char *size_text(uint32_t size, char *buf, size_t buf_size)
{
	if (size >= 0x100000 && size % 0x100000 == 0)
		snprintf(buf, buf_size, "%u MiB", size / 0x100000u);
	else
		snprintf(buf, buf_size, "%u KiB", size / 1024u);
	return buf;
}

static void append(char *buf, size_t size, size_t *at, const char *fmt, ...)
{
	if (*at >= size)
		return;
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf + *at, size - *at, fmt, ap);
	va_end(ap);
	if (n > 0)
		*at += (size_t)n < size - *at ? (size_t)n : size - *at - 1;
}

static void missing_message(const catalog_t *c, const model_def_t *d, char *err, size_t err_size)
{
	char text[16];
	size_t at = 0;
	const char *sep = " ";

	append(err, err_size, &at, "%s: no", d->name);
	if (find_control(c, d->control_set) < 0)
	{
		int first = 1;
		for (int n = 0; n < IMAGE_COUNT; n++)
			if (IMAGES[n].set == d->control_set)
			{
				if (first)
					append(err, err_size, &at, " control ROM (%s, crc %08x",
					       size_text(IMAGES[n].size, text, sizeof(text)), IMAGES[n].crc);
				else
					append(err, err_size, &at, "/%08x", IMAGES[n].crc);
				first = 0;
			}
		append(err, err_size, &at, ")");
		sep = ", no ";
	}
	for (int n = 0; n < d->wave_count; n++)
	{
		if (find_wave(c, d->wave_set, n) >= 0)
			continue;
		for (int i = 0; i < IMAGE_COUNT; i++)
			if (IMAGES[i].set == d->wave_set && IMAGES[i].slot == n)
			{
				append(err, err_size, &at, "%s%s (%s, crc %08x)", sep, IMAGES[i].role,
				       size_text(IMAGES[i].size, text, sizeof(text)), IMAGES[i].crc);
				sep = ", no ";
			}
	}
	const int extra[2] = { d->boot_set, d->tone_set };
	for (int e = 0; e < 2; e++)
	{
		if (extra[e] == SET_NONE || find_control(c, extra[e]) >= 0)
			continue;
		for (int i = 0; i < IMAGE_COUNT; i++)
			if (IMAGES[i].set == extra[e])
			{
				append(err, err_size, &at, "%s%s (%s, crc %08x)", sep, IMAGES[i].role,
				       size_text(IMAGES[i].size, text, sizeof(text)), IMAGES[i].crc);
				sep = ", no ";
			}
	}
}

/* ---------------------------------------------------------------- loading */

static uint64_t hash_image(uint64_t h, const rom_image_t *img)
{
	h = (h ^ img->crc) * 0x100000001b3ull;
	h = (h ^ img->size) * 0x100000001b3ull;
	return h;
}

static int load_model(scplay_roms_t *out, const model_def_t *d, const catalog_t *c,
                      char *err, size_t err_size)
{
	memset(out, 0, sizeof(*out));
	out->model = d->model;
	out->model_name = d->name;

	if (!set_complete(c, d))
	{
		missing_message(c, d, err, err_size);
		return 0;
	}

	int ctl = find_control(c, d->control_set);
	void *program = image_read(c, ctl);
	if (!program)
	{
		snprintf(err, err_size, "%s: cannot read the control ROM from %s", d->name,
		         c->found[ctl].path);
		return 0;
	}
	out->owned[out->owned_count++] = program;
	out->roms.program_rom = program;
	out->roms.program_rom_size = IMAGES[ctl].size;
	out->roms.wave_rom_count = d->wave_count;
	snprintf(out->version, sizeof(out->version), "%s", IMAGES[ctl].version);
	snprintf(out->origin, sizeof(out->origin), "%s", c->found[ctl].path);

	uint64_t h = hash_image(0xcbf29ce484222325ull, &IMAGES[ctl]);

	for (int n = 0; n < d->wave_count; n++)
	{
		int index = find_wave(c, d->wave_set, n);
		void *wave = image_read(c, index);
		if (!wave)
		{
			snprintf(err, err_size, "%s: cannot read the %s from %s", d->name,
			         IMAGES[index].role, c->found[index].path);
			scplay_roms_free(out);
			return 0;
		}
		out->owned[out->owned_count++] = wave;
		out->roms.wave_rom[n] = wave;
		out->roms.wave_rom_size[n] = IMAGES[index].size;
		h = hash_image(h, &IMAGES[index]);
	}
	if (d->boot_set != SET_NONE)
	{
		const int boot = find_control(c, d->boot_set), tone = find_control(c, d->tone_set);
		void *boot_data = image_read(c, boot);
		void *tone_data = boot_data ? image_read(c, tone) : NULL;
		if (!boot_data || !tone_data)
		{
			const int failed = boot_data ? tone : boot;
			snprintf(err, err_size, "%s: cannot read the %s from %s", d->name,
			         IMAGES[failed].role, c->found[failed].path);
			free(boot_data);
			scplay_roms_free(out);
			return 0;
		}
		out->owned[out->owned_count++] = boot_data;
		out->owned[out->owned_count++] = tone_data;
		out->roms.boot_rom = boot_data;
		out->roms.boot_rom_size = IMAGES[boot].size;
		out->roms.tone_rom = tone_data;
		out->roms.tone_rom_size = IMAGES[tone].size;
		h = hash_image(h, &IMAGES[boot]);
		h = hash_image(h, &IMAGES[tone]);
	}
	out->hash = h;
	return 1;
}

int scplay_roms_load(scplay_roms_t *out, const char *model_name, const char *rom_path,
                     const char *exe_dir, char *err, size_t err_size)
{
	const catalog_t *c = catalog_get(rom_path, exe_dir);

	if (model_name)
	{
		for (int n = 0; n < MODEL_COUNT; n++)
			if (!strcmp(MODELS[n].name, model_name))
				return load_model(out, &MODELS[n], c, err, err_size);
		snprintf(err, err_size, "unknown model %s", model_name);
		return 0;
	}

	int best = 0, found_any = 0;
	for (int n = 0; n < MODEL_COUNT; n++)
	{
		char e[256];
		if (load_model(out, &MODELS[n], c, e, sizeof(e)))
			return 1;
		if (missing_count(c, &MODELS[n]) < missing_count(c, &MODELS[best]))
			best = n;
	}
	for (int n = 0; n < IMAGE_COUNT; n++)
		found_any |= c->found[n].present;
	if (!found_any)
	{
		snprintf(err, err_size, "no ROM images at all in the places looked at");
		return 0;
	}
	char e[256];
	missing_message(c, &MODELS[best], e, sizeof(e));
	snprintf(err, err_size, "%s (and no other model's set is complete either)", e);
	return 0;
}

unsigned scplay_roms_available(const char *rom_path, const char *exe_dir)
{
	const catalog_t *c = catalog_get(rom_path, exe_dir);
	unsigned mask = 0;
	for (int n = 0; n < MODEL_COUNT; n++)
		if (set_complete(c, &MODELS[n]))
			mask |= 1u << MODELS[n].model;
	return mask;
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

const char *scplay_model_name(scemu_model_t model)
{
	for (int n = 0; n < MODEL_COUNT; n++)
		if (MODELS[n].model == model)
			return MODELS[n].name;
	return "?";
}
