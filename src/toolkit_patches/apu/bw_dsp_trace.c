/* SPDX-License-Identifier: GPL-2.0-or-later (with xemu's DSP it hosts; see NOTICE.md) */
/*
 * bw_dsp_trace.c - BW_DSP_TRACE=<dir>: everything that enters and leaves the
 * GP and EP DSPs, frame by frame, so a run can be replayed offline through
 * any engine (tools/dsp_replay) and checked bit for bit (tools/au_proof.py).
 * A test aid in every edition. Unset, nothing is installed: the DSP stage
 * runs exactly as without this file. The format is docs/audio/trace_format.md.
 *
 * How it records (seen in bw_dsp.c and bw_apu_xdsp.c: the only ways in):
 *  - Between two runs of a DSP, the bridge changes its state from outside:
 *    resets, bootstraps, DirectSound's window words (sync_in), the VP's mix
 *    buffer. All of it is caught as one difference: the state at the start
 *    of a run against the state at the end of the previous one (DMEM/DCORE).
 *  - During a run, the DSP reaches outside only through its DMA callbacks
 *    (bw_dsp.c's trampolines): FIFO reads are kept whole; scratch reads keep
 *    only the bytes that differ from a model of scratch memory (what the
 *    snapshot held plus what the DSP wrote since), so the reverb's delay
 *    lines cost nothing unless something else wrote them; scratch writes
 *    keep a hash (BW_DSP_TRACE_WRITES=data: the data too); FIFO writes
 *    (the GP's output, the EP's final mix) are kept whole.
 *  - After each run: the cycles used and a hash of each part of the state,
 *    and every BW_DSP_TRACE_KEY runs the whole state (KEY).
 *
 * Switches (frames are the DSP stage's 32-sample frames, 1500 a second of
 * audio; "<n>s" means seconds):
 *  BW_DSP_TRACE=<dir>           on; files go to <dir> (made if missing)
 *  BW_DSP_TRACE_START=<n|ns>    open the traces at this frame (default: the
 *                               GP's first bootstrap); =xy[@<n|ns>]: at the
 *                               first GP run (after that frame) whose X or Y
 *                               memory was changed from outside, e.g. by
 *                               DirectSound's reverb settings at a zone;
 *                               =file:<path>: when that file appears (a test
 *                               tool watching the boat's position makes it)
 *  BW_DSP_TRACE_LEN=<n|ns>      and close them this many frames later
 *                               (default 30 s)
 *  BW_DSP_TRACE_SNAP=<n|ns>,..  extra whole-state snapshots at these frames
 *  BW_DSP_TRACE_KEY=<runs>      a KEY every this many GP runs (default 1500;
 *                               the EP every eighth of that); 0 = none
 *  BW_DSP_TRACE_WRITES=data     scratch writes keep their data
 *  BW_DSP_TRACE_WATCH=<list>    log words whenever they change (see watch_parse)
 * Every reset-to-run edge (a bootstrap) also writes a snapshot file, at the
 * DSP's first run after it; and so does a run in which the DSP changed its
 * own P memory (the GP loads its effect code by DMA about 0.4 s after boot,
 * au-dsp), at the next run, up to 8 per DSP. These hold the game's DSP code:
 * local test data under analysis/, a replay input and nothing else.
 */
#include "apu_state.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "bw_dsp.h"
#include "bw_dsp_trace_fmt.h"
#include "bw_dsp_trace.h"
#include "../../bw_edition.h"

#define FPS 1500
#define FLUSH_AT (1u << 20)             /* hand a chunk to the writer at 1 MB */
#define QUEUE_MAX ((size_t)256 << 20)   /* the DSP thread waits beyond this */

/* A log line, flushed at once: test runs buffer stderr (BW_STDERR_BUFFER),
 * and a run ended by its harness would lose the tail. These lines are few. */
static void tlog(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fflush(stderr);
}

/* ---- a growable record buffer -------------------------------------------- */

typedef struct {
    uint8_t *p;
    size_t len, cap;
    size_t rec;                          /* where the open record's length goes */
} Buf;

static void b_need(Buf *b, size_t n)
{
    if (b->len + n <= b->cap)
        return;
    while (b->len + n > b->cap)
        b->cap = b->cap ? b->cap * 2 : (FLUSH_AT + (FLUSH_AT >> 1));
    b->p = (uint8_t *)realloc(b->p, b->cap);
    if (!b->p) {
        tlog("[DSP-TRACE] out of memory\n");
        abort();
    }
}

static void b_bytes(Buf *b, const void *v, size_t n)
{
    b_need(b, n);
    memcpy(b->p + b->len, v, n);
    b->len += n;
}

static void b_u32(Buf *b, uint32_t v) { b_bytes(b, &v, 4); }
static void b_u64(Buf *b, uint64_t v) { b_bytes(b, &v, 8); }

static void rec_open(Buf *b, uint8_t type)
{
    b_bytes(b, &type, 1);
    b->rec = b->len;
    b_u32(b, 0);
}

static void rec_close(Buf *b)
{
    uint32_t n = (uint32_t)(b->len - b->rec - 4);
    memcpy(b->p + b->rec, &n, 4);
}

/* ---- the writer thread ---------------------------------------------------- */

typedef struct Job {
    struct Job *next;
    HANDLE file;
    uint8_t *data;
    size_t len;
    int close;
} Job;

