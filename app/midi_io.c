/* scgui: MIDI ports on the host, through PortMidi.
 *
 * Copyright (c) 2026 ian karlsson
 * SPDX-License-Identifier: BSD-3-Clause
 */
#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <portmidi.h>
#include <porttime.h>
#include "midi_io.h"

#define IN_FIFO 8192
#define READ_EVENTS 256
#define SYSEX_MAX (1u << 20)

typedef struct in_stream
{
	PortMidiStream *stream;
	bool in_sysex;
} in_stream_t;

typedef struct input
{
	int own_id;
	in_stream_t own, dev;
	char dev_name[80];
	uint8_t fifo[IN_FIFO];
	size_t fifo_head, fifo_count;
} input_t;

typedef struct output
{
	int own_id;
	PortMidiStream *own, *dev;
	char dev_name[80];
	/* the byte stream reassembled into messages */
	uint8_t status, data[2];
	int have, need;
	bool in_sysex;
	uint8_t *sysex;
	size_t sysex_len, sysex_cap;
} output_t;

struct midi_io
{
	pthread_mutex_t lock;
	char client[64];
	input_t in[MIDI_IO_INPUT_COUNT];
	output_t out[MIDI_IO_OUTPUT_COUNT];
};

static const char *const input_names[MIDI_IO_INPUT_COUNT] = { "MIDI IN A", "MIDI IN B" };
static const char *const output_names[MIDI_IO_OUTPUT_COUNT] = { "MIDI OUT", "Song A", "Song B" };

/* ---------------------------------------------------------------- devices */

static int message_length(uint8_t status)
{
	switch (status & 0xf0)
	{
	case 0x80: case 0x90: case 0xa0: case 0xb0: case 0xe0:
		return 3;
	case 0xc0: case 0xd0:
		return 2;
	case 0xf0:
		switch (status)
		{
		case 0xf1: case 0xf3: return 2;
		case 0xf2: return 3;
		default: return 1;
		}
	default:
		return 0;
	}
}

typedef struct sysdep
{
	int structVersion;
	int length;
	struct
	{
		enum PmSysDepPropertyKey key;
		const void *value;
	} properties[1];
} sysdep_t;

static void create_ports(midi_io_t *io)
{
	sysdep_t sd = { PM_SYSDEPINFO_VERS, 1, { { pmKeyAlsaClientName, io->client } } };
	for (int n = 0; n < MIDI_IO_INPUT_COUNT; n++)
	{
		input_t *in = &io->in[n];
		in->own_id = Pm_CreateVirtualInput(input_names[n], NULL, &sd);
		in->own.stream = NULL;
		in->own.in_sysex = false;
		if (in->own_id >= 0 && Pm_OpenInput(&in->own.stream, in->own_id, &sd, READ_EVENTS, NULL, NULL) != pmNoError)
			in->own.stream = NULL;
	}
	for (int n = 0; n < MIDI_IO_OUTPUT_COUNT; n++)
	{
		output_t *out = &io->out[n];
		out->own_id = Pm_CreateVirtualOutput(output_names[n], NULL, &sd);
		out->own = NULL;
		if (out->own_id >= 0 && Pm_OpenOutput(&out->own, out->own_id, &sd, 0, NULL, NULL, 0) != pmNoError)
			out->own = NULL;
	}
}

static void destroy_ports(midi_io_t *io)
{
	for (int n = 0; n < MIDI_IO_INPUT_COUNT; n++)
	{
		input_t *in = &io->in[n];
		if (in->own.stream)
			Pm_Close(in->own.stream);
		in->own.stream = NULL;
		if (in->own_id >= 0)
			Pm_DeleteVirtualDevice(in->own_id);
		in->own_id = -1;
		if (in->dev.stream)
			Pm_Close(in->dev.stream);
		in->dev.stream = NULL;
	}
	for (int n = 0; n < MIDI_IO_OUTPUT_COUNT; n++)
	{
		output_t *out = &io->out[n];
		if (out->own)
			Pm_Close(out->own);
		out->own = NULL;
		if (out->own_id >= 0)
			Pm_DeleteVirtualDevice(out->own_id);
		out->own_id = -1;
		if (out->dev)
			Pm_Close(out->dev);
		out->dev = NULL;
	}
}

