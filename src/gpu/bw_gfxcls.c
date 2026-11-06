/*
 * bw_gfxcls.c - region masks for the comparison tool: which class of draw
 * last changed each pixel (BW_GFXCAP_CLASSES; lane gfx-capef, see
 * bw_gfxcls.h). A test aid of the capture's replay; unset, g_bw_gfxcls stays
 * 0 and nothing here runs.
 *
 * Per colour target (keyed by its texture): two copies of it ("before" and
 * "after", same format and sample count) and a class map (R8_UINT, one byte a
 * pixel). begin() copies the target into "before"; after the draw it is
 * copied into "after" and a compute shader writes the draw's class wherever a
 * sample differs, bit for bit. Only compute state and resources of its own
 * are bound, so the back end's pipeline is untouched (the back end binds no
 * compute state). At a listed flip the shown target's map is read back and
 * written.
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bw_gfxcls.h"

extern ptrdiff_t g_xbox_mem_offset;
#define GUEST(va) ((const uint8_t *)((uintptr_t)(uint32_t)(va) + g_xbox_mem_offset))

int g_bw_gfxcls;

#define CLS_MAX_FLIPS 32
static uint32_t s_flips[CLS_MAX_FLIPS], s_nflips, s_lead = 2;
/* BW_GFXCLS_PROBE (test aid, the timing question in capef.md): 1 classes the
 * draws but does no GPU work; 2 copies the targets but dispatches nothing;
 * 3 the full tracker on single-sample targets only (MSAA targets left alone);
 * 4 no GPU work, but 50 us of CPU time spent at each draw (timing alone);
 * unset (0) the tracker as it is. */
static int s_probe;
static int s_sprites;           /* BW_GFXCLS_SPRITES=1 (gfx-sprites): point batches get their own class */

static int probe_skip(void *tex)
{
    D3D11_TEXTURE2D_DESC td;
    if (s_probe == 1)
        return 1;
    if (s_probe == 4) {
        LARGE_INTEGER a, b, f;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&a);
        do
            QueryPerformanceCounter(&b);
        while ((b.QuadPart - a.QuadPart) * 1000000 < 50 * f.QuadPart);
        return 1;
    }
    if (s_probe != 3 || !tex)
        return 0;
    ID3D11Texture2D_GetDesc((ID3D11Texture2D *)tex, &td);
    return td.SampleDesc.Count > 1;
}
static char s_out[MAX_PATH];
static void trackers_free(void);    /* below */

/* Draws are classed only in the frames from <lead> before each listed flip to
 * the flip (a gameplay target is cleared every frame, so its map is complete
 * by then), and the trackers' GPU copies are freed after each map. Seen: a
 * Remastered replay (m01_rem, MSAA 4x) tracked through 590 frames in a row
 * diverged from flip 1202 on (84 of 600 flips), while 11 frames of tracking
 * and none were exact; the cause is unknown (capef.md). */
void bw_gfxcls_init(const char *outdir)
{
    const char *e = getenv("BW_GFXCAP_CLASSES"), *f = getenv("BW_GFXCAP_CLASSES_LEAD");
    if (!e || !*e)
        return;
    while (*e && s_nflips < CLS_MAX_FLIPS) {
        uint32_t v = (uint32_t)strtoul(e, (char **)&e, 10);
        s_flips[s_nflips++] = v;
        if (*e != ',')
            break;
        e++;
    }
    if (!s_nflips)
        return;
    if (f && *f)
        s_lead = (uint32_t)strtoul(f, NULL, 10);
    if ((f = getenv("BW_GFXCLS_PROBE")) != NULL && *f)
        s_probe = atoi(f);
    if ((f = getenv("BW_GFXCLS_SPRITES")) != NULL && f[0] == '1')
        s_sprites = 1;                      /* gfx-sprites: point batches get their own class */
    snprintf(s_out, sizeof s_out, "%s", outdir ? outdir : ".");
    g_bw_gfxcls = 1;
    fprintf(stderr, "[GFXCLS] region masks: maps written at %u flip(s), draws classed from %u frame(s)"
                    " before each (BW_GFXCAP_CLASSES, BW_GFXCAP_CLASSES_LEAD)\n", s_nflips, s_lead);
}

