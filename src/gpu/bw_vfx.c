/*
 * bw_vfx.c - Remastered's visual-enhancement suite (D-016), agent D9.
 *
 * The rule (D-016): enhance how the original art is rendered, never
 * replace it. Every enhancement has its own switch and a sensible default,
 * and none of it runs in Original: there bw_vfx_init() leaves
 * everything off, and the back end (bw_d3d11.c) takes exactly its old paths:
 * none of the code below runs there except the one-line edition test.
 *
 * Switches (registry src/bw_fixes.def; BW_FIX=-ID turns one off; a BW_VFX_
 * variable sets its value, and "0" turns it off too):
 *   VFX_NATIVE_RES  on   render at the output's resolution
 *       BW_VFX_SCALE=<f>     fixed render scale instead (1 = 640x480)
 *       BW_VFX_SSAA=<f>      supersampling: the scale times f (1..4, default 1)
 *       BW_VFX_GUESTAA=1     keep the game's 2x-wide AA surface at native res
 *                            (2x horizontal supersampling on top of MSAA)
 *       BW_VFX_OUTPUT=WxH    the output picture size (default: the window's
 *                            4:3 rectangle; 1920x1440 when nothing presents)
 *   VFX_MSAA        on   BW_VFX_MSAA=2|4|8 (default 4); with it, alpha-tested
 *                        draws use alpha-to-coverage (BW_VFX_A2C=0: hard test)
 *   VFX_ANISO       on   BW_VFX_AF=2..16 (default 16)
 *   VFX_PRECISION   on   16-bit targets, dithered 8-bit output
 *   VFX_TEXT        on   contour filter on magnified glyph sheets
 *   VFX_HUD_EDGES   on   smooth contour on magnified hard-edged HUD art (V1;
 *                        BW_VFX_HUDEDGE=0 off: the radar ring's beaded rim)
 *   VFX_FXAA        off  BW_VFX_FXAA=1
 *   VFX_SHARPEN     off  BW_VFX_SHARPEN=0..1 (0.3 when on)
 *   VFX_BLOOM       off  BW_VFX_BLOOM=0..1 (0.25 when on)
 *   VFX_HDR         off  BW_VFX_HDR=1; BW_VFX_HDR_PAPER=<nits> (BT.2408 Table 4
 *                        for the screen's EDID peak; 203 without one),
 *                        BW_VFX_HDR_PEAK=<nits> (the EDID peak; 800 without one)
 *   VFX_OLED_CARE   off  BW_VFX_OLED_DIM=0..0.5 (0.25 when on)
 *   VFX_AO          off  BW_VFX_AO=0..1 (0.5 when on): ambient occlusion
 *   VFX_DRAW_DISTANCE on BW_VFX_LOD=<k> (default 2): mesh LODs and draw
 *                        distances k times further (hook in recomp_manual.c)
 *   VFX_TEX_RESTORE on   texture restoration (T1, bw_texrestore.c): 3D DXT
 *                        textures deblocked, with Lanczos-2 enlarged levels
 *                        sampled only above painted size; menu photos too.
 *                        BW_TEXRESTORE=0 off, BW_TEXRESTORE_DEBLOCK=0,
 *                        BW_TEXRESTORE_SCALE=2|4, BW_TEXRESTORE_DEBUG=1 (tint),
 *                        BW_TEXRESTORE_MENUS=0, BW_TEXRESTORE_MOVIES=0,
 *                        BW_TEXRESTORE_CUTOUT=game|threshold (game),
 *                        BW_TEXRESTORE_MB=<n> (2048)
 *   BW_VFX=0             all of the above off (Remastered draws as Original)
 *   BW_VFX_SHOT_EVERY=<n>  write the output picture every n frames (test aid;
 *                        otherwise one per RECOMP_FB_DUMP_FLIPS capture window)
 */
#define BW_VFX_INTERNAL
#include "bw_vfx.h"
#include "bw_vfx_int.h"
#include "bw_gpu_failtest.h"   /* 0.2.5 item 25: BW_GPU_FAILTEST (test aid) */
#include "bw_edition.h"

