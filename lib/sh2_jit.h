#ifndef SCEMU_SH2_JIT_H
#define SCEMU_SH2_JIT_H

#include "sh2.h"

/* The core's internals a translator drives: the bus, one instruction of the
 * interpreter, the interrupt controller and the peripheral clock. */

uint8_t sh2_mem_read8(sh2_t *cpu, uint32_t addr);
uint16_t sh2_mem_read16(sh2_t *cpu, uint32_t addr);
uint32_t sh2_mem_read32(sh2_t *cpu, uint32_t addr);
void sh2_mem_write8(sh2_t *cpu, uint32_t addr, uint8_t data);
void sh2_mem_write16(sh2_t *cpu, uint32_t addr, uint16_t data);
void sh2_mem_write32(sh2_t *cpu, uint32_t addr, uint32_t data);

/* One instruction.  A delayed branch leaves cpu->delay set and cpu->pc at the
 * slot; the next call runs the slot and lands on cpu->delay_target.  No
 * interrupt may be taken while cpu->delay is set. */
int sh2_exec_one(sh2_t *cpu);

void sh2_peripherals_tick(sh2_t *cpu, uint32_t cycles);
uint32_t sh2_tick_horizon(sh2_t *cpu);

int sh2_irq_select(sh2_t *cpu, int *out_level);
void sh2_take_interrupt(sh2_t *cpu, int vector, int level);
void sh2_exception(sh2_t *cpu, int vector, uint32_t ret_pc, int level);
int sh2_vector_level(const sh2_t *cpu, int vector);

void sh2_mac_l(sh2_t *cpu, uint32_t a, uint32_t b);
void sh2_mac_w(sh2_t *cpu, uint16_t a, uint16_t b);
void sh2_div1(sh2_t *cpu, int m, int n);

int sh2_jit_run(sh2_t *cpu, int cycles);

#endif