static Job *s_q_head, *s_q_tail;
static size_t s_q_bytes;
static SRWLOCK s_q_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE s_q_have = CONDITION_VARIABLE_INIT, s_q_room = CONDITION_VARIABLE_INIT;
static HANDLE s_writer;
static void steal_idle(void);

static DWORD WINAPI writer_thread(LPVOID arg)
{
    BwDtrCodec *codec = bw_dtr_codec_new();
    uint8_t *out = NULL;
    size_t out_cap = 0;
    (void)arg;
    for (;;) {
        Job *j;
        AcquireSRWLockExclusive(&s_q_lock);
        while (!s_q_head) {
            if (!SleepConditionVariableSRW(&s_q_have, &s_q_lock, 200, 0) && !s_q_head) {
                ReleaseSRWLockExclusive(&s_q_lock);
                steal_idle();
                AcquireSRWLockExclusive(&s_q_lock);
            }
        }
        j = s_q_head;
        s_q_head = j->next;
        if (!s_q_head)
            s_q_tail = NULL;
        ReleaseSRWLockExclusive(&s_q_lock);

        if (j->len) {
            BwDtrChunk ch;
            DWORD w;
            size_t need = bw_dtr_bound(j->len);
            if (need > out_cap) {
                out_cap = need;
                out = (uint8_t *)realloc(out, out_cap);
            }
            ch.raw_len = (uint32_t)j->len;
            ch.check = bw_dtr_hash32(j->data, j->len);
            ch.stored_len = (uint32_t)bw_dtr_compress(codec, j->data, j->len, out, out_cap, &ch.codec);
            WriteFile(j->file, &ch, sizeof ch, &w, NULL);
            WriteFile(j->file, out, ch.stored_len, &w, NULL);
        }
        if (j->close) {
            FlushFileBuffers(j->file);
            CloseHandle(j->file);
        }
        AcquireSRWLockExclusive(&s_q_lock);
        s_q_bytes -= j->len;
        WakeAllConditionVariable(&s_q_room);
        ReleaseSRWLockExclusive(&s_q_lock);
        free(j->data);
        free(j);
    }
    return 0;
}

/* Hand the buffer's records to the writer (the buffer starts again empty). */
static void enqueue(HANDLE file, Buf *b, int close)
{
    Job *j = (Job *)calloc(1, sizeof *j);
    j->file = file;
    j->data = b->p;
    j->len = b->len;
    j->close = close;
    b->p = NULL;
    b->len = b->cap = 0;
    AcquireSRWLockExclusive(&s_q_lock);
    while (s_q_bytes > QUEUE_MAX)
        SleepConditionVariableSRW(&s_q_room, &s_q_lock, INFINITE, 0);
    s_q_bytes += j->len;
    if (s_q_tail)
        s_q_tail->next = j;
    else
        s_q_head = j;
    s_q_tail = j;
    WakeAllConditionVariable(&s_q_have);
    ReleaseSRWLockExclusive(&s_q_lock);
}

/* ---- settings and per-DSP state ------------------------------------------- */

static int s_on;
static char s_dir[MAX_PATH];
static MCPXAPUState *s_d;
static long long s_start = -1, s_len = 30LL * FPS;
static long long s_snap_at[32];
static int s_nsnap, s_key_every = FPS, s_write_data;
static long s_frame = -1;
static int s_div;
static int s_window;                   /* 0 not yet, 1 open, 2 done */
static int s_start_xy;                 /* BW_DSP_TRACE_START=xy[@frame] */
static long long s_xy_after;
static char s_start_file[MAX_PATH];    /* BW_DSP_TRACE_START=file:<path> */
static char s_text[2048];

typedef struct {
    const char *name;                  /* "gp" / "ep" */
    int idx;                           /* 0 GP, 1 EP */
    HANDLE file;                       /* the trace, while open */
    int open;
    Buf buf;
    BwDspState *model, *live;
    uint8_t *page[BW_DTR_MODEL_PAGES];
    int in_run;
    uint32_t runs, key_every;
    uint32_t pending_ev;               /* BW_DTR_RUN_* since the last run */
    int edge_pending, asked_pending, edges;
    uint32_t *p_before;                /* P at the start of the run */
    int pload_pending, ploads;
    uint32_t pruns;                    /* runs that changed their own P */
    uint32_t *prev;                    /* X, Y, P after the last run */
    int have_prev;
    uint32_t outside_runs;             /* runs that got X/Y/P changes from outside */
    uint64_t raw;                      /* bytes of records written */
    long open_frame;
    /* buf belongs to the DSP thread while in_run, else to whoever holds lk:
     * the writer thread hands on a buffer left idle (the DSP stage stopped,
     * say by its fallback to the fold-down) so the run's end can't lose it. */
    SRWLOCK lk;
    volatile ULONGLONG touched;        /* GetTickCount64 at the last record */
} Tr;

static Tr s_tr[2] = { { "gp", 0 }, { "ep", 1 } };

static BwDsp *s_dsp[2];                /* seen by the hooks */

static Tr *tr_of(BwDsp *dsp)
{
    int i = bw_dsp_is_gp(dsp) ? 0 : 1;
    s_dsp[i] = dsp;
    return &s_tr[i];
}

