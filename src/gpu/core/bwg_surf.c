/*
 * bwg_surf.c - the renderer's API-neutral core: the guest surfaces and depth
 * buffers by address (bwg_surf.h).
 *
 * Lifted verbatim from bw_d3d11.c (lane gfx-core, gate GC: the same
 * decisions in the same order). The back end's BwgSurfOps make, release and
 * forget the API objects when this file says so.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bwg_core.h"
#include "bwg_surf.h"
#include "../bw_vfx.h"                  /* the render scale, MSAA and their epoch (VFX, D9; scale 1 in Original) */
#include "../bw_gfxcap.h"               /* BW_GFXCAP: the seed is a GPU read of guest memory */

static BwgSurf s_surf[BWG_MAX_SURF];
static int s_nsurf;
static BwgDepth s_depth[BWG_MAX_SURF];
static int s_ndepth;
BwgSurf *bwg_cur;               /* bound colour surface */
BwgDepth *bwg_cur_depth;
unsigned bwg_surf_reused, bwg_depth_reused;     /* item 37: slots reused (surf_slot, depth_slot) */
static const BwgSurfOps *s_ops;

void bwg_surf_set_ops(const BwgSurfOps *ops)
{
    s_ops = ops;
}

int bwg_surf_count(void)
{
    return s_nsurf;
}

BwgSurf *bwg_surf_at(int i)
{
    return i >= 0 && i < s_nsurf ? &s_surf[i] : NULL;
}

BwgSurf *bwg_surf_slot_at(int k)
{
    if (k < 0 || k >= BWG_MAX_SURF)
        return NULL;
    while (s_nsurf <= k)
        memset(&s_surf[s_nsurf++], 0, sizeof(s_surf[0]));
    return &s_surf[k];
}

/* 0.2.5 item 38: the guest memory a new surface is seeded from. The seed
 * reads h rows at the surface's pitch from va; where that would run past
 * the end of the guest block at va (the front buffer, 1200 KB, bound at the
 * AA surface's 5120-byte pitch asks for 2.4 MB), the rows that fit are
 * copied and the rest left black, as readback clamps its writes. *tmp is
 * the copy to free (NULL when guest memory is used as it is). NULL: no
 * seed (no memory for the copy). */
const void *bwg_seed_src(const BwgSurf *s, void **tmp)
{
    uint32_t room = bwg_block_size(s->va);
    uint64_t need = (uint64_t)(s->h ? s->h - 1 : 0) * s->pitch + (uint64_t)s->w * s->bpp;
    uint8_t *c;
    *tmp = NULL;
    if (BW_GFXCAP_REC())                /* the seed is a GPU read of guest memory */
        bw_gfxcap_read(s->va, (uint32_t)(room && need > room ? room : need));
    if (!room || need <= room)
        return BWG_GUEST(s->va);
    c = (uint8_t *)calloc((size_t)s->pitch, s->h);     /* pitch x h >= need > room */
    if (!c)
        return NULL;
    memcpy(c, BWG_GUEST(s->va), room);
    {
        static unsigned n;
        if (n++ < 4)
            fprintf(stderr, "[GPU-D3D11] seed of surface %08X clamped: %u rows at pitch %u would read"
                            " past the end of its %u-byte guest block; the rest starts black\n",
                    s->va, s->h, s->pitch, room);
    }
    *tmp = c;
    return c;
}

BwgSurf *bwg_find_surf(uint32_t va)
{
    for (int i = 0; i < s_nsurf; i++)
        if (s_surf[i].va == va && s_surf[i].tex)
            return &s_surf[i];
    return NULL;
}

/* ---- the output size: one place -----------------------------------------
 * D9's bw_vfx_surface_size gives a guest surface's physical size at the
 * render scale (the guest's own size in Original). In a widescreen boot
 * (Remastered, bw_widescreen.h) the device's full-screen targets, the
 * 640x480 front buffer and the 2x-wide anti-aliased back buffer (1280x480),
 * are then made k times wider; their depth buffers follow (get_depth takes
 * pw, ph). Shadow maps, reflections and every other target keep their size.
 * Positions stay in guest units (the shaders' surf constant is w, h) and the
 * viewport covers pw, ph, so 3D stretches over the wider surface; 2D is
 * pillarboxed by the wsx remap (ws_remap). The scissor scales by pw/w. */
static int ws_is_device_target(uint32_t w, uint32_t h)
{
    return h == 480 && (w == 640 || w == 1280);
}

static void surf_physical_size(BwgSurf *s, const Nv2aSurface *in)
{
    bw_vfx_surface_size(s->w, s->h, in->aa_sx, in->aa_sy, &s->pw, &s->ph);   /* VFX (D9) */
    s->wsk = 1.0f;
    if (bwg_ws_k > 1.0f && ws_is_device_target(s->w, s->h)) {
        uint32_t base = s->pw;
        s->pw = ((uint32_t)((float)base * bwg_ws_k + 1.0f)) & ~1u;
        if (s->pw > BWG_MAX_TEXTURE_DIM)      /* item 36: D3D's limit, 16384 */
            s->pw = BWG_MAX_TEXTURE_DIM;
        s->wsk = (float)s->pw / (float)base;
    }
}

