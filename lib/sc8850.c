#include <stdlib.h>
#include <string.h>
#include "scemu_internal.h"
#include "wave_rom.h"

#define GA_SCAN_FRAMES 8
#define GA_TICK_FRAMES 320
#define GA_SEQUENCER_FRAMES 32

#define GA_SOURCE_SWITCHES 0
#define GA_SOURCE_ENCODER 1
#define GA_SOURCE_UIPC_TX 2
#define GA_SOURCE_UIPC_RX 3
#define GA_SOURCE_TICK 10
#define GA_SOURCE_SEQUENCER 11

#define UIPC_STATUS_RX 0x01
#define UIPC_PACKET_START 0x04
#define UIPC_TAG_MIDI 0x50
#define UIPC_CIN_SINGLE 0x0f
#define UIPC_CIN_SYSEX 0x04
#define UIPC_CIN_SYSEX_END 0x05
#define UIPC_CIN_COMMON2 0x02
#define UIPC_CIN_COMMON3 0x03
#define UIPC_HOST_ONLINE 0x00
#define UIPC_ANNOUNCE_FRAMES 3200
#define UIPC_POLL_FRAMES 32

#define GA_LEDS_LOW 0x38
#define GA_LEDS_HIGH 0x39
#define GA_ENABLE_LOW 0x3e
#define GA_ENABLE_HIGH 0x3f
#define GA_SOURCE 0x40
#define GA_ENCODER 0x42
#define GA_SWITCH 0x43

#define PE_MUTE_RELEASE 0x2000
#define PA_EVENT 0x0100

#define SC8820_PE_MUTE_RELEASE 0x0200
#define SC8820_PE_POWER_LAMP 0x0010
#define SC8820_PE_MAP_KEY 0x0040
#define SC8820_PE_PREVIEW_KEY 0x0080
#define SC8820_PE_ROW0 0x8000
#define SC8820_PE_ROW1 0x4000
#define SC8820_PA_ROW2 0x8000
#define SC8820_PE_COLUMNS 0x000f
#define SC8820_PE_IDLE_PINS 0x00c0
#define SC8820_UIPC_TAG_SWITCH 0x90

#define ROM_ID_SEED 0xcbf29ce484222325ull

const char *sc8850_validate_roms(scemu_model_t model, const scemu_roms_t *roms)
{
	if (model == SCEMU_MODEL_SC8820)
	{
		if (!roms->boot_rom || roms->boot_rom_size != SC8850_BOOT_ROM_SIZE)
			return "CPU ROM must be 64 KB";
		if (!roms->program_rom || roms->program_rom_size != SC8820_PROGRAM_ROM_SIZE)
			return "program flash must be 2 MB";
		if (roms->tone_rom || roms->tone_rom_size)
			return "the SC-8820 has no separate tone flash";
		if (roms->wave_rom_count != 2 || !roms->wave_rom[0] || !roms->wave_rom[1]
		    || roms->wave_rom_size[0] != 0x1000000 || roms->wave_rom_size[1] != 0x800000)
			return "wave ROMs must be 16 MB and 8 MB";
		return NULL;
	}
	if (model != SCEMU_MODEL_SC8850)
		return "not an SC-8850";
	if (!roms->boot_rom || roms->boot_rom_size != SC8850_BOOT_ROM_SIZE)
		return "boot ROM must be 64 KB";
	if (!roms->program_rom || roms->program_rom_size != SC8850_PROGRAM_ROM_SIZE)
		return "program flash must be 1 MB";
	if (!roms->tone_rom || roms->tone_rom_size != SC8850_TONE_ROM_SIZE)
		return "tone flash must be 2 MB";
	if (roms->wave_rom_count != 2)
		return "two wave ROMs expected";
	for (int n = 0; n < 2; n++)
		if (!roms->wave_rom[n] || roms->wave_rom_size[n] != 0x1000000)
			return "wave ROMs must be 16 MB each";
	return NULL;
}

/* ---------------------------------------------------------------- the gate array */

static void ga_present(sc8850_t *b)
{
	sc8850_ga_t *ga = &b->ga;
	const uint16_t enabled = ga->requests & (uint16_t)(ga->regs[GA_ENABLE_LOW] | (ga->regs[GA_ENABLE_HIGH] << 8));
	if (ga->pending || !enabled)
		return;
	ga->source = 0;
	while (!((enabled >> ga->source) & 1))
		ga->source++;
	ga->pending = true;
	sh2_set_irq(&b->cpu, SH2_IRQ2, true);
}

static void ga_raise(sc8850_t *b, int source)
{
	b->ga.requests |= (uint16_t)(1u << source);
	ga_present(b);
}

static void ga_lower(sc8850_t *b)
{
	sc8850_ga_t *ga = &b->ga;
	ga->requests &= (uint16_t)~(1u << ga->source);
	ga->pending = false;
	sh2_set_irq(&b->cpu, SH2_IRQ2, false);
	ga_present(b);
}

static void ga_update_leds(sc8850_ga_t *ga)
{
	const uint8_t low = ga->regs[GA_LEDS_LOW], high = ga->regs[GA_LEDS_HIGH];
	uint32_t leds = 0;
	if (!(low & 0x01))
		leds |= 1u << SCEMU_LED_SOLO;
	if (!(low & 0x02))
		leds |= 1u << SCEMU_LED_MUTE;
	if (!(high & 0x01))
		leds |= 1u << SCEMU_LED_EDIT;
	if (!(high & 0x02))
		leds |= 1u << SCEMU_LED_DRUM;
	if (!(high & 0x04))
		leds |= 1u << SCEMU_LED_EFFECTS;
	ga->leds = leds;
}