/* ---- BW_DSP_TRACE_WATCH: words logged whenever they change ----------------
 *
 * A test aid for questions of order between the CPU and the DSPs, checked
 * once a frame (1500 a second) on the DSP stage's thread, before the frame's
 * resets and window merge:
 *   gpx:<addr> / epx:<addr>   the X word in the DSP's window (guest RAM, what
 *                             DirectSound wrote) and in the DSP's own memory
 *   m:<va>                    a guest word
 *   p:<va>,<off>,...,<off>    a pointer chain: MEM32(va), then MEM32(a + off)
 *                             for each offset (the last one gives the word)
 * e.g. BW_DSP_TRACE_WATCH=gpx:0x2AE;m:0x1E7B2C;p:0x1A4394,0,8
 * (entries separated by ';'). Reads only; a guest address outside RAM reads
 * as "-". */
extern ptrdiff_t xbox_GetMemoryOffset(void);

typedef struct {
    char kind;                         /* 'g' gpx, 'e' epx, 'm', 'p' */
    uint32_t va, off[6];
    int noff;
    uint64_t last;                     /* value | valid << 32 (and the DSP's) */
    uint64_t last2;
    int seen;
    char text[64];
} Watch;

static Watch s_watch[16];
static int s_nwatch;

static void watch_parse(const char *e)
{
    while (*e && s_nwatch < 16) {
        Watch *w = &s_watch[s_nwatch];
        const char *end = strchr(e, ';');
        size_t n = end ? (size_t)(end - e) : strlen(e);
        char *q;
        memset(w, 0, sizeof *w);
        _snprintf(w->text, sizeof w->text - 1, "%.*s", (int)(n < 60 ? n : 60), e);
        if (!strncmp(e, "gpx:", 4) || !strncmp(e, "epx:", 4)) {
            w->kind = e[0] == 'g' ? 'g' : 'e';
            w->va = (uint32_t)strtoul(e + 4, NULL, 0);
            s_nwatch++;
        } else if (!strncmp(e, "m:", 2)) {
            w->kind = 'm';
            w->va = (uint32_t)strtoul(e + 2, NULL, 0);
            s_nwatch++;
        } else if (!strncmp(e, "p:", 2)) {
            w->kind = 'p';
            w->va = (uint32_t)strtoul(e + 2, &q, 0);
            while (*q == ',' && w->noff < 6)
                w->off[w->noff++] = (uint32_t)strtoul(q + 1, &q, 0);
            s_nwatch++;
        }
        if (!end)
            break;
        e = end + 1;
    }
}

static int guest_rd(uint32_t va, uint32_t *v)
{
    if (va < 0x10000u || va > 0x03FFFFFCu)
        return 0;
    *v = *(volatile const uint32_t *)(uintptr_t)(va + xbox_GetMemoryOffset());
    return 1;
}

static void watch_frame(void)
{
    int i;
    for (i = 0; i < s_nwatch; i++) {
        Watch *w = &s_watch[i];
        uint32_t v = 0, v2 = 0, a;
        int ok = 0, ok2 = 0, k;
        uint64_t now, now2;
        switch (w->kind) {
        case 'g':
        case 'e': {
            uint32_t base = w->kind == 'g' ? 0xFE830000u : 0xFE850000u, words;
            BwDsp *dsp = s_dsp[w->kind == 'g' ? 0 : 1];
            v = *(volatile const uint32_t *)(uintptr_t)(base + NV_PAPU_GPXMEM + 4 * w->va +
                                                       xbox_GetMemoryOffset());
            ok = 1;
            if (dsp) {
                const uint32_t *m = bw_dsp_mem(dsp, 'X', &words);
                if (m && w->va < words) {
                    v2 = m[w->va];
                    ok2 = 1;
                }
            }
            break;
        }
        case 'm':
            ok = guest_rd(w->va, &v);
            break;
        case 'p':
            ok = guest_rd(w->va, &a);
            for (k = 0; ok && k < w->noff; k++) {
                ok = guest_rd(a + w->off[k], &a);
            }
            v = a;
            break;
        }
        now = (uint64_t)v | ((uint64_t)ok << 32);
        now2 = (uint64_t)v2 | ((uint64_t)ok2 << 32);
        if (w->seen && now == w->last && now2 == w->last2)
            continue;
        w->seen = 1;
        w->last = now;
        w->last2 = now2;
        if (w->kind == 'g' || w->kind == 'e')
            tlog("[DSP-WATCH] frame %ld: %s window 0x%06X, DSP %s%06X\n", s_frame, w->text, v,
                 ok2 ? "0x" : "-", ok2 ? v2 : 0);
        else if (ok)
            tlog("[DSP-WATCH] frame %ld: %s = 0x%08X\n", s_frame, w->text, v);
        else
            tlog("[DSP-WATCH] frame %ld: %s = -\n", s_frame, w->text);
    }
}

static long long parse_frames(const char *s)
{
    char *end;
    double v = strtod(s, &end);
    if (*end == 's')
        v *= FPS;
    return (long long)v;
}

static HANDLE open_file(const char *path, const BwDtrHeader *h)
{
    DWORD w;
    HANDLE f = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        tlog("[DSP-TRACE] can't create %s (error %lu)\n", path, GetLastError());
        return NULL;
    }
    WriteFile(f, h, sizeof *h, &w, NULL);
    WriteFile(f, s_text, h->text_len, &w, NULL);
    return f;
}