#include <d3dcompiler.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static BwVfx s_v;
static int s_inited;

/* A switch: the registry's on/off, then BW_VFX_<name> as its value. Returns
 * the value (def_val when on and unset), 0 when off. */
static float vfx_flag(bw_fix_id_t id, const char *env, float def_val)
{
    const char *e = getenv(env);
    int on = bw_fix_enabled(id);
    if (e && *e) {
        float v = (float)atof(e);
        if (e[0] == 'o' || e[0] == 'O')         /* "on"/"off" */
            return (e[1] == 'n' || e[1] == 'N') ? def_val : 0.0f;
        return v;
    }
    return on ? def_val : 0.0f;
}

void bw_vfx_init(void)
{
    const char *e;
    if (s_inited)
        return;
    s_inited = 1;
    memset(&s_v, 0, sizeof s_v);
    s_v.msaa = 1;
    s_v.aniso = 1;
    s_v.ssaa = 1.0f;
    s_v.lod = 1.0f;
    if (!bw_edition_at_least(BW_ED_REMASTERED))
        return;                                     /* Original: everything off */
    e = getenv("BW_VFX");
    if (e && e[0] == '0') {
        fprintf(stderr, "[VFX] BW_VFX=0: the visual suite is off (draws as Original)\n");
        return;
    }
    s_v.native_res = vfx_flag(BW_FIX_VFX_NATIVE_RES, "BW_VFX_NATIVE_RES", 1.0f) != 0.0f;
    e = getenv("BW_VFX_SCALE");
    if (e && atof(e) > 0.0) {
        s_v.scale_force = (float)atof(e);
        s_v.native_res = 1;
    }
    e = getenv("BW_VFX_SSAA");
    if (e && atof(e) >= 1.0)
        s_v.ssaa = (float)(atof(e) > 4.0 ? 4.0 : atof(e));
    e = getenv("BW_VFX_GUESTAA");
    s_v.guest_aa = e && e[0] == '1';
    e = getenv("BW_VFX_OUTPUT");
    if (e && sscanf(e, "%ux%u", &s_v.out_w, &s_v.out_h) != 2)
        s_v.out_w = s_v.out_h = 0;
    {
        float m = vfx_flag(BW_FIX_VFX_MSAA, "BW_VFX_MSAA", 4.0f);
        s_v.msaa = m >= 8.0f ? 8 : m >= 4.0f ? 4 : m >= 2.0f ? 2 : 1;
    }
    {
        float a = vfx_flag(BW_FIX_VFX_ANISO, "BW_VFX_AF", 16.0f);
        s_v.aniso = a >= 16.0f ? 16 : a >= 2.0f ? (UINT)a : 1;
    }
    s_v.precision = vfx_flag(BW_FIX_VFX_PRECISION, "BW_VFX_PRECISION", 1.0f) != 0.0f;
    e = getenv("BW_VFX_A2C");
    s_v.a2c = s_v.msaa > 1 && !(e && e[0] == '0');
    s_v.text = vfx_flag(BW_FIX_VFX_TEXT, "BW_VFX_TEXT", 1.0f) != 0.0f;
    s_v.hud_edges = vfx_flag(BW_FIX_VFX_HUD_EDGES, "BW_VFX_HUDEDGE", 1.0f) != 0.0f;
    s_v.fxaa = vfx_flag(BW_FIX_VFX_FXAA, "BW_VFX_FXAA", 1.0f) != 0.0f;
    s_v.sharpen = vfx_flag(BW_FIX_VFX_SHARPEN, "BW_VFX_SHARPEN", 0.3f);
    s_v.bloom = vfx_flag(BW_FIX_VFX_BLOOM, "BW_VFX_BLOOM", 0.25f);
    /* 0.2.5 item 97 (D-026): HDR 0 off, 1 on, 2 auto. BW_VFX_HDR=auto|on|off
     * (or 1/0) decides; without it the VFX_HDR fix on means on, and nothing
     * chosen means off (item 100: HDR is opt-in, the launcher's Automatic
     * HDR writes BW_VFX_HDR=auto; was auto). Original: off. */
    e = getenv("BW_VFX_HDR");
    if (e && *e) {
        s_v.hdr = (!_stricmp(e, "auto")) ? 2 : (e[0] == 'o' || e[0] == 'O') ? ((e[1] == 'n' || e[1] == 'N') ? 1 : 0)
                : atof(e) > 0.0 ? 1 : 0;
    } else
        s_v.hdr = bw_fix_enabled(BW_FIX_VFX_HDR) ? 1 : 0;
    if (!bw_edition_at_least(BW_ED_REMASTERED))
        s_v.hdr = 0;
    /* 0.2.5 item 99 (the maintainer): BW_VFX_GAMUT=bt709 (default) | p3stretch, the
     * game's BT.709 colours read as P3-D65 on a screen in HDR mode */
    e = getenv("BW_VFX_GAMUT");
    s_v.gamut = e && !_stricmp(e, "p3stretch");
    e = getenv("BW_VFX_HDR_PAPER");
    /* 0.2.5 item 97 (D-026): with no screen data, BT.2408's Graphics White
     * (203, its Table 4 row for a 1000-nit display); on a screen in HDR mode
     * bw_vfx_post.c takes paper white from the table for its EDID peak */
    s_v.hdr_paper = e && atof(e) > 0 ? (float)atof(e) : 203.0f;
    e = getenv("BW_VFX_HDR_PEAK");
    s_v.hdr_peak = e && atof(e) > 0 ? (float)atof(e) : 800.0f;
    if (s_v.hdr_peak < s_v.hdr_paper)
        s_v.hdr_peak = s_v.hdr_paper;
    s_v.lod = vfx_flag(BW_FIX_VFX_DRAW_DISTANCE, "BW_VFX_LOD", 2.0f);
    if (s_v.lod < 1.0f) s_v.lod = 1.0f;
    if (s_v.lod > 8.0f) s_v.lod = 8.0f;
    s_v.ao = vfx_flag(BW_FIX_VFX_AO, "BW_VFX_AO", 0.5f);
    if (s_v.ao > 1.0f) s_v.ao = 1.0f;
    s_v.oled_dim = vfx_flag(BW_FIX_VFX_OLED_CARE, "BW_VFX_OLED_DIM", 0.25f);
    if (s_v.oled_dim > 0.5f) s_v.oled_dim = 0.5f;
    if (s_v.sharpen > 1.0f) s_v.sharpen = 1.0f;
    if (s_v.bloom > 1.0f) s_v.bloom = 1.0f;
    e = getenv("BW_VFX_SHOT_EVERY");
    s_v.shot_every = e ? atoi(e) : 0;
    s_v.tex_restore = vfx_flag(BW_FIX_VFX_TEX_RESTORE, "BW_TEXRESTORE", 1.0f) != 0.0f;
    e = getenv("BW_TEXRESTORE_DEBLOCK");
    s_v.tr_deblock = !(e && e[0] == '0');
    e = getenv("BW_TEXRESTORE_SCALE");
    s_v.tr_scale = (e && atoi(e) == 2) ? 2 : 4;
    e = getenv("BW_TEXRESTORE_DEBUG");
    s_v.tr_debug = s_v.tex_restore && e && e[0] == '1';
    e = getenv("BW_TEXRESTORE_MENUS");
    s_v.tr_menus = s_v.tex_restore && !(e && e[0] == '0');
    e = getenv("BW_TEXRESTORE_MOVIES");
    s_v.tr_movies = s_v.tex_restore && !(e && e[0] == '0');
    e = getenv("BW_TEXRESTORE_CUTOUT");
    s_v.tr_cutout_threshold = e && (e[0] == 't' || e[0] == 'T');
    e = getenv("BW_TEXRESTORE_MB");
    s_v.tr_budget_mb = (e && atoi(e) > 0) ? (unsigned)atoi(e) : 2048u;
    e = getenv("BW_GPU_PRESENT");
    s_v.offscreen = e && e[0] == '0';
    s_v.active = s_v.native_res || s_v.msaa > 1 || s_v.aniso > 1 || s_v.precision || s_v.text
              || s_v.hud_edges || s_v.fxaa || s_v.sharpen > 0.0f || s_v.bloom > 0.0f || s_v.hdr == 1 || s_v.oled_dim > 0.0f || s_v.lod > 1.0f || s_v.ao > 0.0f
              || s_v.tex_restore;
    fprintf(stderr, "[VFX] visual suite %s: native res %s%s, MSAA %ux%s, AF %ux, precision %s, text %s, HUD edges %s,"
                    " FXAA %s, sharpen %.2f, bloom %.2f, HDR %s (paper %.0f, peak %.0f nits until the screen is read),"
                    " OLED dim %.2f, AO %.2f, draw distance x%.1f, SSAA %.2f, guest AA %s%s\n",
            s_v.active ? "on" : "off", s_v.native_res ? "on" : "off",
            s_v.scale_force > 0.0f ? " (fixed scale)" : "", s_v.msaa,
            s_v.a2c ? " + alpha to coverage" : "", s_v.aniso,
            s_v.precision ? "16-bit" : "8-bit", s_v.text ? "on" : "off",
            s_v.hud_edges ? "on" : "off", s_v.fxaa ? "on" : "off",
            s_v.sharpen, s_v.bloom, s_v.hdr == 1 ? "on" : s_v.hdr == 2 ? "auto" : "off", s_v.hdr_paper, s_v.hdr_peak,
            s_v.oled_dim, s_v.ao, s_v.lod, s_v.ssaa, s_v.guest_aa ? "kept" : "native",
            s_v.offscreen ? ", off-screen output (BW_GPU_PRESENT=0)" : "");
    fprintf(stderr, "[VFX] texture restoration %s: deblock %s, enlarged x%d above painted size, menu photos %s,"
                    " movies %s, cut-out alpha %s, debug tint %s, VRAM budget %u MB\n",
            s_v.tex_restore ? "on" : "off", s_v.tr_deblock ? "on" : "off", s_v.tr_scale,
            s_v.tr_menus ? "on" : "off", s_v.tr_movies ? "on" : "off",
            s_v.tr_cutout_threshold ? "thresholded at 0.5" : "the game's (bilinear)",
            s_v.tr_debug ? "on" : "off",
            s_v.tr_budget_mb);
    fflush(stderr);
}

