/* scemu plugin: the front panel in a host's window.
 *
 * A PUGL view drawing the panel compositor's frame (gui/panel.c) as one
 * OpenGL texture, scaled to whatever size the host gives it, with the
 * pointer logic of gui/controls.c behind it.  Floating or embedded in the
 * host's native window; the host pumps it from its main thread through
 * window_tick.  No thread of its own, and no idea of the machine: it is
 * fed the panel's state and calls back with what the machine is to do.
 *
 * Copyright (c) 2026 ian karlsson
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef SCEMU_PLUGIN_WINDOW_H
#define SCEMU_PLUGIN_WINDOW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "scemu.h"
#include "controls.h"
#include "unit.h"

typedef struct window window_t;

typedef struct window_actions
{
	controls_actions_t controls;   /* the panel's: keys, dial, power, knob */
	void (*closed)(void *user);    /* the user closed a floating window */
} window_actions_t;

/* The panel of a model, at the small bake times `scale`.  The window's
 * name is CLAP's for the platform: "x11" or "win32". */
window_t *window_create(scemu_model_t model, double scale, const window_actions_t *act, void *user,
                        char *err, size_t err_size);
void window_destroy(window_t *w);
const char *window_api(void);

/* Before it is shown: the host's window to sit in, or the one to stay above. */
bool window_set_parent(window_t *w, uintptr_t native);
bool window_set_transient(window_t *w, uintptr_t native);
void window_set_title(window_t *w, const char *title);

/* Size in pixels.  The panel keeps its shape: window_fit rounds a wanted
 * size to the nearest one with the panel's aspect ratio. */
bool window_set_scale(window_t *w, double scale);
void window_size(const window_t *w, uint32_t *width, uint32_t *height);
void window_fit(const window_t *w, uint32_t *width, uint32_t *height);
bool window_set_size(window_t *w, uint32_t width, uint32_t height);

bool window_show(window_t *w);
void window_hide(window_t *w);

/* What the panel shows.  The message, when there is one, is written over
 * the dark panel: the reason there is no machine. */
void window_set_panel(window_t *w, const unit_panel_t *state);
void window_set_knob(window_t *w, float turn);
void window_set_message(window_t *w, const char *text);
/* the keys held through a boot come up */
void window_boot_done(window_t *w);

/* From the host's timer: pointer events in, a redraw when anything changed. */
void window_tick(window_t *w);

#endif
