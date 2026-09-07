/* scplay, scgui, the plugin: the machine's rate to a host's, pulled.
 *
 * Copyright (c) 2026 ian karlsson
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef SCPLAY_RESAMPLE_H
#define SCPLAY_RESAMPLE_H

#include <stddef.h>
#include <stdint.h>

typedef struct resample resample_t;

/* Hands the next block of the source: interleaved stereo float frames at
 * the source's rate, as many as it likes; 0 when it has none, which ends
 * the read short.  The block must stay put until the next call. */
typedef size_t (*resample_source_fn)(void *user, const float **stereo);

/* A converter from in_rate to out_rate over `source`; equal rates pass the
 * source through untouched.  NULL when it cannot be made.  Input and output
 * stay aligned in time -- source frame k comes out at frame k * out/in, so
 * there is no latency to report -- at the price of the source being pulled
 * ahead of the output by the filter's half length, some 46 source frames. */
resample_t *resample_open(uint32_t in_rate, uint32_t out_rate, resample_source_fn source, void *user);
void resample_close(resample_t *r);

/* `frames` frames at the output rate into `stereo`, pulling the source as
 * needed; returns how many it made, fewer only when the source ran dry. */
size_t resample_read(resample_t *r, float *stereo, size_t frames);

/* Forgets what is in the filter, for a source that starts over. */
void resample_reset(resample_t *r);

#endif
