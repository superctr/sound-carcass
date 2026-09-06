#ifndef SCEMU_SH2_H
#define SCEMU_SH2_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/*
 * Hitachi SH-2 of the SH7010 group (SH7016/SH7017): 32-bit addresses,
 * big-endian, with the on-chip peripherals the SC-8850 uses — the interrupt
 * controller, the two serial ports, the multifunction timer, the watchdog,
 * the DMA controller, the mid-speed A/D converter, the I/O ports, the bus
 * state controller's register file and the on-chip RAM.  The board is reached
 * through sh2_bus_t; the register block at FFFF8000-FFFF87FF and the on-chip
 * RAM at FFFFF000 never leave the core.
 *
 * Time is φ cycles (28.224 MHz on the SC-8850).  All state is plain data; a
 * snapshot is a copy of the struct with the bus and the region pointers put
 * back.
 */

typedef struct sh2_bus
{
	uint8_t (*read8)(void *user, uint32_t address);
	uint16_t (*read16)(void *user, uint32_t address);
	uint32_t (*read32)(void *user, uint32_t address);
	void (*write8)(void *user, uint32_t address, uint8_t data);
	void (*write16)(void *user, uint32_t address, uint16_t data);
	void (*write32)(void *user, uint32_t address, uint32_t data);

	/* pins of a port, and the data/direction pair after a write */
	uint16_t (*read_port)(void *user, int port);
	void (*write_port)(void *user, int port, uint16_t data, uint16_t ior);

	/* a 10-bit conversion result for AN0-AN3 */
	uint16_t (*read_adc)(void *user, int channel);

	/* a byte leaves a serial transmitter, after its bit time */
	void (*sci_tx)(void *user, int channel, uint8_t byte);

	void *user;
} sh2_bus_t;

/* Memory the core may touch directly, bypassing the bus callbacks. */
#define SH2_MAX_REGIONS 8

typedef struct sh2_region
{
	uint32_t base;
	uint32_t size;
	uint8_t *data;
	bool writable;
	bool bypass;
} sh2_region_t;

enum
{
	SH2_IRQ0 = 0, SH2_IRQ1, SH2_IRQ2, SH2_IRQ3,
	SH2_IRQ4, SH2_IRQ5, SH2_IRQ6, SH2_IRQ7,
	SH2_NMI = 8
};

enum
{
	SH2_PORT_A = 0,
	SH2_PORT_B,
	SH2_PORT_E,
	SH2_PORT_F
};

enum
{
	SH2_SCI0 = 0,
	SH2_SCI1 = 1
};

#define SH2_IO_BASE 0xffff8000u
#define SH2_IO_END  0xffff8800u
#define SH2_RAM_BASE 0xfffff000u
#define SH2_RAM_SIZE 0x1000u

typedef struct sh2_sci
{
	uint8_t smr, brr, scr, tdr, ssr, rdr;
	uint8_t tx_shift;
	uint32_t tx_timer;
	bool tx_busy;
	uint8_t rx_byte;
	bool rx_pending;
	bool int_rxi, int_txi, int_tei, int_eri;
} sh2_sci_t;

typedef struct sh2_mtu_channel
{
	uint8_t tcr, tmdr, tiorh, tiorl, tier, tsr;
	uint16_t tcnt;
	uint16_t tgr[4];
	uint32_t prescale;
	bool active;
} sh2_mtu_channel_t;

typedef struct sh2_dma_channel
{
	uint32_t sar, dar, chcr;
	uint32_t dmatcr;
	uint32_t count;
	uint32_t timer;
	bool active;
	bool int_te;
} sh2_dma_channel_t;

typedef struct sh2
{
	sh2_bus_t bus;
	sh2_region_t regions[SH2_MAX_REGIONS];
	int region_count;

	/* CPU */
	uint32_t r[16];
	uint32_t sr, gbr, vbr, mach, macl, pr, pc;
	uint32_t delay_target;
	bool delay;
	bool sleeping;

	/* interrupt controller */
	uint16_t ipr[8];
	uint16_t icr, isr;
	uint32_t pending[8];
	uint8_t irq_line;
	bool nmi_line, nmi_pending;

	/* serial */
	sh2_sci_t sci[2];

	/* multifunction timer */
	sh2_mtu_channel_t mtu[3];
	uint8_t tsyr;

	/* watchdog */
	uint8_t wtcsr, wtcnt, rstcsr;
	uint32_t wdt_prescale;
	bool wdt_int;

	/* A/D */
	uint16_t addr_[4];
	uint8_t adcsr, adcr;
	uint32_t adc_timer;
	bool adc_int;

	/* DMA */
	sh2_dma_channel_t dma[2];
	uint16_t dmaor;

	/* I/O ports */
	uint16_t padr, paior, pacr1, pacr2;
	uint16_t pbdr, pbior, pbcr1, pbcr2;
	uint16_t pedr, peior, pecr1, pecr2;
	uint16_t port_out[3];

	/* bus state controller and the cache control register, stored only */
	uint16_t bcr1, bcr2, wcr1, wcr2, dcr, rtcsr, rtcnt, rtcor;
	uint16_t ccr;

	uint8_t ram[SH2_RAM_SIZE];

	/* time */
	uint64_t cycles;

	/* the dynamic translator (sh2_jit.c), when attached */
	struct sh2_jit *jit;
	bool jit_enabled;
	bool irq_ready;
	uint32_t jit_pending;
	uint32_t jit_limit;
	uint64_t jit_deadline;
} sh2_t;

void sh2_init(sh2_t *cpu, const sh2_bus_t *bus);
void sh2_map(sh2_t *cpu, uint32_t base, uint32_t size, uint8_t *data, bool writable);
void sh2_reset(sh2_t *cpu);

/* Execute one instruction — a delayed branch and its slot count as one, since
 * no interrupt may fall between them — or take a pending interrupt, or idle
 * one cycle in SLEEP; returns the φ cycles it took. */
int sh2_step(sh2_t *cpu);

/* Run until at least `cycles` φ cycles have elapsed; returns the number
 * actually run, so the caller can carry the overshoot. */
int sh2_run(sh2_t *cpu, int cycles);

/* External interrupt pins, level: IRQ0-IRQ7 and NMI. */
void sh2_set_irq(sh2_t *cpu, int line, bool state);

/* A whole byte arrives at a serial receiver. */
void sh2_sci_rx(sh2_t *cpu, int channel, uint8_t byte);

/* The core reads and writes its own register block through these; the board
 * uses them for tests and for the boot snapshot. */
uint32_t sh2_io_read(sh2_t *cpu, uint32_t address, int width);
void sh2_io_write(sh2_t *cpu, uint32_t address, int width, uint32_t data);

/* The dynamic translator: attached to a core after its regions are mapped,
 * it takes over sh2_run while jit_enabled is set.  Translated code is taken
 * from the read-only regions only and cached for the life of the
 * attachment, so remapping a region means flushing or attaching again.  On a
 * host sljit cannot generate 64-bit code for, attaching fails and the
 * interpreter stays. */
struct jit_alloc;
bool sh2_jit_attach(sh2_t *cpu, struct jit_alloc *alloc);
void sh2_jit_detach(sh2_t *cpu);
void sh2_jit_flush(sh2_t *cpu);
void sh2_jit_remap(sh2_t *cpu);
size_t sh2_jit_code_size(const sh2_t *cpu);
size_t sh2_jit_block_count(const sh2_t *cpu);
/* blocks run, instructions handed to the interpreter, interrupts taken, peripheral flushes */
void sh2_jit_stats(const sh2_t *cpu, uint64_t out[4]);

#endif
