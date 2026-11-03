/*
 * bw_remaster_hooks.c - the edition hooks (D-015): D-011 wrappers that change
 * the game only in the editions after Original. Agent D5, 2026-10-27.
 *
 * tools/run_pipeline.py scans this file together with src/recomp_manual.c
 * (--exclude-manual gets their concatenation), so the rules of that file hold
 * here too:
 *   - a line-start "extern void sub_X_gen(void);" makes the generator emit
 *     the game's body as sub_X_gen, and a line-start "void sub_X(void)" is the
 *     wrapper every caller and the dispatch table reach;
 *   - "#if 0" is the only conditional the scan understands: switch at run
 *     time, never with #ifdef;
 *   - a hooked function keeps its sub_ name in the generated C (bw_names.py
 *     lists it as a hook).
 *
 * The rule for every wrapper here: the edition test comes first, and the
 * Original path is exactly `sub_X_gen(); return;`, so Original runs the
 * generated body with the same registers, stack and memory as an unhooked
 * build. A wrapper adds its change and nothing else; no game logic is
 * transcribed (D-005, D-011). Every address below was read in the generated
 * code (seen) before it was hooked; the evidence is named at each hook.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <windows.h>
#include "bw_edition.h"
#include "bw_widescreen.h"

extern ptrdiff_t g_xbox_mem_offset;
extern __declspec(thread) uint32_t g_eax, g_ecx, g_edx, g_esp;   /* RECOMP_TLS: TK-7 */

#define RH_MEM32(va) (*(volatile uint32_t *)((uintptr_t)(uint32_t)(va) + g_xbox_mem_offset))
#define RH_MEMF(va)  (*(volatile float *)((uintptr_t)(uint32_t)(va) + g_xbox_mem_offset))

/* ════════════════════════════════════════════════════════════════════════
 * BOAT_SPLASH_RANGE probe (R5's worked example; T3 in editions.md)
 *
 * BOAT_SPLASH_RANGE itself is an IMAGE fix (bw_edition.c: one float at
 * 0x174944). This wrapper is the evidence that it takes effect in play: it
 * runs the game's own test unchanged and, outside Original, counts the
 * answers. BoatSound_ShouldPlaySplash, 0x00013180 (seen): thiscall, ecx = the
 * boat-sound object, plain ret, al = 1 WaterSplash / 0 WaterPlunge; true at
 * once when [ecx+0x28] (the render item) is 0. It reads registers only.
 * ════════════════════════════════════════════════════════════════════════ */
extern void sub_00013180_gen(void);

static volatile long s_splash_calls, s_splash_true, s_splash_noitem;

static void splash_probe_summary(void)
{
    fprintf(stderr, "[ED] probe BoatSound_ShouldPlaySplash (%s): %ld calls, %ld splash "
            "(%ld of them without a render item), %ld plunge\n",
            bw_edition_name(bw_edition()), s_splash_calls, s_splash_true, s_splash_noitem,
            s_splash_calls - s_splash_true);
}