static int on(uint32_t gen)
{
    uint32_t i;
    if (!g_bw_gfxcls)
        return 0;
    for (i = 0; i < s_nflips; i++)
        if (gen <= s_flips[i] && gen + s_lead >= s_flips[i])
            return 1;
    return 0;
}

/* ---- the classes ------------------------------------------------------------ */

/* bw_vfx_is_glyph_sheet's test (bw_vfx.c), without its edition gate: white
 * glyphs on transparency in a DXT1 sheet. */
static int near_white565(uint32_t c)
{
    return ((c >> 11) & 31) >= 27 && ((c >> 5) & 63) >= 54 && (c & 31) >= 27;
}

static int glyph_sheet(const BwClsTex *t)
{
    uint32_t bw = t->width / 4, bh = t->height / 4, n = bw * bh, i, empty = 0, inked = 0, other = 0;
    const uint8_t *p;
    if (t->color != 0x0C || t->surface || t->width < 128 || t->height < 64 || !n || n > (1u << 20))
        return 0;
    p = GUEST(t->offset);
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
    return other * 100 <= n && empty * 5 >= n && inked * 20 >= n;
}

/* The sea's vertex arrays this frame: its three passes draw each tile from
 * the same array (seen, mission 1: the lit pass 0x21081, the reflection
 * 0x62E21 and the foam 0x00021 read one array per tile), so a later draw of
 * an array the lit or reflection pass used is the foam. */
static uint32_t s_sea_gen, s_sea_lo, s_sea_hi;

int bw_gfxcls_classify(const BwClsDraw *d)
{
    int i;
    if (d->gen != s_sea_gen) {
        s_sea_gen = d->gen;
        s_sea_lo = 0xFFFFFFFFu;
        s_sea_hi = 0;
    }
    if (s_sprites && d->points)
        return BW_CLS_SPRITE;       /* gfx-sprites: the particles on their own */
    for (i = 0; i < 4; i++)         /* tr_is_movie's test, without the widescreen part */
        if (d->tex[i].valid && d->tex[i].color == 0x1E && d->tex[i].width == 640
                && d->tex[i].height == 480 && !d->tex[i].surface)
            return BW_CLS_MOVIE;
    if (d->overlay) {
        for (i = 0; i < 4; i++)
            if (d->tex[i].valid && glyph_sheet(&d->tex[i]))
                return BW_CLS_TEXT;
        return BW_CLS_HUD;
    }
    if (d->sea) {
        if (d->vtx && d->vtx < s_sea_lo) s_sea_lo = d->vtx;
        if (d->vtx && d->vtx > s_sea_hi) s_sea_hi = d->vtx;
        return BW_CLS_SEA;
    }
    if (d->vtx && d->depth_test && d->vtx >= s_sea_lo && d->vtx <= s_sea_hi)
        return BW_CLS_SEA;
    return d->depth_test ? BW_CLS_WORLD : BW_CLS_SKY;
}

/* ---- the trackers -------------------------------------------------------------- */

static const char s_cs[] =
"Texture2D<float4> A : register(t0);\n"
"Texture2D<float4> B : register(t1);\n"
"Texture2DMS<float4> AM : register(t2);\n"
"Texture2DMS<float4> BM : register(t3);\n"
"RWTexture2D<uint> M : register(u0);\n"
"cbuffer C : register(b0) { uint cls, w, h, samples; };\n"
"[numthreads(8, 8, 1)] void cs_one(uint3 id : SV_DispatchThreadID) {\n"
"  if (id.x >= w || id.y >= h) return;\n"
"  if (any(asuint(A.Load(int3(id.xy, 0))) != asuint(B.Load(int3(id.xy, 0))))) M[id.xy] = cls;\n"
"}\n"
"[numthreads(8, 8, 1)] void cs_ms(uint3 id : SV_DispatchThreadID) {\n"
"  if (id.x >= w || id.y >= h) return;\n"
"  bool d = false;\n"
"  for (uint s = 0; s < samples; s++)\n"
"    d = d || any(asuint(AM.Load(int2(id.xy), s)) != asuint(BM.Load(int2(id.xy), s)));\n"
"  if (d) M[id.xy] = cls;\n"
"}\n";

