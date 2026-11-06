/*
 * bwg_latch.c - the renderer's API-neutral core: I1, the CPU-write latch
 * (bwg_latch.h; lane gfx-regen, docs/gfx/lanes/regen.md).
 *
 * The game's threads publish each lock into a ring (a slot claimed by an
 * interlocked counter: the main thread and the loading thread both lock).
 * The graphics thread drains the ring at the points where the back end
 * touches a surface (a clear or draw's target, a texture lookup of a made
 * surface, the flip's shown surface) and at its idle point; a lock whose
 * bytes cross a made surface's guest span marks that surface with the union
 * of the rectangles the lock's rows cover, in the surface's own rows (a lock
 * can use another pitch than the surface's binding: the front buffer is
 * locked at 640 pixels a row and drawn at the anti-aliased 1280). The marked
 * rectangle goes up from guest memory at the surface's next use, so the CPU
 * writes, made after LockRect returned and before the commands that use the
 * surface next were kicked off on the same thread, are in it (inferred:
 * ideas.md section 6, "Order"). The loading thread is the exception: it
 * writes the front buffer while nothing is flipped, so the idle point
 * uploads into the shown surface and presents it again once no flip has
 * come for BW_GPU_LATCH_IDLE_MS.
 *
 * The capture (BW_GFXCAP): each drained lock is a LOCK record in stream
 * order, the upload's guest bytes are a GPU read (PAGE/DIFF), and each idle
 * re-present a LATCHSHOW record, so a replay makes the same uploads at the
 * same points. Older replays skip both records (unknown types).
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bwg_core.h"
#include "bwg_surf.h"
#include "bwg_latch.h"
#include "../bw_vfx.h"                  /* Remastered: guest memory lags the GPU's copy (forced read) */
#include "../bw_gfxcap.h"

volatile int g_bw_lockwatch = 1;    /* until the switches are read (read_switches) */
volatile long g_bwg_latch_pub, g_bwg_latch_done;
int bwg_latch_marks;
void (*bwg_latch_wake_fn)(void);        /* bw_wait.c: wakes the graphics thread (bw_ack_kick) */

static const BwgLatchOps *s_ops;
static int s_mode = -1;                 /* 0 off, 1 on, 2 count */
static int s_log;                       /* BW_GPU_LATCH_LOG=<n> */
static double s_idle_ms = 50.0;         /* BW_GPU_LATCH_IDLE_MS */
static int s_lockread;                  /* BW_GPU_LOCKREAD=1 */

#define RING 4096                       /* the loading thread redraws 50 blocks a step; seen: 256 lapped under a capture */
typedef struct {
    volatile LONG seq;                  /* n + 1 once entry n is written */
    uint32_t bits, pitch, flags, level, res, has_rect, tid;
    int32_t r[4];
    double t_ms;
} LockEnt;
static LockEnt s_ring[RING];

typedef struct {
    uint32_t va;                        /* the surface the mark is for (a reused slot drops it) */
    uint32_t r[4];                      /* x0, y0, x1, y1 in the surface's logical pixels */
    int on;
} Mark;
static Mark s_mark[BWG_MAX_SURF];

static BwgSurf *s_shown;                /* the surface the last flip showed */
static double s_flip_ms, s_show_ms;

/* counters (the stats line, the report) */
static unsigned long s_n_locks, s_n_hit, s_n_miss, s_n_lost, s_n_up, s_n_show, s_n_reads, s_n_upx;
static unsigned long long s_up_px;
static unsigned long s_l_locks, s_l_up, s_l_show;

/* the forced read: one request at a time (game threads take turns) */
static CRITICAL_SECTION s_req_cs;
static HANDLE s_req_done;
static volatile LONG s_req_on;
static uint32_t s_req_va, s_req_bytes;

int bwg_latch_mode(void)
{
    return s_mode < 0 ? 0 : s_mode;
}

/* The switches, once: at the first lock (the wrapper can run before the back
 * end's device is made) or at the back end's set-up, whichever is first. */
