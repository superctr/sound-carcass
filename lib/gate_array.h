#ifndef SCEMU_GATE_ARRAY_H
#define SCEMU_GATE_ARRAY_H

#include <stdint.h>
#include <stdbool.h>

/* uPD65622: interrupt aggregator on IRQ0, LED driver, LCD command/data FIFO. */

struct lcd;

typedef struct gate_array
{
	uint8_t regs[0x100];
	uint8_t int_pending;
	uint8_t int_mask;
	uint16_t leds;
	uint8_t lcd_fifo[13];
	uint8_t lcd_fifo_count;
	bool lcd_command_pending;
	uint32_t lcd_busy_frames;
	struct lcd *lcd;
	void (*irq)(void *user, bool state);
	void *user;
} gate_array_t;

void gate_array_init(gate_array_t *ga, struct lcd *lcd, void (*irq)(void *user, bool state), void *user);
void gate_array_reset(gate_array_t *ga);
uint8_t gate_array_read(gate_array_t *ga, uint32_t offset);
void gate_array_write(gate_array_t *ga, uint32_t offset, uint8_t data);
void gate_array_frame(gate_array_t *ga);

#endif