typedef struct {
    ID3D11Texture2D *key;           /* the target (not referenced: matched by pointer and size) */
    D3D11_TEXTURE2D_DESC td;
    ID3D11Texture2D *before, *after, *map, *stg;
    ID3D11ShaderResourceView *sb, *sa;
    ID3D11UnorderedAccessView *um;
    int bad;
} Tracker;

#define CLS_TRACKERS 24
static Tracker s_t[CLS_TRACKERS];
static int s_nt;
static ID3D11Device *s_dev;
static ID3D11ComputeShader *s_cs_one, *s_cs_ms;
static ID3D11Buffer *s_cb;
static int s_broken;
static unsigned s_draws, s_clears;

static void tracker_free(Tracker *t)
{
    if (t->sb) ID3D11ShaderResourceView_Release(t->sb);
    if (t->sa) ID3D11ShaderResourceView_Release(t->sa);
    if (t->um) ID3D11UnorderedAccessView_Release(t->um);
    if (t->before) ID3D11Texture2D_Release(t->before);
    if (t->after) ID3D11Texture2D_Release(t->after);
    if (t->map) ID3D11Texture2D_Release(t->map);
    if (t->stg) ID3D11Texture2D_Release(t->stg);
    memset(t, 0, sizeof *t);
}

static ID3D11ComputeShader *compile(const char *entry)
{
    ID3DBlob *b = NULL, *err = NULL;
    ID3D11ComputeShader *cs = NULL;
    if (FAILED(D3DCompile(s_cs, sizeof s_cs - 1, "bw_gfxcls", NULL, NULL, entry, "cs_5_0", 0, 0, &b, &err))) {
        fprintf(stderr, "[GFXCLS] compute shader %s failed: %s\n", entry,
                err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?");
        if (err) ID3D10Blob_Release(err);
        return NULL;
    }
    ID3D11Device_CreateComputeShader(s_dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b),
                                     NULL, &cs);
    ID3D10Blob_Release(b);
    return cs;
}

static int ensure(ID3D11DeviceContext *ctx)
{
    D3D11_BUFFER_DESC bd;
    if (s_broken)
        return 0;
    if (s_cs_one && s_cs_ms && s_cb)
        return 1;
    ID3D11DeviceContext_GetDevice(ctx, &s_dev);
    s_cs_one = compile("cs_one");
    s_cs_ms = compile("cs_ms");
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = 16;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (!s_dev || FAILED(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_cb)))
        s_cb = NULL;
    if (!s_cs_one || !s_cs_ms || !s_cb) {
        fprintf(stderr, "[GFXCLS] no compute shaders: region masks off\n");
        s_broken = 1;
        return 0;
    }
    return 1;
}

