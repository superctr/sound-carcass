/* scgui: the front-panel button combinations the SC-88Pro understands.
 *
 * One mouse cannot hold two keys, so a right-click on a panel button offers
 * the combinations that button takes part in.  The table is compiled from the
 * SC-88Pro owner's manual; docs/panel/combinations.md carries the same rows
 * with the manual's wording, the preconditions and the ambiguities.
 *
 * Copyright (c) 2026 ian karlsson
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef SCGUI_COMBOS_H
#define SCGUI_COMBOS_H

#include <stdbool.h>
#include "scemu.h"

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
} combo_t;

extern const combo_t combos[];
extern const int combo_count;
/* combos involving a button, in `out` (indexes into combos), at most `max`; returns how many */
int combos_for(scemu_button_t button, int *out, int max);

#endif
