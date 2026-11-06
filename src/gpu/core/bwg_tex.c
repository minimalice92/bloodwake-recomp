/*
 * bwg_tex.c - the renderer's API-neutral core: the texture cache policy
 * (bwg_tex.h).
 *
 * Lifted verbatim from bw_d3d11.c (lane gfx-core, gate GC: the same
 * decisions in the same order). An upload's bytes and format are decided
 * here (BwgUpload); the back end's BwgTexOps make and release the API
 * objects when this file says so.
 */
#include <windows.h>
#include <dxgiformat.h>                 /* DXGI_FORMAT: the upload formats (D3D11's and D3D12's) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bwg_core.h"
#include "bwg_state.h"                  /* bwg_use_mips */
#include "bwg_tex.h"
#include "bwg_latch.h"               /* I1, the CPU-write latch (lane gfx-regen) */
#include "bwg_ideas.h"                  /* I5: BW_GPU_FEEDBACK (lane gfx-ideas) */
#include "d3d/d3d8_swizzle.h"           /* xbox_unswizzle_rect, the D3D8 format tables */
#include "../bw_vfx.h"                  /* the suite's settings (VFX, D9; inactive in Original) */
#include "../bw_gfxcap.h"               /* BW_GFXCAP: the cache's reads of guest memory */
#include "../../bw_widescreen.h"        /* BW_WS_WHAT_MOVIE */

static BwgTex s_tex[BWG_MAX_TEX];
static int s_ntex;
static uint32_t *s_decode;
static size_t s_decode_cap;
unsigned bwg_st_uploads, bwg_st_rtt;
/* T1: the cache entry get_texture_ last returned (NULL for a render target
 * or a failure); read only by the texture restoration (Remastered). */
BwgTex *bwg_tex_last;
static const BwgTexOps *s_ops;

void bwg_tex_set_ops(const BwgTexOps *ops)
{
    s_ops = ops;
}

int bwg_tex_count(void)
{
    return s_ntex;
}

static BwgTexRef ref_none(void)
{
    BwgTexRef r = { NULL, NULL };
    return r;
}

static BwgTexRef ref_e(BwgTex *e)
{
    BwgTexRef r = { e, NULL };
    return r;
}

static BwgTexRef ref_rt(BwgSurf *rt)
{
    BwgTexRef r = { NULL, rt };
    return r;
}

/* The view the draw would sample (the entry's or the surface's), as the
 * back end's get_texture returned it: NULL means none. */
static int ref_view(BwgTexRef r)
{
    return r.rt ? r.rt->srv != NULL : r.e ? r.e->srv != NULL : 0;
}

int bwg_tex_ref_view(BwgTexRef r)
{
    return ref_view(r);
}

static BwgTexRef get_texture_(const Nv2aTexture *t, char *kind);
static BwgTexRef get_cube_texture_(const Nv2aTexture *t, char *kind);

/* Bits per texel of the NV097 colour formats this title is likely to use; 0
 * for DXT (handled by block size). Unknown codes hash as 32 bpp. */
static uint32_t tex_bpp(uint32_t color, int *dxt)
{
    *dxt = 0;
    switch (color) {
    case 0x00: case 0x01: case 0x0B: case 0x13: case 0x19: case 0x1B: case 0x1F:
        return 8;
    case 0x02: case 0x03: case 0x04: case 0x05: case 0x10: case 0x11: case 0x1A:
    case 0x1C: case 0x1D: case 0x20: case 0x27: case 0x28: case 0x29: case 0x30:
        return 16;
    case 0x0C: *dxt = 8; return 0;        /* DXT1: 8 bytes per 4x4 */
    case 0x0E: case 0x0F: *dxt = 16; return 0;
    default:
        return 32;
    }
}

static uint64_t hash_bytes(const uint8_t *p, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t v;
        memcpy(&v, p + i, 8);
        h = (h ^ v) * 0x100000001B3ull;
        h ^= h >> 29;
    }
    for (; i < n; i++)
        h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

static size_t tex_bytes(const Nv2aTexture *t)
{
    int dxt;
    uint32_t bpp = tex_bpp(t->color, &dxt);
    size_t n;
    if (t->pitch)
        n = (size_t)t->pitch * t->height;
    else if (dxt)
        n = (size_t)((t->width + 3) / 4) * ((t->height + 3) / 4) * dxt;
    else
        n = (size_t)t->width * t->height * bpp / 8;
    return n > (64u << 20) ? 0 : n;
}

/* BW_GFXCAP (lane gfx-cap): the span the cache can read for a texture:
 * level 0 and, for a mipped one, room for every level after it (a superset;
 * DXT's smallest levels are a whole block each). */