/* 0.2.5 item 37: the slot tables (BWG_MAX_SURF colour surfaces and as many
 * depth buffers) used to fill for good, and every draw into a new target
 * was dropped from then on, silently. Now a new target takes a free slot,
 * else the one used longest ago (never the bound one), released; the first
 * time is logged, later ones are counted in the BW_GPU_STATS line. A
 * reused colour surface's guest address seeds it again if the game comes
 * back to it, as at its first use. */

/* Test aid, BW_GPU_SLOTS=<n> (2..32): both tables hold only n, so a short
 * run exercises the reuse. Unset: BWG_MAX_SURF, as always. */
static int slot_cap(void)
{
    static int cap;
    if (!cap) {
        const char *e = getenv("BW_GPU_SLOTS");
        int n = e ? atoi(e) : 0;
        cap = (n >= 2 && n < BWG_MAX_SURF) ? n : BWG_MAX_SURF;
        if (cap != BWG_MAX_SURF)
            fprintf(stderr, "[GPU-D3D11] test aid: %d colour surface and depth buffer slots"
                            " (BW_GPU_SLOTS)\n", cap);
    }
    return cap;
}

static BwgSurf *surf_slot(uint32_t want_va)
{
    BwgSurf *old = NULL;
    if (s_nsurf < slot_cap())
        return &s_surf[s_nsurf++];
    for (int i = 0; i < s_nsurf; i++)
        if (!s_surf[i].tex && &s_surf[i] != bwg_cur)
            return &s_surf[i];              /* empty: one item 36 left to retry */
    for (int i = 0; i < s_nsurf; i++) {
        BwgSurf *c = &s_surf[i];
        if (c == bwg_cur)
            continue;
        if (!old || c->used_gen < old->used_gen)
            old = c;
    }
    if (!old)
        return NULL;
    if (!bwg_surf_reused++)
        fprintf(stderr, "[GPU-D3D11] all %d colour surface slots are in use: %08X (%ux%u, last used"
                        " frame %u, now %u) makes room for %08X. Later reuses are counted\n",
                slot_cap(), old->va, old->w, old->h, old->used_gen, bwg_gen, want_va);
    s_ops->release(old);
    s_ops->forget(old);
    return old;
}

static BwgDepth *depth_slot(uint32_t want_va)
{
    BwgDepth *old = NULL;
    for (int i = 0; i < s_ndepth; i++)
        if (!s_depth[i].va)
            return &s_depth[i];             /* item 36: a slot a failure left empty */
    if (s_ndepth < slot_cap())
        return &s_depth[s_ndepth++];
    for (int i = 0; i < s_ndepth; i++) {
        BwgDepth *c = &s_depth[i];
        if (c == bwg_cur_depth)
            continue;
        if (!old || c->used_gen < old->used_gen)
            old = c;
    }
    if (!old)
        return NULL;
    if (!bwg_depth_reused++)
        fprintf(stderr, "[GPU-D3D11] all %d depth buffer slots are in use: %08X (%ux%u, last used"
                        " frame %u, now %u) makes room for %08X. Later reuses are counted\n",
                slot_cap(), old->va, old->w, old->h, old->used_gen, bwg_gen, want_va);
    s_ops->release_depth(old);
    memset(old, 0, sizeof(*old));
    return old;
}

