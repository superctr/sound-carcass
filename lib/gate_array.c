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
	ga->int_mask = 0xff;
	ga->leds = 0;
	ga->lcd_fifo_count = 0;
	ga->lcd_command_pending = false;
	ga->lcd_busy_frames = 0;
}

uint8_t gate_array_read(gate_array_t *ga, uint32_t offset)
{
	return ga->regs[offset & 0xff];
}

void gate_array_write(gate_array_t *ga, uint32_t offset, uint8_t data)
{
	ga->regs[offset & 0xff] = data;
}

void gate_array_set_source(gate_array_t *ga, int source, bool state)
{
	if (state)
		ga->int_pending |= (uint8_t)(1u << source);
	else
		ga->int_pending &= (uint8_t)~(1u << source);
	update_int(ga);
}

void gate_array_frame(gate_array_t *ga)
{
	if (ga->lcd_busy_frames && !--ga->lcd_busy_frames)
		gate_array_set_source(ga, 0, true);
}
