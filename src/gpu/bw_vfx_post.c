/*
 * bw_vfx_post.c - the frame's way to the screen in Remastered (agent D9).
 *
 * The finished frame (the surface the game flipped, at the render scale)
 * goes through the enabled effects into the window, or into an off-screen
 * output of the same size when nothing presents (BW_GPU_PRESENT=0 runs):
 *
 *   [FXAA] -> [sharpen (CAS)] -> [bloom chain] -> [OLED static map] ->
 *   final: scale to the output picture (exact copy when the render scale
 *   matches it, box average when it is larger, Catmull-Rom when smaller),
 *   bloom added, OLED dimming, then 8-bit dithered output (VFX_PRECISION)
 *   or HDR scRGB (VFX_HDR: values up to 1.0 at paper white, light the game
 *   added beyond 1.0 rolls off toward the display's peak).
 *
 * Original never gets here: gpu_present() calls this only when
 * bw_vfx_active(), which is 0 in Original.
 */
#define BW_VFX_INTERNAL
#include "bw_vfx.h"
#include "bw_vfx_int.h"
#include "bw_gpu_failtest.h"   /* 0.2.5 item 25: BW_GPU_FAILTEST (test aid) */
#include <dxgi1_6.h>           /* item 45: IDXGIOutput6, the screen's colour mode */
#include "bw_hdr.h"            /* item 97: the screen, shared with the plain present */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int bw_ab_odd(void);                /* bw_ab.c: the current frame is an odd (fix-off) one */

typedef struct {
    ID3D11Texture2D *t;
    ID3D11RenderTargetView *rtv;
    ID3D11ShaderResourceView *srv;
    UINT w, h;
    DXGI_FORMAT f;
} BwRt;

static void rt_release(BwRt *r)
{
    if (r->srv) ID3D11ShaderResourceView_Release(r->srv);
    if (r->rtv) ID3D11RenderTargetView_Release(r->rtv);
    if (r->t) ID3D11Texture2D_Release(r->t);
    memset(r, 0, sizeof *r);
}

/* A target is complete only with its texture and both views. 0.2.5 item 44
 * (a port fix, every edition that gets here): a view that failed used to
 * leave the texture behind, and the next call called that half-made target
 * complete, so its NULL view reached the passes and the output shot
 * (ID3D11View_GetResource(NULL): the off-screen crash). Now a failure
 * releases everything and returns 0: that effect is skipped this frame and
 * the target is made again next frame. Logged ("[VFX] post-processing
 * target ..."): the first three in full, then every 300th with the count. */
static int rt_ensure(BwRt *r, UINT w, UINT h, DXGI_FORMAT f)
{
    static unsigned fails;
    D3D11_TEXTURE2D_DESC td;
    HRESULT hr;
    const char *what = "texture";
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (r->t && r->rtv && r->srv && r->w == w && r->h == h && r->f == f)
        return 1;
    rt_release(r);
    memset(&td, 0, sizeof td);
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = f;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    hr = ID3D11Device_CreateTexture2D(g_vfx_dev, &td, NULL, &r->t);
    if (SUCCEEDED(hr)) {
        what = "render-target view";
        hr = BW_GPU_FT("post", E_OUTOFMEMORY,    /* item 25 test aid: this view fails */
                       ID3D11Device_CreateRenderTargetView(g_vfx_dev, (ID3D11Resource *)r->t, NULL, &r->rtv));
    }
    if (SUCCEEDED(hr)) {
        what = "shader-resource view";
        hr = ID3D11Device_CreateShaderResourceView(g_vfx_dev, (ID3D11Resource *)r->t, NULL, &r->srv);
    }
    if (FAILED(hr) || !r->t || !r->rtv || !r->srv) {
        fails++;
        if (fails <= 3 || fails % 300 == 0)
            fprintf(stderr, "[VFX] post-processing target %ux%u (DXGI format %d): %s failed (0x%08lX);"
                            " that effect is skipped this frame and retried next (%u failures so far)\n",
                    w, h, (int)f, what, (unsigned long)hr, fails);
        rt_release(r);
        return 0;
    }
    r->w = w; r->h = h; r->f = f;
    return 1;
}

/* ---- shaders ---------------------------------------------------------------- */

static const char s_post_src[] =
"cbuffer P : register(b0) {\n"
"  float4 srcsz;  // source w, h, 1/w, 1/h\n"
"  float4 dstsz;  // pass target w, h, 1/w, 1/h\n"
"  float4 org;    // x, y: the picture's origin in the target; z: scale mode; w: frame\n"
"  float4 opt;    // x: dither, y: hdr, z: bloom strength, w: OLED dim\n"
"  float4 hdrp;   // x: paper nits, y: peak nits, z: sharpen, w: dt (s)\n"
"  float4 ext;    // x: a scene copy is bound (t3); y: the gamma ramp is bound (t4); z: HDR knee\n"
"  float4 eotf;   // HDR decode (bwh_decode): x 2.4 BT.1886 (y a, z b), else 2.2; w reference white / 80\n"
"  float4 eetf;   // the EETF (bwh_eetf): x b, y maxLum, z KS, w PQ span of the source (0: none)\n"
"};\n"
"Texture2D glut : register(t4);\n"
/* the title's DAC gamma ramp (D10, TK-50): linear between its 256 entries;
 * light above 1.0 (HDR's float targets) keeps the ramp's top as its scale */
"float rampc(float c, int ch) { float x = saturate(c) * 255.0, f = floor(x);\n"
"  int a = (int)f, b = min(a + 1, 255);\n"
"  return lerp(glut.Load(int3(a, 0, 0))[ch], glut.Load(int3(b, 0, 0))[ch], x - f); }\n"
"float3 ramp3(float3 c) { float3 top = glut.Load(int3(255, 0, 0)).rgb;\n"
"  return float3(rampc(c.r, 0), rampc(c.g, 1), rampc(c.b, 2)) + max(c - 1.0, 0.0) * top; }\n"
"Texture2D img : register(t0); Texture2D aux : register(t1); Texture2D aux2 : register(t2);\n"
"Texture2D scn : register(t3);\n"
"SamplerState sl : register(s0); SamplerState sp : register(s1);\n"
"struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; };\n"
"float lum(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }\n"
"float3 tap(float2 uv) { return img.SampleLevel(sl, uv, 0).rgb; }\n"
"float3 tapp(float2 uv) { return img.SampleLevel(sp, uv, 0).rgb; }\n"
/* Catmull-Rom in 9 bilinear taps (the usual optimisation) */
"float3 bicubic(float2 uv) {\n"
"  float2 sp0 = uv * srcsz.xy; float2 t1 = floor(sp0 - 0.5) + 0.5; float2 f = sp0 - t1;\n"
"  float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f)); float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);\n"
"  float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f)); float2 w3 = f * f * (-0.5 + 0.5 * f);\n"
"  float2 w12 = w1 + w2; float2 t12 = (t1 + w2 / w12) * srcsz.zw;\n"
"  float2 t0 = (t1 - 1.0) * srcsz.zw; float2 t3 = (t1 + 2.0) * srcsz.zw;\n"
"  float3 r = tap(float2(t0.x, t0.y)) * w0.x * w0.y + tap(float2(t12.x, t0.y)) * w12.x * w0.y\n"
"           + tap(float2(t3.x, t0.y)) * w3.x * w0.y + tap(float2(t0.x, t12.y)) * w0.x * w12.y\n"
"           + tap(float2(t12.x, t12.y)) * w12.x * w12.y + tap(float2(t3.x, t12.y)) * w3.x * w12.y\n"
"           + tap(float2(t0.x, t3.y)) * w0.x * w3.y + tap(float2(t12.x, t3.y)) * w12.x * w3.y\n"
"           + tap(float2(t3.x, t3.y)) * w3.x * w3.y;\n"
"  return max(r, 0.0); }\n"
"float3 boxdown(float2 uv) {\n"
"  float2 f = srcsz.xy * dstsz.zw;\n"
"  int nx = clamp((int)ceil(f.x / 2.0), 1, 4), ny = clamp((int)ceil(f.y / 2.0), 1, 4);\n"
"  float2 o = uv - 0.5 * dstsz.zw; float2 st = dstsz.zw / float2(nx, ny); float3 c = 0;\n"
"  for (int y = 0; y < ny; y++) for (int x = 0; x < nx; x++) c += tap(o + (float2(x, y) + 0.5) * st);\n"
"  return c / (nx * ny); }\n"
"float ign(float2 p) { return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715)))); }\n"
/* item 99 (the maintainer): with ext.w, the linear BT.709 values read as P3-D65 and
 * written back as BT.709 for scRGB (negative values allowed): the inverse of
 * BT.709 to P3-D65, from the BT.709 primaries and SMPTE EG 432-1's P3-D65
 * (R 0.680 0.320, G 0.265 0.690, B 0.150 0.060, white D65). Rows sum to 1:
 * white and greys don't change. */
