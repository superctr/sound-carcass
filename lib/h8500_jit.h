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

/* Execution states, from the H8/520 hardware manual's appendix A.4: Table
 * A-7's figures, Table A-8's adjustment for a fetch from the 16-bit 2-state
 * space, a fetch's states, and what an operand access adds. */
enum { H8500_ADJ_OTHER = 0, H8500_ADJ_MOVB, H8500_ADJ_MOVW };
#define H8500_IRQ_STATES 23

int h8500_general_states(uint8_t op, int mode, int sz);
int h8500_divxu_states(int mode, int sz, int outcome);
int h8500_adjust(int cls, int mode, uint16_t pc);
int h8500_general_adjust(uint8_t op, int mode, uint16_t pc);
int h8500_fetch_states(const h8500_t *cpu, uint32_t addr, int jk, int adj, int waits);
uint32_t h8500_access_states(const h8500_t *cpu, uint32_t addr, int word);

int h8500_jit_run(h8500_t *cpu, int cycles);

#endif
