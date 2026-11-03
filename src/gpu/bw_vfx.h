/* bw_vfx.h - Remastered's visual-enhancement suite (D-016), agent D9.
 *
 * The settings, the one place that decides the render scale, and the GPU
 * helpers the D3D11 back end (bw_d3d11.c) calls at a few marked hooks.
 * Nothing here runs in Original: there bw_vfx_active()
 * is 0, every query returns the value the back end used before (scale 1,
 * B8G8R8A8 targets, no MSAA, no anisotropy), and the back end takes its old
 * paths unchanged. See bw_vfx.c for the switches and docs/day1/D9.md. */
#ifndef BW_VFX_H
#define BW_VFX_H

#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_4.h>    /* IDXGISwapChain3: HDR colour space */
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

/* ---- GPU helpers ----------------------------------------------------------
 * All on the back end's device thread, with its device and context. They
 * change pipeline state; the back end rebinds its own afterwards. */
int  bw_vfx_gpu_init(ID3D11Device *dev, ID3D11DeviceContext *ctx);
/* Draw src (sw x sh texels) into dst (dw x dh pixels): mode 0 bilinear
 * stretch, 1 box average (downsampling by any factor). */
void bw_vfx_blit(ID3D11ShaderResourceView *src, UINT sw, UINT sh,
                 ID3D11RenderTargetView *dst, UINT dw, UINT dh, int mode);

/* ---- the frame ------------------------------------------------------------
 * The finished frame (src, sw x sh physical pixels) through the enabled
 * effects into `dst` (the swap chain's view, vp its letterbox rectangle), or
 * with dst NULL into an off-screen output of the output size (BW_GPU_PRESENT=0
 * runs). Writes the output shots. Returns 1 if it drew. */
int  bw_vfx_present(ID3D11ShaderResourceView *src, UINT sw, UINT sh,
                    ID3D11RenderTargetView *dst, const D3D11_VIEWPORT *vp);
/* The scene before the HUD for the next bw_vfx_present (NULL: none this
 * frame). Bloom reads it, and OLED care dims only where the frame differs. */
void bw_vfx_set_scene(ID3D11ShaderResourceView *scene);

/* The title's DAC gamma ramp as D10's 256x1 lookup (bw_d3d11.c gamma_lut:
 * the fade-in at every mission start, the brightness slider), applied at the
 * end of the chain; NULL for none. Set before each bw_vfx_present. */
void bw_vfx_set_gamma_lut(ID3D11ShaderResourceView *lut);

/* The off-screen output's format; the suite's note when a swap chain is made
 * (its format and colour space are bw_hdr.c's, every edition). */
DXGI_FORMAT bw_vfx_swap_format(void);
void bw_vfx_swap_created(IDXGISwapChain1 *swap);
/* HDR highlights this session (VFX_HDR on, or auto on a screen in HDR mode
 * whose peak is known); decided once, before the first render targets. */
int bw_vfx_hdr_highlights(void);

/* ---- text (VFX_TEXT) --------------------------------------------------------
 * Is this a glyph sheet: a DXT1 texture of white glyphs on 1-bit alpha, as
 * all 16 of the game's fonts are (docs/formats/effects_fonts_ui.md). Checked
 * once per upload from the blocks themselves; nothing is matched by name or
 * hash. 0 in Original. */
int  bw_vfx_is_glyph_sheet(const uint8_t *dxt1, uint32_t w, uint32_t h);

/* Ambient occlusion (VFX_AO), per view before its HUD: depth (D24 as
 * R24_UNORM_X8, multisampled or not) of the target, the view rectangle,
 * the target's colour view (multiplied by the occlusion), the near and far
 * planes and the view's focal length in pixels. */
void bw_vfx_ao_apply(ID3D11ShaderResourceView *depth, int depth_ms, UINT w, UINT h,
                     ID3D11RenderTargetView *target, const D3D11_RECT *rect,
                     float zn, float zf, float focal);

/* A linear, clamped sampler (the text filter binds it at s4). */
ID3D11SamplerState *bw_vfx_linear_sampler(void);

/* The draw-distance factor for the RenderItem_IsVisible hook (game thread;
 * recomp_manual.c). 1 in Original. */
float bw_vfx_lod_scale(void);

/* GPU time of the effects, for BW_GPU_STATS lines (ms per frame, averaged). */
double bw_vfx_post_ms(void);

#endif