static void read_switches(void)
{
    const char *e;
    if (s_mode >= 0)
        return;
    e = getenv("BW_GPU_LATCH");
    s_mode = !e || !*e ? 1 : !strcmp(e, "count") ? 2 : e[0] == '0' ? 0 : 1;
    if ((e = getenv("BW_GPU_LATCH_LOG")) != NULL)
        s_log = atoi(e);
    if ((e = getenv("BW_GPU_LATCH_IDLE_MS")) != NULL && atof(e) > 0.0)
        s_idle_ms = atof(e);
    if ((e = getenv("BW_GPU_LOCKREAD")) != NULL)
        s_lockread = e[0] == '1';
    if (s_lockread) {
        InitializeCriticalSection(&s_req_cs);
        s_req_done = CreateEventA(NULL, FALSE, FALSE, NULL);
        if (!s_req_done)
            s_lockread = 0;
    }
    if (g_bw_gfxcap == 2)
        g_bw_lockwatch = 0;             /* replaying: no game runs; LOCK records stand in */
    else
        g_bw_lockwatch = s_mode != 0;
    fprintf(stderr, "[GPU-LATCH] I1, the CPU-write latch: %s%s (BW_GPU_LATCH)\n",
            s_mode == 0 ? "off" : s_mode == 2 ? "counting only" : "on",
            s_lockread ? ", with the forced read (BW_GPU_LOCKREAD)" : "");
}

void bwg_latch_init(const BwgLatchOps *ops)
{
    read_switches();
    s_ops = ops;
}

/* A lock's CPU address as the surfaces' guest address: the tiled aperture at
 * 0xF0000000 is a second view of the contiguous window at 0x80000000 (the
 * runtime's memory layout; D3D returns it for lock flag 0x40, seen in the
 * body at 0x12E508). */
static uint32_t lock_va(uint32_t bits)
{
    return bits >= 0xF0000000u ? 0x80000000u | (bits & 0x0FFFFFFFu) : bits;
}

void bw_gpu_lock_note(uint32_t res, uint32_t level, uint32_t pitch, uint32_t bits,
                      const int32_t *rect, uint32_t flags)
{
    long n;
    LockEnt *e;
    if (s_mode < 0)
        read_switches();                /* the first lock can come before the device is made */
    if (!g_bw_lockwatch || !bits)
        return;
    n = InterlockedIncrement(&g_bwg_latch_pub) - 1;
    e = &s_ring[n & (RING - 1)];
    e->bits = bits;
    e->pitch = pitch;
    e->flags = flags;
    e->level = level;
    e->res = res;
    e->has_rect = rect != NULL;
    if (rect)
        memcpy(e->r, rect, sizeof e->r);
    else
        memset(e->r, 0, sizeof e->r);
    e->tid = GetCurrentThreadId();
    e->t_ms = bwg_qpc_ms();
    MemoryBarrier();
    InterlockedExchange(&e->seq, n + 1);
    if (bwg_latch_wake_fn)
        bwg_latch_wake_fn();            /* the graphics thread may be napping: the front-buffer rule */
    if (s_lockread && s_mode == 1) {
        /* the forced read: the graphics thread reads back any made surface
         * under these bytes that the GPU drew since guest memory had them,
         * before the game reads or changes them (ideas.md section 6, I2's
         * other half). The body has waited for the GPU to pass the
         * resource's fence (BlockOnResource), so every command touching the
         * surface has been walked and the graphics thread reaches its idle
         * point within a segment (inferred). At most 250 ms: then the game
         * goes on with guest memory as it is, and that is counted. */
        uint32_t span = rect ? (uint32_t)(rect[3] - rect[1]) * pitch : pitch * 480u;
        EnterCriticalSection(&s_req_cs);
        s_req_va = lock_va(bits);
        s_req_bytes = span ? span : pitch;
        ResetEvent(s_req_done);
        InterlockedExchange(&s_req_on, 1);
        if (bwg_latch_wake_fn)
            bwg_latch_wake_fn();
        if (WaitForSingleObject(s_req_done, 250) != WAIT_OBJECT_0) {
            static long late;
            if (InterlockedIncrement(&late) <= 3)
                fprintf(stderr, "[GPU-LATCH] a forced read took over 250 ms: the lock went on without it\n");
            InterlockedExchange(&s_req_on, 0);
        }
        LeaveCriticalSection(&s_req_cs);
    }
}

static int surf_index(const BwgSurf *s)
{
    BwgSurf *base = bwg_surf_at(0);
    int i = base ? (int)(s - base) : -1;
    return i >= 0 && i < BWG_MAX_SURF ? i : -1;
}

static uint32_t surf_pitch(const BwgSurf *s)
{
    return s->cur_pitch ? s->cur_pitch : (s->pitch ? s->pitch : s->w * s->bpp);
}

