/*
 * bw_edgeprobe.c - the edge-pixel residual's probe (see bw_edgeprobe.h; lane
 * gfx-ideas). A test aid: unset, g_bw_edgeprobe becomes 0 at its first read
 * and every hook returns at once.
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

#include "bw_edgeprobe.h"

int g_bw_edgeprobe = -1;

static int s_points, s_churn;
static uint32_t s_from, s_to = 0xFFFFFFFFu;            /* copies only in these flips */
static int s_measure_all;
static uint32_t s_mfrom = 1, s_mto;                     /* measures in these flips */
static char s_out[MAX_PATH];
static unsigned long long s_copies;
static int s_dump;                                      /* BW_GPU_EDGEPROBE_DUMP=1: the resolved texture too */

static void range(const char *e, uint32_t *a, uint32_t *b)
{
    char *end;
    *a = (uint32_t)strtoul(e, &end, 10);
    *b = *end == '-' ? (uint32_t)strtoul(end + 1, NULL, 10) : *a;
}

int bw_edgeprobe_read(void)
{
    const char *e = getenv("BW_GPU_EDGEPROBE");
    char buf[256], *tok, *ctx = NULL;
    if (!e || !*e) {
        g_bw_edgeprobe = 0;
        return 0;
    }
    snprintf(buf, sizeof buf, "%s", e);
    for (tok = strtok_s(buf, ",", &ctx); tok; tok = strtok_s(NULL, ",", &ctx)) {
        if (!strcmp(tok, "begin")) s_points |= BW_EP_BEGIN;
        else if (!strcmp(tok, "draw")) s_points |= BW_EP_DRAW;
        else if (!strcmp(tok, "last")) s_points |= BW_EP_LAST;
        else if (!strcmp(tok, "post")) s_points |= BW_EP_POST;
        else if (!strcmp(tok, "end")) s_points |= BW_EP_END;
        else if (!strcmp(tok, "alloc")) s_points |= BW_EP_ALLOC;
        else if (!strcmp(tok, "churn")) s_churn = 1;
    }
    if ((e = getenv("BW_GPU_EDGEPROBE_FLIPS")) != NULL && *e)
        range(e, &s_from, &s_to);
    if ((e = getenv("BW_GPU_EDGEPROBE_MEASURE")) != NULL && *e) {
        if (!strcmp(e, "all"))
            s_measure_all = 1;
        else
            range(e, &s_mfrom, &s_mto);
    }
    s_dump = (e = getenv("BW_GPU_EDGEPROBE_DUMP")) != NULL && e[0] == '1';
    e = getenv("BW_GPU_EDGEPROBE_OUT");
    if (!e || !*e)
        e = getenv("BW_GFXCAP_OUT");
    snprintf(s_out, sizeof s_out, "%s", e && *e ? e : ".");
    fprintf(stderr, "[EDGEPROBE] copies of the scene target at:%s%s%s%s%s%s%s, flips %u-%u; measures %s"
                    " (BW_GPU_EDGEPROBE, a test aid)\n",
            s_points & BW_EP_BEGIN ? " begin" : "", s_points & BW_EP_DRAW ? " draw" : "",
            s_points & BW_EP_LAST ? " last" : "", s_points & BW_EP_POST ? " post" : "",
            s_points & BW_EP_END ? " end" : "", s_points & BW_EP_ALLOC ? " alloc" : "",
            s_churn ? " (churn)" : "", s_from, s_to,
            s_measure_all ? "every flip" : s_mto ? "in a window" : "none");
    g_bw_edgeprobe = 1;
    return 1;
}

static int in_copies(uint32_t gen)
{
    return gen >= s_from && gen <= s_to;
}

static int in_measure(uint32_t gen)
{
    return s_measure_all || (s_mto && gen >= s_mfrom && gen <= s_mto);
}