static uint32_t gfxcap_tex_span(const Nv2aTexture *t)
{
    size_t n = tex_bytes(t);
    if (n && !t->pitch && t->levels > 1)
        n += n / 3 + 64u * t->levels;
    return (uint32_t)n;
}

static uint64_t tex_hash(const Nv2aTexture *t)
{
    size_t n = tex_bytes(t);
    if (!n)
        return 0;
    return hash_bytes(BWG_GUEST(t->offset), n) ^ ((uint64_t)t->color << 56);
}

/* 128 eight-byte samples spread over the texture: what a texture that has
 * not changed for a while is checked with each frame, instead of hashing all
 * of it. A full check still runs every 32 frames. */
static uint64_t tex_sparse(const Nv2aTexture *t)
{
    size_t n = tex_bytes(t), i;
    const uint8_t *p = BWG_GUEST(t->offset);
    uint64_t h = 1469598103934665603ull;
    if (n < 8)
        return 0;
    for (i = 0; i < 128; i++) {
        uint64_t v;
        size_t off = ((n - 8) * i / 127) & ~(size_t)7;
        memcpy(&v, p + off, 8);
        h = (h ^ v) * 0x100000001B3ull;
        h ^= h >> 29;
    }
    return h;
}

/* How many levels to upload: what the texture says it carries, down to 1x1,
 * at most 12. */
uint32_t bwg_mip_count(const Nv2aTexture *t)
{
    uint32_t n = 1, w = t->width, h = t->height;
    uint32_t want = (bwg_use_mips() && !t->pitch && t->levels > 1) ? t->levels : 1;
    while (n < want && n < 12 && (w > 1 || h > 1)) {
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
        n++;
    }
    return n;
}

/* VFX (V1, VFX_HUD_EDGES): is a DXT3/DXT5 texture's alpha hard-edged:
 * opaque shapes with only a thin anti-aliased rim (the radar's compass ring,
 * weapon and bar icons), as opposed to glows and translucent art (meters, the
 * radar lens). Decided from level 0's alpha blocks: at least 3% of the texels
 * opaque (alpha >= 215) and at most half as many partial ones (41..214).
 * Checked on the 53 DXT5 overlays: 23 qualify (docs/day1/V1.md). */
static int tex_hard_alpha(uint32_t color, const uint8_t *q, uint32_t w, uint32_t h)
{
    uint32_t nb, i, k, opaque = 0, partial = 0, total;
    if ((color != 0x0E && color != 0x0F) || !q || w < 4 || h < 4 || w > 512 || h > 512)
        return 0;
    nb = ((w + 3) / 4) * ((h + 3) / 4);
    total = nb * 16;
    for (i = 0; i < nb; i++, q += 16) {
        uint8_t pal[8];
        uint64_t bits = 0;
        for (k = 0; k < 16; k++) {
            uint32_t a;
            if (color == 0x0E) {                    /* DXT3: explicit 4-bit alpha */
                a = (q[k >> 1] >> ((k & 1) * 4)) & 0xF;
                a = a * 17;
            } else {                                /* DXT5: interpolated alpha */
                if (k == 0) {
                    uint32_t a0 = q[0], a1 = q[1], j;
                    pal[0] = (uint8_t)a0; pal[1] = (uint8_t)a1;
                    if (a0 > a1)
                        for (j = 1; j < 7; j++) pal[j + 1] = (uint8_t)(((7 - j) * a0 + j * a1) / 7);
                    else {
                        for (j = 1; j < 5; j++) pal[j + 1] = (uint8_t)(((5 - j) * a0 + j * a1) / 5);
                        pal[6] = 0; pal[7] = 255;
                    }
                    for (j = 0; j < 6; j++) bits |= (uint64_t)q[2 + j] << (8 * j);
                }
                a = pal[(bits >> (3 * k)) & 7];
            }
            if (a >= 215) opaque++;
            else if (a > 40) partial++;
        }
    }
    return opaque * 32 >= total && partial * 2 <= opaque;
}

/* Upload without the per-texel decoder where the layout allows: DXT blocks
 * go to the GPU as they are (BC1-3 are the same S3TC blocks), the common
 * swizzled and linear 32-bit and R5G6B5 formats are unswizzled or copied in
 * bulk. Swizzled and DXT textures keep their mip levels after level 0, each
 * laid out like a texture of its own size; those are uploaded too.
 * Fills init[], td->format and td->levels; returns 0 to fall back to
 * nv2a_backend_decode_texture (level 0 only). */