"float3 p3wide(float3 c) { return ext.w > 0.5 ? float3(dot(c, float3(1.224940, -0.224940, 0.0)),\n"
"  dot(c, float3(-0.042057, 1.042057, 0.0)), dot(c, float3(-0.019638, -0.078636, 1.098274))) : c; }\n"
/* ---- the final pass ---- */
"float4 ps_final(V i) : SV_Target {\n"
"  float2 q = i.p.xy - org.xy; float2 uv = q * dstsz.zw;\n"
"  float3 c;\n"
"  if (org.z < 0.5) c = img.Load(int3(int2(q), 0)).rgb;\n"
"  else if (org.z < 1.5) c = boxdown(uv);\n"
"  else c = bicubic(uv);\n"
"  if (opt.z > 0.0) c += aux.SampleLevel(sl, uv, 0).rgb * opt.z;\n"
"  if (opt.w > 0.0) {\n"
"    float2 m = aux2.SampleLevel(sl, uv, 0).xy;\n"
     /* with a scene copy: only where the HUD drew (the frame differs from it) */
"    float hud = 1.0;\n"
"    if (ext.x > 0.5) { float3 d = abs(c - scn.SampleLevel(sl, uv, 0).rgb); hud = saturate(max(d.r, max(d.g, d.b)) * 12.0); }\n"
"    c *= 1.0 - opt.w * hud * smoothstep(0.5, 1.0, m.x) * smoothstep(0.35, 0.8, lum(c)); }\n"
"  if (ext.y > 0.5) c = ramp3(c);\n"
/* item 97, opt.y 3: BT.2408's direct mapping (hdr_standards.md sections 3
 * and 7): BT.1886 with the screen's black, 100% at reference white */
"  if (opt.y > 2.5) return float4(p3wide(bwh_decode(saturate(c), eotf) * eotf.w), 1.0);\n"
"  if (opt.y > 0.5 && opt.y < 1.5) {\n"
/* item 97 (D-026): the game's SDR picture decoded with the BT.1886 EOTF
 * (BT.2408 section 5.1.1), relative to reference white; 0.2.4: a 2.2 power */
"    float3 lin = bwh_decode(c, eotf);\n"
"    float P = hdrp.y / hdrp.x;\n"
"    float3 y;\n"
/* item 46: a soft knee at K = ext.z (of paper white): unchanged below it;
 * above it y = P - (P-K)(1 + q u) e^(-b u), u = lin - K, q = b - 1/(P-K),
 * which leaves the knee with slope 1 (no kink), rises at most about 2.4x
 * and eases into P = peak/paper. ext.z 0: 0.2.4's curve (a slope jump from
 * 1 to 1.5(P-1) at paper white: the bright band) */
"    if (ext.z > 0.0) {\n"
"      float K = ext.z, R = P - K, b = max(2.0, 1.0 / R), q = b - 1.0 / R;\n"
"      float3 u = max(lin - K, 0.0);\n"
"      y = lin <= K ? lin : P - R * (1.0 + q * u) * exp(-b * u);\n"
"    } else\n"
"      y = min(lin, 1.0) + (P - 1.0) * (1.0 - exp(-max(lin - 1.0, 0.0) * 1.5));\n"
/* item 97: BT.2408 Annex 5's EETF in BT.2100's terms (BT.2087 to BT.2020,
 * PQ normalised to the source range [0, eetf.w]), for the screen's black,
 * then held to the peak; eetf.w 0: none (BW_VFX_HDR_EETF=0) */
"    float3 L = max(y, 0.0) * hdrp.x;\n"
"    if (eetf.w > 0.0) {\n"
"      float3 L2 = bwh_709to2020(L);\n"
"      float3 e = bwh_eetf(bwh_pq(L2 / 10000.0) / eetf.w, eetf);\n"
"      L2 = min(bwh_pq_eotf(e * eetf.w) * 10000.0, hdrp.y);\n"
"      L = bwh_2020to709(L2); }\n"
"    return float4(p3wide(L / 80.0), 1.0); }\n"
"  if (opt.x > 0.5) {\n"
"    c = saturate(c);\n"
"    float n = ign(i.p.xy) + ign(i.p.xy + float2(37.0, 17.0)) - 1.0;\n"
"    float3 room = saturate(min(c, 1.0 - c) * 127.5);\n"   /* none at pure black or white */
"    c += n * room / 255.0; }\n"
"  c = saturate(c);\n"
/* item 45, opt.y 2: the SDR picture through an scRGB swap chain (its screen
 * left HDR mode): the sRGB values as linear scRGB, which Windows encodes back
 * to the same sRGB values for the SDR screen (1.0 = SDR white) */
