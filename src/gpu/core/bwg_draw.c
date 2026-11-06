/*
 * bwg_draw.c - the renderer's API-neutral core: the per-draw preparation
 * without API calls (bwg_draw.h).
 *
 * Lifted verbatim from bw_d3d11.c (lane gfx-core, gate GC: the same
 * decisions in the same order, the same floating-point expressions): the
 * widescreen markers, 2D tests and remap (D5), the combiner key with its VFX
 * bits and alpha-to-coverage, the key trim (item 41), the constant blocks'
 * values, the overlay (HUD) test, the title's anti-aliasing resolve filter,
 * the ocean cull test aid and the raw vertex fetch's formats and key (P1).
 */
#include <windows.h>
#include <dxgiformat.h>                 /* the vertex fetch's formats (D3D11's and D3D12's) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bwg_core.h"
#include "bwg_state.h"
#include "bwg_surf.h"
#include "bwg_tex.h"
#include "bwg_draw.h"
#include "../bw_vfx.h"                  /* the suite's settings (VFX, D9; inactive in Original) */
#include "../../bw_widescreen.h"        /* the markers' ops and kinds (D5) */
#include "../../bw_edition.h"           /* bw_edition_add_summary, bw_edition_at_least */
#include "../bw_gfxcap.h"               /* g_bw_gfxcap: replaying (gfx-sprites' measuring aid) */

extern int bw_ab_off(const char *fix);      /* BW_GPU_AB (bw_ab.c) */

/* The filter of the title's last anti-aliasing resolve (xdraw_prepare). */
uint32_t bwg_resolve_filter;
int bwg_resolve_seen;

/* ---- widescreen: 2D markers, the per-batch x remap -------------------------
 *
 * The game-side hooks (bw_remaster_hooks.c) write a tagged NV097 NOP before
 * and after each overlay layer and each movie frame; the carried executor
 * hands them here in stream order (bw_set_nop_marker_handler). Inside them a
 * draw is 2D: pillarboxed around its view's centre, so it keeps its shape,
 * except an untextured quad covering the whole view (a fade, a flash), which
 * stretches to fill the wide view. Everything else is 3D and stretches
 * (wsx = 1, 0); the game's own projection is already k times wider (Hor+).
 * All of it runs on the executor's thread, as the draws do. */
extern void bw_set_nop_marker_handler(void (*fn)(uint32_t));

/* 2D is a flag, not a nesting depth: BEGIN sets it, END clears it, and every
 * flip clears it too (no frame ends inside an overlay flush), so a marker
 * that goes missing can't turn later 3D into 2D for longer than one frame.
 * A lost END was seen (run ...-mk10, flip 301) before TK-38 was fixed. */
static int s_ws_cut43;          /* this view's 3D is 4:3 (the 4:3-cutscene option) */
static int s_ws_full;           /* this batch: an untextured full-view 2D quad */
static unsigned s_ws_markers, s_ws_2d_draws;
static unsigned s_ws_rx[4], s_ws_unpaired;   /* markers received per op; frames ending in 2D */
static int s_ws_log = -1;       /* BW_WS_LOG=<n>: the first n 2D batches, for checking */

static void ws_marker(uint32_t p)
{
    uint32_t op = (p >> 8) & 0xFF, arg = p & 0xFF;
    if (!s_ws_markers++)
        fprintf(stderr, "[WS] first in-stream marker %08X\n", p);
    if (s_ws_log > 0 && getenv("BW_WS_LOG_MARKERS"))
        fprintf(stderr, "[WS] f%u marker op %u arg %u in2d %d\n", bwg_gen, op, arg, bwg_ws_2d);
    if (op < 4)
        s_ws_rx[op]++;
    if (op == BW_WS_OP_2D_BEGIN) {
        bwg_ws_2d = 1;
        bwg_ws_what = arg;
    } else if (op == BW_WS_OP_2D_END) {
        bwg_ws_2d = 0;
    } else if (op == BW_WS_OP_3D_43)
        s_ws_cut43 = arg != 0;
}

/* At each flip: no frame ends in 2D; a summary every 30 s when logging. */
void bwg_ws_frame_end(void)
{
    static unsigned next = 900;
    if (bwg_ws_2d) {
        s_ws_unpaired++;
        bwg_ws_2d = 0;
    }
    if (bwg_ws_k > 1.0f && bwg_gen >= next) {
        next = bwg_gen + 900;
        fprintf(stderr, "[WS] frame %u: markers received BEGIN %u END %u, unpaired %u, "
                "2D draws %u\n", bwg_gen, s_ws_rx[1], s_ws_rx[2], s_ws_unpaired, s_ws_2d_draws);
    }
}

static void ws_summary(void)
{
    fprintf(stderr, "[WS] markers received: BEGIN %u END %u, frames ending in 2D %u, "
            "2D draws %u\n", s_ws_rx[1], s_ws_rx[2], s_ws_unpaired, s_ws_2d_draws);
}

/* main.c (through bw_d3d11_ws_start), after bw_ws_init() and before the game
 * runs: this boot's factor.
 * k == 1 (Original, the menus, -WIDESCREEN) registers nothing. */