static int fast_texture(const Nv2aTexture *t, BwgUpload *td, BwgMip *init)
{
    uint32_t w = t->width, h = t->height, x, y, lv, levels = bwg_mip_count(t);
    const uint8_t *src = BWG_GUEST(t->offset);
    uint8_t *dst = (uint8_t *)s_decode;
    switch (t->color) {
    case 0x0C: case 0x0E: case 0x0F: {              /* DXT1, DXT3, DXT5 */
        uint32_t bb = t->color == 0x0C ? 8 : 16;
        if ((w & 3) || (h & 3))
            return 0;
        td->format = t->color == 0x0C ? DXGI_FORMAT_BC1_UNORM
                   : t->color == 0x0E ? DXGI_FORMAT_BC2_UNORM : DXGI_FORMAT_BC3_UNORM;
        for (lv = 0; lv < levels; lv++) {
            uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
            init[lv].data = src;
            init[lv].pitch = bw * bb;
            src += (size_t)bw * bh * bb;
            w = w > 1 ? w / 2 : 1;
            h = h > 1 ? h / 2 : 1;
        }
        td->levels = levels;
        return 1;
    }
    case 0x06: case 0x07: case 0x05: {             /* A8R8G8B8, X8R8G8B8, R5G6B5 (swizzled) */
        uint32_t bpp = t->color == 0x05 ? 2 : 4;
        if ((w & (w - 1)) || (h & (h - 1)))
            return 0;
        for (lv = 0; lv < levels; lv++) {
            xbox_unswizzle_rect(dst, src, w, h, bpp);
            if (t->color == 0x07)
                for (x = 0; x < w * h; x++)
                    ((uint32_t *)dst)[x] |= 0xFF000000u;
            init[lv].data = dst;
            init[lv].pitch = w * bpp;
            src += (size_t)w * h * bpp;
            dst += (size_t)w * h * bpp;
            w = w > 1 ? w / 2 : 1;
            h = h > 1 ? h / 2 : 1;
        }
        td->format = bpp == 2 ? DXGI_FORMAT_B5G6R5_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
        td->levels = levels;
        return 1;
    }
    case 0x12: case 0x1E:                           /* LIN_A8R8G8B8, LIN_X8R8G8B8 */
        if (!t->pitch || t->pitch < w * 4)
            return 0;
        for (y = 0; y < h; y++) {
            memcpy(s_decode + (size_t)y * w, src + (size_t)y * t->pitch, w * 4);
            if (t->color == 0x1E)
                for (x = 0; x < w; x++)
                    s_decode[(size_t)y * w + x] |= 0xFF000000u;
        }
        td->format = DXGI_FORMAT_B8G8R8A8_UNORM;
        init[0].data = s_decode;
        init[0].pitch = w * 4;
        td->levels = 1;
        return 1;
    default:
        return 0;
    }
}

/* BW_GPU_BLACKWATCH=<prefix>: what each frame sampled, to explain a frame
 * that suddenly goes black (see bw_flip). Kinds: R a surface this back end
 * drew (render to texture), F the current target (feedback, read from guest
 * memory), U uploaded this frame, H cached (full hash equal), S cached
 * (sampled check), G cached (already checked this frame). */
BwgTexUse bwg_tex_use[2][BWG_MAX_USE];
int bwg_tex_nuse[2];
int bwg_tex_watch = -1;

static void note_use(const Nv2aTexture *t, char kind)
{
    BwgTexUse *u = bwg_tex_use[bwg_gen & 1];
    int *n = &bwg_tex_nuse[bwg_gen & 1], i;
    if (bwg_tex_watch <= 0)
        return;
    for (i = 0; i < *n; i++)
        if (u[i].offset == t->offset && u[i].kind == kind)
            return;
    if (*n < BWG_MAX_USE) {
        u[*n].offset = t->offset; u[*n].w = t->width; u[*n].h = t->height;
        u[*n].color = t->color; u[*n].kind = kind;
        u[*n].zero_pct = -1;
        if (kind == 'U') {
            /* how much of a fresh upload is zero bytes (a texture caught
             * mid-rewrite?), sampled at 256 points of level 0 */
            size_t nb = tex_bytes(t), k, z = 0;
            const uint8_t *q = BWG_GUEST(t->offset);
            if (nb >= 256) {
                for (k = 0; k < 256; k++)
                    z += q[(nb - 1) * k / 255] == 0;
                u[*n].zero_pct = (int)(z * 100 / 256);
            }
        }
        (*n)++;
    }
}

/* 0.2.5 item 43: the cache entry with this texture's identity (address,
 * size, format, pitch; 2D or cube), or NULL. It was a scan of every entry
 * for every stage of every draw (about 125 in mission 1); now a
 * direct-mapped index of the last hit for each identity is tried first. An
 * index slot is only a hint, checked against the entry it names, so a slot
 * left by an entry since released or refilled just misses (and the scan
 * runs, as before). There are never two entries with one identity: a new
 * entry is made only when the scan finds none. */
#define TEX_IDX 1024
static int16_t s_tex_idx[TEX_IDX];      /* entry + 1; 0: none */

