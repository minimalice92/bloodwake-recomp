/*
 * bwg_pace.h - the renderer's API-neutral core: the present's pacing, the
 * screen wait and the letterbox (bwg_pace.c). Every back end calls these;
 * the functions named bw_d3d11_* in bw_d3d11.h forward here.
 */
#ifndef BWG_PACE_H
#define BWG_PACE_H

#include <windows.h>

/* The present side's flags for the high frame rate's just-in-time frames
 * (lat-q; read by bw_hifps_hooks.c). The back end sets g_bw_flip_ready_ms,
 * g_bw_last_flip_waited and g_bw_flips_done at its flip, and
 * g_bw_present_waiting around its GPU-limit wait. */
extern volatile LONG g_bw_present_waiting, g_bw_last_flip_waited, g_bw_pace_waiting;
extern volatile LONG g_bw_cap_reached, g_bw_flips_done, g_bw_paced_n;
extern volatile double g_bw_flip_ready_ms;
/* gfx-lat11: what a pass that runs a game step needs before its present (ms;
 * the high frame rate sets it in the latency mode's head start). */
extern volatile double g_bwg_beat_need_ms;
/* Set by the waits during a flip (the pacing, the screen, the GPU limit). */
extern int bwg_flip_waited;

/* Configuration (main.c and the high frame rate, through bw_d3d11.h). */
void bwg_pace_config(unsigned hz);     /* presents no sooner than 1/hz s apart (0 = off) */
void bwg_cap_watch(int on);            /* watch whether the frame cap is reached */
void bwg_beat_auto(int on);            /* the head start's beat (BW_HIFPS_JIT=4) */

/* At the flip, before the present: no sooner than the schedule allows. */
void bwg_pace_present(void);
/* The schedule's next slot (qpc ms) once it has paced a present; its period. */
int bwg_present_due(double *due_ms);
double bwg_pace_period(void);

/* The screen wait on the swap chain's frame-latency waitable: 1 when a
 * buffer is free, 0 to drop this picture. */
int bwg_screen_wait(HANDLE waitable);

/* The picture's rectangle in a cw x ch window (4:3, wsk times wider). */
void bwg_letterbox(float wsk, unsigned cw, unsigned ch, float *x, float *y, float *w, float *h);

/* BW_GPU_STATS: the pacing's and the screen wait's lines. */
void bwg_pace_stats_line(void);

#endif
