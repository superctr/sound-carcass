/* scgui, the plugin: a unit -- the machine with its ROMs, cache, settings
 * memory and held keys, short of a clock.
 *
 * Copyright (c) 2026 ian karlsson
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "unit.h"
#include "roms.h"

#define TIMED_KEYS 32

struct unit
{
	scplay_roms_t roms;
	scemu_t *m;
	uint32_t rate;
	session_t session;
	scemu_computer_switch_t computer;
	bool no_cache, keep_settings;
	char rom_path[1024], exe_dir[1024];
	bool have_rom_path, have_exe_dir;

	scemu_map_t map;
	uint32_t midi_rate;
	scemu_midi_out_fn midi_out;
	void *midi_out_user;

	bool power;
	bool held[SCEMU_BUTTON_COUNT];
	struct { scemu_button_t b; bool down; uint64_t due; } timed[TIMED_KEYS];
	int timed_count;
	uint64_t frames;
	unit_panel_t last;

	/* the machine unit_prepare made, waiting for unit_replace */
	struct
	{
		bool ready, same_roms;
		scplay_roms_t roms;
		scemu_t *m;
		scemu_computer_switch_t computer;
	} next;
};

static int midi_ports(scemu_model_t model, scemu_computer_switch_t computer)
{
	return model == SCEMU_MODEL_SC8850 && computer == SCEMU_COMPUTER_MAC ? 4 : 2;
}

static void apply_settings(unit_t *u)
{
	scemu_set_map(u->m, u->map);
	scemu_set_midi_rate(u->m, u->midi_rate);
}

static void hold_keys(unit_t *u)
{
	for (int n = 0; n < SCEMU_BUTTON_COUNT; n++)
		if (u->held[n])
			scemu_button(u->m, (scemu_button_t)n, true);
}

static void key(unit_t *u, scemu_button_t b, bool down)
{
	u->held[b] = down;
	if (u->power)
		scemu_button(u->m, b, down);
}

/* the timed keys, in the order they were posted, once their time has come */
static void timed_keys(unit_t *u)
{
	int kept = 0;
	for (int n = 0; n < u->timed_count; n++)
	{
		if (u->timed[n].due <= u->frames)
			key(u, u->timed[n].b, u->timed[n].down);
		else
			u->timed[kept++] = u->timed[n];
	}
	u->timed_count = kept;
}

static void begin(unit_t *u)
{
	u->rate = scemu_sample_rate(u->m);
	session_init(&u->session, u->m, u->roms.model_name, u->roms.hash, u->computer, u->no_cache, u->keep_settings);
	scemu_set_midi_out(u->m, u->midi_out, u->midi_out_user);
}

unit_t *unit_open(const char *model_name, const char *rom_path, const char *exe_dir,
                  bool no_cache, bool keep_settings, char *err, size_t err_size)
{
	unit_t *u = calloc(1, sizeof(*u));
	if (!u)
	{
		snprintf(err, err_size, "out of memory");
		return NULL;
	}
	if (rom_path)
	{
		snprintf(u->rom_path, sizeof(u->rom_path), "%s", rom_path);
		u->have_rom_path = true;
	}
	if (exe_dir)
	{
		snprintf(u->exe_dir, sizeof(u->exe_dir), "%s", exe_dir);
		u->have_exe_dir = true;
	}
	if (!scplay_roms_load(&u->roms, model_name, rom_path, exe_dir, err, err_size))
	{
		free(u);
		return NULL;
	}
	u->m = scemu_create(u->roms.model, &u->roms.roms, NULL);
	if (!u->m)
	{
		snprintf(err, err_size, "%s", scemu_error(NULL));
		scplay_roms_free(&u->roms);
		free(u);
		return NULL;
	}
	u->computer = SCEMU_COMPUTER_MIDI;
	scemu_set_computer_switch(u->m, u->computer);
	u->no_cache = no_cache;
	u->keep_settings = keep_settings;
	u->map = SCEMU_MAP_NATIVE;
	u->midi_rate = scemu_midi_rate(u->m);
	begin(u);
	return u;
}

void unit_set_computer_switch(unit_t *u, scemu_computer_switch_t computer)
{
	if (u->power || computer == u->computer)
		return;
	u->computer = computer;
	scemu_set_computer_switch(u->m, computer);
	session_free(&u->session);
	begin(u);
}

void unit_close(unit_t *u)
{
	if (!u)
		return;
	if (u->next.ready)
	{
		scemu_destroy(u->next.m);
		if (!u->next.same_roms)
			scplay_roms_free(&u->next.roms);
	}
	if (u->power)
		session_save_settings(&u->session);
	session_free(&u->session);
	scemu_destroy(u->m);
	scplay_roms_free(&u->roms);
	free(u);
}

scemu_t *unit_machine(unit_t *u) { return u->m; }
scemu_model_t unit_model(const unit_t *u) { return u->roms.model; }
const char *unit_model_name(const unit_t *u) { return u->roms.model_name; }
const char *unit_model_label(const unit_t *u) { return scplay_model_label(u->roms.model); }
const char *unit_rom_version(const unit_t *u) { return u->roms.version; }
const char *unit_rom_origin(const unit_t *u) { return u->roms.origin; }
uint32_t unit_rate(const unit_t *u) { return u->rate; }
scemu_computer_switch_t unit_computer(const unit_t *u) { return u->computer; }
int unit_midi_ports(const unit_t *u) { return midi_ports(u->roms.model, u->computer); }

void unit_set_map(unit_t *u, scemu_map_t map)
{
	u->map = map;
	if (u->power)
		scemu_set_map(u->m, map);
}

void unit_set_midi_rate(unit_t *u, uint32_t baud)
{
	u->midi_rate = baud;
	scemu_set_midi_rate(u->m, baud);
}

