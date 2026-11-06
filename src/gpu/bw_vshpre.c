/*
 * bw_vshpre.c - the game's vertex programs compiled at load time, so no
 * program meets a compile at its first draw (lane gfx-regen,
 * docs/gfx/lanes/regen.md; every edition; BW_GPU_VSH_PRE=0 turns it off).
 *
 * Where the programs come from (seen in the generated code): every vertex
 * shader the game makes goes through D3DDevice_CreateVertexShader 0x127820,
 * and every call to it lies in the 16 shader groups Shaders_Init runs from
 * Graphics_Init at boot (Shaders_CreateGroup01..16, 0x32300-0x33850: 49
 * call sites). The D-011 wrapper sub_00127820 (src/recomp_manual.c) hands
 * each one here after the game's body: the function's microcode (a 4-byte
 * header, type 0x2078 and the instruction count, then 4 words an
 * instruction) and the declaration's stream formats.
 *
 * What the GPU path keys a shader by (get_vsh in bw_d3d11.c): the program's
 * words from its start slot to the FINAL instruction, the inputs it reads,
 * each input's conversion when the batch's streams go up raw (P1: D3DCOLOR,
 * unsigned bytes, shorts, packed normals; floats need none), and for a point
 * batch on the GPU (gfx-sprites) the point variant. The declaration gives
 * the formats the game binds with the program (D3DVSD_REG(register, type):
 * the type is the NV2A's array format, size in the high nibble, kind in the
 * low), so each program gets its raw variant and the all-floats one (the
 * gathered path, taken when a batch's streams can't go up raw), and, with
 * GPU_SPRITES, the point variants of each program that writes a point size.
 *
 * The texts are made by the bw_vsh_hlsl translator, exactly as get_vsh makes
 * them, and compiled by worker threads with get_vsh's compiler and flags
 * (vs_5_0, O3, IEEE strictness, BW_WS in a widescreen boot) into the shader
 * cache (bw_pscache_store_vs: the same key get_vsh looks up), skipping any
 * the cache holds. A first sight then finds its shader in the cache; one
 * that comes while its compile is still running waits for it with
 * GPU_VSH_SYNC, else draws as before. Nothing here touches the device.
 */
#define COBJMACROS
#include <windows.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bw_vshpre.h"
#include "bw_vsh_hlsl.h"
#include "bw_pscache.h"
#include "toolkit_patches/kernel/bw_backend_ext.h"
#include "core/bwg_core.h"

volatile int g_bw_vshwatch = 1;         /* until the switch is read (the wrapper's first call) */

#define MAX_JOBS 1024
#define MAX_PROGS 256
#define MAX_INSTR 136

typedef struct {
    char *src;
    size_t len;
    uint64_t h;                         /* FNV-1a of the text (the job table's key) */
    char kind;                          /* 'p' program, 'P' its point variant, 'f' fixed function */
    volatile LONG state;                /* 0 queued, 1 compiling, 2 compiled, 3 in the cache, 4 failed, 5 claimed */
    double ms;
} Job;

typedef struct {
    uint32_t words[MAX_INSTR][4];
    uint32_t n, mask;
    uint8_t conv[16];
    int raw, opts;
} Prog;

static SRWLOCK s_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE s_cv = CONDITION_VARIABLE_INIT;   /* a job queued or finished */
static Job s_job[MAX_JOBS];
static unsigned s_njob, s_next;         /* s_next: the first job that may be queued */
static Prog *s_prog;
static unsigned s_nprog;
static int s_on = -1, s_started, s_sprites, s_nworkers;
static unsigned s_done, s_compiled, s_cached, s_failed, s_claimed, s_nofinal, s_other, s_dup, s_toolong;
static double s_t_first, s_t_done, s_compile_ms, s_max_ms;
static int s_said;

static uint64_t text_hash(const char *s, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    size_t i;
    for (i = 0; i < n; i++)
        h = (h ^ (uint8_t)s[i]) * 1099511628211ull;
    return h ^ n;
}

