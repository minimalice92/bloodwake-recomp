/*
 * bw_texrestore.c - Remastered texture restoration (agent T1, VFX_TEX_RESTORE).
 * See bw_texrestore.h for what it makes, docs/day1/T1.md for how it was
 * checked, and docs/agents/BRIEF_T1T2.md "The design" for the rules.
 *
 * Ported from the lead's approved previews (2026-10-27):
 *   deblock_preview.py  DXT decode and the "Plan A" deblock (H.264-style
 *                       normal filter on the block grid: luma decides, RGB is
 *                       filtered, alpha untouched; A 20, B 6, tc 6);
 *   trad_upscale.py     Lanczos with the anti-ringing clamp; here Lanczos-2
 *                       (a 4x4 neighbourhood, not 3) and no sharpening.
 * One addition, which can only filter less: a block edge whose four texels
 * mix transparent (alpha < 16) and visible ones is left alone, because the
 * colour of a transparent texel is not part of the art (DXT1 stores black).
 *
 * Switches (read in bw_vfx.c): VFX_TEX_RESTORE (registry, on), BW_TEXRESTORE=0
 * off, BW_TEXRESTORE_DEBLOCK=0 no deblock, BW_TEXRESTORE_SCALE=2|4 (4),
 * BW_TEXRESTORE_DEBUG=1 the tint view (bw_rc_hlsl.c), BW_TEXRESTORE_MENUS=0
 * menu photos as before, BW_TEXRESTORE_MB=<n> VRAM budget (2048).
 * Test aids: BW_TEXRESTORE_LOG=<n> per-texture lines (48), BW_TEXRESTORE_DUMP=
 * <prefix> writes the first BW_TEXRESTORE_DUMP_N (8) restored textures' level
 * 0 before and after the deblock and their enlarged level 0 as 32-bit BMPs
 * (tools/bw_texrestore_check.py compares them with the preview code).
 *
 * Menu photos and movie frames (bw_tr_deblock_gpu): the same deblock on the
 * 8x8 grid, on the GPU as each one goes up (about 0.3 ms of the device
 * thread), drawn with Lanczos-2 when magnified (bw_rc_hlsl.c ovz).
 *
 * Never a stalled frame: the first 3D draw of a texture only copies its
 * blocks (bw_tr_request) and keeps drawing the original; a worker thread
 * decodes, classifies and deblocks; at each flip the device thread finishes
 * ready textures (upload, the two enlargement draws, the view) for at most
 * BW_TEXRESTORE_FRAME_MS (0.3 ms) per frame, and the next draws use them.
 */
