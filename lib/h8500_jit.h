#ifndef SCEMU_H8500_JIT_H
#define SCEMU_H8500_JIT_H

#include "h8500.h"

/* The core's internals the translator drives: the bus, one instruction of
 * the interpreter, the interrupt controller and the peripheral clock. */

uint8_t h8500_mem_read8(h8500_t *cpu, uint32_t addr);
void h8500_mem_write8(h8500_t *cpu, uint32_t addr, uint8_t data);
uint16_t h8500_mem_read16(h8500_t *cpu, uint32_t addr);
void h8500_mem_write16(h8500_t *cpu, uint32_t addr, uint16_t data);

int h8500_exec_one(h8500_t *cpu);
void h8500_peripherals_tick(h8500_t *cpu, uint32_t cycles);
uint32_t h8500_tick_horizon(h8500_t *cpu);
int h8500_irq_select(h8500_t *cpu, int *out_level);
void h8500_take_interrupt(h8500_t *cpu, int vector, int level);
void h8500_exception(h8500_t *cpu, int vector, uint16_t ret_pc, int level);

extern const uint8_t h8500_cyc_src[10];
extern const uint8_t h8500_cyc_rmw[10];

int h8500_jit_run(h8500_t *cpu, int cycles);

#endif
