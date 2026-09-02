#ifndef SCEMU_H8500_H
#define SCEMU_H8500_H

#include <stdint.h>
#include <stdbool.h>

/* Hitachi H8/510 (HD6415108) in mode 4: 24-bit addressing, 16-bit external bus. */

typedef struct h8500_bus
{
	uint8_t (*read8)(void *user, uint32_t address);
	void (*write8)(void *user, uint32_t address, uint8_t data);
	uint16_t (*read16)(void *user, uint32_t address);
	void (*write16)(void *user, uint32_t address, uint16_t data);
	uint8_t (*read_port)(void *user, int port);
	void (*write_port)(void *user, int port, uint8_t data, uint8_t ddr);
	uint16_t (*read_adc)(void *user, int channel);
	void (*sci_tx)(void *user, int channel, uint8_t byte);
	void *user;
} h8500_bus_t;

enum
{
	H8500_IRQ0 = 0,
	H8500_IRQ1 = 1,
	H8500_IRQ2 = 2,
	H8500_NMI = 3
};

enum
{
	H8500_PORT1 = 1, H8500_PORT2, H8500_PORT3, H8500_PORT4,
	H8500_PORT5, H8500_PORT6, H8500_PORT7, H8500_PORT8, H8500_PORT9
};

typedef struct h8500_sci
{
	uint8_t smr, brr, scr, tdr, ssr, rdr;
	uint8_t rx_pending;
	bool rx_full;
	uint32_t tx_timer;
} h8500_sci_t;

typedef struct h8500
{
	h8500_bus_t bus;

	uint16_t pc;
	uint16_t sr;
	uint8_t cp, dp, ep, tp, br;
	uint16_t r[8];

	uint8_t irq_lines;
	int32_t icount;
	uint64_t cycles;

	uint8_t io[0x180];
	h8500_sci_t sci[2];
	uint16_t adc[8];
	uint8_t port_ddr[10];
	uint8_t port_data[10];

	bool sleeping;
} h8500_t;

void h8500_init(h8500_t *cpu, const h8500_bus_t *bus);
void h8500_reset(h8500_t *cpu);

/* Run at least `cycles` φ cycles; returns the number actually run so the
 * caller can carry the overshoot. */
int h8500_run(h8500_t *cpu, int cycles);

void h8500_set_irq(h8500_t *cpu, int line, bool state);
void h8500_sci_rx(h8500_t *cpu, int channel, uint8_t byte);

#endif