#define BW_VFX_INTERNAL
#include "bw_texrestore.h"
#include "bw_vfx.h"
#include "bw_vfx_int.h"
#include "bw_gpu_failtest.h"   /* 0.2.5 item 25: BW_GPU_FAILTEST (test aid) */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- what stays exactly as shipped, by content ----------------------------
 * The water's DXT textures and every effect sprite (common/Effects.pak,
 * objects/Misc.pak: splashes, spray, wakes, tracers, the 130-region particle
 * atlas, the effects font page): (format, size, FNV-1a 64 of level 0 as the
 * guest holds it, with BW_GPU_TEXLOG's offset basis), from tools/bw_texmatch.py
 * keys (seen: 100% of DXT uploads match their key, docs/remaster/
 * texture_matching.md). Names are for the reader only. */
typedef struct { uint8_t fmt; uint16_t w, h; uint64_t fnv; } TrKey;
static const TrKey s_keep[] = {
    { 0x0F, 32, 32,     0x5BE6CF30316BD875ull },   /* spray */
    { 0x0F, 32, 32,     0x2403906290E14776ull },   /* water3 */
    { 0x0F, 16, 32,     0xDA4B108C9562B0D3ull },   /* bar */
    { 0x0C, 16, 32,     0x8B17C4C888705B6Bull },   /* tightbar */
    { 0x0F, 64, 32,     0xF0DF5198A6F9E04Bull },   /* rsplash3 */
    { 0x0F, 64, 64,     0x6ED7A575908D2980ull },   /* trail1 */
    { 0x0F, 32, 32,     0x327725DB8063287Cull },   /* bwake1 */
    { 0x0F, 64, 64,     0x964185EECA1F0E84ull },   /* bwake2 */
    { 0x0F, 64, 64,     0x332F3BEE6E6AC1F6ull },   /* bwake3 */
    { 0x0F, 64, 64,     0x05807CC0F023E051ull },   /* splash00 */
    { 0x0F, 64, 64,     0x6BD7B5254D36F5E3ull },   /* splash01 */
    { 0x0F, 64, 64,     0xD2FC65FC2B0E373Eull },   /* splash02 */
    { 0x0F, 64, 64,     0x39A5C80B5B5930E6ull },   /* splash03 */
    { 0x0F, 64, 64,     0x8E568FCA5E79D9F3ull },   /* splash04 */
    { 0x0F, 64, 64,     0x74680DB575B468CBull },   /* splash05 */
    { 0x0F, 64, 64,     0xFC0F2FF24410E1F6ull },   /* splash06 */
    { 0x0F, 64, 64,     0x0E0B663E445E2F2Full },   /* splash07 */
    { 0x0F, 32, 32,     0x55AB1C11DC56F268ull },   /* lens1 */
    { 0x0C, 512, 256,   0x253E354C4C87938Aull },   /* Effects.pak #18, a font page */
    { 0x0F, 512, 1024,  0x6F6555A7F8B0A5A9ull },   /* Effects.pak #19, the particle atlas */
    { 0x0F, 64, 64,     0x332081923717A72Aull },   /* watershadow (Water.pak) */
    { 0x0F, 128, 128,   0xE2FDAEE0AFE15A07ull },   /* BoatWake (Water.pak) */
    { 0x0F, 64, 64,     0xEE9C55B37808B9ACull },   /* tracer (objects/Misc.pak) */
};

static size_t dxt_bytes(uint32_t color, uint32_t w, uint32_t h)
{
    return (size_t)((w + 3) / 4) * ((h + 3) / 4) * (color == 0x0C ? 8u : 16u);
}

static int kept_as_shipped(uint32_t color, const uint8_t *p, uint32_t w, uint32_t h)
{
    size_t i, n;
    int shape = 0;
    uint64_t f = 1469598103934665603ull;    /* BW_GPU_TEXLOG's basis (texture_matching.md) */
    for (i = 0; i < sizeof s_keep / sizeof s_keep[0]; i++)
        if (s_keep[i].fmt == color && s_keep[i].w == w && s_keep[i].h == h)
            shape = 1;
    if (!shape)
        return 0;
    n = dxt_bytes(color, w, h);
    for (i = 0; i < n; i++)
        f = (f ^ p[i]) * 1099511628211ull;
    for (i = 0; i < sizeof s_keep / sizeof s_keep[0]; i++)
        if (s_keep[i].fmt == color && s_keep[i].w == w && s_keep[i].h == h && s_keep[i].fnv == f)
            return 1;
    return 0;
}

/* ---- DXT decode (to R8G8B8A8) ------------------------------------------------
 * As D3D's BC1-3: 5/6-bit endpoints widened by bit replication, the 4-colour
 * palette at 1/3 and 2/3, DXT1's 3-colour mode with transparent black, DXT3's
 * 4-bit alpha x 17, DXT5's 8- or 6-step alpha. DXT3/5 colour is always the
 * 4-colour palette (as the preview). */
static void c565(uint32_t c, int o[3])
{
    int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    o[0] = (r << 3) | (r >> 2);
    o[1] = (g << 2) | (g >> 4);
    o[2] = (b << 3) | (b >> 2);
}

static void decode_level(uint32_t color, const uint8_t *q, uint32_t w, uint32_t h, uint8_t *out)
{
    uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4, bb = color == 0x0C ? 8 : 16, bx, by, k;
    for (by = 0; by < bh; by++)
        for (bx = 0; bx < bw; bx++) {
            const uint8_t *blk = q + ((size_t)by * bw + bx) * bb;
            const uint8_t *cb = color == 0x0C ? blk : blk + 8;
            uint32_t c0 = cb[0] | (cb[1] << 8), c1 = cb[2] | (cb[3] << 8);
            uint32_t idx = cb[4] | (cb[5] << 8) | (cb[6] << 16) | ((uint32_t)cb[7] << 24);
            int a[3], b[3], j;
            uint8_t pal[4][4], ap[8];
            uint64_t abits = 0;
            c565(c0, a);
            c565(c1, b);
            for (j = 0; j < 3; j++) {
                pal[0][j] = (uint8_t)a[j];
                pal[1][j] = (uint8_t)b[j];
            }
            pal[0][3] = pal[1][3] = pal[2][3] = pal[3][3] = 255;
            if (color != 0x0C || c0 > c1) {
                for (j = 0; j < 3; j++) {
                    pal[2][j] = (uint8_t)((2 * a[j] + b[j] + 1) / 3);
                    pal[3][j] = (uint8_t)((a[j] + 2 * b[j] + 1) / 3);
                }
            } else {
                for (j = 0; j < 3; j++) {
                    pal[2][j] = (uint8_t)((a[j] + b[j] + 1) / 2);
                    pal[3][j] = 0;
                }
                pal[3][3] = 0;
            }
            if (color == 0x0F) {
                uint32_t a0 = blk[0], a1 = blk[1];
                ap[0] = (uint8_t)a0; ap[1] = (uint8_t)a1;
                if (a0 > a1)
                    for (j = 1; j < 7; j++) ap[j + 1] = (uint8_t)(((7 - j) * a0 + j * a1 + 3) / 7);
                else {
                    for (j = 1; j < 5; j++) ap[j + 1] = (uint8_t)(((5 - j) * a0 + j * a1 + 2) / 5);
                    ap[6] = 0; ap[7] = 255;
                }
                for (j = 0; j < 6; j++) abits |= (uint64_t)blk[2 + j] << (8 * j);
            }
            for (k = 0; k < 16; k++) {
                uint32_t x = bx * 4 + (k & 3), y = by * 4 + (k >> 2);
                uint8_t *o;
                const uint8_t *c = pal[(idx >> (2 * k)) & 3];
                if (x >= w || y >= h)
                    continue;
                o = out + ((size_t)y * w + x) * 4;
                o[0] = c[0]; o[1] = c[1]; o[2] = c[2]; o[3] = c[3];
                if (color == 0x0E)
                    o[3] = (uint8_t)(((blk[k >> 1] >> ((k & 1) * 4)) & 15) * 17);
                else if (color == 0x0F)
                    o[3] = ap[(abits >> (3 * k)) & 7];
            }
        }
}

/* ---- classification (level 0) ---------------------------------------------- */

/* A tangent-space normal map: mean R and G near 128, mean B high. The same
 * rule as tools/bw_texcat.py, which flags exactly the game's 88 DXT5 normal
 * maps among its 3,716 DDS textures (seen, catalogue). */
static int is_normal_map(const uint8_t *px, uint32_t w, uint32_t h)
{
    double r = 0, g = 0, b = 0, n = (double)w * h;
    size_t i;
    for (i = 0; i < (size_t)w * h; i++) {
        r += px[i * 4]; g += px[i * 4 + 1]; b += px[i * 4 + 2];
    }
    r /= n; g /= n; b /= n;
    return r >= 112 && r <= 144 && g >= 112 && g <= 144 && b >= 190;
}

/* 0 opaque, 1 hard (every texel transparent or opaque: DXT1 transparency and
 * the DXT5 cut-outs; all 17 of the game's flag-2 cut-out images among its 3D
 * textures are exactly 0/255, seen), 2 smooth (glows, clouds, decals). */
static int alpha_class(const uint8_t *px, uint32_t w, uint32_t h)
{
    size_t i, tr = 0, part = 0;
    for (i = 0; i < (size_t)w * h; i++) {
        uint8_t a = px[i * 4 + 3];
        if (a <= 8) tr++;
        else if (a < 247) part++;
    }
    return part ? 2 : tr ? 1 : 0;
}

/* ---- Plan A deblock ------------------------------------------------------------
 * deblock_preview.py's deblock(): edges between texels i-1 and i at i = grid,
 * 2 grid, ... (i + 1 < n), first between columns (all rows), then between
 * rows on the result. On each line across an edge with texels p1 p0 | q0 q1:
 * filtered when |p0 - q0| < A, |p1 - p0| < B, |q1 - q0| < B and
 * |p0 - q0| > 0.5 in luma (0.299 R + 0.587 G + 0.114 B), then for R, G, B
 *   dl = clamp(((q0 - p0) 4 + (p1 - q1)) / 8, +-tc), p0 += dl, q0 -= dl,
 *   p1 += clamp(((p0 + q0) / 2 - p1) / 2, +-tc/2), q1 likewise,
 * all from the values before the line's update; alpha untouched. Floats in,
 * rounded once at the end (half to even, as np.rint). */
#define DB_A 20.0f
#define DB_B 6.0f
#define DB_TC 6.0f

typedef struct { float *f; size_t cap; } TrScratch;     /* one per thread */

static float fclampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

static unsigned db_pass(float *f, uint32_t n, uint32_t lines, size_t is, size_t ls, int grid)
{
    uint32_t i, l;
    unsigned cnt = 0;
    for (i = (uint32_t)grid; i + 1 < n; i += (uint32_t)grid)
        for (l = 0; l < lines; l++) {
            float *P1 = f + ((size_t)l * ls + (size_t)(i - 2) * is) * 4;
            float *P0 = f + ((size_t)l * ls + (size_t)(i - 1) * is) * 4;
            float *Q0 = f + ((size_t)l * ls + (size_t)i * is) * 4;
            float *Q1 = f + ((size_t)l * ls + (size_t)(i + 1) * is) * 4;
            float yp1 = 0.299f * P1[0] + 0.587f * P1[1] + 0.114f * P1[2];
            float yp0 = 0.299f * P0[0] + 0.587f * P0[1] + 0.114f * P0[2];
            float yq0 = 0.299f * Q0[0] + 0.587f * Q0[1] + 0.114f * Q0[2];
            float yq1 = 0.299f * Q1[0] + 0.587f * Q1[1] + 0.114f * Q1[2];
            float d = fabsf(yp0 - yq0);
            int c, vis;
            if (!(d < DB_A && fabsf(yp1 - yp0) < DB_B && fabsf(yq1 - yq0) < DB_B && d > 0.5f))
                continue;
            vis = (P1[3] >= 16.0f) + (P0[3] >= 16.0f) + (Q0[3] >= 16.0f) + (Q1[3] >= 16.0f);
            if (vis != 0 && vis != 4)
                continue;                       /* T1's guard: transparent texels' colour */
            for (c = 0; c < 3; c++) {
                float p1 = P1[c], p0 = P0[c], q0 = Q0[c], q1 = Q1[c];
                float dl = fclampf(((q0 - p0) * 4.0f + (p1 - q1)) / 8.0f, -DB_TC, DB_TC);
                float d1p = fclampf(((p0 + q0) / 2.0f - p1) / 2.0f, -DB_TC / 2, DB_TC / 2);
                float d1q = fclampf(((p0 + q0) / 2.0f - q1) / 2.0f, -DB_TC / 2, DB_TC / 2);
                P0[c] = p0 + dl;
                Q0[c] = q0 - dl;
                P1[c] = p1 + d1p;
                Q1[c] = q1 + d1q;
            }
            cnt++;
        }
    return cnt;
}

/* In place on 4-byte texels; ri/gi/bi: the byte of R, G, B (alpha is 3).
 * Returns how many edge lines it smoothed. */
static unsigned deblock_u8(uint8_t *px, uint32_t w, uint32_t h, int grid, int ri, int gi, int bi,
                           TrScratch *sc)
{
    size_t n = (size_t)w * h, i;
    unsigned cnt = 0;
    float *s_f;
    if (w < (uint32_t)grid * 2 && h < (uint32_t)grid * 2)
        return 0;
    if (n * 4 > sc->cap) {
        free(sc->f);
        sc->f = (float *)malloc(n * 4 * sizeof(float));
        sc->cap = sc->f ? n * 4 : 0;
        if (!sc->f)
            return 0;
    }
    s_f = sc->f;
    for (i = 0; i < n; i++) {
        s_f[i * 4 + 0] = px[i * 4 + ri];
        s_f[i * 4 + 1] = px[i * 4 + gi];
        s_f[i * 4 + 2] = px[i * 4 + bi];
        s_f[i * 4 + 3] = px[i * 4 + 3];
    }
    if (w >= (uint32_t)grid * 2)
        cnt += db_pass(s_f, w, h, 1, w, grid);          /* vertical edges, along each row */
    if (h >= (uint32_t)grid * 2)
        cnt += db_pass(s_f, h, w, w, 1, grid);          /* horizontal edges, down each column */
    if (!cnt)
        return 0;
    for (i = 0; i < n; i++) {
        px[i * 4 + ri] = (uint8_t)lrintf(fclampf(s_f[i * 4 + 0], 0.0f, 255.0f));
        px[i * 4 + gi] = (uint8_t)lrintf(fclampf(s_f[i * 4 + 1], 0.0f, 255.0f));
        px[i * 4 + bi] = (uint8_t)lrintf(fclampf(s_f[i * 4 + 2], 0.0f, 255.0f));
    }
    return cnt;
}

/* ---- the enlargement (GPU) ----------------------------------------------------
 * trad_upscale.py's lanczos() with a = 2: for each output texel centre the
 * source position s = (x + 0.5) / k - 0.5, separable weights over the 4x4
 * texels around it, each axis normalised, then the result held within the
 * 2x2 source texels around the point, per channel. Edges: wrap or mirror as
 * the draw's address mode, else clamp. Cut-out alpha (M.z): 2 (default,
 * BW_TEXRESTORE_CUTOUT=game) the plain bilinear interpolation of the 2x2's
 * alpha, so the game's own alpha test (seen: reference 8 of 255 on mission
 * 1's foliage and girders, no blending) cuts the same silhouette as on the
 * original, to within a small fraction of a texel (bilinear of bilinear is
 * exact except where an enlarged texel straddles a coarse texel centre);
 * 1 (BW_TEXRESTORE_CUTOUT=threshold, the brief's first rule) the filtered
 * alpha thresholded halfway between the 2x2's lowest and highest, which
 * draws the shapes thinner than the game when magnified (about half a texel
 * per edge at reference 8). */
static const char s_tr_src[] =
"cbuffer R : register(b0) { float4 P; int4 M; };\n"   /* P: src w, h, k; M: addr u, v, cut-out alpha */
"Texture2D<float4> src : register(t0);\n"
"struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; };\n"
"float l2(float x) { x = abs(x); if (x < 1e-6) return 1.0; if (x >= 2.0) return 0.0;\n"
"  float a = 3.14159265358979 * x; return 2.0 * sin(a) * sin(0.5 * a) / (a * a); }\n"
"int ax(int i, int n, int m) {\n"
"  if (m == 1) { i = i % n; return i < 0 ? i + n : i; }\n"
"  if (m == 2) { int q = 2 * n; i = i % q; if (i < 0) i += q; return i < n ? i : q - 1 - i; }\n"
"  return clamp(i, 0, n - 1); }\n"
"float4 ps_lanczos2(V v) : SV_Target {\n"
"  int2 n = int2(P.xy);\n"
"  float2 s = v.p.xy / P.z - 0.5; float2 i0 = floor(s); float2 f = s - i0;\n"
"  float4 wx, wy;\n"
"  [unroll] for (int k = 0; k < 4; k++) { wx[k] = l2(f.x - (k - 1)); wy[k] = l2(f.y - (k - 1)); }\n"
"  wx /= dot(wx, 1.0); wy /= dot(wy, 1.0);\n"
"  float4 acc = 0, lo = 1e9, hi = -1e9, qa = 0;\n"
"  [unroll] for (int y = 0; y < 4; y++) { int yy = ax((int)i0.y + y - 1, n.y, M.y);\n"
"    [unroll] for (int x = 0; x < 4; x++) { int xx = ax((int)i0.x + x - 1, n.x, M.x);\n"
"      float4 c = src.Load(int3(xx, yy, 0)); acc += c * (wx[x] * wy[y]);\n"
"      if ((x == 1 || x == 2) && (y == 1 || y == 2)) { lo = min(lo, c); hi = max(hi, c);\n"
"        qa[(y - 1) * 2 + (x - 1)] = c.a; } } }\n"
"  acc = clamp(acc, lo, hi);\n"
"  if (M.z == 1) acc.a = acc.a >= 0.5 * (lo.a + hi.a) ? hi.a : lo.a;\n"
"  if (M.z == 2) acc.a = lerp(lerp(qa.x, qa.y, f.x), lerp(qa.z, qa.w, f.x), f.y);\n"
"  return acc; }\n"
/* Menu photos and movies: the same Plan A rule as deblock_u8 on the GPU, one
 * direction per pass (M.y 0: the edges between columns, 1: between rows, run
 * on the first pass's result), on the M.x grid; P.xy the frame's size. Each
 * texel finds the edge it is p1/p0/q0/q1 of, decides from the four texels'
 * luma and writes its own new value. In 0..255 units as the CPU code: the
 * first pass reads the 8-bit frame (x P.z = 255, rounded: M.z = 1) and writes
 * floats (P.w = 1); the second reads those and writes the frame (P.w = 255). */
"float4 ld(int2 q) { float4 c = src.Load(int3(q, 0)) * P.z; return M.z == 1 ? round(c) : c; }\n"
"precise float luma(float4 c) { return 0.299 * c.r + 0.587 * c.g + 0.114 * c.b; }\n"
"float4 ps_deblock(V v) : SV_Target {\n"
"  int2 p = int2(v.p.xy); int g = M.x; int ax = M.y;\n"
"  int n = ax == 0 ? (int)P.x : (int)P.y; int i = ax == 0 ? p.x : p.y;\n"
"  float4 c = ld(p);\n"
"  int r = i % g; int e;\n"
"  if (r <= 1) e = i - r; else if (r >= g - 2) e = i + (g - r); else return c / P.w;\n"
"  if (e < g || e + 1 >= n) return c / P.w;\n"
"  int2 d = ax == 0 ? int2(1, 0) : int2(0, 1);\n"
"  int2 b = ax == 0 ? int2(e, p.y) : int2(p.x, e);\n"
"  float4 P1 = ld(b - 2 * d), P0 = ld(b - d), Q0 = ld(b), Q1 = ld(b + d);\n"
"  precise float yp1 = luma(P1), yp0 = luma(P0), yq0 = luma(Q0), yq1 = luma(Q1);\n"
"  precise float dd = abs(yp0 - yq0);\n"
"  if (!(dd < 20.0 && abs(yp1 - yp0) < 6.0 && abs(yq1 - yq0) < 6.0 && dd > 0.5)) return c / P.w;\n"
"  int vis = (P1.a >= 16.0 ? 1 : 0) + (P0.a >= 16.0 ? 1 : 0) + (Q0.a >= 16.0 ? 1 : 0) + (Q1.a >= 16.0 ? 1 : 0);\n"
"  if (vis != 0 && vis != 4) return c / P.w;\n"
"  precise float3 dl = clamp(((Q0.rgb - P0.rgb) * 4.0 + (P1.rgb - Q1.rgb)) / 8.0, -6.0, 6.0);\n"
"  precise float3 m = (P0.rgb + Q0.rgb) / 2.0; int role = i - e; precise float3 o;\n"
"  if (role == -1) o = P0.rgb + dl; else if (role == 0) o = Q0.rgb - dl;\n"
"  else if (role == -2) o = P1.rgb + clamp((m - P1.rgb) / 2.0, -3.0, 3.0);\n"
"  else o = Q1.rgb + clamp((m - Q1.rgb) / 2.0, -3.0, 3.0);\n"
"  return float4(o, c.a) / P.w; }\n";

static ID3D11PixelShader *s_ps, *s_ps_db;
static ID3D11Buffer *s_cb;
static int s_gpu_ok = -1;

static int gpu_init(void)
{
    if (s_gpu_ok >= 0)
        return s_gpu_ok;
    s_ps = bw_vfx_compile_ps(s_tr_src, sizeof s_tr_src - 1, "ps_lanczos2");
    s_ps_db = bw_vfx_compile_ps(s_tr_src, sizeof s_tr_src - 1, "ps_deblock");
    s_cb = bw_vfx_make_cb(32);
    s_gpu_ok = s_ps && s_ps_db && s_cb;
    if (!s_gpu_ok)
        fprintf(stderr, "[TEXRESTORE] shader setup failed: textures stay as shipped\n");
    return s_gpu_ok;
}

void bw_tr_init(void)
{
    gpu_init();
}

static double now_ms(void);
static const char *dump_prefix(void);
static void write_bmp32(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h, size_t pitch);

/* Test aid (BW_TEXRESTORE_DUMP): a B8G8R8A8 or R8G8B8A8 2D texture's level 0
 * as a 32-bit BMP. */
static void dump_texture(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11Resource *res, const char *path)
{
    D3D11_TEXTURE2D_DESC td;
    ID3D11Texture2D *stg = NULL;
    D3D11_MAPPED_SUBRESOURCE m;
    uint8_t *rgba;
    uint32_t x, y;
    ID3D11Texture2D_GetDesc((ID3D11Texture2D *)res, &td);
    td.MipLevels = 1; td.ArraySize = 1;
    td.SampleDesc.Count = 1; td.SampleDesc.Quality = 0;
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.MiscFlags = 0;
    if (FAILED(ID3D11Device_CreateTexture2D(dev, &td, NULL, &stg)))
        return;
    ID3D11DeviceContext_CopySubresourceRegion(ctx, (ID3D11Resource *)stg, 0, 0, 0, 0, res, 0, NULL);
    rgba = (uint8_t *)malloc((size_t)td.Width * td.Height * 4);
    if (rgba && SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)stg, 0, D3D11_MAP_READ, 0, &m))) {
        int bgra = td.Format == DXGI_FORMAT_B8G8R8A8_UNORM;
        for (y = 0; y < td.Height; y++) {
            const uint8_t *s = (const uint8_t *)m.pData + (size_t)y * m.RowPitch;
            uint8_t *d = rgba + (size_t)y * td.Width * 4;
            for (x = 0; x < td.Width; x++) {
                d[x * 4 + 0] = s[x * 4 + (bgra ? 2 : 0)];
                d[x * 4 + 1] = s[x * 4 + 1];
                d[x * 4 + 2] = s[x * 4 + (bgra ? 0 : 2)];
                d[x * 4 + 3] = s[x * 4 + 3];
            }
        }
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)stg, 0);
        write_bmp32(path, rgba, td.Width, td.Height, (size_t)td.Width * 4);
    }
    free(rgba);
    ID3D11Texture2D_Release(stg);
}