void sub_00013180(void)
{
    uint32_t item, air;
    long n;
    if (bw_is_original()) {
        sub_00013180_gen();
        return;
    }
    item = RH_MEM32(g_ecx + 0x28);               /* read before the body runs */
    air = RH_MEM32(g_ecx + 0xE0);                /* the landing's air time, ms */
    sub_00013180_gen();
    /* BOAT_LANDING_WEIGHT (agent F2, docs/day1/F2.md section 6): heavy
     * landings plunge. BoatSound_UpdateImpacts 0x13210 counts the air time in
     * ms at +0xE0 and plays WaterSmack below 400 ms; from 400 ms it asks this
     * test (seen), whose v is the cos of the boat's pitch, so with
     * BOAT_SPLASH_RANGE it splits splash and plunge by attitude only (in
     * runs, 24 of 26 landings splashed, the biggest ramp jumps included).
     * The game's own measure of a landing's weight is that air time: a level
     * landing after BW_LANDING_HEAVY_MS (default 1000 ms, about a 1.2 m drop;
     * ours) or more is answered plunge, so light landings splash and heavy
     * ones plunge; steep ones plunge as before. */
    if ((g_eax & 0xFFu) && item && bw_fix_enabled(BW_FIX_BOAT_LANDING_WEIGHT)) {
        static long s_heavy_ms = -1;
        if (s_heavy_ms < 0) {
            const char *e = getenv("BW_LANDING_HEAVY_MS");
            s_heavy_ms = (e && *e) ? strtol(e, NULL, 10) : 1000;
            if (s_heavy_ms < 400)
                s_heavy_ms = 400;
        }
        if ((long)air >= s_heavy_ms) {
            g_eax &= ~0xFFu;
            bw_fix_note_hit(BW_FIX_BOAT_LANDING_WEIGHT);
        }
    }
    n = InterlockedIncrement(&s_splash_calls);
    if (n == 1)
        bw_edition_add_summary(splash_probe_summary);
    if (g_eax & 0xFFu) {
        InterlockedIncrement(&s_splash_true);
        if (!item)
            InterlockedIncrement(&s_splash_noitem);
        else if (bw_fix_enabled(BW_FIX_BOAT_SPLASH_RANGE))
            bw_fix_note_hit(BW_FIX_BOAT_SPLASH_RANGE);   /* a splash the fix made possible */
    }
}

/* ════════════════════════════════════════════════════════════════════════
 * WIDESCREEN (bw_widescreen.h; docs/remaster/widescreen.md)
 *
 * Every wrapper below is a pass-through while bw_ws_k() is 1: Original, the
 * title and menu boots, BW_FIX=-WIDESCREEN, BW_ASPECT=4:3.
 * ════════════════════════════════════════════════════════════════════════ */

/* Guest layout (seen in the generated code, 2026-10-27, and in R3's dumps):
 * renderer cameras 0..4 at 0x1C70F0 + n*0xB0: projection at +0x40, viewport
 * width/height at +0x98/+0x9C, view-params pointer at +0xA8. Game view v
 * (camera-system block v, 0x21C each) is drawn by renderer camera v+1
 * (Camera_WorldToScreen and Camera_LoadTuning both index it so). */
#define RCAM_BASE         0x001C70F0u
#define RCAM_STRIDE       0xB0u
#define RCAM_PROJ         0x40u
#define RCAM_VP_W         0x98u
#define RCAM_VP_H         0x9Cu
#define RCAM_PARAMS       0xA8u
#define CAM_STRIDE        0x21Cu        /* camera-system block per game view */
#define CAM_PARAMS        0x108u        /* the view-params block inside it */
#define CAM_FAR           0x144u        /* params +0x3C */
#define CAM_FOV           0x148u        /* params +0x40: horizontal FOV, radians */
#define CAM_INV_COS       0x150u        /* params +0x48: 1/cos(fov/2), culling */
#define CAM_TAN           0x154u        /* params +0x4C: tan(fov/2), culling */
#define CAM_INV_ASPECT    0x158u        /* params +0x50: H/W of the view, culling */
#define CAMSYS_DEFAULT    0x001CE4A0u   /* R3's dump; the hooks prefer the live ecx */
#define RENDER_CELLS_X    0x288u        /* render manager: world grid size */
#define RENDER_CELLS_Y    0x28Cu
#define GRID_HALF_M       800.0f        /* [0x175380], RenderMgr_DrawScene */
#define BOOST_TAN_HALF    1.4281f       /* tan(55 deg): the 110 deg boost FOV */

static uint32_t s_camsys = CAMSYS_DEFAULT;

/* Append one marker to D3D's pushbuffer through the game's own
 * D3DDevice_SetRenderState_Simple (0x00126300: fastcall, ecx = method header,
 * edx = value; appends the two words at the push pointer [0x133F70], making
 * space through D3D_MakeSpace when full; plain ret; seen), so it lands in
 * stream order with the draws around it. The caller's eax/ecx/edx are kept. */
