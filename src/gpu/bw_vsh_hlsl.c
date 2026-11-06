/*
 * bw_vsh_hlsl.c - HLSL vertex shaders from NV2A vertex programs.
 *
 * The toolkit runs a title's vertex programs on the CPU, one vertex at a
 * time (xboxrecomp src/kernel/nv2a_vsh_interp.c). In a Blood Wake mission
 * that interpreter was 42% of the GPU thread's time, and the reason one core
 * sat near 100%. This writes, for one program, the vertex shader that does
 * what the interpreter does, so the graphics card runs it instead.
 *
 * Semantics follow the interpreter rule for rule, not xemu's GLSL generator
 * (hw/xbox/nv2a/pgraph/glsl/vsh-prog.c), wherever the two differ, so the GPU
 * path draws what the CPU path draws -- except that mul and mad keep the
 * hardware's "anything times zero is zero" (vmul below):
 *   - sources read before any write; mux 1 is a temp (12 is oPos, 13-15
 *     read R0), 2 an input, 0 or 3 a constant (+a0.x if relative; outside
 *     c[0..191] reads c[0]);
 *   - MAC ops 1-13 and ILU ops 1-7 as run_mac / run_ilu, including rcp of 0
 *     as +inf, rcc's clamp, arl as floor(x + 0.001), dph adding b.w;
 *   - writes in the interpreter's order: MAC temp, ILU temp (R1 whenever a
 *     MAC op is present), then the output register; constant writes are
 *     left out (the executor keeps CXT_WRITE programs on the CPU);
 *   - outputs default as there: colours (0,0,0,1), texture coordinates
 *     (0,0,0,1), d0/d1 clamped to 0..1 at the end.
 * After the program, the executor's own steps (transform_vertex,
 * raster_xf_clipped, fog_factor in nv2a_pb_exec.c): w = +0 becomes 1, x and y
 * scale by the anti-aliasing factor, clipping at w = 1e-3 and at the near
 * plane (screen z = c[59].z), and the fog unit on oFog.x.
 *
 * Xbox D3D's programs end in its screen-space epilogue, so oPos arrives in
 * surface pixels with w the clip-space w. The shader maps it as the back
 * end's existing vertex shader does and outputs (x*w, y*w, z*w, w), an affine
 * image of clip space, so the GPU's clipper and interpolation are exact.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bw_vsh_hlsl.h"
#include "bw_rc_hlsl.h"                         /* BW_RC_CBUFFER */
#include "toolkit_patches/kernel/bw_backend_ext.h"   /* bw_vsh_field */

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

#define F bw_vsh_field

static const char *mask_str(uint32_t m, char out[6])
{
    int k = 0;
    out[k++] = '.';
    if (m & 8) out[k++] = 'x';
    if (m & 4) out[k++] = 'y';
    if (m & 2) out[k++] = 'z';
    if (m & 1) out[k++] = 'w';
    out[k] = 0;
    return out;
}

static const char *temp_name(uint32_t t, char out[8])
{
    if (t == 12)
        return "OP";
    snprintf(out, 8, "R%u", t);
    return out;
}

static unsigned popcount(uint32_t v)
{
    unsigned c = 0;
    for (; v; v &= v - 1)
        c++;
    return c;
}

/* read_src: one source operand, as an HLSL float4 expression. */
/* Blood Wake P1: the shader-side conversion of input k (bw_backend_ext.h,
 * BwXVsInput.conv); 0 when the input arrives as floats (gathered, a float
 * format, a constant) and is read as it is. */
static uint8_t in_conv(const uint8_t *conv, uint32_t k)
{
    uint8_t c;
    if (!conv)
        return 0;
    c = conv[k];
    switch (c >> 4) {
    case BW_XVS_COLOR: case BW_XVS_UBYTE: case BW_XVS_SHORT: case BW_XVS_CMP:
        return c;
    default:
        return 0;
    }
}

static void put_src(Buf *b, const char *var, const uint32_t *ins, int which,
                    uint32_t input_mask, const uint8_t *conv)
{
    static const char sw[] = "xyzw";
    uint32_t neg, swz, mux, reg;
    char base[48];

    if (which == 0) {
        neg = F(ins, 1, 8, 1); swz = F(ins, 1, 0, 8);
        mux = F(ins, 2, 26, 2); reg = F(ins, 2, 28, 4);
    } else if (which == 1) {
        neg = F(ins, 2, 25, 1); swz = F(ins, 2, 17, 8);
        mux = F(ins, 2, 11, 2); reg = F(ins, 2, 13, 4);
    } else {
        neg = F(ins, 2, 10, 1); swz = F(ins, 2, 2, 8);
        mux = F(ins, 3, 28, 2);
        reg = (F(ins, 2, 0, 2) << 2) | F(ins, 3, 30, 2);
    }
    if (mux == 1) {
        if (reg == 12)
            snprintf(base, sizeof base, "OP");
        else
            snprintf(base, sizeof base, "R%u", reg < 12 ? reg : 0u);
    } else if (mux == 2) {
        uint32_t in = F(ins, 1, 9, 4), k = popcount(input_mask & ((1u << in) - 1u));
        snprintf(base, sizeof base, in_conv(conv, k) ? "c%u" : "I.i%u", k);
    } else {
        uint32_t ci = F(ins, 1, 13, 8);
        if (F(ins, 3, 1, 1))
            snprintf(base, sizeof base, "K[kix(%u + a0)]", ci);
        else
            snprintf(base, sizeof base, "K[%u]", ci < 192 ? ci : 0u);
    }
    put(b, "    float4 %s = %s%s", var, neg ? "-" : "", base);
    if (swz != 0x1B)                                       /* not .xyzw */
        put(b, ".%c%c%c%c", sw[(swz >> 6) & 3], sw[(swz >> 4) & 3],
            sw[(swz >> 2) & 3], sw[swz & 3]);
    put(b, ";\n");
}