static void header(BwDtrHeader *h, const Tr *t, uint32_t kind)
{
    memset(h, 0, sizeof *h);
    memcpy(h->magic, BW_DTR_MAGIC, 8);
    h->version = BW_DTR_VERSION;
    h->dsp = (uint32_t)t->idx;
    h->state_words = (uint32_t)BW_DSP_STATE_WORDS;
    h->core_words = BW_DSP_CORE_WORDS;
    h->flags = s_write_data ? BW_DTR_F_WRITE_DATA : 0;
    h->key_every = kind ? 0 : t->key_every;
    h->pid = GetCurrentProcessId();
    h->kind = kind;
    h->text_len = (uint32_t)strlen(s_text);
}

/* ---- snapshots ------------------------------------------------------------- */

static int page_zero(const uint8_t *p)
{
    const uint64_t *q = (const uint64_t *)p;
    int i;
    for (i = 0; i < (int)(BW_DTR_PAGE / 8); i++)
        if (q[i])
            return 0;
    return 1;
}

/* A SNAP record of the live state into b. With model set, the scratch model
 * starts again from the snapshot's scratch pages. */
static void put_snap(Tr *t, Buf *b, uint32_t why, const BwDspState *live, int model)
{
    static uint8_t pg[BW_DTR_PAGE];
    int region;
    rec_open(b, BW_DTR_SNAP);
    b_u32(b, (uint32_t)s_frame);
    b_u32(b, (uint32_t)s_div);
    b_u32(b, why);
    b_bytes(b, live, sizeof *live);
    b_u32(b, 2);
    if (model) {
        uint32_t i;
        for (i = 0; i < BW_DTR_MODEL_PAGES; i++) {
            free(t->page[i]);
            t->page[i] = NULL;
        }
    }
    for (region = 0; region < 2; region++) {
        int which = t->idx * 2 + region;
        uint32_t entries = s_d ? bw_xdsp_peek_page(s_d, which, 0, pg) : 0, e, n = 0;
        size_t at_n;
        b_u32(b, (uint32_t)region);
        b_u32(b, entries);
        at_n = b->len;
        b_u32(b, 0);
        for (e = 0; e < entries && e < BW_DTR_MODEL_PAGES; e++) {
            bw_xdsp_peek_page(s_d, which, e, pg);
            if (page_zero(pg))
                continue;
            b_u32(b, e);
            b_bytes(b, pg, BW_DTR_PAGE);
            n++;
            if (model && region == BW_DTR_REGION_SCRATCH) {
                t->page[e] = (uint8_t *)malloc(BW_DTR_PAGE);
                memcpy(t->page[e], pg, BW_DTR_PAGE);
            }
        }
        memcpy(b->p + at_n, &n, 4);
    }
    rec_close(b);
}

/* A snapshot file of its own (a reset-to-run edge, or asked for). */
static void snap_file(Tr *t, uint32_t why, const BwDspState *live)
{
    char path[MAX_PATH + 80];
    BwDtrHeader h;
    HANDLE f;
    Buf b = { 0 };
    if (why == BW_DTR_WHY_EDGE)
        _snprintf(path, sizeof path, "%s\\%s_p%lu_edge%d_f%ld.snap", s_dir, t->name,
                  GetCurrentProcessId(), t->edges, s_frame);
    else if (why == BW_DTR_WHY_PLOAD)
        _snprintf(path, sizeof path, "%s\\%s_p%lu_pload%d_f%ld.snap", s_dir, t->name,
                  GetCurrentProcessId(), t->ploads, s_frame);
    else
        _snprintf(path, sizeof path, "%s\\%s_p%lu_snap_f%ld.snap", s_dir, t->name,
                  GetCurrentProcessId(), s_frame);
    path[sizeof path - 1] = 0;
    header(&h, t, 1);
    f = open_file(path, &h);
    if (!f)
        return;
    put_snap(t, &b, why, live, 0);
    tlog("[DSP-TRACE] %s snapshot (%s) at frame %ld, pc 0x%04X: %s\n",
            t->idx ? "EP" : "GP", why == BW_DTR_WHY_EDGE ? "reset-to-run edge"
                                  : why == BW_DTR_WHY_PLOAD ? "after its own P load" : "asked",
            s_frame, live->pc, path);
    enqueue(f, &b, 1);
}

/* ---- the trace ------------------------------------------------------------- */

static void trace_open(Tr *t, BwDsp *dsp)
{
    char path[MAX_PATH + 40];
    BwDtrHeader h;
    _snprintf(path, sizeof path, "%s\\%s_p%lu.dtr", s_dir, t->name, GetCurrentProcessId());
    path[sizeof path - 1] = 0;
    header(&h, t, 0);
    t->file = open_file(path, &h);
    if (!t->file)
        return;
    t->open = 1;
    t->runs = 0;
    t->raw = 0;
    t->open_frame = s_frame;
    t->pending_ev |= BW_DTR_RUN_FIRST;
    bw_dsp_get_state(dsp, t->model);
    put_snap(t, &t->buf, BW_DTR_WHY_TRACE, t->model, 1);
    tlog("[DSP-TRACE] %s trace open at frame %ld (pc 0x%04X): %s\n",
            t->idx ? "EP" : "GP", s_frame, t->model->pc, path);
}