extern void D3DDevice_SetRenderState_Simple_00126300(void);
/* Built without names/bw_names.json (the public repo, D-019) the generated C
 * names this function by its address alone ("sub" "_" + 00126300): the linker
 * falls back to that name only when the one above is missing. Written split so
 * the hook scans (tools/bw_names.py, the recompiler's) don't count it as a
 * reference, which would rename the function in the named build. */
#pragma comment(linker, "/alternatename:D3DDevice_SetRenderState_Simple_00126300=sub" "_00126300")
static volatile long s_ws_emitted[4];

static void ws_emit_summary(void)
{
    fprintf(stderr, "[WS] markers written by the game: BEGIN %ld END %ld 3D_43 %ld\n",
            s_ws_emitted[1], s_ws_emitted[2], s_ws_emitted[3]);
}

static void ws_emit(uint32_t param)
{
    uint32_t sa = g_eax, sc = g_ecx, sd = g_edx, op = (param >> 8) & 0xFFu;
    if (op < 4 && InterlockedIncrement(&s_ws_emitted[op]) == 1 && op == 1)
        bw_edition_add_summary(ws_emit_summary);
    /* Once. Before TK-38 (the pushbuffer walker lost commands at ring wraps;
     * fixed by D9, main 831bb19) about 1 in 1,000 markers went missing (runs
     * ...-mk10/-mk12a); with the fix, written and received counts match
     * exactly (runs ...-tk38a/-tk38b). The back end's set/clear flag, cleared
     * at every flip, stays as the safety net. */
    g_esp -= 4;
    RH_MEM32(g_esp) = 0x00126300u;          /* a return address for its ret to pop */
    g_ecx = BW_WS_NOP_HEADER;
    g_edx = param;
    D3DDevice_SetRenderState_Simple_00126300();
    g_eax = sa; g_ecx = sc; g_edx = sd;
}

/* ---- 1. the projection: Hor+ ----------------------------------------------
 * Math_MatrixPerspectiveOffCenterLH 0x000DFA79 (seen): stdcall
 * (out, l, r, b, t, zn, zf), ret 0x1C. Its only caller, Viewport_Apply
 * 0x00039970, builds renderer camera n's projection (n >= 1) into
 * 0x1C7130 + n*0xB0 with l/r = (-xs - ox, xs - ox) * zn * tan(fov/2), the FOV
 * from the view-params block +0x40 (the longer viewport axis: horizontal-
 * locked). Scaling l and r by k widens the horizontal field; the vertical
 * field, the projection-centre offset in NDC and the depth terms stay. Every
 * game view, split screen and in-engine cutscene goes through here. */
extern void sub_000DFA79_gen(void);
void sub_000DFA79(void)
{
    float k = bw_ws_k();
    uint32_t out, off;
    if (k <= 1.0f) {
        sub_000DFA79_gen();
        return;
    }
    out = RH_MEM32(g_esp + 4);
    off = out - (RCAM_BASE + RCAM_PROJ);
    if (out > RCAM_BASE + RCAM_PROJ && off % RCAM_STRIDE == 0 && off / RCAM_STRIDE <= 4) {
        RH_MEMF(g_esp + 8)  *= k;                   /* left  */
        RH_MEMF(g_esp + 12) *= k;                   /* right */
    }
    sub_000DFA79_gen();
}

/* ---- 2. culling: widen the frustum the game tests against -----------------
 * RenderItem_IsVisible 0x308E0, Camera_IsPointVisible 0x37540 and the
 * ocean's per-frame copy (Ocean_Render 0x38EC0 copies params +0x38..+0x50 of
 * the current camera; seen) all test |x| <= z*tan + r/cos and
 * |y| <= z*tan*(H/W) + r/cos with params +0x4C, +0x48, +0x50. Camera_SetFov
 * rewrites +0x48/+0x4C from the FOV every frame; Camera_LoadTuning sets +0x50
 * from the viewport at level load (both seen). Recomputed from the FOV and
 * the viewport each time, so this is idempotent; the vertical test is
 * unchanged (tan*k * (H/W)/k). */
