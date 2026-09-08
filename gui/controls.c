/* scemu GUI: the front panel under a pointer.
 *
 * Copyright (c) 2026 ian karlsson
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <math.h>
#include "controls.h"

#define DEGREES_PER_RADIAN (180 / 3.14159265358979323846)

#define DIAL_DEGREES (360.0 / PANEL_DIAL_FRAMES)   /* what the hand turns for one detent */
#define MACRO_HOLD_MS 300    /* a held key is seen held before the next goes down */
#define MACRO_PRESS_MS 150
#define KNOB_NOTCHES_DEFAULT 20

void controls_init(controls_t *c, panel_t *panel, const controls_actions_t *act, void *user)
{
	*c = (controls_t){ 0 };
	c->panel = panel;
	c->act = act;
	c->user = user;
	c->power = true;
	c->knob_notches = KNOB_NOTCHES_DEFAULT;
	c->pressed_element = c->opposite_element = -1;
}

void controls_set_panel(controls_t *c, panel_t *panel)
{
	c->panel = panel;
	panel_set_knob(panel, c->knob);
}

void controls_set_soft_power(controls_t *c, bool soft) { c->soft_power = soft; }

void controls_set_knob(controls_t *c, float turn)
{
	c->knob = turn < 0 ? 0 : turn > 1 ? 1 : turn;
	panel_set_knob(c->panel, c->knob);
}

void controls_set_knob_notches(controls_t *c, int notches) { c->knob_notches = notches; }

/* the other half of a ◀ ▶ pair, or -1 */
static int opposite_button(int b)
{
	if (b == SCEMU_BUTTON_DEC)
		return SCEMU_BUTTON_INC;
	if (b == SCEMU_BUTTON_INC)
		return SCEMU_BUTTON_DEC;
	bool pair = (b >= SCEMU_BUTTON_PART_LEFT && b <= SCEMU_BUTTON_MIDI_CH_RIGHT)
	            || (b >= SCEMU_BUTTON_EDIT1_LEFT && b <= SCEMU_BUTTON_EDIT3_RIGHT);
	return pair ? SCEMU_BUTTON_PART_LEFT + ((b - SCEMU_BUTTON_PART_LEFT) ^ 1) : -1;
}

static int element_for_button(scemu_button_t b)
{
	for (int e = 0; e < PANEL_ELEMENT_COUNT; e++)
		if (panel_element_button((panel_element_t)e) == (int)b)
			return e;
	return -1;
}

static void key(controls_t *c, int e, bool down)
{
	c->act->key(c->user, (scemu_button_t)panel_element_button((panel_element_t)e), down);
}

/* the queued keys go down, in the order they were queued, before the key they modify */
static void queued_press(controls_t *c)
{
	for (int e = 0; e < PANEL_ELEMENT_COUNT; e++)
		if ((c->queued >> e) & 1)
			key(c, e, true);
	c->held |= c->queued;
	c->queued = 0;
}

/* everything the right button holds or queues comes up */
static void held_release(controls_t *c)
{
	for (int e = 0; e < PANEL_ELEMENT_COUNT; e++)
	{
		if ((c->held >> e) & 1)
			key(c, e, false);
		if (((c->held | c->queued) >> e) & 1)
			panel_set_pressed(c->panel, (panel_element_t)e, false);
	}
	c->held = c->queued = 0;
}

static void set_power(controls_t *c, bool on)
{
	c->power = on;
	c->act->power(c->user, on);
}

static void element_action(controls_t *c, int e, double x, double y)
{
	if (e == PANEL_SWITCH_POWER)
	{
		/* the SC-55mkII's is a position in its own switch matrix: the machine puts itself
		   into standby and keeps running, and its own lamp says so */
		if (c->soft_power)
		{
			c->act->key(c->user, SCEMU_BUTTON_POWER, true);
			c->act->key_after(c->user, SCEMU_BUTTON_POWER, false, 100);
			return;
		}
		bool on = !c->power;
		if (on && (c->queued | c->held))
		{
			queued_press(c);
			c->release_after_boot = true;
		}
		set_power(c, on);
		return;
	}
	c->act->element(c->user, (panel_element_t)e, x, y);
}

static double dial_angle_at(controls_t *c, double x, double y)
{
	const panel_rect_t *r = panel_element_rect(c->panel, PANEL_DIAL_VALUE);
	double cx = r->x + r->w / 2.0, cy = r->y + r->h / 2.0;
	return atan2(y - cy, x - cx) * DEGREES_PER_RADIAN;
}