/* VSOut's members (the text every vertex shader carries, unchanged). */
#define BW_VSH_OUT_BODY \
    " float4 p : SV_Position; noperspective float4 d0 : COLOR0;\n" \
    "  noperspective float4 d1 : COLOR1; noperspective float fog : FOG;\n" \
    "  float4 tc0 : TEXCOORD0; float4 tc1 : TEXCOORD1;\n" \
    "  float4 tc2 : TEXCOORD2; float4 tc3 : TEXCOORD3; float fogp : FOG1;\n" \
    "  float2 clip : SV_ClipDistance0;"
#define BW_VSH_OUT "struct VSOut {" BW_VSH_OUT_BODY " };\n"
/* gfx-sprites (I6): a point batch's vertex shader output, VSOut's members
 * then the point's centre in surface pixels and its size (sp: x, y, z as
 * transform_vertex leaves them, oPts.x), for the sprite geometry shader. */
#define BW_VSH_OUT_P "struct VSOutP {" BW_VSH_OUT_BODY "\n  float4 sp : SPRITE0; };\n"

/* raster_xf_clipped drops a whole triangle when a vertex's clip distances
 * are not finite. The game does send such vertices: w around 1e30 with
 * z * w overflowing (BW_GPU_VSH_VERIFY found 79 in 5.6 million, mission 1).
 * The GPU clipper, handed infinities, drew them as screen-wide streaks. This
 * geometry shader keeps the CPU path's rule. */
const char bw_vsh_gs_hlsl[] =
    BW_VSH_OUT
    "[maxvertexcount(3)]\n"
    "void gs_main(triangle VSOut v[3], inout TriangleStream<VSOut> s) {\n"
    "  [unroll] for (int i = 0; i < 3; i++)\n"
    "    if (any(!isfinite(v[i].p)) || any(!isfinite(v[i].clip))) return;\n"
    "  s.Append(v[0]); s.Append(v[1]); s.Append(v[2]);\n"
    "}\n";

/* gfx-sprites (I6): point sprites on the GPU. Each point of a point list
 * becomes the quad bw_draw_points (nv2a_pb_exec.c) makes on the CPU, rule
 * for rule, in the same arithmetic order:
 *   - behind the eye (w not above 0, after transform_vertex's w rules):
 *     dropped; a centre that isn't finite: dropped (the finite filter's
 *     rule, which the CPU path's clipper applies per triangle);
 *   - the size: spr0.z when the batch has one (point params off:
 *     SET_POINT_SIZE / 8), else the program's oPts.x; below 1 or NaN is 1,
 *     above 63.875 is 63.875;
 *   - half the size, times the anti-aliasing scale (never below 1: spr0.x,
 *     spr0.y), either side of the centre in surface pixels; then the
 *     combiner path's own mapping to the surface (bw_d3d11.c s_xvs_hlsl):
 *     x * surf.z * 2 - 1, the widescreen remap, 1 - y * surf.w * 2, times w;
 *     z and w, the colours, fog and clip distances the centre's;
 *   - in sprite mode (spr1.x) stage 3's coordinates (0,0) top-left to (1,1)
 *     bottom-right, z and q 1; the other stages keep the vertex's;
 *   - two triangles in the CPU path's order, wound clockwise on the surface
 *     when spr1.y (the cull state's kept face), else the other way.
 * The GPU's clipper then clips at w = 1e-3 and the near plane with the
 * centre's distances: all four corners share them, so a quad is kept or
 * dropped whole, as on the CPU path. */
