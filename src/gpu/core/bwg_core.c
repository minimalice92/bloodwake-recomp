/*
 * bwg_core.c - the renderer's API-neutral core: its shared basics (see
 * bwg_core.h). Lifted verbatim from bw_d3d11.c (lane gfx-core, gate GC).
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

#include "bwg_core.h"
#include "../bw_gfxcap.h"               /* BW_GFXCAP: the allocator's answers are an input */

extern uint32_t xbox_ContiguousBlockSize(uint32_t va);

uint32_t bwg_gen = 1;      /* frame number */
float bwg_ws_k = 1.0f;     /* bw_ws_k() for this boot, read once (bw_d3d11_ws_start) */
int bwg_ws_2d;             /* inside 2D markers */
uint32_t bwg_ws_what;      /* the last BEGIN marker's argument (BW_WS_WHAT_*) */

uint32_t bwg_block_size(uint32_t va)
{
    uint32_t r = xbox_ContiguousBlockSize(va);
    return g_bw_gfxcap ? bw_gfxcap_block(va, r) : r;
}

int bwg_memo_on(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("BW_GPU_MEMO");
        v = !(e && e[0] == '0');
        if (!v)
            fprintf(stderr, "[GPU-D3D11] per-draw lookup shortcuts off (BW_GPU_MEMO=0)\n");
    }
    return v;
}

double bwg_qpc_ms(void)
{
    static double inv;
    LARGE_INTEGER t;
    if (!inv) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        inv = 1000.0 / (double)f.QuadPart;
    }
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * inv;
}