void bwg_ws_start(void)
{
    bwg_ws_k = bw_ws_k();
    if (bwg_ws_k > 1.0f) {
        bw_set_nop_marker_handler(ws_marker);
        bw_edition_add_summary(ws_summary);
    }
}

/* The surface's width in logical pixels: the bound surface's guest width
 * (what the shaders normalise by) once it is bound, else the width get_surf
 * will give it. */
static float ws_logical_w(const Nv2aSurface *s)
{
    uint32_t sx = s->aa_sx ? s->aa_sx : 1;
    uint32_t bpp = s->bytes_per_pixel ? s->bytes_per_pixel : 4;
    uint32_t w = s->pitch && s->pitch / bpp >= s->width ? s->pitch / bpp : s->width;
    if (bwg_cur && bwg_cur->va == s->color_va)
        w = bwg_cur->w;
    return (float)w / (float)sx;
}

/* The current view's horizontal extent in logical pixels: the window clip D3D
 * sets at every SetViewport (as apply_scissor reads it), else the surface. */
static void ws_view(const Nv2aSurface *s, float *x0, float *x1)
{
    uint32_t h = bw_backend_reg(0x02C0);
    float lw = ws_logical_w(s);
    *x0 = 0.0f;
    *x1 = lw;
    if (h && bw_backend_reg(0x02B4) == 0) {
        float a = (float)(h & 0xFFF), b = (float)((h >> 16) & 0xFFF) + 1.0f;
        if (b > a) {
            *x0 = a;
            *x1 = b < lw ? b : lw;
        }
    }
}

/* lo..hi (logical pixels, or surface pixels when real) covers the view. */
static int ws_covers_view(const Nv2aSurface *s, float lo, float hi, int real)
{
    float x0, x1, sx = s->aa_sx ? (float)s->aa_sx : 1.0f;
    if (real) {
        lo /= sx;
        hi /= sx;
    }
    ws_view(s, &x0, &x1);
    return lo <= x0 + 1.0f && hi >= x1 - 1.0f;
}

/* The view's height in logical pixels (window clip, else 480). */
static float ws_view_h(const Nv2aSurface *s)
{
    uint32_t v = bw_backend_reg(0x02E0);
    (void)s;
    if (v && bw_backend_reg(0x02B4) == 0) {
        float a = (float)(v & 0xFFF), b = (float)((v >> 16) & 0xFFF) + 1.0f;
        if (b > a)
            return b - a;
    }
    return 480.0f;
}

/* A full-view 2D quad is a backdrop (stretch it to the wide view) unless it
 * shows a picture. A picture (a loading screen, a menu background) maps its
 * texels about square onto the screen, at a texel or so per pixel. A backdrop
 * is untextured, or a swatch (at most one texel per 10 pixels across), or a
 * texture squeezed far out of square. The pause screen's darkening is both:
 * a 640x480 quad over 15 x 120 texels of a 512x1024 overlay page, and every
 * 4th frame over the whole page (seen, runs ...-p15, -p17uw). u/v ranges in
 * texels, or in 0..1 when normalized. */
static int ws_backdrop(const Nv2aSurface *s, int textured, int normalized, uint32_t tw,
                       uint32_t th, float u0, float u1, float v0, float v1)
{
    float su = u1 - u0, sv = v1 - v0, x0, x1, vw, vh, r;
    if (!textured)
        return 1;
    if (normalized) {
        su *= (float)tw;
        sv *= (float)th;
    }
    ws_view(s, &x0, &x1);
    vw = x1 - x0;
    vh = ws_view_h(s);
    if (su <= 8.0f || su <= 0.1f * vw)
        return 1;                               /* a swatch */
    if (sv <= 0.0f || vh <= 0.0f)
        return 0;
    r = (su / sv) / (vw / vh);                  /* 1: square texels on screen */
    return r < 0.8f || r > 1.25f;
}

static float s_ws_span_u, s_ws_span_v;   /* the last full-view batch's texel span (log) */

/* A 2D draw that samples a widened surface is a copy of the screen (the
 * pause screen's frozen scene: a full-screen triangle over the 1280x480 back
 * buffer, seen in run ...-pause8): it stretches like the 3D it copies. */
static int ws_samples_wide(int textured, uint32_t offset)
{
    BwgSurf *rt;
    if (!textured)
        return 0;
    rt = bwg_find_surf(offset);
    return rt && rt->wsk > 1.0f;
}

/* wsx for this batch: (1, 0) stretches; (1/kx, c(1 - 1/kx)) pillarboxes
 * around the view's centre c (NDC), which maps guest x to host x + c'(kx - 1)
 * at the guest's own scale. */
static void ws_remap(const Nv2aSurface *s, float out[4], int full)
{
    float kx, x0, x1, c;
    out[0] = 1.0f;
    out[1] = out[2] = out[3] = 0.0f;
    if (bwg_ws_k <= 1.0f || !bwg_cur || bwg_cur->wsk <= 1.0f)
        return;
    if (!((bwg_ws_2d > 0 && !full) || s_ws_cut43))
        return;
    kx = bwg_cur->wsk;
    ws_view(s, &x0, &x1);
    c = (x0 + x1) / ws_logical_w(s) - 1.0f;
    out[0] = 1.0f / kx;
    out[1] = c * (1.0f - out[0]);
    s_ws_2d_draws++;
}

