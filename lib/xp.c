#include <stdlib.h>
#include <string.h>
#include "xp.h"

bool xp_init(xp_t *xp, const xp_link_t *link, jit_alloc_t *jit, const uint8_t *wave, size_t wave_size)
{
	memset(xp, 0, sizeof(*xp));
	xp->link = *link;
	xp->jit = jit;
	xp->wave = wave;
	xp->wave_size = wave_size;
	xp->eram = calloc(XP_ERAM_SIZE, sizeof(int32_t));
	if (!xp->eram)
		return false;
	xp_dsp_init(&xp->dsp, xp->eram);
	return true;
}

void xp_release(xp_t *xp)
{
	xp_dsp_release(&xp->dsp, xp->jit);
	free(xp->eram);
	xp->eram = NULL;
}

void xp_reset(xp_t *xp)
{
	memset(xp->regs, 0, sizeof(xp->regs));
	memset(xp->voices, 0, sizeof(xp->voices));
	memset(xp->bus, 0, sizeof(xp->bus));
	xp->run_mask = 0;
	xp->read_latch = 0;
	xp->frame_counter = 0;
	xp->noise = 1;
	xp->irq_head = 0;
	xp->irq_count = 0;
	xp->int_state = false;
	xp_dsp_reset(&xp->dsp);
}

uint16_t xp_read(xp_t *xp, uint32_t offset)
{
	return xp->regs[offset & (XP_REGS - 1)];
}

void xp_write(xp_t *xp, uint32_t offset, uint16_t data)
{
	xp->regs[offset & (XP_REGS - 1)] = data;
}

void xp_set_serial_words(xp_t *xp, int left, int right)
{
	xp->serial_out_word[0] = (uint8_t)left;
	xp->serial_out_word[1] = (uint8_t)right;
}

void xp_run_frame(xp_t *xp)
{
	memset(xp->bus, 0, sizeof(xp->bus));
	xp->frame_counter++;
	xp->noise ^= xp->noise << 13;
	xp->noise ^= xp->noise >> 17;
	xp->noise ^= xp->noise << 5;

	if (xp->dsp.dirty)
		xp_dsp_compile(&xp->dsp, xp->jit);
	if (xp->dsp.enabled && xp->dsp.frame)
		xp->dsp.frame(xp);
}

int32_t xp_output(const xp_t *xp, int word)
{
	int64_t v = (int64_t)xp->dsp.iram[word & (XP_OUTPUT_WORDS - 1)] * 2;
	if (v > 0x7fffff)
		v = 0x7fffff;
	if (v < -0x800000)
		v = -0x800000;
	return (int32_t)v;
}
