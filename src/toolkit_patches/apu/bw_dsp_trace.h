/* SPDX-License-Identifier: GPL-2.0-or-later (with xemu's DSP it hosts; see NOTICE.md) */
/*
 * bw_dsp_trace.h - BW_DSP_TRACE=<dir>, the DSP trace recorder (au-trace).
 * A test aid in every edition; inert when unset. See bw_dsp_trace.c and
 * docs/audio/trace_format.md.
 */
#ifndef BW_DSP_TRACE_H
#define BW_DSP_TRACE_H

#include <stdint.h>

struct MCPXAPUState;

/* bw_apu_xdsp.c, bw_xdsp_init: read the switches, install the hooks. */
void bw_dsp_trace_init(struct MCPXAPUState *d);
/* bw_apu_xdsp.c, dsp_do_frame: the DSP stage's frame clock (on the thread
 * that runs the DSPs, before the frame's resets and runs). */
void bw_dsp_trace_frame(long frame, int div);

/* bw_apu_xdsp.c: one 4 KB page (entry) of a DSP's scatter-gather space as the
 * DSP would read it, without moving a FIFO position or logging. which: 0 GP
 * scratch, 1 GP FIFO, 2 EP scratch, 3 EP FIFO. Returns the space's entry
 * count (0: no table); out is zeros where the entry names no memory. */
uint32_t bw_xdsp_peek_page(struct MCPXAPUState *d, int which, uint32_t entry, uint8_t *out);

#endif
