/* bw_vfx.h - Remastered's visual-enhancement suite (D-016), agent D9.
 *
 * The settings and the one place that decides the render scale: no graphics
 * API here (the shared core, src/gpu/core, includes this header; GC-3). The
 * GPU helpers the D3D11 back end calls at a few marked hooks are declared in
 * bw_vfx_d3d11.h.
 * Nothing here runs in Original: there bw_vfx_active()
 * is 0, every query returns the value the back end used before (scale 1,
 * B8G8R8A8 targets, no MSAA, no anisotropy), and the back end takes its old
 * paths unchanged. See bw_vfx.c for the switches and docs/day1/D9.md. */
#ifndef BW_VFX_H
#define BW_VFX_H

#define COBJMACROS
#include <windows.h>
#include <dxgiformat.h> /* DXGI_FORMAT (D3D11's and D3D12's) */
#include <stdint.h>

typedef struct {
    int   active;           /* any enhancement on (Remastered+, and not all switched off) */
    int   native_res;       /* VFX_NATIVE_RES */
    float scale_force;      /* BW_VFX_SCALE: fixed render scale (0: from the output) */
    float ssaa;             /* BW_VFX_SSAA: render-scale multiplier, 1..4 */
    int   guest_aa;         /* BW_VFX_GUESTAA=1: keep the game's 2x horizontal surface at native res */
    unsigned out_w, out_h;  /* BW_VFX_OUTPUT=WxH: the output picture size when off-screen */
    int   offscreen;        /* run the chain without a swap chain (BW_GPU_PRESENT=0 runs) */
    UINT  msaa;             /* VFX_MSAA: 1 (off), 2, 4, 8 */
    UINT  aniso;            /* VFX_ANISO: 1 (off) .. 16 */
    int   a2c;              /* alpha-to-coverage for alpha-tested draws (with MSAA; BW_VFX_A2C=0 off) */
    int   precision;        /* VFX_PRECISION: 16-bit targets, dithered output */
    int   text;             /* VFX_TEXT */
    int   hud_edges;        /* VFX_HUD_EDGES (V1): hard-edged HUD art drawn with a smooth contour */
    int   fxaa;            /* VFX_FXAA */
    float sharpen;          /* VFX_SHARPEN strength, 0 = off */
    float bloom;            /* VFX_BLOOM strength, 0 = off */
    int   hdr;              /* VFX_HDR: 0 off, 1 on, 2 auto (Remastered's default; 0.2.5 item 97) */
    float hdr_paper, hdr_peak;   /* nits */
    int   gamut;            /* BW_VFX_GAMUT: 0 bt709 (default), 1 p3stretch, on a screen in HDR mode (item 99) */
    float oled_dim;         /* VFX_OLED_CARE: the most a static bright pixel dims, 0 = off */
    float lod;              /* VFX_DRAW_DISTANCE: LOD/draw-distance factor, 1 = off */
    float ao;               /* VFX_AO strength, 0 = off */
    int   shot_every;       /* BW_VFX_SHOT_EVERY: frames between output shots (0: with the dump trigger) */
    /* T1, VFX_TEX_RESTORE (bw_texrestore.c): texture restoration */
    int   tex_restore;      /* 3D DXT textures: deblocked, with enlarged levels above painted size */
    int   tr_deblock;       /* BW_TEXRESTORE_DEBLOCK=0: no Plan A deblock */
    int   tr_scale;         /* BW_TEXRESTORE_SCALE=2|4: the largest enlarged level (4) */
    int   tr_debug;         /* BW_TEXRESTORE_DEBUG=1: tint enlarged-level samples green, the game's blue */
    int   tr_menus;         /* BW_TEXRESTORE_MENUS=0: menu/title photos as before (JPEG deblock + Lanczos-2) */
    int   tr_movies;        /* BW_TEXRESTORE_MOVIES=0: Bink frames as before (8x8 deblock + Lanczos-2) */
    int   tr_cutout_threshold;  /* BW_TEXRESTORE_CUTOUT=threshold: cut-out alpha thresholded at 0.5
                                 * (default "game": bilinear, the game's own alpha test cuts it) */
    unsigned tr_budget_mb;  /* BW_TEXRESTORE_MB: VRAM for restored textures (2048) */
} BwVfx;

/* Read the edition and the switches once (the back end's device thread, at
 * device creation, which is after bw_edition_init). Logs one [VFX] line. */
void bw_vfx_init(void);
const BwVfx *bw_vfx(void);
static __inline int bw_vfx_active(void) { return bw_vfx()->active; }

/* ---- the render scale (one place decides it) ----------------------------
 * S = physical pixels per guest screen pixel (the game's 640x480 screen):
 * the output picture's height / 480, times BW_VFX_SSAA, or BW_VFX_SCALE.
 * The output picture is the rectangle the frame is shown in: the 4:3
 * letterbox today, or the widescreen rectangle (agent D5's aspect logic
 * sets the rectangle and multiplies device-target widths by its own k;
 * this file never decides the aspect). 1 in Original. */
void     bw_vfx_set_output(unsigned w, unsigned h);
/* The picture's aspect (width / height) for the off-screen output, which has
 * no window to take it from: 4/3, or the widescreen picture's (D5 sets it
 * before each off-screen bw_vfx_present). */
void     bw_vfx_set_picture_aspect(float aspect);
float    bw_vfx_picture_aspect(void);
float    bw_vfx_scale(void);
unsigned bw_vfx_epoch(void);     /* changes whenever S changes: surfaces resize */
/* The physical size of a guest surface of logical w x h (pitch-wide), drawn
 * with anti-aliasing factors aa_sx, aa_sy. In Original: w x h. */
void     bw_vfx_surface_size(uint32_t w, uint32_t h, uint32_t aa_sx, uint32_t aa_sy,
                             uint32_t *pw, uint32_t *ph);
DXGI_FORMAT bw_vfx_target_format(uint32_t bpp);   /* the colour target's format */
UINT     bw_vfx_msaa(void);                       /* sample count for every target */
UINT     bw_vfx_aniso(void);                      /* max anisotropy for mipped samplers */

/* The off-screen output's format (the suite's note when a swap chain is
 * made: bw_vfx_d3d11.h). */
DXGI_FORMAT bw_vfx_swap_format(void);
/* HDR highlights this session (VFX_HDR on, or auto on a screen in HDR mode
 * whose peak is known); decided once, before the first render targets. */
int bw_vfx_hdr_highlights(void);

/* ---- text (VFX_TEXT) --------------------------------------------------------
 * Is this a glyph sheet: a DXT1 texture of white glyphs on 1-bit alpha, as
 * all 16 of the game's fonts are (docs/formats/effects_fonts_ui.md). Checked
 * once per upload from the blocks themselves; nothing is matched by name or
 * hash. 0 in Original. */
int  bw_vfx_is_glyph_sheet(const uint8_t *dxt1, uint32_t w, uint32_t h);

/* The draw-distance factor for the RenderItem_IsVisible hook (game thread;
 * recomp_manual.c). 1 in Original. */
float bw_vfx_lod_scale(void);

/* GPU time of the effects, for BW_GPU_STATS lines (ms per frame, averaged). */
double bw_vfx_post_ms(void);

#endif