static Tracker *tracker(ID3D11DeviceContext *ctx, ID3D11Texture2D *tex)
{
    D3D11_TEXTURE2D_DESC td, cd;
    D3D11_SHADER_RESOURCE_VIEW_DESC sv;
    D3D11_UNORDERED_ACCESS_VIEW_DESC uv;
    Tracker *t = NULL;
    int i;
    if (!tex || !ensure(ctx))
        return NULL;
    ID3D11Texture2D_GetDesc(tex, &td);
    for (i = 0; i < s_nt; i++)
        if (s_t[i].key == tex) {
            t = &s_t[i];
            if (t->td.Width == td.Width && t->td.Height == td.Height && t->td.Format == td.Format
                    && t->td.SampleDesc.Count == td.SampleDesc.Count)
                return t->bad ? NULL : t;
            tracker_free(t);            /* a new target at the same address */
            break;
        }
    if (!t) {
        if (s_nt < CLS_TRACKERS)
            t = &s_t[s_nt++];
        else {
            static unsigned next;
            t = &s_t[next++ % CLS_TRACKERS];
            tracker_free(t);
        }
    }
    t->key = tex;
    t->td = td;
    cd = td;                            /* CopyResource wants the same layout: keep mips and array */
    cd.Usage = D3D11_USAGE_DEFAULT;
    cd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    cd.CPUAccessFlags = 0;
    cd.MiscFlags = 0;
    memset(&sv, 0, sizeof sv);
    sv.Format = td.Format;
    if (td.SampleDesc.Count > 1)
        sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
    else {
        sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D.MipLevels = 1;
    }
    if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &cd, NULL, &t->before))
            || FAILED(ID3D11Device_CreateTexture2D(s_dev, &cd, NULL, &t->after))
            || FAILED(ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)t->before, &sv, &t->sb))
            || FAILED(ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)t->after, &sv, &t->sa)))
        goto fail;
    memset(&cd, 0, sizeof cd);
    cd.Width = td.Width;
    cd.Height = td.Height;
    cd.MipLevels = 1;
    cd.ArraySize = 1;
    cd.Format = DXGI_FORMAT_R8_UINT;
    cd.SampleDesc.Count = 1;
    cd.Usage = D3D11_USAGE_DEFAULT;
    cd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    memset(&uv, 0, sizeof uv);
    uv.Format = DXGI_FORMAT_R8_UINT;
    uv.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
    if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &cd, NULL, &t->map))
            || FAILED(ID3D11Device_CreateUnorderedAccessView(s_dev, (ID3D11Resource *)t->map, &uv, &t->um)))
        goto fail;
    cd.Usage = D3D11_USAGE_STAGING;
    cd.BindFlags = 0;
    cd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &cd, NULL, &t->stg)))
        goto fail;
    {
        UINT zero[4] = { BW_CLS_HELD, BW_CLS_HELD, BW_CLS_HELD, BW_CLS_HELD };
        ID3D11DeviceContext_ClearUnorderedAccessViewUint(ctx, t->um, zero);
    }
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)t->before, (ID3D11Resource *)tex);
    return t;
fail:
    fprintf(stderr, "[GFXCLS] no tracker for a %ux%u target (format %u, %u samples): its pixels stay HELD\n",
            td.Width, td.Height, (unsigned)td.Format, td.SampleDesc.Count);
    tracker_free(t);
    t->key = tex;
    t->td = td;
    t->bad = 1;
    return NULL;
}

void bw_gfxcls_sync(void *ctx, void *tex, uint32_t gen)
{
    Tracker *t;
    if (!on(gen) || probe_skip(tex) || !(t = tracker((ID3D11DeviceContext *)ctx, (ID3D11Texture2D *)tex)))
        return;
    ID3D11DeviceContext_CopyResource((ID3D11DeviceContext *)ctx, (ID3D11Resource *)t->before,
                                     (ID3D11Resource *)tex);
}

void bw_gfxcls_draw(void *vctx, void *tex, uint32_t gen, int cls)
{
    ID3D11DeviceContext *ctx = (ID3D11DeviceContext *)vctx;
    ID3D11ShaderResourceView *srv[4] = { NULL, NULL, NULL, NULL };
    ID3D11UnorderedAccessView *nouav = NULL;
    D3D11_MAPPED_SUBRESOURCE m;
    Tracker *t;
    int ms;
    if (!on(gen) || probe_skip(tex) || !(t = tracker(ctx, (ID3D11Texture2D *)tex)))
        return;
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)t->after, (ID3D11Resource *)tex);
    if (s_probe == 2)
        return;
    if (FAILED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)s_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
        return;
    ((uint32_t *)m.pData)[0] = (uint32_t)cls;
    ((uint32_t *)m.pData)[1] = t->td.Width;
    ((uint32_t *)m.pData)[2] = t->td.Height;
    ((uint32_t *)m.pData)[3] = t->td.SampleDesc.Count;
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)s_cb, 0);
    ms = t->td.SampleDesc.Count > 1;
    srv[ms ? 2 : 0] = t->sa;
    srv[ms ? 3 : 1] = t->sb;
    ID3D11DeviceContext_CSSetShader(ctx, ms ? s_cs_ms : s_cs_one, NULL, 0);
    ID3D11DeviceContext_CSSetShaderResources(ctx, 0, 4, srv);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(ctx, 0, 1, &t->um, NULL);
    ID3D11DeviceContext_CSSetConstantBuffers(ctx, 0, 1, &s_cb);
    ID3D11DeviceContext_Dispatch(ctx, (t->td.Width + 7) / 8, (t->td.Height + 7) / 8, 1);
    memset(srv, 0, sizeof srv);
    ID3D11DeviceContext_CSSetShaderResources(ctx, 0, 4, srv);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(ctx, 0, 1, &nouav, NULL);
    ID3D11DeviceContext_CSSetShader(ctx, NULL, NULL, 0);
    /* the next draw's "before" is this draw's "after" */
    {
        ID3D11Texture2D *x = t->before;
        ID3D11ShaderResourceView *y = t->sb;
        t->before = t->after; t->sb = t->sa;
        t->after = x; t->sa = y;
    }
    s_draws++;
}