static void ga_reset(sc8850_t *b)
{
	sc8850_ga_t *ga = &b->ga;
	memset(ga->regs, 0, sizeof(ga->regs));
	memset(ga->key_state, 0, sizeof(ga->key_state));
	ga->requests = 0;
	ga->source = 0;
	ga->event = 0;
	ga->pending = false;
	ga->encoder = 0;
	ga->scan_position = 0;
	ga->scan_encoder = false;
	ga->scan_frames = 0;
	ga->tick_frames = 0;
	ga->sequencer_frames = 0;
	ga_update_leds(ga);
}

static uint8_t ga_read(sc8850_t *b, uint32_t offset)
{
	sc8850_ga_t *ga = &b->ga;
	switch (offset)
	{
	case GA_SOURCE:
	{
		const uint8_t data = ga->source;
		if (ga->source > GA_SOURCE_ENCODER)
			ga_lower(b);
		return data;
	}
	case GA_ENCODER:
	{
		const int32_t clamped = ga->encoder < -128 ? -128 : ga->encoder > 127 ? 127 : ga->encoder;
		ga->encoder -= clamped;
		ga_lower(b);
		return (uint8_t)clamped;
	}
	case GA_SWITCH:
	{
		const uint8_t data = ga->event;
		ga_lower(b);
		return data;
	}
	default:
		return ga->regs[offset];
	}
}

static void ga_write(sc8850_t *b, uint32_t offset, uint8_t data)
{
	sc8850_ga_t *ga = &b->ga;
	ga->regs[offset] = data;
	switch (offset)
	{
	case GA_LEDS_LOW:
	case GA_LEDS_HIGH:
		ga_update_leds(ga);
		break;
	case GA_ENABLE_LOW:
	case GA_ENABLE_HIGH:
		ga_present(b);
		break;
	default:
		break;
	}
}

static void ga_scan(sc8850_t *b)
{
	sc8850_ga_t *ga = &b->ga;
	if (ga->requests & 0x0003)
		return;

	ga->scan_encoder = !ga->scan_encoder;
	if (ga->scan_encoder && ga->encoder != 0)
	{
		ga_raise(b, GA_SOURCE_ENCODER);
		return;
	}

	for (int n = 0; n < 32; n++)
	{
		ga->scan_position = (uint8_t)((ga->scan_position + 1) & 0x1f);
		const int row = ga->scan_position >> 3, column = ga->scan_position & 7;
		const uint8_t mask = (uint8_t)(1u << column);
		if (((ga->keys[row] ^ ga->key_state[row]) & mask) == 0)
			continue;
		ga->key_state[row] ^= mask;
		const bool up = ((ga->key_state[row] & mask) != 0) == (column == 7);
		ga->event = (uint8_t)((up ? 0x80 : 0x00) | (row << 3) | column);
		ga_raise(b, GA_SOURCE_SWITCHES);
		return;
	}
}

static void ga_frame(sc8850_t *b)
{
	sc8850_ga_t *ga = &b->ga;
	if (++ga->scan_frames >= GA_SCAN_FRAMES)
	{
		ga->scan_frames = 0;
		ga_scan(b);
	}
	if (++ga->tick_frames >= GA_TICK_FRAMES)
	{
		ga->tick_frames = 0;
		ga_raise(b, GA_SOURCE_TICK);
	}
	if (++ga->sequencer_frames >= GA_SEQUENCER_FRAMES)
	{
		ga->sequencer_frames = 0;
		ga_raise(b, GA_SOURCE_SEQUENCER);
	}
}

/* ---------------------------------------------------------------- the USB controller's mailboxes */

static const uint8_t UIPC_BOOT[][2] =
{
	{ 0xe0, 0x00 }, { 0xf0, 0x00 }, { 0x00, 0xfb }, { 0x00, 0xfc }, { 0x00, 0xfd }, { 0x00, 0xff }
};

/* the SC-8820's controller reads the rear switch itself and reports it, tagged 9, before the ff */
static const uint8_t UIPC_BOOT_SC8820[][2] =
{
	{ 0xe0, 0x00 }, { 0xf0, 0x00 }, { 0x00, 0xfb }, { 0x00, 0xfc }, { 0x00, 0xfd },
	{ SC8820_UIPC_TAG_SWITCH, 0x00 }, { 0x00, 0xff }
};

#define UIPC_BOOT_STEPS ((uint8_t)(sizeof UIPC_BOOT / sizeof UIPC_BOOT[0]))
#define UIPC_BOOT_STEPS_SC8820 ((uint8_t)(sizeof UIPC_BOOT_SC8820 / sizeof UIPC_BOOT_SC8820[0]))

static uint8_t uipc_boot_steps(const sc8850_t *b)
{
	return b->one_chip ? UIPC_BOOT_STEPS_SC8820 : UIPC_BOOT_STEPS;
}

static uint8_t sc8820_switch_byte(scemu_computer_switch_t sw)
{
	switch (sw)
	{
	case SCEMU_COMPUTER_PC1: return 3;
	case SCEMU_COMPUTER_PC2: return 2;
	case SCEMU_COMPUTER_MAC: return 0;
	default:                 return 1;
	}
}

static uint16_t uipc_boot_word(const sc8850_t *b, uint8_t step)
{
	if (!b->one_chip)
		return (uint16_t)((UIPC_BOOT[step][0] << 8) | UIPC_BOOT[step][1]);
	const uint8_t status = UIPC_BOOT_SC8820[step][0];
	const uint8_t data = status == SC8820_UIPC_TAG_SWITCH ? sc8820_switch_byte(b->computer_switch) : UIPC_BOOT_SC8820[step][1];
	return (uint16_t)((status << 8) | data);
}