const char bw_vsh_sprite_gs_hlsl[] =
    BW_RC_CBUFFER
    "cbuffer S : register(b1) { float4 spr0; float4 spr1; };\n"
    BW_VSH_OUT_P
    "struct GSOut {" BW_VSH_OUT_BODY "\n"
    "#ifdef BW_SPR_SO\n"
    "  float4 chk : SPRCHK;\n"
    "#endif\n"
    "};\n"
    "GSOut corner(VSOutP c, uint pid, int k, float hx, float hy) {\n"
    "  GSOut q;\n"
    "  q.d0 = c.d0; q.d1 = c.d1; q.fog = c.fog; q.fogp = c.fogp; q.clip = c.clip;\n"
    "  q.tc0 = c.tc0; q.tc1 = c.tc1; q.tc2 = c.tc2; q.tc3 = c.tc3;\n"
    "  float w = c.p.w;\n"
    "  float px = c.sp.x + ((k & 1) ? hx : -hx);\n"
    "  float py = c.sp.y + ((k & 2) ? hy : -hy);\n"
    "  float x = px * surf.z * 2.0 - 1.0;\n"
    BW_WS_REMAP_HLSL
    "  float y = 1.0 - py * surf.w * 2.0;\n"
    "  q.p = float4(x * w, y * w, c.p.z, w);\n"
    "  if (spr1.x > 0.5)\n"
    "    q.tc3 = float4((k & 1) ? 1.0 : 0.0, (k & 2) ? 1.0 : 0.0, 1.0, 1.0);\n"
    "#ifdef BW_SPR_SO\n"
    "  q.chk = float4((float)pid, (float)k, 0.0, 0.0);\n"
    "#endif\n"
    "  return q;\n"
    "}\n"
    "[maxvertexcount(6)]\n"
    "void gs_main(point VSOutP v[1], uint pid : SV_PrimitiveID, inout TriangleStream<GSOut> s) {\n"
    "  VSOutP c = v[0];\n"
    "  if (!(c.p.w > 0.0)) return;\n"
    "  if (any(!isfinite(c.p)) || any(!isfinite(c.clip))) return;\n"
    "  float sz = spr0.z > 0.0 ? spr0.z : c.sp.w;\n"
    "  if (!(sz >= 1.0) || isnan(sz)) sz = 1.0;\n"
    "  if (sz > 63.875) sz = 63.875;\n"
    "  float hx = sz * 0.5 * spr0.x, hy = sz * 0.5 * spr0.y;\n"
    "  GSOut q0 = corner(c, pid, 0, hx, hy), q1 = corner(c, pid, 1, hx, hy);\n"
    "  GSOut q2 = corner(c, pid, 2, hx, hy), q3 = corner(c, pid, 3, hx, hy);\n"
    "  if (spr1.y > 0.5) {\n"
    "    s.Append(q0); s.Append(q1); s.Append(q2); s.RestartStrip();\n"
    "    s.Append(q1); s.Append(q3); s.Append(q2);\n"
    "  } else {\n"
    "    s.Append(q0); s.Append(q2); s.Append(q1); s.RestartStrip();\n"
    "    s.Append(q1); s.Append(q2); s.Append(q3);\n"
    "  }\n"
    "}\n";

static const char s_head[] =
    BW_RC_CBUFFER
    "cbuffer V : register(b1) { float4 K[192]; float4 vsinfo; float4 foginfo; };\n"
    BW_VSH_OUT
    "uint kix(int i) { return (i < 0 || i >= 192) ? 0u : (uint)i; }\n"
    /* run_mac's comparisons, per component, NaN behaving as in C */
    /* mul and mad's product: anything times zero is zero, infinity and NaN
     * included -- the hardware's rule as xemu's vsh-prog.c _MUL keeps it
     * (inferred; not checked on a console). The toolkit's interpreter
     * multiplies plainly (NaN * 0 = NaN); this is the one place the GPU path
     * departs from it. */
    "float4 vmul(float4 a, float4 b) { float4 r = a * b;\n"
    "  if (a.x == 0.0 || b.x == 0.0) r.x = 0.0; if (a.y == 0.0 || b.y == 0.0) r.y = 0.0;\n"
    "  if (a.z == 0.0 || b.z == 0.0) r.z = 0.0; if (a.w == 0.0 || b.w == 0.0) r.w = 0.0;\n"
    "  return r; }\n"
    "float4 vmin(float4 a, float4 b) { return float4(a.x < b.x ? a.x : b.x,\n"
    "  a.y < b.y ? a.y : b.y, a.z < b.z ? a.z : b.z, a.w < b.w ? a.w : b.w); }\n"
    "float4 vmax(float4 a, float4 b) { return float4(a.x >= b.x ? a.x : b.x,\n"
    "  a.y >= b.y ? a.y : b.y, a.z >= b.z ? a.z : b.z, a.w >= b.w ? a.w : b.w); }\n"
    "float4 vslt(float4 a, float4 b) { return float4(a.x < b.x ? 1.0 : 0.0,\n"
    "  a.y < b.y ? 1.0 : 0.0, a.z < b.z ? 1.0 : 0.0, a.w < b.w ? 1.0 : 0.0); }\n"
    "float4 vsge(float4 a, float4 b) { return float4(a.x >= b.x ? 1.0 : 0.0,\n"
    "  a.y >= b.y ? 1.0 : 0.0, a.z >= b.z ? 1.0 : 0.0, a.w >= b.w ? 1.0 : 0.0); }\n"
    /* run_ilu */
    "float4 ircp(float x) { return (x == 0.0 ? asfloat(0x7f800000) : 1.0 / x).xxxx; }\n"
    "float4 ircc(float x) { float f = 1.0 / x;\n"
    "  if (abs(f) < 5.42101e-20) f = f < 0.0 ? -5.42101e-20 : 5.42101e-20;\n"
    "  if (abs(f) > 1.884467e19) f = f < 0.0 ? -1.884467e19 : 1.884467e19;\n"
    "  return f.xxxx; }\n"
    "float4 irsq(float x) { return (1.0 / sqrt(abs(x))).xxxx; }\n"
    "float4 iexp(float x) { float fl = floor(x);\n"
    "  return float4(exp2(fl), x - fl, exp2(x), 1.0); }\n"
    "float4 ilog(float x) { float ax = abs(x);\n"
    "  if (ax == 0.0) return float4(-asfloat(0x7f800000), 1.0, -asfloat(0x7f800000), 1.0);\n"
    "  float e = floor(log2(ax)); return float4(e, ax / exp2(e), log2(ax), 1.0); }\n"
    "float4 ilit(float4 c) { float nl = c.x > 0.0 ? c.x : 0.0, nh = c.y > 0.0 ? c.y : 0.0;\n"
    "  float p = c.w < -127.9961 ? -127.9961 : (c.w > 127.9961 ? 127.9961 : c.w);\n"
    "  float s = nh == 0.0 ? (p == 0.0 ? 1.0 : (p > 0.0 ? 0.0 : asfloat(0x7f800000)))\n"
    "                      : pow(nh, p);\n"
    "  return float4(1.0, nl, c.x > 0.0 ? s : 0.0, 1.0); }\n"
    /* fog_factor */
    "float fogf(float d) {\n"
    "  if (foginfo.x < 0.5) return 1.0;\n"
    "  int m = (int)foginfo.y; float px = foginfo.z, py = foginfo.w, f;\n"
    "  if (m == 0x800 || m == 0x802) f = px + exp2(d * py * 16.0) - 1.5;\n"
    "  else if (m == 0x801 || m == 0x803) f = px + exp2(-d * d * py * py * 32.0) - 1.5;\n"
    "  else f = px + d * py - 1.0;\n"
    "  if (m == 0x802 || m == 0x803 || m == 0x804) f = abs(f);\n"
    "  if (f != f) f = 1.0;\n"
    "  return f < 0.0 ? 0.0 : (f > 1.0 ? 1.0 : f); }\n"
    /* The same factor unclamped, for FOG1 (BW_RC_PERSP): as xemu vsh.c, an
     * infinite distance gives 1, NaN 1, and the rest stays within the float
     * range; the pixel shader clamps it to 0..1. */
    "float fogr(float d) {\n"
    "  if (foginfo.x < 0.5) return 1.0;\n"
    "  int m = (int)foginfo.y; float px = foginfo.z, py = foginfo.w, f;\n"
    "  if (isinf(d)) return 1.0;\n"
    "  if (m == 0x800 || m == 0x802) f = px + exp2(d * py * 16.0) - 1.5;\n"
    "  else if (m == 0x801 || m == 0x803) f = px + exp2(-d * d * py * py * 32.0) - 1.5;\n"
    "  else f = px + d * py - 1.0;\n"
    "  if (m == 0x802 || m == 0x803 || m == 0x804) f = abs(f);\n"
    "  if (f != f) f = 1.0;\n"
    "  return clamp(f, -3.4e38, 3.4e38); }\n";

