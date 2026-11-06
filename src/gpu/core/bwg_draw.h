/*
 * bwg_draw.h - the renderer's API-neutral core: the per-draw preparation
 * without API calls (bwg_draw.c), lifted verbatim from bw_d3d11.c (lane
 * gfx-core, gate GC). The back end calls these in the order the draws did
 * before, between its own API work.
 */
#ifndef BWG_DRAW_H
#define BWG_DRAW_H

#include <stdint.h>
#include "nv2a_backend.h"
#include "toolkit_patches/kernel/bw_backend_ext.h"
#include "../bw_rc_hlsl.h"              /* BwRcKey */
#include "bwg_tex.h"

/* The screen-space path's constants (b0 of bw_d3d11.c's s_hlsl). */
typedef struct {
    float surf[4];      /* W, H, 1/W, 1/H */
    float tex[4];       /* 1/tw, 1/th, textured, - */
    float alpha[4];     /* func (0..7), ref 0..1, enabled, - */
    float depth[4];     /* zmin, 1/(zmax - zmin), - , - */
    float wsx[4];       /* widescreen x remap (BW_WS shaders only): a, b, -, - */
} BwConsts;

/* The combiner path's constants (b0 of BW_RC_CBUFFER, bw_rc_hlsl.h). */
typedef struct {
    float surf[4], depth[4], alpha[4], fogc[4];
    float c0[8][4], c1[8][4], fc0[4], fc1[4];
    float texinfo[4][4];
    float wsx[4];                       /* widescreen x remap (BW_WS shaders only) */
} BwXConsts;

/* The title's last anti-aliasing resolve filter (the present's resolve). */
extern uint32_t bwg_resolve_filter;
extern int bwg_resolve_seen;

/* Widescreen (D5): the in-stream markers (main.c's bw_d3d11_ws_start calls
 * bwg_ws_start), the end of each frame, and each batch's 2D test. */
void bwg_ws_start(void);
void bwg_ws_frame_end(void);
void bwg_ws_xbatch(const Nv2aSurface *s, const BwXBatch *b);
void bwg_ws_vsbatch(const Nv2aSurface *s, const BwXVsBatch *b);
/* gfx-ffp: 1 where a batch's x remap depends on its screen positions */
int bwg_ws_needs_positions(void);

/* The overlay (HUD) test of a vertex-program batch, and its info constants. */
int bwg_vsh_overlay(const BwXVsBatch *b, int *skip_draw);
void bwg_vsh_info(const BwXVsBatch *b, float vsinfo[4], float foginfo[4]);

/* The screen-space path's constants (its texture fetched first). */
void bwg_ss_consts(const Nv2aSurface *s, const Nv2aBatch *b, int textured, BwConsts *c);

/* The combiner path, in its order: the key (cube textures fetched), the
 * back end's restored stages (T1), alpha-to-coverage, the trim, the back
 * end's shader and rasteriser (the cull test aid), the constants, then each
 * textured stage (its texture and sampler first). */
void bwg_xdraw_key(const BwXBatch *b, int overlay, BwRcKey *key, BwgTexRef tex[4]);
int bwg_xdraw_a2c(const Nv2aRenderState *rs, int overlay, BwRcKey *key);
void bwg_rc_key_trim(BwRcKey *k);
int bwg_ocean_culltest(const BwRcKey *key, const Nv2aRenderState *rs, Nv2aRenderState *t);
void bwg_xdraw_consts(const Nv2aSurface *s, const BwXBatch *b, BwXConsts *c);
void bwg_xdraw_stage(const BwXBatch *b, int st, BwXConsts *c);

/* gfx-sprites (I6): point sprites on the GPU. bwg_sprites_on: the switch
 * (on in every edition since GA-13, a port change; in Remastered and later
 * the registry's GPU_SPRITES is its alias; BW_GPU_SPRITES=1/0 over it in
 * every edition; BW_GPU_AB=gpusprites draws odd frames the CPU way). bwg_sprite_consts: the sprite geometry shader's b1
 * (bw_vsh_sprite_gs_hlsl) for a point batch: spr0 = the anti-aliasing scale
 * never below 1 (x, y), the batch's size (0: each point's oPts.x), -;
 * spr1 = sprite coordinates on stage 3, clockwise, -, -. */
typedef struct { float spr0[4], spr1[4]; } BwgSpriteConsts;
int bwg_sprites_on(void);
void bwg_sprite_consts(const BwXVsBatch *b, BwgSpriteConsts *c);

/* P1: the raw vertex fetch's inputs (format: a DXGI_FORMAT; slot, offset). */
typedef struct { int format; uint32_t slot, offset; } BwgVElem;
int bwg_vfetch_format(uint8_t conv);
int bwg_vfetch_elements(const BwXVsBatch *b, uint32_t nin, BwgVElem el[16], uint64_t *key);

#endif