/* the controller's two events reach the SC-8850 as gate array sources and the SC-8820 as edges on IRQ2
 * (a byte waits in UIPC(1)) and IRQ1 (UIPC(0) took the byte) */
static void uipc_rx_event(sc8850_t *b)
{
	if (!b->one_chip)
	{
		ga_raise(b, GA_SOURCE_UIPC_RX);
		return;
	}
	sh2_set_irq(&b->cpu, SH2_IRQ2, true);
	sh2_set_irq(&b->cpu, SH2_IRQ2, false);
}

static void uipc_tx_event(sc8850_t *b)
{
	if (!b->one_chip)
	{
		ga_raise(b, GA_SOURCE_UIPC_TX);
		return;
	}
	sh2_set_irq(&b->cpu, SH2_IRQ1, true);
	sh2_set_irq(&b->cpu, SH2_IRQ1, false);
}

static const uint8_t UIPC_CIN_LENGTH[16] = { 0, 0, 2, 3, 3, 1, 2, 3, 3, 3, 3, 3, 2, 2, 3, 1 };

static bool uipc_peek(const sc8850_t *b, uint16_t *value)
{
	const sc8850_uipc_t *u = &b->uipc;
	if (u->boot < uipc_boot_steps(b))
	{
		*value = uipc_boot_word(b, u->boot);
		return true;
	}
	if (!u->rx_count)
		return false;
	*value = u->rx[u->rx_head];
	return true;
}

static void uipc_push(sc8850_t *b, uint8_t status, uint8_t byte)
{
	sc8850_uipc_t *u = &b->uipc;
	if (u->rx_count >= SC8850_UIPC_RX)
		return;
	u->rx[(u->rx_head + u->rx_count) % SC8850_UIPC_RX] = (uint16_t)((status << 8) | byte);
	u->rx_count++;
	if (!b->one_chip || u->rx_count == 1)
		uipc_rx_event(b);
}

static void uipc_push_packet(sc8850_t *b, uint8_t header, uint8_t a, uint8_t c, uint8_t d)
{
	uipc_push(b, UIPC_TAG_MIDI | UIPC_PACKET_START, header);
	uipc_push(b, UIPC_TAG_MIDI, a);
	uipc_push(b, UIPC_TAG_MIDI, c);
	uipc_push(b, UIPC_TAG_MIDI, d);
}

static void uipc_send(sc8850_t *b, int port, uint8_t cin, const uint8_t *msg)
{
	uipc_push_packet(b, (uint8_t)((port << 4) | cin), msg[0], msg[1], msg[2]);
}

static void uipc_take_midi(sc8850_t *b, int port, uint8_t byte)
{
	sc8850_usb_in_t *in = &b->uipc.in[port];
	if (byte >= 0xf8)
	{
		const uint8_t one[3] = { byte, 0, 0 };
		uipc_send(b, port, UIPC_CIN_SINGLE, one);
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
		uipc_send(b, port, cin, in->msg);
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
			uipc_send(b, port, UIPC_CIN_SYSEX_END, in->msg);
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
			uipc_send(b, port, UIPC_CIN_SYSEX, in->msg);
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
		uipc_send(b, port, in->cin, in->msg);
		in->count = 0;
	}
}

static void uipc_deliver(sc8850_t *b)
{
	const sc8850_uipc_t *u = &b->uipc;
	const int port = u->tx[0] >> 4, length = UIPC_CIN_LENGTH[u->tx[0] & 0x0f];
	if (!length || port >= SC8850_MIDI_PORTS || !b->midi_out)
		return;
	b->midi_out(port, u->tx + 1, (size_t)length, b->midi_out_user);
}

static uint8_t uipc_read(sc8850_t *b, int channel, uint32_t offset)
{
	sc8850_uipc_t *u = &b->uipc;
	uint16_t head;
	if (channel == 0)
		return 0x00;
	if (!uipc_peek(b, &head))
		return 0x00;
	if (offset)
		return (uint8_t)((head >> 8) | UIPC_STATUS_RX);
	if (u->boot < uipc_boot_steps(b))
	{
		if (++u->boot == uipc_boot_steps(b))
		{
			u->running = true;
			u->host = b->computer_switch == SCEMU_COMPUTER_MAC;
			u->announce = UIPC_ANNOUNCE_FRAMES;
		}
	}
	else
	{
		u->rx_head = (uint16_t)((u->rx_head + 1) % SC8850_UIPC_RX);
		u->rx_count--;
		if (b->one_chip && u->rx_count)
			uipc_rx_event(b);
	}
	return (uint8_t)head;
}

static void uipc_write(sc8850_t *b, int channel, uint32_t offset, uint8_t data)
{
	sc8850_uipc_t *u = &b->uipc;
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
			uipc_deliver(b);
			u->tx_count = 0;
		}
	}
	if (u->online)
		uipc_tx_event(b);
}

static void uipc_reset(sc8850_t *b)
{
	memset(&b->uipc, 0, sizeof(b->uipc));
}

static void uipc_frame(sc8850_t *b)
{
	sc8850_uipc_t *u = &b->uipc;
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
		uipc_push(b, 0x00, UIPC_HOST_ONLINE);
		return;
	}
	if (u->rx_count && !b->one_chip)
		ga_raise(b, GA_SOURCE_UIPC_RX);
	if (++u->poll >= UIPC_POLL_FRAMES)
	{
		u->poll = 0;
		uipc_tx_event(b);
	}
}

/* ---------------------------------------------------------------- the chips' wiring */

static void master_irq(void *user, bool state)
{
	sc8850_t *b = user;
	sh2_set_irq(&b->cpu, b->one_chip ? SH2_IRQ0 : SH2_IRQ1, state);
}