static int tex_is(const BwgTex *c, const Nv2aTexture *t, int cube)
{
    return c->offset == t->offset && c->w == t->width && c->h == t->height
        && c->color == t->color && c->pitch == t->pitch && c->cube == (uint8_t)cube;
}

static BwgTex *tex_find(const Nv2aTexture *t, int cube)
{
    uint32_t h = (t->offset >> 7) ^ (t->offset >> 19) ^ (t->width * 31u) ^ (t->height * 131u)
               ^ (t->color << 5) ^ (t->pitch * 7u) ^ (uint32_t)cube;
    int k;
    h &= TEX_IDX - 1;
    k = s_tex_idx[h];
    if (k && bwg_memo_on() && k <= s_ntex && tex_is(&s_tex[k - 1], t, cube))
        return &s_tex[k - 1];
    for (int i = 0; i < s_ntex; i++)
        if (tex_is(&s_tex[i], t, cube)) {
            s_tex_idx[h] = (int16_t)(i + 1);
            return &s_tex[i];
        }
    return NULL;
}

/* A cache entry to (re)fill: `e` itself, a free one, or the least recently
 * used, with its API objects released (the back end's). */
static BwgTex *tex_slot(BwgTex *e)
{
    if (!e) {
        if (s_ntex == BWG_MAX_TEX) {
            int lru = 0;
            for (int i = 1; i < s_ntex; i++)
                if (s_tex[i].last_use < s_tex[lru].last_use) lru = i;
            e = &s_tex[lru];
        } else {
            e = &s_tex[s_ntex++];
            memset(e, 0, sizeof(*e));
        }
    }
    s_ops->release(e);                      /* its objects; T1: and its restored chain's */
    /* 0.2.5 item 36: and no identity until the caller fills it, so a
     * texture that can't be decoded or made isn't found again with no view
     * (it drew blank, for good) but is tried again when next used. A
     * filled entry sets every one of these. */
    e->offset = 0; e->w = 0; e->h = 0; e->pitch = 0; e->color = 0;
    e->hash = 0; e->sparse = 0; e->gen = 0; e->stable = 0; e->last_use = 0; e->cube = 0;
    return e;
}

/* Swizzled and DXT textures have no rows; CONTROL1's pitch can be stale
 * (see get_texture). */
const Nv2aTexture *bwg_drop_stale_pitch(const Nv2aTexture *t, Nv2aTexture *fixed)
{
    static int stale = -1;
    if (stale < 0) {
        const char *e = getenv("BW_GPU_STALEPITCH");
        stale = e && *e == '1';
    }
    if (t && t->pitch && !stale
            && (d3d8_format_is_swizzled(t->color) || d3d8_format_dxt_block_bytes(t->color))) {
        *fixed = *t;
        fixed->pitch = 0;
        return fixed;
    }
    return t;
}

BwgTexRef bwg_get_texture(const Nv2aTexture *t)
{
    char kind = '?';
    BwgTexRef v;
    Nv2aTexture fixed;
    /* Only linear formats have rows. CONTROL1's pitch stays set from earlier
     * linear work while swizzled and DXT textures are bound (seen 2026-10-26,
     * agent R2: DXT uploads with pitch 5120), and a stale pitch made the cache
     * hash pitch x height (2.6 MB for a 512x512 DXT5), so the texture was
     * re-created about every frame, and made bwg_mip_count() upload level 0 only.
     * BW_GPU_STALEPITCH=1 restores the old behaviour for comparison. */
    t = bwg_drop_stale_pitch(t, &fixed);
    v = get_texture_(t, &kind);
    if (t)
        note_use(t, ref_view(v) ? kind : 'X');
    return v;
}

