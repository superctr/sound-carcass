#ifndef SCEMU_H8500_H
#define SCEMU_H8500_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/*
 * The Hitachi H8/500 family in mode 4 (maximum mode), big-endian: the
 * H8/510 (HD6415108, 24-bit addresses, 16-bit bus) and the H8/532
 * (HD6475328, 20-bit addresses, 8-bit bus).  The core owns everything on
 * the chip — CPU, interrupt controller, the SCIs, the A/D converter, the
 * free-running timers, the 8-bit timer, the watchdog, the I/O ports, and
 * the on-chip RAM where the part has one — and reaches the board through
 * h8500_bus_t.  The on-chip register file in page 0 never reaches the bus.
 * Which part is which is one h8500_variant_t; see docs/h8500_notes.md.
 *
 * Time is φ cycles (the crystal halved: 10 MHz on the SC-88, 12 MHz on the
 * SC-55mkII).  All state is plain data; a snapshot is a copy of the struct
 * with the bus, the variant and the region pointers put back.
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
#define H8500_MAX_REGIONS 8

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
	H8500_PORT5, H8500_PORT6, H8500_PORT7, H8500_PORT8, H8500_PORT9
};

enum
{
	H8500_SCI1 = 0,
	H8500_SCI2 = 1
};

enum
{
	H8500_H8510 = 0,
	H8500_H8532
};

/* the H8/510's register file; io[] is sized for it and the H8/532's fits
 * inside */
#define H8500_IO_BASE 0xfe80
#define H8500_IO_SIZE 0x180

#define H8500_MAX_PORTS 10
#define H8500_MAX_FRT 3
#define H8500_MAX_SCI 2
#define H8500_MAX_IRAM 0x400

/* Everything that differs between the parts, as plain data: where each
 * module's registers sit in the register file (offsets from io_base, -1
 * for a module or register the part does not have), which vector each one
 * raises, and the vector -> IPR slot table.  Even slots are bits 6-4 of
 * IPR[slot/2], odd slots bits 2-0; -1 is a source with no programmable
 * priority. */
typedef struct h8500_variant
{
	uint32_t addr_mask;
	uint16_t io_base, io_size;
	uint16_t ram_base, ram_size;
	bool bus8;

	int port_count;
	int16_t port_ddr[H8500_MAX_PORTS];
	int16_t port_dr[H8500_MAX_PORTS];
	uint8_t port_ddr_fixed[H8500_MAX_PORTS];
	uint8_t port_ddr_reset[H8500_MAX_PORTS];
	uint8_t port_mask[H8500_MAX_PORTS];

	int frt_count;
	uint8_t frt_reg[H8500_MAX_FRT];
	uint8_t frt_vector[H8500_MAX_FRT];
	bool frt_temp;

	uint8_t tmr_reg, tmr_vector;

	int sci_count;
	uint8_t sci_reg[H8500_MAX_SCI];
	uint8_t sci_vector[H8500_MAX_SCI];

	uint8_t adc_reg, adc_vector;

	int16_t wdt_reg;
	uint8_t wdt_vector;

	uint8_t ipr_reg;

	int irq_pin_count;
	uint8_t irq_vector[4];
	int16_t ctl_reg[2];
	uint8_t ctl_write[2], ctl_read[2];
	uint8_t irq_ctl, irq_ctl_shift;
	uint8_t nmi_ctl, nmi_ctl_bit;

	signed char vector_slot[64];
} h8500_variant_t;

const h8500_variant_t *h8500_variant(int model);

typedef struct h8500
{
	h8500_bus_t bus;
	const h8500_variant_t *var;
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

	/* on-chip register file and the peripherals behind it; io_kind/io_unit
	 * say which module owns each byte of it, filled in from the variant */
	uint8_t io[H8500_IO_SIZE];
	uint8_t io_kind[H8500_IO_SIZE];
	uint8_t io_unit[H8500_IO_SIZE];
	uint8_t iram[H8500_MAX_IRAM];
	uint16_t frt_count[H8500_MAX_FRT];
	uint32_t frt_prescale[H8500_MAX_FRT];
	uint8_t frt_temp[H8500_MAX_FRT];
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
	uint16_t port_out[H8500_MAX_PORTS];

	/* time */
	uint64_t cycles;

	/* the dynamic translator (h8500_jit.c), when attached */
	struct h8500_jit *jit;
	bool jit_enabled;
	bool irq_ready;
	uint32_t jit_pending;
	uint32_t jit_limit;
	uint64_t jit_deadline;
} h8500_t;

/* h8500_init is the H8/510; h8500_init_model picks the part. */
void h8500_init(h8500_t *cpu, const h8500_bus_t *bus);
void h8500_init_model(h8500_t *cpu, const h8500_bus_t *bus, int model);
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

/* The dynamic translator: attached to a core after its regions are mapped,
 * it takes over h8500_run while jit_enabled is set.  Translated code is
 * taken from the read-only regions only and cached for the life of the
 * attachment, so remapping a region means detaching and attaching again. */
struct jit_alloc;
#ifdef SCEMU_H8500_JIT
bool h8500_jit_attach(h8500_t *cpu, struct jit_alloc *alloc);
void h8500_jit_detach(h8500_t *cpu);
void h8500_jit_flush(h8500_t *cpu);
size_t h8500_jit_code_size(const h8500_t *cpu);
size_t h8500_jit_block_count(const h8500_t *cpu);
/* blocks run, instructions handed to the interpreter, interrupts taken, peripheral flushes */
void h8500_jit_stats(const h8500_t *cpu, uint64_t out[4]);
#else
static inline bool h8500_jit_attach(h8500_t *cpu, struct jit_alloc *alloc) { (void)cpu; (void)alloc; return false; }
static inline void h8500_jit_detach(h8500_t *cpu) { (void)cpu; }
static inline void h8500_jit_flush(h8500_t *cpu) { (void)cpu; }
static inline size_t h8500_jit_code_size(const h8500_t *cpu) { (void)cpu; return 0; }
static inline size_t h8500_jit_block_count(const h8500_t *cpu) { (void)cpu; return 0; }
static inline void h8500_jit_stats(const h8500_t *cpu, uint64_t out[4]) { (void)cpu; out[0] = out[1] = out[2] = out[3] = 0; }
#endif

#endif
