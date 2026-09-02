#ifndef SCEMU_INTERNAL_H
#define SCEMU_INTERNAL_H

#include "scemu.h"
#include "sc88.h"

struct scemu
{
	scemu_model_t model;
	scemu_config_t config;
	const char *error;
	sc88_t machine;
};

#endif