/* Under the lock: a job for this text, or NULL. */
static Job *find_job(uint64_t h, size_t len, const char *src)
{
    unsigned i;
    for (i = 0; i < s_njob; i++)
        if (s_job[i].h == h && s_job[i].len == len && !memcmp(s_job[i].src, src, len))
            return &s_job[i];
    return NULL;
}

/* A text to compile (copied); a duplicate is dropped. */
static void add_job(const char *src, size_t len, char kind)
{
    uint64_t h = text_hash(src, len);
    char *c;
    if (!len)
        return;
    AcquireSRWLockExclusive(&s_lock);
    if (find_job(h, len, src)) {
        s_dup++;
        ReleaseSRWLockExclusive(&s_lock);
        return;
    }
    if (s_njob == MAX_JOBS || !(c = (char *)malloc(len + 1))) {
        ReleaseSRWLockExclusive(&s_lock);
        return;
    }
    memcpy(c, src, len);
    c[len] = 0;
    s_job[s_njob].src = c;
    s_job[s_njob].len = len;
    s_job[s_njob].h = h;
    s_job[s_njob].kind = kind;
    s_job[s_njob].state = 0;
    s_job[s_njob].ms = 0.0;
    if (!s_njob)
        s_t_first = bwg_qpc_ms();
    s_njob++;
    s_t_done = 0.0;
    ReleaseSRWLockExclusive(&s_lock);
    WakeAllConditionVariable(&s_cv);
}

static void add_prog_texts(const Prog *p, int points, char kind)
{
    static const uint8_t none[16];
    const size_t cap = 96 * 1024;       /* get_vsh's buffer */
    char *src = (char *)malloc(cap);    /* the wrapper's thread and the start can overlap */
    size_t n;
    if (!src)
        return;
    if (p->raw) {
        n = bw_vsh_hlsl_ex((const uint32_t (*)[4])p->words, p->n, p->mask, p->conv, points, src, cap);
        add_job(src, n, points ? 'P' : kind);
    }
    n = bw_vsh_hlsl_ex((const uint32_t (*)[4])p->words, p->n, p->mask, none, points, src, cap);
    add_job(src, n, points ? 'P' : kind);
    free(src);
}

/* The conversion get_vsh keys a raw batch's input by (vsh_conv_key, from the
 * executor's bw_vfetch_conv), for the array format byte t (size << 4 | kind);
 * *raw = 0 when no raw batch can have it (the batch is gathered). */
static uint8_t fmt_conv(uint32_t t, int *raw)
{
    uint32_t s = t >> 4;
    if (!s)
        return 0;                       /* no array: the current value (CONST), not in the key */
    if (s > 4) {
        *raw = 0;
        return 0;
    }
    switch (t & 15u) {
    case 2: return 0;                                   /* floats */
    case 0: return (uint8_t)(BW_XVS_COLOR << 4 | 4);
    case 4: return (uint8_t)(BW_XVS_UBYTE << 4 | s);
    case 5: return (uint8_t)(BW_XVS_SHORT << 4 | s);
    case 6: if (s == 1) return (uint8_t)(BW_XVS_CMP << 4 | 1);
            *raw = 0; return 0;
    default: *raw = 0; return 0;                        /* normalised shorts and the rest: gathered */
    }
}

static int writes_point_size(const uint32_t (*w)[4], uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++)
        if (bw_vsh_field(w[i], 3, 12, 4) && bw_vsh_field(w[i], 3, 11, 1) && bw_vsh_field(w[i], 3, 3, 8) == 6)
            return 1;
    return 0;
}

#define GMEM32(va) (*(volatile uint32_t *)((uintptr_t)(uint32_t)(va) + g_xbox_mem_offset))

static void add_program(uint32_t va, uint32_t n, const uint8_t fmt[16]);
static void add_d3d_passthrough(void);
static int s_d3d_done, s_in_d3d;
static unsigned s_npt;

/* BW_GPU_VSH_PRE, once: at the wrapper's first call or the back end's start
 * (a replay has no game, so only the start), whichever is first. */