/* Blood Wake P1: the integer formats' conversion, through the table the back
 * end fills with fetch_attr's own expressions (bw_d3d11.c, vfetch_table):
 * x/1023 for x in -1024..1023 at x + 1024, z/511 for z in -512..511 at
 * z + 2560, c/255 at c + 3072. Only shaders with such an input carry it. */
static const char s_conv_head[] =
    "Buffer<float> bwT : register(t0);\n"
    "float4 bwcmp(uint v) {\n"
    "  int x = asint(v << 21) >> 21, y = asint(v << 10) >> 21, z = asint(v) >> 22;\n"
    "  return float4(bwT.Load(x + 1024), bwT.Load(y + 1024), bwT.Load(z + 2560), 1.0); }\n";

size_t bw_vsh_hlsl(const uint32_t (*prog)[4], uint32_t n, uint32_t input_mask,
                   const uint8_t *conv, char *out, size_t cap)
{
    return bw_vsh_hlsl_ex(prog, n, input_mask, conv, 0, out, cap);
}

size_t bw_vsh_hlsl_ex(const uint32_t (*prog)[4], uint32_t n, uint32_t input_mask,
                      const uint8_t *conv, int points, char *out, size_t cap)
{
    Buf b = { out, 0, cap, 0 };
    uint32_t s, nin = popcount(input_mask), any_conv = 0;
    char m1[6], tn[8];

    for (s = 0; s < nin; s++)
        any_conv |= in_conv(conv, s);
    put(&b, "%s", s_head);
    if (any_conv)
        put(&b, "%s", s_conv_head);
    {
        /* BW_GPU_VSH_MULZERO=0: plain multiplies, exactly as the interpreter
         * (to tell the hardware rule's differences from real ones). */
        static int plain = -1;
        if (plain < 0) {
            const char *e = getenv("BW_GPU_VSH_MULZERO");
            plain = e && e[0] == '0';
        }
        if (plain)
            put(&b, "#define vmul(a, b) ((a) * (b))%c", 10);
    }
    /* The inputs the program reads, in ascending order, as vertex attributes
     * ATTR0..: the back end's input layout for this shader matches. */
    put(&b, "struct VSIn { uint vid : SV_VertexID;");
    for (s = 0; s < nin; s++) {
        uint8_t c = in_conv(conv, s);
        if (!c)
            put(&b, " float4 i%u : ATTR%u;", s, s);
        else
            put(&b, " %s r%u : ATTR%u;", (c >> 4) == BW_XVS_SHORT ? "int4"
                                         : (c >> 4) == BW_XVS_CMP ? "uint" : "uint4", s, s);
    }
    put(&b, " };\n");
    if (points)                                     /* gfx-sprites: the GS's input */
        put(&b, "%s", BW_VSH_OUT_P);
    put(&b,
        "%s vs_main(VSIn I) {\n"
        "  float4 R0 = 0, R1 = 0, R2 = 0, R3 = 0, R4 = 0, R5 = 0, R6 = 0, R7 = 0;\n"
        "  float4 R8 = 0, R9 = 0, R10 = 0, R11 = 0, OP = 0, O5 = 0;\n"
        "  float4 O3 = float4(0, 0, 0, 1), O4 = float4(0, 0, 0, 1);\n"
        "  float4 O9 = float4(0, 0, 0, 1), O10 = float4(0, 0, 0, 1);\n"
        "  float4 O11 = float4(0, 0, 0, 1), O12 = float4(0, 0, 0, 1);\n"
        "  int a0 = 0;\n", points ? "VSOutP" : "VSOut");
    if (points)                                     /* oPts, 0 as the interpreter's outregs */
        put(&b, "  float4 O6 = 0;\n");
    /* P1: each converted input once, as fetch_attr returns it (components
     * past the size read 0, 0, 0, 1) */
    for (s = 0; s < nin; s++) {
        static const char sw[] = "xyzw";
        uint8_t c = in_conv(conv, s);
        uint32_t j, sz = c & 15u;
        if (!c)
            continue;
        if ((c >> 4) == BW_XVS_CMP) {
            put(&b, "  float4 c%u = bwcmp(I.r%u);\n", s, s);
            continue;
        }
        put(&b, "  float4 c%u = float4(", s);
        for (j = 0; j < 4; j++) {
            const char *sep = j < 3 ? ", " : ");\n";
            if ((c >> 4) == BW_XVS_COLOR)          /* bytes B, G, R, A */
                put(&b, "bwT.Load(int(I.r%u.%c) + 3072)%s", s, "zyxw"[j], sep);
            else if (j >= sz)
                put(&b, "%s%s", j == 3 ? "1.0" : "0.0", sep);
            else if ((c >> 4) == BW_XVS_UBYTE)
                put(&b, "bwT.Load(int(I.r%u.%c) + 3072)%s", s, sw[j], sep);
            else
                put(&b, "(float)I.r%u.%c%s", s, sw[j], sep);
        }
    }

    for (s = 0; s < n; s++) {
        const uint32_t *ins = prog[s];
        uint32_t mac = F(ins, 1, 21, 4), ilu = F(ins, 1, 25, 3);
        uint32_t mac_mask = F(ins, 3, 24, 4), ilu_mask = F(ins, 3, 16, 4);
        uint32_t tdst = F(ins, 3, 20, 4);
        uint32_t omask = F(ins, 3, 12, 4), oaddr = F(ins, 3, 3, 8);
        int ua = mac != 0;
        int ub = mac == 2 || (mac >= 4 && mac <= 12);
        int uc = mac == 3 || mac == 4 || ilu != 0;
        const char *msrc = mac ? "m" : "float4(0, 0, 0, 0)";
        const char *isrc = ilu ? "u" : "float4(0, 0, 0, 0)";

        put(&b, "  { /* %u */\n", s);
        if (ua) put_src(&b, "a", ins, 0, input_mask, conv);
        if (ub) put_src(&b, "b", ins, 1, input_mask, conv);
        if (uc) put_src(&b, "c", ins, 2, input_mask, conv);

        switch (mac) {
        case 0:  break;
        case 2:  put(&b, "    float4 m = vmul(a, b);\n"); break;
        case 3:  put(&b, "    float4 m = a + c;\n"); break;
        case 4:  put(&b, "    float4 m = vmul(a, b) + c;\n"); break;
        case 5:  put(&b, "    float4 m = (a.x * b.x + a.y * b.y + a.z * b.z).xxxx;\n"); break;
        case 6:  put(&b, "    float4 m = (a.x * b.x + a.y * b.y + a.z * b.z + b.w).xxxx;\n"); break;
        case 7:  put(&b, "    float4 m = (a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w).xxxx;\n"); break;
        case 8:  put(&b, "    float4 m = float4(1.0, a.y * b.y, a.z, b.w);\n"); break;
        case 9:  put(&b, "    float4 m = vmin(a, b);\n"); break;
        case 10: put(&b, "    float4 m = vmax(a, b);\n"); break;
        case 11: put(&b, "    float4 m = vslt(a, b);\n"); break;
        case 12: put(&b, "    float4 m = vsge(a, b);\n"); break;
        case 13: put(&b, "    float4 m = a;\n"); break;
        default: put(&b, "    float4 m = a;\n"); break;        /* mov, 14, 15 */
        }
        switch (ilu) {
        case 0: break;
        case 1: put(&b, "    float4 u = c;\n"); break;
        case 2: put(&b, "    float4 u = ircp(c.x);\n"); break;
        case 3: put(&b, "    float4 u = ircc(c.x);\n"); break;
        case 4: put(&b, "    float4 u = irsq(c.x);\n"); break;
        case 5: put(&b, "    float4 u = iexp(c.x);\n"); break;
        case 6: put(&b, "    float4 u = ilog(c.x);\n"); break;
        default: put(&b, "    float4 u = ilit(c);\n"); break;
        }
        if (mac == 13)                                  /* arl */
            put(&b, "    a0 = (int)floor(a.x + 0.001);\n");

        if (mac && mac != 13 && mac_mask && tdst <= 12) {
            mask_str(mac_mask, m1);
            put(&b, "    %s%s = m%s;\n", temp_name(tdst, tn), m1, m1);
        }
        if (ilu && ilu_mask) {
            uint32_t it = mac ? 1u : tdst;          /* paired: always R1 */
            if (it <= 12) {
                mask_str(ilu_mask, m1);
                put(&b, "    %s%s = u%s;\n", temp_name(it, tn), m1, m1);
            }
        }
        if (omask && F(ins, 3, 11, 1)) {
            const char *src = F(ins, 3, 2, 1) ? isrc : msrc;
            char ms[6];
            mask_str(omask, ms);
            if (oaddr == 0)
                put(&b, "    OP%s = (%s)%s;\n", ms, src, ms);
            else if (oaddr == 3 || oaddr == 4 || oaddr == 5
                     || (oaddr >= 9 && oaddr <= 12) || (points && oaddr == 6))
                put(&b, "    O%u%s = (%s)%s;\n", oaddr, ms, src, ms);
        }
        put(&b, "  }\n");
        if (F(ins, 3, 0, 1))
            break;
    }

    put(&b,
        "  %s o;\n"
        "  float4 P = OP;\n"
        "  if (asuint(P.w) == 0u) P.w = 1.0;\n"         /* +0 only; -0 stays behind */
        /* An infinite w (rcp of a 0 rhw: the intro movies' full-screen quad)
         * is 2^64, as xemu's clampAwayZeroInf keeps it; see transform_vertex. */
        "  if (isinf(P.w)) P.w = P.w > 0.0 ? 18446744073709551616.0 : -18446744073709551616.0;\n"
        "  P.x *= vsinfo.x; P.y *= vsinfo.y;\n"
        "  float w = P.w;\n"
        "  float x = P.x * surf.z * 2.0 - 1.0;\n"
        BW_WS_REMAP_HLSL                                /* widescreen only (BW_WS) */
        "  float y = 1.0 - P.y * surf.w * 2.0;\n"
        "  float zr = (P.z - depth.x) * depth.y;\n"
        "  float z = depth.z > 0.5 ? saturate(zr) : zr;\n"
        "  o.p = float4(x * w, y * w, z * w, w);\n"
        "  o.clip.x = w - 1e-3;\n"
        "  o.clip.y = vsinfo.z > 0.5 ? (P.z - K[59].z) * w : 1.0;\n"
        "  o.d0 = saturate(O3); o.d1 = saturate(O4);\n"
        "  o.fog = fogf(O5.x); o.fogp = fogr(O5.x);\n"
        "  o.tc0 = O9; o.tc1 = O10; o.tc2 = O11; o.tc3 = O12;\n", points ? "VSOutP" : "VSOut");
    if (points)                 /* the centre in surface pixels, and oPts.x */
        put(&b, "  o.sp = float4(P.x, P.y, P.z, O6.x);\n");
    put(&b,
        "  return o;\n"
        "}\n");
    if (b.overflow)
        return 0;
    return b.n;
}