static void mark_union(BwgSurf *s, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{
    int i = surf_index(s);
    Mark *m;
    if (i < 0 || x1 <= x0 || y1 <= y0)
        return;
    m = &s_mark[i];
    if (!m->on || m->va != s->va) {
        if (!m->on)
            bwg_latch_marks++;
        m->on = 1;
        m->va = s->va;
        m->r[0] = x0; m->r[1] = y0; m->r[2] = x1; m->r[3] = y1;
        return;
    }
    if (x0 < m->r[0]) m->r[0] = x0;
    if (y0 < m->r[1]) m->r[1] = y0;
    if (x1 > m->r[2]) m->r[2] = x1;
    if (y1 > m->r[3]) m->r[3] = y1;
}

/* One lock against the made surfaces: the rectangle of each one its bytes
 * cross, in that surface's rows (a lock row that crosses a surface row's
 * end marks the whole rows). 1 if any was hit. */
static int mark_lock(uint32_t va, uint32_t lpitch, int has_rect, const int32_t *r)
{
    int i, n = bwg_surf_count(), hit = 0;
    for (i = 0; i < n; i++) {
        BwgSurf *s = bwg_surf_at(i);
        uint32_t sp, span, off, rows, rowb, y, x0 = 0xFFFFFFFFu, y0 = 0xFFFFFFFFu, x1 = 0, y1 = 0;
        if (!s || !s->api[0] || !s->bpp || !s->w || !s->h)
            continue;
        sp = surf_pitch(s);
        span = (s->h - 1) * sp + s->w * s->bpp;
        if (va < s->va || va >= s->va + span)
            continue;
        off = va - s->va;
        if (!has_rect) {                    /* the whole level: to the surface's end */
            mark_union(s, 0, off / sp, s->w, s->h);
            hit = 1;
            continue;
        }
        if (r[2] <= r[0] || r[3] <= r[1])
            continue;
        rows = (uint32_t)(r[3] - r[1]);
        rowb = (uint32_t)(r[2] - r[0]) * s->bpp;
        for (y = 0; y < rows; y++) {
            uint64_t a = (uint64_t)off + (uint64_t)y * lpitch, b = a + rowb - 1;
            uint32_t ay, by, ax, bx;
            if (a >= span)
                break;
            if (b >= span)
                b = span - 1;
            ay = (uint32_t)(a / sp);
            by = (uint32_t)(b / sp);
            ax = (uint32_t)(a % sp) / s->bpp;
            bx = (uint32_t)(b % sp) / s->bpp + 1;
            if (ay != by || ax >= s->w || bx > s->w) {
                ax = 0;                     /* across a row's end (or past its pixels): whole rows */
                bx = s->w;
            }
            if (ax < x0) x0 = ax;
            if (bx > x1) x1 = bx;
            if (ay < y0) y0 = ay;
            if (by + 1 > y1) y1 = by + 1;
        }
        if (y1 > s->h) y1 = s->h;
        if (x1 > x0 && y1 > y0) {
            mark_union(s, x0, y0, x1, y1);
            hit = 1;
        }
    }
    return hit;
}

static void take_lock(const LockEnt *e, int at_idle)
{
    uint32_t va = lock_va(e->bits);
    int hit;
    s_n_locks++;
    s_l_locks++;
    if (BW_GFXCAP_REC())
        bw_gfxcap_lock(va, e->pitch, e->flags, e->has_rect, e->r, at_idle);
    /* D3DLOCK_READONLY (0x10): the game only reads; nothing to bring up */
    hit = (e->flags & 0x10u) ? 0 : mark_lock(va, e->pitch, e->has_rect, e->r);
    if (hit)
        s_n_hit++;
    else
        s_n_miss++;
    if (s_log > 0) {
        BwgSurf *so = g_bw_gfxcap != 2 && s_ops && s_ops->scanout ? s_ops->scanout() : NULL;
        s_log--;
        fprintf(stderr, "[GPU-LATCH] lock f%u t%.1f thread %u res %08X level %u bits %08X pitch %u"
                        " rect %s%d,%d..%d,%d flags %02X: %s (scan-out %08X, last flip %08X)\n", bwg_gen,
                e->t_ms, e->tid, e->res, e->level, e->bits, e->pitch, e->has_rect ? "" : "(none) ",
                e->r[0], e->r[1], e->r[2], e->r[3], e->flags, hit ? "marks a made surface" : "no made surface",
                so ? so->va : 0u, s_shown ? s_shown->va : 0u);
    }
}

static void drain(int at_idle)
{
    while (g_bwg_latch_done != g_bwg_latch_pub) {
        long n = g_bwg_latch_done;
        LockEnt *e = &s_ring[n & (RING - 1)];
        LONG q = e->seq;
        if (q != n + 1) {
            if (q > n + 1) {                /* lapped: the ring held more than RING */
                if (!s_n_lost++)
                    fprintf(stderr, "[GPU-LATCH] the lock ring lapped (more than %d locks waited): later"
                                    " losses are counted\n", RING);
                InterlockedIncrement(&g_bwg_latch_done);
                continue;
            }
            break;                          /* claimed, not yet written */
        }
        MemoryBarrier();
        take_lock(e, at_idle);
        InterlockedIncrement(&g_bwg_latch_done);
    }
}

void bwg_latch_drain(void)
{
    if (BWG_LATCH_WAITING())
        drain(0);
}

/* The mark's rectangle up (1 if anything went up). */
static int upload(BwgSurf *s, Mark *m)
{
    uint32_t r[4], sp = surf_pitch(s), room = bwg_block_size(s->va), need;
    memcpy(r, m->r, sizeof r);
    m->on = 0;
    bwg_latch_marks--;
    if (r[2] > s->w) r[2] = s->w;
    if (r[3] > s->h) r[3] = s->h;
    if (r[2] <= r[0] || r[3] <= r[1])
        return 0;
    /* never past the guest block (item 38's rule): the rows that fit */
    if (room && (uint64_t)(r[3] - 1) * sp + (uint64_t)r[2] * s->bpp > room) {
        if ((uint64_t)r[1] * sp + (uint64_t)r[2] * s->bpp > room)
            return 0;
        r[3] = (room - r[2] * s->bpp) / sp + 1;
        if (r[3] <= r[1])
            return 0;
    }
    need = (r[3] - r[1] - 1) * sp + (r[2] - r[0]) * s->bpp;
    if (BW_GFXCAP_REC())
        bw_gfxcap_read(s->va + r[1] * sp + r[0] * s->bpp, need);
    s_n_up++;
    s_l_up++;
    s_up_px += (unsigned long long)(r[2] - r[0]) * (r[3] - r[1]);
    if (s_log > 0) {
        s_log--;
        fprintf(stderr, "[GPU-LATCH] upload f%u surface %08X (%ux%u pitch %u) rows %u..%u columns %u..%u\n",
                bwg_gen, s->va, s->w, s->h, sp, r[1], r[3] - 1, r[0], r[2] - 1);
    }
    if (s_mode == 1 && s_ops && s_ops->upload)
        s_ops->upload(s, r);
    else
        s_n_upx++;                          /* counting only */
    return 1;
}

void bwg_latch_use_(BwgSurf *s)
{
    int i;
    if (BWG_LATCH_WAITING())
        drain(0);
    if (!s || !bwg_latch_marks || (i = surf_index(s)) < 0 || !s_mark[i].on)
        return;
    if (s_mark[i].va != s->va || !s->api[0]) {
        s_mark[i].on = 0;                   /* a reused slot, or not made: its seed reads guest memory */
        bwg_latch_marks--;
        return;
    }
    upload(s, &s_mark[i]);
}

void bwg_latch_forget(BwgSurf *s)
{
    int i = s ? surf_index(s) : -1;
    if (i >= 0 && s_mark[i].on) {
        s_mark[i].on = 0;
        bwg_latch_marks--;
    }
    if (s == s_shown)
        s_shown = NULL;
}

void bwg_latch_flipped(BwgSurf *shown)
{
    s_shown = shown;
    s_flip_ms = bwg_qpc_ms();
}

static int has_mark(const BwgSurf *s)
{
    int i = s ? surf_index(s) : -1;
    return i >= 0 && s_mark[i].on && s_mark[i].va == s->va && s->api[0];
}

/* The front-buffer rule: t's marked rectangle up, and t presented again.
 * t is the surface the display scans out (the console shows its memory),
 * else the one the last flip showed. */
static void show_again(BwgSurf *t)
{
    int i = t ? surf_index(t) : -1;
    if (i < 0 || !s_mark[i].on)
        return;
    if (!has_mark(t)) {
        s_mark[i].on = 0;
        bwg_latch_marks--;
        return;
    }
    if (!upload(t, &s_mark[i]))
        return;
    if (s_mode == 1 && s_ops && s_ops->represent && s_ops->represent(t)) {
        s_n_show++;
        s_l_show++;
    }
    /* LATCHSHOW after the upload's bytes (DIFF) and the present's own record
     * (the suite's clock, PRESENT): a replay patches guest memory and takes
     * the clock, then makes the same upload and present here */
    if (BW_GFXCAP_REC())
        bw_gfxcap_latchshow(t->va);
    s_show_ms = bwg_qpc_ms();
}

static void forced_read(void)
{
    int i, n = bwg_surf_count();
    for (i = 0; i < n; i++) {
        BwgSurf *s = bwg_surf_at(i);
        uint32_t span;
        if (!s || !s->api[0] || !s->bpp)
            continue;
        span = (s->h - 1) * surf_pitch(s) + s->w * s->bpp;
        if (s_req_va + s_req_bytes <= s->va || s_req_va >= s->va + span)
            continue;
        if (!(s->dirty || bw_vfx_active()))
            continue;                       /* guest memory has the GPU's bytes (the flip's readback) */
        if (s_ops && s_ops->read_now) {
            s_ops->read_now(s);
            s_n_reads++;
        }
    }
}

void bwg_latch_idle(void)
{
    double now;
    if (s_mode <= 0 || g_bw_gfxcap == 2 || !s_ops)
        return;
    if (s_req_on) {
        if (BW_GFXCAP_REC())
            bw_gfxcap_settle();             /* the last method has run: records go after it */
        drain(1);
        forced_read();
        InterlockedExchange(&s_req_on, 0);
        SetEvent(s_req_done);
    }
    if (!BWG_LATCH_WAITING() && !bwg_latch_marks)
        return;
    if (BW_GFXCAP_REC())
        bw_gfxcap_settle();
    drain(1);
    if (!bwg_latch_marks)
        return;
    now = bwg_qpc_ms();
    if (now - s_flip_ms < s_idle_ms || now - s_show_ms < 16.0)
        return;                             /* frames are coming, or one was just shown again */
    {
        /* the scan-out buffer first: the loading thread writes the front
         * buffer (GetBackBuffer(-1)), which this back end never flips (it
         * presents the anti-aliased surface it resolves itself) */
        BwgSurf *t = s_ops->scanout ? s_ops->scanout() : NULL;
        /* only the scan-out buffer: a CPU write into a back buffer (the
         * surface this back end flips) reaches the console's screen only at
         * the game's next present, and then the flip uploads it */
        if (has_mark(t))
            show_again(t);
    }
}

void bwg_latch_replay_lock(const uint8_t *p, uint32_t len)
{
    uint32_t va, pitch, flags, has_rect;
    int32_t r[4];
    LockEnt e;
    if (len < 32 || bwg_latch_mode() == 0)
        return;
    memcpy(&va, p, 4);
    memcpy(&pitch, p + 4, 4);
    memcpy(&flags, p + 8, 4);
    memcpy(&has_rect, p + 12, 4);
    memcpy(r, p + 16, 16);
    memset(&e, 0, sizeof e);
    e.bits = va;
    e.pitch = pitch;
    e.flags = flags;
    e.has_rect = has_rect;
    memcpy(e.r, r, sizeof r);
    e.t_ms = bwg_qpc_ms();
    take_lock(&e, 0);
}

void bwg_latch_replay_show(const uint8_t *p, uint32_t len)
{
    uint32_t va;
    if (bwg_latch_mode() == 0 || len < 4)
        return;
    memcpy(&va, p, 4);
    show_again(bwg_find_surf(va));
}

void bwg_latch_line(void)
{
    if (!s_l_locks && !s_l_up && !s_l_show)
        return;
    fprintf(stderr, "[GPU-LATCH] locks %lu (%lu marked a made surface, %lu none, %lu lost) | uploads %lu"
                    " (%llu pixels%s) | shown again %lu | forced reads %lu | this second: locks %lu,"
                    " uploads %lu, shown again %lu\n", s_n_locks, s_n_hit, s_n_miss, s_n_lost, s_n_up,
            s_up_px, s_n_upx ? ", counted only" : "", s_n_show, s_n_reads, s_l_locks, s_l_up, s_l_show);
    s_l_locks = s_l_up = s_l_show = 0;
}