/* the dial follows the pointer around it, a twenty-fourth of a turn to the
 * detent, so the knurl on the panel turns with the hand */
static void dial_turn(controls_t *c, double x, double y)
{
	double angle = dial_angle_at(c, x, y), turn = angle - c->dial_angle;
	if (turn > 180)
		turn -= 360;
	else if (turn < -180)
		turn += 360;
	c->dial_angle = angle;
	c->dial_rest += turn;
	int steps = (int)(c->dial_rest / DIAL_DEGREES);
	if (!steps)
		return;
	c->dial_rest -= steps * DIAL_DEGREES;
	c->act->dial(c->user, steps);
	panel_set_dial(c->panel, steps);
}

void controls_press(controls_t *c, int button, unsigned mods, double x, double y)
{
	int e = panel_hit(c->panel, (int)x, (int)y);
	if (e < 0)
		return;
	int b = panel_element_button((panel_element_t)e);
	if (b >= 0)
	{
		if (button == CONTROLS_BUTTON_RIGHT && c->pressed_element >= 0)
		{
			/* the right button while a half of a pair is held: its other half */
			int held = panel_element_button((panel_element_t)c->pressed_element);
			int other = opposite_button(held);
			int oe = other >= 0 ? element_for_button((scemu_button_t)other) : -1;
			if (oe >= 0 && c->opposite_element < 0)
			{
				c->opposite_element = oe;
				c->act->key(c->user, (scemu_button_t)other, true);
				panel_set_pressed(c->panel, (panel_element_t)oe, true);
			}
		}
		else if (button == CONTROLS_BUTTON_MIDDLE || (button == CONTROLS_BUTTON_RIGHT && (mods & CONTROLS_CONTROL)))
			c->act->combo_menu(c->user, (panel_element_t)e, x, y);
		else if (button == CONTROLS_BUTTON_RIGHT)
		{
			/* the right button queues a key for the next one, or with Shift
			 * holds it down from now; either again lets it go */
			uint64_t bit = (uint64_t)1 << e;
			if ((c->held | c->queued) & bit)
			{
				if (c->held & bit)
					c->act->key(c->user, (scemu_button_t)b, false);
				c->held &= ~bit;
				c->queued &= ~bit;
			}
			else if (mods & CONTROLS_SHIFT)
			{
				c->held |= bit;
				c->act->key(c->user, (scemu_button_t)b, true);
			}
			else
				c->queued |= bit;
			panel_set_pressed(c->panel, (panel_element_t)e, ((c->held | c->queued) & bit) != 0);
		}
		else if (button == CONTROLS_BUTTON_LEFT)
		{
			if (c->queued)
				queued_press(c);
			c->pressed_element = e;
			c->act->key(c->user, (scemu_button_t)b, true);
			panel_set_pressed(c->panel, (panel_element_t)e, true);
		}
	}
	else if (button == CONTROLS_BUTTON_LEFT)
	{
		if (e == PANEL_DIAL_VALUE)
		{
			c->dial_drag = true;
			c->dial_angle = dial_angle_at(c, x, y);
			c->dial_rest = 0;
		}
		else
			element_action(c, e, x, y);
	}
}

void controls_release(controls_t *c, int button)
{
	if (button == CONTROLS_BUTTON_RIGHT)
	{
		int oe = c->opposite_element;
		if (oe >= 0)
		{
			c->opposite_element = -1;
			key(c, oe, false);
			panel_set_pressed(c->panel, (panel_element_t)oe, false);
		}
		return;
	}
	if (button != CONTROLS_BUTTON_LEFT)
		return;
	c->dial_drag = false;
	int e = c->pressed_element;
	if (e < 0)
		return;
	c->pressed_element = -1;
	key(c, e, false);
	panel_set_pressed(c->panel, (panel_element_t)e, false);
	if (c->held | c->queued)
		held_release(c);
}

void controls_motion(controls_t *c, double x, double y)
{
	if (c->dial_drag)
		dial_turn(c, x, y);
}