"  if (opt.y > 1.5 && opt.y < 2.5) c = c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);\n"
"  return float4(c, 1.0); }\n"
/* ---- FXAA (after FXAA 3.11's quality path, compact) ---- */
"float lt(float2 uv) { return lum(tapp(uv)); }\n"
"float4 ps_fxaa(V i) : SV_Target {\n"
"  float2 r = srcsz.zw; float2 uv = i.p.xy * r;\n"
"  float3 cM = tapp(uv); float lM = lum(cM);\n"
"  float lN = lt(uv + float2(0, -r.y)), lS = lt(uv + float2(0, r.y));\n"
"  float lW = lt(uv + float2(-r.x, 0)), lE = lt(uv + float2(r.x, 0));\n"
"  float lMin = min(lM, min(min(lN, lS), min(lW, lE))), lMax = max(lM, max(max(lN, lS), max(lW, lE)));\n"
"  float range = lMax - lMin;\n"
"  if (range < max(0.0312, lMax * 0.125)) return float4(cM, 1.0);\n"
"  float lNW = lt(uv - r), lSE = lt(uv + r), lNE = lt(uv + float2(r.x, -r.y)), lSW = lt(uv + float2(-r.x, r.y));\n"
"  float eH = abs(lNW + lNE - 2.0 * lN) + 2.0 * abs(lW + lE - 2.0 * lM) + abs(lSW + lSE - 2.0 * lS);\n"
"  float eV = abs(lNW + lSW - 2.0 * lW) + 2.0 * abs(lN + lS - 2.0 * lM) + abs(lNE + lSE - 2.0 * lE);\n"
"  bool hz = eH >= eV;\n"
"  float l1 = hz ? lN : lW, l2 = hz ? lS : lE, g1 = l1 - lM, g2 = l2 - lM;\n"
"  bool st1 = abs(g1) >= abs(g2); float gs = 0.25 * max(abs(g1), abs(g2));\n"
"  float stp = hz ? r.y : r.x; float avg;\n"
"  if (st1) { stp = -stp; avg = 0.5 * (l1 + lM); } else avg = 0.5 * (l2 + lM);\n"
"  float2 cu = uv; if (hz) cu.y += stp * 0.5; else cu.x += stp * 0.5;\n"
"  float2 off = hz ? float2(r.x, 0) : float2(0, r.y);\n"
"  float2 u1 = cu - off, u2 = cu + off;\n"
"  float e1 = lum(tap(u1)) - avg, e2 = lum(tap(u2)) - avg;\n"
"  bool d1 = abs(e1) >= gs, d2 = abs(e2) >= gs;\n"
"  [loop] for (int k = 0; k < 10 && !(d1 && d2); k++) {\n"
"    float qk = k < 3 ? 1.0 : (k < 6 ? 1.5 : (k < 8 ? 2.0 : 4.0));\n"
"    if (!d1) { u1 -= off * qk; e1 = lum(tap(u1)) - avg; d1 = abs(e1) >= gs; }\n"
"    if (!d2) { u2 += off * qk; e2 = lum(tap(u2)) - avg; d2 = abs(e2) >= gs; } }\n"
"  float s1 = hz ? uv.x - u1.x : uv.y - u1.y, s2 = hz ? u2.x - uv.x : u2.y - uv.y;\n"
"  bool c1 = s1 < s2; float po = -min(s1, s2) / (s1 + s2) + 0.5;\n"
"  bool ok = ((c1 ? e1 : e2) < 0.0) != (lM < avg);\n"
"  float fo = ok ? po : 0.0;\n"
"  float la = (2.0 * (lN + lS + lW + lE) + lNW + lNE + lSW + lSE) / 12.0;\n"
"  float sb = saturate(abs(la - lM) / range); sb = (-2.0 * sb + 3.0) * sb * sb;\n"
"  fo = max(fo, sb * sb * 0.75);\n"
"  float2 fu = uv; if (hz) fu.y += fo * stp; else fu.x += fo * stp;\n"
"  return float4(tap(fu), 1.0); }\n"
/* ---- contrast-adaptive sharpening (after AMD FidelityFX CAS) ---- */
"float4 ps_cas(V i) : SV_Target {\n"
"  int2 p = int2(i.p.xy); int2 mx = int2(srcsz.xy) - 1;\n"
"  float3 a = img.Load(int3(clamp(p + int2(-1, -1), 0, mx), 0)).rgb, b = img.Load(int3(clamp(p + int2(0, -1), 0, mx), 0)).rgb;\n"
"  float3 c = img.Load(int3(clamp(p + int2(1, -1), 0, mx), 0)).rgb, d = img.Load(int3(clamp(p + int2(-1, 0), 0, mx), 0)).rgb;\n"
"  float3 e = img.Load(int3(p, 0)).rgb, f = img.Load(int3(clamp(p + int2(1, 0), 0, mx), 0)).rgb;\n"
"  float3 g = img.Load(int3(clamp(p + int2(-1, 1), 0, mx), 0)).rgb, h = img.Load(int3(clamp(p + int2(0, 1), 0, mx), 0)).rgb;\n"
"  float3 k = img.Load(int3(clamp(p + int2(1, 1), 0, mx), 0)).rgb;\n"
"  float3 mn = min(min(min(d, e), min(f, b)), h); mn += min(mn, min(min(a, c), min(g, k)));\n"
"  float3 mxc = max(max(max(d, e), max(f, b)), h); mxc += max(mxc, max(max(a, c), max(g, k)));\n"
"  float3 amp = sqrt(saturate(min(mn, 2.0 - mxc) / max(mxc, 1e-5)));\n"
"  float3 w = amp * (-1.0 / lerp(8.0, 5.0, hdrp.z));\n"
"  float3 o = (b * w + d * w + f * w + h * w + e) / (1.0 + 4.0 * w);\n"
"  return float4(max(o, 0.0), 1.0); }\n"
/* ---- bloom: bright pass, downsample, upsample-and-add ---- */
"float4 ps_bright(V i) : SV_Target {\n"
"  float3 c = boxdown(i.p.xy * dstsz.zw);\n"
"  float l = max(c.r, max(c.g, c.b));\n"
"  float k = saturate((l - 0.75) / 0.25); k *= k;\n"   /* soft knee from 0.75 */
"  return float4(c * k, 1.0); }\n"
"float4 ps_down(V i) : SV_Target { return float4(boxdown(i.p.xy * dstsz.zw), 1.0); }\n"
"float4 ps_up(V i) : SV_Target {\n"
"  float2 uv = i.p.xy * dstsz.zw; float2 r = srcsz.zw;\n"   /* srcsz: the smaller level */
"  float3 s = tap(uv + float2(-r.x, -r.y)) + 2.0 * tap(uv + float2(0, -r.y)) + tap(uv + float2(r.x, -r.y))\n"
"           + 2.0 * tap(uv + float2(-r.x, 0)) + 4.0 * tap(uv) + 2.0 * tap(uv + float2(r.x, 0))\n"
"           + tap(uv + float2(-r.x, r.y)) + 2.0 * tap(uv + float2(0, r.y)) + tap(uv + r);\n"
"  return float4(s / 16.0 + aux.SampleLevel(sl, uv, 0).rgb, 1.0); }\n"
/* ---- OLED care: per-cell brightness and how long it has not changed ---- */
"float4 ps_oled(V i) : SV_Target {\n"
"  float2 uv = i.p.xy * dstsz.zw;\n"
"  float l = lum(saturate(boxdown(uv)));\n"
"  float2 prev = aux.SampleLevel(sp, uv, 0).xy;\n"
"  float s = abs(l - prev.y) < 0.03 ? prev.x + hdrp.w / 90.0 : prev.x - hdrp.w / 0.4;\n"
"  return float4(saturate(s), l, 0.0, 1.0); }\n";

enum { PS_FINAL, PS_FXAA, PS_CAS, PS_BRIGHT, PS_DOWN, PS_UP, PS_OLED, PS_N };
static const char *const s_ps_names[PS_N] = {
    "ps_final", "ps_fxaa", "ps_cas", "ps_bright", "ps_down", "ps_up", "ps_oled" };
static ID3D11PixelShader *s_ps[PS_N];
static ID3D11Buffer *s_pcb;
static int s_ready = -1;

typedef struct {
    float srcsz[4], dstsz[4], org[4], opt[4], hdrp[4], ext[4], eotf[4], eetf[4];
} BwPostK;

static ID3D11ShaderResourceView *s_scene, *s_glut;

void bw_vfx_set_scene(ID3D11ShaderResourceView *scene)
{
    s_scene = scene;
}

void bw_vfx_set_gamma_lut(ID3D11ShaderResourceView *lut)
{
    s_glut = lut;
}

#define BLOOM_LEVELS 5
static BwRt s_fx, s_sh, s_bl[BLOOM_LEVELS], s_bu[BLOOM_LEVELS], s_oled[2], s_off;
static int s_oled_cur;
static unsigned s_frame;

static int post_ready(void)
{
    int i;
    size_t n1, n2;
    char *src;
    if (s_ready >= 0)
        return s_ready;
    s_ready = 0;
    if (!g_vfx_dev)
        return 0;
    /* item 97: bw_hdr.c's shared HLSL (BT.1886, PQ, BT.2087, the EETF) first */
    n1 = strlen(bw_hdr_hlsl);
    n2 = sizeof s_post_src - 1;
    if (!(src = (char *)malloc(n1 + n2)))
        return 0;
    memcpy(src, bw_hdr_hlsl, n1);
    memcpy(src + n1, s_post_src, n2);
    for (i = 0; i < PS_N; i++)
        if (!(s_ps[i] = bw_vfx_compile_ps(src, n1 + n2, s_ps_names[i]))) {
            free(src);
            return 0;
        }
    free(src);
    s_pcb = bw_vfx_make_cb(sizeof(BwPostK));
    s_ready = s_pcb != NULL;
    return s_ready;
}

static void sz(float o[4], UINT w, UINT h)
{
    o[0] = (float)w; o[1] = (float)h; o[2] = 1.0f / w; o[3] = 1.0f / h;
}

static void pass(BwRt *dst, int ps, ID3D11ShaderResourceView *a, UINT aw, UINT ah,
                 ID3D11ShaderResourceView *b, BwPostK *k)
{
    D3D11_VIEWPORT vp;
    ID3D11ShaderResourceView *srv[2] = { a, b };
    sz(k->srcsz, aw, ah);
    sz(k->dstsz, dst->w, dst->h);
    bw_vfx_update_cb(s_pcb, k, sizeof *k);
    vp.TopLeftX = 0; vp.TopLeftY = 0;
    vp.Width = (float)dst->w; vp.Height = (float)dst->h;
    vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
    bw_vfx_fullscreen(dst->rtv, &vp, s_ps[ps], s_pcb, srv, b ? 2 : 1);
}

