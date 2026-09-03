/* scplay, scgui: PortAudio output.  The emulation runs on its own thread
 * and fills a single-producer ring two device blocks deep; PortAudio's
 * callback drains it, through a windowed-sinc resampler when the device
 * would not open at the machine's rate.
 *
 * Copyright (c) 2026 ian karlsson
 * SPDX-License-Identifier: BSD-3-Clause
 */
#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <portaudio.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "audio.h"

#define RING_FRAMES 32768u          /* power of two */
#define RING_MASK (RING_FRAMES - 1u)

#define TAPS 32                     /* power of two */
#define TAP_MASK (TAPS - 1)
#define PHASES 256
#define PHASE_BITS 8
#define FRAC_BITS 32

typedef struct resampler
{
	float table[PHASES + 1][TAPS];
	float hist[TAPS][2];
	unsigned hist_pos;              /* where the next frame goes */
	uint64_t step;                  /* input frames per output frame, FRAC_BITS fractional */
	uint64_t phase;                 /* fractional position, below one frame */
	unsigned owed;                  /* input frames to take before the next output */
} resampler_t;

struct scplay_audio
{
	PaStream *stream;
	int16_t *ring;
	_Atomic size_t target;          /* two callbacks' worth: what was asked for, or what the host actually takes */
	size_t block;
	uint32_t rate, device_rate;
	_Atomic size_t head;            /* frames written */
	_Atomic size_t tail;            /* frames read */
	_Atomic uint32_t underruns;
	bool running;
	char driver[64], device[128];
	double stream_latency;
	resampler_t *rs;
};

/* ---------------------------------------------------------------- the resampler */

static double bessel_i0(double x)
{
	double sum = 1, term = 1;
	for (int k = 1; k < 50; k++)
	{
		term *= (x / (2 * k)) * (x / (2 * k));
		sum += term;
		if (term < sum * 1e-12)
			break;
	}
	return sum;
}

static resampler_t *resampler_new(uint32_t in_rate, uint32_t out_rate)
{
	resampler_t *rs = calloc(1, sizeof(*rs));
	if (!rs)
		return NULL;
	double ratio = (double)in_rate / out_rate;
	double cutoff = ratio > 1 ? 1 / ratio : 1;
	const double beta = 9;
	const double half = TAPS / 2.0;
	for (int p = 0; p <= PHASES; p++)
	{
		double frac = (double)p / PHASES;
		double sum = 0;
		for (int n = 0; n < TAPS; n++)
		{
			double t = (n + 1 - half) - frac;
			double x = 3.14159265358979323846 * cutoff * t;
			double sinc = t == 0 ? 1 : sin(x) / x;
			double w = t / half;
			double window = fabs(w) < 1 ? bessel_i0(beta * sqrt(1 - w * w)) / bessel_i0(beta) : 0;
			rs->table[p][n] = (float)(cutoff * sinc * window);
			sum += rs->table[p][n];
		}
		for (int n = 0; n < TAPS; n++)
			rs->table[p][n] = (float)(rs->table[p][n] / sum);
	}
	rs->step = (uint64_t)(ratio * 4294967296.0 + 0.5);
	rs->owed = TAPS;
	return rs;
}

/* Takes ring frames from `tail` (not past `head`), makes `want` device
 * frames; returns how many ring frames it took and how many it made. */
static size_t resample(resampler_t *rs, const int16_t *ring, size_t tail, size_t head, int16_t *out, size_t want,
                       size_t *made)
{
	size_t taken = 0;
	size_t n;
	for (n = 0; n < want; n++)
	{
		while (rs->owed)
		{
			if (tail + taken >= head)
				goto done;
			size_t slot = (tail + taken) & RING_MASK;
			rs->hist[rs->hist_pos & TAP_MASK][0] = ring[2 * slot];
			rs->hist[rs->hist_pos & TAP_MASK][1] = ring[2 * slot + 1];
			rs->hist_pos++;
			taken++;
			rs->owed--;
		}
		unsigned p = (unsigned)(rs->phase >> (FRAC_BITS - PHASE_BITS));
		float t = (float)(rs->phase & ((1ull << (FRAC_BITS - PHASE_BITS)) - 1)) / (float)(1ull << (FRAC_BITS - PHASE_BITS));
		const float *c0 = rs->table[p], *c1 = rs->table[p + 1];
		float l = 0, r = 0;
		unsigned start = rs->hist_pos;   /* the oldest of the TAPS frames */
		for (int k = 0; k < TAPS; k++)
		{
			float c = c0[k] + (c1[k] - c0[k]) * t;
			const float *h = rs->hist[(start + k) & TAP_MASK];
			l += h[0] * c;
			r += h[1] * c;
		}
		out[2 * n] = (int16_t)(l > 32767 ? 32767 : l < -32768 ? -32768 : lrintf(l));
		out[2 * n + 1] = (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : lrintf(r));
		rs->phase += rs->step;
		rs->owed += (unsigned)(rs->phase >> FRAC_BITS);
		rs->phase &= (1ull << FRAC_BITS) - 1;
	}
done:
	*made = n;
	return taken;
}

