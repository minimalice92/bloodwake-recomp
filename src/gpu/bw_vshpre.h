/* bw_vshpre.h - the game's vertex programs compiled at load time (lane
 * gfx-regen, docs/gfx/lanes/regen.md; see bw_vshpre.c). */
#ifndef BW_VSHPRE_H
#define BW_VSHPRE_H

#include <stddef.h>
#include <stdint.h>

/* Nonzero: the D-011 wrapper sub_00127820 reports each vertex shader the
 * game creates (BW_GPU_VSH_PRE=0 turns it to 0 at the first call). */
extern volatile int g_bw_vshwatch;
/* The wrapper, after D3DDevice_CreateVertexShader's body (game thread):
 * the declaration's and the function's guest addresses, its result. */
void bw_gpu_vsh_created(uint32_t decl, uint32_t function, uint32_t hr);

/* The back end, once its shader cache is loaded (bw_pscache_init): the
 * workers start. sprites: point batches draw on the GPU (GPU_SPRITES), so
 * each program that writes a point size gets its point variant too. */
void bw_vshpre_start(int sprites);
/* A known shader text to make with them (kind 'f': a fixed-function shape). */
void bw_vshpre_add_text(const char *src, size_t len, char kind);

/* The device thread, at a vertex shader's first sight (before its cache
 * lookup): 0 none of the load-time work is this text; 1 it was, and is done
 * (waited for when wait is set and it was compiling: *ms, the wait); 2 it
 * was queued and not started: claimed, the caller makes it; 3 it is
 * compiling and wait is 0. */
int bw_vshpre_wait(const char *src, size_t len, int ws, int wait, double *ms);
/* Jobs not yet finished (the load-time batch is under way). */
int bw_vshpre_busy(void);
/* The summary once the batch is done, and on request (BW_GPU_STATS). */
void bw_vshpre_line(int force);

#endif
