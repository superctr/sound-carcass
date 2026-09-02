#include <string.h>
#include "gate_array.h"
#include "lcd.h"

static void update_int(gate_array_t *ga)
{
	ga->irq(ga->user, (ga->int_pending & ~ga->int_mask) != 0);
}

void gate_array_init(gate_array_t *ga, struct lcd *lcd, void (*irq)(void *user, bool state), void *user)
{
	memset(ga, 0, sizeof(*ga));
	ga->lcd = lcd;
	ga->irq = irq;
	ga->user = user;
}

void gate_array_reset(gate_array_t *ga)
{
	memset(ga->regs, 0, sizeof(ga->regs));
	ga->int_pending = 0;
	ga->int_mask = 0;
	ga->leds = 0;
	ga->lcd_fifo_count = 0;
	ga->lcd_command_pending = false;
	ga->lcd_busy_frames = 0;
	update_int(ga);
}

uint8_t gate_array_read(gate_array_t *ga, uint32_t offset)
{
	offset &= 0xff;
	if (offset == 0x04)
	{
		const uint8_t active = ga->int_pending & ~ga->int_mask;
		for (int source = 0; source < 8; source++)
		{
			if ((active >> source) & 1)
			{
				ga->int_pending &= (uint8_t)~(1u << source);
				update_int(ga);
				return (uint8_t)(source + 1);
			}
		}
		return 0;
	}
	return ga->regs[offset];
}

static void update_leds(gate_array_t *ga)
{
	const uint8_t data = ga->regs[0x00];
	const uint8_t commons = ga->regs[0x01];
	ga->leds = (uint16_t)((commons & 1) ? 0 : data);
	if (commons & 2)
		ga->leds |= 0x100;
}

void gate_array_write(gate_array_t *ga, uint32_t offset, uint8_t data)
{
	offset &= 0xff;
	ga->regs[offset] = data;
	switch (offset)
	{
	case 0x00:
	case 0x01:
		update_leds(ga);
		break;
	case 0x05:
		ga->int_mask = data;
		update_int(ga);
		break;
	case 0x1e:
	{
		int bytes = ga->lcd_fifo_count;
		if (ga->lcd_command_pending)
		{
			lcd_command(ga->lcd, ga->regs[0x1f]);
			bytes++;
		}
		for (int i = 0; i < ga->lcd_fifo_count; i++)
			lcd_data(ga->lcd, ga->lcd_fifo[i]);
		ga->lcd_command_pending = false;
		ga->lcd_fifo_count = 0;
		ga->lcd_busy_frames = (uint32_t)((40 * (bytes + 1) * 32 + 999) / 1000);
		break;
	}
	case 0x1f:
		ga->lcd_command_pending = true;
		break;
	default:
		if (offset >= 0x20 && offset <= 0x2c && ga->lcd_fifo_count < 13)
			ga->lcd_fifo[ga->lcd_fifo_count++] = data;
		break;
	}
}

void gate_array_frame(gate_array_t *ga)
{
	if (ga->lcd_busy_frames && !--ga->lcd_busy_frames)
	{
		ga->int_pending |= 1;
		update_int(ga);
	}
}
