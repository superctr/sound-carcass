#ifndef SCEMU_GLCD_H
#define SCEMU_GLCD_H

#include <stdint.h>
#include <stdbool.h>
#include "scemu.h"

/* SED1335F0B graphic LCD controller with 32 KB of its own SRAM, behind the
 * SC-8850's 160 x 64 panel. */

typedef struct glcd
{
	scemu_glcd_t out;
	uint8_t vram[0x8000];

	uint8_t command;
	uint8_t param;

	bool display;
	bool sleep;
	bool dirty;

	uint8_t m0, m1, m2, ws, iv, wf;
	uint8_t fx, fy;
	uint16_t cr, tcr, lf, ap;

	uint16_t sad1, sad2, sad3, sad4;
	uint16_t sl1, sl2;

	uint16_t sag;
	uint8_t hdotscr;
	uint8_t mx, dm1, dm3, ov;
	uint8_t fc, fp;
	uint8_t crx, cry, cm;

	uint16_t csr;
	uint8_t csrdir;

	uint32_t flash_count;
	uint32_t flash_period;
	uint32_t flash_phase;
} glcd_t;

void glcd_init(glcd_t *g);
void glcd_reset(glcd_t *g);
uint8_t glcd_read(glcd_t *g, uint32_t offset);
void glcd_write(glcd_t *g, uint32_t offset, uint8_t data);
void glcd_frame(glcd_t *g);
const scemu_glcd_t *glcd_out(glcd_t *g);

#endif