static void flush(Tr *t, int close)
{
    t->raw += t->buf.len;
    enqueue(t->file, &t->buf, close);
}

/* The writer thread, every 200 ms with nothing queued: a trace's records
 * idle for 200 ms between runs go to the file now. */
static void steal_idle(void)
{
    int i;
    for (i = 0; i < 2; i++) {
        Tr *t = &s_tr[i];
        if (!TryAcquireSRWLockExclusive(&t->lk))
            continue;
        if (t->open && !t->in_run && t->buf.len && GetTickCount64() - t->touched >= 200 &&
            s_q_bytes < QUEUE_MAX / 2)
            flush(t, 0);
        ReleaseSRWLockExclusive(&t->lk);
    }
}

static void trace_close(Tr *t, const char *why)
{
    AcquireSRWLockExclusive(&t->lk);
    if (!t->open) {
        ReleaseSRWLockExclusive(&t->lk);
        return;
    }
    rec_open(&t->buf, BW_DTR_CLOSE);
    b_u32(&t->buf, t->runs);
    b_u32(&t->buf, 0);
    rec_close(&t->buf);
    flush(t, 1);
    t->open = 0;
    t->file = NULL;
    ReleaseSRWLockExclusive(&t->lk);
    tlog("[DSP-TRACE] %s trace closed at frame %ld (%s): %u runs from frame %ld,"
                    " %.1f MB of records; %u runs so far changed their own P\n",
            t->idx ? "EP" : "GP", s_frame, why, t->runs, t->open_frame, t->raw / 1048576.0,
            t->pruns);
}

/* Runs of differing words between a and b (n words), as a DMEM record. */
static void put_dmem(Buf *b, char space, const uint32_t *a, const uint32_t *c, uint32_t n)
{
    uint32_t i = 0, runs = 0;
    size_t at_runs = 0;
    int opened = 0;
    while (i < n) {
        uint32_t s, e, gap;
        if (a[i] == c[i]) {
            i++;
            continue;
        }
        s = i;
        e = i + 1;
        /* extend over gaps of up to 3 equal words (a run header is 2 words) */
        for (;;) {
            while (e < n && a[e] != c[e])
                e++;
            for (gap = 0; e + gap < n && gap < 3 && a[e + gap] == c[e + gap]; gap++)
                ;
            if (e + gap < n && gap < 3)
                e += gap;
            else
                break;
        }
        if (!opened) {
            rec_open(b, BW_DTR_DMEM);
            b_u32(b, (uint32_t)space);
            at_runs = b->len;
            b_u32(b, 0);
            opened = 1;
        }
        b_u32(b, s);
        b_u32(b, e - s);
        b_bytes(b, c + s, (size_t)(e - s) * 4);
        runs++;
        i = e;
    }
    if (opened) {
        memcpy(b->p + at_runs, &runs, 4);
        rec_close(b);
    }
}

static void put_delta(Buf *b, const BwDspState *old, const BwDspState *cur)
{
    const uint32_t *o = (const uint32_t *)old, *c = (const uint32_t *)cur;
    uint32_t i, n = 0;
    size_t at_n = 0;
    for (i = 0; i < BW_DSP_CORE_WORDS; i++) {
        if (o[i] == c[i])
            continue;
        if (!n) {
            rec_open(b, BW_DTR_DCORE);
            at_n = b->len;
            b_u32(b, 0);
        }
        b_u32(b, i);
        b_u32(b, c[i]);
        n++;
    }
    if (n) {
        memcpy(b->p + at_n, &n, 4);
        rec_close(b);
    }
    put_dmem(b, 'R', old->periph, cur->periph, 128);
    put_dmem(b, 'X', old->x, cur->x, 4096);
    put_dmem(b, 'Y', old->y, cur->y, 2048);
    put_dmem(b, 'P', old->p, cur->p, 4096);
    put_dmem(b, 'M', old->mix, cur->mix, 1024);
}

/* The scratch model: byte addresses in the DSP's scratch space, 4 KB pages
 * made on first touch (zeros). Beyond 16 MB it reads zeros and keeps
 * nothing. The replayer's model is the same code (tools/dsp_replay). */
static void model_read(Tr *t, uint32_t addr, uint8_t *out, size_t len)
{
    while (len) {
        uint32_t pg = addr / BW_DTR_PAGE, off = addr % BW_DTR_PAGE;
        size_t n = BW_DTR_PAGE - off < len ? BW_DTR_PAGE - off : len;
        if (pg < BW_DTR_MODEL_PAGES && t->page[pg])
            memcpy(out, t->page[pg] + off, n);
        else
            memset(out, 0, n);
        out += n;
        addr += (uint32_t)n;
        len -= n;
    }
}

static void model_write(Tr *t, uint32_t addr, const uint8_t *in, size_t len)
{
    while (len) {
        uint32_t pg = addr / BW_DTR_PAGE, off = addr % BW_DTR_PAGE;
        size_t n = BW_DTR_PAGE - off < len ? BW_DTR_PAGE - off : len;
        if (pg < BW_DTR_MODEL_PAGES) {
            if (!t->page[pg])
                t->page[pg] = (uint8_t *)calloc(1, BW_DTR_PAGE);
            memcpy(t->page[pg] + off, in, n);
        }
        in += n;
        addr += (uint32_t)n;
        len -= n;
    }
}

/* A scratch read: the bytes that differ from the model (runs closer than 8
 * equal bytes are joined), then the model takes the bytes read. */
