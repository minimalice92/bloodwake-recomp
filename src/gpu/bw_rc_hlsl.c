/*
 * bw_rc_hlsl.c - HLSL pixel shaders from NV2A register-combiner state.
 *
 * Generates, for one combiner configuration, the shader that does on the GPU
 * what the toolkit's CPU evaluator (xboxrecomp src/kernel/nv2a_combiner.c,
 * itself after xemu's psh.c) does per pixel: the texture stages' fetch by
 * SHADER_STAGE_PROGRAM mode, up to eight general combiner stages over the
 * 16-register file, and the final combiner. Rule for rule the same, so the
 * GPU back end draws what the CPU rasteriser draws; the comments name the
 * CPU code each part mirrors.
 *
 * Known gaps: the bump-environment modes, and the dot-product modes other
 * than DOTPRODUCT and DOT_RFLCT_DIFF/SPEC, read zero; a cube-map stage whose
 * texture isn't a six-face cube samples face 0 as 2D, as the CPU path does.
 * The CPU path (rc_stage_fetch) reads every dot-product mode as zero, so
 * those stages are where the two differ.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bw_rc_hlsl.h"
#include "bw_edition.h"

/* P2a (docs/day1/P2a.md section 7), a Remastered test aid, off by default:
 * BW_OCEAN_DEBUGVIEW=<what> replaces the output of the sea's sky-reflection
 * pass (the only program whose stage 3 is DOT_RFLCT_SPEC on a cube, TK-34)
 * with an opaque false colour of one of its inputs, so a frame shows which
 * factor makes a feature. The shader text differs, so the persistent shader
 * cache keeps it apart. */
static int ocean_debugview(void)
{
    static int m = -1;
    if (m < 0) {
        static const char *const names[] = { "", "weight", "bump", "refl", "rz", "eye",
                                             "elen", "cube", "nn", "vnorm", "diff", "wraw", "d0", "ne", "out", "oa", "t0", "t0a", "count", "grey", "q25", "b50", "w0" };
        const char *e = getenv("BW_OCEAN_DEBUGVIEW");
        int i;
        m = 0;
        if (e && *e && bw_edition_at_least(BW_ED_REMASTERED))
            for (i = 1; i < (int)(sizeof names / sizeof names[0]); i++)
                if (!strcmp(e, names[i]))
                    m = i;
    }
    return m;
}

/* The alpha each view outputs (blended by the pass's own SRC_ALPHA /
 * INV_SRC_ALPHA): opaque, except the calibration views. */
static const char *ocean_debugalpha(void)
{
    const char *e = getenv("BW_OCEAN_DEBUGVIEW");
    if (!e) return "1.0";
    if (!strcmp(e, "count") || !strcmp(e, "q25")) return "0.25";   /* white: 1 draw ~ +25%, 2 ~ +44% */
    if (!strcmp(e, "grey") || !strcmp(e, "b50")) return "0.5";
    if (!strcmp(e, "w0")) return "0.0";
    return "1.0";
}

typedef struct {
    char *p;
    size_t n, cap;
    int overflow;
} Buf;

static void put(Buf *b, const char *fmt, ...)
{
    va_list ap;
    int w;
    if (b->overflow)
        return;
    va_start(ap, fmt);
    w = vsnprintf(b->p + b->n, b->cap - b->n, fmt, ap);
    va_end(ap);
    if (w < 0 || (size_t)w >= b->cap - b->n) {
        b->overflow = 1;
        return;
    }
    b->n += (size_t)w;
}

/* read_rgb / read_alpha of nv2a_combiner.c, for one input byte. */
static void put_rgb(Buf *b, const char *var, uint32_t byte)
{
    put(b, "  float3 %s = rd3(R[%u], %d, %u);\n", var, byte & 0xF,
        (byte & 0x10) ? 1 : 0, (byte >> 5) & 7);
}

static void put_alpha(Buf *b, const char *var, uint32_t byte)
{
    put(b, "  float %s = rda(R[%u], %d, %u);\n", var, byte & 0xF,
        (byte & 0x10) ? 1 : 0, (byte >> 5) & 7);
}

