#ifndef SCEMU_LCD_H
#define SCEMU_LCD_H

#include <stdint.h>
#include <stdbool.h>
#include "scemu.h"

/* HD44780-compatible controller behind the RCM2024T glass. */

typedef struct lcd
{
	scemu_lcd_t out;
	uint8_t address;
	bool cgram_mode;
	bool increment;
	bool shift;
	bool two_line;
	uint8_t display_shift;
} lcd_t;

void lcd_init(lcd_t *lcd);
void lcd_reset(lcd_t *lcd);
void lcd_command(lcd_t *lcd, uint8_t data);
void lcd_data(lcd_t *lcd, uint8_t data);

#endif