static void ws_widen_view(uint32_t camsys, uint32_t view)
{
    uint32_t blk = camsys + view * CAM_STRIDE;
    uint32_t rc = RCAM_BASE + (view + 1) * RCAM_STRIDE;
    float k = bw_ws_k();
    float fov = RH_MEMF(blk + CAM_FOV);
    float w = (float)RH_MEM32(rc + RCAM_VP_W), h = (float)RH_MEM32(rc + RCAM_VP_H);
    float t;
    if (!(fov > 0.0f && fov < 3.1416f) || !(w > 0.0f) || !(h > 0.0f))
        return;
    t = tanf(fov * 0.5f) * k;
    RH_MEMF(blk + CAM_TAN) = t;
    RH_MEMF(blk + CAM_INV_COS) = sqrtf(1.0f + t * t);
    RH_MEMF(blk + CAM_INV_ASPECT) = (h / w) / k;
}

/* Camera_SetFov 0x00076820 (seen): thiscall (ecx = camera system; view n,
 * fov), ret 8. Every player, every frame. */
extern void sub_00076820_gen(void);
void sub_00076820(void)
{
    uint32_t camsys, view;
    if (bw_ws_k() <= 1.0f) {
        sub_00076820_gen();
        return;
    }
    camsys = g_ecx;
    view = RH_MEM32(g_esp + 4);
    sub_00076820_gen();
    if (view < 4) {
        s_camsys = camsys;
        ws_widen_view(camsys, view);
    }
}

/* Camera_LoadTuning 0x00078650 (seen): thiscall (ecx = camera system; view
 * count), ret 4. Level load: the same widening for every view, so frames
 * before the first SetFov are right too. */
extern void sub_00078650_gen(void);
void sub_00078650(void)
{
    uint32_t camsys, count, v;
    if (bw_ws_k() <= 1.0f) {
        sub_00078650_gen();
        return;
    }
    camsys = g_ecx;
    count = RH_MEM32(g_esp + 4);
    sub_00078650_gen();
    s_camsys = camsys;
    for (v = 0; v < count && v < 4; v++)
        ws_widen_view(camsys, v);
}

/* ---- 3. the world-projected HUD (reticle, lock-on, boat markers) ----------
 * Camera_WorldToScreen 0x000762D0 (seen): thiscall (view n, vec3 *io), ret 8,
 * al = in front. It maps a world point through renderer camera n+1's
 * projection (now k times wider) to ((x+1)*320, 240 - 240y). The HUD that
 * consumes it is pillarboxed in Original's 640 space, where the side regions
 * are x < 0 and x > 640: x' = 320 + (x - 320) * k puts the marker back on the
 * boat (maths checked by R3 for 1P, 2P and quadrant views). */
extern void sub_000762D0_gen(void);
void sub_000762D0(void)
{
    float k = bw_ws_k();
    uint32_t view, io;
    if (k <= 1.0f) {
        sub_000762D0_gen();
        return;
    }
    view = RH_MEM32(g_esp + 4);
    io = RH_MEM32(g_esp + 8);
    sub_000762D0_gen();                     /* g_eax (al) = "in front": kept */
    if (view < 4 && io)
        RH_MEMF(io) = 320.0f + (RH_MEMF(io) - 320.0f) * k;
}

/* ---- 4. 2D markers for the back end ---------------------------------------
 * RenderMgr_RenderFrame 0x2DEE0 (seen) draws: sorted layer 0; per view
 * [Viewport_Apply, opaque layer v, the 3D, sorted layer v]; then the full-
 * screen camera 0 with sorted layer 7 and opaque layer 0. The overlay
 * elements (Overlay_DrawText, _DrawSolidRect, _DrawImageElement) only queue
 * quads in 640x480 space; Overlay_FlushBatches 0x00028B30 (seen: ecx = the
 * overlay system, no arguments, plain ret; its only callers are the two layer
 * functions, at their end) draws them. The sorted layers also carry depth-
 * sorted 3D (transparent effects drawn with the view's camera: seen in run
 * 20261027-0247xx-mk4/play3, model-space vertex programs inside the layer),
 * which must stretch with the rest of the 3D. So only the flush is 2D. */
