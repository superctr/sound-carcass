#include "i8251.h"
#include "state.h"

void i8251_reset(i8251_t *u)
{
	u->next = I8251_NEXT_MODE;
	u->mode = 0;
	u->command = 0;
	u->status = I8251_TXEMPTY | I8251_TXRDY;
	u->rx_data = 0;
}

uint8_t i8251_read(i8251_t *u, int cd)
{
	if (cd)
		return u->status;
	u->status &= (uint8_t)~I8251_RXRDY;
	return u->rx_data;
}

/* a mode with no baud rate factor is synchronous and takes one or two sync characters after it */
static void write_mode(i8251_t *u, uint8_t data)
{
	u->mode = data;
	if (data & 0x03)
		u->next = I8251_NEXT_COMMAND;
	else
		u->next = (data & 0x80) ? I8251_NEXT_SYNC2 : I8251_NEXT_SYNC1;
}

static void write_command(i8251_t *u, uint8_t data)
{
	u->command = data;
	if (data & 0x10)
		u->status &= (uint8_t)~(I8251_PE | I8251_OE | I8251_FE);
	if (data & 0x40)
		u->next = I8251_NEXT_MODE;
}

void i8251_write(i8251_t *u, int cd, uint8_t data)
{
	if (!cd)
		return;
	switch (u->next)
	{
	case I8251_NEXT_MODE:
		write_mode(u, data);
		break;
	case I8251_NEXT_SYNC1:
		u->next = I8251_NEXT_SYNC2;
		break;
	case I8251_NEXT_SYNC2:
		u->next = I8251_NEXT_COMMAND;
		break;
	case I8251_NEXT_COMMAND:
		write_command(u, data);
		break;
	}
}

void i8251_receive(i8251_t *u, uint8_t byte)
{
	if (!(u->command & 0x04))
		return;
	if (u->status & I8251_RXRDY)
		u->status |= I8251_OE;
	u->rx_data = byte;
	u->status |= I8251_RXRDY;
}

bool i8251_rxrdy(const i8251_t *u)
{
	return (u->status & I8251_RXRDY) != 0;
}

/* ---------------------------------------------------------------- the state */

static bool state_restored(void *user)
{
	const i8251_t *u = user;
	return u->next <= I8251_NEXT_COMMAND;
}

void i8251_state(i8251_t *u, state_registry_t *reg)
{
	state_var(reg, u->next);
	state_var(reg, u->mode);
	state_var(reg, u->command);
	state_var(reg, u->status);
	state_var(reg, u->rx_data);
	state_after_load(reg, state_restored, u);
}