size_t bw_rc_hlsl(const BwRcKey *k, char *out, size_t cap)
{
    Buf b = { out, 0, cap, 0 };
    uint32_t n = k->control & 0xFF, flags = k->control >> 8;
    int st, trdbg;
    uint32_t s;

    if (n > 8)
        n = 8;
    put(&b, BW_RC_CBUFFER);
    /* A stage the back end bound a six-face cube map to is a TextureCube. */
    for (st = 0; st < 4; st++)
        put(&b, "%s t%d : register(t%d);\n", k->tex_cube[st] ? "TextureCube" : "Texture2D", st, st);
    put(&b,
        "SamplerState s0 : register(s0); SamplerState s1 : register(s1);\n"
        "SamplerState s2 : register(s2); SamplerState s3 : register(s3);\n");
    /* The colours' interpolation (BW_RC_PERSP): see bw_rc_hlsl.h. FOG1 is the
     * unclamped fog factor the vertex shaders add after the coordinates. */
    put(&b, "struct PSIn { float4 p : SV_Position; %sfloat4 d0 : COLOR0;\n"
            "  %sfloat4 d1 : COLOR1; noperspective float fog : FOG;\n"
            "  float4 tc0 : TEXCOORD0; float4 tc1 : TEXCOORD1;\n"
            "  float4 tc2 : TEXCOORD2; float4 tc3 : TEXCOORD3; float fogp : FOG1; };\n",
        (k->flags[0] & BW_RC_PERSP) ? "" : "noperspective ",
        (k->flags[0] & BW_RC_PERSP) ? "" : "noperspective ");
    put(&b,
        /* map_in */
        "float mi(float x, int m) { float p = max(x, 0.0);\n"
        "  if (m == 0) return p; if (m == 1) return 1.0 - saturate(x);\n"
        "  if (m == 2) return 2.0 * p - 1.0; if (m == 3) return -2.0 * p + 1.0;\n"
        "  if (m == 4) return p - 0.5; if (m == 5) return -p + 0.5;\n"
        "  if (m == 6) return x; return -x; }\n"
        "float3 rd3(float4 r, int arep, int m) { float3 v = arep ? r.aaa : r.rgb;\n"
        "  return float3(mi(v.x, m), mi(v.y, m), mi(v.z, m)); }\n"
        "float rda(float4 r, int arep, int m) { return mi(arep ? r.a : r.b, m); }\n"
        /* DOT_RGBMAPPING (xemu psh.c sign1/2/3): 0 zero-to-one, 1 -1..1 D3D,
         * 2 -1..1 GL, 3 -1..1 signed; the HILO modes (4-7) need 16-bit
         * texels this back end doesn't keep, so they read as 0..1. */
        "float3 dm(float4 c, uint m) { float3 x = c.rgb * 255.0;\n"
        "  if (m == 1) return (x - 128.0) / 127.0;\n"
        "  if (m == 2) return x >= 128.0 ? (x - 255.5) / 127.5 : (x + 0.5) / 127.5;\n"
        "  if (m == 3) return x >= 128.0 ? (x - 256.0) / 127.0 : x / 127.0;\n"
        "  return c.rgb; }\n"
        /* map_out */
        "float3 mo(float3 x, int m) {\n"
        "  if (m == 0x08) return x - 0.5; if (m == 0x10) return x * 2.0;\n"
        "  if (m == 0x18) return (x - 0.5) * 2.0; if (m == 0x20) return x * 4.0;\n"
        "  if (m == 0x30) return x * 0.5; return x; }\n"
        );
    /* VFX (D9), Remastered only (Original's keys have no vfx flags, so its
     * shader text is unchanged): the contour filter for magnified glyph
     * sheets. The game's glyphs are white on 1-bit alpha (DXT1). The alpha is
     * reconstructed with a cubic B-spline (four bilinear taps), whose 0.5
     * crossing follows the bitmap's own edges on straight runs (a 2-texel
     * stroke keeps its exact width, a 1-texel one 95% of it) but turns its
     * stair-steps into smooth curves; that crossing is drawn with a
     * one-pixel ramp, so edges are crisp and anti-aliased at any size. The
     * colour is un-premultiplied (transparent texels are black), so the edge
     * stays white instead of a magnified grey fringe. Not magnified (one
     * texel per pixel or more): exactly the game's own sample. */
    if (k->vfx[0] | k->vfx[1] | k->vfx[2] | k->vfx[3])
        put(&b,
            "SamplerState sg : register(s4);\n"
            "float4 glyph(Texture2D t, SamplerState s, float2 uv) {\n"
            "  float4 g = t.Sample(s, uv);\n"
            "  float2 sz; t.GetDimensions(sz.x, sz.y);\n"
            "  float2 p = uv * sz - 0.5; float2 f = frac(p); float2 i0 = floor(p);\n"
            "  float2 f2 = f * f, f3 = f2 * f;\n"
            "  float2 w0 = (1.0 - 3.0 * f + 3.0 * f2 - f3) / 6.0, w1 = (4.0 - 6.0 * f2 + 3.0 * f3) / 6.0;\n"
            "  float2 w2 = (1.0 + 3.0 * f + 3.0 * f2 - 3.0 * f3) / 6.0, w3 = f3 / 6.0;\n"
            "  float2 g0 = w0 + w1, g1 = w2 + w3;\n"
            "  float2 h0 = (i0 - 0.5 + w1 / g0) / sz, h1 = (i0 + 1.5 + w3 / g1) / sz;\n"
            "  float4 c = g0.y * (g0.x * t.Sample(sg, float2(h0.x, h0.y)) + g1.x * t.Sample(sg, float2(h1.x, h0.y)))\n"
            "           + g1.y * (g0.x * t.Sample(sg, float2(h0.x, h1.y)) + g1.x * t.Sample(sg, float2(h1.x, h1.y)));\n"
            "  float2 d = fwidth(uv * sz); float tpp = max(max(d.x, d.y), 1e-5);\n"
            "  float aw = clamp(fwidth(c.a), 0.4 * tpp, 2.0 * tpp);\n"
            "  float a = saturate((c.a - 0.5) / aw + 0.5);\n"
            "  float3 rgb = c.a > 0.004 ? saturate(c.rgb / c.a) : g.rgb;\n"
            "  return tpp < 0.75 ? float4(rgb, a) : g; }\n"
            /* HUD art with 1-bit alpha (A1R5G5B5, DXT1): the same contour;
             * the colour is the bilinear mix of the 2x2 texels weighted by
             * their own alpha, so transparent texels (whatever colour they
             * hold) never darken or tint the edge. */
            "float4 ovl(Texture2D t, SamplerState s, float2 uv) {\n"
            "  float4 g = t.Sample(s, uv);\n"
            "  float2 sz; t.GetDimensions(sz.x, sz.y);\n"
            "  float2 p = uv * sz - 0.5; float2 f = frac(p); float2 i0 = floor(p);\n"
            "  float2 f2 = f * f, f3 = f2 * f;\n"
            "  float2 w0 = (1.0 - 3.0 * f + 3.0 * f2 - f3) / 6.0, w1 = (4.0 - 6.0 * f2 + 3.0 * f3) / 6.0;\n"
            "  float2 w2 = (1.0 + 3.0 * f + 3.0 * f2 - 3.0 * f3) / 6.0, w3 = f3 / 6.0;\n"
            "  float2 g0 = w0 + w1, g1 = w2 + w3;\n"
            "  float2 h0 = (i0 - 0.5 + w1 / g0) / sz, h1 = (i0 + 1.5 + w3 / g1) / sz;\n"
            "  float ca = g0.y * (g0.x * t.Sample(sg, float2(h0.x, h0.y)).a + g1.x * t.Sample(sg, float2(h1.x, h0.y)).a)\n"
            "           + g1.y * (g0.x * t.Sample(sg, float2(h0.x, h1.y)).a + g1.x * t.Sample(sg, float2(h1.x, h1.y)).a);\n"
            "  float4 R = t.GatherRed(sg, uv), G = t.GatherGreen(sg, uv), B = t.GatherBlue(sg, uv), A = t.GatherAlpha(sg, uv);\n"
            "  float4 w = float4((1.0 - f.x) * f.y, f.x * f.y, f.x * (1.0 - f.y), (1.0 - f.x) * (1.0 - f.y)) * A;\n"
            "  float sw = dot(w, 1.0);\n"
            "  float3 rgb = sw > 1e-4 ? float3(dot(w, R), dot(w, G), dot(w, B)) / sw : g.rgb;\n"
            "  float2 d = fwidth(uv * sz); float tpp = max(max(d.x, d.y), 1e-5);\n"
            "  float aw = clamp(fwidth(ca), 0.4 * tpp, 2.0 * tpp);\n"
            "  float a = saturate((ca - 0.5) / aw + 0.5);\n"
            "  return tpp < 0.75 ? float4(rgb, a) : g; }\n"
            /* Any other HUD art, magnified: the game samples some of it
             * point-filtered (each texel a block at 3-4.5x) and some
             * bilinear (soft). Catmull-Rom over the 4x4 texels, each
             * premultiplied by its alpha (so transparent texels add no
             * colour), clamped to the 2x2 texels around the point (no
             * ringing): smooth edges, the art's own detail kept sharp. */
            "float4 ovc(Texture2D t, SamplerState s, float2 uv) {\n"
            "  float4 g = t.Sample(s, uv);\n"
            "  float2 sz; t.GetDimensions(sz.x, sz.y);\n"
            "  float2 p = uv * sz - 0.5; float2 i0 = floor(p); float2 f = p - i0;\n"
            "  float2 wa = f * (-0.5 + f * (1.0 - 0.5 * f)), wb = 1.0 + f * f * (-2.5 + 1.5 * f);\n"
            "  float2 wc = f * (0.5 + f * (2.0 - 1.5 * f)), wd = f * f * (-0.5 + 0.5 * f);\n"
            "  float4 wx = float4(wa.x, wb.x, wc.x, wd.x), wy = float4(wa.y, wb.y, wc.y, wd.y);\n"
            "  int2 mx = int2(sz) - 1; float4 acc = 0, lo = 1e9, hi = -1e9;\n"
            "  [unroll] for (int y = 0; y < 4; y++) [unroll] for (int x = 0; x < 4; x++) {\n"
            "    float4 c = t.Load(int3(clamp(int2(i0) + int2(x - 1, y - 1), 0, mx), 0));\n"
            "    c.rgb *= c.a; acc += c * (wx[x] * wy[y]);\n"
            "    if (x == 1 || x == 2) if (y == 1 || y == 2) { lo = min(lo, c); hi = max(hi, c); } }\n"
            "  acc = clamp(acc, lo, hi);\n"
            "  float3 rgb = acc.a > 1e-4 ? saturate(acc.rgb / acc.a) : g.rgb;\n"
            "  float2 d = fwidth(uv * sz); float tpp = max(max(d.x, d.y), 1e-5);\n"
            "  return tpp < 0.75 ? float4(rgb, saturate(acc.a)) : g; }\n"
            /* V1 (VFX_HUD_EDGES): hard-edged HUD art (opaque shapes with a
             * one-texel anti-aliased rim; the upload decides from the alpha
             * blocks). The rim texels' colour is dark and their alpha a
             * staircase, so magnified 4.5x (one texel per guest pixel on the
             * console) the edge turned into beads with a dark notch at every
             * step (the radar's compass ring). Alpha: half a cubic B-spline
             * over the 4x4 texels (its 0.5 crossing follows the art's edge
             * but rounds its stair-steps) and half Catmull-Rom clamped to the
             * 2x2 (keeps small corners, the turbo bar's chevrons, pointed);
             * BW_VFX_HUDEDGE_MIX=0..1 sets the share (0 rounder, 1 crisper).
             * The 0.5 crossing is drawn with a one-pixel ramp (as the glyph
             * contour). Colour: the rim texels are darkened about in
             * proportion to their alpha (premultiplied against black; seen in
             * HUD_COMPASS: alpha 0.81 with about 0.8x the band's colour), and
             * the game blends them as straight alpha, so the rim is doubly
             * dark: a faint one-pixel outline at 1:1, beads when magnified.
             * Each texel is un-premultiplied (rgb / alpha), Catmull-Rom'd with
             * alpha-squared weights (faint texels barely count) and clamped to
             * the colour range of the most opaque texels of the 2x2 around
             * the point (alpha >= 0.9, else >= 0.5, else all four): the
             * shape's own colour reaches its edge, and the art's detail
             * inside stays sharp (opaque texels are unchanged). */
            "float4 ovh(Texture2D t, SamplerState s, float2 uv) {\n"
            "  float4 g = t.Sample(s, uv);\n"
            "  float2 sz; t.GetDimensions(sz.x, sz.y);\n"
            "  float2 p = uv * sz - 0.5; float2 i0 = floor(p); float2 f = p - i0;\n"
            "  float2 f2 = f * f, f3 = f2 * f;\n"
            "  float4 bx = float4((1.0 - 3.0 * f.x + 3.0 * f2.x - f3.x) / 6.0, (4.0 - 6.0 * f2.x + 3.0 * f3.x) / 6.0,"
            " (1.0 + 3.0 * f.x + 3.0 * f2.x - 3.0 * f3.x) / 6.0, f3.x / 6.0);\n"
            "  float4 by = float4((1.0 - 3.0 * f.y + 3.0 * f2.y - f3.y) / 6.0, (4.0 - 6.0 * f2.y + 3.0 * f3.y) / 6.0,"
            " (1.0 + 3.0 * f.y + 3.0 * f2.y - 3.0 * f3.y) / 6.0, f3.y / 6.0);\n"
            "  float2 wa = f * (-0.5 + f * (1.0 - 0.5 * f)), wb = 1.0 + f * f * (-2.5 + 1.5 * f);\n"
            "  float2 wc = f * (0.5 + f * (2.0 - 1.5 * f)), wd = f * f * (-0.5 + 0.5 * f);\n"
            "  float4 wx = float4(wa.x, wb.x, wc.x, wd.x), wy = float4(wa.y, wb.y, wc.y, wd.y);\n"
            "  int2 mx = int2(sz) - 1; float ca = 0, cr = 0, alo = 1, ahi = 0; float4 acc = 0; float amax = 0; float3 best = g.rgb;\n"
            "  float3 l9 = 1e9, h9 = -1e9, l5 = 1e9, h5 = -1e9, la = 1e9, ha = -1e9;\n"
            "  [unroll] for (int y = 0; y < 4; y++) [unroll] for (int x = 0; x < 4; x++) {\n"
            "    float4 c = t.Load(int3(clamp(int2(i0) + int2(x - 1, y - 1), 0, mx), 0));\n"
            "    ca += c.a * (bx[x] * by[y]); cr += c.a * (wx[x] * wy[y]);\n"
            "    if (x == 1 || x == 2) if (y == 1 || y == 2) { alo = min(alo, c.a); ahi = max(ahi, c.a); }\n"
            "    float3 u = c.a > 0.004 ? saturate(c.rgb / c.a) : c.rgb;\n"
            "    float k = c.a * c.a; acc += float4(u * k, k) * (wx[x] * wy[y]);\n"
            "    if (x == 1 || x == 2) if (y == 1 || y == 2) {\n"
            "      la = min(la, u); ha = max(ha, u);\n"
            "      if (c.a >= 0.5) { l5 = min(l5, u); h5 = max(h5, u); }\n"
            "      if (c.a >= 0.9) { l9 = min(l9, u); h9 = max(h9, u); }\n"
            "      if (c.a > amax) { amax = c.a; best = u; } } }\n"
            "  float3 lo = h9.x >= l9.x ? l9 : h5.x >= l5.x ? l5 : la;\n"
            "  float3 hi = h9.x >= l9.x ? h9 : h5.x >= l5.x ? h5 : ha;\n"
            "  float3 rgb = acc.a > 1e-3 ? clamp(acc.rgb / acc.a, lo, hi) : best;\n"
            "  float2 d = fwidth(uv * sz); float tpp = max(max(d.x, d.y), 1e-5);\n");
    if (k->vfx[0] | k->vfx[1] | k->vfx[2] | k->vfx[3]) {      /* ovh, continued */
        static float mix = -1.0f;
        if (mix < 0.0f) {
            const char *e = getenv("BW_VFX_HUDEDGE_MIX");
            float v = e ? (float)atof(e) : 0.5f;   /* 0 rounder (B-spline) .. 1 crisper (Catmull-Rom) */
            /* 0.2.5 item 41: in range, or every shader with it failed to
             * compile ("nan" in the text) and was compiled again per draw */
            if (v != v || v > 1e30f || v < -1e30f) {
                fprintf(stderr, "[VFX] BW_VFX_HUDEDGE_MIX=%s is not a number from 0 to 1: 0.5 used\n", e);
                v = 0.5f;
            } else if (v < 0.0f || v > 1.0f) {
                fprintf(stderr, "[VFX] BW_VFX_HUDEDGE_MIX=%s is outside 0..1: %.0f used\n", e, v < 0.0f ? 0.0 : 1.0);
                v = v < 0.0f ? 0.0f : 1.0f;
            }
            mix = v;
        }
        put(&b,
            "  ca = lerp(ca, clamp(cr, alo, ahi), %.3f);\n"
            "  float aw = clamp(fwidth(ca), 0.4 * tpp, 2.0 * tpp);\n"
            "  float a = saturate((ca - 0.5) / aw + 0.5);\n"
            "  return tpp < 0.75 ? float4(rgb, a) : g; }\n", mix);
        /* T1 (VFX_TEX_RESTORE, menu photos): the JPEG screens, magnified:
         * Lanczos-2 over the 4x4 texels (each axis normalised), premultiplied
         * by alpha (the arena photos sit on transparent black), clamped to the
         * 2x2 texels around the point (no ringing), no sharpening. As ovc with
         * Lanczos-2 weights instead of Catmull-Rom. */
        put(&b,
            "float l2w(float x) { x = abs(x); if (x < 1e-6) return 1.0; if (x >= 2.0) return 0.0;\n"
            "  float a = 3.14159265358979 * x; return 2.0 * sin(a) * sin(0.5 * a) / (a * a); }\n"
            "float4 ovz(Texture2D t, SamplerState s, float2 uv) {\n"
            "  float4 g = t.Sample(s, uv);\n"
            "  float2 sz; t.GetDimensions(sz.x, sz.y);\n"
            "  float2 p = uv * sz - 0.5; float2 i0 = floor(p); float2 f = p - i0;\n"
            "  float4 wx = float4(l2w(f.x + 1.0), l2w(f.x), l2w(1.0 - f.x), l2w(2.0 - f.x));\n"
            "  float4 wy = float4(l2w(f.y + 1.0), l2w(f.y), l2w(1.0 - f.y), l2w(2.0 - f.y));\n"
            "  wx /= dot(wx, 1.0); wy /= dot(wy, 1.0);\n"
            "  int2 mx = int2(sz) - 1; float4 acc = 0, lo = 1e9, hi = -1e9;\n"
            "  [unroll] for (int y = 0; y < 4; y++) [unroll] for (int x = 0; x < 4; x++) {\n"
            "    float4 c = t.Load(int3(clamp(int2(i0) + int2(x - 1, y - 1), 0, mx), 0));\n"
            "    c.rgb *= c.a; acc += c * (wx[x] * wy[y]);\n"
            "    if (x == 1 || x == 2) if (y == 1 || y == 2) { lo = min(lo, c); hi = max(hi, c); } }\n"
            "  acc = clamp(acc, lo, hi);\n"
            "  float3 rgb = acc.a > 1e-4 ? saturate(acc.rgb / acc.a) : g.rgb;\n"
            "  float2 d = fwidth(uv * sz); float tpp = max(max(d.x, d.y), 1e-5);\n"
            "  return tpp < 0.75 ? float4(rgb, saturate(acc.a)) : g; }\n");
    }
    put(&b,
        "float4 ps_main(PSIn i) : SV_Target {\n"
        "  float4 R[16];\n"
        "  float4 tc[4] = { i.tc0, i.tc1, i.tc2, i.tc3 };\n"
        "  float4 T[4];\n"
        "  float dp[4] = { 0.0, 0.0, 0.0, 0.0 };\n");
    /* T1 (BW_TEXRESTORE_DEBUG=1): the LOD the first restored stage samples
     * at, and its chain's level that holds the game's own level 0. */
    trdbg = -1;
    for (st = 0; st < 4 && trdbg < 0; st++)
        if ((k->vfx[st] & BW_RC_VFX_TR_DEBUG) && k->tex_valid[st] && !k->tex_cube[st])
            trdbg = st;
    if (trdbg >= 0)
        put(&b, "  float trl = -100.0, trt = 0.0;\n");

    /* rc_stage_fetch, per stage. */
    for (st = 0; st < 4; st++) {
        uint32_t mode = (k->stage_program >> (st * 5)) & 0x1F;
        switch (mode) {
        case 0:
            put(&b, "  T[%d] = float4(0, 0, 0, 1);\n", st);
            break;
        case 1: case 2: case 3:
            if (!k->tex_valid[st]) {
                put(&b, "  T[%d] = float4(1, 1, 1, 1);\n", st);
                break;
            }
            if (mode == 3 && k->tex_cube[st]) {
                /* CUBEMAP: the coordinate is a direction; D3D picks the face
                 * the same way the CPU path's rc_stage_fetch does. */
                put(&b, "  T[%d] = t%d.Sample(s%d, tc[%d].xyz);\n", st, st, st, st);
                break;
            }
            if (mode == 3)          /* CUBEMAP on a 2D texture: face 0, as the CPU path */
                put(&b, "  { float2 uv = tc[%d].xy;\n", st);
            else
                put(&b, "  { float q = tc[%d].w != 0.0 ? tc[%d].w : 1.0;"
                        " float2 uv = tc[%d].xy / q;\n", st, st, st);
            if (!k->tex_normalized[st])
                put(&b, "    uv *= texinfo[%d].xy;\n", st);
            if (k->tex_conv[st] && mode != 3) {
                /* CONVOLUTION_2D_LOD0 (the anti-aliasing resolve): as xemu
                 * psh.c, a 3x3 Gaussian of filtered taps one texel apart,
                 * for both kernels (xemu does not model quincunx apart). */
                put(&b, "    T[%d] = 0; { int2 o; [unroll] for (o.y = -1; o.y <= 1; o.y++)"
                        " [unroll] for (o.x = -1; o.x <= 1; o.x++)\n"
                        "      T[%d] += t%d.Sample(s%d, uv + o * texinfo[%d].xy)"
                        " * ((2 - abs(o.x)) * (2 - abs(o.y)) / 16.0); } }\n", st, st, st, st, st);
                break;
            }
            if ((k->vfx[st] & BW_RC_VFX_GLYPH) && !k->tex_cube[st])     /* VFX (D9) */
                put(&b, "    T[%d] = glyph(t%d, s%d, uv); }\n", st, st, st);
            else if ((k->vfx[st] & BW_RC_VFX_OVERLAY) && !k->tex_cube[st])
                put(&b, "    T[%d] = ovl(t%d, s%d, uv); }\n", st, st, st);
            else if ((k->vfx[st] & BW_RC_VFX_OVERLAY_HARD) && !k->tex_cube[st])   /* V1 */
                put(&b, "    T[%d] = ovh(t%d, s%d, uv); }\n", st, st, st);
            else if ((k->vfx[st] & BW_RC_VFX_OVERLAY_CR) && !k->tex_cube[st])
                put(&b, "    T[%d] = ovc(t%d, s%d, uv); }\n", st, st, st);
            else if ((k->vfx[st] & BW_RC_VFX_OVERLAY_L2) && !k->tex_cube[st])    /* T1 */
                put(&b, "    T[%d] = ovz(t%d, s%d, uv); }\n", st, st, st);
            else {
                if (st == trdbg && mode != 3)                                    /* T1 */
                    put(&b, "    { float2 sz; t%d.GetDimensions(sz.x, sz.y);"
                            " trt = log2(max(sz.x * texinfo[%d].x, 1.0));"
                            " trl = t%d.CalculateLevelOfDetail(s%d, uv); }\n", st, st, st, st);
                put(&b, "    T[%d] = t%d.Sample(s%d, uv); }\n", st, st, st);
            }
            break;
        case 4:
            put(&b, "  T[%d] = saturate(tc[%d]);\n", st, st);
            break;
        case 0x11:                  /* DOTPRODUCT: a scalar for the stages after it */
        case 0x0B:                  /* DOT_RFLCT_DIFF: cube at N = (dp1, dp2, dp3) */
        case 0x0C: {                /* DOT_RFLCT_SPEC: cube at N reflected about the eye */
            /* NV_texture_shader / xemu psh.c: each dot stage dots its own
             * (s, t, r) with the colour of an earlier stage (stage 1 reads
             * stage 0; 2 and 3 as OTHER_STAGE_INPUT says), mapped to a
             * vector by DOT_RGBMAPPING. The eye vector of SPEC is the three
             * stages' q. The diffuse stage needs stage 3's dot as well. */
            int j, n = (mode == 0x0B) ? 2 : 1;
            put(&b, "  T[%d] = float4(0, 0, 0, 0);\n", st);
            for (j = st; j < st + n && j < 4; j++) {
                uint32_t in, map;
                if (j < 1)              /* before the shift by (j - 1) * 4 */
                    break;
                in = j == 1 ? 0 : (k->other_stage_input >> (j == 2 ? 16 : 20)) & 0xF;
                map = (k->dot_rgbmapping >> ((j - 1) * 4)) & 0xF;
                if (in >= (uint32_t)j)
                    in = 0;
                put(&b, "  dp[%d] = dot(tc[%d].xyz, dm(T[%u], %u));\n", j, j, in, map);
            }
            if (mode == 0x0B && st == 2 && k->tex_cube[st])
                put(&b, "  T[2] = t2.Sample(s2, float3(dp[1], dp[2], dp[3]));\n");
            if (mode == 0x0C && st == 3 && k->tex_cube[st])
                put(&b, "  { float3 n = float3(dp[1], dp[2], dp[3]);"
                        " float3 e = float3(tc[1].w, tc[2].w, tc[3].w);\n"
                        "    float nn = dot(n, n); float3 r = nn != 0.0 ? 2.0 * n * dot(n, e) / nn - e : e;\n"
                        "    T[3] = t3.Sample(s3, r); }\n");
            break;
        }
        case 5: {
            int j;
            put(&b, "  T[%d] = float4(0, 0, 0, 0);\n", st);
            for (j = 0; j < 4; j++) {
                int ge = (k->clip_plane_mode >> (st * 4 + j)) & 1;
                put(&b, "  if (tc[%d][%d] %s 0.0) discard;\n", st, j, ge ? ">=" : "<");
            }
            break;
        }
        default:
            put(&b, "  T[%d] = float4(0, 0, 0, 0);\n", st);
            break;
        }
    }

    /* The register file (nv2a_rc_eval's start). */
    put(&b,
        "  R[0] = 0; R[1] = 0; R[2] = 0; R[6] = 0; R[7] = 0;\n"
        "  R[12] = 0; R[13] = 0; R[14] = 0; R[15] = 0;\n"
        "  R[3] = float4(fogc.rgb, %s); R[4] = i.d0; R[5] = i.d1;\n"
        "  R[8] = T[0]; R[9] = T[1]; R[10] = T[2]; R[11] = T[3];\n"
        "  R[12].a = %s;\n", (k->flags[0] & BW_RC_PERSP) ? "saturate(i.fogp)" : "i.fog",
        (k->stage_program & 0x1F) ? "T[0].a" : "1.0");

    for (s = 0; s < n; s++) {
        uint32_t icw = k->color_icw[s], ocw = k->color_ocw[s];
        uint32_t aicw = k->alpha_icw[s], aocw = k->alpha_ocw[s];
        uint32_t fl = ocw >> 12, afl = aocw >> 12;
        put(&b, "  { /* stage %u */\n", s);
        put(&b, "  R[1] = C0[%u]; R[2] = C1[%u];\n",
            (flags & 0x010) ? s : 0, (flags & 0x100) ? s : 0);
        if (flags & 1)
            put(&b, "  bool mux = R[12].a >= 0.5;\n");
        else
            put(&b, "  bool mux = (((int)(R[12].a * 255.0)) & 1) != 0;\n");
        put_rgb(&b, "A", icw >> 24); put_rgb(&b, "B", icw >> 16);
        put_rgb(&b, "C", icw >> 8);  put_rgb(&b, "D", icw);
        put_alpha(&b, "aA", aicw >> 24); put_alpha(&b, "aB", aicw >> 16);
        put_alpha(&b, "aC", aicw >> 8);  put_alpha(&b, "aD", aicw);
        put(&b, "  float3 pab = %s; float3 pcd = %s;\n",
            (fl & 2) ? "dot(A, B).xxx" : "A * B", (fl & 1) ? "dot(C, D).xxx" : "C * D");
        put(&b, "  float3 sum = %s;\n", (fl & 4) ? "mux ? pcd : pab" : "pab + pcd");
        put(&b, "  float3 ab = clamp(mo(pab, %u), -1.0, 1.0);"
                " float3 cd = clamp(mo(pcd, %u), -1.0, 1.0);"
                " float3 ms = clamp(mo(sum, %u), -1.0, 1.0);\n",
            fl & 0x38, fl & 0x38, fl & 0x38);
        put(&b, "  float apab = aA * aB, apcd = aC * aD;\n");
        put(&b, "  float asum = %s;\n", (afl & 4) ? "mux ? apcd : apab" : "apab + apcd");
        put(&b, "  float aab = clamp(mo(apab.xxx, %u).x, -1.0, 1.0);"
                " float acd = clamp(mo(apcd.xxx, %u).x, -1.0, 1.0);"
                " float ams = clamp(mo(asum.xxx, %u).x, -1.0, 1.0);\n",
            afl & 0x38, afl & 0x38, afl & 0x38);
        if ((ocw >> 4) & 0xF) {
            put(&b, "  R[%u].rgb = ab;\n", (ocw >> 4) & 0xF);
            if (fl & 0x80)
                put(&b, "  R[%u].a = ab.b;\n", (ocw >> 4) & 0xF);
        }
        if (ocw & 0xF) {
            put(&b, "  R[%u].rgb = cd;\n", ocw & 0xF);
            if (fl & 0x40)
                put(&b, "  R[%u].a = cd.b;\n", ocw & 0xF);
        }
        if ((ocw >> 8) & 0xF)
            put(&b, "  R[%u].rgb = ms;\n", (ocw >> 8) & 0xF);
        if ((aocw >> 4) & 0xF)
            put(&b, "  R[%u].a = aab;\n", (aocw >> 4) & 0xF);
        if (aocw & 0xF)
            put(&b, "  R[%u].a = acd;\n", aocw & 0xF);
        if ((aocw >> 8) & 0xF)
            put(&b, "  R[%u].a = ams;\n", (aocw >> 8) & 0xF);
        put(&b, "  R[0] = 0; }\n");
    }

    if (k->final0 || k->final1) {
        uint32_t f0 = k->final0, f1 = k->final1, ff = f1 & 0xFF;
        put(&b, "  { /* final combiner */\n  R[1] = FC0; R[2] = FC1;\n");
        put(&b, "  float3 va = %s; float3 vb = %s;\n",
            (ff & 0x40) ? "1.0 - R[5].rgb" : "R[5].rgb",
            (ff & 0x20) ? "1.0 - R[12].rgb" : "R[12].rgb");
        put(&b, "  R[14] = float4(%s, 0.0);\n", (ff & 0x80) ? "saturate(va + vb)" : "va + vb");
        put_rgb(&b, "E", f1 >> 24);
        put_rgb(&b, "F", f1 >> 16);
        put(&b, "  R[15] = float4(E * F, 0.0);\n");
        put_rgb(&b, "A", f0 >> 24); put_rgb(&b, "B", f0 >> 16);
        put_rgb(&b, "C", f0 >> 8);  put_rgb(&b, "D", f0);
        put_alpha(&b, "G", f1 >> 8);
        put(&b, "  float4 o = float4(D + C * (1.0 - A) + B * A, G); o = saturate(o);\n");
    } else {
        put(&b, "  { float4 o = saturate(R[12]);\n");
    }
    if (((k->stage_program >> 15) & 0x1F) == 0x0C && k->tex_cube[3] && ocean_debugview()) {
        /* P2a: the sea's reflection pass in false colour (opaque, so it
         * replaces what the lit pass drew underneath). n, e and r are the
         * stage program's own (rc_stage_fetch above). */
        static const char *const view[] = { "",
            /* weight: the vertex alpha the combiner weights the reflection
             * by (about 0.58 in mission 10), stretched 0.5..0.7 to
             * black..white (red: the raw value); wraw shows it as is */
            "o.rgb = float3(saturate(i.d0.a), saturate((i.d0.a - 0.5) * 5.0), saturate((i.d0.a - 0.5) * 5.0));",
            "o.rgb = normalize(n) * 0.5 + 0.5;",                    /* bump: the perturbed normal */
            "o.rgb = normalize(r) * 0.5 + 0.5;",                    /* refl: the reflection vector */
            "o.rgb = r.z >= 0.0 ? float3(0.0, saturate(normalize(r).z * 4.0), 0.0)"
            " : float3(saturate(-normalize(r).z * 4.0) * 0.8 + 0.2, 0.0, 0.0);",   /* rz: up green, down red */
            "o.rgb = normalize(e) * 0.5 + 0.5;",                    /* eye */
            "o.rgb = saturate(length(e)).xxx;",                     /* elen */
            "o.rgb = T[3].rgb;",                                    /* cube: the raw sample */
            "o.rgb = saturate(nn / 4.0).xxx;",                      /* nn: |n|^2 / 4 */
            "o.rgb = normalize(tc[3].xyz) * 0.5 + 0.5;",            /* vnorm: the basis' third row */
            "o.rgb = saturate(abs(normalize(n) - normalize(tc[3].xyz)) * 4.0);",   /* diff: bump's tilt */
            "o.rgb = i.d0.aaa;",                                    /* wraw: the vertex alpha as is */
            "o.rgb = i.d0.rgb;",                                    /* d0: the vertex colour */
            "o.rgb = saturate(dot(tc[3].xyz, e) * 0.5 + 0.5).xxx;",  /* ne: N.E (interpolated) */
            "",                                                     /* out: the pass's own colour, opaque */
            "o.rgb = o.aaa;",                                       /* oa: the pass's output alpha */
            "o.rgb = T[0].rgb;",                                    /* t0: the normal map's texel */
            "o.rgb = T[0].aaa;",                                    /* t0a: its alpha */
            "o.rgb = float3(1.0, 1.0, 1.0);",                       /* count: white at alpha 0.5 */
            "o.rgb = float3(0.5, 0.5, 0.5);",                       /* grey: 0.5 opaque (calibration) */
            "o.rgb = float3(1.0, 1.0, 1.0);",                       /* q25: white at alpha 0.25 */
            "o.rgb = float3(0.0, 0.0, 0.0);",                       /* b50: black at alpha 0.5 */
            "o.rgb = float3(1.0, 1.0, 1.0);" };                     /* w0: white at alpha 0 */
        put(&b, "  { float3 n = float3(dp[1], dp[2], dp[3]); float3 e = float3(tc[1].w, tc[2].w, tc[3].w);\n"
                "    float nn = dot(n, n); float3 r = nn != 0.0 ? 2.0 * n * dot(n, e) / nn - e : e;\n"
                "    %s o.a = %s; }\n", view[ocean_debugview()], ocean_debugalpha());
    }
    if (trdbg >= 0)         /* T1: the debug tint (green: enlarged levels, blue: the game's) */
        put(&b, "  if (trl > -99.0) o.rgb = lerp(o.rgb, trl < trt - 0.01 ? float3(0.1, 1.0, 0.15)"
                " : float3(0.1, 0.4, 1.0), 0.5);\n");
    /* alpha_test_pass, on the final alpha as 0..255 */
    if (k->vfx[0] & BW_RC_VFX_A2C)      /* VFX (D9): alpha to coverage, Remastered + MSAA */
        put(&b,
            "  if (alpha.z > 0.5) { float r = alpha.y / 255.0; float w = max(fwidth(o.a), 1.0 / 255.0);\n"
            "    o.a = saturate((o.a - r) / w + 0.5); if (o.a <= 0.0) discard; }\n"
            "  return o; }\n"
            "}\n");
    else
    put(&b,
        "  if (alpha.z > 0.5) { int v = (int)(o.a * 255.0 + 0.5), r = (int)alpha.y; int f = (int)alpha.x;\n"
        "    bool ok = (f == 7) || (f == 1 && v < r) || (f == 2 && v == r) || (f == 3 && v <= r)\n"
        "      || (f == 4 && v > r) || (f == 5 && v != r) || (f == 6 && v >= r);\n"
        "    if (!ok) discard; }\n"
        "  return o; }\n"
        "}\n");
    if (b.overflow)
        return 0;
    if (((k->stage_program >> 15) & 0x1F) == 0x0C && k->tex_cube[3]) {
        /* P2a test aid: the sea's reflection-pass shader text, once */
        static int dumped;
        const char *f = getenv("BW_OCEAN_DEBUGVIEW_DUMP");
        if (f && *f && !dumped && bw_edition_at_least(BW_ED_REMASTERED)) {
            FILE *fp = fopen(f, "w");
            dumped = 1;
            if (fp) {
                fwrite(b.p, 1, b.n, fp);
                fprintf(fp, "%c/* key: control 0x%X final 0x%X 0x%X stage_program 0x%X */%c",
                        10, k->control, k->final0, k->final1, k->stage_program, 10);
                fclose(fp);
            }
        }
    }
    return b.n;
}