float bw_vfx_lod_scale(void)
{
    return s_v.active ? s_v.lod : 1.0f;
}

const BwVfx *bw_vfx(void)
{
    return &s_v;
}

/* ---- the render scale ------------------------------------------------------ */

static float s_scale = 1.0f;
static unsigned s_epoch = 1;
static unsigned s_win_w, s_win_h;

static float compute_scale(void)
{
    unsigned h;
    float s;
    if (!s_v.active || !s_v.native_res)
        return 1.0f;
    if (s_v.scale_force > 0.0f)
        s = s_v.scale_force;
    else {
        h = s_v.out_h ? s_v.out_h : s_win_h ? s_win_h : (s_v.offscreen ? 1440u : 960u);
        s = (float)h / 480.0f;
        s *= s_v.ssaa;
    }
    if (s < 0.5f) s = 0.5f;
    if (s > 10.0f) s = 10.0f;
    return s;
}

static float s_pic_aspect = 4.0f / 3.0f;
void bw_vfx_set_picture_aspect(float aspect)
{
    if (aspect > 0.5f && aspect < 5.0f)
        s_pic_aspect = aspect;
}
float bw_vfx_picture_aspect(void)
{
    return s_pic_aspect;
}

/* 0.2.5 item 32: a new output size counts only once it has held for
 * BW_VFX_RESIZE_MS (default 250 ms). Dragging the window's edge changes it
 * on every frame, and each change of the render scale rebuilds and re-seeds
 * every render target: about a second each in a Best Quality drag (run
 * 20261103-015538-r6-gpu-i30drag: 4 sizes in 5 s). The first size counts at
 * once; meanwhile the last frame's scale is drawn into the new window size.
 * BW_VFX_RESIZE_MS=0 counts every size at once, as before. */
