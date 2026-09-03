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

/* `rate` is the machine's; a device that refuses it runs at its own rate
 * and the output is resampled.  `device` is an index from audio_list or -1
 * for the host's default.  `block` is the device's buffer in frames; the
 * producer keeps two blocks ahead of it. */
scplay_audio_t *audio_open(uint32_t rate, int device, size_t block, char *err, size_t err_size);
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

#endif