static void ws_note(char path, uint32_t n, int tex, uint32_t tw, uint32_t th, float lo, float hi,
                    const Nv2aSurface *s)
{
    static unsigned from;               /* BW_WS_LOG_FROM=<flip>: start there */
    float x0, x1;
    if (s_ws_log < 0) {
        const char *e = getenv("BW_WS_LOG");
        s_ws_log = e && *e ? atoi(e) : 0;
        e = getenv("BW_WS_LOG_FROM");
        from = e && *e ? (unsigned)atoi(e) : 0;
    }
    if (s_ws_log <= 0 || bwg_ws_2d <= 0 || bwg_gen < from)
        return;
    s_ws_log--;
    ws_view(s, &x0, &x1);
    fprintf(stderr, "[WS] f%u 2D L%u %c n%u tex%d %ux%u uv span %.3g x %.3g x %.1f..%.1f view %.0f..%.0f"
            " aa%u full%d surf %08X\n", bwg_gen, bwg_ws_what, path, n, tex, tex ? tw : 0, tex ? th : 0,
            s_ws_span_u, s_ws_span_v, lo, hi, x0, x1, s->aa_sx, s_ws_full, bwg_cur ? bwg_cur->va : 0);
}

/* The CPU path's batch (bw_xdraw): is it a 2D draw that fills the view (a
 * backdrop: stretched), the widescreen 2D test's inputs. */
void bwg_ws_xbatch(const Nv2aSurface *s, const BwXBatch *b)
{
    if (bwg_ws_k > 1.0f) {
        float lo = b->vertices[0].x, hi = lo;
        float u0 = b->vertices[0].tc[0][0], u1 = u0, v0 = b->vertices[0].tc[0][1], v1 = v0;
        for (uint32_t i = 1; i < b->count; i++) {
            const BwXVertex *q = &b->vertices[i];
            if (q->x < lo) lo = q->x;
            if (q->x > hi) hi = q->x;
            if (q->tc[0][0] < u0) u0 = q->tc[0][0];
            if (q->tc[0][0] > u1) u1 = q->tc[0][0];
            if (q->tc[0][1] < v0) v0 = q->tc[0][1];
            if (q->tc[0][1] > v1) v1 = q->tc[0][1];
        }
        s_ws_span_u = u1 - u0;
        s_ws_span_v = v1 - v0;
        s_ws_full = (ws_covers_view(s, lo, hi, 1)
                     && ws_backdrop(s, b->tex_valid[0], b->tex_normalized[0], b->tex[0].width,
                                    b->tex[0].height, u0, u1, v0, v1))
                    || ws_samples_wide(b->tex_valid[0], b->tex[0].offset);
        ws_note('x', b->count, b->tex_valid[0], b->tex[0].width, b->tex[0].height, lo, hi, s);
    }
}

/* gfx-ffp: would this batch's x remap depend on its screen positions (the
 * CPU path's bwg_ws_xbatch)? Only inside a 2D section or with the
 * 4:3-cutscene option (ws_remap); elsewhere the remap is the identity
 * whatever the positions, so a fixed-function batch can go to the GPU
 * untransformed. */
int bwg_ws_needs_positions(void)
{
    return bwg_ws_k > 1.0f && (bwg_ws_2d > 0 || s_ws_cut43);
}

