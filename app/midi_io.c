/* scgui: MIDI ports on the host, through the ALSA sequencer.
 *
 * Copyright (c) 2026 ian karlsson
 * SPDX-License-Identifier: BSD-3-Clause
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <alloca.h>
#include <alsa/asoundlib.h>
#include "midi_io.h"

struct midi_io
{
	snd_seq_t *seq;
	int client;
	int in_port[MIDI_IO_INPUT_COUNT];
	int out_port[MIDI_IO_OUTPUT_COUNT];
	snd_seq_addr_t in_from[MIDI_IO_INPUT_COUNT];
	snd_seq_addr_t out_to[MIDI_IO_OUTPUT_COUNT];
	snd_midi_event_t *decoder;
	snd_midi_event_t *encoder[MIDI_IO_OUTPUT_COUNT];
};

static const char *const input_names[MIDI_IO_INPUT_COUNT] = { "MIDI IN A", "MIDI IN B" };
static const char *const output_names[MIDI_IO_OUTPUT_COUNT] = { "MIDI OUT", "Song A", "Song B" };

int midi_io_list(midi_port_info_t *out, int max)
{
	snd_seq_t *seq;
	if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_DUPLEX, 0) < 0)
		return 0;
	int self = snd_seq_client_id(seq);
	int count = 0;
	snd_seq_client_info_t *cinfo;
	snd_seq_port_info_t *pinfo;
	snd_seq_client_info_alloca(&cinfo);
	snd_seq_port_info_alloca(&pinfo);
	snd_seq_client_info_set_client(cinfo, -1);
	while (snd_seq_query_next_client(seq, cinfo) >= 0 && count < max)
	{
		int client = snd_seq_client_info_get_client(cinfo);
		if (client == self || client == SND_SEQ_CLIENT_SYSTEM)
			continue;
		snd_seq_port_info_set_client(pinfo, client);
		snd_seq_port_info_set_port(pinfo, -1);
		while (snd_seq_query_next_port(seq, pinfo) >= 0 && count < max)
		{
			unsigned caps = snd_seq_port_info_get_capability(pinfo);
			if (caps & SND_SEQ_PORT_CAP_NO_EXPORT)
				continue;
			midi_port_info_t *p = &out[count];
			p->client = client;
			p->port = snd_seq_port_info_get_port(pinfo);
			p->readable = (caps & (SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ)) == (SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ);
			p->writable = (caps & (SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE)) == (SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE);
			if (!p->readable && !p->writable)
				continue;
			const char *cname = snd_seq_client_info_get_name(cinfo);
			const char *pname = snd_seq_port_info_get_name(pinfo);
			if (strncmp(pname, cname, strlen(cname)) == 0)
				snprintf(p->name, sizeof(p->name), "%s", pname);
			else
				snprintf(p->name, sizeof(p->name), "%s: %s", cname, pname);
			size_t len = strlen(p->name);
			while (len && p->name[len - 1] == ' ')
				p->name[--len] = 0;
			count++;
		}
	}
	snd_seq_close(seq);
	return count;
}

midi_io_t *midi_io_open(const char *client_name)
{
	midi_io_t *io = calloc(1, sizeof(*io));
	if (!io)
		return NULL;
	if (snd_seq_open(&io->seq, "default", SND_SEQ_OPEN_DUPLEX, SND_SEQ_NONBLOCK) < 0)
	{
		free(io);
		return NULL;
	}
	snd_seq_set_client_name(io->seq, client_name);
	io->client = snd_seq_client_id(io->seq);
	for (int n = 0; n < MIDI_IO_INPUT_COUNT; n++)
	{
		io->in_port[n] = snd_seq_create_simple_port(io->seq, input_names[n],
		                                            SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
		                                            SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
		io->in_from[n].client = 255;
	}
	for (int n = 0; n < MIDI_IO_OUTPUT_COUNT; n++)
	{
		io->out_port[n] = snd_seq_create_simple_port(io->seq, output_names[n],
		                                             SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
		                                             SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
		io->out_to[n].client = 255;
		snd_midi_event_new(1024, &io->encoder[n]);
	}
	snd_midi_event_new(1024, &io->decoder);
	snd_midi_event_no_status(io->decoder, 1);
	return io;
}

void midi_io_close(midi_io_t *io)
{
	if (!io)
		return;
	for (int n = 0; n < MIDI_IO_OUTPUT_COUNT; n++)
		snd_midi_event_free(io->encoder[n]);
	snd_midi_event_free(io->decoder);
	snd_seq_close(io->seq);
	free(io);
}

bool midi_io_connect_input(midi_io_t *io, int which, int client, int port)
{
	if (!io || which < 0 || which >= MIDI_IO_INPUT_COUNT)
		return false;
	if (io->in_from[which].client != 255)
		snd_seq_disconnect_from(io->seq, io->in_port[which], io->in_from[which].client, io->in_from[which].port);
	io->in_from[which].client = 255;
	if (client < 0)
		return true;
	if (snd_seq_connect_from(io->seq, io->in_port[which], client, port) < 0)
		return false;
	io->in_from[which].client = (unsigned char)client;
	io->in_from[which].port = (unsigned char)port;
	return true;
}

bool midi_io_connect_output(midi_io_t *io, int which, int client, int port)
{
	if (!io || which < 0 || which >= MIDI_IO_OUTPUT_COUNT)
		return false;
	if (io->out_to[which].client != 255)
		snd_seq_disconnect_to(io->seq, io->out_port[which], io->out_to[which].client, io->out_to[which].port);
	io->out_to[which].client = 255;
	if (client < 0)
		return true;
	if (snd_seq_connect_to(io->seq, io->out_port[which], client, port) < 0)
		return false;
	io->out_to[which].client = (unsigned char)client;
	io->out_to[which].port = (unsigned char)port;
	return true;
}

size_t midi_io_read(midi_io_t *io, int *which, uint8_t *buf, size_t max)
{
	if (!io)
		return 0;
	for (;;)
	{
		snd_seq_event_t *ev;
		if (snd_seq_event_input(io->seq, &ev) < 0)
			return 0;
		int port = -1;
		for (int n = 0; n < MIDI_IO_INPUT_COUNT; n++)
			if (ev->dest.port == io->in_port[n])
				port = n;
		if (port < 0)
			continue;
		long got = snd_midi_event_decode(io->decoder, buf, (long)max, ev);
		if (got > 0)
		{
			*which = port;
			return (size_t)got;
		}
	}
}

void midi_io_write(midi_io_t *io, int which, const uint8_t *bytes, size_t count)
{
	if (!io || which < 0 || which >= MIDI_IO_OUTPUT_COUNT || io->out_to[which].client == 255)
		return;
	while (count)
	{
		snd_seq_event_t ev;
		snd_seq_ev_clear(&ev);
		long used = snd_midi_event_encode(io->encoder[which], bytes, (long)count, &ev);
		if (used <= 0)
			return;
		bytes += used;
		count -= (size_t)used;
		if (ev.type == SND_SEQ_EVENT_NONE)
			continue;
		snd_seq_ev_set_source(&ev, io->out_port[which]);
		snd_seq_ev_set_subs(&ev);
		snd_seq_ev_set_direct(&ev);
		snd_seq_event_output_direct(io->seq, &ev);
	}
}