static unsigned resize_settle_ms(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("BW_VFX_RESIZE_MS");
        v = e && *e ? atoi(e) : 250;
        if (v < 0) v = 0;
        if (v > 5000) v = 5000;
        if (v != 250)
            fprintf(stderr, "[VFX] a new output size counts after %d ms (BW_VFX_RESIZE_MS)\n", v);
    }
    return (unsigned)v;
}

void bw_vfx_set_output(unsigned w, unsigned h)
{
    static unsigned pend_w, pend_h;
    static ULONGLONG pend_t;
    float s;
    if (!s_v.active || !w || !h)
        return;
    if (w == s_win_w && h == s_win_h) {
        pend_w = pend_h = 0;            /* back to the size in use: nothing pending */
        return;
    }
    if (s_win_w && s_win_h && resize_settle_ms()) {
        ULONGLONG now = GetTickCount64();
        if (w != pend_w || h != pend_h) {
            pend_w = w;
            pend_h = h;
            pend_t = now;
            return;
        }
        if (now - pend_t < resize_settle_ms())
            return;
    }
    pend_w = pend_h = 0;
    s_win_w = w;
    s_win_h = h;
    s = compute_scale();
    if (fabsf(s - s_scale) > 0.001f) {
        s_scale = s;
        s_epoch++;
        fprintf(stderr, "[VFX] output %ux%u: render scale %.3f (the 640x480 screen at %.0fx%.0f)\n",
                w, h, s, 640.0f * s, 480.0f * s);
    }
}