/* Menu photos and movie frames: src (w x h) through the two deblock passes
 * into a new B8G8R8A8 texture, returned with its view. The middle step is a
 * float target (the CPU code keeps floats between its passes too), kept
 * between frames. Device thread; changes pipeline state. */
int bw_tr_deblock_gpu(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11ShaderResourceView *src,
                      uint32_t w, uint32_t h, int grid, int kind, ID3D11Texture2D **out_tex,
                      ID3D11ShaderResourceView **out_srv)
{
    static ID3D11Texture2D *mid;
    static ID3D11ShaderResourceView *mid_srv;
    static ID3D11RenderTargetView *mid_rtv;
    static uint32_t mw, mh;
    static unsigned n_frames[3], dumped[3];
    static double ms_sum[3], ms_max[3];
    static const char *names[3] = { "?", "menu photo", "movie frame" };
    ID3D11Texture2D *ot = NULL;
    ID3D11ShaderResourceView *os = NULL;
    ID3D11RenderTargetView *ortv = NULL;
    D3D11_TEXTURE2D_DESC td;
    D3D11_VIEWPORT vp;
    struct { float p[4]; int32_t m[4]; } cb;
    double t0 = now_ms(), dt;
    *out_tex = NULL;
    *out_srv = NULL;
    if (kind < 1 || kind > 2)
        kind = 0;
    if (!gpu_init() || !src || w < 16 || h < 16 || grid < 4)
        return 0;
    memset(&td, 0, sizeof td);
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    if (!mid || mw != w || mh != h) {
        if (mid_srv) ID3D11ShaderResourceView_Release(mid_srv);
        if (mid_rtv) ID3D11RenderTargetView_Release(mid_rtv);
        if (mid) ID3D11Texture2D_Release(mid);
        mid = NULL; mid_srv = NULL; mid_rtv = NULL;
        td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        if (FAILED(ID3D11Device_CreateTexture2D(dev, &td, NULL, &mid))
                || FAILED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)mid, NULL, &mid_srv))
                || FAILED(ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)mid, NULL, &mid_rtv)))
            return 0;
        mw = w; mh = h;
    }
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    if (FAILED(ID3D11Device_CreateTexture2D(dev, &td, NULL, &ot))
            || FAILED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)ot, NULL, &os))
            || FAILED(ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)ot, NULL, &ortv))) {
        if (os) ID3D11ShaderResourceView_Release(os);
        if (ot) ID3D11Texture2D_Release(ot);
        return 0;
    }
    vp.TopLeftX = 0; vp.TopLeftY = 0;
    vp.Width = (float)w; vp.Height = (float)h;
    vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
    cb.p[0] = (float)w; cb.p[1] = (float)h; cb.p[2] = 255.0f; cb.p[3] = 1.0f;
    cb.m[0] = grid; cb.m[1] = 0; cb.m[2] = 1; cb.m[3] = 0;
    bw_vfx_update_cb(s_cb, &cb, sizeof cb);
    bw_vfx_fullscreen(mid_rtv, &vp, s_ps_db, s_cb, &src, 1);       /* edges between columns */
    cb.p[2] = 1.0f; cb.p[3] = 255.0f;
    cb.m[1] = 1; cb.m[2] = 0;
    bw_vfx_update_cb(s_cb, &cb, sizeof cb);
    bw_vfx_fullscreen(ortv, &vp, s_ps_db, s_cb, &mid_srv, 1);      /* then between rows */
    ID3D11RenderTargetView_Release(ortv);
    *out_tex = ot;
    *out_srv = os;
    dt = now_ms() - t0;
    n_frames[kind]++;
    ms_sum[kind] += dt;
    if (dt > ms_max[kind]) ms_max[kind] = dt;
    if (n_frames[kind] == 1 || n_frames[kind] % 300 == 0)
        fprintf(stderr, "[TEXRESTORE] %s %ux%u: %u deblocked on the GPU (%dx%d grid), device thread"
                        " %.3f ms avg / %.3f max\n", names[kind], w, h, n_frames[kind], grid, grid,
                ms_sum[kind] / n_frames[kind], ms_max[kind]);
    if (dump_prefix() && dumped[kind] < 2) {
        char p[600];
        ID3D11Resource *res = NULL;
        ID3D11View_GetResource((ID3D11View *)src, &res);
        if (res) {
            _snprintf(p, sizeof p, "%strg%d_%s%u_%ux%u_orig.bmp", dump_prefix(), grid,
                      kind == 1 ? "photo" : "movie", dumped[kind], w, h);
            p[sizeof p - 1] = 0;
            dump_texture(dev, ctx, res, p);
            ID3D11Resource_Release(res);
            _snprintf(p, sizeof p, "%strg%d_%s%u_%ux%u_deblock.bmp", dump_prefix(), grid,
                      kind == 1 ? "photo" : "movie", dumped[kind], w, h);
            p[sizeof p - 1] = 0;
            dump_texture(dev, ctx, (ID3D11Resource *)ot, p);
        }
        dumped[kind]++;
    }
    return 1;
}

