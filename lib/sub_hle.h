#ifndef SCEMU_SUB_HLE_H
#define SCEMU_SUB_HLE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* M38881 sub-CPU, high-level: MIDI IN A/B and the computer port into the IPC
 * window on /CS5, the panel matrix, MIDI OUT. */

#define SUB_SOURCES 3
#define SUB_MAX_EXCLUSIVE 0x8a
#define SUB_SYSEX_MAX (SUB_MAX_EXCLUSIVE + 9)
#define SUB_BLOCK_SIZE 0x24
#define SUB_QUEUE_SIZE 64

/* one 31.25 us frame per sub_hle_frame() call */
#define SUB_DELIVER_FRAMES 1
#define SUB_BLOCK_RETRY_FRAMES 3
#define SUB_TX_BYTE_FRAMES 10

typedef struct sub_message
{
	uint8_t code, flags, d1, d2;
	uint8_t block_size;
	uint8_t block[SUB_BLOCK_SIZE];
} sub_message_t;

typedef struct sub_source
{
	uint8_t status;
	uint8_t data[2];
	uint8_t count;
	uint8_t sysex[SUB_SYSEX_MAX];
	uint16_t sysex_size;
	bool in_sysex;
} sub_source_t;

typedef struct sub_hle
{
	uint8_t dpram[0xd8];
	uint8_t ipcm[4];
	uint8_t ipcer[4];
	uint8_t flags[0x1b];
	uint8_t sem;
	uint8_t spcon;
	uint8_t pa, pa_dir, pb, pb_dir;
	bool int_state;
	bool in_reset;

	sub_source_t src[SUB_SOURCES];
	sub_message_t queue[SUB_QUEUE_SIZE];
	uint8_t queue_head, queue_count;
	bool busy;
	uint32_t deliver_frames;
	uint32_t queue_drops;
	uint32_t sysex_drops;

	uint8_t tx_rd, tx_left, tx_end;
	bool tx_running;
	uint32_t tx_frames;

	uint8_t keys[4];

	void (*irq)(void *user, bool state);
	void (*midi_out)(void *user, uint8_t byte);
	void *user;
} sub_hle_t;

void sub_hle_init(sub_hle_t *sub, void (*irq)(void *user, bool state), void (*midi_out)(void *user, uint8_t byte), void *user);
void sub_hle_reset(sub_hle_t *sub);
/* /RST from the main CPU's P4-0 (SC-88VL); active low */
void sub_hle_reset_w(sub_hle_t *sub, bool state);
uint8_t sub_hle_read(sub_hle_t *sub, uint32_t offset);
void sub_hle_write(sub_hle_t *sub, uint32_t offset, uint8_t data);
void sub_hle_midi_byte(sub_hle_t *sub, int source, uint8_t byte);
void sub_hle_set_key(sub_hle_t *sub, int row, int bit, bool down);
void sub_hle_frame(sub_hle_t *sub);

#endif
