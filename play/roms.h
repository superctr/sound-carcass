/* scplay: finding and loading a machine's ROM images.
 *
 * Copyright (c) 2026 ian karlsson
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef SCPLAY_ROMS_H
#define SCPLAY_ROMS_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
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

/* Where a machine's ROMs come from.  Every string may be empty.  path is --rom,
 * a zip or a directory, looked at before the usual places; exe_dir is the
 * program's own directory, looked at too.  control, internal and wave name, per
 * model, a file taken as its control ROM (in the byte order of the newest
 * dump), one taken as its CPU's internal ROM (the SC-55s', the SC-8820's and
 * the SC-8850's; ignored elsewhere) and one taken as its wave set, descrambled
 * and joined, whatever their CRCs: only their sizes are checked. */
#define SCPLAY_PATH_LEN 512

typedef struct scplay_rom_source
{
	char path[SCPLAY_PATH_LEN];
	char exe_dir[SCPLAY_PATH_LEN];
	char control[SCEMU_MODEL_COUNT][SCPLAY_PATH_LEN];
	char internal[SCEMU_MODEL_COUNT][SCPLAY_PATH_LEN];
	char wave[SCEMU_MODEL_COUNT][SCPLAY_PATH_LEN];
} scplay_rom_source_t;

/* path and exe_dir may be NULL; no overrides */
void scplay_rom_source_init(scplay_rom_source_t *s, const char *path, const char *exe_dir);
/* a model's override files: "" for none, NULL leaves one as it is; false when
 * the name is no model */
bool scplay_rom_source_override(scplay_rom_source_t *s, const char *model_name, const char *control,
                                const char *internal, const char *wave);

/* model_name NULL means "try them all, best first"; source NULL means the usual
 * places alone.  Returns 1 on success, 0 with a message in err. */
int scplay_roms_load(scplay_roms_t *out, const char *model_name, const scplay_rom_source_t *source,
                     char *err, size_t err_size);
void scplay_roms_free(scplay_roms_t *r);

/* Bit per scemu_model_t: the models whose complete set is there, no image read. */
unsigned scplay_roms_available(const scplay_rom_source_t *source);
/* true when the model's set is complete; otherwise text says what is missing */
bool scplay_roms_status(const scplay_rom_source_t *source, scemu_model_t model, char *text, size_t size);
/* The places are scanned once and remembered; this forgets them, so the next
 * call looks again (files added or taken away meanwhile).  The scan is not
 * shared between threads: one thread at a time asks. */
void scplay_roms_rescan(void);

/* the model a name ("sc88pro") stands for, or -1 */
int scplay_model_index(const char *name);
const char *scplay_model_label(scemu_model_t model);
const char *scplay_model_name(scemu_model_t model);
/* the power key is a standby key in the machine's own matrix, with the STANDBY lamp beside it */
bool scplay_model_standby_key(scemu_model_t model);

#endif