static void put_sread(Tr *t, const uint8_t *data, uint32_t addr, size_t len)
{
    static uint8_t *m;
    static size_t m_cap;
    Buf *b = &t->buf;
    size_t i = 0, at_runs, s, e;
    uint32_t runs = 0;
    if (len > m_cap) {
        m_cap = len;
        m = (uint8_t *)realloc(m, m_cap);
    }
    model_read(t, addr, m, len);
    rec_open(b, BW_DTR_SREAD);
    b_u32(b, addr);
    b_u32(b, (uint32_t)len);
    at_runs = b->len;
    b_u32(b, 0);
    if (memcmp(data, m, len)) {
        while (i < len) {
            if (data[i] == m[i]) {
                i++;
                continue;
            }
            s = i;
            e = i + 1;
            for (;;) {
                size_t gap = 0;
                while (e < len && data[e] != m[e])
                    e++;
                while (e + gap < len && gap < 8 && data[e + gap] == m[e + gap])
                    gap++;
                if (e + gap < len && gap < 8)
                    e += gap;
                else
                    break;
            }
            b_u32(b, (uint32_t)s);
            b_u32(b, (uint32_t)(e - s));
            b_bytes(b, data + s, e - s);
            runs++;
            i = e;
        }
        model_write(t, addr, data, len);
    }
    memcpy(b->p + at_runs, &runs, 4);
    rec_close(b);
}

static void put_swrite(Tr *t, const uint8_t *data, uint32_t addr, size_t len)
{
    Buf *b = &t->buf;
    rec_open(b, BW_DTR_SWRITE);
    b_u32(b, addr);
    b_u32(b, (uint32_t)len);
    b_u64(b, bw_dtr_hash_bytes(data, len));
    if (s_write_data)
        b_bytes(b, data, len);
    rec_close(b);
    model_write(t, addr, data, len);
}

static void put_fifo(Tr *t, uint8_t type, const uint8_t *data, uint32_t index, size_t len)
{
    Buf *b = &t->buf;
    rec_open(b, type);
    b_u32(b, index);
    b_u32(b, (uint32_t)len);
    b_bytes(b, data, len);
    rec_close(b);
}

/* ---- the hooks (on the thread that runs the DSPs) ------------------------- */

static void hk_io(BwDsp *dsp, int kind, uint8_t *ptr, uint32_t where, size_t len, bool dir)
{
    Tr *t = tr_of(dsp);
    if (!t->open || !t->in_run)
        return;                         /* a bootstrap's load: in the next delta */
    if (kind == 0) {
        if (dir)
            put_swrite(t, ptr, where, len);
        else
            put_sread(t, ptr, where, len);
    } else {
        put_fifo(t, dir ? BW_DTR_FWRITE : BW_DTR_FREAD, ptr, where, len);
    }
}

/* What changed in X, Y and P from outside since the DSP's last run
 * (DirectSound's window words, resets, bootstraps), whether or not a trace is
 * open: logged, and with BW_DSP_TRACE_START=xy the trigger of the window. */
static void outside_check(Tr *t, BwDsp *dsp)
{
    static const char sp[3] = { 'X', 'Y', 'P' };
    static const uint32_t words[3] = { 4096, 2048, 4096 };
    uint32_t k, w, off = 0, n[3] = { 0, 0, 0 }, lo[3] = { 0, 0, 0 }, hi[3] = { 0, 0, 0 };
    if (!t->have_prev)
        return;
    for (k = 0; k < 3; k++) {
        const uint32_t *m = bw_dsp_mem(dsp, sp[k], &w);
        uint32_t i;
        if (memcmp(m, t->prev + off, words[k] * 4))
            for (i = 0; i < words[k]; i++)
                if (m[i] != t->prev[off + i]) {
                    if (!n[k])
                        lo[k] = i;
                    hi[k] = i;
                    n[k]++;
                }
        off += words[k];
    }
    if (!n[0] && !n[1] && !n[2])
        return;
    t->outside_runs++;
    if (t->outside_runs <= 64)
        tlog("[DSP-TRACE] %s: from outside before its run at frame %ld: X %u words"
                        " (0x%04X-0x%04X), Y %u (0x%04X-0x%04X), P %u (0x%04X-0x%04X)%s\n",
                t->idx ? "EP" : "GP", s_frame, n[0], lo[0], hi[0], n[1], lo[1], hi[1], n[2],
                lo[2], hi[2], t->outside_runs == 64 ? " (no more of these lines)" : "");
    if (s_start_xy && t->idx == 0 && s_window == 0 && s_frame >= s_xy_after && (n[0] || n[1])) {
        s_start = s_frame;
        s_window = 1;
        tlog("[DSP-TRACE] the window opens: the GP's X/Y memory changed from outside"
                        " at frame %ld (BW_DSP_TRACE_START=xy)\n", s_frame);
    }
}