static void slave_irq(void *user, bool state)
{
	sc8850_t *b = user;
	sh2_set_irq(&b->cpu, SH2_IRQ0, state);
}

static int32_t master_link_in(void *user, int strobe)
{
	sc8850_t *b = user;
	return xp_port_a_out(&b->slave, strobe);
}

static int32_t slave_link_in(void *user, int strobe)
{
	sc8850_t *b = user;
	return xp_port_a_out(&b->master, strobe);
}

static void slave_port_out(void *user, int port, int32_t word)
{
	sc8850_t *b = user;
	if ((port >> 1) != XP_PORT_C)
		return;
	lsp_serial_write(&b->lsp, port & 1, word);
	if (port & 1)
		lsp_run_sample(&b->lsp);
}

static int32_t slave_port_b_in(void *user, int group)
{
	sc8850_t *b = user;
	return -(lsp_serial_read(&b->lsp, group & 1) >> 8);
}

/* the SC-8820's one chip puts the insertion effect on the lines the SC-8850 keeps for the link:
 * the send leaves on port B and the return comes back on port A */
static void sc8820_port_out(void *user, int port, int32_t word)
{
	sc8850_t *b = user;
	if ((port >> 1) != XP_PORT_B)
		return;
	lsp_serial_write(&b->lsp, port & 1, word);
	if (port & 1)
		lsp_run_sample(&b->lsp);
}

static int32_t sc8820_port_a_in(void *user, int strobe)
{
	sc8850_t *b = user;
	return -(lsp_serial_read(&b->lsp, strobe & 1) >> 8);
}

/* ---------------------------------------------------------------- the SH-2's bus */

enum { DEV_NONE, DEV_DRAM, DEV_GLCD, DEV_UIPC0, DEV_UIPC1, DEV_LSP, DEV_GA, DEV_SLAVE, DEV_MASTER, DEV_PROGRAM_FLASH, DEV_TONE_FLASH };

static int decode_sc8820(uint32_t address, uint32_t *offset)
{
	switch ((address >> 22) & 3)
	{
	case 3:
		*offset = address & (SC8820_PROGRAM_ROM_SIZE - 1);
		return DEV_PROGRAM_FLASH;
	case 1:
		switch ((address >> 18) & 0xf)
		{
		case 0x4: *offset = address & 0xf; return DEV_LSP;
		case 0x5: *offset = address & 1; return DEV_UIPC0;
		case 0x6: *offset = address & 1; return DEV_UIPC1;
		default: return DEV_NONE;
		}
	case 2:
		*offset = address & 0x3fff;
		return (address & 0xffc000) == 0x00900000 ? DEV_MASTER : DEV_NONE;
	default:
		return DEV_NONE;
	}
}

static int decode(const sc8850_t *b, uint32_t address, uint32_t *offset)
{
	if (address & 0x01000000)
	{
		*offset = address & (SC8850_DRAM_SIZE - 1);
		return DEV_DRAM;
	}
	if (b->one_chip)
		return decode_sc8820(address, offset);
	switch ((address >> 22) & 3)
	{
	case 0:
		if ((address & 0xffffff) < SC8850_BOOT_ROM_SIZE)
			return DEV_NONE;
		*offset = address & (SC8850_PROGRAM_ROM_SIZE - 1);
		return DEV_PROGRAM_FLASH;
	case 3:
		*offset = address & (SC8850_TONE_ROM_SIZE - 1);
		return DEV_TONE_FLASH;
	case 1:
		switch ((address >> 18) & 0xf)
		{
		case 0x4: *offset = address & 1; return DEV_GLCD;
		case 0x5: *offset = address & 1; return DEV_UIPC0;
		case 0x6: *offset = address & 1; return DEV_UIPC1;
		case 0x7: *offset = address & 0xf; return DEV_LSP;
		case 0xb: *offset = address & 0xff; return DEV_GA;
		default: return DEV_NONE;
		}
	case 2:
		*offset = address & 0x3fff;
		if ((address & 0xffc000) == 0x00a00000)
			return DEV_SLAVE;
		if ((address & 0xffc000) == 0x00a80000)
			return DEV_MASTER;
		return DEV_NONE;
	default:
		return DEV_NONE;
	}
}

static uint8_t bus_read8(void *user, uint32_t address)
{
	sc8850_t *b = user;
	uint32_t offset;
	switch (decode(b, address, &offset))
	{
	case DEV_DRAM:  return b->dram[offset];
	case DEV_GLCD:  return glcd_read(&b->glcd, offset);
	case DEV_UIPC0: return uipc_read(b, 0, offset);
	case DEV_UIPC1: return uipc_read(b, 1, offset);
	case DEV_LSP:   return lsp_host_read(&b->lsp, offset);
	case DEV_GA:    return ga_read(b, offset);
	case DEV_SLAVE:
	{
		const uint16_t word = xp_read(&b->slave, offset >> 1);
		return (uint8_t)((address & 1) ? word : word >> 8);
	}
	case DEV_MASTER:
	{
		const uint16_t word = xp_read(&b->master, offset >> 1);
		return (uint8_t)((address & 1) ? word : word >> 8);
	}
	case DEV_PROGRAM_FLASH:
	{
		const uint16_t word = flash_read(&b->program_flash, offset);
		return (uint8_t)((address & 1) ? word : word >> 8);
	}
	case DEV_TONE_FLASH:
	{
		const uint16_t word = flash_read(&b->tone_flash, offset);
		return (uint8_t)((address & 1) ? word : word >> 8);
	}
	default:        return 0;
	}
}