/* ---- the copies ------------------------------------------------------------ */
#define EP_SCRATCH 4
typedef struct {
    ID3D11Texture2D *t;
    D3D11_TEXTURE2D_DESC td;
} Scratch;
static Scratch s_sc[EP_SCRATCH];
static int s_nsc;
static ID3D11Device *s_dev;

static void scratch_free_all(void)
{
    int i;
    for (i = 0; i < s_nsc; i++)
        if (s_sc[i].t) ID3D11Texture2D_Release(s_sc[i].t);
    memset(s_sc, 0, sizeof s_sc);
    s_nsc = 0;
}

static ID3D11Texture2D *scratch_for(ID3D11Texture2D *src)
{
    D3D11_TEXTURE2D_DESC td, cd;
    int i;
    ID3D11Texture2D_GetDesc(src, &td);
    for (i = 0; i < s_nsc; i++)
        if (s_sc[i].td.Width == td.Width && s_sc[i].td.Height == td.Height && s_sc[i].td.Format == td.Format
                && s_sc[i].td.SampleDesc.Count == td.SampleDesc.Count && s_sc[i].td.MipLevels == td.MipLevels
                && s_sc[i].td.ArraySize == td.ArraySize)
            return s_sc[i].t;
    if (s_nsc == EP_SCRATCH)
        return NULL;
    cd = td;
    cd.Usage = D3D11_USAGE_DEFAULT;
    cd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    cd.CPUAccessFlags = 0;
    cd.MiscFlags = 0;
    if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &cd, NULL, &s_sc[s_nsc].t))) {
        fprintf(stderr, "[EDGEPROBE] no scratch copy for a %ux%u target (%u samples)\n",
                td.Width, td.Height, td.SampleDesc.Count);
        return NULL;
    }
    s_sc[s_nsc].td = td;
    return s_sc[s_nsc++].t;
}

void bw_edgeprobe_at(int point, void *vctx, void *vtex, uint32_t gen)
{
    ID3D11DeviceContext *ctx = (ID3D11DeviceContext *)vctx;
    ID3D11Texture2D *tex = (ID3D11Texture2D *)vtex, *sc;
    if (!(s_points & point) || !tex || !in_copies(gen))
        return;
    if (!s_dev)
        ID3D11DeviceContext_GetDevice(ctx, &s_dev);
    if (!(sc = scratch_for(tex)))
        return;
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)sc, (ID3D11Resource *)tex);
    s_copies++;
}

/* ---- the measure ----------------------------------------------------------- */
static const char s_cs[] =
"Texture2DMS<float4> tms : register(t0);\n"
"cbuffer C : register(b0) { uint w; uint h; uint ns; uint pad; };\n"
"RWByteAddressBuffer o : register(u0);\n"
"[numthreads(64, 1, 1)] void cs_rows(uint3 id : SV_DispatchThreadID) {\n"
"  uint y = id.x; if (y >= h) return;\n"
"  uint a = 2166136261u, b = 0x9E3779B9u;\n"
"  for (uint x = 0; x < w; x++) for (uint s = 0; s < ns; s++) {\n"
"    uint4 u = asuint(tms.Load(int2(x, y), s));\n"
"    a = (a ^ u.x) * 16777619u; a = (a ^ u.y) * 16777619u; a = (a ^ u.z) * 16777619u; a = (a ^ u.w) * 16777619u;\n"
"    b = (b ^ (u.x + 0x7F4A7C15u * x)) * 2654435761u; b ^= b >> 13; b = (b ^ u.y) * 2246822519u;\n"
"    b ^= (u.z << 7) ^ (u.w >> 3); }\n"
"  o.Store2(y * 8, uint2(a, b)); }\n";