extern void sub_00028B30_gen(void);
void sub_00028B30(void)
{
    if (bw_ws_k() <= 1.0f) {
        sub_00028B30_gen();
        return;
    }
    ws_emit(BW_WS_MARK(BW_WS_OP_2D_BEGIN, BW_WS_WHAT_OVERLAY));
    sub_00028B30_gen();
    ws_emit(BW_WS_MARK(BW_WS_OP_2D_END, BW_WS_WHAT_OVERLAY));   /* keeps eax, ecx, edx */
}

/* Movie_DrawQuad 0x000D6050 (seen): four pre-transformed vertices through
 * DrawVerticesUP, ret 0x20. A Bink frame is a 640x480 picture: pillarboxed,
 * never stretched. */
extern void sub_000D6050_gen(void);
void sub_000D6050(void)
{
    if (bw_ws_k() <= 1.0f) {
        sub_000D6050_gen();
        return;
    }
    ws_emit(BW_WS_MARK(BW_WS_OP_2D_BEGIN, BW_WS_WHAT_MOVIE));
    sub_000D6050_gen();
    ws_emit(BW_WS_MARK(BW_WS_OP_2D_END, BW_WS_WHAT_MOVIE));   /* keeps eax, ecx, edx */
}

/* ---- 5. the static world: widen the grid square ---------------------------
 * RenderMgr_DrawScene 0x2DCD0 (seen) walks only the 100 m cells within
 * +-800 m of the camera's look-at point, clamped to the grid, and passes the
 * range to World_DrawGridCells 0x00036B70 (thiscall ecx = render manager;
 * x0, y0, x1, y1, nx; ret 0x14) and RenderMgr_CollectVisible 0x0002D720
 * (thiscall; x0, y0, x1, y1; ret 0x10). At 4:3 the square just covers the
 * 90 degree frustum to the far plane; a wider view reaches far*k*tan(half)
 * sideways. The extra cells are still frustum-culled item by item, so this
 * costs walk time only. (The 800 itself is also an LOD distance in
 * World_DrawGridCells and PFX_RenderEffect, so the constant is not touched.) */
static int ws_extra_cells(void)
{
    float k = bw_ws_k(), far_m, lat;
    if (k <= 1.0f)
        return 0;
    far_m = RH_MEMF(s_camsys + CAM_FAR);       /* view 0's far plane (fog-driven) */
    if (!(far_m > 0.0f && far_m < 5000.0f))
        far_m = 850.0f;
    lat = far_m * k * BOOST_TAN_HALF;
    return lat > GRID_HALF_M ? (int)ceilf((lat - GRID_HALF_M) / 100.0f) : 0;
}

static void ws_widen_cells(uint32_t args, uint32_t nx, uint32_t ny)
{
    int m = ws_extra_cells();
    int32_t x0 = (int32_t)RH_MEM32(args + 0), y0 = (int32_t)RH_MEM32(args + 4);
    int32_t x1 = (int32_t)RH_MEM32(args + 8), y1 = (int32_t)RH_MEM32(args + 12);
    if (!m || !nx || !ny || nx > 4096 || ny > 4096)
        return;
    x0 -= m; y0 -= m; x1 += m; y1 += m;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int32_t)nx - 1) x1 = (int32_t)nx - 1;
    if (y1 > (int32_t)ny - 1) y1 = (int32_t)ny - 1;
    RH_MEM32(args + 0) = (uint32_t)x0; RH_MEM32(args + 4) = (uint32_t)y0;
    RH_MEM32(args + 8) = (uint32_t)x1; RH_MEM32(args + 12) = (uint32_t)y1;
}

extern void sub_00036B70_gen(void);
void sub_00036B70(void)
{
    if (bw_ws_k() <= 1.0f) {
        sub_00036B70_gen();
        return;
    }
    ws_widen_cells(g_esp + 4, RH_MEM32(g_ecx + RENDER_CELLS_X), RH_MEM32(g_ecx + RENDER_CELLS_Y));
    sub_00036B70_gen();
}

