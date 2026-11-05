/* SPDX-License-Identifier: GPL-2.0-or-later (adapted from xemu's DSP; see NOTICE.md) */
/*
 * bw_dsp_fast.h - the translated engine for xemu's DSP56300 (lane au-dsp).
 *
 * Runs DirectSound's GP and EP programs from a cache of pre-decoded
 * instructions built at run time, in memory, from the DSP's own P memory
 * (call A6: nothing is generated ahead of time or written to disk). It works
 * on xemu's own state (DSPState and the interpreter's dsp_core_t), so the two
 * engines can swap at any frame, and it is meant to be bit-identical to the
 * interpreter: BW_DSP_LOCKSTEP (bw_dsp.c) proves it frame by frame.
 */
#ifndef BW_DSP_FAST_H
#define BW_DSP_FAST_H

#include <stdint.h>
#include "dsp.h"
#include "bw_dsp.h"

typedef struct BwFast BwFast;

/* bw_dsp.c: the xemu state behind a BwDsp (for the engines). */
DSPState *bw_dsp_xemu(BwDsp *b);

BwFast *bwf_new(int is_gp);
void bwf_free(BwFast *f);

/* One frame, with exactly bw_dsp_run_frame's contract on the interpreter:
 * start-of-frame, then run until the program halts or max_cycles pass (at
 * the interpreter's 1000-cycle chunk boundaries). Returns the cycles used. */
uint32_t bwf_run_frame(BwFast *f, DSPState *dsp, uint32_t max_cycles);

/* P memory was written from outside the engine's frames (the CPU's window,
 * a bootstrap, a snapshot or a poke): blocks check their words again. */
void bwf_p_changed(BwFast *f);

/* BW_DSP_TIME: QPC ticks the inline DMA spent in the bridge's system-memory
 * callbacks since the last call. */
int64_t bwf_take_io_time(BwFast *f);

/* Counters since the last call (then zeroed): instructions run by the
 * translated handlers, by the interpreter's own step, and decodes. */
void bwf_take_stats(BwFast *f, uint64_t *fast, uint64_t *slow, uint64_t *decodes);

/* Per-kind instruction counts (BW_DSP_FASTSTAT=2), for tuning: written to
 * buf as "kind:count" pairs, then zeroed. No program words are printed. */
void bwf_kind_report(BwFast *f, char *buf, size_t cap);

/* BW_DSP_FASTSTAT=3: count instructions by form and mnemonic (xemu's
 * names; no program words), and report the top ones. */
void bwf_set_profile(BwFast *f, int on);
void bwf_profile_report(BwFast *f, char *buf, size_t cap, int top);

/* Checks each register-based ALU routine against the interpreter's on
 * `iters` random register files; one that ever differs is dropped (the
 * interpreter's runs instead). Returns how many were dropped (msg: which). */
int bwf_selftest_alu(uint64_t seed, int iters, char *msg, size_t cap);

/* Checks every handler of the engine's own (not the interpreter's routine)
 * against the interpreter's step: random instruction words and states, the
 * whole core compared. Returns how many cases differed; *tested how many ran. */
int bwf_selftest_ops(uint64_t seed, int iters, char *msg, size_t cap, int *tested);

/* Checks the JIT's native code against the C bodies on random blocks and
 * states (straight and DO-loop). Returns how many differed. */
int bwf_selftest_jit(uint64_t seed, int iters, char *msg, size_t cap, int *tested);

#endif
