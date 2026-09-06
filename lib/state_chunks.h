#ifndef SCEMU_STATE_CHUNKS_H
#define SCEMU_STATE_CHUNKS_H

#include "state_stream.h"
#include "h8500.h"
#include "xp.h"
#include "lsp.h"
#include "midi_queue.h"

/* The chunks the boards share: a chip's state as a byte stream, the ERAMs apart. */

void state_put_h8500(state_writer_t *w, const h8500_t *c);
void state_get_h8500(state_reader_t *r, h8500_t *c);
void state_put_xp(state_writer_t *w, const xp_t *x);
void state_get_xp(state_reader_t *r, xp_t *x);
void state_put_lsp(state_writer_t *w, const lsp_t *l);
void state_get_lsp(state_reader_t *r, lsp_t *l);
void state_put_midi(state_writer_t *w, const midi_queue_t *q);
void state_get_midi(state_reader_t *r, midi_queue_t *q);

#endif
