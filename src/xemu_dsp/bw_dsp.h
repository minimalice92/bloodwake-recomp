/* SPDX-License-Identifier: GPL-2.0-or-later (with xemu's DSP it hosts; see NOTICE.md) */
/*
 * bw_dsp.h - Blood Wake's opaque wrapper around xemu's MCPX DSP (D-012).
 *
 * xemu's DSP56300 interpreter (src/xemu_dsp, from xemu v0.8.139, GPL-2.0+)
 * defines its own DSPState, which the toolkit's APU headers also name, so the
 * carried apu_dsp.c talks to it only through this header.
 */
#ifndef BW_DSP_H
#define BW_DSP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct BwDsp BwDsp;

/* dir true: DSP -> memory (a write), false: memory -> DSP (a read). */
typedef void (*bw_dsp_scratch_rw)(void *opaque, uint8_t *ptr, uint32_t addr,
                                  size_t len, bool dir);
typedef void (*bw_dsp_fifo_rw)(void *opaque, uint8_t *ptr, unsigned int index,
                               size_t len, bool dir);

BwDsp *bw_dsp_new(void *opaque, bw_dsp_scratch_rw scratch, bw_dsp_fifo_rw fifo, bool is_gp);
void bw_dsp_free(BwDsp *dsp);
void bw_dsp_reset(BwDsp *dsp);
void bw_dsp_bootstrap(BwDsp *dsp);

/* One frame: start-of-frame interrupt, then run until the program halts or
 * max_cycles pass. Returns the cycles used. */
uint32_t bw_dsp_run_frame(BwDsp *dsp, uint32_t max_cycles);

uint32_t bw_dsp_read(BwDsp *dsp, char space, uint32_t addr);
void bw_dsp_write(BwDsp *dsp, char space, uint32_t addr, uint32_t value);

/* Direct views of the memories, for the per-frame window sync (sizes in
 * 32-bit words; each holds one 24-bit DSP word). */
uint32_t *bw_dsp_mem(BwDsp *dsp, char space, uint32_t *words);

/* The engine when BW_DSP_ENGINE is unset: 1 the translated engine (with its
 * JIT), 0 xemu's interpreter. Call before the first bw_dsp_new. */
void bw_dsp_engine_default(int recomp);

/* ---- the whole state, engine-neutral (au-trace, docs/audio/trace_format.md) --
 *
 * Every field of the interpreter that carries from one instruction (or one
 * frame) to the next: xemu's dsp_core_t, plus DSPState's cycle carry-over,
 * frame-interrupt bits and DMA registers. All 32-bit words, in this order, so
 * a snapshot is this struct written as little-endian words. Narrower fields
 * hold their value (uint16 as 0..0xFFFF, int16 sign-extended). The memories
 * are the raw arrays: x[] is xram[] as stored, not as the DSP reads it
 * (X:0xC00-0xFFF and X:0x1400-0x17FF both read the mix buffer). */
#define BW_DSP_STATE_VERSION 1
#define BW_DSP_CORE_WORDS 160          /* the non-memory section, padded */

typedef struct BwDspState {
    /* core section: BW_DSP_CORE_WORDS words */
    uint32_t version;                  /* BW_DSP_STATE_VERSION */
    uint32_t is_gp;
    uint32_t pc;
    uint32_t regs[64];                 /* registers[DSP_REG_MAX] */
    uint32_t stack[2][16];             /* [0] SSH, [1] SSL */
    uint32_t loop_rep, pc_on_rep;
    uint32_t is_idle;                  /* halt requested */
    uint32_t cycle_count;
    uint32_t instr_cycle, cur_inst, cur_inst_len;   /* per instruction: not compared */
    uint32_t int_state, int_instr_fetch, int_save_pc, int_counter;
    uint32_t int_ipl_to_raise, int_pipeline_count;
    int32_t int_ipl[4];
    uint32_t int_pending[4];
    int32_t save_cycles;               /* DSPState.save_cycles (dsp_c_run's carry) */
    uint32_t interrupts;               /* DSPState.interrupts (the frame-start bit) */
    uint32_t dma_configuration, dma_control, dma_start_block, dma_next_block;
    uint32_t dma_error, dma_eol, dma_read_count;
    uint32_t reserved[BW_DSP_CORE_WORDS - 129];   /* zero */
    /* memories */
    uint32_t periph[128];              /* x:0xFFFF80.. (xemu never uses it: stays 0) */
    uint32_t x[4096];
    uint32_t y[2048];
    uint32_t p[4096];
    uint32_t mix[1024];                /* the mix buffer (X:0x1400 and X:0xC00) */
} BwDspState;

#define BW_DSP_STATE_WORDS (sizeof(BwDspState) / 4)
/* Word offsets of the memories in the state. */
#define BW_DSP_OFF_PERIPH BW_DSP_CORE_WORDS
#define BW_DSP_OFF_X (BW_DSP_OFF_PERIPH + 128)
#define BW_DSP_OFF_Y (BW_DSP_OFF_X + 4096)
#define BW_DSP_OFF_P (BW_DSP_OFF_Y + 2048)
#define BW_DSP_OFF_M (BW_DSP_OFF_P + 4096)

/* Copy the whole state out, or in. set_state replaces everything (and drops
 * the interpreter's decode cache); set_core replaces only the core section
 * and periph[], keeping the memories and the cache. */
void bw_dsp_get_state(BwDsp *dsp, BwDspState *s);
void bw_dsp_set_state(BwDsp *dsp, const BwDspState *s);
void bw_dsp_set_core(BwDsp *dsp, const BwDspState *s);
/* One raw memory word: 'X', 'Y', 'P', or 'M' (the mix buffer by index). No
 * aliasing; a P write drops that word's decode cache. */
void bw_dsp_poke(BwDsp *dsp, char space, uint32_t addr, uint32_t value);
int bw_dsp_is_gp(BwDsp *dsp);

/* ---- observation hooks (BW_DSP_TRACE; inert when none are set) -------------
 *
 * One process-wide table. io() sees every transfer the DSP makes through its
 * scratch or FIFO callbacks: a write (dir true) before the callback runs, a
 * read (dir false) after it, so ptr holds the DSP's data either way. kind 0 =
 * scratch (where = byte address), 1 = FIFO (where = FIFO index).
 * run_begin / run_end bracket bw_dsp_run_frame; event() follows a reset or a
 * bootstrap. */
typedef struct BwDspHooks {
    void (*io)(BwDsp *dsp, int kind, uint8_t *ptr, uint32_t where, size_t len, bool dir);
    void (*run_begin)(BwDsp *dsp, uint32_t max_cycles);
    void (*run_end)(BwDsp *dsp, uint32_t used);
    void (*event)(BwDsp *dsp, int ev);
} BwDspHooks;
enum { BW_DSP_EV_RESET = 1, BW_DSP_EV_BOOT = 2 };
void bw_dsp_set_hooks(const BwDspHooks *hooks);

#endif