static void hk_run_begin(BwDsp *dsp, uint32_t max_cycles)
{
    Tr *t = tr_of(dsp);
    uint32_t pw;
    outside_check(t, dsp);
    if (t->edge_pending || t->asked_pending || t->pload_pending) {
        bw_dsp_get_state(dsp, t->live);
        if (t->edge_pending)
            snap_file(t, BW_DTR_WHY_EDGE, t->live);
        if (t->pload_pending)
            snap_file(t, BW_DTR_WHY_PLOAD, t->live);
        if (t->asked_pending)
            snap_file(t, BW_DTR_WHY_ASKED, t->live);
        t->edge_pending = t->asked_pending = t->pload_pending = 0;
    }
    memcpy(t->p_before, bw_dsp_mem(dsp, 'P', &pw), sizeof(uint32_t) * 4096);
    AcquireSRWLockExclusive(&t->lk);
    if (!t->open && s_window == 1)
        trace_open(t, dsp);
    if (!t->open) {
        ReleaseSRWLockExclusive(&t->lk);
        return;
    }
    if (t->in_run) {
        rec_open(&t->buf, BW_DTR_NOTE);
        b_bytes(&t->buf, "previous run never ended (exception?)", 37);
        rec_close(&t->buf);
    }
    bw_dsp_get_state(dsp, t->live);
    rec_open(&t->buf, BW_DTR_RUN);
    b_u32(&t->buf, t->runs);
    b_u32(&t->buf, (uint32_t)s_frame);
    b_u32(&t->buf, (uint32_t)s_div);
    b_u32(&t->buf, max_cycles);
    b_u32(&t->buf, t->pending_ev);
    rec_close(&t->buf);
    t->pending_ev = 0;
    put_delta(&t->buf, t->model, t->live);
    {
        BwDspState *x = t->model;
        t->model = t->live;
        t->live = x;
    }
    t->in_run = 1;                      /* buf is this thread's until run_end */
    ReleaseSRWLockExclusive(&t->lk);
}

static void hk_run_end(BwDsp *dsp, uint32_t used)
{
    Tr *t = tr_of(dsp);
    BwDtrHashes h;
    uint32_t pw, *p = bw_dsp_mem(dsp, 'P', &pw), i, first = 0, last = 0, n = 0;
    memcpy(t->prev, bw_dsp_mem(dsp, 'X', &pw), 4096 * 4);
    memcpy(t->prev + 4096, bw_dsp_mem(dsp, 'Y', &pw), 2048 * 4);
    memcpy(t->prev + 6144, p, 4096 * 4);
    t->have_prev = 1;
    for (i = 0; i < 4096; i++)
        if (p[i] != t->p_before[i]) {
            if (!n)
                first = i;
            last = i;
            n++;
        }
    if (n) {
        /* open question 1: does P change while a program runs? (seen: the EP
         * writes a few P words in every run; the GP loads its effect code) */
        t->pruns++;
        if (t->pruns <= 8)
            tlog("[DSP-TRACE] %s's run at frame %ld changed its own P memory: %u words"
                            " in 0x%04X-0x%04X%s\n", t->idx ? "EP" : "GP", s_frame, n, first,
                    last, t->pruns == 8 ? " (no more of these lines; a count at the close)" : "");
        /* a snapshot after a load of code (not the EP's few data words) */
        if (n >= 64 && t->ploads < 8) {
            t->ploads++;
            t->pload_pending = 1;
        }
    }
    if (!t->open || !t->in_run)
        return;
    AcquireSRWLockExclusive(&t->lk);
    t->in_run = 0;
    bw_dsp_get_state(dsp, t->live);
    bw_dtr_hash_state(t->live, &h);
    rec_open(&t->buf, BW_DTR_END);
    b_u32(&t->buf, used);
    b_u32(&t->buf, t->live->is_idle);
    b_u64(&t->buf, h.core);
    b_u64(&t->buf, h.x);
    b_u64(&t->buf, h.y);
    b_u64(&t->buf, h.p);
    b_u64(&t->buf, h.m);
    rec_close(&t->buf);
    if (t->key_every && t->runs % t->key_every == 0) {
        rec_open(&t->buf, BW_DTR_KEY);
        b_u32(&t->buf, t->runs);
        b_bytes(&t->buf, t->live, sizeof *t->live);
        rec_close(&t->buf);
    }
    {
        BwDspState *x = t->model;
        t->model = t->live;
        t->live = x;
    }
    t->runs++;
    if (t->buf.len >= FLUSH_AT)
        flush(t, 0);
    t->touched = GetTickCount64();
    ReleaseSRWLockExclusive(&t->lk);
}

static void hk_event(BwDsp *dsp, int ev)
{
    Tr *t = tr_of(dsp);
    if (ev == BW_DSP_EV_BOOT) {
        t->edge_pending = 1;
        t->edges++;
        if (t->idx == 0 && s_start < 0 && !s_start_xy && !s_start_file[0]) {
            s_start = s_frame;          /* the default window: from the GP's boot, */
            if (s_window == 0)
                s_window = 1;           /* starting with this very run */
        }
    }
    t->pending_ev |= ev == BW_DSP_EV_BOOT ? BW_DTR_RUN_BOOT : BW_DTR_RUN_RESET;
    AcquireSRWLockExclusive(&t->lk);
    if (t->open && !t->in_run) {
        rec_open(&t->buf, BW_DTR_EVENT);
        b_u32(&t->buf, (uint32_t)s_frame);
        b_u32(&t->buf, (uint32_t)ev);
        rec_close(&t->buf);
        t->touched = GetTickCount64();
    }
    ReleaseSRWLockExclusive(&t->lk);
}

static const BwDspHooks s_hooks = { hk_io, hk_run_begin, hk_run_end, hk_event };

