#include <string.h>
#include "sub55_hle.h"
#include "state.h"

#define DP_TX_BLOCK 0x00
#define DP_CHANNEL 0x20
#define DP_VERSION 0x28
#define DP_CONFIG 0xb0
#define DP_COMMAND 0xb9
#define DP_MAIN_READS 0x20

#define REG_FLAGS 0xc0
#define REG_FLAGS_END 0xd8
#define REG_KEYS 0xf5
#define REG_PORT 0xf6
#define REG_PORT_DIR 0xf7
#define REG_REASON 0xf8
#define REG_IPC1 0xf9
#define REG_IPC3 0xfb
#define REG_COLLISION 0xfe
#define REG_SEM 0xff

#define REASON_CHANNEL 0x01
#define REASON_TX_BLOCK 0x10
#define REASON_SENSE 0x20

#define COMMAND_START 0x01
#define COMMAND_TRANSMIT 0x02
#define COMMAND_RESET 0x80

#define SENSE_TIMEOUT 50
#define SENSE_FIRST 24
#define SENSE_PERIOD 25

#define LED_MASK 0x70

/* ---------------------------------------------------------------- */
/* access flags                                                     */

static void flag_set(sub55_hle_t *sub, uint32_t offset)
{
	sub->flags[offset >> 3] |= (uint8_t)(1u << (offset & 7));
}

static void flag_clear(sub55_hle_t *sub, uint32_t offset)
{
	sub->flags[offset >> 3] &= (uint8_t)~(1u << (offset & 7));
}

static bool flag_test(const sub55_hle_t *sub, uint32_t offset)
{
	return ((sub->flags[offset >> 3] >> (offset & 7)) & 1) != 0;
}

static void sub_write(sub55_hle_t *sub, uint32_t offset, uint8_t data)
{
	sub->dpram[offset] = data;
	flag_set(sub, offset);
}

static uint8_t sub_read(sub55_hle_t *sub, uint32_t offset)
{
	flag_clear(sub, offset);
	return sub->dpram[offset];
}

static void reason_set(sub55_hle_t *sub, uint8_t bits)
{
	sub->reason |= bits;
	if (sub->attention)
		sub->attention(sub->user);
}

/* ---------------------------------------------------------------- */
/* the shared output                                                */

static void tx_push(sub55_hle_t *sub, uint8_t byte)
{
	if (sub->tx_count >= SUB55_TX_SIZE)
		return;
	sub->tx[(sub->tx_head + sub->tx_count) % SUB55_TX_SIZE] = byte;
	sub->tx_count++;
}

static void tx_filtered(sub55_hle_t *sub, uint8_t byte)
{
	if (byte < 0xf8)
	{
		if (byte >= 0x80 && byte <= 0xef)
		{
			if (byte == sub->tx_status)
				return;
			sub->tx_status = byte;
		}
		else if (byte == 0xf0 || byte == 0xf7)
			sub->tx_status = 0;
	}
	tx_push(sub, byte);
}

static void transmit_block(sub55_hle_t *sub)
{
	for (uint32_t i = 0; i < SUB55_TX_BLOCK; i++)
	{
		if (!flag_test(sub, DP_TX_BLOCK + i))
			break;
		tx_filtered(sub, sub_read(sub, DP_TX_BLOCK + i));
	}
	reason_set(sub, REASON_TX_BLOCK);
}

/* ---------------------------------------------------------------- */
/* MIDI input parsing                                               */

static int target_channel(const sub55_hle_t *sub, int source)
{
	const uint8_t route = sub->dpram[DP_CONFIG + source];

	if (source == SUB55_COMPUTER)
	{
		if (route == 0x00)
			return -1;
		return (route == 0x07) ? 0 : 1;
	}
	if (route == 0x01)
		return 0;
	if (route == 0x08)
		return -1;
	return 1;
}

static void channel_push(sub55_hle_t *sub, int channel, uint8_t byte)
{
	sub55_channel_t *c = &sub->ch[channel];

	if (byte >= 0xf8)
		return;
	if (byte >= 0x80 && byte <= 0xef)
	{
		if (byte == c->last_status)
			return;
		c->last_status = byte;
	}
	else if (byte >= 0xf0)
		c->last_status = 0;

	if (c->count >= SUB55_FIFO_SIZE)
		return;
	c->fifo[(c->head + c->count) % SUB55_FIFO_SIZE] = byte;
	c->count++;
}

