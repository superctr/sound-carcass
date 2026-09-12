#include <string.h>
#include "uipc.h"
#include "state.h"

#define UIPC_STATUS_RX 0x01
#define UIPC_PACKET_START 0x04
#define UIPC_TAG_MIDI 0x50
#define UIPC_TAG_SWITCH 0x90
#define UIPC_CIN_SINGLE 0x0f
#define UIPC_CIN_SYSEX 0x04
#define UIPC_CIN_SYSEX_END 0x05
#define UIPC_CIN_COMMON2 0x02
#define UIPC_CIN_COMMON3 0x03
#define UIPC_HOST_ONLINE 0x00
#define UIPC_ANNOUNCE_FRAMES 3200
#define UIPC_POLL_FRAMES 32

/* the power-on handshake: a controller that is there and already running its application */
static const uint8_t BOOT[][2] =
{
	{ 0xe0, 0x00 }, { 0xf0, 0x00 }, { 0x00, 0xfb }, { 0x00, 0xfc }, { 0x00, 0xfd }, { 0x00, 0xff }
};

/* the same with the rear switch reported, tagged 9, before the ff */
static const uint8_t BOOT_SWITCH[][2] =
{
	{ 0xe0, 0x00 }, { 0xf0, 0x00 }, { 0x00, 0xfb }, { 0x00, 0xfc }, { 0x00, 0xfd },
	{ UIPC_TAG_SWITCH, 0x00 }, { 0x00, 0xff }
};

#define BOOT_STEPS ((uint8_t)(sizeof BOOT / sizeof BOOT[0]))
#define BOOT_SWITCH_STEPS ((uint8_t)(sizeof BOOT_SWITCH / sizeof BOOT_SWITCH[0]))

static const uint8_t CIN_LENGTH[16] = { 0, 0, 2, 3, 3, 1, 2, 3, 3, 3, 3, 3, 2, 2, 3, 1 };

void uipc_init(uipc_t *u, const uipc_link_t *link, bool reports_switch)
{
	memset(u, 0, sizeof(*u));
	u->link = *link;
	u->reports_switch = reports_switch;
}

void uipc_reset(uipc_t *u)
{
	const uipc_link_t link = u->link;
	const bool reports_switch = u->reports_switch;
	memset(u, 0, sizeof(*u));
	u->link = link;
	u->reports_switch = reports_switch;
}

static scemu_computer_switch_t computer_switch(const uipc_t *u)
{
	return u->link.computer_switch ? u->link.computer_switch(u->link.user) : SCEMU_COMPUTER_MIDI;
}

/* what the SC-8820's controller reports for each position of its switch */
static uint8_t switch_byte(scemu_computer_switch_t sw)
{
	switch (sw)
	{
	case SCEMU_COMPUTER_PC1: return 3;
	case SCEMU_COMPUTER_PC2: return 2;
	case SCEMU_COMPUTER_MAC: return 0;
	default:                 return 1;
	}
}

static uint8_t boot_steps(const uipc_t *u)
{
	return u->reports_switch ? BOOT_SWITCH_STEPS : BOOT_STEPS;
}

static uint16_t boot_word(const uipc_t *u, uint8_t step)
{
	if (!u->reports_switch)
		return (uint16_t)((BOOT[step][0] << 8) | BOOT[step][1]);
	const uint8_t status = BOOT_SWITCH[step][0];
	const uint8_t data = status == UIPC_TAG_SWITCH ? switch_byte(computer_switch(u)) : BOOT_SWITCH[step][1];
	return (uint16_t)((status << 8) | data);
}

static void rx_event(uipc_t *u)
{
	if (u->link.rx_event)
		u->link.rx_event(u->link.user);
}

static void tx_event(uipc_t *u)
{
	if (u->link.tx_event)
		u->link.tx_event(u->link.user);
}

static bool peek(const uipc_t *u, uint16_t *value)
{
	if (u->boot < boot_steps(u))
	{
		*value = boot_word(u, u->boot);
		return true;
	}
	if (!u->rx_count)
		return false;
	*value = u->rx[u->rx_head];
	return true;
}

static void push(uipc_t *u, uint8_t status, uint8_t byte)
{
	if (u->rx_count >= UIPC_RX)
		return;
	u->rx[(u->rx_head + u->rx_count) % UIPC_RX] = (uint16_t)((status << 8) | byte);
	u->rx_count++;
	rx_event(u);
}

static void send(uipc_t *u, int port, uint8_t cin, const uint8_t *msg)
{
	push(u, UIPC_TAG_MIDI | UIPC_PACKET_START, (uint8_t)((port << 4) | cin));
	push(u, UIPC_TAG_MIDI, msg[0]);
	push(u, UIPC_TAG_MIDI, msg[1]);
	push(u, UIPC_TAG_MIDI, msg[2]);
}