void bw_dsp_trace_frame(long frame, int div)
{
    int i;
    if (!s_on)
        return;
    s_frame = frame;
    s_div = div;
    if (s_nwatch)
        watch_frame();
    if (s_window == 0 && s_start_file[0] && frame % 150 == 0 &&
        GetFileAttributesA(s_start_file) != INVALID_FILE_ATTRIBUTES) {
        s_start = frame;                /* a test tool made the file: open now */
        tlog("[DSP-TRACE] the window opens: %s appeared (frame %ld)\n", s_start_file,
                frame);
    }
    if (s_window == 0 && s_start >= 0 && frame >= s_start)
        s_window = 1;                   /* each DSP opens at its next run */
    if (s_window == 1 && frame >= s_start + s_len) {
        trace_close(&s_tr[0], "window ended");
        trace_close(&s_tr[1], "window ended");
        s_window = 2;
    }
    for (i = 0; i < s_nsnap; i++)
        if (s_snap_at[i] >= 0 && frame >= s_snap_at[i]) {
            s_tr[0].asked_pending = s_tr[1].asked_pending = 1;
            s_snap_at[i] = -1;
        }
}

void bw_dsp_trace_init(MCPXAPUState *d)
{
    static const char *const keep[] = {
        "BW_EDITION", "BW_SEED", "BW_LAUNCH_DATA_FILE", "BW_APU_DSP", "BW_APU_DSP_THREAD",
        "BW_AUDIO_SURROUND", "BW_AUDIO_SPEAKERS", "BW_FIX", "BW_DSP_TRACE_START",
        "BW_DSP_TRACE_LEN", "BW_DSP_TRACE_KEY", "BW_DSP_TRACE_WRITES", "BW_DSP_TRACE_WATCH", NULL
    };
    const char *e = getenv("BW_DSP_TRACE");
    char exe[MAX_PATH] = "";
    size_t n;
    int i;
    if (s_on || !e || !*e)
        return;
    s_d = d;
    _snprintf(s_dir, sizeof s_dir - 1, "%s", e);
    CreateDirectoryA(s_dir, NULL);
    if ((e = getenv("BW_DSP_TRACE_START")) && *e) {
        if (e[0] == 'x') {
            const char *at = strchr(e, '@');
            s_start_xy = 1;
            s_xy_after = at ? parse_frames(at + 1) : 0;
        } else if (!strncmp(e, "file:", 5)) {
            _snprintf(s_start_file, sizeof s_start_file - 1, "%s", e + 5);
            DeleteFileA(s_start_file);
        } else {
            s_start = parse_frames(e);
        }
    }
    if ((e = getenv("BW_DSP_TRACE_WATCH")) && *e)
        watch_parse(e);
    if ((e = getenv("BW_DSP_TRACE_LEN")) && *e)
        s_len = parse_frames(e);
    if ((e = getenv("BW_DSP_TRACE_KEY")) && *e)
        s_key_every = atoi(e);
    s_write_data = (e = getenv("BW_DSP_TRACE_WRITES")) && e[0] == 'd';
    if ((e = getenv("BW_DSP_TRACE_SNAP")) && *e) {
        const char *p = e;
        while (*p && s_nsnap < 32) {
            s_snap_at[s_nsnap++] = parse_frames(p);
            p = strchr(p, ',');
            if (!p)
                break;
            p++;
        }
    }
    s_tr[0].key_every = (uint32_t)s_key_every;
    s_tr[1].key_every = (uint32_t)(s_key_every > 0 ? (s_key_every + 7) / 8 : 0);
    for (i = 0; i < 2; i++) {
        s_tr[i].model = (BwDspState *)calloc(1, sizeof(BwDspState));
        s_tr[i].live = (BwDspState *)calloc(1, sizeof(BwDspState));
        s_tr[i].p_before = (uint32_t *)calloc(4096, sizeof(uint32_t));
        s_tr[i].prev = (uint32_t *)calloc(4096 + 2048 + 4096, sizeof(uint32_t));
    }
    GetModuleFileNameA(NULL, exe, sizeof exe);
    n = (size_t)_snprintf(s_text, sizeof s_text - 1,
                          "Blood Wake DSP trace (BW_DSP_TRACE)\nexe=%s\nedition=%s\n", exe,
                          bw_edition_name(bw_edition()));
    for (i = 0; keep[i] && n < sizeof s_text - 200; i++) {
        const char *v = getenv(keep[i]);
        if (v)
            n += (size_t)_snprintf(s_text + n, sizeof s_text - 1 - n, "%s=%s\n", keep[i], v);
    }
    s_writer = CreateThread(NULL, 0, writer_thread, NULL, 0, NULL);
    s_on = 1;
    bw_dsp_set_hooks(&s_hooks);
    tlog("[DSP-TRACE] on: %s; window %s%s%lld + %lld frames; KEY every %d GP runs;"
                    " scratch writes %s\n", s_dir, s_start_file[0] ? s_start_file : "",
            s_start_file[0] ? " appearing, "
            : s_start_xy ? "from the GP's first X/Y change from outside after frame "
                         : s_start < 0 ? "from the GP's boot, " : "frame ",
            s_start_xy ? s_xy_after : s_start < 0 ? 0LL : s_start, s_len, s_key_every,
            s_write_data ? "with data" : "hashed");
}
