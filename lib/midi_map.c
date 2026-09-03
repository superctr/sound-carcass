#include <string.h>
#include "midi_map.h"

void midi_map_reset(midi_map_t *f, uint8_t map)
{
	memset(f, 0, sizeof(*f));
	f->map = map;
}

static size_t select_part(const midi_map_t *f, int channel, uint8_t *out)
{
	out[0] = (uint8_t)(0xb0 | channel);
	out[1] = 0x20;
	out[2] = f->map;
	return 3;
}

size_t midi_map_preset(midi_map_t *f, int port, uint8_t *out)
{
	size_t n = 0;
	midi_map_port_t *p = &f->port[port & (MIDI_MAP_PORTS - 1)];
	for (int channel = 0; channel < 16; channel++)
	{
		n += select_part(f, channel, out + n);
		out[n++] = (uint8_t)(0xc0 | channel);
		out[n++] = 0x00;
	}
	p->selected = 0xffff;
	return n;
}

static bool is_reset(const uint8_t *x, int len)
{
	if (len >= 4 && x[0] == 0x7e && x[2] == 0x09 && (x[3] == 0x01 || x[3] == 0x03))
		return true;
	if (len >= 7 && x[0] == 0x43 && x[2] == 0x4c && x[3] == 0x00 && x[4] == 0x00 && x[5] == 0x7e)
		return true;
	if (len >= 7 && x[0] == 0x41 && x[2] == 0x42 && x[3] == 0x12)
	{
		if (x[4] == 0x40 && x[5] == 0x00 && x[6] == 0x7f)
			return true;
		if (x[4] == 0x00 && x[5] == 0x00 && x[6] == 0x7f)
			return true;
	}
	return false;
}

static size_t channel_message(midi_map_t *f, midi_map_port_t *p, int port, uint8_t *out)
{
	const int kind = p->status & 0xf0;
	const int channel = p->status & 0x0f;
	size_t n = 0;

	if (kind == 0xb0 && p->data[0] == 0x20)
	{
		p->data[1] = f->map;
		p->selected |= (uint16_t)(1u << channel);
	}
	else if (kind == 0xc0 && !(p->selected & (1u << channel)))
	{
		n += select_part(f, channel, out);
		p->selected |= (uint16_t)(1u << channel);
	}

	out[n++] = p->status;
	out[n++] = p->data[0];
	if (p->need == 2)
		out[n++] = p->data[1];
	return n;
}

size_t midi_map_filter(midi_map_t *f, int port, uint8_t byte, uint8_t *out)
{
	midi_map_port_t *p = &f->port[port & (MIDI_MAP_PORTS - 1)];

	if (byte >= 0xf8)
	{
		out[0] = byte;
		return 1;
	}

	if (p->in_sysex)
	{
		out[0] = byte;
		if (byte == 0xf7)
		{
			p->in_sysex = false;
			if (is_reset(p->sysex, p->sysex_len))
				return 1 + midi_map_preset(f, port, out + 1);
			return 1;
		}
		if (byte & 0x80)
		{
			p->in_sysex = false;
			p->have = 0;
		}
		else
		{
			if (p->sysex_len < sizeof(p->sysex))
				p->sysex[p->sysex_len++] = byte;
			return 1;
		}
	}

	if (byte == 0xf0)
	{
		p->in_sysex = true;
		p->sysex_len = 0;
		p->status = 0;
		out[0] = byte;
		return 1;
	}

	if (byte & 0x80)
	{
		if (byte >= 0xf0)
		{
			p->status = 0;
			out[0] = byte;
			return 1;
		}
		p->status = byte;
		p->have = 0;
		p->need = ((byte & 0xf0) == 0xc0 || (byte & 0xf0) == 0xd0) ? 1 : 2;
		return 0;
	}

	if (!p->status)
		return 0;

	p->data[p->have++] = byte;
	if (p->have < p->need)
		return 0;
	p->have = 0;
	return channel_message(f, p, port, out);
}
