/* scplay: a booted machine, from the cache when there is one, and the
 * settings memory across sessions when asked.
 *
 * Copyright (c) 2026 ian karlsson
 * SPDX-License-Identifier: BSD-3-Clause
 */
#define _POSIX_C_SOURCE 200809L
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "session.h"

#define BLOCK 256
#define BOOT_LIMIT_SECONDS 20

void session_exe_directory(const char *argv0, char *out, size_t size)
{
	char path[PATH_MAX];
	ssize_t got = readlink("/proc/self/exe", path, sizeof(path) - 1);
	if (got > 0)
		path[got] = 0;
	else
		snprintf(path, sizeof(path), "%s", argv0 ? argv0 : "");
	char *slash = strrchr(path, '/');
	if (slash)
		*slash = 0;
	else
		snprintf(path, sizeof(path), ".");
	snprintf(out, size, "%s", path);
}

const char *session_base_name(const char *path)
{
	const char *slash = strrchr(path, '/');
	return slash ? slash + 1 : path;
}

static int make_dir(const char *path)
{
	return mkdir(path, 0755) == 0 || errno == EEXIST;
}

static int cache_dir(char *out, size_t size)
{
	char dir[900];
	const char *xdg = getenv("XDG_CACHE_HOME");
	const char *home = getenv("HOME");
	if (xdg && *xdg)
		snprintf(dir, sizeof(dir), "%s", xdg);
	else if (home && *home)
	{
		snprintf(dir, sizeof(dir), "%s/.cache", home);
		if (!make_dir(dir))
			return 0;
	}
	else
		return 0;
	if (!make_dir(dir))
		return 0;
	snprintf(out, size, "%s/scemu", dir);
	return make_dir(out);
}

static int read_file_exact(const char *path, void *buffer, size_t size)
{
	FILE *fp = fopen(path, "rb");
	if (!fp)
		return 0;
	int ok = fread(buffer, 1, size, fp) == size && fgetc(fp) == EOF;
	fclose(fp);
	return ok;
}

void session_write_file(const char *path, const void *data, size_t size)
{
	char tmp[1200];
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	FILE *fp = fopen(tmp, "wb");
	if (!fp)
		return;
	int ok = fwrite(data, 1, size, fp) == size;
	fclose(fp);
	if (ok)
		rename(tmp, path);
	else
		remove(tmp);
}

static bool load_state(scemu_t *m, const char *path)
{
	FILE *fp = fopen(path, "rb");
	if (!fp)
		return false;
	fseek(fp, 0, SEEK_END);
	long size = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	if (size <= 0)
	{
		fclose(fp);
		return false;
	}
	void *buf = malloc((size_t)size);
	bool ok = buf && fread(buf, 1, (size_t)size, fp) == (size_t)size && scemu_state_load(m, buf, (size_t)size);
	free(buf);
	fclose(fp);
	return ok;
}

static void save_state(const scemu_t *m, const char *path)
{
	size_t size = scemu_state_size(m);
	void *buf = malloc(size);
	if (!buf)
		return;
	if (scemu_state_save(m, buf, size) == size)
		session_write_file(path, buf, size);
	free(buf);
}