/* ---- timing ------------------------------------------------------------------ */

/* The effects' GPU time, from timestamp queries in a ring of TQ frames. A
 * slot is used again only after its last results were read (0.2.5, B2's
 * finding: reusing it while they were still outstanding made the debug
 * layer warn #408/#410 every frame, "abandoning previous results"); when the
 * GPU is more than TQ frames behind, that frame simply isn't timed. Never
 * waits. */
#define TQ 8
static ID3D11Query *s_qdis[TQ], *s_qa[TQ], *s_qb[TQ];
static uint8_t s_qbusy[TQ];         /* ended, results not read yet */
static int s_qn, s_qskip;
static double s_ms_sum, s_ms_avg;
static unsigned s_ms_cnt;

static void tq_begin(void)
{
    int i = s_qn % TQ;
    s_qskip = 1;
    if (!s_qdis[i]) {
        D3D11_QUERY_DESC qd;
        qd.MiscFlags = 0;
        qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
        ID3D11Device_CreateQuery(g_vfx_dev, &qd, &s_qdis[i]);
        qd.Query = D3D11_QUERY_TIMESTAMP;
        ID3D11Device_CreateQuery(g_vfx_dev, &qd, &s_qa[i]);
        ID3D11Device_CreateQuery(g_vfx_dev, &qd, &s_qb[i]);
    }
    if (!s_qdis[i] || !s_qa[i] || !s_qb[i])
        return;
    if (s_qbusy[i]) {
        /* this slot's last results: read them if they are in, else skip */
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
        UINT64 a, b;
        if (ID3D11DeviceContext_GetData(g_vfx_ctx, (ID3D11Asynchronous *)s_qdis[i], &dj, sizeof dj,
                                        D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK
            || ID3D11DeviceContext_GetData(g_vfx_ctx, (ID3D11Asynchronous *)s_qa[i], &a, sizeof a,
                                           D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK
            || ID3D11DeviceContext_GetData(g_vfx_ctx, (ID3D11Asynchronous *)s_qb[i], &b, sizeof b,
                                           D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
            return;                         /* still in flight: this frame isn't timed */
        s_qbusy[i] = 0;
        if (!dj.Disjoint && dj.Frequency) {
            s_ms_sum += (double)(b - a) * 1000.0 / (double)dj.Frequency;
            if (++s_ms_cnt >= 30) {
                s_ms_avg = s_ms_sum / s_ms_cnt;
                s_ms_sum = 0;
                s_ms_cnt = 0;
            }
        }
    }
    ID3D11DeviceContext_Begin(g_vfx_ctx, (ID3D11Asynchronous *)s_qdis[i]);
    ID3D11DeviceContext_End(g_vfx_ctx, (ID3D11Asynchronous *)s_qa[i]);
    s_qskip = 0;
}

static void tq_end(void)
{
    int i = s_qn % TQ;
    if (s_qskip)
        return;
    ID3D11DeviceContext_End(g_vfx_ctx, (ID3D11Asynchronous *)s_qb[i]);
    ID3D11DeviceContext_End(g_vfx_ctx, (ID3D11Asynchronous *)s_qdis[i]);
    s_qbusy[i] = 1;
    s_qn++;
}

double bw_vfx_post_ms(void)
{
    return s_ms_avg;
}

/* ---- output shots ---------------------------------------------------------------- */

/* item 97: the paper white (nits) and decode power of the frame being shot,
 * so an HDR shot's SDR part comes back to the game's own values */
static float s_shot_paper = 203.0f, s_shot_gamma = 2.2f;

static float half_to_float(uint16_t h)
{
    uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
    float v;
    if (e == 0) v = (float)m / 1024.0f / 16384.0f;
    else if (e == 31) v = 65504.0f;
    else v = (1.0f + m / 1024.0f) * ldexpf(1.0f, (int)e - 15);
    return s ? -v : v;
}

/* The picture as a 24-bit BMP, from the target texture (any of the output
 * formats; an HDR picture is written as its SDR part: paper white = 255;
 * mode 2, the SDR picture in scRGB, back to its sRGB values). */
static void shot_write(ID3D11Resource *res, UINT x0, UINT y0, UINT w, UINT h, DXGI_FORMAT f, int mode)
{
    static unsigned seq;
    static ID3D11Texture2D *stg;
    static UINT sw, shh;
    static DXGI_FORMAT sf;
    const char *prefix = getenv("RECOMP_FB_DUMP");
    char path[600];
    D3D11_MAPPED_SUBRESOURCE m;
    D3D11_BOX box;
    FILE *fp;
    uint8_t hd[54], *row;
    uint32_t rowb = (w * 3 + 3) & ~3u, size = 54 + rowb * h, y, x;
    float paper = s_shot_paper / 80.0f, ig = 1.0f / s_shot_gamma;    /* item 97: as the frame was drawn */

    if (!prefix || !*prefix || !w || !h)
        return;
    if (!stg || sw != w || shh != h || sf != f) {
        D3D11_TEXTURE2D_DESC td;
        if (stg) ID3D11Texture2D_Release(stg);
        stg = NULL;
        memset(&td, 0, sizeof td);
        td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = f;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(ID3D11Device_CreateTexture2D(g_vfx_dev, &td, NULL, &stg)))
            return;
        sw = w; shh = h; sf = f;
    }
    box.left = x0; box.top = y0; box.front = 0;
    box.right = x0 + w; box.bottom = y0 + h; box.back = 1;
    ID3D11DeviceContext_CopySubresourceRegion(g_vfx_ctx, (ID3D11Resource *)stg, 0, 0, 0, 0, res, 0, &box);
    if (FAILED(ID3D11DeviceContext_Map(g_vfx_ctx, (ID3D11Resource *)stg, 0, D3D11_MAP_READ, 0, &m)))
        return;
    _snprintf(path, sizeof path, "%svfx_%05u.bmp", prefix, seq++);
    path[sizeof path - 1] = 0;
    fp = fopen(path, "wb");
    row = (uint8_t *)malloc(rowb);
    if (fp && row) {
        memset(hd, 0, sizeof hd);
        hd[0] = 'B'; hd[1] = 'M';
        memcpy(hd + 2, &size, 4);
        hd[10] = 54; hd[14] = 40;
        memcpy(hd + 18, &w, 4);
        { int32_t hh = -(int32_t)h; memcpy(hd + 22, &hh, 4); }
        hd[26] = 1; hd[28] = 24;
        fwrite(hd, 1, 54, fp);
        for (y = 0; y < h; y++) {
            const uint8_t *src = (const uint8_t *)m.pData + (size_t)y * m.RowPitch;
            memset(row, 0, rowb);
            for (x = 0; x < w; x++) {
                if (f == DXGI_FORMAT_R16G16B16A16_FLOAT) {
                    const uint16_t *p = (const uint16_t *)src + x * 4;
                    int k;
                    for (k = 0; k < 3; k++) {
                        float v = half_to_float(p[k]);
                        if (mode == 2)
                            v = v <= 0.0f ? 0.0f : v >= 1.0f ? 1.0f
                              : v <= 0.0031308f ? v * 12.92f : 1.055f * powf(v, 1.0f / 2.4f) - 0.055f;
                        else {
                            v /= paper;
                            v = v <= 0.0f ? 0.0f : v >= 1.0f ? 1.0f : powf(v, ig);
                        }
                        row[x * 3 + 2 - k] = (uint8_t)(v * 255.0f + 0.5f);
                    }
                } else {
                    const uint8_t *p = src + x * 4;        /* B8G8R8A8 */
                    row[x * 3 + 0] = p[0]; row[x * 3 + 1] = p[1]; row[x * 3 + 2] = p[2];
                }
            }
            fwrite(row, 1, rowb, fp);
        }
        if (f == DXGI_FORMAT_R16G16B16A16_FLOAT && (mode == 1 || mode == 3)) {
            /* item 46 test aid, BW_VFX_SHOT_HDR=1: the HDR picture itself as
             * well, as a float PFM next to the BMP, in nits (item 97: so two
             * mappings with different paper whites compare directly) */
            static int want = -1;
            if (want < 0) {
                const char *e = getenv("BW_VFX_SHOT_HDR");
                want = e && (e[0] == '1' || e[0] == '2' || e[0] == '3');
            }
            if (want) {
                char pp[600];
                FILE *pf;
                size_t n = strlen(path);
                memcpy(pp, path, n + 1);
                if (n > 4) memcpy(pp + n - 4, ".pfm", 5);
                pf = fopen(pp, "wb");
                if (pf) {
                    float *fr = (float *)malloc((size_t)w * 12);
                    fprintf(pf, "PF\n%u %u\n-1.0\n", w, h);
                    for (y = h; fr && y-- > 0; ) {          /* PFM rows run bottom to top */
                        const uint16_t *p = (const uint16_t *)((const uint8_t *)m.pData + (size_t)y * m.RowPitch);
                        for (x = 0; x < w; x++) {
                            int k;
                            for (k = 0; k < 3; k++)
                                fr[x * 3 + k] = half_to_float(p[x * 4 + k]) * 80.0f;    /* scRGB 1.0 = 80 nits */
                        }
                        fwrite(fr, 4, (size_t)w * 3, pf);
                    }
                    free(fr);
                    fclose(pf);
                    fprintf(stderr, "[VFX] shot %s (HDR, linear, nits; paper white %.0f nits)\n", pp, s_shot_paper);
                }
            }
        }
        {
            /* with BW_GPU_AB (bw_ab.c), which half of the pair this frame is */
            const char *ab = getenv("BW_GPU_AB");
            fprintf(stderr, "[VFX] shot %s (%ux%u)%s%s%s\n", path, w, h,
                    ab && *ab ? " BW_GPU_AB " : "", ab && *ab ? ab : "",
                    ab && *ab ? (bw_ab_odd() ? " off (odd frame)" : " on (even frame)") : "");
        }
    }
    if (fp) fclose(fp);
    free(row);
    ID3D11DeviceContext_Unmap(g_vfx_ctx, (ID3D11Resource *)stg, 0);
}

/* One shot per capture window (RECOMP_FB_DUMP_FLIPS's file appears), or
 * every BW_VFX_SHOT_EVERY frames. Test aid (T1 for T2's shimmer test):
 * with BW_VFX_SHOT_GATE=1 as well, every BW_VFX_SHOT_EVERY frames only while
 * the RECOMP_FB_DUMP_FLIPS trigger file exists (consecutive native-res frames
 * of a capture window). Unset: as before. */
static int shot_due(void)
{
    static int was, gate = -1;
    const char *trig = getenv("RECOMP_FB_DUMP_FLIPS");
    int every = bw_vfx()->shot_every, now = 0, due;
    if (gate < 0) {
        const char *e = getenv("BW_VFX_SHOT_GATE");
        gate = e && e[0] == '1';
    }
    if (every > 0 && !(gate && trig && *trig))
        return (s_frame % (unsigned)every) == 0;
    if (trig && *trig) {
        FILE *f = fopen(trig, "rb");
        if (f) { fclose(f); now = 1; }
    }
    if (every > 0)                          /* gated: every n frames inside the window */
        return now && (s_frame % (unsigned)every) == 0;
    due = now && !was;
    was = now;
    return due;
}

/* Test aid: BW_VFX_SHOT_RECT=x,y,w,h writes only that crop of the picture
 * (output pixels from its top left). Unset or invalid: the whole picture. */
static void shot_rect(UINT dw, UINT dh, UINT *x, UINT *y, UINT *w, UINT *h)
{
    static int init;
    static unsigned rx, ry, rw, rh;
    if (!init) {
        const char *e = getenv("BW_VFX_SHOT_RECT");
        init = 1;
        if (!e || sscanf(e, "%u,%u,%u,%u", &rx, &ry, &rw, &rh) != 4)
            rw = rh = 0;
    }
    *x = 0; *y = 0; *w = dw; *h = dh;
    if (rw && rh && rx < dw && ry < dh) {
        *x = rx; *y = ry;
        *w = rx + rw > dw ? dw - rx : rw;
        *h = ry + rh > dh ? dh - ry : rh;
    }
}

/* ---- the frame -------------------------------------------------------------------- */

/* The off-screen output's format (no screen there: HDR only when asked for,
 * BW_VFX_HDR=on). The swap chain's format is bw_hdr.c's, in every edition
 * (0.2.5 item 97). */
DXGI_FORMAT bw_vfx_swap_format(void)
{
    const BwVfx *v = bw_vfx();
    return (v->active && v->hdr == 1) ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_B8G8R8A8_UNORM;
}

/* ---- HDR (0.2.5 items 45, 46 and 97; D-026) ------------------------------------
 * The screen, its data (EDID first), the swap chain and the standards'
 * formulas are bw_hdr.c's, shared with the plain present of every edition.
 * Here: what Remastered shows on a screen in HDR mode.
 *   Mode 1, HDR highlights: VFX_HDR auto (Remastered's default) with the
 *   screen's peak known from its EDID or calibration profile, or VFX_HDR on
 *   (with neither: VESA DisplayHDR 1000's defaults). The game's picture is
 *   decoded with BT.1886 and sits at BT.2408's reference white for the
 *   screen (Table 4); light the game adds above it rises through a soft knee
 *   from 0.9 of reference white (item 46) toward the screen's peak, the
 *   up-mapping BT.2408 leaves to the producer; BT.2408 Annex 5's EETF then
 *   gives the screen's black its standard lift (source range LB 0, LW = that
 *   peak, so the EETF's own knee doesn't engage), and the result is held to
 *   the peak. Needs half-float render targets, decided once at the start.
 *   Mode 3, BT.2408's direct mapping: VFX_HDR off, auto without a known
 *   peak, or a screen that entered HDR mode after the start: the SDR picture
 *   at reference white, BT.1886 with the screen's black, no expansion.
 *   Mode 2: an scRGB swap chain on a screen that has just left HDR mode, for
 *   the frame or two before bw_hdr.c makes the swap chain again: the SDR
 *   values written as linear scRGB (Windows shows them as SDR).
 *   Mode 0: a screen not in HDR mode, the SDR picture as before.
 * Undo: BW_VFX_HDR_CURVE=0 (0.2.4's curve), BW_VFX_HDR_EETF=0 (no EETF),
 * BW_VFX_HDR_EOTF=2.2 (0.2.4's 2.2 decode), BW_GPU_HDR=0 (no scRGB at all). */
static int s_hdr_mode_shown = -1;    /* the on-screen output mode last logged */

/* HDR highlights this session: decided once, before the first render
 * targets (half-float only then). */
int bw_vfx_hdr_highlights(void)
{
    static int v = -1;
    extern void *bw_fb_window(void);
    if (v < 0) {
        const BwVfx *x = bw_vfx();
        const char *why = "VFX_HDR off";
        v = 0;
        if (x->active && x->hdr == 1) {
            v = 1;
            bw_hdr_set_forced(1);
            why = bw_vfx()->offscreen ? "VFX_HDR on, off screen" : "VFX_HDR on";
        } else if (x->active && x->hdr == 2) {
            int h = bw_vfx()->offscreen ? 0 : bw_hdr_window_hdr((HWND)bw_fb_window());
            v = h == 1 && bw_hdr_have_data();
            why = bw_vfx()->offscreen ? "VFX_HDR auto, off screen (no screen to ask)"
                : h != 1 ? "VFX_HDR auto: the screen isn't in HDR mode"
                : v ? "VFX_HDR auto: the screen is in HDR mode and its peak is known"
                : "VFX_HDR auto: the screen is in HDR mode but neither its EDID nor a calibration profile"
                  " gives its peak (the direct mapping instead)";
        }
        if (x->active)
            fprintf(stderr, "[VFX] HDR highlights %s this session (%s)\n", v ? "on" : "off", why);
    }
    return v;
}

static int eetf_on(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("BW_VFX_HDR_EETF");
        v = !(e && e[0] == '0');
    }
    return v;
}

/* Item 46: where the highlight curve leaves the original, as a fraction of
 * paper white (linear). BW_VFX_HDR_KNEE=<0.5..0.95> (0.9); BW_VFX_HDR_CURVE=0
 * gives 0.2.4's curve (0 here). Read once, logged once.
 * 0.9, not the plan's 0.8 (call B3-3): the knee has to brighten what lies
 * just under paper white for the curve to bend smoothly, and at 0.8 a pixel
 * at paper white (white HUD and text) came out 13% brighter at an 800-nit
 * peak and 30% brighter at the 1345 nits the maintainer's screen reports; at 0.9, 4%
 * and 8%, with the same smoothness (curvature at the knee 8.4 against 8.8). */
static float hdr_knee(void)
{
    static float k = -1.0f;
    if (k < 0.0f) {
        const char *e = getenv("BW_VFX_HDR_CURVE"), *n = getenv("BW_VFX_HDR_KNEE");
        k = 0.9f;
        if (n && atof(n) > 0.0)
            k = (float)atof(n);
        if (k < 0.5f) k = 0.5f;
        if (k > 0.95f) k = 0.95f;
        if (e && e[0] == '0')
            k = 0.0f;
        if (k > 0.0f)
            fprintf(stderr, "[VFX] HDR highlights: a soft knee from %.2f of reference white (BW_VFX_HDR_KNEE),"
                            " unchanged below it; BW_VFX_HDR_CURVE=0 gives 0.2.4's curve\n", k);
        else
            fprintf(stderr, "[VFX] HDR highlights: 0.2.4's curve (BW_VFX_HDR_CURVE=0)\n");
    }
    return k;
}

/* The on-screen output mode, logged when it changes. */
static void hdr_mode_note(int mode)
{
    if (mode == s_hdr_mode_shown)
        return;
    s_hdr_mode_shown = mode;
    if (mode == 1)
        bw_hdr_log_mapping(eetf_on() ? "Remastered, HDR highlights (a soft knee to the peak, then BT.2408 Annex 5's"
                                       " EETF for the black, held to the peak)"
                                     : "Remastered, HDR highlights (a soft knee to the peak; BW_VFX_HDR_EETF=0: no EETF)");
    else if (mode == 3)
        bw_hdr_log_mapping("Remastered, the picture by BT.2408's direct mapping (no highlight expansion)");
    else if (mode == 2)
        fprintf(stderr, "[VFX] the screen left HDR mode: the SDR picture through scRGB until the swap chain"
                        " is made again\n");
}

void bw_vfx_swap_created(IDXGISwapChain1 *swap)
{
    (void)swap;
    s_hdr_mode_shown = -1;
}

int bw_vfx_present(ID3D11ShaderResourceView *src, UINT sw, UINT sh,
                   ID3D11RenderTargetView *dst, const D3D11_VIEWPORT *vp_in)
{
    const BwVfx *v = bw_vfx();
    BwPostK k;
    D3D11_VIEWPORT vp;
    ID3D11ShaderResourceView *in = src, *srv[5];
    ID3D11Resource *dst_res = NULL;
    DXGI_FORMAT work;
    DXGI_FORMAT outf = bw_vfx_swap_format();
    UINT dw, dh, i;
    static LARGE_INTEGER last;
    LARGE_INTEGER now, fq;
    float dt;
    int onscreen = dst && vp_in;
    int mode;       /* the output: 0 SDR, 1 HDR highlights, 2 SDR values in scRGB, 3 BT.2408 direct (scRGB) */

    if (!v->active || !src || !sw || !sh || !post_ready())
        return 0;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&fq);
    dt = last.QuadPart ? (float)((double)(now.QuadPart - last.QuadPart) / (double)fq.QuadPart) : 1.0f / 30.0f;
    if (dt > 0.25f) dt = 0.25f;
    last = now;
    s_frame++;
    memset(&k, 0, sizeof k);

    if (onscreen) {
        /* item 45: the swap chain's real format (made SDR when the screen
         * isn't in HDR mode, bw_vfx_swap_created) */
        D3D11_RENDER_TARGET_VIEW_DESC rd;
        vp = *vp_in;
        ID3D11RenderTargetView_GetDesc(dst, &rd);
        outf = rd.Format;
    } else {
        /* Off-screen: the output picture size, or the frame's own size. */
        vp.TopLeftX = 0; vp.TopLeftY = 0;
        vp.Width = (float)(v->out_w ? v->out_w : sw);
        vp.Height = (float)(v->out_h ? v->out_h : sh);
        if (!v->out_w) {
            /* the frame is a 4:3 picture whatever its pixel count (or the
             * widescreen picture's shape: D5, bw_vfx_set_picture_aspect) */
            vp.Width = vp.Height * bw_vfx_picture_aspect();
        }
        vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
        if (!rt_ensure(&s_off, (UINT)(vp.Width + 0.5f), (UINT)(vp.Height + 0.5f), outf))
            return 0;
        dst = s_off.rtv;
    }
    if (!dst)
        return 0;
    /* Item 44: the GPU timer starts only once the output target exists, and
     * nothing below returns before tq_end (a begun timer left open was a
     * debug-layer error and stale bw_vfx_post_ms). */
    tq_begin();
    dw = (UINT)(vp.Width + 0.5f);
    dh = (UINT)(vp.Height + 0.5f);
    /* Items 45 and 97: what this frame's output is (see the HDR section).
     * Off screen there is no screen: HDR highlights only when asked for. */
    mode = 0;
    if (outf == DXGI_FORMAT_R16G16B16A16_FLOAT) {
        if (!onscreen)
            mode = 1;
        else if (bw_hdr_scrgb())
            mode = bw_vfx_hdr_highlights() ? 1 : 3;
        else
            mode = 2;
    }
    if (onscreen)
        hdr_mode_note(mode);
    work = mode == 1 ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R16G16B16A16_UNORM;
    k.opt[0] = v->precision && (mode == 0 || mode == 2) ? 1.0f : 0.0f;
    k.opt[1] = (float)mode;
    k.hdrp[0] = v->hdr_paper;
    k.hdrp[1] = v->hdr_peak;
    s_shot_paper = v->hdr_paper;
    s_shot_gamma = 2.2f;
    if (mode == 1 || mode == 3) {
        /* item 97 (D-026): BT.2408 Table 4's reference white for the screen's
         * peak, BT.1886 (mode 1 without the black: the EETF lifts it; mode 3
         * with the screen's black), the EETF to the screen (bw_hdr.c) */
        static int logged_off;
        k.hdrp[0] = bw_hdr_paper(NULL);
        k.hdrp[1] = bw_hdr_peak(NULL);
        if (k.hdrp[1] < k.hdrp[0])
            k.hdrp[1] = k.hdrp[0];
        bw_hdr_direct_consts(k.eotf, mode == 3);
        if (mode == 1 && eetf_on())
            bw_hdr_eetf_consts(k.eetf);
        s_shot_paper = k.hdrp[0];
        s_shot_gamma = k.eotf[0];
        if (!onscreen && !logged_off++)
            bw_hdr_log_mapping("Remastered off screen, HDR highlights (no screen: the HDR1000 defaults unless set)");
    }
    k.hdrp[2] = v->sharpen; k.hdrp[3] = dt;
    {
        /* BW_VFX_DEBUG_SCENE=1 (test aid): show the scene copy (the 3D
         * before the HUD) instead of the frame. */
        static int dbg = -1;
        if (dbg < 0) {
            const char *e = getenv("BW_VFX_DEBUG_SCENE");
            dbg = e && e[0] == '1';
        }
        if (dbg && s_scene)
            in = s_scene;
    }

    if (v->fxaa && rt_ensure(&s_fx, sw, sh, work)) {
        pass(&s_fx, PS_FXAA, in, sw, sh, NULL, &k);
        in = s_fx.srv;
    }
    if (v->sharpen > 0.0f && rt_ensure(&s_sh, sw, sh, work)) {
        pass(&s_sh, PS_CAS, in, sw, sh, NULL, &k);
        in = s_sh.srv;
    }
    srv[1] = NULL;
    if (v->bloom > 0.0f) {
        UINT w = sw / 2, h = sh / 2, pw = sw, ph = sh;
        int ok = 1;
        for (i = 0; i < BLOOM_LEVELS && ok; i++, w /= 2, h /= 2)
            ok = rt_ensure(&s_bl[i], w, h, DXGI_FORMAT_R16G16B16A16_FLOAT)
              && rt_ensure(&s_bu[i], w, h, DXGI_FORMAT_R16G16B16A16_FLOAT);
        if (ok) {
            pass(&s_bl[0], PS_BRIGHT, s_scene ? s_scene : in, pw, ph, NULL, &k);
            for (i = 1; i < BLOOM_LEVELS; i++)
                pass(&s_bl[i], PS_DOWN, s_bl[i - 1].srv, s_bl[i - 1].w, s_bl[i - 1].h, NULL, &k);
            /* up: bu[n-1] = bl[n-1]; bu[i] = bl[i] + tent(bu[i+1]) */
            pass(&s_bu[BLOOM_LEVELS - 1], PS_DOWN, s_bl[BLOOM_LEVELS - 1].srv,
                 s_bl[BLOOM_LEVELS - 1].w, s_bl[BLOOM_LEVELS - 1].h, NULL, &k);
            for (i = BLOOM_LEVELS - 1; i-- > 0; )
                pass(&s_bu[i], PS_UP, s_bu[i + 1].srv, s_bu[i + 1].w, s_bu[i + 1].h, s_bl[i].srv, &k);
            srv[1] = s_bu[0].srv;
            k.opt[2] = v->bloom / (float)BLOOM_LEVELS;
        }
    }
    srv[2] = NULL;
    if (v->oled_dim > 0.0f) {
        UINT w = (sw + 15) / 16, h = (sh + 15) / 16;
        if (rt_ensure(&s_oled[0], w, h, DXGI_FORMAT_R16G16_FLOAT)
            && rt_ensure(&s_oled[1], w, h, DXGI_FORMAT_R16G16_FLOAT)) {
            BwRt *prev = &s_oled[s_oled_cur], *next = &s_oled[s_oled_cur ^ 1];
            pass(next, PS_OLED, in, sw, sh, prev->srv, &k);
            s_oled_cur ^= 1;
            srv[2] = next->srv;
            k.opt[3] = v->oled_dim;
        }
    }

    /* the final pass, into the picture rectangle */
    srv[0] = in;
    srv[3] = s_scene;
    srv[4] = s_glut;
    k.ext[1] = s_glut ? 1.0f : 0.0f;
    k.ext[0] = s_scene ? 1.0f : 0.0f;
    k.ext[2] = mode == 1 ? hdr_knee() : 0.0f;
    /* item 99: the P3 stretch, only on a screen in HDR mode (modes 1 and 3) */
    k.ext[3] = v->gamut && (mode == 1 || mode == 3) ? 1.0f : 0.0f;
    {
        static int told = -1;
        int now = v->gamut ? (mode == 1 || mode == 3 ? 2 : 1) : 0;
        if (now && now != told) {
            told = now;
            fprintf(stderr, now == 2
                ? "[VFX] colour: Wide (P3 stretch, BW_VFX_GAMUT=p3stretch): the game's BT.709 colours shown as P3-D65"
                  " (white and greys unchanged, more vivid reds, greens and blues)\n"
                : "[VFX] colour: Wide (P3 stretch) is set, but the screen isn't in HDR mode: the colours as made"
                  " (BT.709) here\n");
        }
    }
    sz(k.srcsz, sw, sh);
    sz(k.dstsz, dw, dh);
    k.org[0] = vp.TopLeftX; k.org[1] = vp.TopLeftY;
    k.org[2] = (sw == dw && sh == dh) ? 0.0f : (sw > dw || sh > dh) ? 1.0f : 2.0f;
    k.org[3] = (float)s_frame;
    bw_vfx_update_cb(s_pcb, &k, sizeof k);
    bw_vfx_fullscreen(dst, &vp, s_ps[PS_FINAL], s_pcb, srv, 5);
    tq_end();

    if (shot_due()) {
        ID3D11View_GetResource((ID3D11View *)dst, &dst_res);
        if (dst_res) {
            UINT cx, cy, cw, ch;
            shot_rect(dw, dh, &cx, &cy, &cw, &ch);
            shot_write(dst_res, (UINT)vp.TopLeftX + cx, (UINT)vp.TopLeftY + cy, cw, ch, outf, mode);
            {
                /* item 46/97 test aid, BW_VFX_SHOT_HDR=2 (off screen, HDR):
                 * the same frame again with 0.2.4's HDR mapping (its curve,
                 * a 2.2 decode, paper white 200 nits), as the next shot */
                const char *e = getenv("BW_VFX_SHOT_HDR");
                if (e && e[0] == '2' && !onscreen && mode == 1) {
                    k.ext[2] = 0.0f;
                    k.eotf[0] = 2.2f; k.eotf[1] = 1.0f; k.eotf[2] = 0.0f;
                    k.eetf[3] = 0.0f;               /* no EETF */
                    k.hdrp[0] = 200.0f;
                    s_shot_paper = 200.0f;
                    s_shot_gamma = 2.2f;
                    bw_vfx_update_cb(s_pcb, &k, sizeof k);
                    bw_vfx_fullscreen(dst, &vp, s_ps[PS_FINAL], s_pcb, srv, 5);
                    fprintf(stderr, "[VFX] shot: the next one is the same frame with 0.2.4's HDR mapping"
                                    " (its curve, a 2.2 decode, paper white 200 nits, peak %.0f)\n", k.hdrp[1]);
                    shot_write(dst_res, (UINT)vp.TopLeftX + cx, (UINT)vp.TopLeftY + cy, cw, ch, outf, mode);
                } else if (e && e[0] == '3' && !onscreen && mode == 1) {
                    /* item 99 test aid, BW_VFX_SHOT_HDR=3: the same frame again
                     * with the colour setting flipped (BT.709 / P3 stretch) */
                    k.ext[3] = k.ext[3] > 0.5f ? 0.0f : 1.0f;
                    bw_vfx_update_cb(s_pcb, &k, sizeof k);
                    bw_vfx_fullscreen(dst, &vp, s_ps[PS_FINAL], s_pcb, srv, 5);
                    fprintf(stderr, "[VFX] shot: the next one is the same frame with the colour %s\n",
                            k.ext[3] > 0.5f ? "stretched to P3" : "as made (BT.709)");
                    shot_write(dst_res, (UINT)vp.TopLeftX + cx, (UINT)vp.TopLeftY + cy, cw, ch, outf, mode);
                }
            }
            ID3D11Resource_Release(dst_res);
        }
    }
    {
        static int told;
        if (!told++)
            fprintf(stderr, "[VFX] first frame out: %ux%u from %ux%u (%s)%s%s%s%s\n", dw, dh, sw, sh,
                    k.org[2] == 0.0f ? "exact" : k.org[2] == 1.0f ? "box down" : "Catmull-Rom up",
                    v->fxaa ? ", FXAA" : "", v->sharpen > 0 ? ", CAS" : "",
                    v->bloom > 0 ? ", bloom" : "", v->oled_dim > 0 ? ", OLED care" : "");
    }
    return 1;
}


/* ==== ambient occlusion (VFX_AO) ============================================
 *
 * Per view, before its HUD (bw_d3d11.c calls this at the view's first overlay
 * draw): the target's depth buffer, a perspective z-buffer (the game sets
 * ZEnable 1, not a W-buffer; seen), is turned back into view depth with the
 * view's near and far planes; positions come from depth and the pixel
 * position; the normal from their screen derivatives. Sixteen samples on a
 * disc of about a metre around each pixel: what lies above the surface's
 * plane and within range occludes (normal-oriented, so flat ground and water
 * get none). Near range only, fading out by 100 m, where fog takes over.
 * Two depth-aware blur passes, then the target is multiplied by it (colour
 * only; alpha is kept). */
static const char s_ao_src[] =
"cbuffer A : register(b0) { float4 rect; float4 prm; float4 prm2; };\n"
/* rect: view x0 y0 x1 y1 (pixels); prm: zn, zf, focal (pixels), strength;
 * prm2: radius (m), fade start (m), blur dx, blur dy */
"#ifdef DMS\n"
"Texture2DMS<float> dep : register(t0);\n"
"float D(int2 p) { return dep.Load(p, 0); }\n"
"#else\n"
"Texture2D<float> dep : register(t0);\n"
"float D(int2 p) { return dep.Load(int3(p, 0)); }\n"
"#endif\n"
"Texture2D aot : register(t1);\n"
"struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; };\n"
"float lin(float d) { return prm.x * prm.y / (prm.y - d * (prm.y - prm.x)); }\n"
"float3 pos(float2 q, float z) { float2 c = 0.5 * (rect.xy + rect.zw); return float3((q - c) / prm.z * z, z); }\n"
"float ign(float2 p) { return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715)))); }\n"
"float4 ps_ao(V i) : SV_Target {\n"
"  int2 p = int2(i.p.xy); float d = D(p);\n"
"  float z = lin(d); float3 P = pos(i.p.xy, z);\n"
"  float3 n = normalize(cross(ddx(P), ddy(P))); if (dot(n, P) > 0.0) n = -n;\n"
"  if (d >= 0.99999 || z > prm2.y * 1.6) return 1.0;\n"
"  float r = clamp(prm2.x * prm.z / z, 3.0, 96.0);\n"
"  float a0 = ign(i.p.xy) * 6.2831853, occ = 0.0;\n"
"  int2 lo = int2(rect.xy), hi = int2(rect.zw) - 1;\n"
"  [unroll] for (int k = 0; k < 16; k++) {\n"
"    float a = a0 + k * 2.3999632; float t = sqrt((k + 0.5) / 16.0);\n"
"    float2 o = float2(cos(a), sin(a)) * r * t;\n"
"    int2 q = clamp(p + int2(o), lo, hi);\n"
"    float3 S = pos(float2(q) + 0.5, lin(D(q))) - P;\n"
"    float l = length(S) + 1e-4;\n"
"    float h = dot(n, S) / l - 0.15;\n"                       /* above the plane, with a bias */
"    float fall = saturate(1.0 - l / (prm2.x * 2.0));\n"
"    occ += max(h, 0.0) * fall; }\n"
"  float ao = 1.0 - prm.w * saturate(occ / 16.0 * 2.0);\n"
"  return lerp(ao, 1.0, saturate((z - prm2.y) / (prm2.y * 0.6))); }\n"
/* depth-aware blur along (dx, dy): 9 taps */
"float4 ps_aoblur(V i) : SV_Target {\n"
"  int2 p = int2(i.p.xy); int2 dir = int2(prm2.zw); float zc = lin(D(p));\n"
"  int2 lo = int2(rect.xy), hi = int2(rect.zw) - 1;\n"
"  float s = 0.0, w = 0.0;\n"
"  [unroll] for (int k = -4; k <= 4; k++) {\n"
"    int2 q = clamp(p + dir * k * 2, lo, hi);\n"
"    float wk = exp(-k * k / 8.0) * exp(-abs(lin(D(q)) - zc) / (0.05 * zc + 0.01));\n"
"    s += aot.Load(int3(q, 0)).r * wk; w += wk; }\n"
"  return s / max(w, 1e-4); }\n"
/* the multiply: colour x occlusion */
"float4 ps_aomul(V i) : SV_Target { float a = aot.Load(int3(int2(i.p.xy), 0)).r; return float4(a, a, a, 1.0); }\n";

static ID3D11PixelShader *s_ao_ps[2][3];    /* [multisampled depth][ao, blur, multiply] */
static ID3D11Buffer *s_ao_cb;
static ID3D11BlendState *s_ao_mul;
static BwRt s_ao_a, s_ao_b;
static int s_ao_ready = -1;

static int ao_ready(void)
{
    static const char *const names[3] = { "ps_ao", "ps_aoblur", "ps_aomul" };
    static char src[sizeof s_ao_src + 32];
    D3D11_BLEND_DESC bd;
    int ms, k;
    if (s_ao_ready >= 0)
        return s_ao_ready;
    s_ao_ready = 0;
    if (!g_vfx_dev)
        return 0;
    for (ms = 0; ms < 2; ms++) {
        size_t n = 0;
        if (ms) {
            memcpy(src, "#define DMS 1\n", 14);
            n = 14;
        }
        memcpy(src + n, s_ao_src, sizeof s_ao_src - 1);
        n += sizeof s_ao_src - 1;
        for (k = 0; k < 3; k++)
            if (!(s_ao_ps[ms][k] = bw_vfx_compile_ps(src, n, names[k])))
                return 0;
    }
    s_ao_cb = bw_vfx_make_cb(48);
    memset(&bd, 0, sizeof bd);
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ZERO;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_SRC_COLOR;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED
        | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
    ID3D11Device_CreateBlendState(g_vfx_dev, &bd, &s_ao_mul);
    s_ao_ready = s_ao_cb && s_ao_mul;
    return s_ao_ready;
}

void bw_vfx_ao_apply(ID3D11ShaderResourceView *depth, int depth_ms, UINT w, UINT h,
                     ID3D11RenderTargetView *target, const D3D11_RECT *rect,
                     float zn, float zf, float focal)
{
    const BwVfx *v = bw_vfx();
    float k[12];
    D3D11_VIEWPORT vp;
    ID3D11ShaderResourceView *srv[2];
    int ms = depth_ms ? 1 : 0;

    if (!v->active || v->ao <= 0.0f || !depth || !target || !rect || !ao_ready())
        return;
    if (rect->right <= rect->left || rect->bottom <= rect->top)
        return;
    if (!rt_ensure(&s_ao_a, w, h, DXGI_FORMAT_R8_UNORM) || !rt_ensure(&s_ao_b, w, h, DXGI_FORMAT_R8_UNORM))
        return;
    k[0] = (float)rect->left; k[1] = (float)rect->top; k[2] = (float)rect->right; k[3] = (float)rect->bottom;
    k[4] = zn; k[5] = zf; k[6] = focal; k[7] = v->ao;
    k[8] = 1.0f;                            /* radius, metres */
    k[9] = 60.0f;                           /* fade starts (metres), gone by about 100 */
    k[10] = 1.0f; k[11] = 0.0f;
    vp.TopLeftX = (float)rect->left; vp.TopLeftY = (float)rect->top;
    vp.Width = (float)(rect->right - rect->left); vp.Height = (float)(rect->bottom - rect->top);
    vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
    bw_vfx_update_cb(s_ao_cb, k, sizeof k);
    srv[0] = depth; srv[1] = NULL;
    bw_vfx_fullscreen_bs(s_ao_a.rtv, &vp, s_ao_ps[ms][0], s_ao_cb, srv, 1, NULL);
    srv[1] = s_ao_a.srv;
    bw_vfx_fullscreen_bs(s_ao_b.rtv, &vp, s_ao_ps[ms][1], s_ao_cb, srv, 2, NULL);
    k[10] = 0.0f; k[11] = 1.0f;
    bw_vfx_update_cb(s_ao_cb, k, sizeof k);
    srv[1] = s_ao_b.srv;
    bw_vfx_fullscreen_bs(s_ao_a.rtv, &vp, s_ao_ps[ms][1], s_ao_cb, srv, 2, NULL);
    srv[0] = NULL; srv[1] = s_ao_a.srv;
    bw_vfx_fullscreen_bs(target, &vp, s_ao_ps[ms][2], s_ao_cb, srv, 2, s_ao_mul);
}