static void read_switch(void)
{
    const char *e;
    if (s_on >= 0)
        return;
    e = getenv("BW_GPU_VSH_PRE");
    s_on = !(e && e[0] == '0');
    if (!s_on) {
        g_bw_vshwatch = 0;
        fprintf(stderr, "[GPU-VSHPRE] the load-time vertex-shader compile is off (BW_GPU_VSH_PRE=0)\n");
    }
}

void bw_gpu_vsh_created(uint32_t decl, uint32_t function, uint32_t hr)
{
    uint32_t head, n, i;
    uint8_t fmt[16];
    read_switch();
    if (!s_on || hr || !function)
        return;
    if (!s_d3d_done) {
        s_d3d_done = 1;
        add_d3d_passthrough();
    }
    head = GMEM32(function);
    n = head >> 16;
    if ((head & 0xFFFFu) != 0x2078u) {  /* 0x7378: a state shader (no draws) */
        s_other++;
        return;
    }
    if (!n || n > MAX_INSTR) {
        s_toolong++;
        return;
    }
    /* the declaration: D3DVSD_STREAM 0x2..., D3DVSD_REG(reg, type) 0x4.....,
     * SKIP (bit 28), CONST 0x8... (+ 4 words a constant), EXT 0xA... (+ its
     * count), END 0xFFFFFFFF; at most 64 tokens read */
    memset(fmt, 0x02, sizeof fmt);      /* undeclared: no array (size 0) */
    if (decl) {
        for (i = 0; i < 64; i++) {
            uint32_t t = GMEM32(decl + 4 * i), kind = t >> 29;
            if (t == 0xFFFFFFFFu || kind == 7)
                break;
            if (kind == 2 && !(t & 0x10000000u))
                fmt[t & 15u] = (uint8_t)(t >> 16);
            else if (kind == 4)
                i += 4 * ((t >> 25) & 15u);
            else if (kind == 5)
                i += (t >> 24) & 31u;
        }
    }
    add_program(function + 4, n, fmt);
}

/* One program (n instructions at guest va) with these array formats. */
static void add_program(uint32_t va, uint32_t n, const uint8_t fmt[16])
{
    uint32_t i, j, a, k;
    Prog *p;
    if (!s_prog && !(s_prog = (Prog *)calloc(MAX_PROGS, sizeof *s_prog)))
        return;
    if (s_nprog == MAX_PROGS)
        return;
    p = &s_prog[s_nprog];
    memset(p, 0, sizeof *p);
    for (i = 0; i < n; i++)
        for (j = 0; j < 4; j++)
            p->words[i][j] = GMEM32(va + 16 * i + 4 * j);
    if (!(p->words[n - 1][3] & 1)) {
        s_nofinal++;                    /* the executor would run past it: its key is not this program's */
        return;
    }
    p->n = n;
    p->mask = bw_vsh_inputs_used((const uint32_t (*)[4])p->words, n);
    p->raw = 1;
    for (a = 0, k = 0; a < 16 && k < 16; a++) {
        if (!(p->mask & (1u << a)))
            continue;
        p->conv[k++] = fmt_conv(fmt[a], &p->raw);
    }
    {
        static const uint8_t none[16];
        if (!memcmp(p->conv, none, sizeof none))
            p->raw = 0;                 /* the all-floats text covers it */
    }
    p->opts = writes_point_size((const uint32_t (*)[4])p->words, n);
    s_nprog++;
    add_prog_texts(p, 0, s_in_d3d ? 'd' : 'p');
    if (s_started && s_sprites && p->opts)
        add_prog_texts(p, 1, 'P');
}

/* D3D's own programs, which no CreateVertexShader makes: the pass-through
 * programs D3D_UpdatePassthroughProgram (0x127C20) loads at slot 0 for a
 * pre-transformed (D3DFVF_XYZRHW) vertex format, one of three fixed ones in
 * D3D's data (seen in its code: 0x1328B0 and 0x132A20, 0x30 words, and
 * 0x132970, 0x2C words; 12 and 11 instructions). They are read from the
 * game's memory here, never stored. An FVF's colours are D3DCOLOR arrays
 * and everything else floats, so each program gets every set of its colour
 * inputs (v3 diffuse, v4 specular, v7 and v8 the back ones) as arrays. A
 * program whose last instruction isn't FINAL (another build of the game) is
 * skipped. */