static BwgTexRef get_texture_(const Nv2aTexture *t, char *kind)
{
    BwgTex *e = NULL;
    BwgSurf *rt;
    uint64_t h;
    BwgUpload td;
    BwgMip *init = td.mip;
    size_t need;

    bwg_tex_last = NULL;
    if (!t || !t->width || !t->height || t->width > 4096 || t->height > 4096)
        return ref_none();
    /* Render to texture: a surface this back end drew. */
    rt = bwg_find_surf(t->offset);
    /* I5 (BW_GPU_FEEDBACK, lane gfx-ideas; unset: nothing here runs): a
     * stage sampling the target this draw renders into is counted, and with
     * "copy" it samples the back end's copy of the target as it is now */
    if (bwg_feedback_mode() && bwg_fb_stage(t, gfxcap_tex_span(t)) && rt && rt == bwg_cur
            && bwg_feedback_mode() == BWG_FB_COPY) {
        BwgSurf *cp = bwg_fb_copy(rt, s_ops->feedback);
        if (cp) {
            *kind = 'F';
            return ref_rt(cp);
        }
    }
    if (rt && rt != bwg_cur) {
        /* I1 (lane gfx-regen): a CPU write into it, up before it is sampled,
         * where that sets no pipeline state (scale 1, no MSAA: Original);
         * otherwise at its next clear, draw or flip */
        if (!bw_vfx_active() && !rt->mtex && rt->pw == rt->w && rt->ph == rt->h)
            BWG_LATCH_USE(rt);
        s_ops->surf_resolve(rt);            /* VFX (D9): no-op in Original */
        rt->used_gen = bwg_gen;               /* item 37: sampled counts as used */
        bwg_st_rtt++;
        *kind = 'R';
        return ref_rt(rt);
    }
    *kind = rt ? 'F' : 'G';
    e = tex_find(t, 0);                     /* item 43: indexed */
    if (e && e->gen == bwg_gen) {
        e->last_use = bwg_gen;
        bwg_tex_last = e;
        return ref_e(e);
    }
    /* BW_GFXCAP: from here on the cache looks at guest memory (the checks,
     * the upload and its mips, the suite's glyph and alpha tests) */
    if (BW_GFXCAP_REC())
        bw_gfxcap_read(t->offset, gfxcap_tex_span(t));
    /* A texture unchanged for 30 frames: a sampled check, with a full one
     * every 32 frames (staggered by address). */
    if (e && e->stable >= 30 && ((bwg_gen + (e->offset >> 12)) & 31) != 0
            && tex_sparse(t) == e->sparse) {
        *kind = 'S';
        e->gen = bwg_gen;
        e->last_use = bwg_gen;
        bwg_tex_last = e;
        return ref_e(e);
    }
    h = tex_hash(t);
    *kind = 'H';
    if (e && e->hash == h && h) {
        if (e->stable < 0xFFFFFFFFu)
            e->stable++;
        if (e->stable == 30)
            e->sparse = tex_sparse(t);
        e->gen = bwg_gen;
        e->last_use = bwg_gen;
        bwg_tex_last = e;
        return ref_e(e);
    }
    {
        int same = e != NULL;               /* T1: re-created with new contents, or a new entry */
        e = tex_slot(e);
        if (!same) {
            e->tr_churn = 0;
            e->tr_gen = 0;
        }
        e->tr_bound = 0;
    }
    e->cube = 0;
    need = (size_t)t->width * t->height * 2;    /* level 0, and room for its mips */
    if (need > s_decode_cap) {
        free(s_decode);
        s_decode = (uint32_t *)malloc(need * 4);
        s_decode_cap = s_decode ? need : 0;
        if (!s_decode)
            return ref_none();
    }
    memset(&td, 0, sizeof(td));
    td.w = t->width; td.h = t->height; td.levels = 1; td.faces = 1;
    td.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    if (!fast_texture(t, &td, init)) {
        td.format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.levels = 1;
        init[0].data = s_decode;
        init[0].pitch = t->width * 4;
        if (!nv2a_backend_decode_texture(t, s_decode))
            return ref_none();
    }
    if (!s_ops->create(e, t, &td))          /* the back end's texture and view (item 36: logged) */
        return ref_none();
    /* T1 (VFX_TEX_RESTORE): the menu and title photos (BW_TEXRESTORE_MENUS:
     * the game decodes each JPEG into a 640x480 LIN_A8R8G8B8 screen texture,
     * docs/remaster/texture_matching.md section 3) and the Bink frames
     * (BW_TEXRESTORE_MOVIES, item 11) get the 8x8 deblock on the GPU as they
     * go up; the entry then holds the deblocked copy. */
    e->jpeg = 0;
    e->movie = 0;
    if (bw_vfx_active() && e->srv && t->width == 640 && t->height == 480) {
        int kind = (bw_vfx()->tr_menus && t->color == 0x12 && td.levels == 1) ? 1
                 : (bw_vfx()->tr_movies && bwg_tr_is_movie(t)) ? 2 : 0;
        if (kind) {
            /* the back end deblocks on the GPU into a new texture and view in
             * place of these, then binds its targets again */
            if (s_ops->deblock(e, t, kind)) {
                e->jpeg = kind == 1;
                e->movie = kind == 2;
            }
        }
    }
    e->offset = t->offset; e->w = t->width; e->h = t->height;
    e->color = t->color; e->pitch = t->pitch;
    e->hash = h; e->gen = bwg_gen; e->last_use = bwg_gen;
    e->stable = 0; e->sparse = 0;
    /* VFX (D9): is it one of the game's glyph sheets (text) */
    e->glyph = (uint8_t)(t->color == 0x0C && bw_vfx_is_glyph_sheet(BWG_GUEST(t->offset), t->width, t->height));
    e->alpha1 = 0;
    if (bw_vfx_active() && bw_vfx()->text) {
        if (t->color == 0x02 || t->color == 0x10)               /* A1R5G5B5, swizzled or linear */
            e->alpha1 = 1;
        else if (t->color == 0x0C) {                            /* DXT1: any 3-colour block with index 3 */
            const uint8_t *q = BWG_GUEST(t->offset);
            uint32_t nb = ((t->width + 3) / 4) * ((t->height + 3) / 4), i;
            for (i = 0; i < nb && !e->alpha1; i++, q += 8) {
                uint32_t c0 = q[0] | (q[1] << 8), c1 = q[2] | (q[3] << 8);
                uint32_t ix = q[4] | (q[5] << 8) | (q[6] << 16) | ((uint32_t)q[7] << 24), k;
                if (c0 <= c1)
                    for (k = 0; k < 16; k++)
                        if (((ix >> (2 * k)) & 3) == 3) { e->alpha1 = 1; break; }
            }
        }
    }
    /* VFX (V1, VFX_HUD_EDGES): hard-edged alpha (only overlay draws use it) */
    e->hard = (uint8_t)(bw_vfx_active() && bw_vfx()->hud_edges
                        && tex_hard_alpha(t->color, BWG_GUEST(t->offset), t->width, t->height));
    if (e->hard) {
        static int told_hard;
        if (told_hard++ < 48)
            fprintf(stderr, "[VFX] hard-edged alpha %08X %ux%u (HUD edges contour when drawn as an overlay)\n",
                    t->offset, t->width, t->height);
    }
    if (e->glyph) {
        static int told;
        if (told++ < 24)
            fprintf(stderr, "[VFX] glyph sheet %08X %ux%u: text drawn with the contour filter\n",
                    t->offset, t->width, t->height);
    }
    bwg_st_uploads++;
    *kind = 'U';
    bwg_tex_last = e;
    /* BW_GPU_TEXLOG=<file>: one line per upload with a plain FNV-1a 64 of the
     * level-0 bytes as the game hands them to the GPU, so a script can match
     * them against the texture files (Remastered texture packs). Test aid. */
    {
        static int texlog_init;
        static FILE *texlog;
        if (!texlog_init) {
            const char *p = getenv("BW_GPU_TEXLOG");
            texlog_init = 1;
            if (p && *p)
                texlog = fopen(p, "a");
        }
        if (texlog) {
            /* Level 0 as stored: DXT blocks, or width x height x bpp (the
             * pitch field is set even for DXT here, so it isn't used). For
             * non-DXT textures with a pitch, a second hash covers pitch x
             * height (linear images). */
            int dxt;
            uint32_t bpp = tex_bpp(t->color, &dxt);
            size_t n = dxt ? (size_t)((t->width + 3) / 4) * ((t->height + 3) / 4) * dxt
                           : (size_t)t->width * t->height * bpp / 8;
            size_t nl = (!dxt && t->pitch) ? (size_t)t->pitch * t->height : 0, i;
            const uint8_t *q = BWG_GUEST(t->offset);
            uint64_t f = 1469598103934665603ull, fl = 1469598103934665603ull;
            if (n > (64u << 20)) n = 0;
            if (nl > (64u << 20)) nl = 0;
            for (i = 0; i < n; i++)
                f = (f ^ q[i]) * 1099511628211ull;
            for (i = 0; i < nl; i++)
                fl = (fl ^ q[i]) * 1099511628211ull;
            fprintf(texlog, "T %u color 0x%02X %ux%u %s%u pitch %u l0bytes %zu fnv1a64 %016llX linbytes %zu linfnv %016llX at %08X\n",
                    (unsigned)bwg_gen, (unsigned)t->color, (unsigned)t->width, (unsigned)t->height,
                    dxt ? "dxtblock " : "bpp ", dxt ? (unsigned)dxt : bpp, (unsigned)t->pitch,
                    n, (unsigned long long)f, nl, (unsigned long long)(nl ? fl : 0), (unsigned)t->offset);
            fflush(texlog);
        }
    }
    return ref_e(e);
}