static void emit(sub55_hle_t *sub, int channel, bool out, uint8_t byte)
{
	if (channel >= 0)
		channel_push(sub, channel, byte);
	if (out)
		tx_filtered(sub, byte);
}

static uint8_t message_length(uint8_t status)
{
	if (status >= 0xf0)
		return (status == 0xf2) ? 2 : 1;
	return (status >= 0xc0 && status <= 0xdf) ? 1 : 2;
}

static void parse(sub55_hle_t *sub, int source, uint8_t byte, int channel, bool out)
{
	sub55_source_t *s = &sub->src[source];

	if (byte >= 0xf8)
	{
		emit(sub, channel, out, byte);
		return;
	}

	if (byte >= 0x80)
	{
		s->count = 0;
		switch (byte)
		{
		case 0xf0:
			s->status = byte;
			emit(sub, channel, out, byte);
			break;
		case 0xf4:
		case 0xf5:
			s->status = 0;
			break;
		case 0xf6:
		case 0xf7:
			s->status = 0;
			emit(sub, channel, out, byte);
			break;
		default:
			s->status = byte;
			break;
		}
		return;
	}

	if (s->status == 0)
		return;
	if (s->status == 0xf0)
	{
		emit(sub, channel, out, byte);
		return;
	}

	s->data[s->count++] = byte;
	if (s->count < message_length(s->status))
		return;

	emit(sub, channel, out, s->status);
	for (uint8_t i = 0; i < s->count; i++)
		emit(sub, channel, out, s->data[i]);
	s->count = 0;
	if (s->status >= 0xf0)
		s->status = 0;
}

bool sub55_hle_ready(const sub55_hle_t *sub, int source)
{
	if (source < 0 || source >= SUB55_SOURCES)
		return true;
	if (!sub->started)
		return false;

	const int channel = target_channel(sub, source);
	if (channel < 0)
		return true;
	return sub->ch[channel].count + 3 <= SUB55_FIFO_SIZE;
}

void sub55_hle_midi_byte(sub55_hle_t *sub, int source, uint8_t byte)
{
	if (source < 0 || source >= SUB55_SOURCES || !sub->started)
		return;

	sub55_source_t *s = &sub->src[source];
	if (s->sense)
		s->sense = SENSE_TIMEOUT;
	if (byte == 0xfe)
	{
		s->sense = SENSE_TIMEOUT;
		return;
	}

	const uint8_t route = sub->dpram[DP_CONFIG + source];
	if (source == SUB55_COMPUTER && route == 0x00)
		return;

	const bool out = (source == SUB55_COMPUTER) ? (route == 0x07) : (route == 0x08);
	parse(sub, source, byte, target_channel(sub, source), out);
}

/* ---------------------------------------------------------------- */
/* the port passthrough                                             */

static uint8_t port_pins(const sub55_hle_t *sub)
{
	return (uint8_t)(sub->p0 | (uint8_t)~sub->p0_dir);
}

static uint8_t matrix_r(const sub55_hle_t *sub)
{
	const uint8_t strobes = port_pins(sub);
	uint8_t columns = 0xff;

	for (int row = 0; row < 4; row++)
		if (!((strobes >> row) & 1))
			columns &= sub->keys[row];
	return columns;
}

void sub55_hle_set_key(sub55_hle_t *sub, int row, int bit, bool down)
{
	if (down)
		sub->keys[row & 3] &= (uint8_t)~(1u << (bit & 7));
	else
		sub->keys[row & 3] |= (uint8_t)(1u << (bit & 7));
}

uint8_t sub55_hle_leds(const sub55_hle_t *sub)
{
	const uint8_t lit = (uint8_t)(~port_pins(sub) & sub->p0_dir & LED_MASK);

	return (uint8_t)(((lit >> 6) & 1) | ((lit >> 4) & 2) | ((lit >> 2) & 4));
}

/* ---------------------------------------------------------------- */
/* the system bus interface                                         */

static void reset(sub55_hle_t *sub, bool ports);

static void doorbell(sub55_hle_t *sub)
{
	sub->sem &= 0x7f;

	const uint8_t command = sub->dpram[DP_COMMAND];
	if (command & COMMAND_RESET)
	{
		reset(sub, false);
		return;
	}

	if (command & COMMAND_START)
	{
		sub->started = true;
		sub->sensing = sub->dpram[DP_CONFIG + 3] == 0x04
				|| sub->dpram[DP_CONFIG + 4] == 0x04
				|| sub->dpram[DP_CONFIG + 5] == 0x04;
		sub->sense_div = SENSE_FIRST;
		sub->tick_frames = sub->tick_reload;
	}
	if (command & COMMAND_TRANSMIT)
		transmit_block(sub);

	sub_write(sub, DP_COMMAND, 0x00);
	sub->sem |= 0x80;
}