static ID3D11ComputeShader *s_cs_rows;
static ID3D11Buffer *s_cb;
static ID3D11Texture2D *s_ms_copy;       /* the MSAA samples, readable */
static D3D11_TEXTURE2D_DESC s_ms_td;
static ID3D11ShaderResourceView *s_ms_srv;
static ID3D11Buffer *s_rows, *s_rows_stg;
static ID3D11UnorderedAccessView *s_rows_uav;
static UINT s_rows_cap;
static int s_broken;

static int cs_ready(void)
{
    ID3DBlob *b = NULL, *err = NULL;
    D3D11_BUFFER_DESC bd;
    if (s_broken)
        return 0;
    if (s_cs_rows)
        return 1;
    if (FAILED(D3DCompile(s_cs, sizeof s_cs - 1, "bw_edgeprobe", NULL, NULL, "cs_rows", "cs_5_0", 0, 0, &b, &err))) {
        fprintf(stderr, "[EDGEPROBE] its compute shader failed: %s\n",
                err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?");
        if (err) ID3D10Blob_Release(err);
        s_broken = 1;
        return 0;
    }
    ID3D11Device_CreateComputeShader(s_dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL,
                                     &s_cs_rows);
    ID3D10Blob_Release(b);
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = 16;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (!s_cs_rows || FAILED(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_cb))) {
        s_broken = 1;
        return 0;
    }
    return 1;
}

static int rows_buffers(UINT h)
{
    D3D11_BUFFER_DESC bd;
    D3D11_UNORDERED_ACCESS_VIEW_DESC uv;
    if (s_rows && s_rows_cap >= h)
        return 1;
    if (s_rows_uav) ID3D11UnorderedAccessView_Release(s_rows_uav);
    if (s_rows) ID3D11Buffer_Release(s_rows);
    if (s_rows_stg) ID3D11Buffer_Release(s_rows_stg);
    s_rows_uav = NULL; s_rows = NULL; s_rows_stg = NULL;
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = h * 8;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    if (FAILED(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_rows)))
        return 0;
    memset(&uv, 0, sizeof uv);
    uv.Format = DXGI_FORMAT_R32_TYPELESS;
    uv.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uv.Buffer.NumElements = h * 2;
    uv.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
    if (FAILED(ID3D11Device_CreateUnorderedAccessView(s_dev, (ID3D11Resource *)s_rows, &uv, &s_rows_uav)))
        return 0;
    bd.Usage = D3D11_USAGE_STAGING;
    bd.BindFlags = 0;
    bd.MiscFlags = 0;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_rows_stg)))
        return 0;
    s_rows_cap = h;
    return 1;
}

/* The MSAA samples' row hashes into rows[] (h of them); 0 if it couldn't. */
static int rows_ms(ID3D11DeviceContext *ctx, ID3D11Texture2D *mtex, uint64_t *rows, UINT *pw, UINT *ph, UINT *pn)
{
    D3D11_TEXTURE2D_DESC td;
    D3D11_MAPPED_SUBRESOURCE m;
    ID3D11UnorderedAccessView *nouav = NULL;
    ID3D11ShaderResourceView *nosrv = NULL;
    if (!cs_ready())
        return 0;
    ID3D11Texture2D_GetDesc(mtex, &td);
    if (!s_ms_copy || memcmp(&td, &s_ms_td, sizeof td)) {
        D3D11_TEXTURE2D_DESC cd = td;
        if (s_ms_srv) ID3D11ShaderResourceView_Release(s_ms_srv);
        if (s_ms_copy) ID3D11Texture2D_Release(s_ms_copy);
        s_ms_srv = NULL; s_ms_copy = NULL;
        cd.Usage = D3D11_USAGE_DEFAULT;
        cd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        cd.CPUAccessFlags = 0;
        cd.MiscFlags = 0;
        if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &cd, NULL, &s_ms_copy))
                || FAILED(ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)s_ms_copy, NULL, &s_ms_srv)))
            return 0;
        s_ms_td = td;
    }
    if (td.Height > 16384 || !rows_buffers(td.Height))
        return 0;
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)s_ms_copy, (ID3D11Resource *)mtex);
    if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)s_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        ((UINT *)m.pData)[0] = td.Width;
        ((UINT *)m.pData)[1] = td.Height;
        ((UINT *)m.pData)[2] = td.SampleDesc.Count;
        ((UINT *)m.pData)[3] = 0;
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)s_cb, 0);
    }
    ID3D11DeviceContext_CSSetShader(ctx, s_cs_rows, NULL, 0);
    ID3D11DeviceContext_CSSetShaderResources(ctx, 0, 1, &s_ms_srv);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(ctx, 0, 1, &s_rows_uav, NULL);
    ID3D11DeviceContext_CSSetConstantBuffers(ctx, 0, 1, &s_cb);
    ID3D11DeviceContext_Dispatch(ctx, (td.Height + 63) / 64, 1, 1);
    ID3D11DeviceContext_CSSetShaderResources(ctx, 0, 1, &nosrv);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(ctx, 0, 1, &nouav, NULL);
    ID3D11DeviceContext_CSSetShader(ctx, NULL, NULL, 0);
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)s_rows_stg, (ID3D11Resource *)s_rows);
    if (FAILED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)s_rows_stg, 0, D3D11_MAP_READ, 0, &m)))
        return 0;
    memcpy(rows, m.pData, (size_t)td.Height * 8);
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)s_rows_stg, 0);
    *pw = td.Width; *ph = td.Height; *pn = td.SampleDesc.Count;
    return 1;
}