static void add_d3d_passthrough(void)
{
    static const struct { uint32_t va, n; } pt[3] = { { 0x1328B0u, 12 }, { 0x132970u, 11 }, { 0x132A20u, 12 } };
    static const uint8_t col[4] = { 3, 4, 7, 8 };
    unsigned i, m, c;
    s_in_d3d = 1;
    for (i = 0; i < 3; i++)
        for (m = 0; m < 16; m++) {
            uint8_t fmt[16];
            memset(fmt, 0x42, sizeof fmt);  /* floats */
            for (c = 0; c < 4; c++)
                fmt[col[c]] = (m >> c) & 1 ? 0x40 : 0x02;   /* D3DCOLOR, or no array */
            add_program(pt[i].va, pt[i].n, fmt);
            s_npt++;
        }
    s_in_d3d = 0;
}

void bw_vshpre_add_text(const char *src, size_t len, char kind)
{
    if (s_on == 0)
        return;
    add_job(src, len, kind);
}

static const D3D_SHADER_MACRO s_ws_def[] = { { "BW_WS", "1" }, { NULL, NULL } };

static DWORD WINAPI worker(LPVOID arg)
{
    (void)arg;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;) {
        Job *j = NULL;
        ID3DBlob *blob = NULL, *err = NULL;
        int ws = bwg_ws_k > 1.0f;
        LARGE_INTEGER t0, t1, f;
        LONG st;
        AcquireSRWLockExclusive(&s_lock);
        for (;;) {
            unsigned i;
            /* the first queued job of the earliest kind the boot draws: the
             * fixed function (the loading screen's caption), D3D's
             * pass-through programs (the movies), then the game's programs
             * and their point variants, each in the order they came */
            static const char order[] = "fdpP";
            const char *o;
            for (o = order; *o && !j; o++)
                for (i = s_next; i < s_njob; i++)
                    if (s_job[i].state == 0 && s_job[i].kind == *o) {
                        j = &s_job[i];
                        j->state = 1;
                        break;
                    }
            if (j)
                break;
            s_next = s_njob;
            SleepConditionVariableSRW(&s_cv, &s_lock, INFINITE, 0);
        }
        ReleaseSRWLockExclusive(&s_lock);
        QueryPerformanceCounter(&t0);
        if (bw_pscache_has_vs(j->src, j->len, ws))
            st = 3;
        else if (SUCCEEDED(D3DCompile(j->src, j->len, "bw_vsh", ws ? s_ws_def : NULL, NULL, "vs_main",
                                      "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_IEEE_STRICTNESS,
                                      0, &blob, &err))) {
            bw_pscache_store_vs(j->src, j->len, ws, ID3D10Blob_GetBufferPointer(blob),
                                ID3D10Blob_GetBufferSize(blob));
            st = 2;
        } else {
            static LONG said;
            if (InterlockedIncrement(&said) <= 3)
                fprintf(stderr, "[GPU-VSHPRE] a %s's shader failed to compile: %s\n",
                        j->kind == 'f' ? "fixed-function shape" : "program",
                        err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?");
            st = 4;
        }
        if (blob) ID3D10Blob_Release(blob);
        if (err) ID3D10Blob_Release(err);
        QueryPerformanceCounter(&t1);
        QueryPerformanceFrequency(&f);
        AcquireSRWLockExclusive(&s_lock);
        j->ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;
        j->state = st;
        s_done++;
        if (st == 2) {
            s_compiled++;
            s_compile_ms += j->ms;
            if (j->ms > s_max_ms)
                s_max_ms = j->ms;
        } else if (st == 3)
            s_cached++;
        else
            s_failed++;
        if (s_done + s_claimed >= s_njob)
            s_t_done = bwg_qpc_ms();
        ReleaseSRWLockExclusive(&s_lock);
        WakeAllConditionVariable(&s_cv);
    }
}