void bw_gfxcls_clear(void *vctx, void *tex, uint32_t gen)
{
    ID3D11DeviceContext *ctx = (ID3D11DeviceContext *)vctx;
    UINT v[4] = { BW_CLS_CLEAR, BW_CLS_CLEAR, BW_CLS_CLEAR, BW_CLS_CLEAR };
    Tracker *t;
    if (!on(gen) || probe_skip(tex) || !(t = tracker(ctx, (ID3D11Texture2D *)tex)))
        return;
    /* ClearRenderTargetView clears the whole view (the scissor doesn't apply) */
    ID3D11DeviceContext_ClearUnorderedAccessViewUint(ctx, t->um, v);
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)t->before, (ID3D11Resource *)tex);
    s_clears++;
}

void bw_gfxcls_flip(void *vctx, void *tex, uint32_t gen)
{
    ID3D11DeviceContext *ctx = (ID3D11DeviceContext *)vctx;
    D3D11_MAPPED_SUBRESOURCE m;
    char path[MAX_PATH + 64];
    unsigned counts[BW_CLS_N];
    uint32_t i, y, x, hd[4];
    Tracker *t = NULL;
    FILE *f;
    int want = 0;
    if (!g_bw_gfxcls)
        return;
    for (i = 0; i < s_nflips; i++)
        if (s_flips[i] == gen)
            want = 1;
    if (!want)
        return;
    for (i = 0; i < (uint32_t)s_nt; i++)
        if (s_t[i].key == (ID3D11Texture2D *)tex && !s_t[i].bad)
            t = &s_t[i];
    if (!t) {
        fprintf(stderr, "[GFXCLS] flip %u: the shown target was never tracked (no draws in the %u frame(s) before it)\n",
                gen, s_lead);
        return;
    }
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)t->stg, (ID3D11Resource *)t->map);
    if (FAILED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)t->stg, 0, D3D11_MAP_READ, 0, &m)))
        return;
    snprintf(path, sizeof path, "%s/cls_replay_f%05u.raw", s_out, gen);
    memset(counts, 0, sizeof counts);
    if ((f = fopen(path, "wb")) != NULL) {
        fwrite("BWCLS001", 1, 8, f);
        hd[0] = t->td.Width; hd[1] = t->td.Height; hd[2] = gen;
        hd[3] = s_sprites ? BW_CLS_N : BW_CLS_SPRITE;    /* gfx-sprites: unset, the header as before */
        fwrite(hd, 4, 4, f);
        for (y = 0; y < t->td.Height; y++) {
            const uint8_t *row = (const uint8_t *)m.pData + (size_t)y * m.RowPitch;
            fwrite(row, 1, t->td.Width, f);
            for (x = 0; x < t->td.Width; x++)
                counts[row[x] < BW_CLS_N ? row[x] : 0]++;
        }
        fclose(f);
    }
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)t->stg, 0);
    fprintf(stderr, "[GFXCLS] flip %u: %s (%ux%u; %u draws and %u clears classed so far); pixels held %u,"
                    " clear %u, world %u, sea %u, sky %u, hud %u, text %u, movie %u\n",
            gen, path, t->td.Width, t->td.Height, s_draws, s_clears, counts[0], counts[1], counts[2],
            counts[3], counts[4], counts[5], counts[6], counts[7]);
    fflush(stderr);
    if (!on(gen + 1))
        trackers_free();                /* nothing tracked until the next map's frames */
}

static void trackers_free(void)
{
    int i;
    for (i = 0; i < s_nt; i++)
        tracker_free(&s_t[i]);
    s_nt = 0;
}