/* ---- stats, logs, dumps -------------------------------------------------------- */

static unsigned s_n_ok, s_n_why[BW_TR_WHY_N], s_n_alpha[3], s_n_lk[3];
static double s_total_mb, s_cpu_ms, s_cpu_max, s_dev_ms, s_dev_max, s_pump_max;
static unsigned s_n_dev, s_logged, s_changed, s_inflight;
static unsigned s_n_mixed;          /* item 48: restored textures also drawn with other edge modes */
static double s_freq;

static double now_ms(void)
{
    LARGE_INTEGER c;
    if (s_freq == 0.0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        s_freq = (double)f.QuadPart / 1000.0;
    }
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / s_freq;
}

static unsigned log_limit(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("BW_TEXRESTORE_LOG");
        v = e ? atoi(e) : 48;
    }
    return (unsigned)v;
}

static double frame_budget_ms(void)
{
    static double v = -1.0;
    if (v < 0.0) {
        const char *e = getenv("BW_TEXRESTORE_FRAME_MS");
        v = e ? atof(e) : 0.3;
        if (v < 0.0) v = 0.0;
    }
    return v;
}

static const char *why_name(int why)
{
    static const char *n[] = { "", "normal map", "water/effect texture", "size", "VRAM budget",
                               "contents keep changing", "failure" };
    return why > 0 && why < BW_TR_WHY_N ? n[why] : "?";
}