void bw_vshpre_start(int sprites)
{
    unsigned i, n;
    SYSTEM_INFO si;
    read_switch();
    if (s_started || s_on == 0)
        return;
    s_started = 1;
    s_sprites = sprites;
    if (sprites)                        /* the point variants of the programs made so far */
        for (i = 0; i < s_nprog; i++)
            if (s_prog[i].opts)
                add_prog_texts(&s_prog[i], 1, 'P');
    GetSystemInfo(&si);
    n = si.dwNumberOfProcessors / 2;     /* below normal priority: the boot's own threads go first */
    n = n < 1 ? 1 : n > 8 ? 8 : n;
    {
        const char *e = getenv("BW_GPU_VSH_PRE_THREADS");
        if (e && atoi(e) > 0 && atoi(e) <= 16)
            n = (unsigned)atoi(e);
    }
    for (i = 0; i < n; i++) {
        HANDLE t = CreateThread(NULL, 0, worker, NULL, 0, NULL);
        if (!t)
            break;
        CloseHandle(t);
        s_nworkers++;
    }
    fprintf(stderr, "[GPU-VSHPRE] load-time vertex-shader compile: %u programs from the game's shader groups"
                    " so far (and D3D's pass-through ones), %u texts queued, %d worker thread%s%s\n",
            s_nprog >= s_npt ? s_nprog - s_npt : 0, s_njob, s_nworkers,
            s_nworkers == 1 ? "" : "s", sprites ? " (with the point variants)" : "");
    if (!s_nworkers)
        s_on = 0;                       /* no worker: first sights compile as before */
}

int bw_vshpre_wait(const char *src, size_t len, int ws, int wait, double *ms)
{
    uint64_t h;
    Job *j;
    int r = 0;
    double t0 = 0.0;
    (void)ws;
    if (ms)
        *ms = 0.0;
    if (s_on <= 0 || !s_njob)
        return 0;
    h = text_hash(src, len);
    AcquireSRWLockExclusive(&s_lock);
    if ((j = find_job(h, len, src)) != NULL) {
        if (j->state == 0) {
            j->state = 5;               /* not started: the caller makes it now */
            s_claimed++;
            if (s_done + s_claimed >= s_njob)
                s_t_done = bwg_qpc_ms();
            r = 2;
        } else if (j->state == 1 && !wait)
            r = 3;
        else {
            if (j->state == 1) {
                t0 = bwg_qpc_ms();
                while (j->state == 1)
                    if (!SleepConditionVariableSRW(&s_cv, &s_lock, 2000, 0))
                        break;
                if (ms)
                    *ms = bwg_qpc_ms() - t0;
            }
            r = 1;
        }
    }
    ReleaseSRWLockExclusive(&s_lock);
    return r;
}

int bw_vshpre_busy(void)
{
    return s_on > 0 && s_done + s_claimed < s_njob;
}

void bw_vshpre_line(int force)
{
    if (s_on <= 0 || !s_njob)
        return;
    if (!force && (s_said || bw_vshpre_busy()))
        return;
    s_said = 1;
    fprintf(stderr, "[GPU-VSHPRE] %u of the game's programs and %u of D3D's pass-through program and format sets"
                    " (%u state shaders skipped, %u without a FINAL, %u too long): %u shader texts (%u duplicates"
                    " dropped), %u compiled (%.0f ms on %d threads, the longest %.1f ms), %u already in the cache,"
                    " %u failed, %u made at first sight instead; %s %.0f ms after the first\n",
            s_nprog >= s_npt ? s_nprog - s_npt : 0, s_npt, s_other, s_nofinal, s_toolong, s_njob, s_dup,
            s_compiled, s_compile_ms, s_nworkers, s_max_ms, s_cached, s_failed, s_claimed,
            bw_vshpre_busy() ? "still going" : "all done", (s_t_done ? s_t_done : bwg_qpc_ms()) - s_t_first);
}
