#ifndef SCEMU_UIPC_H
#define SCEMU_UIPC_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "scemu.h"

/* The M37640 USB controller as the SH-2 sees it: two byte-wide mailboxes,
 * UIPC(0) going out and UIPC(1) coming back, each with data at +0 and status
 * at +1.  A high-level emulation of the controller's application: the
 * power-on handshake, the tagged byte stream in, the four byte packets out,
 * and the four port groups packed as USB-MIDI event packets.  The board
 * decides how the controller's two events reach the CPU. */

#define UIPC_PORTS 4
#define UIPC_RX 1024

/* One port group's wire bytes on their way into four byte packets. */
typedef struct uipc_in
{
	uint8_t msg[3];
	uint8_t count;
	uint8_t need;
	uint8_t cin;
	uint8_t status;
	bool sysex;
} uipc_in_t;

typedef struct uipc_link
{
	/* a byte has been put into UIPC(1); uipc_waiting() says whether one still is */
	void (*rx_event)(void *user);
	/* UIPC(0) has taken the byte written to it */
	void (*tx_event)(void *user);
	/* a packet the firmware sent, with the port group it left by */
	void (*midi_out)(void *user, int port, const uint8_t *bytes, size_t count);
	/* the rear COMPUTER switch: a host is on the bus in its USB position */
	scemu_computer_switch_t (*computer_switch)(void *user);
	void *user;
} uipc_link_t;

typedef struct uipc
{
	uipc_link_t link;
	/* the SC-8820's controller has the switch on its own pins and reports it in the handshake */
	bool reports_switch;
	uipc_in_t in[UIPC_PORTS];
	uint16_t rx[UIPC_RX];
	uint16_t rx_head;
	uint16_t rx_count;
	uint8_t tx[4];
	uint8_t tx_count;
	uint8_t boot;
	bool running;
	bool host;
	bool online;
	uint32_t announce;
	uint32_t poll;
} uipc_t;

void uipc_init(uipc_t *u, const uipc_link_t *link, bool reports_switch);
void uipc_reset(uipc_t *u);
uint8_t uipc_read(uipc_t *u, int channel, uint32_t offset);
void uipc_write(uipc_t *u, int channel, uint32_t offset, uint8_t data);
void uipc_frame(uipc_t *u);

/* the switch put a host on the bus; the host has announced itself; a byte waits in UIPC(1);
 * there is room for one more packet */
static inline bool uipc_host(const uipc_t *u) { return u->host; }
static inline bool uipc_online(const uipc_t *u) { return u->online; }
static inline bool uipc_waiting(const uipc_t *u) { return u->rx_count != 0; }
static inline bool uipc_can_take(const uipc_t *u) { return u->rx_count + 4 <= UIPC_RX; }
void uipc_take_midi(uipc_t *u, int port, uint8_t byte);

#endif
