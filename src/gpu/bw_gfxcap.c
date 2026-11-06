/*
 * bw_gfxcap.c - the GPU frame capture and its replay in this executable
 * (BW_GFXCAP, BW_GFXCAP_REPLAY; lane gfx-cap, docs/gfx/capture.md, which
 * describes the format). A test aid in every edition; unset, nothing here
 * runs (g_bw_gfxcap stays 0).
 *
 * Recording: the file opens at the first method the executor gets (the
 * process's first: D3D's device initialisation), so the executor and the
 * back end start the replay in the state they started the live run in.
 * Everything is written on the NV2A thread in stream order; full chunks go to
 * a writer thread, which compresses them (XPRESS Huffman, the Windows
 * compression API) and writes them.
 *
 * Order: a method's reads happen while it runs (an upload during
 * SET_BEGIN_END 0, the ramp during FLIP_STALL). The newest method is held
 * back until the next one arrives, so every record its run made is written
 * before it. The replay patches memory, then runs the method.
 *
 * The memory model: 4 KB pages over [0, 64 MB) and the contiguous window
 * [0x80000000, +64 MB). A page is unknown until the GPU side first reads it;
 * that read records the page whole (PAGE). Later reads record the runs that
 * differ from the model (DIFF) and update it. No snapshot: a read the hooks
 * miss finds the replay's own memory and shows as a different picture.
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <compressapi.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bw_gfxcap.h"
#include "bw_gfxcls.h"             /* BW_GFXCAP_CLASSES: the replay's region masks (lane gfx-capef) */
#include "core/bwg_gate.h"         /* gfx-lat11: the latency mode's back-end side in a replay */

extern ptrdiff_t g_xbox_mem_offset;
#define GUEST(va) ((uint8_t *)((uintptr_t)(uint32_t)(va) + g_xbox_mem_offset))

/* the executor and the gamma ramp, for the replay */
extern void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param);
extern int nv2a_pb_exec_idx16(const uint32_t *p, uint32_t n);
extern void nv2a_pb_set_semaphore_target(uint32_t guest_va);
extern void bw_gamma_from_title(const uint8_t *ramp);
extern void xbox_FramebufferWindowStart(void);   /* the toolkit's window (RECOMP_FB_WINDOW) */
extern void *bw_fb_window(void);

volatile int g_bw_gfxcap;

/* ---- the format ---------------------------------------------------------- */
#define GC_MAGIC "BWGCAP01"
#define GC_VERSION 1u
enum {
    GC_METHODS = 1,     /* u32 n, n x (u32 subch << 16 | method, u32 param) */
    GC_IDX16 = 2,       /* u32 n, n x u32 */
    GC_PAGE = 3,        /* u32 va, 4096 bytes */
    GC_DIFF = 4,        /* u32 va, bytes to the record's end */
    GC_GAMMA = 5,       /* u32 seq, 768 bytes */
    GC_SEM = 6,         /* u32 va */
    GC_FLIP = 7,        /* u32 gen, va, w, h, bpp, u64 hash */
    GC_NOTE = 8,        /* text */
    GC_OPEN = 9,        /* u32 why (0: the process's first method; 1: mid-scene, then
                         * u32 gen, hwm, sem, surfaces, state bytes, the state block,
                         * u32 preamble methods, the methods as METHODS pairs) */
    GC_CLOSE = 10,      /* u32 flips, u32 why */
    GC_CONTIG = 11,     /* u32 the contiguous allocator's high-water mark */
    GC_BLOCK = 12,      /* u32 va, u32 bytes from va to its block's end (0: none) */
    GC_PRESENT = 13,    /* u32 gen, float dt: the suite presented (its clock, OLED care) */
    GC_SURF = 14,       /* a mid-scene OPEN's surface: BwGcSurf (96 bytes); its rows follow */
    GC_SURFROWS = 15,   /* u32 va, u32 first row, u32 rows, the rows (row_bytes each) */
    GC_LOCK = 16,       /* lane gfx-regen, I1: a lock the latch drained: u32 va, pitch, flags,
                         * has_rect, i32 rect[4] (bwg_latch.c) */
    GC_LATCHSHOW = 17   /* I1: u32 va: that surface's marked rows uploaded and presented
                         * again at the graphics thread's idle point */
};
extern uint32_t xbox_ContiguousAllocatedBytes(void);    /* the runtime's allocator (dma_resolve) */
typedef struct {
    char magic[8];
    uint32_t version, flags, pid, text_len;
    uint32_t reserved[10];
} GcHeader;                                 /* 64 bytes */
typedef struct {
    uint32_t raw_len, stored_len, codec, check;
} GcChunk;
enum { GC_STORED = 0, GC_XPRESS = 1 };

#define GC_PAGE_SZ 4096u
#define GC_R0_END (64u << 20)
#define GC_R1_BASE 0x80000000u
#define GC_R1_END (GC_R1_BASE + (64u << 20))
#define GC_NPAGES ((64u << 20) / GC_PAGE_SZ * 2u)
#define GC_UNREADABLE ((uint8_t *)(uintptr_t)1)

static uint32_t fnv32(const uint8_t *p, size_t n)
{
    uint32_t h = 2166136261u;
    size_t i;
    for (i = 0; i < n; i++)
        h = (h ^ p[i]) * 16777619u;
    return h;
}

/* The flip hash: 8 bytes a step (FNV-style, with a shift so high bits mix
 * down), the rows in order. */
static uint64_t rows_hash(const uint8_t *base, uint32_t pitch, uint32_t row, uint32_t rows)
{
    uint64_t h = 0xCBF29CE484222325ull ^ ((uint64_t)row << 32) ^ rows;
    uint32_t y, x;
    for (y = 0; y < rows; y++) {
        const uint8_t *p = base + (size_t)y * pitch;
        for (x = 0; x + 8 <= row; x += 8) {
            uint64_t v;
            memcpy(&v, p + x, 8);
            h = (h ^ v) * 0x100000001B3ull;
            h ^= h >> 29;
        }
        for (; x < row; x++) {
            h = (h ^ p[x]) * 0x100000001B3ull;
            h ^= h >> 29;
        }
    }
    return h;
}

static int page_index(uint32_t va)
{
    if (va < GC_R0_END)
        return (int)(va / GC_PAGE_SZ);
    if (va >= GC_R1_BASE && va < GC_R1_END)
        return (int)((GC_R0_END + (va - GC_R1_BASE)) / GC_PAGE_SZ);
    return -1;
}