static uint16_t bus_read16(void *user, uint32_t address)
{
	sc8850_t *b = user;
	uint32_t offset;
	switch (decode(b, address, &offset))
	{
	case DEV_DRAM:   return (uint16_t)((b->dram[offset] << 8) | b->dram[offset + 1]);
	case DEV_SLAVE:  return xp_read(&b->slave, offset >> 1);
	case DEV_MASTER: return xp_read(&b->master, offset >> 1);
	case DEV_PROGRAM_FLASH: return flash_read(&b->program_flash, offset);
	case DEV_TONE_FLASH:    return flash_read(&b->tone_flash, offset);
	case DEV_NONE:   return 0;
	default:         return (uint16_t)((bus_read8(user, address) << 8) | bus_read8(user, address + 1));
	}
}

static void flash_bus_write(sc8850_t *b, flash_t *f, uint32_t offset, uint16_t data)
{
	flash_write(f, offset, data);
	for (int n = 0; n < b->cpu.region_count; n++)
	{
		sh2_region_t *r = &b->cpu.regions[n];
		if (r->data == f->data && r->bypass != !flash_in_array(f))
		{
			r->bypass = !flash_in_array(f);
			sh2_jit_remap(&b->cpu);
		}
	}
	if (f->erased)
	{
		f->erased = false;
		b->code_flush_pending = true;
	}
}

static void bus_write8(void *user, uint32_t address, uint8_t data)
{
	sc8850_t *b = user;
	uint32_t offset;
	switch (decode(b, address, &offset))
	{
	case DEV_DRAM:   b->dram[offset] = data; break;
	case DEV_GLCD:   glcd_write(&b->glcd, offset, data); break;
	case DEV_LSP:    lsp_host_write(&b->lsp, offset, data); b->tg_written = b->frame; break;
	case DEV_UIPC0:  uipc_write(b, 0, offset, data); break;
	case DEV_UIPC1:  uipc_write(b, 1, offset, data); break;
	case DEV_GA:     ga_write(b, offset, data); break;
	case DEV_SLAVE:  xp_write(&b->slave, offset >> 1, (uint16_t)(data * 0x101), (address & 1) ? 0x00ff : 0xff00); b->tg_written = b->frame; break;
	case DEV_MASTER: xp_write(&b->master, offset >> 1, (uint16_t)(data * 0x101), (address & 1) ? 0x00ff : 0xff00); b->tg_written = b->frame; break;
	case DEV_PROGRAM_FLASH: flash_bus_write(b, &b->program_flash, offset, data); break;
	case DEV_TONE_FLASH:    flash_bus_write(b, &b->tone_flash, offset, data); break;
	default: break;
	}
}

static void bus_write16(void *user, uint32_t address, uint16_t data)
{
	sc8850_t *b = user;
	uint32_t offset;
	switch (decode(b, address, &offset))
	{
	case DEV_DRAM:
		b->dram[offset] = (uint8_t)(data >> 8);
		b->dram[offset + 1] = (uint8_t)data;
		break;
	case DEV_SLAVE:
		xp_write(&b->slave, offset >> 1, data, 0xffff);
		b->tg_written = b->frame;
		break;
	case DEV_MASTER:
		xp_write(&b->master, offset >> 1, data, 0xffff);
		b->tg_written = b->frame;
		break;
	case DEV_PROGRAM_FLASH:
		flash_bus_write(b, &b->program_flash, offset, data);
		break;
	case DEV_TONE_FLASH:
		flash_bus_write(b, &b->tone_flash, offset, data);
		break;
	case DEV_NONE:
		break;
	default:
		bus_write8(user, address, (uint8_t)(data >> 8));
		bus_write8(user, address + 1, (uint8_t)data);
		break;
	}
}

static uint16_t bus_read_port(void *user, int port)
{
	sc8850_t *b = user;
	if (b->one_chip)
	{
		if (port == SH2_PORT_E)
			return (uint16_t)(SC8820_PE_IDLE_PINS & ~(b->panel.map_key ? SC8820_PE_MAP_KEY : 0)
			                  & ~(b->panel.preview_key ? SC8820_PE_PREVIEW_KEY : 0));
		return 0;
	}
	if (port == SH2_PORT_A)
		return b->ga.pending ? 0x0000 : PA_EVENT;
	return 0;
}

static void sc8820_panel_update(sc8820_panel_t *p)
{
	static const uint8_t COLUMN_LED[SC8820_PANEL_ROWS][4] =
	{
		{ SCEMU_LED_PART_A2, SCEMU_LED_PART_A1, SCEMU_LED_PART_A4, SCEMU_LED_PART_A3 },
		{ SCEMU_LED_PART_B2, SCEMU_LED_PART_B1, SCEMU_LED_PART_B4, SCEMU_LED_PART_B3 },
		{ SCEMU_LED_MAP, 0, SCEMU_LED_USB, 0 },
	};
	uint32_t leds = 0;
	for (int row = 0; row < SC8820_PANEL_ROWS; row++)
		for (int column = 0; column < 4; column++)
			if (((p->column[row] >> column) & 1) && COLUMN_LED[row][column])
				leds |= 1u << COLUMN_LED[row][column];
	if (!(p->pe & SC8820_PE_POWER_LAMP))
		leds |= 1u << SCEMU_LED_POWER;
	p->leds = leds;
}

static void sc8820_panel_sample(sc8820_panel_t *p)
{
	const bool row[SC8820_PANEL_ROWS] =
	{
		!(p->pe & SC8820_PE_ROW0), !(p->pe & SC8820_PE_ROW1), !(p->pa & SC8820_PA_ROW2)
	};
	for (int n = 0; n < SC8820_PANEL_ROWS; n++)
		if (row[n])
			p->column[n] = (uint8_t)(p->pe & SC8820_PE_COLUMNS);
	sc8820_panel_update(p);
}