static const char *mode_name(uint32_t a)
{
    return a == 1 ? "wrap" : a == 2 ? "mirror" : "clamp";
}

static const char *fmt_name(uint32_t c)
{
    return c == 0x0C ? "DXT1" : c == 0x0E ? "DXT3" : "DXT5";
}

static void write_bmp32(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h, size_t pitch)
{
    FILE *f = fopen(path, "wb");
    uint8_t hd[54];
    uint32_t size = 54 + w * h * 4, y, x;
    uint8_t *row;
    if (!f)
        return;
    memset(hd, 0, sizeof hd);
    hd[0] = 'B'; hd[1] = 'M';
    memcpy(hd + 2, &size, 4);
    hd[10] = 54; hd[14] = 40;
    memcpy(hd + 18, &w, 4);
    { int32_t hh = -(int32_t)h; memcpy(hd + 22, &hh, 4); }
    hd[26] = 1; hd[28] = 32;
    fwrite(hd, 1, 54, f);
    row = (uint8_t *)malloc((size_t)w * 4);
    if (row) {
        for (y = 0; y < h; y++) {
            const uint8_t *s = rgba + (size_t)y * pitch;
            for (x = 0; x < w; x++) {
                row[x * 4 + 0] = s[x * 4 + 2]; row[x * 4 + 1] = s[x * 4 + 1];
                row[x * 4 + 2] = s[x * 4 + 0]; row[x * 4 + 3] = s[x * 4 + 3];
            }
            fwrite(row, 1, (size_t)w * 4, f);
        }
        free(row);
    }
    fclose(f);
}

static const char *dump_prefix(void)
{
    static int init;
    static const char *p;
    if (!init) {
        init = 1;
        p = getenv("BW_TEXRESTORE_DUMP");
        if (p && !*p) p = NULL;
    }
    return p;
}

static unsigned dump_limit(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("BW_TEXRESTORE_DUMP_N");
        v = e ? atoi(e) : 8;
    }
    return (unsigned)v;
}

static void dump_level(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11Texture2D *tex,
                       uint32_t mip, uint32_t W, uint32_t H, const char *path)
{
    D3D11_TEXTURE2D_DESC td;
    ID3D11Texture2D *stg = NULL;
    D3D11_MAPPED_SUBRESOURCE m;
    memset(&td, 0, sizeof td);
    td.Width = W; td.Height = H; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(ID3D11Device_CreateTexture2D(dev, &td, NULL, &stg)))
        return;
    ID3D11DeviceContext_CopySubresourceRegion(ctx, (ID3D11Resource *)stg, 0, 0, 0, 0,
                                              (ID3D11Resource *)tex, mip, NULL);
    if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)stg, 0, D3D11_MAP_READ, 0, &m))) {
        write_bmp32(path, (const uint8_t *)m.pData, W, H, m.RowPitch);
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)stg, 0);
    }
    ID3D11Texture2D_Release(stg);
}