float bw_vfx_scale(void)
{
    static int first = 1;
    if (first && s_v.active) {
        first = 0;
        s_scale = compute_scale();
        fprintf(stderr, "[VFX] render scale %.3f (the 640x480 screen at %.0fx%.0f)\n", s_scale,
                640.0f * s_scale, 480.0f * s_scale);
    }
    return s_v.active ? s_scale : 1.0f;
}

unsigned bw_vfx_epoch(void)
{
    return s_epoch;
}

void bw_vfx_surface_size(uint32_t w, uint32_t h, uint32_t aa_sx, uint32_t aa_sy,
                         uint32_t *pw, uint32_t *ph)
{
    float s = bw_vfx_scale(), sx = s, sy = s;
    if (!s_v.active || !s_v.native_res || s == 1.0f) {
        *pw = w;
        *ph = h;
        return;
    }
    /* A 2x-wide anti-aliasing surface (1280x480 for the 640x480 screen) is
     * physically the output's size: its extra columns were the console's
     * supersampling, which MSAA replaces at native resolution. */
    if (!s_v.guest_aa) {
        if (aa_sx > 1) sx /= (float)aa_sx;
        if (aa_sy > 1) sy /= (float)aa_sy;
    }
    *pw = (uint32_t)(w * sx + 0.5f);
    *ph = (uint32_t)(h * sy + 0.5f);
    if (*pw < 1) *pw = 1;
    if (*ph < 1) *ph = 1;
    if (*pw > 16384) *pw = 16384;
    if (*ph > 16384) *ph = 16384;
}

DXGI_FORMAT bw_vfx_target_format(uint32_t bpp)
{
    if (bpp == 2)
        return DXGI_FORMAT_B5G6R5_UNORM;     /* kept: its alpha always reads 1 */
    if (s_v.active && bw_vfx_hdr_highlights())     /* item 97: HDR highlights this session */
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (s_v.active && s_v.precision)
        return DXGI_FORMAT_R16G16B16A16_UNORM;
    return DXGI_FORMAT_B8G8R8A8_UNORM;
}

UINT bw_vfx_msaa(void)
{
    return s_v.active ? s_v.msaa : 1;
}

UINT bw_vfx_aniso(void)
{
    return s_v.active ? s_v.aniso : 1;
}

/* ---- glyph sheets ------------------------------------------------------------ */

