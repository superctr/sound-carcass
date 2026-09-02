#include <string.h>
#include "lcd.h"

void lcd_init(lcd_t *lcd)
{
	memset(lcd, 0, sizeof(*lcd));
}

void lcd_reset(lcd_t *lcd)
{
	memset(&lcd->out, 0, sizeof(lcd->out));
	memset(lcd->out.ddram, ' ', sizeof(lcd->out.ddram));
	lcd->address = 0;
	lcd->cgram_mode = false;
	lcd->increment = true;
	lcd->shift = false;
	lcd->two_line = false;
	lcd->display_shift = 0;
	lcd->out.changed = true;
}

void lcd_command(lcd_t *lcd, uint8_t data)
{
	if (data & 0x80)
	{
		lcd->cgram_mode = false;
		lcd->address = data & 0x7f;
	}
	else if (data & 0x40)
	{
		lcd->cgram_mode = true;
		lcd->address = data & 0x3f;
	}
	else if (data & 0x20)
	{
		lcd->two_line = (data & 0x08) != 0;
	}
	else if (data & 0x08)
	{
		lcd->out.display_on = (data & 0x04) != 0;
		lcd->out.changed = true;
	}
	else if (data & 0x04)
	{
		lcd->increment = (data & 0x02) != 0;
		lcd->shift = (data & 0x01) != 0;
	}
	else if (data & 0x02)
	{
		lcd->address = 0;
		lcd->cgram_mode = false;
		lcd->display_shift = 0;
	}
	else if (data & 0x01)
	{
		memset(lcd->out.ddram, ' ', sizeof(lcd->out.ddram));
		lcd->address = 0;
		lcd->cgram_mode = false;
		lcd->increment = true;
		lcd->out.changed = true;
	}
}

void lcd_data(lcd_t *lcd, uint8_t data)
{
	if (lcd->cgram_mode)
	{
		lcd->out.cgram[lcd->address & 0x3f] = data;
		lcd->address = (lcd->address + (lcd->increment ? 1 : -1)) & 0x3f;
	}
	else
	{
		uint8_t a = lcd->address;
		if (lcd->two_line)
			a = (a & 0x40) ? 40 + (a & 0x3f) : (a & 0x3f);
		if (a < 80)
			lcd->out.ddram[a] = data;
		lcd->address = (lcd->address + (lcd->increment ? 1 : -1)) & 0x7f;
	}
	lcd->out.changed = true;
}