uint8_t sub55_hle_read(sub55_hle_t *sub, uint32_t offset)
{
	offset &= 0xff;

	if (offset < SUB55_DPRAM_SIZE)
	{
		if (offset >= DP_MAIN_READS)
			flag_clear(sub, offset);
		return sub->dpram[offset];
	}
	if (offset < REG_FLAGS_END)
		return sub->flags[offset - REG_FLAGS];

	switch (offset)
	{
	case REG_KEYS:
		return matrix_r(sub);
	case REG_PORT:
		return port_pins(sub);
	case REG_PORT_DIR:
		return sub->p0_dir;
	case REG_REASON:
	{
		const uint8_t reason = sub->reason;
		sub->reason = 0;
		return reason;
	}
	case REG_IPC1:
	case REG_IPC1 + 1:
	case REG_IPC3:
	case REG_COLLISION:
		return 0x00;
	case REG_SEM:
		return sub->sem;
	default:
		return 0xff;
	}
}

void sub55_hle_write(sub55_hle_t *sub, uint32_t offset, uint8_t data)
{
	offset &= 0xff;

	if (offset < SUB55_DPRAM_SIZE)
	{
		sub->dpram[offset] = data;
		flag_set(sub, offset);
		return;
	}

	switch (offset)
	{
	case REG_PORT:
		sub->p0 = data;
		break;
	case REG_PORT_DIR:
		sub->p0_dir = data;
		break;
	case REG_REASON:
		doorbell(sub);
		break;
	case REG_SEM:
	{
		const uint8_t bit = (uint8_t)(1u << (data & 7));
		if (data & 0x80)
			sub->sem |= bit;
		else
			sub->sem &= (uint8_t)~bit;
		break;
	}
	default:
		break;
	}
}

/* ---------------------------------------------------------------- */
/* time                                                             */

static void tick(sub55_hle_t *sub)
{
	if (!sub->started)
		return;

	if (sub->sensing && sub->sense_div && !--sub->sense_div)
	{
		sub->sense_div = SENSE_PERIOD;
		if (!sub->tx_count)
			tx_push(sub, 0xfe);
	}

	for (int i = 0; i < SUB55_SOURCES; i++)
	{
		sub55_source_t *s = &sub->src[i];
		if (s->sense && !--s->sense)
			reason_set(sub, (uint8_t)(REASON_SENSE << i));
	}
}

static void deliver(sub55_hle_t *sub)
{
	for (int i = 0; i < SUB55_CHANNELS; i++)
	{
		sub55_channel_t *c = &sub->ch[i];
		const uint32_t offset = DP_CHANNEL + (uint32_t)i;

		if (!c->count || flag_test(sub, offset))
			continue;
		sub_write(sub, offset, c->fifo[c->head]);
		c->head = (uint16_t)((c->head + 1) % SUB55_FIFO_SIZE);
		c->count--;
		reason_set(sub, (uint8_t)(REASON_CHANNEL << i));
	}
}

void sub55_hle_frame(sub55_hle_t *sub)
{
	if (sub->boot_frames && !--sub->boot_frames)
	{
		sub->dpram[DP_VERSION + 0] = '1';
		sub->dpram[DP_VERSION + 1] = '0';
		sub->dpram[DP_VERSION + 2] = '1';
		sub->sem = 0x80;
	}
	if (sub->boot_frames)
		return;

	if (sub->tick_frames && !--sub->tick_frames)
	{
		sub->tick_frames = sub->tick_reload;
		tick(sub);
	}

	deliver(sub);

	if (sub->tx_frames)
		sub->tx_frames--;
	if (!sub->tx_frames && sub->tx_count)
	{
		const uint8_t byte = sub->tx[sub->tx_head];
		sub->tx_head = (uint8_t)((sub->tx_head + 1) % SUB55_TX_SIZE);
		sub->tx_count--;
		sub->tx_frames = sub->tx_reload;
		if (sub->midi_out)
			sub->midi_out(sub->user, byte);
	}
}

/* ---------------------------------------------------------------- */
/* life cycle                                                       */

/* the port latch and its direction are shared hardware the main CPU owns, so only a
   power-on reset clears them; the sub's own reset (command bit 7) leaves them alone */