static bool ours(const midi_io_t *io, int id)
{
	for (int n = 0; n < MIDI_IO_INPUT_COUNT; n++)
		if (io->in[n].own_id == id)
			return true;
	for (int n = 0; n < MIDI_IO_OUTPUT_COUNT; n++)
		if (io->out[n].own_id == id)
			return true;
	return false;
}

static int find_device(const midi_io_t *io, const char *name, bool input)
{
	int count = Pm_CountDevices();
	for (int id = 0; id < count; id++)
	{
		const PmDeviceInfo *d = Pm_GetDeviceInfo(id);
		if (!d || ours(io, id) || !(input ? d->input : d->output))
			continue;
		if (strcmp(d->name, name) == 0)
			return id;
	}
	return -1;
}

static bool open_input_device(input_t *in, int id)
{
	if (in->dev.stream)
		Pm_Close(in->dev.stream);
	in->dev.stream = NULL;
	in->dev.in_sysex = false;
	in->dev_name[0] = 0;
	if (id < 0)
		return true;
	const PmDeviceInfo *d = Pm_GetDeviceInfo(id);
	if (!d || !d->input)
		return false;
	if (Pm_OpenInput(&in->dev.stream, id, NULL, READ_EVENTS, NULL, NULL) != pmNoError)
	{
		in->dev.stream = NULL;
		return false;
	}
	snprintf(in->dev_name, sizeof(in->dev_name), "%s", d->name);
	return true;
}

static bool open_output_device(output_t *out, int id)
{
	if (out->dev)
		Pm_Close(out->dev);
	out->dev = NULL;
	out->dev_name[0] = 0;
	if (id < 0)
		return true;
	const PmDeviceInfo *d = Pm_GetDeviceInfo(id);
	if (!d || !d->output)
		return false;
	if (Pm_OpenOutput(&out->dev, id, NULL, 0, NULL, NULL, 0) != pmNoError)
	{
		out->dev = NULL;
		return false;
	}
	snprintf(out->dev_name, sizeof(out->dev_name), "%s", d->name);
	return true;
}

int midi_io_list(midi_io_t *io, midi_port_info_t *out, int max)
{
	if (!io)
		return 0;
	pthread_mutex_lock(&io->lock);
	int count = 0;
	int n_dev = Pm_CountDevices();
	for (int id = 0; id < n_dev && count < max; id++)
	{
		const PmDeviceInfo *d = Pm_GetDeviceInfo(id);
		if (!d || ours(io, id) || (!d->input && !d->output))
			continue;
		/* one line per device: an input and an output of the same name join */
		int at = -1;
		for (int k = 0; k < count; k++)
			if (strcmp(out[k].name, d->name) == 0 && (d->input ? !out[k].readable : !out[k].writable))
				at = k;
		if (at < 0)
		{
			at = count++;
			out[at].in_id = out[at].out_id = -1;
			out[at].readable = out[at].writable = false;
			snprintf(out[at].name, sizeof(out[at].name), "%s", d->name);
		}
		if (d->input)
		{
			out[at].readable = true;
			out[at].in_id = id;
		}
		if (d->output)
		{
			out[at].writable = true;
			out[at].out_id = id;
		}
	}
	pthread_mutex_unlock(&io->lock);
	return count;
}

midi_io_t *midi_io_open(const char *client_name)
{
	midi_io_t *io = calloc(1, sizeof(*io));
	if (!io)
		return NULL;
	snprintf(io->client, sizeof(io->client), "%s", client_name);
	if (Pm_Initialize() != pmNoError)
	{
		free(io);
		return NULL;
	}
	Pt_Start(1, NULL, NULL);
	pthread_mutex_init(&io->lock, NULL);
	for (int n = 0; n < MIDI_IO_INPUT_COUNT; n++)
		io->in[n].own_id = -1;
	for (int n = 0; n < MIDI_IO_OUTPUT_COUNT; n++)
		io->out[n].own_id = -1;
	create_ports(io);
	return io;
}

