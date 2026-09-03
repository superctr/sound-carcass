/* scplay: SDL audio output through a ring buffer.
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

scplay_audio_t *audio_open(uint32_t rate, char *err, size_t err_size);
void audio_close(scplay_audio_t *a);

/* Frames the producer may write without blocking, keeping the queue near its
 * target depth. */
size_t audio_space(const scplay_audio_t *a);
void audio_push(scplay_audio_t *a, const int16_t *stereo, size_t frames);
void audio_pause(scplay_audio_t *a, bool paused);
void audio_drain(scplay_audio_t *a);

uint64_t audio_played(const scplay_audio_t *a);
uint32_t audio_underruns(const scplay_audio_t *a);
const char *audio_driver(const scplay_audio_t *a);

#endif
