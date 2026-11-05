/* SPDX-License-Identifier: GPL-2.0-or-later (with xemu's DSP it hosts; see NOTICE.md) */
/*
 * bw_dsp_engine.h - what runs a DSP's frame: the interpreter, or (au-dsp) the
 * recompiled programs. The offline replayer (tools/dsp_replay) drives any
 * engine through this, and checks it against a trace or another engine.
 *
 * An engine works on a BwDsp: xemu's DSP state (registers, memories, DMA)
 * and its scratch/FIFO callbacks. Its DMA goes through xemu's own helpers, so
 * every transfer reaches the BwDsp's callbacks, which the replayer feeds from
 * the trace. An engine that keeps state of its own (cached registers, a P
 * memory check) syncs it in state_in / state_out.
 *
 * The contract of run_frame is bw_dsp_run_frame's, exactly: raise the
 * start-of-frame bit, clear the halt, set the cycle count to 0, then run whole
 * instructions in dsp_run(1000) slices (dsp_c_run's carry in save_cycles)
 * until the program halts or the count reaches max_cycles; return the count.
 * It must not call the observation hooks (bw_dsp_set_hooks): in the game,
 * bw_dsp_run_frame brackets whichever engine runs.
 *
 * P memory changes inside runs too (seen, au-trace's traces): the EP writes
 * 3 of its own P words in every run (0x0007-0x01A2), and the GP DMAs its
 * effect code in (3,119 words at 0x171-0xDB4, about 0.4 s after boot). The
 * interpreter sees each change (write_memory_raw clears pram_opcache[addr];
 * the DMA's mem_write goes the same way). A runtime translator (call A6) must
 * see them as well, and take P as the BwDsp holds it whenever state_in says
 * it was changed from outside.
 */
#ifndef BW_DSP_ENGINE_H
#define BW_DSP_ENGINE_H

#include "bw_dsp.h"

typedef struct BwDspEngine {
    const char *name;
    /* Bind to a DSP; returns the engine's handle for it (NULL = failed). */
    void *(*attach)(BwDsp *dsp);
    void (*detach)(void *h);
    /* The BwDsp's state was replaced or edited from outside (a snapshot
     * loaded, a frame's inputs poked in): take it up. May be NULL. */
    void (*state_in)(void *h);
    /* Make the BwDsp's state current, before anyone reads it. May be NULL. */
    void (*state_out)(void *h);
    uint32_t (*run_frame)(void *h, uint32_t max_cycles);
} BwDspEngine;

/* xemu's interpreter (bw_dsp.c): the reference. */
extern const BwDspEngine bw_dsp_engine_interp;

/* The raw interpreter frame, without the observation hooks. */
uint32_t bw_dsp_run_frame_raw(BwDsp *dsp, uint32_t max_cycles);

#endif