bool controls_scroll(controls_t *c, double x, double y, double dy)
{
	int e = panel_hit(c->panel, (int)x, (int)y);
	if (e == PANEL_DIAL_VALUE || e == PANEL_BUTTON_VALUE)
	{
		/* a notch of the wheel is one detent of the dial, whether the notch
		 * arrives whole or as the several fractions a smooth wheel sends */
		c->wheel_rest -= dy;
		int steps = (int)c->wheel_rest;
		if (steps)
		{
			c->wheel_rest -= steps;
			c->act->dial(c->user, steps);
			panel_set_dial(c->panel, steps);
		}
		return true;
	}
	if (e != PANEL_KNOB_VOLUME && e != PANEL_BUTTON_PREVIEW)
		return false;
	c->knob -= (float)dy / (float)(c->knob_notches > 0 ? c->knob_notches : KNOB_NOTCHES_DEFAULT);
	c->knob = c->knob < 0 ? 0 : c->knob > 1 ? 1 : c->knob;
	panel_set_knob(c->panel, c->knob);
	c->act->knob(c->user, c->knob);
	return true;
}

void controls_power_cycle(controls_t *c)
{
	if (c->power)
		c->act->power(c->user, false);
	set_power(c, true);
}

unsigned controls_boot_done(controls_t *c)
{
	if (c->release_after_boot)
	{
		c->release_after_boot = false;
		held_release(c);
	}
	if (!c->macro_after_boot)
		return 0;
	c->macro_after_boot = false;
	return c->macro_ms;
}

static void macro_key(controls_t *c, scemu_button_t b, bool down, unsigned ms)
{
	c->act->key_after(c->user, b, down, ms);
	int e = element_for_button(b);
	if (e >= 0 && down)
	{
		c->macro_pressed |= (uint64_t)1 << e;
		panel_set_pressed(c->panel, (panel_element_t)e, true);
	}
}

void controls_macro_done(controls_t *c)
{
	for (int e = 0; e < PANEL_ELEMENT_COUNT; e++)
		if ((c->macro_pressed >> e) & 1)
			panel_set_pressed(c->panel, (panel_element_t)e, ((c->queued | c->held) >> e) & 1);
	c->macro_pressed = 0;
}

/* the held keys go down in order, the pressed one follows (at once when the
 * manual says "simultaneously", after a moment when it says "while
 * holding"), and everything comes up in reverse */
unsigned controls_play_combo(controls_t *c, const combo_t *combo)
{
	if (c->macro_pressed || c->release_after_boot)
		return 0;
	int first = combo->timing == COMBO_HOLD_THEN_PAIR && !combo->power_on ? combo->hold_count - 1
	                                                                     : combo->hold_count;
	for (int n = 0; n < first; n++)
		macro_key(c, combo->hold[n], true, 0);
	if (combo->power_on)
		controls_power_cycle(c);   /* the held keys go through a power cycle; the rest follows the boot */
	unsigned t = combo->timing != COMBO_TOGETHER || combo->power_on ? MACRO_HOLD_MS : 0;
	for (int n = first; n < combo->hold_count; n++)
		macro_key(c, combo->hold[n], true, t);
	if (combo->press != SCEMU_BUTTON_COUNT)
		macro_key(c, combo->press, true, t);
	t += MACRO_PRESS_MS;
	if (combo->press != SCEMU_BUTTON_COUNT)
		macro_key(c, combo->press, false, t);
	for (int n = combo->hold_count - 1; n >= 0; n--)
		macro_key(c, combo->hold[n], false, t);
	c->macro_ms = t + 50;
	if (combo->power_on)
	{
		c->macro_after_boot = true;
		return 0;
	}
	return c->macro_ms;
}

void controls_highlight(controls_t *c, const combo_t *combo, bool on)
{
	for (int k = 0; k < combo->hold_count; k++)
	{
		int e = element_for_button(combo->hold[k]);
		if (e >= 0)
			panel_set_pressed(c->panel, (panel_element_t)e, on);
	}
	if (combo->press != SCEMU_BUTTON_COUNT)
	{
		int e = element_for_button(combo->press);
		if (e >= 0)
			panel_set_pressed(c->panel, (panel_element_t)e, on);
	}
}

void controls_highlight_clear(controls_t *c)
{
	for (int n = 0; n < combo_count; n++)
		controls_highlight(c, &combos[n], false);
	for (int e = 0; e < PANEL_ELEMENT_COUNT; e++)
		if (((c->queued | c->held | c->macro_pressed) >> e) & 1)
			panel_set_pressed(c->panel, (panel_element_t)e, true);
}