/* The vertex-program path's batch (bw_xdraw_vsh). */
void bwg_ws_vsbatch(const Nv2aSurface *s, const BwXVsBatch *b)
{
    if (bwg_ws_k > 1.0f) {
        /* Position is input 0 when the program reads it; for 2D (D3D's
         * pass-through program) it is in logical pixels. */
        s_ws_full = 0;
        if (bwg_ws_2d > 0 && (b->input_mask & 1u) && b->ninputs && b->nverts && b->inputs) {
            /* The overlay system's quads read v0 position (logical pixels),
             * v1 colour, v2 texture coordinate 0 (normalized) (seen, run
             * ...-r6-gpu-in7: mask 000F); D3D's own fixed-function layout
             * would put it in v9. Inputs arrive in ascending order of the
             * mask (bw_backend_ext.h). */
            int tci = -1, tca = (b->input_mask & (1u << 2)) ? 2 : 9;
            float lo = b->inputs[0], hi = lo, u0 = 0, u1 = 0, v0 = 0, v1 = 0;
            if (b->input_mask & (1u << tca)) {
                uint32_t m = b->input_mask & ((1u << tca) - 1u);
                tci = 0;
                while (m) { tci += (int)(m & 1u); m >>= 1; }
                u0 = u1 = b->inputs[tci * 4];
                v0 = v1 = b->inputs[tci * 4 + 1];
            }
            for (uint32_t i = 1; i < b->nverts; i++) {
                const float *in = &b->inputs[(size_t)i * b->ninputs * 4];
                if (in[0] < lo) lo = in[0];
                if (in[0] > hi) hi = in[0];
                if (tci >= 0) {
                    float u = in[tci * 4], vv = in[tci * 4 + 1];
                    if (u < u0) u0 = u;
                    if (u > u1) u1 = u;
                    if (vv < v0) v0 = vv;
                    if (vv > v1) v1 = vv;
                }
            }
            s_ws_span_u = u1 - u0;
            s_ws_span_v = v1 - v0;
            if (s_ws_log > 0) {
                static uint32_t seen[8];
                int k;
                for (k = 0; k < 8 && seen[k] && seen[k] != b->input_mask; k++)
                    ;
                if (k < 8 && !seen[k]) {
                    seen[k] = b->input_mask;
                    fprintf(stderr, "[WS] 2D vertex-program inputs: mask %04X (%u), first vertex",
                            b->input_mask, b->ninputs);
                    for (uint32_t a = 0; a < b->ninputs && a < 6; a++)
                        fprintf(stderr, " (%g %g %g %g)", b->inputs[a * 4], b->inputs[a * 4 + 1],
                                b->inputs[a * 4 + 2], b->inputs[a * 4 + 3]);
                    fprintf(stderr, "\n");
                }
            }
            /* Textured with no known texture coordinate: not a backdrop. */
            s_ws_full = (ws_covers_view(s, lo, hi, 0)
                         && (b->xb.tex_valid[0] ? tci >= 0 : 1)
                         && ws_backdrop(s, b->xb.tex_valid[0], b->xb.tex_normalized[0],
                                        b->xb.tex[0].width, b->xb.tex[0].height, u0, u1, v0, v1))
                        || ws_samples_wide(b->xb.tex_valid[0], b->xb.tex[0].offset);
            ws_note('v', b->nindices, b->xb.tex_valid[0], b->xb.tex[0].width, b->xb.tex[0].height,
                    lo, hi, s);
        }
    }
}

/* VFX (D9): an overlay (HUD) draw: D3D's pass-through program, no depth
 * test. *skip: the BW_VFX_HUDSKIP test aid leaves this draw out (the
 * overlay decision is then 0). */
int bwg_vsh_overlay(const BwXVsBatch *b, int *skip_draw)
{
    int s_vfx_overlay;
    *skip_draw = 0;
    if (b->points)
        return 0;       /* gfx-sprites: points are 3D, as the CPU path draws them (never handed back as HUD) */
    s_vfx_overlay = bw_vfx_active() && b->ninstr <= 4 && b->xb.state && !b->xb.state->depth_test_enable;
    if (s_vfx_overlay && b->xb.tex_valid[0]) {
        /* V1 test aid: BW_VFX_HUDSKIP=<hex texture address> leaves out the
         * overlay draws of that texture (which HUD draw is which). */
        static int skip_init;
        static uint32_t skip;           /* 0: none */
        if (!skip_init) {
            const char *e = getenv("BW_VFX_HUDSKIP");
            skip_init = 1;
            skip = e ? (uint32_t)strtoul(e, NULL, 16) : 0;
        }
        if (skip && b->xb.tex[0].offset == skip) {
            *skip_draw = 1;
            return 0;
        }
    }
    return s_vfx_overlay;
}

/* The vertex program's info constants (BwVshConsts vsinfo, foginfo). */
void bwg_vsh_info(const BwXVsBatch *b, float vsinfo[4], float foginfo[4])
{
    vsinfo[0] = b->aa[0]; vsinfo[1] = b->aa[1];
    vsinfo[2] = b->near_clip ? 1.0f : 0.0f; vsinfo[3] = 0.0f;
    foginfo[0] = b->fog_enable ? 1.0f : 0.0f;
    foginfo[1] = (float)b->fog_mode;
    foginfo[2] = b->fog_param[0]; foginfo[3] = b->fog_param[1];
}

/* ---- gfx-sprites (I6): point sprites on the GPU ----------------------------
 * The executor hands a point batch over as a point list only when the back
 * end says it draws them (BwXBackend points_ok); the back end runs the
 * program's vertex shader (it writes oPts) and the sprite geometry shader
 * (bw_vsh_sprite_gs_hlsl), which makes each quad by bw_draw_points's rules.
 * Every edition since GA-13 (the maintainer, 2026-11-05: "Original gets the
 * 100% native treatment"): a port change outside the registry, as ORIG-1 is
 * for the audio; the CPU path (bw_draw_points) stays as the reference and
 * the undo. */
