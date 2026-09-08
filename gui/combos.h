/* scemu GUI: the front-panel button combinations each model understands.
 *
 * One mouse cannot hold two keys, so a right-click on a panel button offers
 * the combinations that button takes part in.  The table is compiled from the
 * owner's manual of every model and from the service notes where they go
 * further; docs/panel/combinations.md carries the same rows with the manuals'
 * wording, the preconditions and the ambiguities.
 *
 * Copyright (c) 2026 ian karlsson
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef SCEMU_COMBOS_H
#define SCEMU_COMBOS_H

#include <stdbool.h>
#include <stddef.h>
#include "scemu.h"
#include "panel_layout.h"

/* How the firmware wants the keys: the manuals' [A]*[B] means both keys are
 * pressed at once and must reach it in one scan, their [A]+[B] means [A] is
 * held, and seen held, before [B] goes down; [A]+[B]*[C] holds [A] first and
 * then presses the other two at once. */
typedef enum combo_timing
{
	COMBO_TOGETHER,
	COMBO_HOLD_THEN_PRESS,
	COMBO_HOLD_THEN_PAIR
} combo_timing_t;

typedef struct combo
{
	const char *name;           /* the manual's name for the operation */
	const char *effect;         /* one sentence */
	const char *mode;           /* the display or edit mode the combination applies in, in the
	                               manual's words; NULL when it applies anywhere */
	panel_model_t panel;        /* the panel it belongs to */
	scemu_button_t hold[4];     /* held first, in this order; with COMBO_HOLD_THEN_PAIR the last
	                               of them goes down with `press` */
	int hold_count;
	scemu_button_t press;       /* pressed while they are held; SCEMU_BUTTON_COUNT if none */
	bool power_on;              /* the hold set is held while switching on */
	const char *page;           /* where it is written down: "manual p.25", "service notes p.4" */
	combo_timing_t timing;
} combo_t;

extern const combo_t combos[];
extern const int combo_count;
/* combos of one panel involving a button, in `out` (indexes into combos), at most `max`; returns how many */
int combos_for(panel_model_t panel, scemu_button_t button, int *out, int max);
/* a key in its panel's own words, NULL when that panel has no such key */
const char *combo_button_label(panel_model_t panel, scemu_button_t button);
/* the keys of a combination in the panel's own words, for a menu: "hold ALL, then press MUTE" */
void combo_text(const combo_t *c, char *out, size_t size);

#endif
