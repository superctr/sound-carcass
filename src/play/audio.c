/* scplay, scgui: PortAudio output.  The emulation runs on its own thread
 * and fills a single-producer ring two device blocks deep; PortAudio's
 * callback drains it, through libsamplerate when the device is not running
 * at the machine's rate.
 *
 * Copyright (c) 2026 ian karlsson
 * SPDX-License-Identifier: BSD-3-Clause
 */
#define _POSIX_C_SOURCE 200809L
#include <portaudio.h>
#include <samplerate.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "audio.h"

#define RING_FRAMES 32768u          /* power of two */
#define RING_MASK (RING_FRAMES - 1u)

/* The callback converts in the host's real-time thread, so the medium sinc
 * rather than the best one; its stopband is already under the 16 bits the
 * device takes, at a fraction of the cost. */
#define CONVERTER SRC_SINC_MEDIUM_QUALITY
#define CONVERT_MAX_OUT (RING_FRAMES / 2)   /* device frames one callback may ask for */
#define CONVERT_SPARE 16                    /* frames beyond the ratio's share, for the filter */

typedef struct resampler
{
	SRC_STATE *src;
	double ratio;                   /* device frames per machine frame */
	float *in, *out;                /* interleaved stereo, staged for src_process */
	size_t in_cap, out_cap;
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

static void resampler_free(resampler_t *rs)
{
	if (!rs)
		return;
	if (rs->src)
		src_delete(rs->src);
	free(rs->in);
	free(rs->out);
	free(rs);
}

static resampler_t *resampler_new(uint32_t in_rate, uint32_t out_rate)
{
	resampler_t *rs = calloc(1, sizeof(*rs));
	if (!rs)
		return NULL;
	int error = 0;
	rs->src = src_new(CONVERTER, 2, &error);
	rs->ratio = (double)out_rate / in_rate;
	rs->out_cap = CONVERT_MAX_OUT;
	rs->in_cap = (size_t)((double)rs->out_cap / rs->ratio) + CONVERT_SPARE;
	rs->in = malloc(rs->in_cap * 2 * sizeof(float));
	rs->out = malloc(rs->out_cap * 2 * sizeof(float));
	if (!rs->src || !rs->in || !rs->out || src_set_ratio(rs->src, rs->ratio) != 0)
	{
		resampler_free(rs);
		return NULL;
	}
	return rs;
}

/* Takes ring frames from `tail` (not past `head`), makes at most `want` device
 * frames; returns how many ring frames it took and how many it made. */
static size_t resample(resampler_t *rs, const int16_t *ring, size_t tail, size_t head, int16_t *out, size_t want,
                       size_t *made)
{
	SRC_DATA data;
	size_t have = head - tail;
	if (want > rs->out_cap)
		want = rs->out_cap;
	size_t take = (size_t)((double)want / rs->ratio) + CONVERT_SPARE;
	if (take > rs->in_cap)
		take = rs->in_cap;
	if (take > have)
		take = have;
	for (size_t n = 0; n < take; n++)
	{
		size_t slot = (tail + n) & RING_MASK;
		rs->in[2 * n] = ring[2 * slot] * (1.0f / 32768);
		rs->in[2 * n + 1] = ring[2 * slot + 1] * (1.0f / 32768);
	}
	memset(&data, 0, sizeof(data));
	data.data_in = rs->in;
	data.input_frames = (long)take;
	data.data_out = rs->out;
	data.output_frames = (long)want;
	data.src_ratio = rs->ratio;
	if (src_process(rs->src, &data) != 0)
	{
		*made = 0;
		return 0;
	}
	src_float_to_short_array(rs->out, out, (int)data.output_frames_gen * 2);
	*made = (size_t)data.output_frames_gen;
	return (size_t)data.input_frames_used;
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

	size_t need = a->rs ? (size_t)((double)want * a->rate / a->device_rate) + CONVERT_SPARE : want;
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

scplay_audio_t *audio_open(uint32_t rate, uint32_t want_rate, int device, size_t block, char *err, size_t err_size)
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

	double device_rate = want_rate ? want_rate : rate;
	if (!try_rate(&p, device_rate))
	{
		/* not that rate, then: the machine's own next, so that nothing is
		 * converted when the device happens to take it, then the usual ones */
		static const double candidates[] = { 0, 48000, 44100, 96000, 88200, 32000, 22050 };
		device_rate = try_rate(&p, rate) ? rate : 0;
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
	resampler_free(a->rs);
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
	resampler_free(a->rs);
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

/* ---------------------------------------------------------------- off the device */

struct audio_convert
{
	SRC_STATE *src;
	double ratio;
	float *in, *out;
	int16_t *pcm;                   /* what the caller reads back */
	size_t in_cap, out_cap;
};

audio_convert_t *audio_convert_open(uint32_t in_rate, uint32_t out_rate)
{
	audio_convert_t *c = calloc(1, sizeof(*c));
	if (!c)
		return NULL;
	int error = 0;
	c->src = src_new(CONVERTER, 2, &error);
	c->ratio = (double)out_rate / in_rate;
	if (!c->src || src_set_ratio(c->src, c->ratio) != 0)
	{
		audio_convert_close(c);
		return NULL;
	}
	return c;
}

void audio_convert_close(audio_convert_t *c)
{
	if (!c)
		return;
	if (c->src)
		src_delete(c->src);
	free(c->in);
	free(c->out);
	free(c->pcm);
	free(c);
}

static bool convert_room(audio_convert_t *c, size_t in_frames, size_t out_frames)
{
	if (in_frames > c->in_cap)
	{
		float *in = realloc(c->in, in_frames * 2 * sizeof(float));
		if (!in)
			return false;
		c->in = in;
		c->in_cap = in_frames;
	}
	if (out_frames > c->out_cap)
	{
		float *out = realloc(c->out, out_frames * 2 * sizeof(float));
		if (!out)
			return false;
		c->out = out;
		int16_t *pcm = realloc(c->pcm, out_frames * 2 * sizeof(int16_t));
		if (!pcm)
			return false;
		c->pcm = pcm;
		c->out_cap = out_frames;
	}
	return true;
}

const int16_t *audio_convert_run(audio_convert_t *c, const int16_t *stereo, size_t frames, bool last, size_t *made)
{
	size_t room = (size_t)((double)frames * c->ratio) + CONVERT_SPARE;
	size_t used = 0, total = 0;
	*made = 0;
	if (!convert_room(c, frames, room))
		return NULL;
	for (size_t n = 0; n < frames * 2; n++)
		c->in[n] = stereo[n] * (1.0f / 32768);
	for (;;)
	{
		SRC_DATA data;
		if (total == room)          /* the converter had more to give than the ratio asked for */
		{
			room = room * 2 + CONVERT_SPARE;
			if (!convert_room(c, frames, room))
				return NULL;
		}
		memset(&data, 0, sizeof(data));
		data.data_in = c->in + used * 2;
		data.input_frames = (long)(frames - used);
		data.data_out = c->out + total * 2;
		data.output_frames = (long)(room - total);
		data.src_ratio = c->ratio;
		data.end_of_input = last;
		if (src_process(c->src, &data) != 0)
			return NULL;
		used += (size_t)data.input_frames_used;
		total += (size_t)data.output_frames_gen;
		if (used >= frames && total < room)
			break;
	}
	src_float_to_short_array(c->out, c->pcm, (int)total * 2);
	*made = total;
	return c->pcm;
}