/* ---- cube maps ------------------------------------------------------------ */

/* BW_GPU_CUBE=0: sample a cube map's first face as a 2D texture, as this back
 * end did before 2026-10-27 (reflections came out wrong). */
static int use_cube(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("BW_GPU_CUBE");
        v = !(e && e[0] == '0');
    }
    return v;
}

/* BW_GPU_DOTMODES=0: the dot-product reflection stages read zero, as before
 * 2026-10-27 (and as the CPU path still does). */
static int use_dotmodes(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("BW_GPU_DOTMODES");
        v = !(e && e[0] == '0');
    }
    return v;
}

int bwg_dot_program(uint32_t prog)
{
    int st;
    for (st = 1; st < 4; st++) {
        uint32_t m = (prog >> (st * 5)) & 0x1F;
        if (m == 0x11 || m == 0x0B || m == 0x0C)
            return 1;
    }
    return 0;
}

/* Does stage `st` of this program sample a cube map: CUBEMAP, or the
 * dot-product reflections (diffuse at stage 2, specular at stage 3). */
int bwg_cube_stage(uint32_t prog, int st)
{
    uint32_t m = (prog >> (st * 5)) & 0x1F;
    if (m == 3)
        return use_cube();
    if ((m == 0x0B && st == 2) || (m == 0x0C && st == 3))
        return use_cube() && use_dotmodes();
    return 0;
}

