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

typedef struct scplay_roms
{
	scemu_model_t model;
	const char *model_name;
	scemu_roms_t roms;
	void *owned[3 + SCEMU_MAX_WAVE_ROMS];
	int owned_count;
	uint64_t hash;           /* the chosen images' identity, stable across runs */
	char version[16];        /* the control ROM version taken */
	char origin[512];        /* the file the control ROM came from */
} scplay_roms_t;

/* model_name NULL means "try them all, best first"; rom_path is --rom, or NULL.
 * Returns 1 on success, 0 with a message in err. */
int scplay_roms_load(scplay_roms_t *out, const char *model_name, const char *rom_path,
                     const char *exe_dir, char *err, size_t err_size);
void scplay_roms_free(scplay_roms_t *r);

/* Bit per scemu_model_t: the models whose complete set is there, no image read. */
unsigned scplay_roms_available(const char *rom_path, const char *exe_dir);

const char *scplay_model_label(scemu_model_t model);
const char *scplay_model_name(scemu_model_t model);

#endif