static int host_readable(const void *p)
{
    MEMORY_BASIC_INFORMATION mbi;
    DWORD pr;
    if (!VirtualQuery(p, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT)
        return 0;
    pr = mbi.Protect & 0xFF;
    if (mbi.Protect & PAGE_GUARD)
        return 0;
    return pr == PAGE_READONLY || pr == PAGE_READWRITE || pr == PAGE_WRITECOPY
        || pr == PAGE_EXECUTE_READ || pr == PAGE_EXECUTE_READWRITE || pr == PAGE_EXECUTE_WRITECOPY;
}

/* ---- switches --------------------------------------------------------------- */
static char s_dir[MAX_PATH];        /* BW_GFXCAP: where the capture goes */
static char s_replay[MAX_PATH];     /* BW_GFXCAP_REPLAY: the capture to replay */
static char s_outdir[MAX_PATH];     /* where shots and the verdict go */
static uint32_t s_limit = 600;      /* BW_GFXCAP_FLIPS: close after this many flips */
#define GC_MAX_SHOTS 32
static uint32_t s_shots[GC_MAX_SHOTS], s_nshots;
/* BW_GFXCAP_TOUCH=<file>@<flip> (replay): make that file when the replay
 * reaches the flip, so a flag-file aid (RECOMP_FRAME_TRACE, ...) traces it */
static char s_touch[MAX_PATH];
static uint32_t s_touch_gen;
/* BW_GFXCAP_PICS=<flips> (both sides): the suite's output picture at those
 * flips (Remastered; bw_vfx_post.c writes it) */
static uint32_t s_pics[GC_MAX_SHOTS], s_npics;
static volatile uint32_t s_frame_gen;     /* the back end's flip in progress */

/* Step B: the mid-scene open (bw_gfxcap.h) */
static uint32_t s_start_gen;            /* BW_GFXCAP_START=<flip>: the first captured flip */
static char s_start_file[MAX_PATH];     /* BW_GFXCAP_START=file:<path> */
static int s_start_wait;                /* recording waits for the start */
static uint32_t s_mid_gen;              /* BW_GFXCAP_MIDOPEN=<flip> */
static int s_mid_done;
static uint32_t s_open_gen;             /* recording: a mid-scene open is due at the next method */
static int s_from_open, s_use_preamble, s_ignore_drop;     /* replay modes */
static const BwGfxcapBackend *s_be;

void bw_gfxcap_backend(const BwGfxcapBackend *b)
{
    s_be = b;
}

static void parse_flips(const char *e, uint32_t *out, uint32_t *n)
{
    while (e && *e && *n < GC_MAX_SHOTS) {
        out[(*n)++] = (uint32_t)strtoul(e, (char **)&e, 10);
        if (*e != ',')
            break;
        e++;
    }
}

void bw_gfxcap_init(void)
{
    const char *d = getenv("BW_GFXCAP"), *r = getenv("BW_GFXCAP_REPLAY"), *e;
    if ((e = getenv("BW_GFXCAP_FLIPS")) != NULL && atoi(e) > 0)
        s_limit = (uint32_t)atoi(e);
    parse_flips(getenv("BW_GFXCAP_SHOTS"), s_shots, &s_nshots);
    parse_flips(getenv("BW_GFXCAP_PICS"), s_pics, &s_npics);
    if ((e = getenv("BW_GFXCAP_TOUCH")) != NULL && strrchr(e, '@')) {
        const char *at = strrchr(e, '@');
        snprintf(s_touch, sizeof s_touch, "%.*s", (int)(at - e), e);
        s_touch_gen = (uint32_t)strtoul(at + 1, NULL, 10);
    }
    if (r && *r) {
        snprintf(s_replay, sizeof s_replay, "%s", r);
        e = getenv("BW_GFXCAP_OUT");
        if (e && *e)
            snprintf(s_outdir, sizeof s_outdir, "%s", e);
        else {
            char *slash;
            snprintf(s_outdir, sizeof s_outdir, "%s", r);
            slash = strrchr(s_outdir, '\\');
            if (!slash || strrchr(s_outdir, '/') > slash)
                slash = strrchr(s_outdir, '/');
            if (slash)
                *slash = 0;
            else
                strcpy(s_outdir, ".");
        }
        CreateDirectoryA(s_outdir, NULL);
        s_from_open = (e = getenv("BW_GFXCAP_REPLAY_FROM")) != NULL && !strcmp(e, "open");
        s_use_preamble = (e = getenv("BW_GFXCAP_STATE")) != NULL && !strcmp(e, "preamble");
        s_ignore_drop = (e = getenv("BW_GFXCAP_IGNORE_DROP")) != NULL && e[0] == '1';
        g_bw_gfxcap = 2;
        bw_gfxcls_init(s_outdir);       /* BW_GFXCAP_CLASSES (region masks), if set */
        fprintf(stderr, "[GFXCAP] replay of %s%s: the game will not run; results in %s\n",
                s_replay, s_from_open ? (s_use_preamble ? " from its mid-scene open (state: the preamble)"
                                                        : " from its mid-scene open (state: the block)") : "",
                s_outdir);
    } else if (d && *d) {
        snprintf(s_dir, sizeof s_dir, "%s", d);
        snprintf(s_outdir, sizeof s_outdir, "%s", d);
        CreateDirectoryA(s_dir, NULL);
        if ((e = getenv("BW_GFXCAP_START")) != NULL && *e) {
            if (!strncmp(e, "file:", 5)) {
                snprintf(s_start_file, sizeof s_start_file, "%s", e + 5);
                DeleteFileA(s_start_file);
            } else
                s_start_gen = (uint32_t)strtoul(e, NULL, 10);
            s_start_wait = 1;
        }
        if ((e = getenv("BW_GFXCAP_MIDOPEN")) != NULL && *e)
            s_mid_gen = (uint32_t)strtoul(e, NULL, 10);
        g_bw_gfxcap = 1;
        if (s_start_wait && s_start_file[0])
            fprintf(stderr, "[GFXCAP] recording into %s from the first flip after %s appears, %u flips"
                            " (BW_GFXCAP_START, BW_GFXCAP_FLIPS)\n", s_dir, s_start_file, s_limit);
        else if (s_start_wait)
            fprintf(stderr, "[GFXCAP] recording into %s from flip %u (a mid-scene open), %u flips"
                            " (BW_GFXCAP_START, BW_GFXCAP_FLIPS)\n", s_dir, s_start_gen, s_limit);
        else
            fprintf(stderr, "[GFXCAP] recording into %s from the first GPU method, %u flips"
                            " (BW_GFXCAP_FLIPS)%s\n", s_dir, s_limit,
                    s_mid_gen ? "; a mid-scene open as well (BW_GFXCAP_MIDOPEN)" : "");
    }
    fflush(stderr);
}

static int shot_wanted(uint32_t gen)
{
    uint32_t i;
    for (i = 0; i < s_nshots; i++)
        if (s_shots[i] == gen)
            return 1;
    return 0;
}

void bw_gfxcap_frame(uint32_t gen)
{
    s_frame_gen = gen;
}

int bw_gfxcap_pic_path(char *path, size_t cap)
{
    uint32_t i, gen = s_frame_gen;
    if (!g_bw_gfxcap || !s_npics)
        return 0;
    for (i = 0; i < s_npics; i++)
        if (s_pics[i] == gen) {
            snprintf(path, cap, "%s/pic_%s_f%05u.raw", s_outdir, g_bw_gfxcap == 1 ? "live" : "replay", gen);
            return 1;
        }
    return 0;
}

static void write_shot(const char *who, uint32_t gen, const uint8_t *base, uint32_t pitch,
                       uint32_t row, uint32_t rows, uint32_t bpp)
{
    char path[MAX_PATH + 64];
    FILE *f;
    uint32_t hd[8] = {0}, y;
    snprintf(path, sizeof path, "%s/shot_%s_f%05u.raw", s_outdir, who, gen);
    if (!(f = fopen(path, "wb")))
        return;
    memcpy(hd, "BWSHOT01", 8);
    hd[2] = row / (bpp ? bpp : 1);
    hd[3] = rows;
    hd[4] = bpp;
    hd[5] = gen;
    fwrite(hd, 4, 8, f);
    for (y = 0; y < rows; y++)
        fwrite(base + (size_t)y * pitch, 1, row, f);
    fclose(f);
}

/* ================================================================ recording */
#define GC_CHUNK (8u << 20)
#define GC_QUEUE 8
static FILE *s_f;
static int s_open, s_closed;
static uint8_t *s_buf;
static uint32_t s_len;
static struct { uint8_t *buf; uint32_t len; } s_q[GC_QUEUE];
static int s_qh, s_qn, s_wstop;
static CRITICAL_SECTION s_qcs;
static CONDITION_VARIABLE s_qput, s_qget;
static HANDLE s_wthread;
static uint64_t s_raw_total, s_stored_total;

/* the methods batch; the last entry is held back while s_pend */
#define GC_MB 65536u
static uint32_t s_mb[GC_MB * 2], s_mb_n;
static int s_pend;
static uint32_t s_sem_last;
static uint32_t s_flips;
static int s_close_due;
static uint8_t *s_model[GC_NPAGES];
static struct {
    uint64_t methods, idx16_runs, idx16_words, reads, read_bytes, pages, diffs, diff_bytes,
             gammas, outside;
} s_st;

static DWORD WINAPI writer(LPVOID unused)
{
    COMPRESSOR_HANDLE comp = NULL;
    uint8_t *out = (uint8_t *)malloc(GC_CHUNK + GC_CHUNK / 8 + 4096);
    (void)unused;
    if (!CreateCompressor(COMPRESS_ALGORITHM_XPRESS_HUFF, NULL, &comp))
        comp = NULL;
    for (;;) {
        uint8_t *b;
        uint32_t n;
        GcChunk ch;
        SIZE_T got = 0;
        EnterCriticalSection(&s_qcs);
        while (!s_qn && !s_wstop)
            SleepConditionVariableCS(&s_qget, &s_qcs, INFINITE);
        if (!s_qn && s_wstop) {
            LeaveCriticalSection(&s_qcs);
            break;
        }
        b = s_q[s_qh].buf;
        n = s_q[s_qh].len;
        s_qh = (s_qh + 1) % GC_QUEUE;
        s_qn--;
        WakeConditionVariable(&s_qput);
        LeaveCriticalSection(&s_qcs);
        ch.raw_len = n;
        ch.check = fnv32(b, n);
        if (comp && out && Compress(comp, b, n, out, GC_CHUNK + GC_CHUNK / 8 + 4096, &got)
                && got > 0 && got < n) {
            ch.codec = GC_XPRESS;
            ch.stored_len = (uint32_t)got;
            fwrite(&ch, sizeof ch, 1, s_f);
            fwrite(out, 1, got, s_f);
        } else {
            ch.codec = GC_STORED;
            ch.stored_len = n;
            fwrite(&ch, sizeof ch, 1, s_f);
            fwrite(b, 1, n, s_f);
        }
        fflush(s_f);
        s_raw_total += n;
        s_stored_total += ch.stored_len;
        free(b);
    }
    if (comp)
        CloseCompressor(comp);
    free(out);
    return 0;
}

static void chunk_submit(void)
{
    if (!s_len)
        return;
    EnterCriticalSection(&s_qcs);
    while (s_qn == GC_QUEUE)
        SleepConditionVariableCS(&s_qput, &s_qcs, INFINITE);
    s_q[(s_qh + s_qn) % GC_QUEUE].buf = s_buf;
    s_q[(s_qh + s_qn) % GC_QUEUE].len = s_len;
    s_qn++;
    WakeConditionVariable(&s_qget);
    LeaveCriticalSection(&s_qcs);
    s_buf = (uint8_t *)malloc(GC_CHUNK);
    s_len = 0;
    if (!s_buf) {
        fprintf(stderr, "[GFXCAP] out of memory: the capture stops here\n");
        g_bw_gfxcap = 0;
    }
}

static uint8_t *rec(uint8_t type, uint32_t len)
{
    uint8_t *p;
    if (s_len + 5 + len > GC_CHUNK)
        chunk_submit();
    if (!s_buf)
        return NULL;
    p = s_buf + s_len;
    p[0] = type;
    memcpy(p + 1, &len, 4);
    s_len += 5 + len;
    return p + 5;
}

/* Write the batch's methods; keep the held-back one when asked. */
static void mb_flush(int keep_pending)
{
    uint32_t n = s_mb_n - ((keep_pending && s_pend && s_mb_n) ? 1u : 0u);
    if (n) {
        uint8_t *p = rec(GC_METHODS, 4 + n * 8);
        if (p) {
            memcpy(p, &n, 4);
            memcpy(p + 4, s_mb, (size_t)n * 8);
        }
    }
    if (keep_pending && s_pend && s_mb_n) {
        s_mb[0] = s_mb[2 * (s_mb_n - 1)];
        s_mb[1] = s_mb[2 * (s_mb_n - 1) + 1];
        s_mb_n = 1;
    } else {
        s_mb_n = 0;
        s_pend = 0;
    }
}

static void header_text(char *t, size_t cap)
{
    char exe[MAX_PATH];
    LPCH env = GetEnvironmentStringsA(), e;
    size_t n;
    GetModuleFileNameA(NULL, exe, sizeof exe);
    n = (size_t)snprintf(t, cap, "exe=%s\n", exe);
    for (e = env; e && *e && n < cap; e += strlen(e) + 1)
        if (!strncmp(e, "BW_", 3) || !strncmp(e, "RECOMP_", 7))
            n += (size_t)snprintf(t + n, cap - n, "%s\n", e);
    if (env)
        FreeEnvironmentStringsA(env);
}

static void write_open_mid(uint32_t sem);      /* step B, below */

static void cap_open(uint32_t sem)
{
    char path[MAX_PATH + 32], text[16384];
    GcHeader h;
    uint8_t *p;
    s_open = 1;
    snprintf(path, sizeof path, "%s/gfx_p%lu.gcap", s_dir, (unsigned long)GetCurrentProcessId());
    if (!(s_f = fopen(path, "wb")) || !(s_buf = (uint8_t *)malloc(GC_CHUNK))) {
        fprintf(stderr, "[GFXCAP] could not open %s: no capture\n", path);
        g_bw_gfxcap = 0;
        return;
    }
    header_text(text, sizeof text);
    memset(&h, 0, sizeof h);
    memcpy(h.magic, GC_MAGIC, 8);
    h.version = GC_VERSION;
    h.pid = GetCurrentProcessId();
    h.text_len = (uint32_t)strlen(text);
    fwrite(&h, sizeof h, 1, s_f);
    fwrite(text, 1, h.text_len, s_f);
    InitializeCriticalSection(&s_qcs);
    InitializeConditionVariable(&s_qput);
    InitializeConditionVariable(&s_qget);
    s_wthread = CreateThread(NULL, 0, writer, NULL, 0, NULL);
    fprintf(stderr, "[GFXCAP] capture open: %s\n", path);
    if (s_open_gen)
        write_open_mid(sem);            /* BW_GFXCAP_START: the file starts mid-scene */
    else if ((p = rec(GC_OPEN, 4)) != NULL)
        memset(p, 0, 4);
    fflush(stderr);
}

static void cap_close(uint32_t why)
{
    uint8_t *p;
    if (!s_open || s_closed)
        return;
    s_closed = 1;
    s_pend = 0;
    mb_flush(0);
    if ((p = rec(GC_CLOSE, 8)) != NULL) {
        memcpy(p, &s_flips, 4);
        memcpy(p + 4, &why, 4);
    }
    chunk_submit();
    EnterCriticalSection(&s_qcs);
    s_wstop = 1;
    WakeConditionVariable(&s_qget);
    LeaveCriticalSection(&s_qcs);
    WaitForSingleObject(s_wthread, INFINITE);
    fclose(s_f);
    s_f = NULL;
    g_bw_gfxcap = 0;
    fprintf(stderr, "[GFXCAP] capture closed after %u flips: methods %llu, index runs %llu (%llu words),"
                    " reads %llu (%llu MB looked at), pages %llu (%llu MB), diffs %llu (%llu KB),"
                    " gamma %llu, outside the model %llu; %.1f MB raw, %.1f MB on disk\n",
            s_flips, (unsigned long long)s_st.methods, (unsigned long long)s_st.idx16_runs,
            (unsigned long long)s_st.idx16_words, (unsigned long long)s_st.reads,
            (unsigned long long)(s_st.read_bytes >> 20), (unsigned long long)s_st.pages,
            (unsigned long long)(s_st.pages * GC_PAGE_SZ >> 20), (unsigned long long)s_st.diffs,
            (unsigned long long)(s_st.diff_bytes >> 10), (unsigned long long)s_st.gammas,
            (unsigned long long)s_st.outside, s_raw_total / 1048576.0, s_stored_total / 1048576.0);
    fflush(stderr);
}

void bw_gfxcap_method(uint32_t subch, uint32_t method, uint32_t param, uint32_t sem_va)
{
    if (!s_open) {
        if (s_start_wait && !s_open_gen)
            return;                     /* BW_GFXCAP_START: not yet */
        cap_open(sem_va);
    }
    if (s_close_due) {                  /* the last flip's method is complete now */
        cap_close(0);
        return;
    }
    if (!s_buf)
        return;
    s_pend = 0;                         /* the previous method has run */
    if (s_open_gen) {                   /* BW_GFXCAP_MIDOPEN: the flip's method is complete now */
        mb_flush(0);
        write_open_mid(sem_va);
    }
    if (sem_va != s_sem_last) {
        uint8_t *p;
        mb_flush(0);
        if ((p = rec(GC_SEM, 4)) != NULL)
            memcpy(p, &sem_va, 4);
        s_sem_last = sem_va;
    }
    if (s_mb_n == GC_MB)
        mb_flush(0);
    s_mb[2 * s_mb_n] = (subch << 16) | (method & 0xFFFFu);
    s_mb[2 * s_mb_n + 1] = param;
    s_mb_n++;
    s_pend = 1;
    s_st.methods++;
}

void bw_gfxcap_idx16(const uint32_t *p, uint32_t n)
{
    uint8_t *r;
    if (!s_open || !s_buf || s_closed)
        return;
    if (s_close_due) {
        cap_close(0);
        return;
    }
    s_pend = 0;
    mb_flush(0);
    if ((r = rec(GC_IDX16, 4 + n * 4)) != NULL) {
        memcpy(r, &n, 4);
        memcpy(r + 4, p, (size_t)n * 4);
    }
    s_st.idx16_runs++;
    s_st.idx16_words += n;
}

static void emit_page(uint32_t pva, const uint8_t *m)
{
    uint8_t *r = rec(GC_PAGE, 4 + GC_PAGE_SZ);
    if (r) {
        memcpy(r, &pva, 4);
        memcpy(r + 4, m, GC_PAGE_SZ);
    }
    s_st.pages++;
}

void bw_gfxcap_read(uint32_t va, uint32_t len)
{
    uint64_t end = (uint64_t)va + len;
    uint32_t a;
    if (!s_open || !s_buf || s_closed || !len)
        return;
    if (va < GC_R0_END) {
        if (end > GC_R0_END)
            end = GC_R0_END;
    } else if (va >= GC_R1_BASE && va < GC_R1_END) {
        if (end > GC_R1_END)
            end = GC_R1_END;
    } else {
        if (s_st.outside++ < 8)
            fprintf(stderr, "[GFXCAP] a GPU read at %08X (%u bytes) is outside the model\n", va, len);
        return;
    }
    mb_flush(1);
    s_st.reads++;
    s_st.read_bytes += end - va;
    for (a = va; a < end; ) {
        uint32_t pva = a & ~(GC_PAGE_SZ - 1), off = a - pva;
        uint32_t n = (uint32_t)((end - a) < (uint64_t)(GC_PAGE_SZ - off) ? (end - a) : (GC_PAGE_SZ - off));
        int idx = page_index(pva);
        uint8_t *m = s_model[idx];
        const uint8_t *g = GUEST(pva);
        a += n;
        if (m == GC_UNREADABLE)
            continue;
        if (!m) {
            if (!host_readable(g) || !(m = (uint8_t *)malloc(GC_PAGE_SZ))) {
                s_model[idx] = GC_UNREADABLE;
                continue;
            }
            memcpy(m, g, GC_PAGE_SZ);
            s_model[idx] = m;
            emit_page(pva, m);
            continue;
        }
        if (!memcmp(m + off, g + off, n))
            continue;
        {
            uint32_t i = off, e = off + n;
            while (i < e) {
                uint32_t s, last;
                uint8_t *r;
                while (i < e && m[i] == g[i])
                    i++;
                if (i >= e)
                    break;
                s = last = i;
                while (i < e) {
                    if (m[i] != g[i])
                        last = i++;
                    else if (i - last < 8)
                        i++;
                    else
                        break;
                }
                i = last + 1;
                if ((r = rec(GC_DIFF, 4 + (i - s))) != NULL) {
                    uint32_t dva = pva + s;
                    memcpy(r, &dva, 4);
                    memcpy(r + 4, g + s, i - s);
                    memcpy(m + s, r + 4, i - s);    /* the model holds what was recorded */
                }
                s_st.diffs++;
                s_st.diff_bytes += i - s;
            }
        }
    }
}

static uint32_t s_gamma_last;

void bw_gfxcap_gamma(uint32_t seq, const uint8_t ramp[768])
{
    uint8_t *r;
    if (!s_open || !s_buf || s_closed || !seq || seq == s_gamma_last)
        return;
    s_gamma_last = seq;
    mb_flush(1);
    if ((r = rec(GC_GAMMA, 4 + 768)) != NULL) {
        memcpy(r, &seq, 4);
        memcpy(r + 4, ramp, 768);
    }
    s_st.gammas++;
}

/* ---- lane gfx-regen, I1 (the CPU-write latch, core/bwg_latch.c) ----------- */
#include "core/bwg_latch.h"

/* The method held back has run (the graphics thread is idle between
 * methods): it is written now, so records made here go after it. */
void bw_gfxcap_settle(void)
{
    if (!s_open || !s_buf || s_closed)
        return;
    s_pend = 0;
    mb_flush(0);
}

void bw_gfxcap_lock(uint32_t va, uint32_t pitch, uint32_t flags, uint32_t has_rect,
                    const int32_t r[4], int after)
{
    uint8_t *p;
    if (!s_open || !s_buf || s_closed)
        return;
    if (after) {
        s_pend = 0;
        mb_flush(0);
    } else
        mb_flush(1);
    if ((p = rec(GC_LOCK, 32)) != NULL) {
        memcpy(p, &va, 4);
        memcpy(p + 4, &pitch, 4);
        memcpy(p + 8, &flags, 4);
        memcpy(p + 12, &has_rect, 4);
        memcpy(p + 16, r, 16);
    }
}

void bw_gfxcap_latchshow(uint32_t va)
{
    uint8_t *p;
    if (!s_open || !s_buf || s_closed)
        return;
    s_pend = 0;
    mb_flush(0);
    if ((p = rec(GC_LATCHSHOW, 4)) != NULL)
        memcpy(p, &va, 4);
}

/* The suite's present clock (OLED care accumulates it per cell): an input
 * of the picture that is neither in the stream nor in guest memory. */
static float s_dt_rep;
static int s_dt_have;

float bw_gfxcap_dt(float live)
{
    if (g_bw_gfxcap == 2)
        return s_dt_have ? s_dt_rep : live;
    if (g_bw_gfxcap == 1 && s_open && s_buf && !s_closed) {
        uint8_t *r;
        uint32_t gen = s_frame_gen;
        mb_flush(1);
        if ((r = rec(GC_PRESENT, 8)) != NULL) {
            memcpy(r, &gen, 4);
            memcpy(r + 4, &live, 4);
        }
    }
    return live;
}

/* ---- the runtime's contiguous allocator, as the GPU side asks it ---------- */
#define GC_BLOCKS 64
static uint32_t s_hwm = 0xFFFFFFFFu;                 /* recording: last recorded; replay: current */
static struct { uint32_t va, room; } s_blk[GC_BLOCKS];
static unsigned s_nblk, s_blk_next;

static int blk_find(uint32_t va)
{
    unsigned i;
    for (i = 0; i < s_nblk; i++)
        if (s_blk[i].va == va)
            return (int)i;
    return -1;
}

static void blk_set(uint32_t va, uint32_t room)
{
    int i = blk_find(va);
    if (i < 0) {
        if (s_nblk < GC_BLOCKS)
            i = (int)s_nblk++;
        else {
            i = (int)s_blk_next;
            s_blk_next = (s_blk_next + 1) % GC_BLOCKS;
        }
        s_blk[i].va = va;
    }
    s_blk[i].room = room;
}

uint32_t bw_gfxcap_contig_hwm(uint32_t live)
{
    if (g_bw_gfxcap == 2)
        return s_hwm == 0xFFFFFFFFu ? live : s_hwm;
    if (g_bw_gfxcap == 1 && s_open && s_buf && !s_closed && live != s_hwm) {
        uint8_t *r;
        mb_flush(1);
        if ((r = rec(GC_CONTIG, 4)) != NULL)
            memcpy(r, &live, 4);
        s_hwm = live;
    }
    return live;
}

uint32_t bw_gfxcap_block(uint32_t va, uint32_t live)
{
    int i;
    if (g_bw_gfxcap == 2) {
        i = blk_find(va);
        return i < 0 ? live : s_blk[i].room;
    }
    if (g_bw_gfxcap == 1 && s_open && s_buf && !s_closed) {
        i = blk_find(va);
        if (i < 0 || s_blk[i].room != live) {
            uint8_t *r;
            mb_flush(1);
            if ((r = rec(GC_BLOCK, 8)) != NULL) {
                memcpy(r, &va, 4);
                memcpy(r + 4, &live, 4);
            }
            blk_set(va, live);
        }
    }
    return live;
}

/* ============================================== step B: the mid-scene open */
/* The surfaces the back end handed over at the end of the flip before the
 * open (open_live), written after the OPEN record. */
#define GC_HELD 64
static struct { BwGcSurf h; uint8_t *bytes; } s_held[GC_HELD];
static int s_nheld;

static void held_free(void)
{
    int i;
    for (i = 0; i < s_nheld; i++)
        free(s_held[i].bytes);
    s_nheld = 0;
}

void bw_gfxcap_surface(const BwGcSurf *s, const uint8_t *rows, uint32_t row_pitch)
{
    uint8_t *b;
    uint32_t y;
    if (s_nheld >= GC_HELD || !s->row_bytes || !s->ph
            || !(b = (uint8_t *)malloc((size_t)s->row_bytes * s->ph)))
        return;
    for (y = 0; y < s->ph; y++)
        memcpy(b + (size_t)y * s->row_bytes, rows + (size_t)y * row_pitch, s->row_bytes);
    s_held[s_nheld].h = *s;
    s_held[s_nheld].bytes = b;
    s_nheld++;
}

/* Recording: the end of the back end's flip gen. The mid-scene open is due
 * when the next flip is the start (BW_GFXCAP_START, BW_GFXCAP_MIDOPEN) or the
 * start file has appeared: the back end hands over its surfaces and drops its
 * texture cache now, and the OPEN is written at the next method (the flip's
 * own method is complete then). */
void bw_gfxcap_flip_end(uint32_t gen)
{
    int due = 0;
    if (g_bw_gfxcap != 1 || s_closed || s_open_gen)
        return;
    if (!s_open && s_start_wait) {
        if (s_start_file[0])
            due = GetFileAttributesA(s_start_file) != INVALID_FILE_ATTRIBUTES;
        else
            due = gen + 1 >= s_start_gen;
    } else if (s_open && s_mid_gen && !s_mid_done && gen + 1 >= s_mid_gen) {
        due = 1;
        s_mid_done = 1;
    }
    if (!due)
        return;
    s_open_gen = gen + 1;
    held_free();
    if (s_be && s_be->open_live)
        s_be->open_live();
}

static void write_open_mid(uint32_t sem)
{
    uint32_t why = 1, gen = s_open_gen, hwm = xbox_ContiguousAllocatedBytes(), ns = (uint32_t)s_nheld;
    uint32_t slen = (uint32_t)bw_exec_state_save(NULL), npre = bw_exec_preamble(NULL, 0), i;
    uint8_t *p = rec(GC_OPEN, 6 * 4 + slen + 4 + npre * 8);
    if (p) {
        memcpy(p, &why, 4); memcpy(p + 4, &gen, 4); memcpy(p + 8, &hwm, 4);
        memcpy(p + 12, &sem, 4); memcpy(p + 16, &ns, 4); memcpy(p + 20, &slen, 4);
        bw_exec_state_save(p + 24);
        memcpy(p + 24 + slen, &npre, 4);
        if (bw_exec_preamble((uint32_t *)(p + 28 + slen), npre) != npre)
            fprintf(stderr, "[GFXCAP] the state preamble changed size while written\n");
    }
    for (i = 0; i < ns; i++) {
        const BwGcSurf *h = &s_held[i].h;
        uint32_t y, step = (2u << 20) / h->row_bytes;
        if (!step)
            step = 1;
        if ((p = rec(GC_SURF, sizeof(BwGcSurf))) != NULL)
            memcpy(p, h, sizeof(BwGcSurf));
        for (y = 0; y < h->ph; y += step) {
            uint32_t n = h->ph - y < step ? h->ph - y : step;
            if ((p = rec(GC_SURFROWS, 12 + n * h->row_bytes)) != NULL) {
                memcpy(p, &h->va, 4); memcpy(p + 4, &y, 4); memcpy(p + 8, &n, 4);
                memcpy(p + 12, s_held[i].bytes + (size_t)y * h->row_bytes, (size_t)n * h->row_bytes);
            }
        }
    }
    /* the memory model starts empty again, and every answer the replay keeps
     * is recorded again when next asked */
    for (i = 0; i < GC_NPAGES; i++) {
        if (s_model[i] && s_model[i] != GC_UNREADABLE)
            free(s_model[i]);
        s_model[i] = NULL;
    }
    s_hwm = hwm;
    s_nblk = s_blk_next = 0;
    s_gamma_last = 0;
    s_sem_last = sem;
    fprintf(stderr, "[GFXCAP] mid-scene open at flip %u: executor state %u bytes, preamble %u methods,"
                    " %u surfaces, allocator mark %08X; the back end's texture cache was dropped\n",
            gen, slen, npre, ns, hwm);
    held_free();
    s_open_gen = 0;
    s_start_wait = 0;
}

/* ================================================================== replay */
typedef struct { uint32_t gen, va, w, h, bpp; uint64_t hash; } GcFlip;
#define GC_FQ 256
static GcFlip s_fq[GC_FQ];
static int s_fqh, s_fqn;
static struct {
    uint32_t compared, same, differ, missing, extra, first_bad;
} s_rv;
static FILE *s_vlog;

static void verdict_line(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    if (s_vlog) {
        va_start(ap, fmt);
        vfprintf(s_vlog, fmt, ap);
        va_end(ap);
        fflush(s_vlog);
    }
}

void bw_gfxcap_flip(uint32_t gen, uint32_t va, uint32_t pitch, uint32_t row, uint32_t rows,
                    uint32_t bpp)
{
    const uint8_t *base;
    uint64_t h;
    if (!g_bw_gfxcap || !rows || !row)
        return;
    if (g_bw_gfxcap == 1 && (!s_open || !s_buf || s_closed))
        return;
    base = GUEST(va);
    h = rows_hash(base, pitch, row, rows);
    if (shot_wanted(gen))
        write_shot(g_bw_gfxcap == 1 ? "live" : "replay", gen, base, pitch, row, rows, bpp);
    if (g_bw_gfxcap == 1) {
        uint8_t *r;
        uint32_t w = row / (bpp ? bpp : 1);
        mb_flush(1);
        if ((r = rec(GC_FLIP, 20 + 8)) != NULL) {
            memcpy(r, &gen, 4);
            memcpy(r + 4, &va, 4);
            memcpy(r + 8, &w, 4);
            memcpy(r + 12, &rows, 4);
            memcpy(r + 16, &bpp, 4);
            memcpy(r + 20, &h, 8);
        }
        if (++s_flips >= s_limit)
            s_close_due = 1;
        return;
    }
    if (s_touch_gen && gen + 1 == s_touch_gen && s_touch[0]) {     /* before the flip's next frame */
        FILE *tf = fopen(s_touch, "wb");
        if (tf)
            fclose(tf);
    }
    /* replay: the live flips before this one that the replay never made */
    while (s_fqn && s_fq[s_fqh].gen < gen) {
        if (s_rv.missing++ < 16)
            verdict_line("[GFXCAP] flip %u: in the capture, not in the replay\n", s_fq[s_fqh].gen);
        if (!s_rv.first_bad)
            s_rv.first_bad = s_fq[s_fqh].gen;
        s_fqh = (s_fqh + 1) % GC_FQ;
        s_fqn--;
    }
    if (!s_fqn || s_fq[s_fqh].gen != gen) {
        if (s_rv.extra++ < 16)
            verdict_line("[GFXCAP] flip %u: in the replay, not in the capture\n", gen);
        if (!s_rv.first_bad)
            s_rv.first_bad = gen;
        return;
    }
    {
        GcFlip *x = &s_fq[s_fqh];
        uint32_t w = row / (bpp ? bpp : 1);
        s_rv.compared++;
        if (x->hash == h && x->va == va && x->w == w && x->h == rows && x->bpp == bpp)
            s_rv.same++;
        else {
            if (s_rv.differ++ < 24)
                verdict_line("[GFXCAP] flip %u DIFFERS: live %08X %ux%u bpp %u hash %016llX,"
                             " replay %08X %ux%u bpp %u hash %016llX\n", gen, x->va, x->w, x->h, x->bpp,
                             (unsigned long long)x->hash, va, w, rows, bpp, (unsigned long long)h);
            if (!s_rv.first_bad)
                s_rv.first_bad = gen;
        }
        s_fqh = (s_fqh + 1) % GC_FQ;
        s_fqn--;
    }
}

/* Step B, replaying a mid-scene OPEN. Loading it (BW_GFXCAP_REPLAY_FROM=open,
 * or a file that starts mid-scene): the allocator's mark and the semaphore
 * target, the executor's state (its block, or its preamble run through the
 * executor and then checked against the block), the frame number, and then
 * the surfaces that follow. Met in a replay from boot: the replay's own
 * executor state is checked against the block, and the texture cache is
 * dropped as the live side's was. */
static uint32_t s_open_at;                  /* the flip a loaded mid-scene OPEN opened at */
static int s_surf_load;                     /* GC_SURF records to load */
static BwGcSurf s_sl_h;
static uint8_t *s_sl_bytes;
static uint32_t s_sl_rows, s_surf_made, s_surf_failed;

static void replay_open(const uint8_t *p, uint32_t len, int load)
{
    uint32_t why, gen, hwm, sem, ns, slen, npre, i;
    char diff[4096];
    int bad;
    if (len < 28)
        return;
    memcpy(&why, p, 4);
    if (why != 1)
        return;
    memcpy(&gen, p + 4, 4); memcpy(&hwm, p + 8, 4); memcpy(&sem, p + 12, 4);
    memcpy(&ns, p + 16, 4); memcpy(&slen, p + 20, 4);
    if (28 + (uint64_t)slen > len)
        return;
    memcpy(&npre, p + 24 + slen, 4);
    if (!load) {
        bad = bw_exec_state_compare(p + 24, slen, diff, sizeof diff);
        verdict_line("[GFXCAP] mid-scene OPEN at flip %u met in a replay from boot: the replay's executor"
                     " state %s%s%s; texture cache %s\n", gen,
                     bad == 0 ? "equals its block" : bad < 0 ? "can't be compared (size)" : "differs from its block: ",
                     bad > 0 ? diff : "", "", s_ignore_drop ? "kept (BW_GFXCAP_IGNORE_DROP)" : "dropped, as live");
        if (!s_ignore_drop && s_be && s_be->drop_textures)
            s_be->drop_textures();
        return;
    }
    s_hwm = hwm;
    nv2a_pb_set_semaphore_target(sem);
    if (s_use_preamble && 28 + (uint64_t)slen + (uint64_t)npre * 8 <= len) {
        const uint8_t *m = p + 28 + slen;
        for (i = 0; i < npre; i++) {
            uint32_t sm, param;
            memcpy(&sm, m + 8 * i, 4);
            memcpy(&param, m + 8 * i + 4, 4);
            nv2a_pb_exec_method(sm >> 16, sm & 0xFFFFu, param);
        }
        bad = bw_exec_state_compare(p + 24, slen, diff, sizeof diff);
        verdict_line("[GFXCAP] state from the preamble (%u methods): %s%s\n", npre,
                     bad == 0 ? "identical to the block" : bad < 0 ? "can't be compared" : "differs from the block in ",
                     bad > 0 ? diff : "");
    } else if (!bw_exec_state_load(p + 24, slen))
        verdict_line("[GFXCAP] replay: the state block doesn't fit this executable (%u bytes)\n", slen);
    if (s_be && s_be->set_gen)
        s_be->set_gen(gen);
    s_open_at = gen;
    s_surf_load = (int)ns;
    verdict_line("[GFXCAP] opened mid-scene at flip %u (state: %s; %u surfaces)\n", gen,
                 s_use_preamble ? "the preamble" : "the block", ns);
}

static void replay_surf(int type, const uint8_t *p, uint32_t len)
{
    if (type == GC_SURF) {
        if (!s_surf_load || len < sizeof(BwGcSurf))
            return;
        s_surf_load--;
        free(s_sl_bytes);
        memcpy(&s_sl_h, p, sizeof s_sl_h);
        s_sl_bytes = (uint8_t *)calloc((size_t)s_sl_h.row_bytes, s_sl_h.ph ? s_sl_h.ph : 1);
        s_sl_rows = 0;
        return;
    }
    if (s_sl_bytes && len >= 12) {
        uint32_t va, y, n;
        memcpy(&va, p, 4); memcpy(&y, p + 4, 4); memcpy(&n, p + 8, 4);
        if (va != s_sl_h.va || y + (uint64_t)n > s_sl_h.ph || 12 + (uint64_t)n * s_sl_h.row_bytes > len)
            return;
        memcpy(s_sl_bytes + (size_t)y * s_sl_h.row_bytes, p + 12, (size_t)n * s_sl_h.row_bytes);
        s_sl_rows += n;
        if (s_sl_rows >= s_sl_h.ph) {
            if (s_be && s_be->surface && s_be->surface(&s_sl_h, s_sl_bytes))
                s_surf_made++;
            else
                s_surf_failed++;
            free(s_sl_bytes);
            s_sl_bytes = NULL;
        }
    }
}

/* Write guest memory as recorded (pages of the image can be read-only). */
static void guest_write(uint32_t va, const uint8_t *src, uint32_t n)
{
    uint8_t *g = GUEST(va);
    DWORD old;
    if (!memcmp(g, src, n))
        return;
    if (VirtualProtect(g, n, PAGE_READWRITE, &old)) {
        memcpy(g, src, n);
        if (old != PAGE_READWRITE)
            VirtualProtect(g, n, old, &old);
    } else {
        static int told;
        if (told++ < 8)
            fprintf(stderr, "[GFXCAP] replay: guest %08X (%u bytes) is not writable\n", va, n);
    }
}

int bw_gfxcap_replay_main(void)
{
    FILE *f = fopen(s_replay, "rb");
    GcHeader h;
    GcChunk ch;
    DECOMPRESSOR_HANDLE dec = NULL;
    uint8_t *stored = NULL, *raw = NULL;
    size_t stored_cap = 0, raw_cap = 0;
    uint64_t methods = 0, idx_words = 0, pages = 0, diffs = 0, records = 0;
    uint32_t chunks = 0, closed = 0;
    char path[MAX_PATH + 32];
    LARGE_INTEGER t0, t1, fq;
    int bad_file = 0, skipping = s_from_open;

    snprintf(path, sizeof path, "%s/replay_verdict.txt", s_outdir);
    s_vlog = fopen(path, "w");
    if (!f || fread(&h, sizeof h, 1, f) != 1 || memcmp(h.magic, GC_MAGIC, 8) || h.version != GC_VERSION) {
        verdict_line("[GFXCAP] replay: %s is not a capture\n", s_replay);
        return 2;
    }
    fseek(f, (long)h.text_len, SEEK_CUR);
    if (!CreateDecompressor(COMPRESS_ALGORITHM_XPRESS_HUFF, NULL, &dec))
        dec = NULL;
    {
        /* gfx-lat11: BW_HIFPS_LATMODE=1|2 in a replay runs the latency mode's
         * back-end side (the fence per flip, each frame submitted at its
         * flip, the GPU timestamps, no GPU-limit wait in the flip), so the
         * corpus shows it changes no picture; its gate needs the game */
        const char *lm = getenv("BW_HIFPS_LATMODE");
        if (lm && (lm[0] == '1' || lm[0] == '2')) {
            bwg_gate_config(lm[0] - '0', 1, 1);
            fprintf(stderr, "[GFXCAP] replay: the latency mode's back-end side on (BW_HIFPS_LATMODE=%c)\n", lm[0]);
        }
    }
    /* The window the back end presents into: the game opens it with its
     * display mode (or the executor at the first clear); here no game runs,
     * so it is opened first, and the replay waits for it (at most 5 s). */
    {
        int i;
        xbox_FramebufferWindowStart();
        for (i = 0; i < 500 && !bw_fb_window(); i++)
            Sleep(10);
        fprintf(stderr, "[GFXCAP] replay: the window is %s\n", bw_fb_window() ? "up" : "not up (no presents)");
    }
    QueryPerformanceCounter(&t0);
    while (!closed && fread(&ch, sizeof ch, 1, f) == 1) {
        uint32_t at = 0;
        SIZE_T got = 0;
        if (ch.stored_len > stored_cap) {
            free(stored);
            stored_cap = ch.stored_len;
            stored = (uint8_t *)malloc(stored_cap);
        }
        if (ch.raw_len > raw_cap) {
            free(raw);
            raw_cap = ch.raw_len;
            raw = (uint8_t *)malloc(raw_cap);
        }
        if (!stored || !raw || fread(stored, 1, ch.stored_len, f) != ch.stored_len) {
            verdict_line("[GFXCAP] replay: the file ends inside chunk %u (cut short)\n", chunks);
            bad_file = 1;
            break;
        }
        if (ch.codec == GC_STORED && ch.stored_len == ch.raw_len)
            memcpy(raw, stored, ch.raw_len);
        else if (ch.codec != GC_XPRESS || !dec
                 || !Decompress(dec, stored, ch.stored_len, raw, ch.raw_len, &got) || got != ch.raw_len) {
            verdict_line("[GFXCAP] replay: chunk %u does not decompress\n", chunks);
            bad_file = 1;
            break;
        }
        if (fnv32(raw, ch.raw_len) != ch.check) {
            verdict_line("[GFXCAP] replay: chunk %u fails its check\n", chunks);
            bad_file = 1;
            break;
        }
        chunks++;
        while (at + 5 <= ch.raw_len) {
            uint8_t type = raw[at];
            uint32_t len, i, n;
            const uint8_t *p;
            memcpy(&len, raw + at + 1, 4);
            p = raw + at + 5;
            at += 5 + len;
            if (at > ch.raw_len)
                break;
            records++;
            if (skipping) {                 /* BW_GFXCAP_REPLAY_FROM=open: to the mid-scene OPEN */
                uint32_t why = 0;
                if (type == GC_OPEN && len >= 4)
                    memcpy(&why, p, 4);
                if (why == 1) {
                    replay_open(p, len, 1);
                    skipping = 0;
                }
                continue;
            }
            switch (type) {
            case GC_OPEN:                   /* mid-scene: load it if nothing ran before it */
                replay_open(p, len, !methods);
                break;
            case GC_SURF:
            case GC_SURFROWS:
                replay_surf(type, p, len);
                break;
            case GC_METHODS:
                memcpy(&n, p, 4);
                for (i = 0; i < n; i++) {
                    uint32_t sm, param;
                    memcpy(&sm, p + 4 + 8 * i, 4);
                    memcpy(&param, p + 8 + 8 * i, 4);
                    nv2a_pb_exec_method(sm >> 16, sm & 0xFFFFu, param);
                }
                methods += n;
                break;
            case GC_IDX16: {
                const uint32_t *w = (const uint32_t *)(p + 4);
                memcpy(&n, p, 4);
                if (!nv2a_pb_exec_idx16(w, n))
                    for (i = 0; i < n; i++)
                        nv2a_pb_exec_method(0, 0x1800, w[i]);
                idx_words += n;
                break;
            }
            case GC_PAGE: {
                uint32_t va;
                memcpy(&va, p, 4);
                guest_write(va, p + 4, GC_PAGE_SZ);
                pages++;
                break;
            }
            case GC_DIFF: {
                uint32_t va;
                memcpy(&va, p, 4);
                guest_write(va, p + 4, len - 4);
                diffs++;
                break;
            }
            case GC_GAMMA:
                bw_gamma_from_title(p + 4);
                break;
            case GC_SEM: {
                uint32_t va;
                memcpy(&va, p, 4);
                nv2a_pb_set_semaphore_target(va);
                break;
            }
            case GC_FLIP:
                if (s_fqn < GC_FQ) {
                    GcFlip *x = &s_fq[(s_fqh + s_fqn) % GC_FQ];
                    memcpy(&x->gen, p, 4);
                    memcpy(&x->va, p + 4, 4);
                    memcpy(&x->w, p + 8, 4);
                    memcpy(&x->h, p + 12, 4);
                    memcpy(&x->bpp, p + 16, 4);
                    memcpy(&x->hash, p + 20, 8);
                    s_fqn++;
                }
                break;
            case GC_CONTIG:
                memcpy(&s_hwm, p, 4);
                break;
            case GC_BLOCK: {
                uint32_t va, room;
                memcpy(&va, p, 4);
                memcpy(&room, p + 4, 4);
                blk_set(va, room);
                break;
            }
            case GC_PRESENT:
                memcpy(&s_dt_rep, p + 4, 4);
                s_dt_have = 1;
                break;
            case GC_CLOSE:
                closed = 1;
                break;
            case GC_LOCK:               /* I1: into the latch, as the wrapper would have */
                bwg_latch_replay_lock(p, len);
                break;
            case GC_LATCHSHOW:
                bwg_latch_replay_show(p, len);
                break;
            default:
                break;              /* OPEN, NOTE, and types this build doesn't know */
            }
        }
    }
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&fq);
    while (s_fqn) {                 /* live flips the replay never reached */
        if (s_rv.missing++ < 16)
            verdict_line("[GFXCAP] flip %u: in the capture, not in the replay\n", s_fq[s_fqh].gen);
        if (!s_rv.first_bad)
            s_rv.first_bad = s_fq[s_fqh].gen;
        s_fqh = (s_fqh + 1) % GC_FQ;
        s_fqn--;
    }
    verdict_line("[GFXCAP] replay %s: %u chunks, %llu records, %llu methods, %llu index words,"
                 " %llu pages, %llu diffs, in %.1f s%s\n", s_replay, chunks, (unsigned long long)records,
                 (unsigned long long)methods, (unsigned long long)idx_words, (unsigned long long)pages,
                 (unsigned long long)diffs, (double)(t1.QuadPart - t0.QuadPart) / fq.QuadPart,
                 closed ? "" : " (no CLOSE record)");
    if (skipping) {
        verdict_line("[GFXCAP] replay: the capture has no mid-scene OPEN (BW_GFXCAP_REPLAY_FROM=open)\n");
        bad_file = 1;
    }
    if (s_open_at)
        verdict_line("[GFXCAP] opened mid-scene at flip %u: %u surfaces made from the capture, %u not\n",
                     s_open_at, s_surf_made, s_surf_failed);
    {
        char fb[48] = "";
        if (s_rv.first_bad)
            snprintf(fb, sizeof fb, ", first bad flip %u", s_rv.first_bad);
        verdict_line("[GFXCAP] VERDICT %s: flips compared %u, identical %u, different %u,"
                     " missing %u, extra %u%s\n",
                     (!s_rv.differ && !s_rv.missing && !s_rv.extra && s_rv.compared && !bad_file)
                         ? "EXACT" : "DIFFERS",
                     s_rv.compared, s_rv.same, s_rv.differ, s_rv.missing, s_rv.extra, fb);
    }
    if (dec)
        CloseDecompressor(dec);
    free(stored);
    free(raw);
    fclose(f);
    if (s_vlog)
        fclose(s_vlog);
    fflush(stderr);
    return (!s_rv.differ && !s_rv.missing && !s_rv.extra && s_rv.compared && !bad_file) ? 0 : 1;
}