static void reset(sub55_hle_t *sub, bool ports)
{
	memset(sub->dpram, 0, sizeof(sub->dpram));
	memset(sub->flags, 0, sizeof(sub->flags));
	sub->reason = 0;
	sub->sem = 0x00;
	sub->boot_frames = sub->boot_reload;
	if (ports)
	{
		sub->p0 = 0;
		sub->p0_dir = 0;
	}
	sub->started = false;

	memset(sub->src, 0, sizeof(sub->src));
	memset(sub->ch, 0, sizeof(sub->ch));
	memset(sub->tx, 0, sizeof(sub->tx));
	sub->tx_head = sub->tx_count = 0;
	sub->tx_status = 0;
	sub->tx_frames = 0;

	sub->tick_frames = sub->tick_reload;
	sub->sense_div = 0;
	sub->sensing = false;
}

void sub55_hle_reset(sub55_hle_t *sub)
{
	reset(sub, true);
}

void sub55_hle_init(sub55_hle_t *sub, uint32_t rate,
		void (*attention)(void *user), void (*midi_out)(void *user, uint8_t byte), void *user)
{
	memset(sub, 0, sizeof(*sub));
	sub->attention = attention;
	sub->midi_out = midi_out;
	sub->user = user;

	sub->tick_reload = rate / 100;
	if (!sub->tick_reload)
		sub->tick_reload = 1;
	sub->tx_reload = rate / 3125;
	if (!sub->tx_reload)
		sub->tx_reload = 1;
	sub->boot_reload = rate * 24 / 10000;
	if (!sub->boot_reload)
		sub->boot_reload = 1;

	memset(sub->keys, 0xff, sizeof(sub->keys));
	reset(sub, true);
}

/* ---------------------------------------------------------------- the state */

/* a channel's bytes from its head, so a load starts it at zero */
static void put_fifos(state_writer_t *w, void *user)
{
	const sub55_hle_t *sub = user;
	for (int n = 0; n < SUB55_CHANNELS; n++)
	{
		const sub55_channel_t *ch = &sub->ch[n];
		put16(w, ch->count);
		for (uint16_t i = 0; i < ch->count; i++)
			put8(w, ch->fifo[(ch->head + i) % SUB55_FIFO_SIZE]);
		put8(w, ch->last_status);
	}
	put8(w, sub->tx_count);
	for (uint8_t i = 0; i < sub->tx_count; i++)
		put8(w, sub->tx[(sub->tx_head + i) % SUB55_TX_SIZE]);
}

static void get_fifos(state_reader_t *r, void *user)
{
	sub55_hle_t *sub = user;
	for (int n = 0; n < SUB55_CHANNELS; n++)
	{
		sub55_channel_t *ch = &sub->ch[n];
		const uint16_t count = get16(r);
		if (count > SUB55_FIFO_SIZE)
		{
			r->ok = false;
			return;
		}
		ch->head = 0;
		ch->count = count;
		for (uint16_t i = 0; i < count; i++)
			ch->fifo[i] = get8(r);
		ch->last_status = get8(r);
	}
	const uint8_t count = get8(r);
	if (count > SUB55_TX_SIZE)
	{
		r->ok = false;
		return;
	}
	sub->tx_head = 0;
	sub->tx_count = count;
	for (uint8_t i = 0; i < count; i++)
		sub->tx[i] = get8(r);
}

void sub55_hle_state(sub55_hle_t *sub, state_registry_t *reg)
{
	state_array(reg, sub->dpram);
	state_array(reg, sub->flags);
	state_var(reg, sub->reason);
	state_var(reg, sub->sem);
	state_var(reg, sub->p0);
	state_var(reg, sub->p0_dir);
	state_array(reg, sub->keys);
	state_bool(reg, sub->started);

	state_field(reg, sub->src, SUB55_SOURCES, status);
	state_field(reg, sub->src, SUB55_SOURCES, data[0]);
	state_field(reg, sub->src, SUB55_SOURCES, data[1]);
	state_field(reg, sub->src, SUB55_SOURCES, count);
	state_field(reg, sub->src, SUB55_SOURCES, sense);

	state_custom(reg, put_fifos, get_fifos, sub);
	state_var(reg, sub->tx_status);
	state_var(reg, sub->tx_frames);
	state_var(reg, sub->boot_frames);
	state_var(reg, sub->tick_frames);
	state_var(reg, sub->sense_div);
	state_bool(reg, sub->sensing);
}