void midi_io_close(midi_io_t *io)
{
	if (!io)
		return;
	destroy_ports(io);
	for (int n = 0; n < MIDI_IO_OUTPUT_COUNT; n++)
		free(io->out[n].sysex);
	Pt_Stop();
	Pm_Terminate();
	pthread_mutex_destroy(&io->lock);
	free(io);
}

void midi_io_rescan(midi_io_t *io)
{
	if (!io)
		return;
	pthread_mutex_lock(&io->lock);
	char in_names[MIDI_IO_INPUT_COUNT][80], out_names[MIDI_IO_OUTPUT_COUNT][80];
	for (int n = 0; n < MIDI_IO_INPUT_COUNT; n++)
		memcpy(in_names[n], io->in[n].dev_name, sizeof(in_names[n]));
	for (int n = 0; n < MIDI_IO_OUTPUT_COUNT; n++)
		memcpy(out_names[n], io->out[n].dev_name, sizeof(out_names[n]));
	destroy_ports(io);
	Pm_Terminate();
	Pm_Initialize();
	create_ports(io);
	for (int n = 0; n < MIDI_IO_INPUT_COUNT; n++)
		if (in_names[n][0])
			open_input_device(&io->in[n], find_device(io, in_names[n], true));
	for (int n = 0; n < MIDI_IO_OUTPUT_COUNT; n++)
		if (out_names[n][0])
			open_output_device(&io->out[n], find_device(io, out_names[n], false));
	pthread_mutex_unlock(&io->lock);
}

bool midi_io_connect_input(midi_io_t *io, int which, int id)
{
	if (!io || which < 0 || which >= MIDI_IO_INPUT_COUNT)
		return false;
	pthread_mutex_lock(&io->lock);
	bool ok = open_input_device(&io->in[which], id);
	pthread_mutex_unlock(&io->lock);
	return ok;
}

bool midi_io_connect_output(midi_io_t *io, int which, int id)
{
	if (!io || which < 0 || which >= MIDI_IO_OUTPUT_COUNT)
		return false;
	pthread_mutex_lock(&io->lock);
	bool ok = open_output_device(&io->out[which], id);
	pthread_mutex_unlock(&io->lock);
	return ok;
}

/* ---------------------------------------------------------------- input */

static void fifo_push(input_t *in, uint8_t byte)
{
	if (in->fifo_count >= IN_FIFO)
		return;
	in->fifo[(in->fifo_head + in->fifo_count) % IN_FIFO] = byte;
	in->fifo_count++;
}

/* One PortMidi event back into wire bytes: a short message in one word, a
 * sysex four bytes per word until the F7, real-time bytes as words of
 * their own even inside a sysex. */
static void unpack(input_t *in, in_stream_t *s, PmMessage m)
{
	uint8_t b[4] = { (uint8_t)m, (uint8_t)(m >> 8), (uint8_t)(m >> 16), (uint8_t)(m >> 24) };
	if (s->in_sysex)
	{
		if (b[0] >= 0xf8)
		{
			fifo_push(in, b[0]);
			return;
		}
		if (b[0] < 0x80 || b[0] == 0xf7)
		{
			for (int k = 0; k < 4; k++)
			{
				fifo_push(in, b[k]);
				if (b[k] == 0xf7)
				{
					s->in_sysex = false;
					return;
				}
			}
			return;
		}
		s->in_sysex = false;   /* cut short by another status */
	}
	if (b[0] == 0xf0)
	{
		s->in_sysex = true;
		for (int k = 0; k < 4; k++)
		{
			fifo_push(in, b[k]);
			if (k && b[k] == 0xf7)
			{
				s->in_sysex = false;
				return;
			}
		}
		return;
	}
	int len = message_length(b[0]);
	for (int k = 0; k < len; k++)
		fifo_push(in, b[k]);
}

