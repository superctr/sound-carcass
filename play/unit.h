/* scgui, the plugin: a unit -- the ROMs, the machine, its boot cache and
 * settings memory, the keys held on its panel -- short of a clock.
 *
 * The caller renders when it wants frames, from whichever thread drives the
 * machine, and nothing here is thread-safe: a player puts it on a thread of
 * its own paced by the sound device, a plugin under the host's process
 * call.  The machine's inputs (MIDI, the map, the rate) are reached through
 * unit_machine().
 *
 * Copyright (c) 2026 ian karlsson
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef SCPLAY_UNIT_H
#define SCPLAY_UNIT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "scemu.h"
#include "session.h"

typedef struct unit unit_t;

/* What the panel shows of the machine. */
typedef struct unit_panel
{
	scemu_lcd_t lcd;
	scemu_glcd_t glcd;
	bool has_glcd;            /* the glass is the bitmap, not the character cells */
	uint32_t leds;
	bool power;
} unit_panel_t;

/* Loads the ROMs (model_name NULL for the best set found, rom_path --rom or
 * NULL, exe_dir where to look beside the program) and creates the machine,
 * off, its rear switch on MIDI.  NULL with a message in err. */
unit_t *unit_open(const char *model_name, const char *rom_path, const char *exe_dir,
                  bool no_cache, bool keep_settings, char *err, size_t err_size);
/* The rear switch, before the first boot: the firmware reads it then and
 * never again, and the boot cache is kept per position.  Ignored once the
 * unit is on; a running unit changes it through unit_prepare. */
void unit_set_computer_switch(unit_t *u, scemu_computer_switch_t computer);
/* Saves the settings memory when it is kept, and lets everything go. */
void unit_close(unit_t *u);

scemu_t *unit_machine(unit_t *u);
scemu_model_t unit_model(const unit_t *u);
const char *unit_model_name(const unit_t *u);     /* "sc88pro" */
const char *unit_model_label(const unit_t *u);    /* "SC-88Pro" */
const char *unit_rom_version(const unit_t *u);    /* the control ROM's */
const char *unit_rom_origin(const unit_t *u);     /* the file it came from */
uint32_t unit_rate(const unit_t *u);
scemu_computer_switch_t unit_computer(const unit_t *u);
/* the port groups the machine takes: 4 on an SC-8850 whose switch is on USB, 2 on everything else */
int unit_midi_ports(const unit_t *u);

/* The host's settings the machine keeps: applied now and again after every boot. */
void unit_set_map(unit_t *u, scemu_map_t map);
void unit_set_midi_rate(unit_t *u, uint32_t baud);
scemu_map_t unit_map(const unit_t *u);
/* the machine's MIDI OUT, kept across a replacement */
void unit_set_midi_out(unit_t *u, scemu_midi_out_fn fn, void *user);

/* Switches the unit on: from the boot cache when use_cache and there is
 * one, else the firmware boots here, calling progress every block.  The
 * keys held are down through it.  Returns true when the machine came up. */
bool unit_boot(unit_t *u, bool use_cache, session_progress_fn progress, void *user);
/* Off: the machine stands still and renders silence.  On again: a power-on
 * reset and a boot from cold, the keys held. */
void unit_power(unit_t *u, bool on);
bool unit_power_on(const unit_t *u);
/* The last boot, as the session saw it: from the cache, or how long it took. */
bool unit_booted_from_cache(const unit_t *u);
uint64_t unit_boot_frames(const unit_t *u);

/* A key: remembered while the unit is off, so it is held through the boot. */
void unit_key(unit_t *u, scemu_button_t b, bool down);
/* The same, ms of the machine's own time later (it stands still while the
 * machine boots), in the order posted; for the panel's key combinations. */
void unit_key_after(unit_t *u, scemu_button_t b, bool down, unsigned ms);
void unit_dial(unit_t *u, int steps);
/* one message to every port group the machine takes, now */
void unit_send(unit_t *u, const uint8_t *bytes, size_t count);

/* `frames` frames as scemu_render, silence while off; the timed keys whose
 * time has come go down or up. */
void unit_render(unit_t *u, int32_t *const out[2], size_t frames);
uint64_t unit_frames(const unit_t *u);     /* rendered since the unit was opened */

/* The glass, the lamps and the power, as the panel wants them: the graphic
 * glass comes whole when it has changed or when the last look had none
 * (a fresh machine, or one just switched on), else as it was.  Returns
 * true when anything differs from the last call. */
bool unit_panel(unit_t *u, unit_panel_t *out);

/* Another machine in place of this one -- another model, or the same one
 * with its rear switch in another position, which the firmware reads only
 * at boot -- in two steps, so the caller can quiet the old one between
 * them.  unit_prepare loads the new set and creates the instance: false
 * with err empty when there is nothing to change, false with a message
 * when the set is not there, the old machine untouched either way.
 * unit_replace retires the old one, saving its settings memory, and boots
 * the new one from its cache if the old one was on. */
bool unit_prepare(unit_t *u, scemu_model_t model, scemu_computer_switch_t computer, char *err, size_t err_size);
void unit_replace(unit_t *u, session_progress_fn progress, void *user);

/* The factor a host puts on the words: 2, and 2.5 on the SC-8850, which plays
 * 2 dB under the rest of them.  The SC-88 family's 6 dB under the SC-55mkII
 * stands as the machines have it. */
float unit_output_trim(scemu_model_t model);

#endif