void session_init(session_t *s, scemu_t *m, const char *model_name, uint64_t rom_hash,
                  scemu_computer_switch_t computer, bool no_cache, bool keep_settings)
{
	static const char *const computer_tag[] = { "", "-pc1", "-pc2", "-mac" };
	char root[1024];
	memset(s, 0, sizeof(*s));
	s->m = m;
	s->keep_settings = keep_settings;
	s->have_cache = !no_cache && cache_dir(root, sizeof(root));
	s->nvram_size = scemu_nvram_size(m);
	s->nvram = malloc(s->nvram_size);
	if (!s->have_cache || !s->nvram)
	{
		s->have_cache = false;
		return;
	}
	snprintf(s->factory_file, sizeof(s->factory_file), "%s/%s-factory.nvram", root, model_name);
	snprintf(s->settings_file, sizeof(s->settings_file), "%s/%s.nvram", root, model_name);
	snprintf(s->state_file, sizeof(s->state_file), "%s/boot-%s%s-%016llx.state", root, model_name,
	         computer_tag[computer], (unsigned long long)rom_hash);
	if (keep_settings && read_file_exact(s->settings_file, s->nvram, s->nvram_size)
	    && scemu_nvram_set(m, s->nvram, s->nvram_size))
		s->have_seed = s->seed_is_user = true;
	else if (read_file_exact(s->factory_file, s->nvram, s->nvram_size)
	         && scemu_nvram_set(m, s->nvram, s->nvram_size))
		s->have_seed = true;
	/* The cached boot state is a machine that has just come up from the
	 * firmware's own factory initialisation, so it carries no settings from an
	 * earlier session; a machine seeded with the user's own settings memory is
	 * booted every time instead. */
	s->use_state = !s->seed_is_user;
}

bool session_boot(session_t *s, bool use_cache, session_progress_fn progress, void *user)
{
	uint32_t rate = scemu_sample_rate(s->m);
	s->from_cache = use_cache && s->have_cache && s->use_state && load_state(s->m, s->state_file);
	s->boot_frames = 0;
	if (s->from_cache)
		return true;

	int32_t *const out[2] = { NULL, NULL };
	bool released = false;
	while (!released && s->boot_frames < (uint64_t)BOOT_LIMIT_SECONDS * rate)
	{
		scemu_render(s->m, out, BLOCK);
		s->boot_frames += BLOCK;
		released = !scemu_muted(s->m);
		if (progress && !progress(user, s->boot_frames))
			return false;
	}
	/* A blank settings memory sends the firmware through a first power-on
	 * initialisation that takes a longer path than any later start.  Keep
	 * what that boot wrote -- it is the firmware's own factory image, not
	 * anyone's settings -- and let the next run, which starts from a machine
	 * that has been switched on before, be the one that is cached. */
	if (s->have_cache && released && use_cache)
	{
		if (!s->have_seed && scemu_nvram_get(s->m, s->nvram, s->nvram_size) == s->nvram_size)
		{
			session_write_file(s->factory_file, s->nvram, s->nvram_size);
			s->have_seed = true;
		}
		else if (s->use_state)
			save_state(s->m, s->state_file);
	}
	return released;
}

bool session_restore(session_t *s)
{
	return s->have_cache && s->use_state && load_state(s->m, s->state_file);
}

/* the cache holds a boot state per model, ROM set and switch position, and the
 * factory settings image the firmware wrote on its first run; the user's own
 * settings memory is not a cache and stays */
static bool cached_file(const char *name)
{
	const size_t n = strlen(name);
	const char *state = ".state", *factory = "-factory.nvram";
	return (n > strlen(state) && !strcmp(name + n - strlen(state), state))
	       || (n > strlen(factory) && !strcmp(name + n - strlen(factory), factory));
}

int session_clear_cache(void)
{
	char root[1024];
	if (!cache_dir(root, sizeof(root)))
		return -1;
	DIR *dir = opendir(root);
	if (!dir)
		return -1;
	int gone = 0;
	const struct dirent *entry;
	while ((entry = readdir(dir)))
	{
		char path[1200];
		if (!cached_file(entry->d_name))
			continue;
		snprintf(path, sizeof(path), "%s/%s", root, entry->d_name);
		if (unlink(path) == 0)
			gone++;
	}
	closedir(dir);
	return gone;
}

bool session_save_settings(session_t *s)
{
	if (!s->keep_settings || !s->have_cache || scemu_nvram_get(s->m, s->nvram, s->nvram_size) != s->nvram_size)
		return false;
	session_write_file(s->settings_file, s->nvram, s->nvram_size);
	return true;
}

void session_free(session_t *s)
{
	free(s->nvram);
	s->nvram = NULL;
}