/* Bytes between cube faces, as the CPU path (nv2a_pb_exec.c tex_face_stride,
 * after xemu): one face with every level the texture says it stores, rounded
 * up to the NV2A's 128-byte face alignment. */
static uint32_t cube_face_stride(const Nv2aTexture *t)
{
    uint32_t w = t->width, h = t->height, lv, total = 0;
    uint32_t levels = t->levels ? t->levels : 1, bpp;
    int dxt;
    if (t->pitch)
        return (t->pitch * t->height + 127u) & ~127u;
    bpp = tex_bpp(t->color, &dxt);
    for (lv = 0; lv < levels; lv++) {
        uint32_t lw = (w >> lv) ? (w >> lv) : 1, lh = (h >> lv) ? (h >> lv) : 1;
        total += dxt ? ((lw + 3) / 4) * ((lh + 3) / 4) * (uint32_t)dxt : lw * lh * bpp / 8;
    }
    return (total + 127u) & ~127u;
}

static uint32_t *s_cube_decode;
static size_t s_cube_cap;       /* uint32s */

BwgTexRef bwg_get_cube_texture(const Nv2aTexture *t)
{
    char kind = '?';
    BwgTexRef v;
    Nv2aTexture fixed;
    t = bwg_drop_stale_pitch(t, &fixed);
    v = get_cube_texture_(t, &kind);
    if (t)
        note_use(t, ref_view(v) ? kind : 'X');
    return v;
}

/* Six faces, one after another at cube_face_stride, each laid out like a 2D
 * texture of its own (with its mips): uploaded as one TEXTURECUBE resource.
 * The face order (+X, -X, +Y, -Y, +Z, -Z) and the direction-to-face mapping
 * are D3D's, which the CPU path's rc_stage_fetch follows too. */