scemu_map_t unit_map(const unit_t *u) { return u->map; }

void unit_set_midi_out(unit_t *u, scemu_midi_out_fn fn, void *user)
{
	u->midi_out = fn;
	u->midi_out_user = user;
	scemu_set_midi_out(u->m, fn, user);
}

bool unit_boot(unit_t *u, bool use_cache, session_progress_fn progress, void *user)
{
	u->power = true;
	hold_keys(u);
	bool up = session_boot(&u->session, use_cache, progress, user);
	apply_settings(u);
	return up;
}

void unit_power(unit_t *u, bool on)
{
	if (on && !u->power)
	{
		scemu_reset(u->m);
		unit_boot(u, false, NULL, NULL);
	}
	else if (!on && u->power)
		u->power = false;
}

bool unit_power_on(const unit_t *u) { return u->power; }
bool unit_booted_from_cache(const unit_t *u) { return u->session.from_cache; }
uint64_t unit_boot_frames(const unit_t *u) { return u->session.boot_frames; }

void unit_key(unit_t *u, scemu_button_t b, bool down)
{
	key(u, b, down);
}

void unit_key_after(unit_t *u, scemu_button_t b, bool down, unsigned ms)
{
	if (u->timed_count >= TIMED_KEYS)
		return;
	u->timed[u->timed_count].b = b;
	u->timed[u->timed_count].down = down;
	u->timed[u->timed_count].due = u->frames + (uint64_t)(ms ? ms : 1) * u->rate / 1000;
	u->timed_count++;
}

void unit_dial(unit_t *u, int steps)
{
	if (u->power)
		scemu_dial(u->m, steps);
}

void unit_send(unit_t *u, const uint8_t *bytes, size_t count)
{
	if (!u->power)
		return;
	for (int port = 0; port < unit_midi_ports(u); port++)
		scemu_midi_write(u->m, port, bytes, count, 0);
}

void unit_render(unit_t *u, int32_t *const out[2], size_t frames)
{
	if (u->power)
		scemu_render(u->m, out, frames);
	else
		for (int pair = 0; pair < 2; pair++)
			if (out[pair])
				memset(out[pair], 0, frames * 2 * sizeof(int32_t));
	u->frames += frames;
	if (u->timed_count)
		timed_keys(u);
}

uint64_t unit_frames(const unit_t *u) { return u->frames; }

bool unit_panel(unit_t *u, unit_panel_t *out)
{
	unit_panel_t s;
	memset(&s, 0, sizeof(s));
	bool glass_changed = false;
	if (u->power)
	{
		const scemu_lcd_t *lcd = scemu_lcd(u->m);
		const scemu_glcd_t *glcd = scemu_glcd(u->m);
		if (lcd)
			s.lcd = *lcd;
		s.has_glcd = glcd != NULL;
		if (glcd && (glcd->changed || !u->last.has_glcd))
		{
			s.glcd = *glcd;
			s.glcd.changed = false;
			scemu_glcd_ack(u->m);
			glass_changed = true;
		}
		else if (glcd)
			s.glcd = u->last.glcd;
		s.leds = scemu_leds(u->m);
	}
	s.power = u->power;
	bool changed = glass_changed || memcmp(&s.lcd, &u->last.lcd, sizeof(s.lcd)) != 0 || s.leds != u->last.leds
	               || s.power != u->last.power;
	u->last = s;
	*out = s;
	return changed;
}

bool unit_prepare(unit_t *u, scemu_model_t model, scemu_computer_switch_t computer, char *err, size_t err_size)
{
	err[0] = 0;
	const char *name = scplay_model_name(model);
	bool same_roms = model == u->roms.model;
	if (!name || (same_roms && computer == u->computer))
		return false;
	if (u->next.ready)
	{
		scemu_destroy(u->next.m);
		if (!u->next.same_roms)
			scplay_roms_free(&u->next.roms);
		u->next.ready = false;
	}
	if (!same_roms && !scplay_roms_load(&u->next.roms, name, u->have_rom_path ? u->rom_path : NULL,
	                                    u->have_exe_dir ? u->exe_dir : NULL, err, err_size))
		return false;
	u->next.m = scemu_create(model, same_roms ? &u->roms.roms : &u->next.roms.roms, NULL);
	if (!u->next.m)
	{
		snprintf(err, err_size, "%s", scemu_error(NULL));
		if (!same_roms)
			scplay_roms_free(&u->next.roms);
		return false;
	}
	scemu_set_computer_switch(u->next.m, computer);
	u->next.same_roms = same_roms;
	u->next.computer = computer;
	u->next.ready = true;
	return true;
}

void unit_replace(unit_t *u, session_progress_fn progress, void *user)
{
	if (!u->next.ready)
		return;
	bool was_on = u->power;
	bool other_machine = !u->next.same_roms;
	if (was_on)
		session_save_settings(&u->session);
	u->power = false;
	session_free(&u->session);
	scemu_destroy(u->m);
	if (!u->next.same_roms)
	{
		scplay_roms_free(&u->roms);
		u->roms = u->next.roms;
	}
	u->m = u->next.m;
	u->computer = u->next.computer;
	u->next.ready = false;
	memset(&u->last, 0, sizeof(u->last));
	if (other_machine)
	{
		/* the keys down, and the ones waiting for their moment, were pressed
		   on another machine's panel; the rear switch keeps its own, which the
		   panel still shows held */
		memset(u->held, 0, sizeof(u->held));
		u->timed_count = 0;
	}
	begin(u);
	if (was_on)
		unit_boot(u, true, progress, user);
}

float unit_output_trim(scemu_model_t model)
{
	return model == SCEMU_MODEL_SC8850 ? 2.5f : 2.0f;
}
