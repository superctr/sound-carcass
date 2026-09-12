#ifndef SCEMU_FLASH_H
#define SCEMU_FLASH_H

#include <stdint.h>
#include <stdbool.h>

/* A word-wide Sharp LH28F-series flash: the Intel-style command set over
 * a byte array the board owns, 64 KB blocks. */

#define FLASH_BLOCK_SIZE 0x10000u
#define FLASH_MANUFACTURER 0x00b0

enum { FLASH_READ_ARRAY, FLASH_READ_ID, FLASH_READ_STATUS };

typedef struct flash
{
	uint8_t *data;
	uint32_t size;
	uint16_t device;
	uint8_t mode;
	uint8_t pending;
	uint8_t status;
	bool erased;
} flash_t;

void flash_init(flash_t *f, uint8_t *data, uint32_t size, uint16_t device);
void flash_reset(flash_t *f);
uint16_t flash_read(const flash_t *f, uint32_t offset);
void flash_write(flash_t *f, uint32_t offset, uint16_t data);

static inline bool flash_in_array(const flash_t *f) { return f->mode == FLASH_READ_ARRAY; }

struct state_registry;
void flash_state(flash_t *f, struct state_registry *reg);

#endif