static void sc8820_panel_frame(sc8820_panel_t *p)
{
	if (p->written)
	{
		p->written = false;
		return;
	}
	sc8820_panel_sample(p);
}

/* the keys are the host's, held through a reset as through the power switch */
static void sc8820_panel_reset(sc8820_panel_t *p)
{
	const bool map_key = p->map_key, preview_key = p->preview_key;
	memset(p, 0, sizeof(*p));
	p->pe = 0xffff;
	p->pa = 0xffff;
	p->map_key = map_key;
	p->preview_key = preview_key;
}

static void bus_write_port(void *user, int port, uint16_t data, uint16_t ior)
{
	sc8850_t *b = user;
	if (b->one_chip)
	{
		if (port == SH2_PORT_E)
		{
			if (ior & SC8820_PE_MUTE_RELEASE)
				b->mute = !(data & SC8820_PE_MUTE_RELEASE);
			b->panel.pe = (uint16_t)((data & ior) | ~ior);
			b->panel.written = true;
			sc8820_panel_update(&b->panel);
		}
		else if (port == SH2_PORT_A)
		{
			b->panel.pa = (uint16_t)((data & ior) | ~ior);
			b->panel.written = true;
		}
		return;
	}
	if (port == SH2_PORT_E && (ior & PE_MUTE_RELEASE))
		b->mute = !(data & PE_MUTE_RELEASE);
}

static uint16_t bus_read_adc(void *user, int channel)
{
	sc8850_t *b = user;
	if (channel != 0)
		return 0;
	switch (b->computer_switch)
	{
	case SCEMU_COMPUTER_PC1: return 0x180;
	case SCEMU_COMPUTER_PC2: return 0x280;
	case SCEMU_COMPUTER_MAC: return 0x3ff;
	default:                 return 0x000;
	}
}

static void bus_sci_tx(void *user, int channel, uint8_t byte)
{
	sc8850_t *b = user;
	if (channel == SH2_SCI0 && b->midi_out)
		b->midi_out(SCEMU_MIDI_IN_A, &byte, 1, b->midi_out_user);
}

/* ---------------------------------------------------------------- lifetime */

static uint64_t hash_add(uint64_t h, const void *data, size_t size)
{
	const uint8_t *p = data;
	for (size_t n = 0; n + 8 <= size; n += 8)
	{
		uint64_t v;
		memcpy(&v, p + n, 8);
		h = (h ^ v) * 0x100000001b3ull;
	}
	for (size_t n = size & ~(size_t)7; n < size; n++)
		h = (h ^ p[n]) * 0x100000001b3ull;
	return h ^ (uint64_t)size;
}

static uint8_t *copy_rom(const void *data, size_t size)
{
	uint8_t *rom = malloc(size);
	if (rom)
		memcpy(rom, data, size);
	return rom;
}

bool sc8850_init(sc8850_t *b, scemu_model_t model, const scemu_roms_t *roms, const scemu_config_t *config)
{
	memset(b, 0, sizeof(*b));
	b->model = model;
	b->one_chip = model == SCEMU_MODEL_SC8820;
	midi_queue_init(&b->midi, SC8850_MIDI_PORTS, SC8850_SAMPLE_RATE);
	jit_alloc_init(&b->jit, config);

	uint64_t id = ROM_ID_SEED;
	id = hash_add(id, roms->boot_rom, roms->boot_rom_size);
	id = hash_add(id, roms->program_rom, roms->program_rom_size);
	if (roms->tone_rom)
		id = hash_add(id, roms->tone_rom, roms->tone_rom_size);
	for (int n = 0; n < roms->wave_rom_count; n++)
		id = hash_add(id, roms->wave_rom[n], roms->wave_rom_size[n]);
	b->rom_id = id;

	const uint32_t program_size = b->one_chip ? SC8820_PROGRAM_ROM_SIZE : SC8850_PROGRAM_ROM_SIZE;
	b->boot_rom = copy_rom(roms->boot_rom, SC8850_BOOT_ROM_SIZE);
	b->program_rom = copy_rom(roms->program_rom, program_size);
	if (!b->boot_rom || !b->program_rom)
		return false;
	if (b->one_chip)
		flash_init(&b->program_flash, b->program_rom, program_size, SC8850_TONE_FLASH_DEVICE);
	else
	{
		b->tone_rom = copy_rom(roms->tone_rom, SC8850_TONE_ROM_SIZE);
		if (!b->tone_rom)
			return false;
		flash_init(&b->program_flash, b->program_rom, program_size, SC8850_PROGRAM_FLASH_DEVICE);
		flash_init(&b->tone_flash, b->tone_rom, SC8850_TONE_ROM_SIZE, SC8850_TONE_FLASH_DEVICE);
	}

	b->wave_rom_size = wave_rom_size(model);
	b->wave_rom = malloc(b->wave_rom_size);
	if (!b->wave_rom || !wave_rom_build(model, roms, b->wave_rom, b->wave_rom_size))
		return false;

	sh2_bus_t bus =
	{
		bus_read8, bus_read16, NULL, bus_write8, bus_write16, NULL,
		bus_read_port, bus_write_port, bus_read_adc, bus_sci_tx, b
	};
	sh2_init(&b->cpu, &bus);
	sh2_map(&b->cpu, 0x00000000, SC8850_BOOT_ROM_SIZE, b->boot_rom, false);
	if (b->one_chip)
		sh2_map(&b->cpu, 0x00d00000, SC8820_PROGRAM_ROM_SIZE, b->program_rom, false);
	else
	{
		sh2_map(&b->cpu, 0x00200000, SC8850_PROGRAM_ROM_SIZE, b->program_rom, false);
		sh2_map(&b->cpu, 0x00d00000, SC8850_TONE_ROM_SIZE, b->tone_rom, false);
	}
	sh2_map(&b->cpu, 0x01000000, SC8850_DRAM_SIZE, b->dram, true);
	sh2_jit_attach(&b->cpu, &b->jit);
	if (config->sh2_interpreter)
		b->cpu.jit_enabled = false;

	const uint32_t chip_size = wave_rom_chip_size(model);
	if (b->one_chip)
	{
		xp_link_t link = { master_irq, sc8820_port_out, sc8820_port_a_in, NULL, b };
		if (!xp_init(&b->master, &link, &b->jit, b->wave_rom, b->wave_rom_size, chip_size))
			return false;
	}
	else
	{
		xp_link_t master_link = { master_irq, NULL, master_link_in, NULL, b };
		xp_link_t slave_link = { slave_irq, slave_port_out, slave_link_in, slave_port_b_in, b };
		if (!xp_init(&b->master, &master_link, &b->jit, b->wave_rom, b->wave_rom_size, chip_size))
			return false;
		if (!xp_init(&b->slave, &slave_link, &b->jit, b->wave_rom, b->wave_rom_size, chip_size))
			return false;
	}
	if (!lsp_init(&b->lsp, &b->jit))
		return false;
	if (!b->one_chip)
		glcd_init(&b->glcd);
	return true;
}