static int near_white565(uint32_t c)
{
    return ((c >> 11) & 31) >= 27 && ((c >> 5) & 63) >= 54 && (c & 31) >= 27;
}

int bw_vfx_is_glyph_sheet(const uint8_t *p, uint32_t w, uint32_t h)
{
    uint32_t bw = w / 4, bh = h / 4, n = bw * bh, i, empty = 0, inked = 0, other = 0;
    if (!s_v.active || !s_v.text || !p || w < 128 || h < 64 || !n)
        return 0;
    for (i = 0; i < n; i++, p += 8) {
        uint32_t c0 = p[0] | (p[1] << 8), c1 = p[2] | (p[3] << 8);
        uint32_t idx = p[4] | (p[5] << 8) | (p[6] << 16) | ((uint32_t)p[7] << 24), k;
        int transparent = 0, solid = 0, bad = 0;
        for (k = 0; k < 16; k++) {
            uint32_t ix = (idx >> (2 * k)) & 3;
            if (c0 <= c1 && ix == 3) { transparent = 1; continue; }
            solid = 1;
            if (ix == 0 && !near_white565(c0)) bad = 1;
            else if (ix == 1 && !near_white565(c1)) bad = 1;
            else if (ix >= 2 && !(near_white565(c0) && near_white565(c1))) bad = 1;
        }
        if (bad) other++;
        else if (!solid && transparent) empty++;
        else if (solid && transparent) inked++;
    }
    /* White glyphs on transparency: nothing coloured, lots of empty space,
     * and glyph edges (blocks that are part ink, part transparent). */
    return other * 100 <= n && empty * 5 >= n && inked * 20 >= n;
}

/* ---- GPU helpers ---------------------------------------------------------------- */

ID3D11Device *g_vfx_dev;
ID3D11DeviceContext *g_vfx_ctx;
static ID3D11VertexShader *s_fs_vs;
static ID3D11PixelShader *s_blit_ps[2];
static ID3D11SamplerState *s_lin, *s_pt;
static ID3D11RasterizerState *s_rs;
static ID3D11BlendState *s_bs;
static ID3D11DepthStencilState *s_ds;
static ID3D11Buffer *s_cb;

/* A full-screen triangle, and the blits. The box filter averages the source
 * texels under each destination pixel with bilinear taps on a grid, so any
 * downsampling factor up to 8 per axis averages every texel once. */
static const char s_blit_src[] =
"cbuffer B : register(b0) { float4 srcsz; float4 dstsz; float4 p0; float4 p1; };\n"
"Texture2D t0 : register(t0);\n"
"SamplerState sl : register(s0); SamplerState sp : register(s1);\n"
"struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; };\n"
"V vs_full(uint id : SV_VertexID) {\n"
"  V o; float2 uv = float2((id << 1) & 2, id & 2);\n"
"  o.p = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);\n"
"  o.uv = uv; return o; }\n"
"float4 ps_bilinear(V i) : SV_Target { return t0.SampleLevel(sl, i.uv, 0); }\n"
"float4 ps_box(V i) : SV_Target {\n"
"  float2 f = srcsz.xy * dstsz.zw;\n"            /* source texels per destination pixel */
"  int nx = clamp((int)ceil(f.x / 2.0), 1, 4), ny = clamp((int)ceil(f.y / 2.0), 1, 4);\n"
"  float2 o = i.uv - 0.5 * dstsz.zw; float2 st = dstsz.zw / float2(nx, ny);\n"
"  float4 c = 0;\n"
"  for (int y = 0; y < ny; y++) for (int x = 0; x < nx; x++)\n"
"    c += t0.SampleLevel(sl, o + (float2(x, y) + 0.5) * st, 0);\n"
"  return c / (nx * ny); }\n";

