/* scemu GUI: the front panel under a pointer.
 *
 * What a click, a drag and the wheel mean on the panel: which key an
 * element is and the other half of its pair, the keys the right button
 * queues or holds down (the middle one when the two are swapped), the value dial turned by hand, the wheel over the
 * dial and the volume knob, the power switch with keys held through the
 * boot, and the button combinations from the manual played out on the
 * machine's clock.  No toolkit: the host feeds it pointer events in panel
 * pixels and it calls back with what the machine is to do and what the
 * host is to show, so the desktop player and the plugin share it.
 *
 * Copyright (c) 2026 ian karlsson
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef SCEMU_CONTROLS_H
#define SCEMU_CONTROLS_H

#include <stdbool.h>
#include <stdint.h>
#include "scemu.h"
#include "panel.h"
#include "combos.h"

/* pointer buttons and modifiers, as the host reports them */
enum { CONTROLS_BUTTON_LEFT = 1, CONTROLS_BUTTON_MIDDLE = 2, CONTROLS_BUTTON_RIGHT = 3 };
enum { CONTROLS_SHIFT = 1, CONTROLS_CONTROL = 2 };

typedef struct controls_actions
{
	void (*key)(void *user, scemu_button_t b, bool down);
	/* the same, ms of the machine's time later, in the order posted */
	void (*key_after)(void *user, scemu_button_t b, bool down, unsigned ms);
	void (*dial)(void *user, int steps);             /* the value dial, positive clockwise */
	void (*power)(void *user, bool on);               /* the hard power switch */
	void (*knob)(void *user, float turn);             /* the volume knob moved: 0 silent, 1 full */
	/* a host-side element (a jack, the logo) clicked with the left button, at the pointer */
	void (*element)(void *user, panel_element_t e, double x, double y);
	/* the host shows the combinations a key takes part in (combos_for) */
	void (*combo_menu)(void *user, panel_element_t e, double x, double y);
} controls_actions_t;

typedef struct controls
{
	panel_t *panel;
	const controls_actions_t *act;
	void *user;
	bool power;
	bool soft_power;           /* the power key is a position in the machine's own matrix (the SC-55mkII) */
	float knob;
	int knob_notches;          /* wheel notches from silent to full */
	bool swap_buttons;         /* the right button opens the combination menu, the middle one queues keys */
	int pressed_element;       /* the element under the held left button, or -1 */
	int opposite_element;      /* the other half of the pair, pressed with the right button meanwhile */
	uint64_t queued;           /* elements queued with the right button: they go down with the next key */
	uint64_t held;             /* elements the right button holds down right now */
	bool release_after_boot;   /* the held keys come up when the boot they were held through ends */
	uint64_t macro_pressed;    /* elements shown pressed while a chosen combination plays out */
	bool macro_after_boot;
	unsigned macro_ms;         /* how long it plays after the boot */
	bool dial_drag;            /* the left button is turning the value dial */
	double dial_angle, dial_rest;   /* where it was last seen, and the part of a detent left over */
	double wheel_rest;         /* the part of a wheel notch over the dial not yet a detent */
} controls_t;

void controls_init(controls_t *c, panel_t *panel, const controls_actions_t *act, void *user);
/* another model's panel, the knob carried over */
void controls_set_panel(controls_t *c, panel_t *panel);
void controls_set_soft_power(controls_t *c, bool soft);
void controls_set_knob(controls_t *c, float turn);
void controls_set_knob_notches(controls_t *c, int notches);
/* the right button and the middle one change places, so the combination menu
 * is a right-click and the queue-and-hold gesture a middle-click */
void controls_set_swap_buttons(controls_t *c, bool swap);

/* Pointer events in panel pixels.  A scroll returns true when the panel took it. */
void controls_press(controls_t *c, int button, unsigned mods, double x, double y);
void controls_release(controls_t *c, int button);
void controls_motion(controls_t *c, double x, double y);
bool controls_scroll(controls_t *c, double x, double y, double dy);

/* The host's own hand on the power: off and on again, keys held. */
void controls_power_cycle(controls_t *c);

/* The machine has come up from a boot: the keys held through it come up.
 * Returns the ms after which the host is to call controls_macro_done for a
 * combination that was waiting on the boot, 0 when none was. */
unsigned controls_boot_done(controls_t *c);

/* A combination from the manual plays out on the machine's clock: the held
 * keys go down in order, the pressed one follows, everything comes up in
 * reverse, and the panel shows the keys pressed meanwhile.  Returns the ms
 * after which the host is to call controls_macro_done; 0 when the
 * combination follows a boot (controls_boot_done then says) or when one is
 * already playing. */
unsigned controls_play_combo(controls_t *c, const combo_t *combo);
void controls_macro_done(controls_t *c);
/* the keys of a combination light up on the panel, for a menu's hover */
void controls_highlight(controls_t *c, const combo_t *combo, bool on);
/* every highlight off, the keys queued, held or playing shown again */
void controls_highlight_clear(controls_t *c);

#endif
