/* scemu GUI: the front panel compositor.
 *
 * Draws a model's front panel from the baked artwork (gui/panel_layout.h,
 * generated) and the machine's state: the display controller's memory, the
 * LEDs, the volume knob and the keys held down.  It has no window of its
 * own; the host gives it a 32-bit frame to fill and feeds it pointer
 * positions, which it maps back to the panel's elements.
 *
 * Copyright (c) 2026 ian karlsson
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef SCEMU_PANEL_H
#define SCEMU_PANEL_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "scemu.h"
#include "panel_layout.h"

typedef struct panel panel_t;

/* One panel per model and size; `pitch` is the glass's dot pitch in pixels,
 * one of the baked sizes (4, or 8 for twice the size).  NULL if there is no
 * such bake or the artwork does not decode. */
panel_t *panel_create(panel_model_t model, int pitch);
void panel_destroy(panel_t *p);
panel_model_t panel_model(const panel_t *p);
/* The panel a machine wears: a model with no panel of its own (the VE-GS Pro) shows the Pro's. */
panel_model_t panel_model_for(scemu_model_t model);
int panel_width(const panel_t *p);
int panel_height(const panel_t *p);
int panel_pitch(const panel_t *p);
const panel_rect_t *panel_element_rect(const panel_t *p, panel_element_t e);

/* State.  Each setter marks the panel dirty when something changed. */
void panel_set_lcd(panel_t *p, const scemu_lcd_t *lcd);
void panel_set_leds(panel_t *p, uint32_t mask);          /* scemu_leds() */
void panel_set_knob(panel_t *p, float turn);             /* 0 = fully left, 1 = fully right */
void panel_set_standby(panel_t *p, bool standby);        /* the STANDBY lamp of the soft-switched models: lit while off */
void panel_set_pressed(panel_t *p, panel_element_t e, bool down);
bool panel_dirty(const panel_t *p);

/* Draw the whole panel into `pixels`, 0xAARRGGBB, `stride` pixels per row,
 * at least panel_width() x panel_height().  Clears the dirty flag. */
void panel_render(panel_t *p, uint32_t *pixels, size_t stride);

/* The element under a point, or -1.  Keys win over what they sit on. */
int panel_hit(const panel_t *p, int x, int y);

/* The scemu button an element is, or -1 for the host-side ones. */
int panel_element_button(panel_element_t e);

#endif