static BwgTexRef get_cube_texture_(const Nv2aTexture *t, char *kind)
{
    BwgTex *e = NULL;
    uint32_t stride, f, lv, levels = 1;
    size_t face_texels;
    uint64_t h;
    BwgUpload td;
    BwgMip *init = td.mip;
    uint32_t *keep;
    size_t keep_cap;
    int fast = 1, ok = 1, i;

    if (!t || !t->width || t->width != t->height || t->width > 2048)
        return ref_none();
    stride = cube_face_stride(t);
    if (!stride || (size_t)stride * 6 > (64u << 20))
        return ref_none();
    if (bwg_feedback_mode())                /* I5: counted only (a cube is never the target) */
        bwg_fb_stage(t, stride * 6);
    *kind = 'G';
    e = tex_find(t, 1);                     /* item 43: indexed */
    if (e && e->gen == bwg_gen) {
        e->last_use = bwg_gen;
        return ref_e(e);
    }
    /* Cube maps are few and small here: a full hash of all six faces once a
     * frame, no sampled check. */
    if (BW_GFXCAP_REC())                /* BW_GFXCAP: the six faces */
        bw_gfxcap_read(t->offset, stride * 6);
    h = hash_bytes(BWG_GUEST(t->offset), (size_t)stride * 6) ^ ((uint64_t)t->color << 56) ^ 0xC0BEull;
    *kind = 'H';
    if (e && e->hash == h) {
        e->gen = bwg_gen;
        e->last_use = bwg_gen;
        return ref_e(e);
    }
    e = tex_slot(e);
    face_texels = (size_t)t->width * t->height * 2;  /* level 0 and room for its mips */
    if (face_texels * 6 > s_cube_cap) {
        free(s_cube_decode);
        s_cube_decode = (uint32_t *)malloc(face_texels * 6 * 4);
        s_cube_cap = s_cube_decode ? face_texels * 6 : 0;
        if (!s_cube_decode)
            return ref_none();
    }
    memset(&td, 0, sizeof(td));
    keep = s_decode;
    keep_cap = s_decode_cap;
    for (f = 0; f < 6 && ok; f++) {
        Nv2aTexture ft = *t;
        BwgUpload fd;
        BwgMip *fi = fd.mip;
        ft.offset = t->offset + f * stride;
        memset(&fd, 0, sizeof(fd));
        fd.levels = 1;
        fd.format = DXGI_FORMAT_B8G8R8A8_UNORM;
        s_decode = s_cube_decode + f * face_texels;   /* fast_texture's scratch */
        s_decode_cap = face_texels;
        if (fast && fast_texture(&ft, &fd, fi)) {
            if (f == 0) {
                levels = fd.levels;
                td.format = fd.format;
            } else if (fd.levels != levels || fd.format != td.format) {
                ok = 0;
                break;
            }
            for (lv = 0; lv < levels; lv++)
                init[f * levels + lv] = fi[lv];
        } else {
            if (f > 0 && fast) {        /* the format decides; it can't differ by face */
                ok = 0;
                break;
            }
            fast = 0;
            levels = 1;
            td.format = DXGI_FORMAT_B8G8R8A8_UNORM;
            if (!nv2a_backend_decode_texture(&ft, s_decode)) {
                ok = 0;
                break;
            }
            init[f].data = s_decode;
            init[f].pitch = t->width * 4;
        }
    }
    s_decode = keep;
    s_decode_cap = keep_cap;
    if (!ok)
        return ref_none();
    td.w = t->width; td.h = t->height; td.levels = levels; td.faces = 6;
    if (!s_ops->create(e, t, &td))          /* the back end's TextureCube and its view (item 36: logged) */
        return ref_none();
    e->offset = t->offset; e->w = t->width; e->h = t->height;
    e->color = t->color; e->pitch = t->pitch; e->cube = 1;
    e->hash = h; e->gen = bwg_gen; e->last_use = bwg_gen;
    e->stable = 0; e->sparse = 0;
    bwg_st_uploads++;
    *kind = 'U';
    /* BW_GPU_TEXLOG: one "C" line per face (same hash as the T lines), for
     * texture packs that replace cube maps. */
    {
        const char *p = getenv("BW_GPU_TEXLOG");
        FILE *fp = (p && *p) ? fopen(p, "a") : NULL;
        if (fp) {
            int dxt;
            uint32_t bpp = tex_bpp(t->color, &dxt);
            size_t n = dxt ? (size_t)((t->width + 3) / 4) * ((t->height + 3) / 4) * dxt
                           : (size_t)t->width * t->height * bpp / 8, k;
            for (f = 0; f < 6; f++) {
                const uint8_t *q = BWG_GUEST(t->offset + f * stride);
                uint64_t fh = 1469598103934665603ull;
                for (k = 0; k < n; k++)
                    fh = (fh ^ q[k]) * 1099511628211ull;
                fprintf(fp, "C %u color 0x%02X %ux%u face %u stride %u levels %u l0bytes %zu fnv1a64 %016llX at %08X\n",
                        (unsigned)bwg_gen, (unsigned)t->color, (unsigned)t->width, (unsigned)t->height,
                        (unsigned)f, (unsigned)stride, (unsigned)levels, n,
                        (unsigned long long)fh, (unsigned)(t->offset + f * stride));
            }
            fclose(fp);
        }
    }
    return ref_e(e);
}

/* VFX (D9): the cached texture's flags (set at upload): 1 glyph sheet,
 * 2 1-bit alpha in use, 4 hard-edged alpha (V1). */
int bwg_tex_vfx_flags(const Nv2aTexture *t)
{
    for (int i = 0; i < s_ntex; i++)
        if (s_tex[i].offset == t->offset && s_tex[i].w == t->width && s_tex[i].h == t->height
                && s_tex[i].color == t->color && !s_tex[i].cube)
            return (s_tex[i].glyph ? 1 : 0) | (s_tex[i].alpha1 ? 2 : 0) | (s_tex[i].hard ? 4 : 0)
                 | (s_tex[i].jpeg ? 8 : 0);
    return 0;
}

/* T1 (item 11): is this the Bink movie texture: Movie_CreateTexture's
 * LIN_X8R8G8B8 640x480 (seen, T3: one per movie, rewritten every decode;
 * docs/remaster/texture_matching.md: the only 0x1E 640x480 uploads), from
 * guest memory (not a surface this back end drew), and, when the widescreen
 * markers run, inside Movie_DrawQuad's (BW_WS_WHAT_MOVIE). */
int bwg_tr_is_movie(const Nv2aTexture *t)
{
    if (t->color != 0x1E || t->width != 640 || t->height != 480 || bwg_find_surf(t->offset))
        return 0;
    if (bwg_ws_k > 1.0f)
        return bwg_ws_2d > 0 && bwg_ws_what == BW_WS_WHAT_MOVIE;
    return 1;
}

BwgTex *bwg_tex_at(int i)
{
    return i >= 0 && i < s_ntex ? &s_tex[i] : NULL;
}

void bwg_tex_drop_all(void)
{
    for (int i = 0; i < s_ntex; i++)
        tex_slot(&s_tex[i]);
    s_ntex = 0;
    memset(s_tex_idx, 0, sizeof s_tex_idx);
    bwg_tex_last = NULL;
}