extern void sub_0002D720_gen(void);
void sub_0002D720(void)
{
    if (bw_ws_k() <= 1.0f) {
        sub_0002D720_gen();
        return;
    }
    ws_widen_cells(g_esp + 4, RH_MEM32(g_ecx + RENDER_CELLS_X), RH_MEM32(g_ecx + RENDER_CELLS_Y));
    sub_0002D720_gen();
}

/* ---- 6. the HUD at the screen edges (WS_HUD_EDGES) ------------------------
 * Each HUD element keeps its view's safe rectangle (floats, written by its
 * Init from Overlays_Load's per-player-count tables) and places its parts
 * from it in SetLayout (seen in the generated code, 2026-10-27; tables read
 * from the XBE):
 *   HudStatus_SetLayout   0x00086C90  rect +0x04 left .. +0x0C right: parts at
 *                                     right - 88 (or 66) + table x: right edge
 *   HudRadar_SetLayout    0x000851A0  rect +0x08 left .. +0x10 right: centre at
 *                                     left + 64: left edge
 *   HudScore_SetLayout    0x00083930  rect +0x08 left: left + 64/39: left edge
 *   HudHoldTimer_SetLayout 0x000861B0 rect +0x08 left: left + 100..39: left edge
 * all thiscall (mode), ret 4. The wrapper widens the rectangle's left and
 * right about its view's centre by k, so the parts land at the wide view's
 * edges with the same proportional margin (48/640 = 7.5% of the width), runs
 * the game's own SetLayout, and puts the two floats back bit for bit. The
 * parts keep the positions SetLayout gave them (the Update functions don't
 * read the rectangle; seen). A view's centre is 320 for a full-width view,
 * 160/480 for a split-screen quadrant (the safe margin is on outer edges
 * only, so a quadrant's rectangle ends at 320). Mode 2 hides the element. */
static float ws_view_centre(float l, float r)
{
    if (r <= 320.5f)
        return 160.0f;
    if (l >= 319.5f)
        return 480.0f;
    return 320.0f;
}

static void ws_hud_layout(void (*body)(void), uint32_t lf, uint32_t rf)
{
    uint32_t el = g_ecx, lbits, rbits;
    float k = bw_ws_k(), l, r, c;
    if (!bw_fix_enabled(BW_FIX_WS_HUD_EDGES)) {
        body();
        return;
    }
    lbits = RH_MEM32(el + lf);
    rbits = RH_MEM32(el + rf);
    l = RH_MEMF(el + lf);
    r = RH_MEMF(el + rf);
    if (!(r > l) || l < -1.0f || r > 641.0f) {   /* not a rectangle we know */
        body();
        return;
    }
    c = ws_view_centre(l, r);
    RH_MEMF(el + lf) = c - (c - l) * k;
    RH_MEMF(el + rf) = c + (r - c) * k;
    body();
    RH_MEM32(el + lf) = lbits;
    RH_MEM32(el + rf) = rbits;
}

extern void sub_00086C90_gen(void);
void sub_00086C90(void)                     /* HudStatus_SetLayout: right edge */
{
    if (bw_ws_k() <= 1.0f) {
        sub_00086C90_gen();
        return;
    }
    ws_hud_layout(sub_00086C90_gen, 0x04, 0x0C);
}

extern void sub_000851A0_gen(void);
void sub_000851A0(void)                     /* HudRadar_SetLayout: left edge */
{
    if (bw_ws_k() <= 1.0f) {
        sub_000851A0_gen();
        return;
    }
    ws_hud_layout(sub_000851A0_gen, 0x08, 0x10);
}

extern void sub_00083930_gen(void);
void sub_00083930(void)                     /* HudScore_SetLayout: left edge */
{
    if (bw_ws_k() <= 1.0f) {
        sub_00083930_gen();
        return;
    }
    ws_hud_layout(sub_00083930_gen, 0x08, 0x10);
}

extern void sub_000861B0_gen(void);
void sub_000861B0(void)                     /* HudHoldTimer_SetLayout: left edge */
{
    if (bw_ws_k() <= 1.0f) {
        sub_000861B0_gen();
        return;
    }
    ws_hud_layout(sub_000861B0_gen, 0x08, 0x10);
}
