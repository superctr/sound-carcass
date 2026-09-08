/* scplay: a booted machine, from the cache when there is one, and the
 * settings memory across sessions when asked.
 *
 * Copyright (c) 2026 ian karlsson
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef SCPLAY_SESSION_H
#define SCPLAY_SESSION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "scemu.h"

typedef struct session
{
	scemu_t *m;
	bool have_cache, have_seed, seed_is_user, use_state, keep_settings;
	char state_file[1100], factory_file[1100], settings_file[1100];
	uint8_t *nvram;
	size_t nvram_size;
	bool from_cache;
	uint64_t boot_frames;
} session_t;

/* Called every block of the boot with the frames run so far; return false to
 * stop it. */
typedef bool (*session_progress_fn)(void *user, uint64_t frames);

/* Sets up the cache files for this model and ROM set, and seeds the machine's
 * settings memory: the user's own when keep_settings and it exists, else the
 * firmware's factory image from an earlier run.  The boot state is kept per
 * position of the rear computer switch: the firmware's boot path depends on
 * it. */
void session_init(session_t *s, scemu_t *m, const char *model_name, uint64_t rom_hash,
                  scemu_computer_switch_t computer, bool no_cache, bool keep_settings);

/* Runs the firmware until it releases the mute, restoring the cached boot
 * state instead when use_cache and there is one.  Returns true when the
 * machine came up; s->from_cache and s->boot_frames say how. */
bool session_boot(session_t *s, bool use_cache, session_progress_fn progress, void *user);

/* Loads the cached boot state again, for a fresh machine without a boot;
 * false if there is none. */
bool session_restore(session_t *s);

/* Throws away the cached boot states and factory settings images of every
 * model, so the next boot runs the firmware again; the machines' own settings
 * memory stays.  Returns how many files went, -1 without a cache directory. */
int session_clear_cache(void);

/* Writes the settings memory when keep_settings; true if it did. */
bool session_save_settings(session_t *s);
void session_free(session_t *s);

void session_exe_directory(const char *argv0, char *out, size_t size);
const char *session_base_name(const char *path);
void session_write_file(const char *path, const void *data, size_t size);

#endif
