/*
 * bw_gfxcls.h - region masks for the comparison tool (BW_GFXCAP_CLASSES; lane
 * gfx-capef, docs/gfx/capture.md section 6). A test aid of the capture's
 * replay, in every edition; unset (or not replaying), nothing here runs.
 *
 * While a replay tracks classes, every draw into a colour target is compared
 * on the GPU with the target as it was just before the draw (a copy taken in
 * begin()), and each pixel the draw changed gets the draw's class. A colour
 * clear sets the whole target to CLEAR. At the flips BW_GFXCAP_CLASSES names,
 * the shown target's map is written next to the replay's shots
 * (cls_replay_f<flip>.raw: "BWCLS001", u32 w, h, flip, classes, then one byte
 * a pixel, top row first, at the target's physical size). tools/bw_gfxcmp.py
 * reads them.
 *
 * What it can't see (inferred): a draw that writes the value a pixel already
 * holds leaves the pixel's class as it was; changes the back end makes between
 * draws (the Remastered suite's passes) keep the class the pixel had.
 */
#ifndef BW_GFXCLS_H
#define BW_GFXCLS_H

#include <stdint.h>

/* The classes; tools/bw_gfxcmp.py has the same table. */
enum {
    BW_CLS_HELD = 0,    /* not written since tracking began (the seed, or an earlier frame) */
    BW_CLS_CLEAR,       /* a colour clear */
    BW_CLS_WORLD,       /* 3D with the depth test: boats, land, props, particles (the rest) */
    BW_CLS_SEA,         /* the sea's lit pass (stage program 0x21081), its dot-product passes,
                           and draws of the same vertex arrays in the frame (the wake foam pass) */
    BW_CLS_SKY,         /* 3D without the depth test: the sky dome, sun, moon and glare */
    BW_CLS_HUD,         /* 2D overlays: D3D's pass-through program without the depth test,
                           and screen-space batches */
    BW_CLS_TEXT,        /* an overlay sampling a glyph sheet */
    BW_CLS_MOVIE,       /* a draw sampling the Bink frame (LIN_X8R8G8B8 640x480 from guest memory) */
    BW_CLS_SPRITE,      /* gfx-sprites, with BW_GFXCLS_SPRITES=1 only: a point batch (particles:
                           rain splashes, spray), drawn by either path; else they stay in world */
    BW_CLS_N
};

/* What the back end knows about a draw (bw_d3d11.c fills it). */
typedef struct {
    uint8_t valid, surface;     /* the stage has a texture; it is a surface the back end drew */
    uint32_t offset, width, height, color;
} BwClsTex;
typedef struct {
    int overlay;                /* 2D: pass-through program without the depth test, or screen space */
    int depth_test;
    int sea;                    /* the sea's stage programs */
    uint32_t vtx;               /* guest address of the first vertex array the draw reads (0: unknown) */
    uint32_t gen;               /* the back end's frame */
    int points;                 /* gfx-sprites: a point batch (bw_backend_batch_points) */
    BwClsTex tex[4];
} BwClsDraw;

/* 1 while a replay tracks classes (BW_GFXCAP_CLASSES set); 0 otherwise. */
extern int g_bw_gfxcls;

/* bw_gfxcap_init, when replaying: reads BW_GFXCAP_CLASSES=<flip>[,<flip>...]
 * and BW_GFXCAP_CLASSES_LEAD=<frames> (default 2): draws are classed in that many
 * frames before each listed flip, and the GPU copies freed after each map. */
void bw_gfxcls_init(const char *outdir);
int bw_gfxcls_classify(const BwClsDraw *d);

/* The back end's hooks (D3D11 objects as void *, so this header needs no
 * D3D): ctx is the immediate context, tex the ID3D11Texture2D the draw
 * renders into (the MSAA one when there is one), gen the back end's frame. */
void bw_gfxcls_sync(void *ctx, void *tex, uint32_t gen);              /* begin(): before a draw */
void bw_gfxcls_draw(void *ctx, void *tex, uint32_t gen, int cls);     /* after a draw */
void bw_gfxcls_clear(void *ctx, void *tex, uint32_t gen);             /* after a colour clear */
void bw_gfxcls_flip(void *ctx, void *tex, uint32_t gen);              /* the flip of frame gen */

#endif
