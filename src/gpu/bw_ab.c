/*
 * bw_ab.c - same-run A/B for GPU fixes (test aid).
 *
 * The game does not repeat pixel for pixel under BW_SEED (rain, lightning and
 * small camera offsets differ between two runs of one build: seen 2026-10-27,
 * runs *-ab-m27-on1 / -on2), so a fix cannot be proven by comparing two runs
 * frame by frame. Instead BW_GPU_AB=<fix> turns that one fix off on every
 * odd frame of a single run: adjacent frames then differ by the fix and by
 * one frame of motion only (tools/bw_shown.py pairs). <fix> is one of
 * points, polyoffset, ffpfog, conv, gamma, texrestore (T1: odd frames draw
 * the original textures in 3D), gpusprites (gfx-sprites: odd frames draw
 * point sprites the CPU way), or hideffp (a diagnostic: odd frames leave
 * the fixed-function batches out). The output shots' log line says which
 * half each shot is (bw_vfx_post.c).
 *
 * The frame number is the GPU back end's flip count (bw_d3d11.c bw_flip), so
 * the draws of a frame and its present see the same parity.
 */
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static volatile uint32_t s_frame;

void bw_ab_frame(uint32_t n)
{
    s_frame = n;
}

int bw_ab_off(const char *fix)
{
    static const char *which = (const char *)-1;
    if (which == (const char *)-1)
        which = getenv("BW_GPU_AB");
    return which && !strcmp(which, fix) && (s_frame & 1);
}

/* The current frame is an odd one (the fix off), for the shots' log line. */
int bw_ab_odd(void)
{
    return (int)(s_frame & 1);
}
