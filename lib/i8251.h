#ifndef SCEMU_I8251_H
#define SCEMU_I8251_H

#include <stdint.h>
#include <stdbool.h>

/* An 8251 USART (the SC-55's MB89251A, MIDI IN 2) from the receiving side: a whole byte
 * arrives at once, and the host sees the data and status registers, and RXRDY.  Its
 * transmitter is wired to nothing, so it only ever reports itself empty and ready. */

enum
{
	I8251_TXRDY = 0x01,
	I8251_RXRDY = 0x02,
	I8251_TXEMPTY = 0x04,
	I8251_PE = 0x08,
	I8251_OE = 0x10,
	I8251_FE = 0x20
};

typedef enum i8251_next
{
	I8251_NEXT_MODE,
	I8251_NEXT_SYNC1,
	I8251_NEXT_SYNC2,
	I8251_NEXT_COMMAND
} i8251_next_t;

typedef struct i8251
{
	i8251_next_t next;
	uint8_t mode;
	uint8_t command;
	uint8_t status;
	uint8_t rx_data;
} i8251_t;

void i8251_reset(i8251_t *u);
uint8_t i8251_read(i8251_t *u, int cd);
void i8251_write(i8251_t *u, int cd, uint8_t data);

/* a byte off the wire; lost while the receiver is disabled, and over one not yet read */
void i8251_receive(i8251_t *u, uint8_t byte);
bool i8251_rxrdy(const i8251_t *u);

#endif