void sc8850_release(sc8850_t *b)
{
	sh2_jit_detach(&b->cpu);
	lsp_release(&b->lsp);
	if (!b->one_chip)
		xp_release(&b->slave);
	xp_release(&b->master);
	free(b->wave_rom);
	free(b->tone_rom);
	free(b->program_rom);
	free(b->boot_rom);
	b->wave_rom = NULL;
	b->tone_rom = NULL;
	b->program_rom = NULL;
	b->boot_rom = NULL;
}

void sc8850_reset(sc8850_t *b)
{
	b->cpu_overshoot = 0;
	b->mute = true;
	b->frame = 0;
	b->tg_written = 0;
	b->code_flush_pending = false;
	uipc_reset(b);
	midi_queue_reset(&b->midi);
	xp_reset(&b->master);
	lsp_reset(&b->lsp);
	flash_reset(&b->program_flash);
	if (b->one_chip)
		sc8820_panel_reset(&b->panel);
	else
	{
		xp_reset(&b->slave);
		glcd_reset(&b->glcd);
		flash_reset(&b->tone_flash);
		ga_reset(b);
	}
	for (int n = 0; n < b->cpu.region_count; n++)
		b->cpu.regions[n].bypass = false;
	sh2_jit_remap(&b->cpu);
	sh2_reset(&b->cpu);
}

/* ---------------------------------------------------------------- the frame */

static bool midi_take(void *user, int port, uint8_t byte)
{
	sc8850_t *b = user;
	if (b->uipc.host)
	{
		if (!b->uipc.online)
			return true;
		if (b->uipc.rx_count + 4 > SC8850_UIPC_RX)
			return false;
		uipc_take_midi(b, port, byte);
		return true;
	}
	if (port >= (b->one_chip ? 1 : 2))
		return true;
	if (b->cpu.sci[port].rx_pending)
		return false;
	sh2_sci_rx(&b->cpu, port, byte);
	return true;
}

void sc8850_deliver_midi(sc8850_t *b)
{
	midi_queue_deliver(&b->midi, (uint32_t)b->frame, midi_take, b);
}

void sc8850_run_frame(sc8850_t *b)
{
	sc8850_deliver_midi(b);

	const int cycles = (int)(SC8850_SH2_CYCLES_PER_FRAME - b->cpu_overshoot);
	const int ran = sh2_run(&b->cpu, cycles);
	if (b->code_flush_pending)
	{
		b->code_flush_pending = false;
		sh2_jit_flush(&b->cpu);
	}
	b->cpu_overshoot = ran > cycles ? (uint32_t)(ran - cycles) : 0;

	if (b->one_chip)
	{
		sc8820_panel_frame(&b->panel);
		uipc_frame(b);
		xp_run_frame(&b->master);
	}
	else
	{
		ga_frame(b);
		uipc_frame(b);
		xp_run_frame(&b->master);
		xp_run_frame(&b->slave);
		glcd_frame(&b->glcd);
	}
	b->frame++;
}

bool sc8850_idle(const sc8850_t *b)
{
	return !b->mute && b->frame - b->tg_written >= SC8850_IDLE_FRAMES;
}

void sc8850_queue_midi(sc8850_t *b, int port, uint8_t byte, uint32_t frame_offset)
{
	midi_queue_push(&b->midi, port, byte, (uint32_t)b->frame + frame_offset);
}

/* ---------------------------------------------------------------- the panel */

typedef struct key_position
{
	scemu_button_t button;
	uint8_t row, column;
} key_position_t;

static const key_position_t KEYS[] =
{
	{ SCEMU_BUTTON_MAP, 0, 0 }, { SCEMU_BUTTON_F4, 0, 1 }, { SCEMU_BUTTON_F3, 0, 2 },
	{ SCEMU_BUTTON_F2, 0, 3 }, { SCEMU_BUTTON_F1, 0, 4 }, { SCEMU_BUTTON_VALUE, 0, 7 },
	{ SCEMU_BUTTON_EDIT, 1, 0 }, { SCEMU_BUTTON_DRUM, 1, 1 }, { SCEMU_BUTTON_EFFECTS, 1, 2 },
	{ SCEMU_BUTTON_SHIFT, 1, 3 }, { SCEMU_BUTTON_PREVIEW, 1, 7 },
	{ SCEMU_BUTTON_PART_LEFT, 2, 0 }, { SCEMU_BUTTON_DOWN, 2, 1 }, { SCEMU_BUTTON_EXIT, 2, 2 },
	{ SCEMU_BUTTON_SOLO, 2, 3 },
	{ SCEMU_BUTTON_PART_RIGHT, 3, 0 }, { SCEMU_BUTTON_UP, 3, 1 }, { SCEMU_BUTTON_ENTER, 3, 2 },
	{ SCEMU_BUTTON_MUTE, 3, 3 }, { SCEMU_BUTTON_DEC, 3, 4 }, { SCEMU_BUTTON_INC, 3, 5 },
};