static uint64_t fnv_row(const uint8_t *p, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    size_t i;
    for (i = 0; i < n; i++)
        h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

static UINT fmt_bytes(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_UNORM: return 8;
    case DXGI_FORMAT_R32G32B32A32_FLOAT: return 16;
    case DXGI_FORMAT_B5G6R5_UNORM: case DXGI_FORMAT_B5G5R5A1_UNORM: return 2;
    default: return 4;
    }
}

/* A single-sample texture's row hashes (its level 0 through a staging copy);
 * with dump set, its bytes too (dump: a file path). */
static int rows_one(ID3D11DeviceContext *ctx, ID3D11Texture2D *tex, uint64_t *rows, UINT *pw, UINT *ph,
                    const char *dump)
{
    D3D11_TEXTURE2D_DESC td, cd;
    D3D11_MAPPED_SUBRESOURCE m;
    ID3D11Texture2D *stg = NULL;
    UINT y, bpp;
    ID3D11Texture2D_GetDesc(tex, &td);
    if (td.SampleDesc.Count != 1 || td.Height > 16384)
        return 0;
    cd = td;
    cd.MipLevels = 1;
    cd.ArraySize = 1;
    cd.Usage = D3D11_USAGE_STAGING;
    cd.BindFlags = 0;
    cd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    cd.MiscFlags = 0;
    if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &cd, NULL, &stg)))
        return 0;
    ID3D11DeviceContext_CopySubresourceRegion(ctx, (ID3D11Resource *)stg, 0, 0, 0, 0, (ID3D11Resource *)tex, 0, NULL);
    if (FAILED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)stg, 0, D3D11_MAP_READ, 0, &m))) {
        ID3D11Texture2D_Release(stg);
        return 0;
    }
    bpp = fmt_bytes(td.Format);
    for (y = 0; y < td.Height; y++)
        rows[y] = fnv_row((const uint8_t *)m.pData + (size_t)y * m.RowPitch, (size_t)td.Width * bpp);
    if (dump) {
        FILE *f = fopen(dump, "wb");
        if (f) {
            uint32_t hd[4];
            hd[0] = td.Width; hd[1] = td.Height; hd[2] = bpp; hd[3] = (uint32_t)td.Format;
            fwrite("BWEDGEPX", 1, 8, f);
            fwrite(hd, 4, 4, f);
            for (y = 0; y < td.Height; y++)
                fwrite((const uint8_t *)m.pData + (size_t)y * m.RowPitch, 1, (size_t)td.Width * bpp, f);
            fclose(f);
        }
    }
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)stg, 0);
    ID3D11Texture2D_Release(stg);
    *pw = td.Width; *ph = td.Height;
    return 1;
}

