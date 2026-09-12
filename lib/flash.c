#include <string.h>
#include "flash.h"
#include "state.h"

#define STATUS_READY 0x80
#define STATUS_PROGRAM_ERROR 0x10

void flash_init(flash_t *f, uint8_t *data, uint32_t size, uint16_t device)
{
	memset(f, 0, sizeof(*f));
	f->data = data;
	f->size = size;
	f->device = device;
	f->status = STATUS_READY;
}

void flash_reset(flash_t *f)
{
	f->mode = FLASH_READ_ARRAY;
	f->pending = 0;
	f->status = STATUS_READY;
	f->erased = false;
}

uint16_t flash_read(const flash_t *f, uint32_t offset)
{
	offset &= f->size - 1;
	offset &= ~1u;
	switch (f->mode)
	{
	case FLASH_READ_ID:
		if (offset == 0)
			return FLASH_MANUFACTURER;
		if (offset == 2)
			return f->device;
		return 0;
	case FLASH_READ_STATUS:
		return f->status;
	default:
		return (uint16_t)((f->data[offset] << 8) | f->data[offset + 1]);
	}
}

void flash_write(flash_t *f, uint32_t offset, uint16_t data)
{
	offset &= f->size - 1;
	offset &= ~1u;
	const uint8_t command = (uint8_t)data;
	const uint8_t pending = f->pending;
	f->pending = 0;

	switch (pending)
	{
	case 0x40:
		f->data[offset] &= (uint8_t)(data >> 8);
		f->data[offset + 1] &= (uint8_t)data;
		if (f->data[offset] != (uint8_t)(data >> 8) || f->data[offset + 1] != (uint8_t)data)
			f->status |= STATUS_PROGRAM_ERROR;
		f->mode = FLASH_READ_STATUS;
		return;
	case 0x20:
		if (command == 0xd0)
		{
			memset(f->data + (offset & ~(FLASH_BLOCK_SIZE - 1)), 0xff, FLASH_BLOCK_SIZE);
			f->erased = true;
			f->mode = FLASH_READ_STATUS;
			return;
		}
		break;
	case 0x60:
		if (command == 0xd0 || command == 0x01)
		{
			f->mode = FLASH_READ_STATUS;
			return;
		}
		break;
	default:
		break;
	}

	switch (command)
	{
	case 0xff: f->mode = FLASH_READ_ARRAY; break;
	case 0x90: f->mode = FLASH_READ_ID; break;
	case 0x70: f->mode = FLASH_READ_STATUS; break;
	case 0x50: f->status = STATUS_READY; f->mode = FLASH_READ_STATUS; break;
	case 0x40: case 0x10: f->pending = 0x40; break;
	case 0x20: f->pending = 0x20; break;
	case 0x60: f->pending = 0x60; break;
	case 0xb0: case 0xd0: f->mode = FLASH_READ_STATUS; break;
	default: break;
	}
}

/* ---------------------------------------------------------------- the state */

static bool state_restored(void *user)
{
	flash_t *f = user;
	f->erased = false;
	return true;
}

void flash_state(flash_t *f, state_registry_t *reg)
{
	state_var(reg, f->mode);
	state_var(reg, f->pending);
	state_var(reg, f->status);
	state_after_load(reg, state_restored, f);
}
