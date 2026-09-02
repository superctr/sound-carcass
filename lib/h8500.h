#ifndef SCEMU_H8500_H
#define SCEMU_H8500_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/*
 * Hitachi H8/510 (HD6415108) in mode 4: 24-bit addresses, 16-bit external
 * bus, big-endian.  The core owns everything on the chip — CPU, interrupt
 * controller, the two SCIs, the A/D converter, FRT1/FRT2, the 8-bit timer,
 * the watchdog, the I/O ports — and reaches the board through h8500_bus_t.
 * The on-chip register file at page 0 FE80-FFFF never reaches the bus.
 *
 * Time is φ cycles (the crystal halved: 10 MHz on the SC-88).  All state is
 * plain data; a snapshot is a copy of the struct with the bus and the region
 * pointers put back.
 */

typedef struct h8500_bus
{
	uint8_t (*read8)(void *user, uint32_t address);
	void (*write8)(void *user, uint32_t address, uint8_t data);
	uint16_t (*read16)(void *user, uint32_t address);
	void (*write16)(void *user, uint32_t address, uint16_t data);

	/* pins of a port: the value on the input pins, and the data/direction
	 * registers after a write */
	uint8_t (*read_port)(void *user, int port);
	void (*write_port)(void *user, int port, uint8_t data, uint8_t ddr);

	/* a 10-bit conversion result for AN0-AN7 */
	uint16_t (*read_adc)(void *user, int channel);

	/* a byte leaves a serial transmitter, after its bit time */
	void (*sci_tx)(void *user, int channel, uint8_t byte);

	void *user;
} h8500_bus_t;

/* Memory the core may touch directly, bypassing the bus callbacks: the
 * program ROM and the SRAM.  Any address outside every region goes through
 * the callbacks. */
#define H8500_MAX_REGIONS 4

typedef struct h8500_region
{
	uint32_t base;
	uint32_t size;
	uint8_t *data;
	bool writable;
} h8500_region_t;

enum
{
	H8500_IRQ0 = 0,
	H8500_IRQ1,
	H8500_IRQ2,
	H8500_NMI
};

enum
{
	H8500_PORT1 = 1, H8500_PORT2, H8500_PORT3, H8500_PORT4,
	H8500_PORT5, H8500_PORT6, H8500_PORT7, H8500_PORT8
};

enum
{
	H8500_SCI1 = 0,
	H8500_SCI2 = 1
};

#define H8500_IO_BASE 0xfe80
#define H8500_IO_SIZE 0x180

typedef struct h8500
{
	h8500_bus_t bus;
	h8500_region_t regions[H8500_MAX_REGIONS];
	int region_count;

	/* CPU */
	uint16_t pc;
	uint16_t sr;
	uint8_t cp, dp, ep, tp, br;
	uint16_t r[8];
	bool sleeping;

	/* interrupts */
	uint8_t irq_lines;
	uint8_t irq_req;
	uint32_t irq_pend[3];
	bool no_irq;

	/* on-chip register file, FE80-FFFF, and the peripherals behind it */
	uint8_t io[H8500_IO_SIZE];
	uint16_t frt_count[2];
	uint32_t frt_prescale[2];
	uint8_t tmr_count;
	uint32_t tmr_prescale;
	uint32_t wdt_prescale;
	uint32_t adc_busy;
	uint8_t adc_channel;
	struct
	{
		uint8_t rx_byte;
		bool rx_pending;
		uint32_t tx_timer;
		uint8_t tx_shift;
		uint8_t ssr_read;
		bool tx_busy;
	} sci[2];
	uint16_t port_out[9];

	/* time */
	uint64_t cycles;
} h8500_t;

void h8500_init(h8500_t *cpu, const h8500_bus_t *bus);
void h8500_map(h8500_t *cpu, uint32_t base, uint32_t size, uint8_t *data, bool writable);
void h8500_reset(h8500_t *cpu);

/* Execute one instruction (or take a pending interrupt, or idle one cycle in
 * SLEEP); returns the φ cycles it took. */
int h8500_step(h8500_t *cpu);

/* Run until at least `cycles` φ cycles have elapsed; returns the number
 * actually run, so the caller can carry the overshoot. */
int h8500_run(h8500_t *cpu, int cycles);

/* External interrupt pins, level: IRQ0-IRQ2 and NMI. */
void h8500_set_irq(h8500_t *cpu, int line, bool state);

/* A whole byte arrives at a serial receiver. */
void h8500_sci_rx(h8500_t *cpu, int channel, uint8_t byte);

/* The core reads and writes its own register file through these; the board
 * uses them for tests and for the boot snapshot. */
uint8_t h8500_io_read(h8500_t *cpu, uint16_t address);
void h8500_io_write(h8500_t *cpu, uint16_t address, uint8_t data);

#endif