/* ---------------------------------------------------------------- the callback */

static int audio_callback(const void *input, void *output, unsigned long frames, const PaStreamCallbackTimeInfo *time,
                          PaStreamCallbackFlags flags, void *user)
{
	scplay_audio_t *a = user;
	size_t want = frames;
	size_t head = atomic_load_explicit(&a->head, memory_order_acquire);
	size_t tail = atomic_load_explicit(&a->tail, memory_order_relaxed);
	int16_t *out = output;
	size_t made, taken;

	size_t need = a->rs ? (size_t)((double)want * a->rate / a->device_rate) + TAPS + 2 : want;
	if (need * 2 > atomic_load_explicit(&a->target, memory_order_relaxed))
	{
		size_t target = need * 2 > RING_FRAMES / 2 ? RING_FRAMES / 2 : need * 2;
		atomic_store_explicit(&a->target, target, memory_order_relaxed);
	}
	if (a->rs)
		taken = resample(a->rs, a->ring, tail, head, out, want, &made);
	else
	{
		size_t have = head - tail;
		made = taken = have < want ? have : want;
		for (size_t n = 0; n < taken; n++)
		{
			size_t slot = (tail + n) & RING_MASK;
			out[2 * n] = a->ring[2 * slot];
			out[2 * n + 1] = a->ring[2 * slot + 1];
		}
	}
	if (made < want)
	{
		memset(out + 2 * made, 0, (want - made) * 4);
		atomic_fetch_add_explicit(&a->underruns, 1, memory_order_relaxed);
	}
	atomic_store_explicit(&a->tail, tail + taken, memory_order_release);
	return paContinue;
}

/* ---------------------------------------------------------------- devices */

static void describe(int index, char *out, size_t size)
{
	const PaDeviceInfo *d = Pa_GetDeviceInfo(index);
	const PaHostApiInfo *h = d ? Pa_GetHostApiInfo(d->hostApi) : NULL;
	snprintf(out, size, "%s (%s)", d ? d->name : "?", h ? h->name : "?");
}

int audio_list(audio_device_info_t *out, int max)
{
	if (Pa_Initialize() != paNoError)
		return 0;
	int count = 0;
	int def = Pa_GetDefaultOutputDevice();
	int n_dev = Pa_GetDeviceCount();
	for (int n = 0; n < n_dev && count < max; n++)
	{
		const PaDeviceInfo *d = Pa_GetDeviceInfo(n);
		if (!d || d->maxOutputChannels < 2)
			continue;
		out[count].index = n;
		out[count].is_default = n == def;
		describe(n, out[count].name, sizeof(out[count].name));
		count++;
	}
	Pa_Terminate();
	return count;
}

/* ---------------------------------------------------------------- open, close */

static bool try_rate(const PaStreamParameters *p, double rate)
{
	return Pa_IsFormatSupported(NULL, p, rate) == paFormatIsSupported;
}