/* ---- gfx-ffp: the fixed function as a generated vertex shader -----------
 *
 * BW_GPU_FFP (every edition since GA-13; the D3D11 back end, get_ffp). The
 * executor's fixed-function path (nv2a_pb_exec.c ffp_vertex, lit_color,
 * ffp_fog_distance) as HLSL, rule for rule, in its own order of operations
 * (the shaders compile with IEEE strictness, so nothing is reassociated):
 *   - clip = composite x position (the position as fetch_attr gives it);
 *     position = (clip.xy / w + c[59].xy) * aa, z = clip.z / w + c[59].z;
 *     |w| <= 1e-6 has no image (see below);
 *   - lighting (lit_color): eye-space position and normal by the model-view
 *     matrix (the position's w taken as 1, the normal by the matrix itself),
 *     the normal normalised when longer than 1e-8; scene ambient plus, for
 *     each enabled light, att * (ambient + diffuse * max(0, N.L)), infinite
 *     lights by their direction, local ones by their position with the range
 *     test and 1 / (a0 + a1 d + a2 d d); colour material 1 multiplies the
 *     light's ambient or diffuse by the vertex colour; alpha the vertex's
 *     with diffuse colour material, else the material's; clamped to 0..1
 *     (NaN kept, as the C comparisons keep it);
 *   - unlit: the colour array, else white;
 *   - the fog distance (ffp_fog_distance): 0 with fog off, else by the
 *     generation mode: 0 v4.w clamped, 6 v5.x, 1 |eye|, 3 |plane|, any other
 *     the plane dot(c[57].xyz, eye) + c[57].w, with eye = model-view x
 *     position (w included here);
 *   - texture coordinates as fetched, (0,0,0,1) without an array; d1 zero.
 * Then the program shaders' epilogue (bw_vsh_hlsl above): the same surface
 * mapping, widescreen remap, depth, clip distances and fog unit, with the
 * one difference that belongs to ffp_vertex: its w = 0 and infinite-w rules
 * are the program path's, not the fixed function's, so they are left out.
 *
 * The one rule a vertex shader can't keep: ffp_vertex fails at |w| <= 1e-6,
 * which makes the CPU path drop the whole batch. Here that vertex's position
 * is NaN, so the finite-vertex filter drops every triangle that uses it
 * (BW_GPU_VSH_VERIFY counts such batches). */
