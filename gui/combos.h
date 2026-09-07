/* scemu GUI: the front-panel button combinations the SC-88Pro understands.
 *
 * One mouse cannot hold two keys, so a right-click on a panel button offers
 * the combinations that button takes part in.  The table is compiled from the
 * SC-88Pro owner's manual; docs/panel/combinations.md carries the same rows
 * with the manual's wording, the preconditions and the ambiguities.
 *
 * Copyright (c) 2026 ian karlsson
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef SCEMU_COMBOS_H
#define SCEMU_COMBOS_H

#include <stdbool.h>
#include <stddef.h>
#include "scemu.h"

/* How the firmware wants the keys: the manual's [A]*[B] means both keys are
 * pressed at once and must reach it in one scan, its [A]+[B] means [A] is
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
	scemu_button_t hold[3];     /* held first, in this order */
	int hold_count;
	scemu_button_t press;       /* pressed while they are held; SCEMU_BUTTON_COUNT if none */
	bool power_on;              /* the hold set is held while switching on */
	int page;                   /* in the user manual */
	combo_timing_t timing;
} combo_t;

extern const combo_t combos[];
extern const int combo_count;
/* combos involving a button, in `out` (indexes into combos), at most `max`; returns how many */
int combos_for(scemu_button_t button, int *out, int max);
/* the keys of a combination in the panel's own words, for a menu: "hold ALL, then press MUTE" */
void combo_text(const combo_t *c, char *out, size_t size);

#endif
