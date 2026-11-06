/*
 * bwg_core.h - the renderer's API-neutral core (docs/gfx/arch.md 3.2), its
 * shared basics.
 *
 * The core holds the policies every back end shares (D3D11 today, D3D12 and
 * the standalone replayer's engines later), lifted verbatim from bw_d3d11.c
 * (lane gfx-core, gate GC): the guest surfaces (bwg_surf), the texture cache
 * (bwg_tex), the state translation (bwg_state), the per-draw preparation
 * (bwg_draw) and the present's pacing (bwg_pace). It makes no graphics-API
 * call and depends only on the executor's interfaces, guest memory, the log,
 * and the edition and settings queries; the back end supplies the API
 * objects and the calls that make them.
 */
#ifndef BWG_CORE_H
#define BWG_CORE_H

#include <stddef.h>
#include <stdint.h>

extern ptrdiff_t g_xbox_mem_offset;
#define BWG_GUEST(va) ((uint8_t *)((uintptr_t)(va) + g_xbox_mem_offset))

/* The back end's frame number: 1 at the start, one more after each flip
 * (the texture cache's once-a-frame checks, slot ages, the logs). */
extern uint32_t bwg_gen;

/* Host milliseconds (the performance counter). */
double bwg_qpc_ms(void);

/* This boot's widescreen factor, bw_ws_k(), read once before the title runs
 * (bw_d3d11_ws_start); 1 in Original, the menus and -WIDESCREEN. */
extern float bwg_ws_k;

/* The guest block at va (xbox_ContiguousBlockSize); with BW_GFXCAP, recorded
 * or answered from the capture (an input of the seed and the readback). */
uint32_t bwg_block_size(uint32_t va);

/* Item 43's per-draw shortcuts (memos of the last lookup, the constant
 * shadow, the index of texture entries): BW_GPU_MEMO=0 turns them all off,
 * as before 0.2.5, to compare or to rule them out. */
int bwg_memo_on(void);

/* Widescreen (D5): inside the in-stream 2D markers, and the last BEGIN
 * marker's argument (BW_WS_WHAT_*); the back end's marker handler sets them. */
extern int bwg_ws_2d;
extern uint32_t bwg_ws_what;

#endif
