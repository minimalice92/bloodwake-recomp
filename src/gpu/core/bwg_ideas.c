/*
 * bwg_ideas.c - the renderer's API-neutral core: I5's feedback counter (see
 * bwg_ideas.h; lane gfx-ideas). Inert unless BW_GPU_FEEDBACK is set: the
 * texture cache asks bwg_feedback_mode() first and calls nothing else here.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bwg_core.h"
#include "bwg_surf.h"
#include "bwg_ideas.h"
#include "../../bw_edition.h"           /* bw_edition_at_least: the defaults */

BwgFbCounts bwg_fb;

static uint64_t s_draw;             /* the current clear or draw (bwg_fb_begin) */
static uint64_t s_noted_exact, s_noted_overlap, s_noted_depth;   /* the draw each kind was last counted for */
static uint64_t s_last_line;        /* bwg_fb's totals at the last summary line */
static unsigned s_logged;
static int s_fb_explicit;           /* BW_GPU_FEEDBACK was set (else the default: quiet unless a count moves) */

int bwg_feedback_mode(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("BW_GPU_FEEDBACK");
        s_fb_explicit = e && *e;
        v = BWG_FB_OFF;
        if (e && !strcmp(e, "count"))
            v = BWG_FB_COUNT;
        else if (e && !strcmp(e, "copy"))
            v = BWG_FB_COPY;
        else if ((!e || !*e) && bw_edition_at_least(BW_ED_REMASTERED))
            v = BWG_FB_COPY;                /* the lead's default (lane gfx-ideas): Remastered and later */
        if (v)
            fprintf(stderr, "[GPU-FEEDBACK] a draw sampling its own target: %s (BW_GPU_FEEDBACK)\n",
                    v == BWG_FB_COPY ? "sampled from a copy made before the draw, and counted"
                                     : "counted (today's rule: the guest bytes)");
    }
    return v;
}

void bwg_fb_begin(void)
{
    s_draw++;
    bwg_fb.draws++;
}

static int spans_cross(uint32_t a, uint32_t an, uint32_t b, uint32_t bn)
{
    return an && bn && a < b + bn && b < a + an;
}

static void note(const char *kind, const Nv2aTexture *t, uint32_t span, uint32_t tva, uint32_t tspan)
{
    if (s_logged++ >= 32)
        return;
    fprintf(stderr, "[GPU-FEEDBACK] flip %u draw %llu: %s: texture %08X %ux%u format 0x%02X (%u bytes)"
                    " against target %08X (%u bytes)\n",
            bwg_gen, (unsigned long long)s_draw, kind, t->offset, t->width, t->height, t->color, span,
            tva, tspan);
}

int bwg_fb_stage(const Nv2aTexture *t, uint32_t span)
{
    const BwgSurf *c = bwg_cur;
    uint32_t cpitch, cspan;
    if (!t || !c)
        return 0;
    cpitch = c->cur_pitch ? c->cur_pitch : (c->pitch ? c->pitch : c->w * c->bpp);
    cspan = cpitch * c->h;
    if (t->offset == c->va) {
        if (s_noted_exact != s_draw) {
            s_noted_exact = s_draw;
            bwg_fb.exact++;
            note("exact", t, span, c->va, cspan);
        }
        return 1;
    }
    if (spans_cross(t->offset, span, c->va, cspan) && s_noted_overlap != s_draw) {
        s_noted_overlap = s_draw;
        bwg_fb.overlap++;
        note("overlapping", t, span, c->va, cspan);
    }
    /* the depth buffer's guest bytes: the colour target's logical size at 4
     * bytes a pixel (D24S8; inferred, the back end keeps only its physical
     * size) */
    if (bwg_cur_depth && spans_cross(t->offset, span, bwg_cur_depth->va, c->w * c->h * 4u)
            && s_noted_depth != s_draw) {
        s_noted_depth = s_draw;
        bwg_fb.depth++;
        note("over the depth buffer", t, span, bwg_cur_depth->va, c->w * c->h * 4u);
    }
    return 0;
}

BwgSurf *bwg_fb_copy(BwgSurf *cur, BwgSurf *(*op)(BwgSurf *))
{
    static uint64_t made_for;
    static BwgSurf *made;
    if (made_for == s_draw && made)
        return made;
    made = op ? op(cur) : NULL;
    made_for = s_draw;
    if (made)
        bwg_fb.copies++;
    else
        bwg_fb.copy_failed++;
    return made;
}

void bwg_fb_flip(void)
{
    uint64_t tot = bwg_fb.exact + bwg_fb.overlap + bwg_fb.depth + bwg_fb.copies + bwg_fb.copy_failed;
    if (!bwg_feedback_mode())
        return;
    /* every 60 flips when asked for, and at once when a count moved: the
     * last line always holds the final counts (a replay ends with
     * ExitProcess, no atexit); the default (unset) is quiet until one moves */
    if (!s_last_line)
        s_last_line = 1;
    if (((bwg_gen % 60) == 0 && s_fb_explicit) || tot + 1 != s_last_line) {
        fprintf(stderr, "[GPU-FEEDBACK] flip %u: %llu clears and draws; feedback draws: exact %llu,"
                        " overlapping %llu, over the depth buffer %llu; copies %llu (%llu failed)\n",
                bwg_gen, (unsigned long long)bwg_fb.draws, (unsigned long long)bwg_fb.exact,
                (unsigned long long)bwg_fb.overlap, (unsigned long long)bwg_fb.depth,
                (unsigned long long)bwg_fb.copies, (unsigned long long)bwg_fb.copy_failed);
        s_last_line = tot + 1;
    }
}