void bw_tr_frame(void)
{
    static double last;
    double t = now_ms();
    if (!s_changed || t - last < 10000.0)
        return;
    last = t;
    s_changed = 0;
    fprintf(stderr, "[TEXRESTORE] %u restored (%u at 4x, %u at 2x; alpha %u opaque, %u cut-out, %u smooth),"
                    " %.0f MB VRAM, %u in the works; left as shipped: %u normal maps, %u water/effect,"
                    " %u size, %u budget, %u changing, %u failed; %u drawn with two edge modes; worker CPU %.2f ms avg / %.2f max"
                    " (%.0f ms total); device thread %.2f ms avg / %.2f max per texture, %.2f ms max"
                    " in a frame\n",
            s_n_ok, s_n_lk[2], s_n_lk[1], s_n_alpha[0], s_n_alpha[1], s_n_alpha[2], s_total_mb,
            s_inflight, s_n_why[BW_TR_WHY_NORMAL], s_n_why[BW_TR_WHY_EXCLUDED],
            s_n_why[BW_TR_WHY_SMALL], s_n_why[BW_TR_WHY_BUDGET], s_n_why[BW_TR_WHY_CHURN],
            s_n_why[BW_TR_WHY_FAIL], s_n_mixed, s_n_dev ? s_cpu_ms / s_n_dev : 0.0, s_cpu_max, s_cpu_ms,
            s_n_dev ? s_dev_ms / s_n_dev : 0.0, s_dev_max, s_pump_max);
}

/* ---- the build: request (device thread), CPU work (worker), finish (device) ---- */

typedef struct TrJob {
    struct TrJob *next;
    BwTrSlot *slot;
    uint32_t ticket, color, w, h, levels, au, av, addr;
    uint8_t *dxt;               /* the guest's blocks, every level, copied at the request */
    uint8_t *rgba;              /* worker: the decoded, deblocked levels, one after another */
    uint8_t *orig0;             /* worker, dump only: level 0 before the deblock */
    int why, ac;
    unsigned deb;
    double cpu_ms;
} TrJob;

static CRITICAL_SECTION s_cs;
static HANDLE s_wake, s_thread;
static TrJob *s_todo, *s_todo_tail, *s_done, *s_done_tail;
static uint32_t s_ticket;

static void q_push(TrJob **head, TrJob **tail, TrJob *j)
{
    j->next = NULL;
    if (*tail) (*tail)->next = j; else *head = j;
    *tail = j;
}

static TrJob *q_pop(TrJob **head, TrJob **tail)
{
    TrJob *j = *head;
    if (j) {
        *head = j->next;
        if (!*head) *tail = NULL;
    }
    return j;
}

static void job_free(TrJob *j)
{
    if (!j) return;
    free(j->dxt);
    free(j->rgba);
    free(j->orig0);
    free(j);
}

static uint32_t lvl_dim(uint32_t v, uint32_t lv)
{
    return (v >> lv) ? (v >> lv) : 1;
}

/* The worker's part: decode every level, the classification, the deblock. */
static void tr_cpu(TrJob *j, TrScratch *sc)
{
    double t0 = now_ms();
    const BwVfx *v = bw_vfx();
    uint32_t lv;
    size_t texels = 0, off = 0, doff = 0;
    if (kept_as_shipped(j->color, j->dxt, j->w, j->h)) {
        j->why = BW_TR_WHY_EXCLUDED;
        goto out;
    }
    for (lv = 0; lv < j->levels; lv++)
        texels += (size_t)lvl_dim(j->w, lv) * lvl_dim(j->h, lv);
    j->rgba = (uint8_t *)malloc(texels * 4);
    if (!j->rgba) {
        j->why = BW_TR_WHY_FAIL;
        goto out;
    }
    for (lv = 0; lv < j->levels; lv++) {
        uint32_t lw = lvl_dim(j->w, lv), lh = lvl_dim(j->h, lv);
        decode_level(j->color, j->dxt + doff, lw, lh, j->rgba + off * 4);
        doff += dxt_bytes(j->color, lw, lh);
        off += (size_t)lw * lh;
    }
    if (is_normal_map(j->rgba, j->w, j->h)) {
        j->why = BW_TR_WHY_NORMAL;
        goto out;
    }
    j->ac = alpha_class(j->rgba, j->w, j->h);
    if (dump_prefix()) {
        j->orig0 = (uint8_t *)malloc((size_t)j->w * j->h * 4);
        if (j->orig0) memcpy(j->orig0, j->rgba, (size_t)j->w * j->h * 4);
    }
    if (v->tr_deblock)                      /* Plan A on the game's own level 0 and its own mips */
        for (lv = 0, off = 0; lv < j->levels; lv++) {
            uint32_t lw = lvl_dim(j->w, lv), lh = lvl_dim(j->h, lv);
            j->deb += deblock_u8(j->rgba + off * 4, lw, lh, 4, 0, 1, 2, sc);
            off += (size_t)lw * lh;
        }
out:
    j->cpu_ms = now_ms() - t0;
}

static DWORD WINAPI tr_worker(LPVOID arg)
{
    TrScratch sc = { NULL, 0 };
    (void)arg;
    for (;;) {
        WaitForSingleObject(s_wake, INFINITE);
        for (;;) {
            TrJob *j;
            EnterCriticalSection(&s_cs);
            j = q_pop(&s_todo, &s_todo_tail);
            LeaveCriticalSection(&s_cs);
            if (!j)
                break;
            tr_cpu(j, &sc);
            EnterCriticalSection(&s_cs);
            q_push(&s_done, &s_done_tail, j);
            LeaveCriticalSection(&s_cs);
        }
    }
    return 0;
}

static int worker_start(void)
{
    static int tried, ok;
    if (tried)
        return ok;
    tried = 1;
    InitializeCriticalSection(&s_cs);
    s_wake = CreateEventA(NULL, FALSE, FALSE, NULL);
    s_thread = s_wake ? CreateThread(NULL, 0, tr_worker, NULL, 0, NULL) : NULL;
    if (s_thread)
        SetThreadPriority(s_thread, THREAD_PRIORITY_BELOW_NORMAL);   /* never ahead of the game */
    ok = s_thread != NULL;
    if (!ok)
        fprintf(stderr, "[TEXRESTORE] worker thread failed: textures stay as shipped\n");
    return ok;
}

/* Item 48: the second copies' slots. A fixed pool, never freed, so a job
 * still in the works for a returned slot reads valid memory (and no longer
 * matches it: release zeroes the ticket). */
#define TR_ALT_N 256
static BwTrSlot s_alt_pool[TR_ALT_N];
static uint8_t s_alt_busy[TR_ALT_N];
static unsigned s_alt_full;

static BwTrSlot *alt_take(void)
{
    unsigned i;
    for (i = 0; i < TR_ALT_N; i++)
        if (!s_alt_busy[i]) {
            s_alt_busy[i] = 1;
            memset(&s_alt_pool[i], 0, sizeof s_alt_pool[i]);
            return &s_alt_pool[i];
        }
    return NULL;
}