static ID3D11PixelShader *mk_ps(const char *src, size_t len, const char *entry)
{
    ID3DBlob *b = NULL, *err = NULL;
    ID3D11PixelShader *ps = NULL;
    if (bw_gpu_failtest("vfxshader")) {          /* item 25 test aid */
        fprintf(stderr, "[VFX] shader %s not made (BW_GPU_FAILTEST)\n", entry);
        return NULL;
    }
    if (FAILED(D3DCompile(src, len, "bw_vfx", NULL, NULL, entry, "ps_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &b, &err))) {
        fprintf(stderr, "[VFX] shader %s failed: %s\n", entry,
                err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?");
        if (err) ID3D10Blob_Release(err);
        return NULL;
    }
    if (err) ID3D10Blob_Release(err);
    ID3D11Device_CreatePixelShader(g_vfx_dev, ID3D10Blob_GetBufferPointer(b),
                                   ID3D10Blob_GetBufferSize(b), NULL, &ps);
    ID3D10Blob_Release(b);
    return ps;
}

ID3D11PixelShader *bw_vfx_compile_ps(const char *src, size_t len, const char *entry)
{
    return mk_ps(src, len, entry);
}

ID3D11Buffer *bw_vfx_make_cb(UINT bytes)
{
    D3D11_BUFFER_DESC bd;
    ID3D11Buffer *b = NULL;
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = (bytes + 15) & ~15u;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Device_CreateBuffer(g_vfx_dev, &bd, NULL, &b);
    return b;
}

void bw_vfx_update_cb(ID3D11Buffer *b, const void *data, UINT bytes)
{
    D3D11_MAPPED_SUBRESOURCE m;
    if (b && SUCCEEDED(ID3D11DeviceContext_Map(g_vfx_ctx, (ID3D11Resource *)b, 0,
                                               D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, data, bytes);
        ID3D11DeviceContext_Unmap(g_vfx_ctx, (ID3D11Resource *)b, 0);
    }
}

int bw_vfx_gpu_init(ID3D11Device *dev, ID3D11DeviceContext *ctx)
{
    static int done, ok;
    ID3DBlob *vsb = NULL, *err = NULL;
    D3D11_SAMPLER_DESC sd;
    D3D11_RASTERIZER_DESC rd;
    D3D11_BLEND_DESC bd;
    D3D11_DEPTH_STENCIL_DESC dd;

    if (done)
        return ok;
    done = 1;
    g_vfx_dev = dev;
    g_vfx_ctx = ctx;
    if (!s_v.active)
        return 0;
    /* MSAA: every target format and the depth format must support the count. */
    while (s_v.msaa > 1) {
        UINT q1 = 0, q2 = 0, q3 = 0;
        ID3D11Device_CheckMultisampleQualityLevels(dev, bw_vfx_target_format(4), s_v.msaa, &q1);
        ID3D11Device_CheckMultisampleQualityLevels(dev, DXGI_FORMAT_B5G6R5_UNORM, s_v.msaa, &q2);
        ID3D11Device_CheckMultisampleQualityLevels(dev, DXGI_FORMAT_D24_UNORM_S8_UINT, s_v.msaa, &q3);
        if (q1 && q2 && q3)
            break;
        fprintf(stderr, "[VFX] MSAA %ux not supported by every target format; trying %ux\n",
                s_v.msaa, s_v.msaa / 2);
        s_v.msaa /= 2;
    }
    if (FAILED(D3DCompile(s_blit_src, sizeof s_blit_src - 1, "bw_vfx", NULL, NULL, "vs_full",
                          "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vsb, &err))) {
        fprintf(stderr, "[VFX] vertex shader failed: %s\n",
                err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?");
        if (err) ID3D10Blob_Release(err);
        return 0;
    }
    ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(vsb),
                                    ID3D10Blob_GetBufferSize(vsb), NULL, &s_fs_vs);
    ID3D10Blob_Release(vsb);
    s_blit_ps[0] = mk_ps(s_blit_src, sizeof s_blit_src - 1, "ps_bilinear");
    s_blit_ps[1] = mk_ps(s_blit_src, sizeof s_blit_src - 1, "ps_box");
    memset(&sd, 0, sizeof sd);
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    ID3D11Device_CreateSamplerState(dev, &sd, &s_lin);
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    ID3D11Device_CreateSamplerState(dev, &sd, &s_pt);
    memset(&rd, 0, sizeof rd);
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    ID3D11Device_CreateRasterizerState(dev, &rd, &s_rs);
    memset(&bd, 0, sizeof bd);
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_ZERO;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ID3D11Device_CreateBlendState(dev, &bd, &s_bs);
    memset(&dd, 0, sizeof dd);
    dd.DepthEnable = FALSE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    ID3D11Device_CreateDepthStencilState(dev, &dd, &s_ds);
    s_cb = bw_vfx_make_cb(64);
    ok = s_fs_vs && s_blit_ps[0] && s_blit_ps[1] && s_lin && s_pt && s_rs && s_bs && s_ds && s_cb;
    if (!ok) {
        fprintf(stderr, "[VFX] GPU helpers failed to initialise; the suite stays off\n");
        s_v.active = 0;
    }
    return ok;
}

ID3D11SamplerState *bw_vfx_linear_sampler(void) { return s_lin; }
ID3D11SamplerState *bw_vfx_point_sampler(void) { return s_pt; }

/* Common full-screen pass setup: no blending, depth or culling, one viewport. */
void bw_vfx_fullscreen(ID3D11RenderTargetView *dst, const D3D11_VIEWPORT *vp,
                       ID3D11PixelShader *ps, ID3D11Buffer *cb,
                       ID3D11ShaderResourceView *const *srv, UINT nsrv)
{
    bw_vfx_fullscreen_bs(dst, vp, ps, cb, srv, nsrv, s_bs);
}

void bw_vfx_fullscreen_bs(ID3D11RenderTargetView *dst, const D3D11_VIEWPORT *vp,
                          ID3D11PixelShader *ps, ID3D11Buffer *cb,
                          ID3D11ShaderResourceView *const *srv, UINT nsrv, ID3D11BlendState *bs)
{
    ID3D11SamplerState *ss[2] = { s_lin, s_pt };
    ID3D11ShaderResourceView *none[8] = { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL };
    ID3D11DeviceContext *c = g_vfx_ctx;
    ID3D11DeviceContext_PSSetShaderResources(c, 0, 8, none);
    ID3D11DeviceContext_OMSetRenderTargets(c, 1, &dst, NULL);
    ID3D11DeviceContext_RSSetViewports(c, 1, vp);
    ID3D11DeviceContext_IASetInputLayout(c, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(c, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(c, s_fs_vs, NULL, 0);
    ID3D11DeviceContext_GSSetShader(c, NULL, NULL, 0);
    ID3D11DeviceContext_PSSetShader(c, ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(c, 0, 1, &cb);
    ID3D11DeviceContext_PSSetShaderResources(c, 0, nsrv, srv);
    ID3D11DeviceContext_PSSetSamplers(c, 0, 2, ss);
    ID3D11DeviceContext_RSSetState(c, s_rs);
    ID3D11DeviceContext_OMSetBlendState(c, bs ? bs : s_bs, NULL, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(c, s_ds, 0);
    ID3D11DeviceContext_Draw(c, 3, 0);
    ID3D11DeviceContext_PSSetShaderResources(c, 0, 8, none);
}

void bw_vfx_blit(ID3D11ShaderResourceView *src, UINT sw, UINT sh,
                 ID3D11RenderTargetView *dst, UINT dw, UINT dh, int mode)
{
    float k[16];
    D3D11_VIEWPORT vp;
    if (!s_fs_vs || !src || !dst || !dw || !dh)
        return;
    memset(k, 0, sizeof k);
    k[0] = (float)sw; k[1] = (float)sh; k[2] = 1.0f / sw; k[3] = 1.0f / sh;
    k[4] = (float)dw; k[5] = (float)dh; k[6] = 1.0f / dw; k[7] = 1.0f / dh;
    bw_vfx_update_cb(s_cb, k, sizeof k);
    vp.TopLeftX = 0; vp.TopLeftY = 0;
    vp.Width = (float)dw; vp.Height = (float)dh;
    vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
    bw_vfx_fullscreen(dst, &vp, s_blit_ps[mode ? 1 : 0], s_cb, &src, 1);
}
