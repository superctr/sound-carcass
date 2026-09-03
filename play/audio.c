/* scplay: SDL audio.  The emulation runs on the main thread and fills a
 * single-producer ring; SDL's callback drains it.
 *
 * Copyright (c) 2026 ian karlsson
 * SPDX-License-Identifier: BSD-3-Clause
 */
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "audio.h"

#define RING_FRAMES 32768u          /* power of two */
#define RING_MASK (RING_FRAMES - 1u)

struct scplay_audio
{
	SDL_AudioDeviceID dev;
	int16_t *ring;
	size_t target;
	_Atomic size_t head;            /* frames written */
	_Atomic size_t tail;            /* frames read */
	_Atomic uint32_t underruns;
	const char *driver;
};

static void audio_callback(void *user, Uint8 *stream, int len)
{
	scplay_audio_t *a = user;
	size_t want = (size_t)len / 4;
	size_t head = atomic_load_explicit(&a->head, memory_order_acquire);
	size_t tail = atomic_load_explicit(&a->tail, memory_order_relaxed);
	size_t have = head - tail;
	size_t take = have < want ? have : want;
	int16_t *out = (int16_t *)stream;

	for (size_t n = 0; n < take; n++)
	{
		size_t slot = (tail + n) & RING_MASK;
		out[2 * n] = a->ring[2 * slot];
		out[2 * n + 1] = a->ring[2 * slot + 1];
	}
	if (take < want)
	{
		memset(out + 2 * take, 0, (want - take) * 4);
		atomic_fetch_add_explicit(&a->underruns, 1, memory_order_relaxed);
	}
	atomic_store_explicit(&a->tail, tail + take, memory_order_release);
}

scplay_audio_t *audio_open(uint32_t rate, char *err, size_t err_size)
{
	if (SDL_Init(SDL_INIT_AUDIO) != 0)
	{
		snprintf(err, err_size, "SDL_Init: %s", SDL_GetError());
		return NULL;
	}

	scplay_audio_t *a = calloc(1, sizeof(*a));
	if (!a)
	{
		snprintf(err, err_size, "out of memory");
		return NULL;
	}
	a->ring = calloc(RING_FRAMES * 2, sizeof(int16_t));
	if (!a->ring)
	{
		free(a);
		snprintf(err, err_size, "out of memory");
		return NULL;
	}
	a->target = rate / 5;           /* about 200 ms of lead */
	if (a->target > RING_FRAMES / 2)
		a->target = RING_FRAMES / 2;

	SDL_AudioSpec want, have;
	memset(&want, 0, sizeof(want));
	want.freq = (int)rate;
	want.format = AUDIO_S16SYS;
	want.channels = 2;
	want.samples = 1024;
	want.callback = audio_callback;
	want.userdata = a;

	/* allowed_changes 0: SDL converts for a device that insists on its own rate. */
	a->dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
	if (!a->dev)
	{
		snprintf(err, err_size, "SDL_OpenAudioDevice: %s", SDL_GetError());
		free(a->ring);
		free(a);
		return NULL;
	}
	a->driver = SDL_GetCurrentAudioDriver();
	return a;   /* left paused; the caller starts it once the queue has filled */
}

void audio_close(scplay_audio_t *a)
{
	if (!a)
		return;
	SDL_CloseAudioDevice(a->dev);
	SDL_QuitSubSystem(SDL_INIT_AUDIO);
	free(a->ring);
	free(a);
}

size_t audio_space(const scplay_audio_t *a)
{
	size_t head = atomic_load_explicit(&a->head, memory_order_relaxed);
	size_t tail = atomic_load_explicit(&a->tail, memory_order_acquire);
	size_t fill = head - tail;
	return fill >= a->target ? 0 : a->target - fill;
}

void audio_push(scplay_audio_t *a, const int16_t *stereo, size_t frames)
{
	size_t head = atomic_load_explicit(&a->head, memory_order_relaxed);
	for (size_t n = 0; n < frames; n++)
	{
		size_t slot = (head + n) & RING_MASK;
		a->ring[2 * slot] = stereo[2 * n];
		a->ring[2 * slot + 1] = stereo[2 * n + 1];
	}
	atomic_store_explicit(&a->head, head + frames, memory_order_release);
}

void audio_pause(scplay_audio_t *a, bool paused)
{
	SDL_PauseAudioDevice(a->dev, paused ? 1 : 0);
}

void audio_drain(scplay_audio_t *a)
{
	for (int n = 0; n < 2000; n++)
	{
		size_t head = atomic_load_explicit(&a->head, memory_order_relaxed);
		size_t tail = atomic_load_explicit(&a->tail, memory_order_acquire);
		if (head == tail)
			return;
		SDL_Delay(2);
	}
}

uint64_t audio_played(const scplay_audio_t *a)
{
	return atomic_load_explicit(&a->tail, memory_order_relaxed);
}

uint32_t audio_underruns(const scplay_audio_t *a)
{
	return atomic_load_explicit(&a->underruns, memory_order_relaxed);
}

const char *audio_driver(const scplay_audio_t *a)
{
	return a->driver ? a->driver : "?";
}
