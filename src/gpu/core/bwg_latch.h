/*
 * bwg_latch.h - the renderer's API-neutral core: I1, the CPU-write latch
 * (lane gfx-regen, docs/gfx/lanes/regen.md; design docs/gfx/lanes/ideas.md
 * section 6).
 *
 * A surface the GPU drew is seeded once from guest memory; after that the
 * GPU's copy is the truth. A CPU write into such a surface (the game locks
 * it through D3D_LockPixelContainer and writes the bytes) never reached the
 * GPU's copy. The D-011 wrapper sub_0012E4C0 (src/recomp_manual.c) reports
 * every lock here (bw_gpu_lock_note, on the game's threads); the graphics
 * thread drains them, marks the made surfaces whose bytes a lock crosses,
 * and uploads the marked rectangle from guest memory before the surface is
 * next drawn into, sampled or shown. The loading screen's progress bar is
 * written straight into the front buffer while no frame is presented, so
 * the graphics thread's idle point also uploads into the shown surface and
 * presents it again once no flip has come for BW_GPU_LATCH_IDLE_MS (50).
 *
 * BW_GPU_LATCH: unset or 1, on (every edition); 0, off (the wrapper runs the
 * game's body alone, as before); count, the locks are drained, matched and
 * counted (BW_GPU_LATCH_LOG=<n> logs the first n) but nothing is uploaded.
 * BW_GPU_LOCKREAD=1 (off by default): the forced read, a lock of a surface
 * the GPU drew since guest memory last had its bytes waits while the
 * graphics thread reads that surface back.
 */
#ifndef BWG_LATCH_H
#define BWG_LATCH_H

#include <stdint.h>
#include "bwg_surf.h"

/* Nonzero: the wrapper reports each lock (bw_gpu_lock_note). */
extern volatile int g_bw_lockwatch;
/* Locks published and not yet drained (one test on the graphics thread). */
extern volatile long g_bwg_latch_pub, g_bwg_latch_done;
#define BWG_LATCH_WAITING() (g_bwg_latch_pub != g_bwg_latch_done)

/* The back end's part. */
typedef struct {
    /* Write guest rows y0..y1-1, columns x0..x1-1 (logical pixels, at the
     * surface's cur_pitch) into s's GPU copy; r = x0, y0, x1, y1. */
    void (*upload)(BwgSurf *s, const uint32_t r[4]);
    /* Present s again, without a flip (the idle rule); 1 if it presented. */
    int (*represent)(BwgSurf *s);
    /* The forced read: read s back into guest memory now, waiting. */
    void (*read_now)(BwgSurf *s);
    /* The made surface the display scans out now (the live run only), or NULL. */
    BwgSurf *(*scanout)(void);
} BwgLatchOps;

/* The back end, at device set-up: its ops; reads the switches and sets
 * g_bw_lockwatch. */
void bwg_latch_init(const BwgLatchOps *ops);
/* 0 off, 1 on, 2 count. */
int bwg_latch_mode(void);

/* The game's threads (the wrapper). */
void bw_gpu_lock_note(uint32_t res, uint32_t level, uint32_t pitch, uint32_t bits,
                      const int32_t *rect, uint32_t flags);

/* The graphics thread. */
void bwg_latch_drain(void);
/* Before s is drawn into, sampled or presented: its marked rectangle up. */
#define BWG_LATCH_USE(s) do { if (BWG_LATCH_WAITING() || bwg_latch_marks) bwg_latch_use_(s); } while (0)
extern int bwg_latch_marks;             /* surfaces with a mark */
void bwg_latch_use_(BwgSurf *s);
/* At each flip, the surface shown. */
void bwg_latch_flipped(BwgSurf *shown);
/* The graphics thread's idle point (bw_ack_idle): the front-buffer rule and
 * the forced reads. Never while replaying (the replay thread runs the back
 * end; LATCHSHOW records stand in). */
void bwg_latch_idle(void);
/* A surface released or remade: its mark goes (its next seed reads guest
 * memory, CPU writes included). */
void bwg_latch_forget(BwgSurf *s);
/* Replaying a capture: a LOCK record (bw_gfxcap.c), a LATCHSHOW record. */
void bwg_latch_replay_lock(const uint8_t *p, uint32_t len);
void bwg_latch_replay_show(const uint8_t *p, uint32_t len);
/* The once-a-second line (BW_GPU_STATS), when anything moved. */
void bwg_latch_line(void);

#endif