BwgSurf *bwg_get_surf(const Nv2aSurface *in)
{
    uint32_t bpp = in->bytes_per_pixel ? in->bytes_per_pixel : 4;
    uint32_t w = in->pitch && in->pitch / bpp >= in->width ? in->pitch / bpp : in->width;
    uint32_t h = in->height;
    BwgSurf *s = NULL;
    for (int i = 0; i < s_nsurf; i++)
        if (s_surf[i].va == in->color_va) { s = &s_surf[i]; break; }
    if (s && (s->bpp != bpp || s->w < w || s->h < h || !s->tex
              || (bw_vfx_active() && s->epoch != bw_vfx_epoch()))) {   /* VFX (D9): rescaled */
        uint32_t ow = s->w, oh = s->h, obpp = s->bpp, opitch = s->pitch;
        if (!s->tex && s->fail_gen == bwg_gen)
            return NULL;                    /* item 36: it couldn't be made this frame; next frame */
        s_ops->release(s);
        /* 0.2.5 item 38 (a port bug, every edition): the bound surface,
         * re-made, must be bound again. begin only binds when the surface
         * changes, so the context kept the released target and the draws
         * after a re-make (the front buffer grows when the game binds it at
         * the AA surface's pitch) went into the old texture, never shown. */
        if (bwg_cur == s)
            bwg_cur = NULL;
        s->bpp = bpp;
        s->w = w > s->w ? w : s->w;
        s->h = h > s->h ? h : s->h;
        s->pitch = in->pitch;
        surf_physical_size(s, in);
        if (!s_ops->create(s)) {
            s->fail_gen = bwg_gen;            /* item 36: released; kept by address to retry */
            return NULL;
        }
        if (s->w != ow || s->h != oh || s->bpp != obpp) {
            static unsigned n;              /* item 38: a fact for bug reports (a few, then quiet) */
            if (n++ < 8)
                fprintf(stderr, "[GPU-D3D11] surface %08X re-made at frame %u: %ux%u %u-byte"
                                " (pitch %u) -> %ux%u %u-byte (pitch %u), bound again\n",
                        s->va, bwg_gen, ow, oh, obpp, opitch, s->w, s->h, s->bpp, s->pitch);
        }
    }
    if (!s) {
        if (!(s = surf_slot(in->color_va)))  /* item 37: a free slot, or the oldest one's */
            return NULL;
        memset(s, 0, sizeof(*s));
        s->va = in->color_va;
        s->pitch = in->pitch;
        s->bpp = bpp;
        s->w = w;
        s->h = h;
        surf_physical_size(s, in);          /* VFX (D9) render scale, widescreen k */
        if (!s_ops->create(s)) {
            /* item 36: kept (empty, never drawn with: find_surf wants a
             * texture) so it is tried once a frame, not once a draw */
            s->fail_gen = bwg_gen;
            return NULL;
        }
        if (s->wsk > 1.0f)
            fprintf(stderr, "[WS] surface %08X %ux%u -> %ux%u on the GPU (widescreen x%.4f)\n",
                    s->va, s->w, s->h, s->pw, s->ph, s->wsk);
    }
    s->cur_pitch = in->pitch;
    s->aa_sx = (uint8_t)(in->aa_sx ? in->aa_sx : 1);
    return s;
}

/* The depth buffer at guest address va for a colour target of w x h with
 * `samples` samples per pixel (1 in Original; the MSAA count in Remastered,
 * or 1 for a surface whose MSAA target couldn't be made, item 36). */
BwgDepth *bwg_get_depth(uint32_t va, uint32_t w, uint32_t h, unsigned samples)
{
    BwgDepth *d = NULL;
    static uint32_t fail_va, fail_gen;     /* item 36: the last one that couldn't be made, and when */
    if (!va)
        return NULL;
    for (int i = 0; i < s_ndepth; i++) {
        BwgDepth *c = &s_depth[i];
        if (c->va != va)
            continue;
        if (!bw_vfx_active()) {
            d = c;                          /* Original: one per address, grown to the largest */
            break;
        }
        /* 0.2.5 item 39, Remastered: one per address, size and sample
         * count, so a depth address used with two sizes of target keeps one
         * of each instead of being re-made at every switch. One made for an
         * earlier render scale is released here. */
        if (c->epoch != bw_vfx_epoch()) {
            if (c == bwg_cur_depth) {
                bwg_cur_depth = NULL;
                bwg_cur = NULL;
            }
            s_ops->release_depth(c);
            memset(c, 0, sizeof(*c));
            continue;
        }
        if (c->w == w && c->h == h && c->samples == samples) {
            d = c;
            break;
        }
    }
    if (d && d->samples == samples && d->w >= w && d->h >= h
            && (!bw_vfx_active() || (d->epoch == bw_vfx_epoch() && d->w == w && d->h == h)))
        return d;
    if (va == fail_va && bwg_gen == fail_gen)
        return NULL;                        /* item 36: tried this frame already; next frame */
    if (d) {
        s_ops->release_depth(d);
        if (d == bwg_cur_depth) {
            /* item 38: the bound depth buffer, re-made, is bound again (begin
             * binds when the pair changes; clearing bwg_cur makes it) */
            bwg_cur_depth = NULL;
            bwg_cur = NULL;
        }
        if (!bw_vfx_active()) {
            w = w > d->w ? w : d->w;
            h = h > d->h ? h : d->h;
        }
        {
            static unsigned n;              /* item 39: a fact for bug reports (a few, then quiet) */
            if (n++ < 8)
                fprintf(stderr, "[GPU-D3D11] depth buffer %08X re-made at frame %u: %ux%u x%u -> %ux%u x%u"
                                " samples\n", va, bwg_gen, d->w, d->h, d->samples, w, h, samples);
        }
    } else if (!(d = depth_slot(va)))       /* items 36-37: a free slot, or the oldest one's */
        return NULL;
    memset(d, 0, sizeof(*d));
    d->va = va; d->w = w; d->h = h; d->samples = samples;
    d->epoch = bw_vfx_epoch();
    if (s_ops->create_depth(d))         /* the back end's objects */
        return d;
    /* item 36: nothing half-made stays in a live slot (a NULL view used to
     * be released and cleared later); the slot is left empty for reuse */
    s_ops->release_depth(d);
    memset(d, 0, sizeof(*d));
    if (d == &s_depth[s_ndepth - 1])
        s_ndepth--;
    fail_va = va;
    fail_gen = bwg_gen;
    return NULL;
}