int bwg_sprites_on(void)
{
    static int on = -1;
    if (on < 0) {
        /* On in every edition. In Remastered and later the registry's
         * GPU_SPRITES stays as an alias (BW_FIX=-GPU_SPRITES: the CPU path);
         * BW_GPU_SPRITES=0 (or off) keeps the CPU path and =1 asks for the
         * GPU in every edition, Original included. (Batch 2's replay-only
         * aid BW_GPU_SPRITES_ANY_EDITION is no longer needed for that.) */
        const char *e = getenv("BW_GPU_SPRITES");
        on = bw_edition_at_least(BW_ED_REMASTERED) ? bw_fix_enabled(BW_FIX_GPU_SPRITES) : 1;
        if (e && *e)
            on = !(e[0] == '0' || ((e[0] == 'o' || e[0] == 'O') && (e[1] == 'f' || e[1] == 'F')));  /* 0, off */
        if (on)
            fprintf(stderr, "[GPU-SPRITES] point sprites are drawn on the GPU (geometry shader;"
                            " BW_GPU_SPRITES=0 keeps the CPU path)%c", 10);
    }
    return on && !bw_ab_off("gpusprites");
}

void bwg_sprite_consts(const BwXVsBatch *b, BwgSpriteConsts *c)
{
    memset(c, 0, sizeof *c);
    c->spr0[0] = b->aa[0] > 1.0f ? b->aa[0] : 1.0f;     /* aa_scale_x() */
    c->spr0[1] = b->aa[1] > 1.0f ? b->aa[1] : 1.0f;     /* aa_scale_y() */
    c->spr0[2] = b->point_size;
    c->spr1[0] = b->point_sprite ? 1.0f : 0.0f;
    c->spr1[1] = b->point_cw ? 1.0f : 0.0f;
}

/* The screen-space path's constants (bw_draw); textured: its texture is bound. */
void bwg_ss_consts(const Nv2aSurface *s, const Nv2aBatch *b, int textured, BwConsts *pc)
{
    const Nv2aRenderState *rs = b->state;
#define c (*pc)
    memset(&c, 0, sizeof(c));
    c.surf[0] = (float)bwg_cur->w; c.surf[1] = (float)bwg_cur->h;
    c.surf[2] = 1.0f / bwg_cur->w; c.surf[3] = 1.0f / bwg_cur->h;
    if (bwg_ws_k > 1.0f) {
        float lo = b->vertices[0].x, hi = lo;
        for (uint32_t i = 1; i < b->count; i++) {
            if (b->vertices[i].x < lo) lo = b->vertices[i].x;
            if (b->vertices[i].x > hi) hi = b->vertices[i].x;
        }
        {
            float u0 = b->vertices[0].u, u1 = u0, v0 = b->vertices[0].v, v1 = v0;
            for (uint32_t i = 1; i < b->count; i++) {
                if (b->vertices[i].u < u0) u0 = b->vertices[i].u;
                if (b->vertices[i].u > u1) u1 = b->vertices[i].u;
                if (b->vertices[i].v < v0) v0 = b->vertices[i].v;
                if (b->vertices[i].v > v1) v1 = b->vertices[i].v;
            }
            s_ws_span_u = u1 - u0;
            s_ws_span_v = v1 - v0;
            s_ws_full = (ws_covers_view(s, lo, hi, 1)
                         && ws_backdrop(s, b->texture != NULL, 0, 0, 0, u0, u1, v0, v1))
                        || ws_samples_wide(b->texture != NULL, b->texture ? b->texture->offset : 0);
        }
        ws_note('s', b->count, b->texture != NULL, b->texture ? b->texture->width : 0,
                b->texture ? b->texture->height : 0, lo, hi, s);
    }
    ws_remap(s, c.wsx, s_ws_full);
    if (textured) {
        c.tex[0] = 1.0f / b->texture->width;
        c.tex[1] = 1.0f / b->texture->height;
        c.tex[2] = 1.0f;
    }
    if (rs->alpha_test_enable) {
        c.alpha[0] = (float)(rs->alpha_func >= 0x200 && rs->alpha_func <= 0x207
                             ? rs->alpha_func - 0x200 : 7);
        c.alpha[1] = rs->alpha_ref / 255.0f;
        c.alpha[2] = 1.0f;
    }
    {
        float zmin = rs->depth_min, zmax = rs->depth_max;
        if (!(zmax > zmin)) { zmin = 0.0f; zmax = 16777215.0f; }
        c.depth[0] = zmin;
        c.depth[1] = 1.0f / (zmax - zmin);
        c.depth[2] = bwg_zmode_vertex_clamp() ? 1.0f : 0.0f;
    }
#undef c
}

static void unpack_argb(uint32_t c, float o[4])
{
    o[0] = ((c >> 16) & 0xFF) / 255.0f;
    o[1] = ((c >> 8) & 0xFF) / 255.0f;
    o[2] = (c & 0xFF) / 255.0f;
    o[3] = (c >> 24) / 255.0f;
}

/* 0.2.5 item 41: a key's parts the generated text never reads are zeroed,
 * so states that differ only in them share one shader (one table slot, one
 * create) instead of one each: combiner stages past the count (control &
 * 0xFF), and the texture fields of a stage whose program doesn't sample
 * (bw_rc_hlsl's rc_stage_fetch: only modes 1-3 read tex_valid, those with a
 * texture read tex_normalized unless a cube is bound, modes 1-2 tex_conv;
 * only mode-5 stages read their clip_plane_mode bits). A stage with any
 * Remastered variant bit is left as it is. The text, so the bytecode and the
 * pixels, are the same. Test aid BW_GPU_RCKEY_CHECK=1 generates the text
 * from the whole key too, once per distinct key, and logs any difference. */