scplay_audio_t *audio_open(uint32_t rate, int device, size_t block, char *err, size_t err_size)
{
	PaError e = Pa_Initialize();
	if (e != paNoError)
	{
		snprintf(err, err_size, "Pa_Initialize: %s", Pa_GetErrorText(e));
		return NULL;
	}

	scplay_audio_t *a = calloc(1, sizeof(*a));
	if (!a)
	{
		snprintf(err, err_size, "out of memory");
		Pa_Terminate();
		return NULL;
	}
	a->ring = calloc(RING_FRAMES * 2, sizeof(int16_t));
	if (!a->ring)
	{
		free(a);
		snprintf(err, err_size, "out of memory");
		Pa_Terminate();
		return NULL;
	}
	if (block < 32)
		block = 32;
	if (block > RING_FRAMES / 8)
		block = RING_FRAMES / 8;
	a->rate = rate;
	a->block = block;
	a->target = 2 * block;

	if (device < 0)
		device = Pa_GetDefaultOutputDevice();
	const PaDeviceInfo *info = device >= 0 ? Pa_GetDeviceInfo(device) : NULL;
	if (!info)
	{
		snprintf(err, err_size, "no output device");
		goto fail;
	}
	PaStreamParameters p;
	memset(&p, 0, sizeof(p));
	p.device = device;
	p.channelCount = 2;
	p.sampleFormat = paInt16;
	p.suggestedLatency = 2.0 * block / rate;
	if (p.suggestedLatency < info->defaultLowOutputLatency)
		p.suggestedLatency = info->defaultLowOutputLatency;

	double device_rate = rate;
	if (!try_rate(&p, device_rate))
	{
		static const double candidates[] = { 0, 48000, 44100, 96000, 88200, 32000, 22050 };
		device_rate = 0;
		for (size_t n = 0; n < sizeof(candidates) / sizeof(candidates[0]) && device_rate == 0; n++)
		{
			double r = n == 0 ? info->defaultSampleRate : candidates[n];
			if (r > 0 && try_rate(&p, r))
				device_rate = r;
		}
		if (device_rate == 0)
		{
			snprintf(err, err_size, "%s: no usable sample rate", info->name);
			goto fail;
		}
	}
	a->device_rate = (uint32_t)device_rate;
	if (a->device_rate != rate)
	{
		a->rs = resampler_new(rate, a->device_rate);
		if (!a->rs)
		{
			snprintf(err, err_size, "out of memory");
			goto fail;
		}
	}

	/* The host sizes the callback from the latency asked for: its own period,
	 * not pieces of one, so a callback's take is what the ring must hold. */
	e = Pa_OpenStream(&a->stream, NULL, &p, device_rate, paFramesPerBufferUnspecified, paClipOff | paDitherOff,
	                  audio_callback, a);
	if (e != paNoError)
	{
		snprintf(err, err_size, "%s: %s", info->name, Pa_GetErrorText(e));
		goto fail;
	}
	const PaStreamInfo *si = Pa_GetStreamInfo(a->stream);
	a->stream_latency = si ? si->outputLatency : 0;
	const PaHostApiInfo *h = Pa_GetHostApiInfo(info->hostApi);
	snprintf(a->driver, sizeof(a->driver), "%s", h ? h->name : "?");
	snprintf(a->device, sizeof(a->device), "%s", info->name);
	return a;   /* left stopped; the caller starts it once the queue has filled */

fail:
	free(a->rs);
	free(a->ring);
	free(a);
	Pa_Terminate();
	return NULL;
}

void audio_close(scplay_audio_t *a)
{
	if (!a)
		return;
	if (a->running)
		Pa_AbortStream(a->stream);
	Pa_CloseStream(a->stream);
	Pa_Terminate();
	free(a->rs);
	free(a->ring);
	free(a);
}

/* ---------------------------------------------------------------- the producer's side */

size_t audio_space(const scplay_audio_t *a)
{
	size_t head = atomic_load_explicit(&a->head, memory_order_relaxed);
	size_t tail = atomic_load_explicit(&a->tail, memory_order_acquire);
	size_t fill = head - tail;
	size_t target = atomic_load_explicit(&a->target, memory_order_relaxed);
	return fill >= target ? 0 : target - fill;
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
	if (paused && a->running)
	{
		Pa_StopStream(a->stream);
		a->running = false;
	}
	else if (!paused && !a->running)
	{
		if (Pa_StartStream(a->stream) == paNoError)
			a->running = true;
	}
}

void audio_drain(scplay_audio_t *a)
{
	for (int n = 0; n < 2000; n++)
	{
		size_t head = atomic_load_explicit(&a->head, memory_order_relaxed);
		size_t tail = atomic_load_explicit(&a->tail, memory_order_acquire);
		if (head == tail || !a->running)
			break;
		struct timespec ts = { 0, 2000000 };
		nanosleep(&ts, NULL);
	}
	audio_pause(a, true);   /* Pa_StopStream plays out what the device holds */
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
	return a->driver;
}

const char *audio_device_name(const scplay_audio_t *a)
{
	return a->device;
}

uint32_t audio_device_rate(const scplay_audio_t *a)
{
	return a->device_rate;
}

double audio_latency(const scplay_audio_t *a)
{
	return (double)atomic_load_explicit(&a->target, memory_order_relaxed) / a->rate + a->stream_latency;
}