void bw_tr_release(BwTrSlot *s)
{
    if (s->alt) {                           /* item 48: its second copy goes with it */
        BwTrSlot *a = s->alt;
        s->alt = NULL;
        bw_tr_release(a);
        if (a >= s_alt_pool && a < s_alt_pool + TR_ALT_N)
            s_alt_busy[a - s_alt_pool] = 0;
    }
    if (s->srv) { ID3D11ShaderResourceView_Release(s->srv); s->srv = NULL; }
    if (s->tex) { ID3D11Texture2D_Release(s->tex); s->tex = NULL; }
    if (s->state == 1) {
        s_total_mb -= s->bytes / 1048576.0;
        if (s_n_ok) s_n_ok--;
        if (s_n_alpha[s->alpha]) s_n_alpha[s->alpha]--;
        if (s_n_lk[s->lk]) s_n_lk[s->lk]--;
        s_changed = 1;
    }
    memset(s, 0, sizeof *s);                /* a job still in the works no longer matches */
}


BwTrSlot *bw_tr_slot_for(BwTrSlot *s, uint32_t addr_u, uint32_t addr_v, uint32_t addr)
{
    uint8_t u = (uint8_t)(addr_u == 1 ? 1 : addr_u == 2 ? 2 : 0);
    uint8_t v = (uint8_t)(addr_v == 1 ? 1 : addr_v == 2 ? 2 : 0);
    if ((s->state != 1 && s->state != 3) || (u == s->au && v == s->av))
        return s;
    if (!s->mixed) {
        s->mixed = 1;
        s_n_mixed++;
        s_changed = 1;
        if (!s->alt)
            s->alt = alt_take();
        if (!s->alt)
            s_alt_full++;
        if (s_n_mixed <= 16)
            fprintf(stderr, "[TEXRESTORE] %08X is also drawn with edges %s/%s; its restored copy was made"
                            " for %s/%s, so %s (%u such textures so far)\n",
                    addr, mode_name(u), mode_name(v), mode_name(s->au), mode_name(s->av),
                    s->alt ? "it gets a second copy for these" : "that copy serves both (no slot left)",
                    s_n_mixed);
    }
    if (!s->alt || (s->alt->state != 0 && (u != s->alt->au || v != s->alt->av)))
        return s;                           /* a third combination: the first copy */
    return s->alt;
}

static int left(BwTrSlot *s, int why, uint32_t addr, uint32_t color, uint32_t w, uint32_t h)
{
    s->state = 2;
    s->why = (uint8_t)why;
    s->ticket = 0;
    s_n_why[why]++;
    s_changed = 1;
    if (s_logged < log_limit()) {
        s_logged++;
        fprintf(stderr, "[TEXRESTORE] %08X %ux%u %s left as shipped: %s\n", addr, w, h,
                fmt_name(color), why_name(why));
    }
    return 0;
}

void bw_tr_request(BwTrSlot *s, uint32_t color, const uint8_t *src, uint32_t w, uint32_t h,
                   uint32_t levels, uint32_t addr_u, uint32_t addr_v, uint32_t addr)
{
    uint32_t lv;
    size_t n = 0;
    TrJob *j;
    memset(s, 0, sizeof *s);
    s->au = (uint8_t)(addr_u == 1 ? 1 : addr_u == 2 ? 2 : 0);   /* item 48 */
    s->av = (uint8_t)(addr_v == 1 ? 1 : addr_v == 2 ? 2 : 0);
    if (!gpu_init() || !worker_start()) {
        left(s, BW_TR_WHY_FAIL, addr, color, w, h);
        return;
    }
    if ((w & 3) || (h & 3) || w < 16 || h < 16 || w > 4096 || h > 4096) {
        left(s, BW_TR_WHY_SMALL, addr, color, w, h);
        return;
    }
    if (levels < 1) levels = 1;
    if (levels > 13) levels = 13;
    for (lv = 0; lv < levels; lv++)
        n += dxt_bytes(color, lvl_dim(w, lv), lvl_dim(h, lv));
    j = (TrJob *)calloc(1, sizeof *j);
    if (j)
        j->dxt = (uint8_t *)malloc(n);
    if (!j || !j->dxt) {
        free(j);
        left(s, BW_TR_WHY_FAIL, addr, color, w, h);
        return;
    }
    memcpy(j->dxt, src, n);
    j->slot = s;
    if (!++s_ticket) ++s_ticket;
    j->ticket = s->ticket = s_ticket;
    j->color = color; j->w = w; j->h = h; j->levels = levels;
    j->au = addr_u; j->av = addr_v; j->addr = addr;
    s->state = 3;
    s_inflight++;
    s_changed = 1;
    EnterCriticalSection(&s_cs);
    q_push(&s_todo, &s_todo_tail, j);
    LeaveCriticalSection(&s_cs);
    SetEvent(s_wake);
}

/* The device thread's part: the chain (lk enlarged levels, then the game's),
 * the game's levels uploaded once and copied in on the GPU, the enlargement
 * drawn from level 0. Returns 1 if the texture was restored; *drew becomes 1
 * once the enlargement has drawn (it binds its own target and state), which
 * can happen before a later step fails (0.2.5 item 47: the caller then
 * rebinds its targets either way). */