static const char s_ffp_cbuf[] =
    "cbuffer F : register(b2) { float4 FM[4]; float4 FMV[4]; float4 Famb; float4 Fimm;"
    " float4 FL[40]; };\n";

/* role r's value as an HLSL float4, or `dflt` when there is no array */
static const char *ffp_in(const BwFfpKey *k, int r, const char *dflt, char buf[16])
{
    int i = k->in[r];
    if (i < 0)
        return dflt;
    snprintf(buf, 16, in_conv(k->conv, (uint32_t)i) ? "c%d" : "I.i%d", i);
    return buf;
}

/* row `m` (FM or FMV) times p, left to right; `w` the fourth term */
static void ffp_row(Buf *b, const char *dst, const char *m, int row, const char *p, const char *w)
{
    put(b, "  %s = %s[%d].x * %s.x + %s[%d].y * %s.y + %s[%d].z * %s.z + %s[%d].w%s;\n",
        dst, m, row, p, m, row, p, m, row, p, m, row, w);
}

size_t bw_ffp_hlsl(const BwFfpKey *k, char *out, size_t cap)
{
    Buf b = { out, 0, cap, 0 };
    uint32_t s, any_conv = 0;
    char nb[16];
    int i;

    for (s = 0; s < k->nin; s++)
        any_conv |= in_conv(k->conv, s);
    put(&b, "%s", s_head);
    if (any_conv)
        put(&b, "%s", s_conv_head);
    put(&b, "%s", s_ffp_cbuf);
    /* the inputs, as the program shaders declare them */
    put(&b, "struct VSIn { uint vid : SV_VertexID;");
    for (s = 0; s < k->nin; s++) {
        uint8_t c = in_conv(k->conv, s);
        if (!c)
            put(&b, " float4 i%u : ATTR%u;", s, s);
        else
            put(&b, " %s r%u : ATTR%u;", (c >> 4) == BW_XVS_SHORT ? "int4"
                                         : (c >> 4) == BW_XVS_CMP ? "uint" : "uint4", s, s);
    }
    put(&b, " };\n");
    put(&b, "VSOut vs_main(VSIn I) {\n");
    for (s = 0; s < k->nin; s++) {           /* P1's conversions, as bw_vsh_hlsl */
        static const char sw[] = "xyzw";
        uint8_t c = in_conv(k->conv, s);
        uint32_t j, sz = c & 15u;
        if (!c)
            continue;
        if ((c >> 4) == BW_XVS_CMP) {
            put(&b, "  float4 c%u = bwcmp(I.r%u);\n", s, s);
            continue;
        }
        put(&b, "  float4 c%u = float4(", s);
        for (j = 0; j < 4; j++) {
            const char *sep = j < 3 ? ", " : ");\n";
            if ((c >> 4) == BW_XVS_COLOR)
                put(&b, "bwT.Load(int(I.r%u.%c) + 3072)%s", s, "zyxw"[j], sep);
            else if (j >= sz)
                put(&b, "%s%s", j == 3 ? "1.0" : "0.0", sep);
            else if ((c >> 4) == BW_XVS_UBYTE)
                put(&b, "bwT.Load(int(I.r%u.%c) + 3072)%s", s, sw[j], sep);
            else
                put(&b, "(float)I.r%u.%c%s", s, sw[j], sep);
        }
    }

    /* ffp_vertex: the transform */
    put(&b, "  float4 vp = %s;\n  float4 cl;\n", ffp_in(k, BW_FFP_POS, "float4(0, 0, 0, 1)", nb));
    ffp_row(&b, "cl.x", "FM", 0, "vp", " * vp.w");
    ffp_row(&b, "cl.y", "FM", 1, "vp", " * vp.w");
    ffp_row(&b, "cl.z", "FM", 2, "vp", " * vp.w");
    ffp_row(&b, "cl.w", "FM", 3, "vp", " * vp.w");
    put(&b,
        "  float w = cl.w;\n"
        "  float4 P = float4((cl.x / w + K[59].x) * vsinfo.x, (cl.y / w + K[59].y) * vsinfo.y,\n"
        "                    cl.z / w + K[59].z, w);\n");

    /* the colour: lit_color, or the colour array, or white */
    if (k->lit) {
        put(&b, "  float4 nr = %s;\n", ffp_in(k, BW_FFP_NRM, "float4(0, 0, 0, 1)", nb));
        put(&b, "  float4 vc = %s;\n", ffp_in(k, BW_FFP_COL, "float4(0, 0, 0, 1)", nb));
        put(&b, "  float3 e, n;\n");
        ffp_row(&b, "e.x", "FMV", 0, "vp", "");
        ffp_row(&b, "e.y", "FMV", 1, "vp", "");
        ffp_row(&b, "e.z", "FMV", 2, "vp", "");
        put(&b,
            "  n.x = FMV[0].x * nr.x + FMV[0].y * nr.y + FMV[0].z * nr.z;\n"
            "  n.y = FMV[1].x * nr.x + FMV[1].y * nr.y + FMV[1].z * nr.z;\n"
            "  n.z = FMV[2].x * nr.x + FMV[2].y * nr.y + FMV[2].z * nr.z;\n"
            "  float len = sqrt(n.x * n.x + n.y * n.y + n.z * n.z);\n"
            "  if (len > 1e-8) { n.x = n.x / len; n.y = n.y / len; n.z = n.z / len; }\n"
            "  float4 col = float4(Famb.x, Famb.y, Famb.z, 0.0);\n");
        for (i = 0; i < 8; i++) {
            uint32_t kind = (k->light_mask >> (2 * i)) & 3;
            int L = 5 * i;
            if (!kind)
                continue;
            put(&b, "  { /* light %d */\n    float3 l; float att = 1.0; bool on = true;\n", i);
            if (kind == 1)
                put(&b, "    l = FL[%d].xyz;\n", L + 2);
            else
                put(&b,
                    "    l.x = FL[%d].x - e.x; l.y = FL[%d].y - e.y; l.z = FL[%d].z - e.z;\n"
                    "    float d = sqrt(l.x * l.x + l.y * l.y + l.z * l.z);\n"
                    "    if (d > FL[%d].w && FL[%d].w > 0.0) on = false;\n"
                    "    if (d > 1e-8) { l.x = l.x / d; l.y = l.y / d; l.z = l.z / d; }\n"
                    "    att = FL[%d].x + FL[%d].y * d + FL[%d].z * d * d;\n"
                    "    att = att > 1e-8 ? 1.0 / att : 1.0;\n",
                    L + 3, L + 3, L + 3, L, L, L + 4, L + 4, L + 4);
            put(&b,
                "    if (on) {\n"
                "      float ndl = n.x * l.x + n.y * l.y + n.z * l.z;\n"
                "      if (ndl < 0.0) ndl = 0.0;\n"
                "      float3 am = FL[%d].xyz, df = FL[%d].xyz;\n", L, L + 1);
            if ((k->colmat & 1) && k->in[BW_FFP_COL] >= 0)
                put(&b, "      am.x = am.x * vc.x; am.y = am.y * vc.y; am.z = am.z * vc.z;\n");
            if ((k->colmat & 2) && k->in[BW_FFP_COL] >= 0)
                put(&b, "      df.x = df.x * vc.x; df.y = df.y * vc.y; df.z = df.z * vc.z;\n");
            put(&b,
                "      col.x = col.x + att * (am.x + df.x * ndl);\n"
                "      col.y = col.y + att * (am.y + df.y * ndl);\n"
                "      col.z = col.z + att * (am.z + df.z * ndl);\n"
                "    }\n  }\n");
        }
        put(&b, "  col.w = %s;\n",
            (k->colmat & 2) && k->in[BW_FFP_COL] >= 0 ? "vc.w" : "Famb.w");
    } else
        put(&b, "  float4 col = %s;\n", ffp_in(k, BW_FFP_COL, "float4(1, 1, 1, 1)", nb));
    put(&b,
        "  col.x = col.x < 0.0 ? 0.0 : (col.x > 1.0 ? 1.0 : col.x);\n"
        "  col.y = col.y < 0.0 ? 0.0 : (col.y > 1.0 ? 1.0 : col.y);\n"
        "  col.z = col.z < 0.0 ? 0.0 : (col.z > 1.0 ? 1.0 : col.z);\n"
        "  col.w = col.w < 0.0 ? 0.0 : (col.w > 1.0 ? 1.0 : col.w);\n");

    /* ffp_fog_distance */
    if (!k->fog_on)
        put(&b, "  float fd = 0.0;\n");
    else if (k->fog_gen == 0)
        put(&b, "  float4 fv = %s;\n  float fd = fv.w < 0.0 ? 0.0 : (fv.w > 1.0 ? 1.0 : fv.w);\n",
            ffp_in(k, BW_FFP_FOG, "Fimm", nb));
    else if (k->fog_gen == 6)
        put(&b, "  float fd = (%s).x;\n", ffp_in(k, BW_FFP_FOG, "Fimm", nb));
    else {
        put(&b, "  float3 fe;\n");
        ffp_row(&b, "fe.x", "FMV", 0, "vp", " * vp.w");
        ffp_row(&b, "fe.y", "FMV", 1, "vp", " * vp.w");
        ffp_row(&b, "fe.z", "FMV", 2, "vp", " * vp.w");
        if (k->fog_gen == 1)
            put(&b, "  float fd = sqrt(fe.x * fe.x + fe.y * fe.y + fe.z * fe.z);\n");
        else
            put(&b, "  float fd = K[57].x * fe.x + K[57].y * fe.y + K[57].z * fe.z + K[57].w;\n%s",
                k->fog_gen == 3 ? "  fd = abs(fd);\n" : "");
    }

    /* the program shaders' epilogue, from P on (bw_vsh_hlsl) */
    put(&b,
        "  VSOut o;\n"
        "  float x = P.x * surf.z * 2.0 - 1.0;\n"
        BW_WS_REMAP_HLSL
        "  float y = 1.0 - P.y * surf.w * 2.0;\n"
        "  float zr = (P.z - depth.x) * depth.y;\n"
        "  float z = depth.z > 0.5 ? saturate(zr) : zr;\n"
        "  o.p = float4(x * w, y * w, z * w, w);\n"
        /* ffp_vertex: no image at |w| <= 1e-6 (NaN: the filter drops it) */
        "  if (abs(w) <= 1e-6) o.p = asfloat(0x7fc00000).xxxx;\n"
        "  o.clip.x = w - 1e-3;\n"
        "  o.clip.y = vsinfo.z > 0.5 ? (P.z - K[59].z) * w : 1.0;\n"
        "  o.d0 = col; o.d1 = float4(0, 0, 0, 0);\n"
        "  o.fog = fogf(fd); o.fogp = fogr(fd);\n");
    put(&b, "  o.tc0 = %s;\n", ffp_in(k, BW_FFP_TC0, "float4(0, 0, 0, 1)", nb));
    put(&b, "  o.tc1 = %s;\n", ffp_in(k, BW_FFP_TC1, "float4(0, 0, 0, 1)", nb));
    put(&b, "  o.tc2 = %s;\n", ffp_in(k, BW_FFP_TC2, "float4(0, 0, 0, 1)", nb));
    put(&b, "  o.tc3 = %s;\n", ffp_in(k, BW_FFP_TC3, "float4(0, 0, 0, 1)", nb));
    put(&b, "  return o;\n}\n");
    if (b.overflow)
        return 0;
    return b.n;
}
