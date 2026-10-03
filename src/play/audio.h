/* scplay, scgui: PortAudio output through a short ring buffer.
 *
 * Copyright (c) 2026 ian karlsson
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef SCPLAY_AUDIO_H
#define SCPLAY_AUDIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct scplay_audio scplay_audio_t;

typedef struct audio_device_info
{
	int index;                /* what audio_open wants */
	char name[128];           /* "device (host API)" */
	bool is_default;
} audio_device_info_t;

/* The host's output devices; returns how many fit.  Indices are stable as
 * long as the same devices are attached. */
int audio_list(audio_device_info_t *out, int max);

/* `rate` is the machine's and `want_rate` is what the device is asked for, 0
 * for the machine's own; a device that refuses it takes the machine's own rate
 * or one of its own instead.  Whatever it opens at, the output is converted to
 * it.  `device` is an index from audio_list or -1 for the host's default.
 * `block` is the device's buffer in frames; the producer keeps two blocks ahead
 * of it. */
scplay_audio_t *audio_open(uint32_t rate, uint32_t want_rate, int device, size_t block, char *err, size_t err_size);
void audio_close(scplay_audio_t *a);

/* Frames the producer may write without blocking, keeping the queue near its
 * target depth. */
size_t audio_space(const scplay_audio_t *a);
void audio_push(scplay_audio_t *a, const int16_t *stereo, size_t frames);
void audio_pause(scplay_audio_t *a, bool paused);
void audio_drain(scplay_audio_t *a);

uint64_t audio_played(const scplay_audio_t *a);      /* frames taken from the ring */
uint32_t audio_underruns(const scplay_audio_t *a);
const char *audio_driver(const scplay_audio_t *a);    /* the host API */
const char *audio_device_name(const scplay_audio_t *a);
uint32_t audio_device_rate(const scplay_audio_t *a);
/* Seconds from a pushed frame to the jack: the ring's depth plus what the
 * host reports for the stream. */
double audio_latency(const scplay_audio_t *a);

/* ---------------------------------------------------------------- off the device */

/* The same conversion for what does not go to a sound card -- scplay's --wav
 * at a rate that is not the machine's. */
typedef struct audio_convert audio_convert_t;

audio_convert_t *audio_convert_open(uint32_t in_rate, uint32_t out_rate);
void audio_convert_close(audio_convert_t *c);
/* Converts 16-bit stereo, `last` on the final call so that what the converter
 * still holds comes out.  The frames it made stay in its own buffer until the
 * next call; NULL and 0 when it made none. */
const int16_t *audio_convert_run(audio_convert_t *c, const int16_t *stereo, size_t frames, bool last, size_t *made);

#endif
