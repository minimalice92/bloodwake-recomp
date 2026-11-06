/*
 * bwg_gate.h - the latency mode's shared parts (docs/gfx/arch.md 3.3, 3.8
 * and 4; lane gfx-lat11): what the GPU has finished, its own time per frame,
 * the measured display horizon, and the questions the one wait gate before
 * the game pass asks (bw_hifps_hooks.c, jit_wait). API-neutral: the back end
 * reports its flips, the GPU's done events and timestamps and the screen's
 * times; the core keeps them and answers.
 *
 * Remastered's high frame rate only (BW_HIFPS_LATMODE, default 0 = parity:
 * every wait stays inside the flip, exactly as before). Nothing here runs in
 * parity mode, so Original never reaches it.
 */
#ifndef BWG_GATE_H
#define BWG_GATE_H

#include <windows.h>
#include <stdint.h>

/* The mode (bw_hifps_boot): 0 parity, 1 the gate, 2 the gate with the head
 * start; limit: GPU frames a pass may start with unfinished (1 by default). */
void bwg_gate_config(int mode, int limit, int gpu_aim);
int bwg_gate_mode(void);
int bwg_gate_limit(void);
int bwg_gate_gpu_aim(void);

/* The back end, NV2A thread. Each flip is numbered 1, 2, ... in order; after
 * its present (or none) the back end marks it on the GPU (a fence) and says
 * so here, with the present call's time (qpc ms). live: the back end can
 * tell when the GPU finishes flips (0 turns the gate's GPU condition off). */
void bwg_gate_live(int live);
void bwg_gate_submitted(uint32_t flip, double present_ms);
/* GPU timestamps for a flip, on the GPU's clock in ms (the back end reads
 * them a frame or two later): begin = the GPU reached the frame's first
 * command, end = its last (the fence's mark). */
void bwg_gate_gpu_times(uint32_t flip, double begin_ms, double end_ms);
/* The screen showed this flip at screen_ms (qpc ms; on screen only). */
void bwg_gate_screen(uint32_t flip, double screen_ms);

/* The GPU-done watcher (any thread): the GPU finished every flip up to
 * this one; done_ms is when the watcher saw it (qpc ms). */
void bwg_gate_gpu_done(uint32_t flip, double done_ms);

/* The gate (game thread). */
uint32_t bwg_gate_unfinished(void);    /* flips submitted, not finished by the GPU */
uint32_t bwg_gate_done_count(void);    /* the last flip the GPU finished */
/* When the GPU should have finished every submitted flip (qpc ms), from the
 * last finished flip and the GPU's measured time per frame; 0 when it has
 * finished them all (or nothing is known yet). */
double bwg_gate_gpu_free_ms(void);
/* The GPU's own time per frame (ms, the median of the last 32; 0 until
 * known): its timestamps when the back end has them, else the done events'
 * spacing while it was busy. */
double bwg_gate_gpu_period(void);
/* Wait until the GPU finishes another flip or timeout_ms passes; 1 if it did. */
int bwg_gate_wait_progress(DWORD timeout_ms);
/* The measured horizon: present -> screen (on screen) or -> the GPU done
 * (off screen), median of the last 32 flips, ms; 0 until known. */
double bwg_gate_horizon_ms(void);

/* BW_TEST_GPUSTALL (test aid): the stretch's per-frame GPU time now, else 0. */
double bwg_gate_test_stall_ms(void);

/* BW_GPU_STATS: a line a second (the gate's waits are the hook's line). */
void bwg_gate_stats_line(void);

#endif
