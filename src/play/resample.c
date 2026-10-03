/* scplay, scgui, the plugin: the machine's rate to a host's, pulled.
 *
 * libsamplerate's callback converter: the host asks for output frames and
 * the converter asks the source for input frames as it needs them, so the
 * same code sits under a plugin's process call and a player's device
 * callback.  The medium sinc, as the players' device path: its stopband is
 * under the 16 bits a device takes at a fraction of the best one's cost.
 *
 * Copyright (c) 2026 ian karlsson
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <samplerate.h>
#include <stdlib.h>
#include <string.h>
#include "resample.h"

#define CONVERTER SRC_SINC_MEDIUM_QUALITY

struct resample
{
	SRC_STATE *src;               /* NULL when the rates are equal */
	double ratio;                 /* output frames per input frame */
	int channels;
	resample_source_fn source;
	void *user;
	const float *held;            /* the source's block a pass-through read is inside */
	size_t held_frames, held_used;
};

static long pull(void *user, float **data)
{
	resample_t *r = user;
	const float *block;
	size_t frames = r->source(r->user, &block, 0);
	*data = (float *)block;
	return (long)frames;
}

resample_t *resample_open(uint32_t in_rate, uint32_t out_rate, int channels, resample_source_fn source, void *user)
{
	resample_t *r = calloc(1, sizeof(*r));
	if (!r)
		return NULL;
	r->channels = channels;
	r->source = source;
	r->user = user;
	r->ratio = (double)out_rate / in_rate;
	if (in_rate == out_rate)
		return r;
	int error = 0;
	r->src = src_callback_new(pull, CONVERTER, channels, &error, r);
	if (!r->src)
	{
		free(r);
		return NULL;
	}
	return r;
}

void resample_close(resample_t *r)
{
	if (!r)
		return;
	if (r->src)
		src_delete(r->src);
	free(r);
}

size_t resample_read(resample_t *r, float *out, size_t frames)
{
	if (r->src)
	{
		long made = src_callback_read(r->src, r->ratio, (long)frames, out);
		return made < 0 ? 0 : (size_t)made;
	}
	size_t done = 0, ch = (size_t)r->channels;
	while (done < frames)
	{
		if (r->held_used == r->held_frames)
		{
			r->held_frames = r->source(r->user, &r->held, frames - done);
			r->held_used = 0;
			if (!r->held_frames)
				break;
		}
		size_t take = r->held_frames - r->held_used;
		if (take > frames - done)
			take = frames - done;
		memcpy(out + done * ch, r->held + r->held_used * ch, take * ch * sizeof(float));
		r->held_used += take;
		done += take;
	}
	return done;
}

void resample_reset(resample_t *r)
{
	if (r->src)
		src_reset(r->src);
	r->held_frames = r->held_used = 0;
}