static const char *s_fail_what;     /* the step that failed, for the log */
static int tr_finish(TrJob *j, ID3D11Device *dev, ID3D11DeviceContext *ctx, int *drew)
{
    const BwVfx *v = bw_vfx();
    BwTrSlot *s = j->slot;
    uint32_t w = j->w, h = j->h, k = v->tr_scale >= 4 ? 4 : 2, lk, lv, W, H, mips;
    size_t off = 0;
    double t0 = now_ms(), t1, mb;
    ID3D11Texture2D *tex = NULL, *stex = NULL;
    ID3D11ShaderResourceView *ssrv = NULL, *srv = NULL;
    D3D11_TEXTURE2D_DESC td;
    D3D11_SUBRESOURCE_DATA init[13];
    D3D11_SHADER_RESOURCE_VIEW_DESC sd;
    static unsigned dumped;
    int ok = 0;

    if (j->why)
        return left(s, j->why, j->addr, j->color, w, h);
    while (k > 1 && (w * k > 4096 || h * k > 4096))
        k /= 2;
    if (k < 2)
        return left(s, BW_TR_WHY_SMALL, j->addr, j->color, w, h);
    lk = k == 4 ? 2 : 1;
    W = w * k; H = h * k;
    mips = lk + j->levels;
    mb = 0.0;
    for (lv = 0; lv < j->levels; lv++)
        mb += 4.0 * lvl_dim(w, lv) * lvl_dim(h, lv);
    for (lv = 0; lv < lk; lv++)
        mb += 4.0 * (double)(W >> lv) * (double)(H >> lv);
    mb /= 1048576.0;
    if (s_total_mb + mb > (double)v->tr_budget_mb)
        return left(s, BW_TR_WHY_BUDGET, j->addr, j->color, w, h);

    /* the game's levels, deblocked, as one immutable texture */
    memset(init, 0, sizeof init);
    for (lv = 0; lv < j->levels; lv++) {
        uint32_t lw = lvl_dim(w, lv), lh = lvl_dim(h, lv);
        init[lv].pSysMem = j->rgba + off * 4;
        init[lv].SysMemPitch = lw * 4;
        off += (size_t)lw * lh;
    }
    memset(&td, 0, sizeof td);
    td.Width = w; td.Height = h; td.MipLevels = j->levels; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    s_fail_what = "the game's levels (texture)";
    if (FAILED(ID3D11Device_CreateTexture2D(dev, &td, init, &stex)))
        goto fail;
    memset(&sd, 0, sizeof sd);
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MostDetailedMip = 0;
    sd.Texture2D.MipLevels = 1;
    s_fail_what = "the game's levels (view)";
    if (FAILED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)stex, &sd, &ssrv)))
        goto fail;
    /* the chain */
    td.Width = W; td.Height = H; td.MipLevels = mips;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    s_fail_what = "the restored chain (texture)";
    if (FAILED(ID3D11Device_CreateTexture2D(dev, &td, NULL, &tex)))
        goto fail;
    for (lv = 0; lv < j->levels; lv++)
        ID3D11DeviceContext_CopySubresourceRegion(ctx, (ID3D11Resource *)tex, lk + lv, 0, 0, 0,
                                                  (ID3D11Resource *)stex, lv, NULL);
    for (lv = 0; lv < lk; lv++) {
        D3D11_RENDER_TARGET_VIEW_DESC rd;
        ID3D11RenderTargetView *rtv = NULL;
        D3D11_VIEWPORT vp;
        struct { float p[4]; int32_t m[4]; } cb;
        memset(&rd, 0, sizeof rd);
        rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        rd.Texture2D.MipSlice = lv;
        s_fail_what = "an enlarged level (render-target view)";
        if (FAILED(ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)tex, &rd, &rtv)))
            goto fail;
        cb.p[0] = (float)w; cb.p[1] = (float)h; cb.p[2] = (float)(k >> lv); cb.p[3] = 0.0f;
        cb.m[0] = j->au == 1 ? 1 : j->au == 2 ? 2 : 0;
        cb.m[1] = j->av == 1 ? 1 : j->av == 2 ? 2 : 0;
        cb.m[2] = j->ac == 1 ? (v->tr_cutout_threshold ? 1 : 2) : 0;
        cb.m[3] = 0;
        bw_vfx_update_cb(s_cb, &cb, sizeof cb);
        vp.TopLeftX = 0; vp.TopLeftY = 0;
        vp.Width = (float)(W >> lv); vp.Height = (float)(H >> lv);
        vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
        *drew = 1;                          /* from here the caller's targets are unbound */
        bw_vfx_fullscreen(rtv, &vp, s_ps, s_cb, &ssrv, 1);
        ID3D11RenderTargetView_Release(rtv);
    }
    s_fail_what = "the restored chain (view)";
    if (FAILED(BW_GPU_FT("texrestore", E_OUTOFMEMORY,       /* item 25 test aid */
                         ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)tex, NULL, &srv))))
        goto fail;
    ok = 1;
    t1 = now_ms();

    s->tex = tex; tex = NULL;
    s->srv = srv; srv = NULL;
    s->state = 1;
    s->ticket = 0;
    s->lk = (uint8_t)lk;
    s->alpha = (uint8_t)j->ac;
    s->bytes = (uint32_t)(mb * 1048576.0);
    s_n_ok++;
    s_n_alpha[j->ac]++;
    s_n_lk[lk]++;
    s_total_mb += mb;
    s_n_dev++;
    s_cpu_ms += j->cpu_ms;
    if (j->cpu_ms > s_cpu_max) s_cpu_max = j->cpu_ms;
    s_dev_ms += t1 - t0;
    if (t1 - t0 > s_dev_max) s_dev_max = t1 - t0;
    s_changed = 1;
    if (s_logged < log_limit()) {
        static const char *an[] = { "opaque", "cut-out", "smooth" };
        s_logged++;
        fprintf(stderr, "[TEXRESTORE] %08X %ux%u %s %u levels -> %ux%u, %u levels (%s alpha, edges %s/%s,"
                        " %u block-edge lines deblocked), %.1f MB, worker %.2f ms, device thread %.2f ms\n",
                j->addr, w, h, fmt_name(j->color), j->levels, W, H, mips, an[j->ac],
                mode_name(j->au), mode_name(j->av), j->deb, mb, j->cpu_ms, t1 - t0);
    }
    if (j->orig0 && dumped < dump_limit()) {
        const char *dump = dump_prefix();
        char p[600];
        dumped++;
        _snprintf(p, sizeof p, "%str_%08X_%ux%u_orig.bmp", dump, j->addr, w, h); p[sizeof p - 1] = 0;
        write_bmp32(p, j->orig0, w, h, (size_t)w * 4);
        _snprintf(p, sizeof p, "%str_%08X_%ux%u_deblock.bmp", dump, j->addr, w, h); p[sizeof p - 1] = 0;
        write_bmp32(p, j->rgba, w, h, (size_t)w * 4);
        _snprintf(p, sizeof p, "%str_%08X_%ux%u_x%u_%s-%s_%s.bmp", dump, j->addr, w, h, k,
                  mode_name(j->au), mode_name(j->av),
                  j->ac != 1 ? "soft" : v->tr_cutout_threshold ? "hard" : "cut");
        p[sizeof p - 1] = 0;
        dump_level(dev, ctx, s->tex, 0, W, H, p);
        _snprintf(p, sizeof p, "%str_%08X_%ux%u_chain%u.bmp", dump, j->addr, w, h, lk); p[sizeof p - 1] = 0;
        dump_level(dev, ctx, s->tex, lk, w, h, p);      /* the chain's copy of the game's level 0 */
        fprintf(stderr, "[TEXRESTORE] dumped %08X (%s...)\n", j->addr, dump);
    }
fail:
    if (ssrv) ID3D11ShaderResourceView_Release(ssrv);
    if (stex) ID3D11Texture2D_Release(stex);
    if (srv) ID3D11ShaderResourceView_Release(srv);
    if (tex) ID3D11Texture2D_Release(tex);
    if (!ok) {
        if (s_logged < log_limit())
            fprintf(stderr, "[TEXRESTORE] %08X %ux%u: %s could not be made%s\n", j->addr, w, h, s_fail_what,
                    *drew ? "; the game's targets are bound again" : "");
        return left(s, BW_TR_WHY_FAIL, j->addr, j->color, w, h);
    }
    return 1;
}

int bw_tr_pump(ID3D11Device *dev, ID3D11DeviceContext *ctx)
{
    double t0, budget = frame_budget_ms();
    int drew = 0, n = 0;              /* drew: pipeline state changed (item 47) */
    if (!s_thread || !s_inflight)
        return 0;
    t0 = now_ms();
    for (;;) {
        TrJob *j;
        if (n && now_ms() - t0 >= budget)
            break;                          /* the rest next frame */
        EnterCriticalSection(&s_cs);
        j = q_pop(&s_done, &s_done_tail);
        LeaveCriticalSection(&s_cs);
        if (!j)
            break;
        if (s_inflight) s_inflight--;
        if (j->slot->state == 3 && j->slot->ticket == j->ticket) {
            tr_finish(j, dev, ctx, &drew);
            n++;
        }
        job_free(j);
    }
    if (n) {
        double dt = now_ms() - t0;
        if (dt > s_pump_max) s_pump_max = dt;
    }
    return drew;
}