void bwg_rc_key_trim(BwRcKey *k)
{
    static int check = -1;
    BwRcKey full;
    uint32_t n = k->control & 0xFF, s, st, clip = 0;
    if (check < 0) {
        const char *e = getenv("BW_GPU_RCKEY_CHECK");
        check = e && e[0] == '1';
        if (check)
            fprintf(stderr, "[GPU-D3D11] test aid: each combiner key's shader text checked whole and"
                            " trimmed (BW_GPU_RCKEY_CHECK)\n");
    }
    if (check)
        full = *k;
    if (n > 8)
        n = 8;
    for (s = n; s < 8; s++)
        k->color_icw[s] = k->alpha_icw[s] = k->color_ocw[s] = k->alpha_ocw[s] = 0;
    for (st = 0; st < 4; st++) {
        uint32_t mode = (k->stage_program >> (st * 5)) & 0x1F;
        if (mode == 5)
            clip |= k->clip_plane_mode & (0xFu << (st * 4));
        if (k->vfx[st])
            continue;
        if (mode < 1 || mode > 3)
            k->tex_valid[st] = 0;
        if (!k->tex_valid[st] || (mode == 3 && k->tex_cube[st]))
            k->tex_normalized[st] = 0;
        if (!k->tex_valid[st] || mode == 3)
            k->tex_conv[st] = 0;
    }
    k->clip_plane_mode = clip;
    if (check && memcmp(&full, k, sizeof *k)) {
        static uint64_t seen[4096];
        static char a[64 * 1024], b[64 * 1024];
        static unsigned keys, differ;
        uint64_t h = 1469598103934665603ull;
        const uint8_t *p = (const uint8_t *)&full;
        size_t i, la, lb;
        for (i = 0; i < sizeof full; i++)
            h = (h ^ p[i]) * 1099511628211ull;
        h |= 1;
        for (i = h & 4095; seen[i] && seen[i] != h; i = (i + 1) & 4095)
            ;
        if (seen[i] == h || keys >= 3500)
            return;
        seen[i] = h;
        keys++;
        la = bw_rc_hlsl(&full, a, sizeof a);
        lb = bw_rc_hlsl(k, b, sizeof b);
        if (la != lb || memcmp(a, b, la)) {
            if (differ++ < 3)
                fprintf(stderr, "[GPU-D3D11] rc key check: a trimmed key's shader text DIFFERS (control 0x%X"
                                " stage_program 0x%X, %u vs %u bytes)\n", full.control, full.stage_program,
                        (unsigned)la, (unsigned)lb);
        }
        if (keys <= 40 || keys % 50 == 0)
            fprintf(stderr, "[GPU-D3D11] rc key check: %u trimmed keys checked, %u with different text\n",
                    keys, differ);
    }
}

/* The combiner path's key (xdraw_prepare): the combiners, the stages'
 * textures and modes, the dot-product inputs, the VFX overlay and movie
 * bits; a cube map sampled by a CUBEMAP stage is fetched here (tex[st]). */
