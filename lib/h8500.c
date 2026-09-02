#include <string.h>
#include "h8500.h"

void h8500_init(h8500_t *cpu, const h8500_bus_t *bus)
{
	memset(cpu, 0, sizeof(*cpu));
	cpu->bus = *bus;
}

void h8500_reset(h8500_t *cpu)
{
	cpu->sr = 0x0700;
	cpu->cp = 0;
	cpu->dp = 0;
	cpu->ep = 0;
	cpu->tp = 0;
	cpu->br = 0;
	cpu->irq_lines = 0;
	cpu->sleeping = false;
	cpu->pc = cpu->bus.read16(cpu->bus.user, 0x000000);
}

int h8500_run(h8500_t *cpu, int cycles)
{
	cpu->cycles += (uint64_t)cycles;
	return cycles;
}

void h8500_set_irq(h8500_t *cpu, int line, bool state)
{
	if (state)
		cpu->irq_lines |= (uint8_t)(1u << line);
	else
		cpu->irq_lines &= (uint8_t)~(1u << line);
}

void h8500_sci_rx(h8500_t *cpu, int channel, uint8_t byte)
{
	h8500_sci_t *sci = &cpu->sci[channel & 1];
	sci->rx_pending = byte;
	sci->rx_full = true;
}