static void fill(input_t *in, in_stream_t *s)
{
	if (!s->stream)
		return;
	PmEvent events[READ_EVENTS];
	for (;;)
	{
		int got = Pm_Read(s->stream, events, READ_EVENTS);
		if (got <= 0)
			return;
		for (int n = 0; n < got; n++)
			unpack(in, s, events[n].message);
		if (got < READ_EVENTS)
			return;
	}
}

size_t midi_io_read(midi_io_t *io, int *which, uint8_t *buf, size_t max)
{
	if (!io)
		return 0;
	pthread_mutex_lock(&io->lock);
	size_t got = 0;
	for (int n = 0; n < MIDI_IO_INPUT_COUNT && !got; n++)
	{
		input_t *in = &io->in[n];
		if (!in->fifo_count)
		{
			fill(in, &in->own);
			fill(in, &in->dev);
		}
		while (in->fifo_count && got < max)
		{
			buf[got++] = in->fifo[in->fifo_head];
			in->fifo_head = (in->fifo_head + 1) % IN_FIFO;
			in->fifo_count--;
		}
		if (got)
			*which = n;
	}
	pthread_mutex_unlock(&io->lock);
	return got;
}

/* ---------------------------------------------------------------- output */

static void send_short(output_t *out, PmMessage m)
{
	if (out->own)
		Pm_WriteShort(out->own, 0, m);
	if (out->dev)
		Pm_WriteShort(out->dev, 0, m);
}

static void sysex_append(output_t *out, uint8_t byte)
{
	if (out->sysex_len >= out->sysex_cap)
	{
		size_t cap = out->sysex_cap ? out->sysex_cap * 2 : 1024;
		if (cap > SYSEX_MAX)
			return;
		uint8_t *grown = realloc(out->sysex, cap);
		if (!grown)
			return;
		out->sysex = grown;
		out->sysex_cap = cap;
	}
	out->sysex[out->sysex_len++] = byte;
}

static void out_byte(output_t *out, uint8_t b)
{
	if (b >= 0xf8)
	{
		send_short(out, b);
		return;
	}
	if (out->in_sysex)
	{
		if (b < 0x80)
		{
			sysex_append(out, b);
			return;
		}
		out->in_sysex = false;
		if (b == 0xf7)
		{
			sysex_append(out, b);
			if (out->sysex_len && out->sysex[out->sysex_len - 1] == 0xf7)
			{
				if (out->own)
					Pm_WriteSysEx(out->own, 0, out->sysex);
				if (out->dev)
					Pm_WriteSysEx(out->dev, 0, out->sysex);
			}
			return;
		}
		/* cut short by another status: dropped, the status goes on */
	}
	if (b == 0xf0)
	{
		out->in_sysex = true;
		out->sysex_len = 0;
		sysex_append(out, b);
		return;
	}
	if (b >= 0x80)
	{
		out->status = b;
		out->have = 0;
		out->need = message_length(b) - 1;
		if (out->need == 0)
		{
			send_short(out, b);
			out->status = 0;
		}
		return;
	}
	if (!out->status)
		return;
	out->data[out->have++] = b;
	if (out->have == out->need)
	{
		send_short(out, Pm_Message(out->status, out->data[0], out->need > 1 ? out->data[1] : 0));
		out->have = 0;
		if (out->status >= 0xf0)
			out->status = 0;
	}
}

void midi_io_write(midi_io_t *io, int which, const uint8_t *bytes, size_t count)
{
	if (!io || which < 0 || which >= MIDI_IO_OUTPUT_COUNT)
		return;
	pthread_mutex_lock(&io->lock);
	output_t *out = &io->out[which];
	if (out->own || out->dev)
		for (size_t n = 0; n < count; n++)
			out_byte(out, bytes[n]);
	pthread_mutex_unlock(&io->lock);
}
