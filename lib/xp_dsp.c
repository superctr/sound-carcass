#include <string.h>
#include "xp_dsp.h"

void xp_dsp_init(xp_dsp_t *d, int32_t *eram)
{
	memset(d, 0, sizeof(*d));
	d->eram = eram;
}

void xp_dsp_reset(xp_dsp_t *d)
{
	memset(&d->state, 0, sizeof(d->state));
	memset(d->iram, 0, sizeof(d->iram));
	memset(d->iram_ramping, 0, sizeof(d->iram_ramping));
	memset(d->eram, 0, XP_ERAM_SIZE * sizeof(int32_t));
	d->enabled = false;
	d->dirty = true;
}

void xp_dsp_decode(xp_dsp_t *d, const uint16_t *pram, const uint16_t *cram)
{
	(void)pram;
	(void)cram;
	d->dirty = true;
}

bool xp_dsp_compile(xp_dsp_t *d, jit_alloc_t *a)
{
	(void)a;
	d->dirty = false;
	d->frame = NULL;
	return false;
}

void xp_dsp_release(xp_dsp_t *d, jit_alloc_t *a)
{
	jit_code_free(a, &d->code[0]);
	jit_code_free(a, &d->code[1]);
	d->frame = NULL;
}