static uint64_t fold(const uint64_t *rows, UINT n)
{
    uint64_t h = 1469598103934665603ull;
    UINT i;
    for (i = 0; i < n; i++)
        h = (h ^ rows[i]) * 0x100000001B3ull;
    return h;
}

static uint64_t s_rbuf[3][16384];

void bw_edgeprobe_flip_end(void *vctx, void *vmtex, void *vtex, void *vrb, uint32_t gen)
{
    ID3D11DeviceContext *ctx = (ID3D11DeviceContext *)vctx;
    UINT w[3] = { 0, 0, 0 }, h[3] = { 0, 0, 0 }, ns = 0;
    int ok[3] = { 0, 0, 0 }, k;
    if (!g_bw_edgeprobe)
        return;
    if (!s_dev && ctx)
        ID3D11DeviceContext_GetDevice(ctx, &s_dev);
    if ((s_points & BW_EP_ALLOC) && vtex && in_copies(gen) && s_dev) {   /* memory churn alone */
        D3D11_TEXTURE2D_DESC td;
        ID3D11Texture2D *t = NULL;
        ID3D11Texture2D_GetDesc((ID3D11Texture2D *)(vmtex ? vmtex : vtex), &td);
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = 0;
        td.MiscFlags = 0;
        if (SUCCEEDED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &t)))
            ID3D11Texture2D_Release(t);
    }
    if (in_measure(gen) && s_dev) {
        if (vmtex)
            ok[0] = rows_ms(ctx, (ID3D11Texture2D *)vmtex, s_rbuf[0], &w[0], &h[0], &ns);
        if (vtex) {
            char dp[MAX_PATH + 64];
            snprintf(dp, sizeof dp, "%s/edge_f%05u_resolved.px", s_out, gen);
            ok[1] = rows_one(ctx, (ID3D11Texture2D *)vtex, s_rbuf[1], &w[1], &h[1], s_dump ? dp : NULL);
        }
        if (vrb)
            ok[2] = rows_one(ctx, (ID3D11Texture2D *)vrb, s_rbuf[2], &w[2], &h[2], NULL);
        {
            char path[MAX_PATH + 64];
            FILE *f;
            snprintf(path, sizeof path, "%s/edge_f%05u.rows", s_out, gen);
            if ((f = fopen(path, "wb")) != NULL) {
                fwrite("BWEDGE01", 1, 8, f);
                for (k = 0; k < 3; k++) {
                    uint32_t hd[4];
                    hd[0] = (uint32_t)ok[k]; hd[1] = w[k]; hd[2] = ok[k] ? h[k] : 0; hd[3] = k ? 1 : ns;
                    fwrite(hd, 4, 4, f);
                    if (ok[k])
                        fwrite(s_rbuf[k], 8, h[k], f);
                }
                fclose(f);
            }
        }
        fprintf(stderr, "[EDGEPROBE] flip %u: samples %s %016llx (%ux%u x%u), resolved %s %016llx (%ux%u),"
                        " readback %s %016llx (%ux%u); copies so far %llu\n", gen,
                ok[0] ? "" : "-", ok[0] ? (unsigned long long)fold(s_rbuf[0], h[0]) : 0ull, w[0], h[0], ns,
                ok[1] ? "" : "-", ok[1] ? (unsigned long long)fold(s_rbuf[1], h[1]) : 0ull, w[1], h[1],
                ok[2] ? "" : "-", ok[2] ? (unsigned long long)fold(s_rbuf[2], h[2]) : 0ull, w[2], h[2],
                s_copies);
    }
    if (s_churn)
        scratch_free_all();
}
