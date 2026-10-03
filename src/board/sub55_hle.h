#ifndef SCEMU_SUB55_HLE_H
#define SCEMU_SUB55_HLE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* M37409M2 sub-CPU, high-level: MIDI IN 1 and 2 and the computer port into
 * the dual-port window the main CPU sees at 0EC00-0ECFF, the front panel
 * matrix through the port passthrough, and MIDI OUT off its own UART.  The
 * main CPU never sees a byte stream: the sub parses each input into whole
 * messages and hands them over one byte at a time, and every dual-port byte
 * carries an access flag that is the real handshake. */

#define SUB55_DPRAM_SIZE 0xc0
#define SUB55_FLAG_BYTES (SUB55_DPRAM_SIZE / 8)

/* the three inputs, in the order the sub's UARTs carry them */
enum { SUB55_MIDI1 = 0, SUB55_MIDI2 = 1, SUB55_COMPUTER = 2 };
#define SUB55_SOURCES 3

/* the two byte channels the main CPU reads, EC20 and EC21 */
#define SUB55_CHANNELS 2
#define SUB55_FIFO_SIZE 256

/* the sub's software transmit queue behind the UART */
#define SUB55_TX_SIZE 64

/* the transmit block the main CPU fills, EC00-EC0F */
#define SUB55_TX_BLOCK 16

/* one message being assembled, with the running status it was assembled under */
typedef struct sub55_source
{
	uint8_t status;      /* running status */
	uint8_t data[2];
	uint8_t count;
	uint8_t sense;       /* the 500 ms watchdog in 10 ms ticks; 0 = disarmed */
} sub55_source_t;

/* a byte channel: what the parser and the delivery filter have left to deposit */
typedef struct sub55_channel
{
	uint8_t fifo[SUB55_FIFO_SIZE];
	uint16_t head, count;
	uint8_t last_status;  /* the filter's memory; 0 = none */
} sub55_channel_t;

typedef struct sub55_hle
{
	uint8_t dpram[SUB55_DPRAM_SIZE];
	uint8_t flags[SUB55_FLAG_BYTES];
	uint8_t reason;       /* ECF8 read side */
	uint8_t sem;          /* ECFF, bit 7 = the sub is idle and ECB9 is free */
	uint8_t p0, p0_dir;   /* ECF6 and ECF7 */
	uint8_t keys[4];      /* the matrix columns per row, active low */
	bool started;

	sub55_source_t src[SUB55_SOURCES];
	sub55_channel_t ch[SUB55_CHANNELS];

	uint8_t tx[SUB55_TX_SIZE];
	uint8_t tx_head, tx_count;
	uint8_t tx_status;    /* running status on the way out */
	uint32_t tx_frames;   /* until the transmitter takes the next byte */

	uint32_t boot_frames; /* the 2.4 ms the sub takes to come up, before which it answers as cleared */
	uint32_t tick_frames; /* until the next 10 ms tick */
	uint32_t tick_reload, tx_reload, boot_reload;
	uint8_t sense_div;    /* the 25-tick divider under the 250 ms generator */
	bool sensing;

	void (*attention)(void *user);
	void (*midi_out)(void *user, uint8_t byte);
	void *user;
} sub55_hle_t;

/* `rate` is the board's frame rate, which sets the 10 ms tick and the byte time */
void sub55_hle_init(sub55_hle_t *sub, uint32_t rate,
		void (*attention)(void *user), void (*midi_out)(void *user, uint8_t byte), void *user);
void sub55_hle_reset(sub55_hle_t *sub);

/* the window at 0EC00-0ECFF, offset 00-FF, from the main CPU's side */
uint8_t sub55_hle_read(sub55_hle_t *sub, uint32_t offset);
void sub55_hle_write(sub55_hle_t *sub, uint32_t offset, uint8_t data);

/* room for the messages one more byte on that input can produce */
bool sub55_hle_ready(const sub55_hle_t *sub, int source);
void sub55_hle_midi_byte(sub55_hle_t *sub, int source, uint8_t byte);

/* the matrix, and the three LEDs on the same port: bit 0 ALL, 1 MUTE, 2 STANDBY */
void sub55_hle_set_key(sub55_hle_t *sub, int row, int bit, bool down);
uint8_t sub55_hle_leds(const sub55_hle_t *sub);

void sub55_hle_frame(sub55_hle_t *sub);

struct state_registry;
void sub55_hle_state(sub55_hle_t *sub, struct state_registry *reg);

#endif