void sc8850_button(sc8850_t *b, scemu_button_t button, bool down)
{
	if (b->one_chip)
	{
		if (button == SCEMU_BUTTON_MAP)
			b->panel.map_key = down;
		else if (button == SCEMU_BUTTON_PREVIEW)
			b->panel.preview_key = down;
		return;
	}
	for (size_t n = 0; n < sizeof(KEYS) / sizeof(KEYS[0]); n++)
	{
		if (KEYS[n].button != button)
			continue;
		const uint8_t mask = (uint8_t)(1u << KEYS[n].column);
		if (down)
			b->ga.keys[KEYS[n].row] |= mask;
		else
			b->ga.keys[KEYS[n].row] &= (uint8_t)~mask;
		return;
	}
}

void sc8850_dial(sc8850_t *b, int steps)
{
	b->ga.encoder += steps;
}

/* ---------------------------------------------------------------- the board as the API sees it */

static const char *ops_validate_roms(scemu_model_t model, const scemu_roms_t *roms) { return sc8850_validate_roms(model, roms); }
static bool ops_init(void *b, scemu_model_t model, const scemu_roms_t *roms, const scemu_config_t *config) { return sc8850_init(b, model, roms, config); }
static void ops_release(void *b) { sc8850_release(b); }
static void ops_reset(void *b) { sc8850_reset(b); }
static void ops_run_frame(void *b) { sc8850_run_frame(b); }
static bool ops_idle(const void *b) { return sc8850_idle(b); }
static uint64_t ops_frame(const void *b) { return ((const sc8850_t *)b)->frame; }
static uint64_t ops_rom_id(const void *b) { return ((const sc8850_t *)b)->rom_id; }
static uint32_t ops_sample_rate(const void *b) { (void)b; return SC8850_SAMPLE_RATE; }
static int ops_output_count(const void *b) { return ((const sc8850_t *)b)->one_chip ? 1 : 2; }

/* the master's two DACs: SDOC carries OUTPUT 1 as words 2 and 3, SDOD OUTPUT 2 as 4 and 5; the
 * SC-8820's program puts its one DAC on words 4 and 5 */
static int32_t ops_output(const void *board, int pair, int channel)
{
	const sc8850_t *b = board;
	if (b->mute)
		return 0;
	if (b->one_chip)
		return xp_output(&b->master, 4 + channel);
	return xp_output(&b->master, 2 + pair * 2 + channel);
}

static midi_queue_t *ops_midi(void *b) { return &((sc8850_t *)b)->midi; }
static void ops_set_midi_out(void *board, scemu_midi_out_fn fn, void *user)
{
	sc8850_t *b = board;
	b->midi_out = fn;
	b->midi_out_user = user;
}
static void ops_button(void *b, scemu_button_t button, bool down) { sc8850_button(b, button, down); }
static void ops_set_computer_switch(void *b, scemu_computer_switch_t sw) { ((sc8850_t *)b)->computer_switch = sw; }
static uint32_t ops_leds(const void *board)
{
	const sc8850_t *b = board;
	return b->one_chip ? b->panel.leds : b->ga.leds;
}
static scemu_glcd_t *ops_glcd(void *board)
{
	sc8850_t *b = board;
	return b->one_chip ? NULL : &b->glcd.out;
}
static void ops_dial(void *b, int steps) { sc8850_dial(b, steps); }

/* the SC-8820's flash is never written by its firmware: no settings memory */
static size_t ops_nvram_size(const void *b) { return ((const sc8850_t *)b)->one_chip ? 0 : SC8850_NVRAM_SIZE; }

static size_t ops_nvram_get(const void *board, void *buffer, size_t size)
{
	const sc8850_t *b = board;
	if (b->one_chip || size < SC8850_NVRAM_SIZE)
		return 0;
	memcpy(buffer, b->program_rom + SC8850_NVRAM_BASE, SC8850_NVRAM_SIZE);
	return SC8850_NVRAM_SIZE;
}

static bool ops_nvram_set(void *board, const void *buffer, size_t size)
{
	sc8850_t *b = board;
	if (b->one_chip || size != SC8850_NVRAM_SIZE)
		return false;
	memcpy(b->program_rom + SC8850_NVRAM_BASE, buffer, SC8850_NVRAM_SIZE);
	sh2_jit_flush(&b->cpu);
	return true;
}

static size_t ops_state_size(const void *b) { return sc8850_state_size(b); }
static size_t ops_state_save(const void *b, void *buffer, size_t size) { return sc8850_state_save(b, buffer, size); }
static bool ops_state_load(void *b, const void *buffer, size_t size) { return sc8850_state_load(b, buffer, size); }

const board_ops_t sc8850_board_ops =
{
	ops_validate_roms, ops_init, ops_release, ops_reset, ops_run_frame, ops_idle, ops_frame, ops_rom_id, ops_sample_rate,
	ops_output_count, ops_output,
	ops_midi, ops_set_midi_out,
	ops_button, ops_set_computer_switch, ops_leds, NULL, ops_glcd, ops_dial,
	ops_nvram_size, ops_nvram_get, ops_nvram_set,
	ops_state_size, ops_state_save, ops_state_load,
};
