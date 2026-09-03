/* scplay: finding and loading a machine's ROM images.
 *
 * Copyright (c) 2026 ian karlsson
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef SCPLAY_ROMS_H
#define SCPLAY_ROMS_H

#include <stddef.h>
#include <stdint.h>
#include "scemu.h"

#define SCPLAY_MAX_SOURCES 16

typedef struct scplay_roms
{
	scemu_model_t model;
	const char *model_name;
	scemu_roms_t roms;
	void *owned[1 + SCEMU_MAX_WAVE_ROMS];
	int owned_count;
	uint64_t hash;
	char origin[512];
} scplay_roms_t;

/* model_name NULL means "try them all, best first"; rom_path is --rom, or NULL.
 * Returns 1 on success, 0 with a message in err. */
int scplay_roms_load(scplay_roms_t *out, const char *model_name, const char *rom_path,
                     const char *exe_dir, char *err, size_t err_size);
void scplay_roms_free(scplay_roms_t *r);

const char *scplay_model_label(scemu_model_t model);

#endif