void bwg_xdraw_key(const BwXBatch *b, int overlay, BwRcKey *pkey, BwgTexRef tex[4])
{
    const Nv2aCombiner *rc = b->rc;
    int st;
#define key (*pkey)
#define s_vfx_overlay overlay
    for (st = 0; st < 4; st++)
        tex[st].e = NULL, tex[st].rt = NULL;
    memset(&key, 0, sizeof(key));
    memcpy(key.color_icw, rc->color_icw, sizeof(key.color_icw));
    memcpy(key.alpha_icw, rc->alpha_icw, sizeof(key.alpha_icw));
    memcpy(key.color_ocw, rc->color_ocw, sizeof(key.color_ocw));
    memcpy(key.alpha_ocw, rc->alpha_ocw, sizeof(key.alpha_ocw));
    key.final0 = rc->final0;
    key.final1 = rc->final1;
    key.control = rc->control;
    key.stage_program = rc->stage_program;
    key.clip_plane_mode = b->clip_plane_mode;
    key.flags[0] = (uint8_t)(bwg_use_persp() ? BW_RC_PERSP : 0);     /* colour/fog interpolation */
    for (st = 0; st < 4; st++) {
        key.tex_valid[st] = b->tex_valid[st];
        key.tex_normalized[st] = b->tex_normalized[st];
        key.tex_conv[st] = (uint8_t)(b->tex_valid[st] ? bwg_conv_kernel(b->tex[st].filter) : 0);
        /* A cube map sampled by a CUBEMAP stage goes up as six faces; in any
         * other case (or BW_GPU_CUBE=0, or if that fails) its first face is
         * sampled as a 2D texture. */
        key.tex_cube[st] = 0;
        if (b->tex_valid[st] && b->tex_cube[st] && bwg_cube_stage(rc->stage_program, st)) {
            tex[st] = bwg_get_cube_texture(&b->tex[st]);
            key.tex_cube[st] = bwg_tex_ref_view(tex[st]);
        }
    }
    /* The dot-product stages' inputs, only where a program has them (so other
     * programs don't compile once per stale register value). */
    if (bwg_dot_program(rc->stage_program)) {
        key.dot_rgbmapping = b->dot_rgbmapping;
        key.other_stage_input = b->other_stage_input;
    }
    if (s_vfx_overlay && (bw_vfx()->text || bw_vfx()->hud_edges || bw_vfx()->tr_menus))  /* VFX (D9): text and HUD art (overlay draws) */
        for (st = 0; st < 4; st++) {
            int fl = 0;
            uint32_t c = b->tex[st].color;
            if (!b->tex_valid[st] || b->tex_cube[st])
                continue;
            if (c == 0x0C || c == 0x02 || c == 0x10 || c == 0x0E || c == 0x0F)
                fl = bwg_tex_vfx_flags(&b->tex[st]);
            else if (c == 0x12 && bw_vfx()->tr_menus)
                fl = bwg_tex_vfx_flags(&b->tex[st]) & 8;
            if (fl & 8) {                               /* T1: a menu/title photo: Lanczos-2 */
                key.vfx[st] |= BW_RC_VFX_OVERLAY_L2;
                continue;
            }
            if ((fl & 4) && bw_vfx()->hud_edges)        /* V1: hard-edged HUD art */
                key.vfx[st] |= BW_RC_VFX_OVERLAY_HARD;
            else if (!bw_vfx()->text)
                continue;
            else if (fl & 1)
                key.vfx[st] |= BW_RC_VFX_GLYPH;
            else if (fl & 2)
                key.vfx[st] |= BW_RC_VFX_OVERLAY;
            else
                key.vfx[st] |= BW_RC_VFX_OVERLAY_CR;
        }
    /* T1 (BW_TEXRESTORE_MOVIES, item 11): a Bink frame, wherever it is drawn
     * (its quad isn't always caught by the overlay test): Lanczos-2 when
     * magnified, the game's own sample at 1:1. */
    if (bw_vfx_active() && bw_vfx()->tr_movies)
        for (st = 0; st < 4; st++)
            if (b->tex_valid[st] && !b->tex_cube[st] && bwg_tr_is_movie(&b->tex[st]))
                key.vfx[st] = (uint8_t)((key.vfx[st] & BW_RC_VFX_A2C) | BW_RC_VFX_OVERLAY_L2);
#undef s_vfx_overlay
#undef key
}

/* VFX (D9): alpha-to-coverage for this draw (its key's bit set too). */
int bwg_xdraw_a2c(const Nv2aRenderState *rs, int overlay, BwRcKey *pkey)
{
#define key (*pkey)
#define s_vfx_overlay overlay
    /* VFX (D9): alpha-tested, unblended draws on an MSAA target (foliage,
     * fences, rigging) get alpha-to-coverage instead of a hard test. */
    int vfx_a2c = bw_vfx_active() && bw_vfx()->a2c && !s_vfx_overlay && rs->alpha_test_enable
               && !rs->blend_enable && (rs->alpha_func == 0x204 || rs->alpha_func == 0x206)
               && bwg_cur && bwg_cur->mtex;
    if (vfx_a2c)
        key.vfx[0] |= BW_RC_VFX_A2C;
    return vfx_a2c;
#undef s_vfx_overlay
#undef key
}

/* P2a test aid (Remastered only), BW_OCEAN_CULLTEST: 1 and *t the state to
 * draw the sea's reflection pass with, or 0 (the game's own). */
int bwg_ocean_culltest(const BwRcKey *pkey, const Nv2aRenderState *rs, Nv2aRenderState *pt)
{
#define key (*pkey)
        /* P2a test aid (docs/day1/P2a.md section 7), Remastered only:
         * BW_OCEAN_CULLTEST=1|2 forces back-face culling with the front face
         * CW (1, the game's own state, seen) or CCW (2: the game's front
         * faces culled, only the back faces drawn) for the sea's reflection
         * pass only (stage 3 DOT_RFLCT_SPEC on a cube). */
        static int ct = -1;
        if (ct < 0) {
            const char *e = getenv("BW_OCEAN_CULLTEST");
            ct = (e && (e[0] == '1' || e[0] == '2') && bw_edition_at_least(BW_ED_REMASTERED)) ? e[0] - '0' : 0;
        }
        if (ct && ((key.stage_program >> 15) & 0x1F) == 0x0C && key.tex_cube[3]) {
            static int shown;
            Nv2aRenderState t = *rs;
            if (!shown++)
                fprintf(stderr, "[BW] ocean cull test: the game's own state for the reflection pass:"
                        " cull_enable %u cull_face 0x%X front_face 0x%X%c",
                        (unsigned)rs->cull_enable, (unsigned)rs->cull_face, (unsigned)rs->front_face, 10);
            t.cull_enable = 1;
            t.cull_face = 0x405;                        /* GL_BACK */
            t.front_face = ct == 1 ? 0x900 : 0x901;     /* GL_CW / GL_CCW */
            *pt = t;
            return 1;
        }
    return 0;
#undef key
}