void uipc_take_midi(uipc_t *u, int port, uint8_t byte)
{
	uipc_in_t *in = &u->in[port];
	if (byte >= 0xf8)
	{
		const uint8_t one[3] = { byte, 0, 0 };
		send(u, port, UIPC_CIN_SINGLE, one);
		return;
	}
	if (byte == 0xf7)
	{
		if (!in->sysex)
			return;
		const uint8_t cin = (uint8_t)(UIPC_CIN_SYSEX_END + in->count);
		in->msg[in->count++] = byte;
		while (in->count < 3)
			in->msg[in->count++] = 0;
		send(u, port, cin, in->msg);
		in->sysex = false;
		in->count = 0;
		return;
	}
	if (byte >= 0x80)
	{
		in->sysex = false;
		in->count = 0;
		in->msg[0] = byte;
		in->msg[1] = 0;
		in->msg[2] = 0;
		switch (byte)
		{
		case 0xf0:
			in->sysex = true;
			in->status = 0;
			in->count = 1;
			break;
		case 0xf1:
		case 0xf3:
			in->status = 0;
			in->count = 1;
			in->need = 2;
			in->cin = UIPC_CIN_COMMON2;
			break;
		case 0xf2:
			in->status = 0;
			in->count = 1;
			in->need = 3;
			in->cin = UIPC_CIN_COMMON3;
			break;
		case 0xf6:
			in->status = 0;
			send(u, port, UIPC_CIN_SYSEX_END, in->msg);
			break;
		default:
			if (byte >= 0xf4)
			{
				in->status = 0;
				break;
			}
			in->status = byte;
			in->count = 1;
			in->need = (byte & 0xe0) == 0xc0 ? 2 : 3;
			in->cin = (uint8_t)(byte >> 4);
			break;
		}
		return;
	}
	if (in->sysex)
	{
		in->msg[in->count++] = byte;
		if (in->count == 3)
		{
			send(u, port, UIPC_CIN_SYSEX, in->msg);
			in->count = 0;
		}
		return;
	}
	if (in->count == 0)
	{
		if (!in->status)
			return;
		in->msg[0] = in->status;
		in->msg[1] = 0;
		in->msg[2] = 0;
		in->count = 1;
	}
	in->msg[in->count++] = byte;
	if (in->count == in->need)
	{
		send(u, port, in->cin, in->msg);
		in->count = 0;
	}
}

static void deliver(uipc_t *u)
{
	const int port = u->tx[0] >> 4, length = CIN_LENGTH[u->tx[0] & 0x0f];
	if (!length || port >= UIPC_PORTS || !u->link.midi_out)
		return;
	u->link.midi_out(u->link.user, port, u->tx + 1, (size_t)length);
}

uint8_t uipc_read(uipc_t *u, int channel, uint32_t offset)
{
	uint16_t head;
	if (channel == 0)
		return 0x00;
	if (!peek(u, &head))
		return 0x00;
	if (offset)
		return (uint8_t)((head >> 8) | UIPC_STATUS_RX);
	if (u->boot < boot_steps(u))
	{
		if (++u->boot == boot_steps(u))
		{
			u->running = true;
			u->host = computer_switch(u) == SCEMU_COMPUTER_MAC;
			u->announce = UIPC_ANNOUNCE_FRAMES;
		}
	}
	else
	{
		u->rx_head = (uint16_t)((u->rx_head + 1) % UIPC_RX);
		u->rx_count--;
	}
	return (uint8_t)head;
}

void uipc_write(uipc_t *u, int channel, uint32_t offset, uint8_t data)
{
	if (channel != 0 || !u->running)
		return;
	if (offset)
	{
		u->tx[0] = data;
		u->tx_count = 1;
	}
	else if (u->tx_count > 0 && u->tx_count < 4)
	{
		u->tx[u->tx_count++] = data;
		if (u->tx_count == 4)
		{
			deliver(u);
			u->tx_count = 0;
		}
	}
	if (u->online)
		tx_event(u);
}

void uipc_frame(uipc_t *u)
{
	if (!u->running || !u->host)
		return;
	if (!u->online)
	{
		if (u->announce)
		{
			u->announce--;
			return;
		}
		u->online = true;
		push(u, 0x00, UIPC_HOST_ONLINE);
		return;
	}
	if (++u->poll >= UIPC_POLL_FRAMES)
	{
		u->poll = 0;
		tx_event(u);
	}
}

/* ---------------------------------------------------------------- the state */

/* the mailbox queue from its head, so a load starts it at zero */
static void put_rx(state_writer_t *w, void *user)
{
	const uipc_t *u = user;
	put16(w, u->rx_count);
	for (uint16_t n = 0; n < u->rx_count; n++)
		put16(w, u->rx[(u->rx_head + n) % UIPC_RX]);
}

static void get_rx(state_reader_t *r, void *user)
{
	uipc_t *u = user;
	const uint16_t count = get16(r);
	if (count > UIPC_RX)
	{
		r->ok = false;
		return;
	}
	u->rx_head = 0;
	u->rx_count = count;
	for (uint16_t n = 0; n < count; n++)
		u->rx[n] = get16(r);
}

void uipc_state(uipc_t *u, state_registry_t *reg)
{
	state_custom(reg, put_rx, get_rx, u);
	state_array(reg, u->tx);
	state_var(reg, u->tx_count);
	state_var(reg, u->boot);
	state_bool(reg, u->running);
	state_bool(reg, u->host);
	state_bool(reg, u->online);
	for (int n = 0; n < 3; n++)
		state_field(reg, u->in, UIPC_PORTS, msg[n]);
	state_field(reg, u->in, UIPC_PORTS, count);
	state_field(reg, u->in, UIPC_PORTS, need);
	state_field(reg, u->in, UIPC_PORTS, cin);
	state_field(reg, u->in, UIPC_PORTS, status);
	state_bool_field(reg, u->in, UIPC_PORTS, sysex);
	state_var(reg, u->announce);
	state_var(reg, u->poll);
}