/* The combiner path's constants (xdraw_prepare), before the stages'. */
void bwg_xdraw_consts(const Nv2aSurface *s, const BwXBatch *b, BwXConsts *pc)
{
    const Nv2aRenderState *rs = b->state;
    const Nv2aCombiner *rc = b->rc;
    int st;
#define c (*pc)
    memset(&c, 0, sizeof(c));
    c.surf[0] = (float)bwg_cur->w; c.surf[1] = (float)bwg_cur->h;
    c.surf[2] = 1.0f / bwg_cur->w; c.surf[3] = 1.0f / bwg_cur->h;
    ws_remap(s, c.wsx, s_ws_full);
    {
        float zmin = rs->depth_min, zmax = rs->depth_max;
        if (!(zmax > zmin)) { zmin = 0.0f; zmax = 16777215.0f; }
        c.depth[0] = zmin;
        c.depth[1] = 1.0f / (zmax - zmin);
        c.depth[2] = bwg_zmode_vertex_clamp() ? 1.0f : 0.0f;
    }
    if (rs->alpha_test_enable) {
        c.alpha[0] = (float)(rs->alpha_func >= 0x200 && rs->alpha_func <= 0x207
                             ? rs->alpha_func - 0x200 : 7);
        c.alpha[1] = (float)(rs->alpha_ref & 0xFF);
        c.alpha[2] = 1.0f;
    }
    c.fogc[0] = b->fogc[0]; c.fogc[1] = b->fogc[1]; c.fogc[2] = b->fogc[2];
    for (st = 0; st < 8; st++) {
        unpack_argb(rc->factor0[st], c.c0[st]);
        unpack_argb(rc->factor1[st], c.c1[st]);
    }
    unpack_argb(rc->final_c0, c.fc0);
    unpack_argb(rc->final_c1, c.fc1);
#undef c
}

/* A textured stage's constants, and the title's anti-aliasing resolve: a
 * 2x-wide surface sampled into a 1x one (render targets are linear images).
 * Its filter is how the present turns the 2x-wide surface into the picture. */
void bwg_xdraw_stage(const BwXBatch *b, int st, BwXConsts *pc)
{
#define c (*pc)
        c.texinfo[st][0] = 1.0f / b->tex[st].width;
        c.texinfo[st][1] = 1.0f / b->tex[st].height;
        /* The title's anti-aliasing resolve: a 2x-wide surface sampled into a
         * 1x one (render targets are linear images). Its filter is how
         * gpu_present turns the 2x-wide surface into the picture. */
        if (!b->tex_normalized[st] && bwg_cur && bwg_cur->aa_sx == 1) {
            BwgSurf *rt = bwg_find_surf(b->tex[st].offset);
            if (rt && rt != bwg_cur && rt->aa_sx == 2) {
                bwg_resolve_filter = b->tex[st].filter;
                bwg_resolve_seen = 1;
            }
        }
#undef c
}

/* P1: the raw vertex fetch: each input's format (DXGI) for its conversion. */
int bwg_vfetch_format(uint8_t conv)
{
    uint32_t s = conv & 15u;
    switch (conv >> 4) {
    case BW_XVS_FLOAT:
        return s == 1 ? DXGI_FORMAT_R32_FLOAT : s == 2 ? DXGI_FORMAT_R32G32_FLOAT
             : s == 3 ? DXGI_FORMAT_R32G32B32_FLOAT : DXGI_FORMAT_R32G32B32A32_FLOAT;
    case BW_XVS_CONST: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case BW_XVS_COLOR: return DXGI_FORMAT_R8G8B8A8_UINT;
    case BW_XVS_UBYTE:
        return s == 1 ? DXGI_FORMAT_R8_UINT : s == 2 ? DXGI_FORMAT_R8G8_UINT : DXGI_FORMAT_R8G8B8A8_UINT;
    case BW_XVS_SHORT:
        return s == 1 ? DXGI_FORMAT_R16_SINT : s == 2 ? DXGI_FORMAT_R16G16_SINT
                                                     : DXGI_FORMAT_R16G16B16A16_SINT;
    case BW_XVS_CMP:   return DXGI_FORMAT_R32_UINT;
    default:           return DXGI_FORMAT_UNKNOWN;
    }
}

/* The input layout for this batch's streams: one element per input, in its
 * stream's slot at its offset, in its own format, and the layout's key. 0:
 * an input has no format (the CPU path draws it). */
int bwg_vfetch_elements(const BwXVsBatch *b, uint32_t nin, BwgVElem el[16], uint64_t *pkey)
{
    uint64_t key = 1469598103934665603ull;
    uint32_t k;
    for (k = 0; k < nin; k++) {
        const BwXVsInput *in = &b->in[k];
        el[k].format = bwg_vfetch_format(in->conv);
        el[k].slot = in->stream;
        el[k].offset = in->offset;
        if (el[k].format == DXGI_FORMAT_UNKNOWN)
            return 0;
        key = (key ^ ((uint64_t)el[k].format | (uint64_t)in->stream << 8
                      | (uint64_t)in->offset << 16)) * 1099511628211ull;
    }
    *pkey = key;
    return 1;
}
