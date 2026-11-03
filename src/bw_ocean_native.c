/*
 * bw_ocean_native.c - the native ocean (Remastered; agent P2a,
 * docs/day1/P2a.md, registry OCEAN_NATIVE).
 *
 * The sea's vertices are the game thread's largest cost: per drawn tile,
 * Ocean_FillVertices (0x37A60) asks WaterWorld_EvaluateVertexBatch (0x98130,
 * mode 0x1D) for 25 vertices, and each one runs WaterWorld_VertexHeightNormal
 * (0x961A0: the swell's bicubic patch and slopes, the shore maps, the static
 * zones and every ripple, wake and disturbance in its 100 m cell). The
 * generated code for these functions emulates the Pentium III's x87 with a
 * thread-local stack array and a run-time top, a volatile guest stack frame
 * and a precision-control test on every operation.
 *
 * tools/bw_ocean_xlate.py translates those six functions (the batch,
 * VertexHeightNormal, HeightAndSlopeAt, GridSlopeAt, GetBezierPatch,
 * GetCell) from the generated C into src/recomp/gen/bw_ocean_native.inc, with
 * the x87 stack depth and the frame offsets resolved at translation time:
 * the same operations on the same values in the same order, so the result is
 * bit-identical by construction. This file supplies what the translation
 * uses, runs it for the render-vertex batches only, and checks the
 * conditions under which "the same operations" also means "the same
 * results":
 *   - the guest's x87 precision control picks the variant: at 24 bits
 *     RECOMP_FP_PC is (double)(float)x inside float's range (NB_R), at 53/64
 *     bits the double as it is (the translation is compiled once for each);
 *   - no result left float's range: the host's overflow and underflow flags
 *     are cleared before and read after (and before each Bezier patch is
 *     marked built); if either is set, the native work is thrown away, the
 *     state it changed is put back and the game's own function runs;
 *   - the x87's trigonometric range (|x| < 2^63) holds, likewise.
 *
 * Only the batch Ocean_FillVertices calls (return address 0x37CC2, mode
 * 0x1D) comes here. Every other caller of the water (boats, hulls,
 * projectiles, AI, cameras, the secondary water render) runs the game's own
 * code, untouched.
 *
 * Switches (every edition but Original, which never runs any of this):
 *   BW_FIX=-OCEAN_NATIVE or BW_OCEAN_NATIVE=0   the game's own code again
 *   BW_OCEAN_NATIVE_VERIFY=1  run both on every batch, keep the game's result,
 *                             count differing bytes (outputs, Bezier patches
 *                             and their flags, registers, x87 status)
 *   BW_OCEAN_NATIVE_FUZZ=1    (with VERIFY) also verify on altered states, for
 *                             the branches play rarely reaches (see nb_fuzz)
 *   BW_OCEAN_NATIVE_COVER=<file> (with VERIFY) basic-block counts, cumulative
 *   BW_OCEAN_SEAM_TEST=1      diagnostic only (changes the picture): every
 *                             render tile uses the bicubic heights (P2a.md)
 *   BW_OCEAN_NATIVE_LOG=1     a line every 5 s: batches, native/fallback,
 *                             mismatches, and the time spent (every edition:
 *                             in Original it times the game's own code)
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <setjmp.h>
#include <intrin.h>
#include <xmmintrin.h>

#include "recomp_types.h"           /* the generated C's (BW_GEN_DIR, CMakeLists.txt) */
#include "bw_edition.h"

#ifdef _MSC_VER
#pragma warning(disable: 4102 4189 4101 4100 4702)  /* the translation's labels and locals */
#endif

extern void sub_00098130_gen(void);

/* ---- what the translation uses ------------------------------------------ */

typedef struct {
    uint32_t eax, ebx, ecx, edx, esi, edi;   /* the guest's registers (TLS in the generated code) */
    uint32_t gebp, gsebp;                    /* g_ebp, g_seh_ebp */
    int fcmp;                                /* g_fp_cmp */
    uint16_t fcc;                            /* g_fp_cc */
} NbRegs;

typedef struct {
    jmp_buf jb;
    uint16_t cw;                             /* g_fp_control_word, fixed for the batch */
    int pc24;                                /* its precision control is 24 bits */
    uint32_t *cov;                           /* basic-block counts (verify mode), else NULL */
    uintptr_t mo;                            /* g_xbox_mem_offset */
} NbCtx;

#define NB_ENTER() \
    uint32_t eax = NBR->eax, ebx = NBR->ebx, ecx = NBR->ecx, edx = NBR->edx; \
    uint32_t esi = NBR->esi, edi = NBR->edi, gebp = NBR->gebp, gsebp = NBR->gsebp; \
    int fcmp = NBR->fcmp; uint16_t fcc = NBR->fcc; \
    const uint16_t CW = NBC->cw; const uintptr_t MO = NBC->mo; uint32_t *const COV = NBC->cov; \
    double S0 = 0, S1 = 0, S2 = 0, S3 = 0, S4 = 0, S5 = 0, S6 = 0, S7 = 0
#define NB_SAVE() (NBR->eax = eax, NBR->ebx = ebx, NBR->ecx = ecx, NBR->edx = edx, \
                   NBR->esi = esi, NBR->edi = edi, NBR->gebp = gebp, NBR->gsebp = gsebp, \
                   NBR->fcmp = fcmp, NBR->fcc = fcc)
#define NB_LOAD() (eax = NBR->eax, ebx = NBR->ebx, ecx = NBR->ecx, edx = NBR->edx, \
                   esi = NBR->esi, edi = NBR->edi, gebp = NBR->gebp, gsebp = NBR->gsebp, \
                   fcmp = NBR->fcmp, fcc = NBR->fcc)
#define NB_LEAVE() NB_SAVE()
#define NB_COVER(i) do { if (COV) COV[i]++; } while (0)
#define NB_BAIL() longjmp(NBC->jb, 1)
#define NB_UNREACHABLE() do { NB_BAIL(); } while (0)
/* NB_R is RECOMP_FP_PC for one precision control (the translation is
 * included twice below): at 24 bits it is (double)(float)x inside float's
 * range (the driver checks the range); at 53 or 64 bits the generated code's
 * double-backed stack keeps the double as it is. */
/* recomp_fp_trig_in_range's test, without its status write (the caller does it) */
#define NB_TRIG_OK(x) (!(isfinite(x) && fabs(x) >= 9223372036854775808.0))
/* Before a Bezier patch is marked built (the only shared state the
 * translation commits besides its outputs): no result may have left float's
 * range so far, or the whole native batch is thrown away. */
#define MX_RANGE (_MM_EXCEPT_OVERFLOW | _MM_EXCEPT_UNDERFLOW)
#define NB_COMMIT_CHECK() do { if (_mm_getcsr() & MX_RANGE) NB_BAIL(); } while (0)

/* Guest memory, not volatile (this code is the only thing running on the
 * game thread while it runs, as the generated code is). */
#define NB_P(a)       ((uintptr_t)(uint32_t)(a) + MO)
#define NMEM8(a)      (*(uint8_t  *)NB_P(a))
#define NMEM16(a)     (*(uint16_t *)NB_P(a))
#define NMEM32(a)     (*(uint32_t *)NB_P(a))
#define NSMEM8(a)     (*(int8_t   *)NB_P(a))
#define NSMEM16(a)    (*(int16_t  *)NB_P(a))
#define NSMEM32(a)    (*(int32_t  *)NB_P(a))
#define NSMEM64(a)    (*(int64_t  *)NB_P(a))
#define NMEMF(a)      (*(float    *)NB_P(a))
#define NMEMD(a)      (*(double   *)NB_P(a))

/* crt_ftol2 0xDFB88 (MSVC's _ftol2, as lifted): fistp at the current
 * rounding, fild it back, then a one-step correction toward zero from the
 * sign of x and of (x - rounded) as floats. Consumes ST0; returns eax:edx;
 * publishes its frame (g_ebp = g_seh_ebp = esp after `push ebp`), as the
 * generated body does. Its scratch stores went to an aligned frame below the
 * caller's stack top, which nothing reads afterwards; they aren't repeated. */
static void nbx_ftol2(NbCtx *NBC, NbRegs *NBR, uint32_t SP, double x)
{
    float xf = (float)x;                                    /* fst  [esp+0x18] */
    int64_t i = recomp_fist(x, NBC->cw, 64);                /* fistp [esp+0x10] */
    double fi = (double)i;                                  /* fild */
    uint32_t lo = (uint32_t)(uint64_t)i, hi = (uint32_t)((uint64_t)i >> 32);
    uint32_t eax = lo, edx, ecx = NBR->ecx, db;
    float df;
    int cf;
    memcpy(&edx, &xf, 4);                                   /* edx = [esp+0x18] */
    if (eax == 0) {                                         /* loc_DFBE7 */
        edx = hi;
        if (!(edx & 0x7FFFFFFFu))
            goto done;                                      /* loc_DFBF3: both popped */
    }
    {
        double dd = x - fi;                                 /* fsubp st(1) (RECOMP_FP_PC) */
        if (NBC->pc24)
            dd = (double)(float)dd;
        df = (float)dd;                                     /* fstp [esp] */
    }
    memcpy(&db, &df, 4);
    if ((int32_t)edx >= 0) {                                /* loc_DFBCF */
        ecx = db;
        cf = (int)((((uint64_t)ecx + 0x7FFFFFFFu) >> 32) & 1);
        ecx += 0x7FFFFFFFu;
        { uint64_t t = (uint64_t)eax - (uint64_t)cf; cf = (int)((t >> 32) & 1); eax = (uint32_t)t; }
        edx = hi;
        { uint64_t t = (uint64_t)edx - (uint64_t)cf; edx = (uint32_t)t; }
    } else {                                                /* loc_DFBB1 */
        ecx = db ^ 0x80000000u;
        cf = (int)((((uint64_t)ecx + 0x7FFFFFFFu) >> 32) & 1);
        ecx += 0x7FFFFFFFu;
        { uint64_t t = (uint64_t)eax + (uint64_t)cf; cf = (int)((t >> 32) & 1); eax = (uint32_t)t; }
        edx = hi;
        { uint64_t t = (uint64_t)edx + (uint64_t)cf; edx = (uint32_t)t; }
    }
done:
    NBR->eax = eax;
    NBR->ecx = ecx;
    NBR->edx = edx;
    NBR->gebp = NBR->gsebp = SP - 4u;                       /* push ebp; mov ebp, esp; publish */
    {   /* the pushed ebp sits at [SP-4] in the generated body: keep the stack the same */
        const uintptr_t MO = NBC->mo;
        (void)MO;
    }
}

/* The translation at 24-bit precision (the Xbox's D3D-era default; what
 * Water_Update itself sets for its run) ... */
#define NB_R(x) ((double)(float)(x))
#include "bw_ocean_native.inc"
#undef NB_R
/* ... and at 53/64 bits (the runtime's initial control word 0x037F), as
 * nbx64_*: the same code with RECOMP_FP_PC an identity. */
#define NB_R(x) ((double)(x))
#define nbx_getcell nbx64_getcell
#define nbx_gbp     nbx64_gbp
#define nbx_hsa     nbx64_hsa
#define nbx_gsa     nbx64_gsa
#define nbx_vhn     nbx64_vhn
#define nbx_batch   nbx64_batch
#include "bw_ocean_native.inc"
#undef nbx_getcell
#undef nbx_gbp
#undef nbx_hsa
#undef nbx_gsa
#undef nbx_vhn
#undef nbx_batch
#undef NB_R

/* ---- the driver --------------------------------------------------------- */

#include "bw_waterworld.h"     /* WW_VA, WW_PROF_TIME, the Water object (item 71) */
#define PROF_DEPTH_VA    0x001E7D20u   /* the rdtsc profiler's stack index */

#define GP(va)  ((void *)((uintptr_t)(uint32_t)(va) + (uintptr_t)g_xbox_mem_offset))
#define GU32(va) (*(volatile uint32_t *)GP(va))

static int s_init, s_on, s_verify, s_log;
static int s_fuzz, s_seamtest, s_tiledump;
static unsigned s_fuzz_n;
static unsigned long s_n_fuzz[4];
static uint32_t s_cov[NB_COVER_N];           /* both precision variants share the table */
static unsigned long long s_t_native, s_t_game, s_t_last;
static unsigned long s_n_batch, s_n_native, s_n_game, s_n_fb_mode, s_n_fb_pc, s_n_fb_range,
                     s_n_fb_bail, s_n_verify, s_n_vbad, s_n_vbad_bytes;
static double s_tsc_per_ms;

static unsigned env_u(const char *name, unsigned dflt)
{
    const char *e = getenv(name);
    return e && *e ? (unsigned)strtoul(e, NULL, 10) : dflt;
}

static void nb_init(void)
{
    LARGE_INTEGER f, q0, q1;
    unsigned long long t0, t1;
    s_init = 1;
    s_log = env_u("BW_OCEAN_NATIVE_LOG", 0) == 1;
    s_on = bw_edition_at_least(BW_ED_REMASTERED) && bw_fix_enabled(BW_FIX_OCEAN_NATIVE)
        && env_u("BW_OCEAN_NATIVE", 1) != 0;
    s_verify = s_on && env_u("BW_OCEAN_NATIVE_VERIFY", 0) == 1;
    s_fuzz = s_verify && env_u("BW_OCEAN_NATIVE_FUZZ", 0) == 1;
    /* Diagnostic for the distant-water line (P2a.md, the lead's question):
     * every render tile asks for the bicubic surface (flag 0x80 cleared), so
     * coarse tiles no longer take nearest-grid-point heights. Changes the
     * picture; Remastered only; never a default. */
    s_seamtest = bw_edition_at_least(BW_ED_REMASTERED) && env_u("BW_OCEAN_SEAM_TEST", 0) == 1;
    s_tiledump = getenv("BW_OCEAN_TILEDUMP") && *getenv("BW_OCEAN_TILEDUMP");
    if (s_log) {
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&q0);
        t0 = __rdtsc();
        do QueryPerformanceCounter(&q1); while ((q1.QuadPart - q0.QuadPart) * 1000 < f.QuadPart * 20);
        t1 = __rdtsc();
        s_tsc_per_ms = (double)(t1 - t0) * (double)f.QuadPart / (double)(q1.QuadPart - q0.QuadPart) / 1000.0;
        s_t_last = t1;
    }
    if (s_on || s_log)
        fprintf(stderr, "[BW] native ocean: %s%s (render-vertex batches; docs/day1/P2a.md)\n",
                s_on ? "on" : "off", s_verify ? ", verifying against the game's function" : "");
}

/* One run of the translation at the batch's own stack frame. 1 = done (the
 * result is in guest memory and NBR holds the final registers); 0 = thrown
 * away, the caller restores and runs the game's function. */
static int nb_run(NbRegs *r, uint32_t sp)
{
    NbCtx c;
    unsigned int mx = _mm_getcsr();
    volatile int ok = 0;
    c.cw = g_fp_control_word;
    c.pc24 = (c.cw & 0x300u) == 0;
    c.cov = s_verify ? s_cov : NULL;
    c.mo = (uintptr_t)g_xbox_mem_offset;
    _mm_setcsr(mx & ~0x3Fu);
    if (!setjmp(c.jb)) {
        if (c.pc24)
            nbx_batch(&c, r, sp, (unsigned)g_fp_top & 7u);
        else
            nbx64_batch(&c, r, sp, (unsigned)g_fp_top & 7u);
        ok = (_mm_getcsr() & MX_RANGE) == 0 ? 1 : 2;
    }
    _mm_setcsr(mx);
    if (ok == 2) s_n_fb_range++;
    else if (!ok) s_n_fb_bail++;
    return ok == 1;
}

/* The bytes the batch writes: the five output streams of its pointer table
 * (WaterWorld+0xCCB8.. +0xCCD0, strides +0xCCD4..), nv vertices each. */
static int out_span(uint32_t nv, uint32_t *lo, uint32_t *hi)
{
    static const uint32_t ks[5] = { 0, 2, 4, 5, 6 };   /* +0xCCB8 height, +0xCCC0 normal,
                                                           +0xCCC8, +0xCCCC, +0xCCD0 (mode 0x1D's
                                                           outputs; +0xCCBC is the positions,
                                                           +0xCCC4 mode 2's) */
    uint32_t a = 0xFFFFFFFFu, b = 0, j;
    for (j = 0; j < 5; j++) {
        uint32_t k = ks[j];
        uint32_t p = GU32(WW_VA + 0xCCB8u + 4u * k), s = GU32(WW_VA + 0xCCD4u + 4u * k);
        uint32_t end;
        if (p < 0x00010000u || s > 0x1000u)
            return 0;
        end = p + (nv ? nv - 1u : 0u) * s + 12u;
        if (p < a) a = p;
        if (end > b) b = end;
    }
    if (b <= a || b - a > 0x10000u)
        return 0;
    *lo = a;
    *hi = b;
    return 1;
}

static void restore_regs_ok(const NbRegs *r, uint32_t sp)
{
    g_eax = r->eax; g_ebx = r->ebx; g_ecx = r->ecx; g_edx = r->edx;
    g_esi = r->esi; g_edi = r->edi;
    g_ebp = r->gebp; g_seh_ebp = r->gsebp;
    g_fp_cmp = r->fcmp; g_fp_cc = r->fcc;
    g_esp = sp + NBX_BATCH_RETPOP;
}

/* Verify mode: native first (into guest memory), keep its bytes, put the
 * state back, run the game's function, compare. The game's result stays. */
static void nb_verify(uint32_t sp, uint32_t nv)
{
    static uint8_t out_n[0x10000], pf0[W_PFLAG_SZ], pf_n[W_PFLAG_SZ];
    static uint8_t patch_n[W_PFLAG_SZ][64];
    uint32_t lo, hi, w = GU32(WW_VA), nw, i, bad = 0, prof_depth, prof_time, frame[3];
    NbRegs r0, r;
    int ok;
    if (!out_span(nv, &lo, &hi) || w < 0x00010000u || (nw = GU32(w + W_IDX_NEW)) > 1u) {
        sub_00098130_gen();
        return;
    }
    frame[0] = GU32(sp); frame[1] = GU32(sp + 4); frame[2] = GU32(sp + 8);
    prof_depth = GU32(PROF_DEPTH_VA);
    prof_time = GU32(WW_VA + WW_PROF_TIME);
    memcpy(pf0, GP(w + W_PFLAG + nw * W_PFLAG_SZ), W_PFLAG_SZ);
    r0.eax = g_eax; r0.ebx = g_ebx; r0.ecx = g_ecx; r0.edx = g_edx;
    r0.esi = g_esi; r0.edi = g_edi; r0.gebp = g_ebp; r0.gsebp = g_seh_ebp;
    r0.fcmp = g_fp_cmp; r0.fcc = g_fp_cc;
    r = r0;
    {
        uint8_t *pre = (uint8_t *)malloc(hi - lo);
        if (!pre) { sub_00098130_gen(); return; }
        memcpy(pre, GP(lo), hi - lo);
        ok = nb_run(&r, sp);
        memcpy(out_n, GP(lo), hi - lo);
        memcpy(pf_n, GP(w + W_PFLAG + nw * W_PFLAG_SZ), W_PFLAG_SZ);
        for (i = 0; i < W_PFLAG_SZ; i++)
            if (pf_n[i] != pf0[i])
                memcpy(patch_n[i], GP(w + W_PATCH + nw * W_PATCH_SZ + i * 64u), 64);
        /* put back what the native run changed, then the game's own run */
        memcpy(GP(lo), pre, hi - lo);
        free(pre);
    }
    memcpy(GP(w + W_PFLAG + nw * W_PFLAG_SZ), pf0, W_PFLAG_SZ);
    GU32(PROF_DEPTH_VA) = prof_depth;
    GU32(WW_VA + WW_PROF_TIME) = prof_time;
    GU32(sp) = frame[0]; GU32(sp + 4) = frame[1]; GU32(sp + 8) = frame[2];
    g_esp = sp;
    g_ecx = r0.ecx;
    sub_00098130_gen();
    s_n_verify++;
    if (!ok)
        return;
    for (i = 0; i < hi - lo; i++)
        bad += out_n[i] != ((const uint8_t *)GP(lo))[i];
    for (i = 0; i < W_PFLAG_SZ; i++) {
        const uint8_t *gpf = (const uint8_t *)GP(w + W_PFLAG + nw * W_PFLAG_SZ);
        if (gpf[i] != pf_n[i])
            bad++;
        else if (pf_n[i] != pf0[i] && memcmp(patch_n[i], GP(w + W_PATCH + nw * W_PATCH_SZ + i * 64u), 64))
            bad++;
    }
    /* registers that don't carry the batch's rdtsc timing */
    bad += r.ebx != g_ebx; bad += r.esi != g_esi; bad += r.edi != g_edi;
    bad += r.gebp != g_ebp; bad += r.gsebp != g_seh_ebp;
    bad += r.fcc != g_fp_cc; bad += r.fcmp != g_fp_cmp;
    bad += g_esp != sp + NBX_BATCH_RETPOP;
    bad += GU32(PROF_DEPTH_VA) != prof_depth;
    if (bad) {
        static int shown;
        s_n_vbad++;
        s_n_vbad_bytes += bad;
        if (shown++ < 10) {
            uint32_t first = 0;
            while (first < hi - lo && out_n[first] == ((const uint8_t *)GP(lo))[first])
                first++;
            fprintf(stderr, "[BW] native ocean verify: %u differences (first output byte %u of %u; "
                    "flags 0x%X, %u vertices)\n", bad, first, hi - lo, frame[1], nv);
            if (first + 4 <= hi - lo) {
                float a, b;
                uint32_t o = first & ~3u;
                memcpy(&a, out_n + o, 4);
                memcpy(&b, (const uint8_t *)GP(lo) + o, 4);
                fprintf(stderr, "[BW]   at +0x%X: native %.9g, game %.9g\n", o, a, b);
            }
        }
    }
}

/* Test aid (BW_OCEAN_NATIVE_FUZZ=1, with VERIFY): every 8th batch is
 * verified on a deliberately altered state, to run the branches ordinary play
 * rarely reaches, then the state is put back and the game's function runs
 * again so the frame gets its true result. The arithmetic is the same
 * whatever the values mean, so this checks the translation of those branches
 * against the game's own code:
 *   0  every live main-wake-trail record (type 1, WaterWorld+0x704, 96 x 0x70)
 *      read as type 2 (the second trail's kernel)
 *   1  ... as type 5 (the sinking boat's)
 *   2  the swell off (WaterWorld+0xCCB0 = 0)
 *   3  the batch's flag 0x40 clear (the normal normalised) */
#define WW_TRAIL_A      0x704u
#define WW_TRAIL_A_N    96u
#define WW_REC          0x70u

static void nb_fuzz(uint32_t sp, uint32_t nv, unsigned kind)
{
    static uint8_t flipped[WW_TRAIL_A_N];
    uint32_t frame[3], i, regs[8];
    uint8_t swell = 0;
    uint16_t fcc = g_fp_cc;
    int fcmp = g_fp_cmp;
    frame[0] = GU32(sp); frame[1] = GU32(sp + 4); frame[2] = GU32(sp + 8);
    regs[0] = g_eax; regs[1] = g_ebx; regs[2] = g_ecx; regs[3] = g_edx;
    regs[4] = g_esi; regs[5] = g_edi; regs[6] = g_ebp; regs[7] = g_seh_ebp;
    if (kind <= 1) {
        for (i = 0; i < WW_TRAIL_A_N; i++) {
            uint32_t rec = WW_VA + WW_TRAIL_A + i * WW_REC;
            flipped[i] = GU32(rec) == 1u;
            if (flipped[i])
                GU32(rec) = kind == 0 ? 2u : 5u;
        }
    } else if (kind == 2) {
        swell = *(volatile uint8_t *)GP(WW_VA + 0xCCB0u);
        *(volatile uint8_t *)GP(WW_VA + 0xCCB0u) = 0;
    } else {
        GU32(sp + 4) = frame[1] & ~0x40u;
    }
    nb_verify(sp, nv);
    s_n_fuzz[kind]++;
    /* put the state back, then the game's own run on it */
    if (kind <= 1) {
        for (i = 0; i < WW_TRAIL_A_N; i++)
            if (flipped[i])
                GU32(WW_VA + WW_TRAIL_A + i * WW_REC) = 1u;
    } else if (kind == 2) {
        *(volatile uint8_t *)GP(WW_VA + 0xCCB0u) = swell;
    }
    GU32(sp) = frame[0]; GU32(sp + 4) = frame[1]; GU32(sp + 8) = frame[2];
    g_eax = regs[0]; g_ebx = regs[1]; g_ecx = regs[2]; g_edx = regs[3];
    g_esi = regs[4]; g_edi = regs[5]; g_ebp = regs[6]; g_seh_ebp = regs[7];
    g_fp_cc = fcc; g_fp_cmp = fcmp;
    g_esp = sp;
    sub_00098130_gen();
}

static void nb_log(void)
{
    unsigned long long now = __rdtsc();
    double el = (double)(now - s_t_last) / s_tsc_per_ms;
    if (el < 5000.0)
        return;
    fprintf(stderr, "[BW] native ocean: %.1f s: %lu batches (%.0f/s): native %lu, game %lu "
            "(fallbacks: mode %lu, range %lu, bail %lu; at 53/64-bit precision %lu); "
            "time native %.2f ms/s (%.2f us/batch), game %.2f ms/s (%.2f us/batch)",
            el / 1000.0, s_n_batch, s_n_batch * 1000.0 / el, s_n_native, s_n_game,
            s_n_fb_mode, s_n_fb_range, s_n_fb_bail, s_n_fb_pc,
            s_t_native / s_tsc_per_ms * 1000.0 / el,
            s_n_native ? s_t_native / s_tsc_per_ms * 1000.0 / s_n_native : 0.0,
            s_t_game / s_tsc_per_ms * 1000.0 / el,
            s_n_game ? s_t_game / s_tsc_per_ms * 1000.0 / s_n_game : 0.0);
    if (s_verify) {
        unsigned k, hit = 0;
        const char *cf = getenv("BW_OCEAN_NATIVE_COVER");
        for (k = 0; k < NB_COVER_N; k++)
            hit += s_cov[k] != 0;
        fprintf(stderr, "; verified %lu, mismatching %lu (%lu differences); blocks run %u of %u",
                s_n_verify, s_n_vbad, s_n_vbad_bytes, hit, (unsigned)NB_COVER_N);
        if (s_fuzz)
            fprintf(stderr, "; altered states (total) type2 %lu type5 %lu noswell %lu normalised %lu",
                    s_n_fuzz[0], s_n_fuzz[1], s_n_fuzz[2], s_n_fuzz[3]);
        if (cf && *cf) {                     /* cumulative, rewritten each time */
            FILE *f = fopen(cf, "w");
            if (f) {
                fprintf(f, "# native ocean basic-block coverage (verify mode): %u of %u blocks run\n",
                        hit, (unsigned)NB_COVER_N);
                for (k = 0; k < NB_COVER_N; k++)
                    fprintf(f, "%10lu %s\n", (unsigned long)s_cov[k], nb_cover_names[k]);
                fclose(f);
            }
        }
    }
    fputc('\n', stderr);
    s_t_last = now;
    s_t_native = s_t_game = 0;
    s_n_batch = s_n_native = s_n_game = s_n_fb_mode = s_n_fb_pc = s_n_fb_range = s_n_fb_bail = 0;
    s_n_verify = s_n_vbad = s_n_vbad_bytes = 0;
}

/* Test aid (BW_OCEAN_TILEDUMP=<file>, read-only, P2a.md section 6): from
 * BW_OCEAN_TILEDUMP_AT seconds after the first batch (default 40), the next
 * 600 batches' flags and corner positions (vertex 0 and 24 of the 5x5 tile)
 * plus the first view's camera eye, one line each. */
static FILE *s_td;
static unsigned s_td_n;
static unsigned long long s_td_t0;
static void nb_tiledump(uint32_t sp)
{
    unsigned long long now = GetTickCount64();
    uint32_t base = GU32(WW_VA + 0xCCBCu), stride = GU32(WW_VA + 0xCCD8u);
    float x0, z0, x1, z1, ex, ey, ez;
    if (!s_td_t0)
        s_td_t0 = now;
    if (now - s_td_t0 < 1000ull * env_u("BW_OCEAN_TILEDUMP_AT", 40) || s_td_n >= 600 || GU32(sp + 8) != 25u)
        return;
    if (!s_td) {
        s_td = fopen(getenv("BW_OCEAN_TILEDUMP"), "w");
        if (!s_td) { s_tiledump = 0; return; }
        fprintf(s_td, "# flags x0 z0 x24 z24 eye_x eye_y eye_z\n");
    }
    memcpy(&x0, GP(base), 4); memcpy(&z0, GP(base + 4), 4);
    memcpy(&x1, GP(base + 24 * stride), 4); memcpy(&z1, GP(base + 24 * stride + 4), 4);
    memcpy(&ex, GP(0x001CE4A0u + 0x114u), 4); memcpy(&ey, GP(0x001CE4A0u + 0x118u), 4);
    memcpy(&ez, GP(0x001CE4A0u + 0x11Cu), 4);
    fprintf(s_td, "%X %.4f %.4f %.4f %.4f %.3f %.3f %.3f\n", GU32(sp + 4), x0, z0, x1, z1, ex, ey, ez);
    if (s_td_n == 0) {
        /* the level's static (calm) zones, from every cell's static list
         * (+0xC: nodes {entry, next}; entry: x, z, r_in, r_out, level,
         * 1/(r_out - r_in)), once */
        uint32_t w = GU32(WW_VA + 0xA818u), h = GU32(WW_VA + 0xA81Cu), cells = GU32(WW_VA + 0xA820u);
        uint32_t seen[256], ns = 0, c, k;
        for (c = 0; cells >= 0x00010000u && w < 1000u && h < 1000u && c < w * h; c++) {
            uint32_t node = GU32(cells + c * 16u + 0xCu), guard = 0;
            while (node >= 0x00010000u && guard++ < 64u) {
                uint32_t e = GU32(node);
                for (k = 0; k < ns && seen[k] != e; k++) {}
                if (k == ns && ns < 256u) {
                    float z[6];
                    seen[ns++] = e;
                    memcpy(z, GP(e), 24);
                    fprintf(s_td, "# zone 0x%08X x %.2f z %.2f r_in %.2f r_out %.2f level %.3f inv %.5f\n",
                            e, z[0], z[1], z[2], z[3], z[4], z[5]);
                }
                node = GU32(node + 4u);
            }
        }
        fprintf(s_td, "# %u zones; grid %u x %u\n", ns, w, h);
        {
            /* the frame's depth tree (Ocean 0x1C6990 +0x90, nodes {tile, left,
             * right}: what Ocean_DrawTileTree draws and records for passes 2
             * and 3) against the filled tile list (+0x8C, count +0x94) */
            const uint32_t oc = 0x001C6990u;
            uint32_t stack[4096], sp2 = 0, nodes = 0, root = GU32(oc + 0x90u);
            uint32_t list = GU32(oc + 0x8Cu), cnt = GU32(oc + 0x94u);
            static uint32_t items[4096];
            uint32_t ni = 0, a, b2, dup = 0;
            if (root >= 0x00010000u)
                stack[sp2++] = root;
            while (sp2 && nodes < 4096u) {
                uint32_t nd = stack[--sp2];
                nodes++;
                if (ni < 4096u)
                    items[ni++] = GU32(nd);
                if (GU32(nd + 4u) >= 0x00010000u && sp2 < 4095u) stack[sp2++] = GU32(nd + 4u);
                if (GU32(nd + 8u) >= 0x00010000u && sp2 < 4095u) stack[sp2++] = GU32(nd + 8u);
            }
            for (a = 0; a < ni; a++)
                for (b2 = a + 1; b2 < ni; b2++)
                    if (items[a] == items[b2]) {
                        float t[4];
                        memcpy(t, GP(items[a]), 16);
                        if (dup++ < 40)
                            fprintf(s_td, "# tree duplicate: tile 0x%08X (%.2f, %.2f, %.2f, size %.3f) at %u and %u\n",
                                    items[a], t[0], t[1], t[2], t[3], a, b2);
                    }
            fprintf(s_td, "# depth tree: %u nodes, %u duplicate pairs; tile list 0x%08X, %u tiles\n",
                    nodes, dup, list, cnt);
        }
    }
    ++s_td_n;               /* closed by bw_ocean_eval after this batch's vertices */
}

/* The game's function, or the same work natively (bw_ocean_eval below). */
static void nb_eval(void)
{
    uint32_t sp = g_esp, flags, nv, prof_depth, prof_time;
    unsigned long long t0 = 0;
    NbRegs r;
    if (s_log)
        t0 = __rdtsc();
    if (s_seamtest)                              /* diagnostic: bicubic heights for every tile */
        GU32(sp + 4) &= ~0x80u;
    if (s_tiledump)
        nb_tiledump(sp);
    if (!s_on) {
        sub_00098130_gen();
        goto timed_game;
    }
    s_n_batch++;
    flags = GU32(sp + 4);
    nv = GU32(sp + 8);
    if ((flags & 0x1Fu) != 0x1Du || nv > 0x1000u) {
        s_n_fb_mode++;
        sub_00098130_gen();
        goto timed_game;
    }
    if (g_fp_control_word & 0x300u)            /* counted: which precision the render runs at */
        s_n_fb_pc++;
    if (s_verify) {
        if (s_fuzz && (++s_fuzz_n & 7u) == 0)
            nb_fuzz(sp, nv, (s_fuzz_n >> 3) & 3u);
        else
            nb_verify(sp, nv);
        goto timed_game;
    }
    r.eax = g_eax; r.ebx = g_ebx; r.ecx = g_ecx; r.edx = g_edx;
    r.esi = g_esi; r.edi = g_edi; r.gebp = g_ebp; r.gsebp = g_seh_ebp;
    r.fcmp = g_fp_cmp; r.fcc = g_fp_cc;
    prof_depth = GU32(PROF_DEPTH_VA);
    prof_time = GU32(WW_VA + WW_PROF_TIME);
    {
        uint32_t ra = GU32(sp), a1 = GU32(sp + 4), a2 = GU32(sp + 8);
        if (nb_run(&r, sp)) {
            restore_regs_ok(&r, sp);
            s_n_native++;
            if (s_log) {
                s_t_native += __rdtsc() - t0;
                nb_log();
            }
            return;
        }
        /* thrown away: the outputs are rewritten by the game's run; a Bezier
         * patch is only marked built after the range check, so a patch from
         * the native run is either exact or rebuilt; the profiler's counters
         * and the call's frame go back */
        GU32(PROF_DEPTH_VA) = prof_depth;
        GU32(WW_VA + WW_PROF_TIME) = prof_time;
        GU32(sp) = ra; GU32(sp + 4) = a1; GU32(sp + 8) = a2;
        g_esp = sp;
        sub_00098130_gen();
    }
timed_game:
    s_n_game++;
    if (s_log) {
        s_t_game += __rdtsc() - t0;
        nb_log();
    }
}

/* Test aids for the distant-water line (P2a.md section 6), Remastered only,
 * never defaults:
 *   BW_OCEAN_PASSES=<mask>  the game's own developer toggles for the three
 *                           ocean passes, set before each tile's batch:
 *                           bit 0 [0x18F534] pass 1 (Ocean_DrawTileTree),
 *                           bit 1 [0x18F535] pass 2 (sky reflection),
 *                           bit 2 [0x18F536] pass 3 (wake foam). All four
 *                           readers are in the ocean's draw code (seen);
 *                           the XBE's defaults are 1.
 *   BW_OCEAN_TINT=1         tile-size rings: the normals of every tile whose
 *                           size is an odd number of doublings above 3.125 m
 *                           are tilted (x + 1.5), so the rings show as
 *                           shading bands. Changes the picture. */
static int s_passtest, s_tint, s_cubetest, s_nodist;
static unsigned s_flatten;
static unsigned s_passes = 7u;

/*   BW_OCEAN_CUBETEST=1     the sky cube the reflection pass samples
 *                           ([[0x1C2874]], Skymaps +0x10; 256x256 DXT1, one
 *                           level per face, faces in +X -X +Y -Y +Z -Z
 *                           order) gets one solid colour per face, written
 *                           once into its texture memory: +X red, -X green,
 *                           +Y blue, -Y yellow, +Z magenta, -Z cyan. Face
 *                           boundaries then show as colour edges in the
 *                           water's reflection (and in the sky, if the sky
 *                           draws it). Changes the picture. */
extern uint32_t xbox_ContiguousAllocatedBytes(void);
static void nb_cubetest(void)
{
    static uint32_t done;
    static const uint16_t col[6] = { 0xF800u, 0x07E0u, 0x001Fu, 0xFFE0u, 0xF81Fu, 0x07FFu };
    uint32_t pp = GU32(0x001C2874u), tex, data, va, f, i;
    if (pp < 0x00010000u || (tex = GU32(pp)) < 0x00010000u || tex == done)
        return;
    data = GU32(tex + 4u);
    va = data < xbox_ContiguousAllocatedBytes() ? 0x80000000u + data : data;
    for (f = 0; f < 6u; f++)
        for (i = 0; i < 32768u / 8u; i++) {
            uint32_t b = va + f * 32768u + i * 8u;
            GU32(b) = (uint32_t)col[f] | ((uint32_t)col[f] << 16);
            GU32(b + 4u) = 0;
        }
    done = tex;
    fprintf(stderr, "[BW] ocean cube test: texture 0x%08X data 0x%08X -> 0x%08X painted%c", tex, data, va, 10);
}

/* WaterWorld_EvaluateVertexBatch for Ocean_FillVertices's call (the caller,
 * sub_00098130 in bw_hifps_hooks.c, has checked the return address 0x37CC2
 * and this = WaterWorld). */
void bw_ocean_eval(void)
{
    uint32_t nrm, nstride, pos, pstride, nv, i;
    if (!s_init) {
        nb_init();
        s_passtest = bw_edition_at_least(BW_ED_REMASTERED) && getenv("BW_OCEAN_PASSES")
            && *getenv("BW_OCEAN_PASSES");
        if (s_passtest)
            s_passes = env_u("BW_OCEAN_PASSES", 7);
        s_tint = bw_edition_at_least(BW_ED_REMASTERED) && env_u("BW_OCEAN_TINT", 0) == 1;
        s_cubetest = bw_edition_at_least(BW_ED_REMASTERED) && env_u("BW_OCEAN_CUBETEST", 0) == 1;
        /* BW_OCEAN_NODIST=1..7 (diagnostic): for the render batch only, one
         * of the water world's switches (+0xCCB0 + n) reads 0, or (7) the
         * second trail's records are skipped; put back right after the
         * batch, so no other water query sees it. */
        s_nodist = bw_edition_at_least(BW_ED_REMASTERED) ? (int)env_u("BW_OCEAN_NODIST", 0) : 0;
        s_flatten = bw_edition_at_least(BW_ED_REMASTERED) ? env_u("BW_OCEAN_FLATTEN", 0) : 0;
    }
    if (s_cubetest)
        nb_cubetest();
    if (s_tiledump) {
        /* the dump window (nb_tiledump): each batch's 25 evaluated vertices
         * (x, z, height) after its tile line */
        uint32_t before = s_td_n, hp = GU32(WW_VA + 0xCCB8u), hs = GU32(WW_VA + 0xCCD4u);
        pos = GU32(WW_VA + 0xCCBCu);  pstride = GU32(WW_VA + 0xCCD8u);
        nv = GU32(g_esp + 8);
        nb_eval();
        if (s_td && s_td_n != before && nv <= 64u)
            for (i = 0; i < nv; i++) {
                float x, z, h;
                memcpy(&x, GP(pos + i * pstride), 4);
                memcpy(&z, GP(pos + i * pstride + 4), 4);
                memcpy(&h, GP(hp + i * hs), 4);
                fprintf(s_td, "V %.4f %.4f %.6f%c", x, z, h, 10);
            }
        if (s_td && s_td_n >= 600u) {
            fclose(s_td);
            s_td = NULL;
        }
        return;
    }
    if (s_passtest) {
        *(volatile uint8_t *)GP(0x0018F534u) = (uint8_t)(s_passes & 1u);
        *(volatile uint8_t *)GP(0x0018F535u) = (uint8_t)((s_passes >> 1) & 1u);
        *(volatile uint8_t *)GP(0x0018F536u) = (uint8_t)((s_passes >> 2) & 1u);
    }
    if (s_nodist == 7) {
        /* 7: type 2 (the second trail: 192 records at +0x3808) has no switch
         * of its own; its live records read as type 6, which the vertex code
         * skips (types above 5), for the render batch only */
        static uint8_t was2[192];
        uint32_t k;
        for (k = 0; k < 192u; k++) {
            uint32_t rec = WW_VA + 0x3808u + k * 0x70u;
            was2[k] = GU32(rec) == 2u;
            if (was2[k])
                GU32(rec) = 6u;
        }
        nb_eval();
        for (k = 0; k < 192u; k++)
            if (was2[k])
                GU32(WW_VA + 0x3808u + k * 0x70u) = 2u;
        return;
    }
    if (s_nodist >= 1 && s_nodist <= 6) {
        /* n -> byte WaterWorld+0xCCB0+n: 1 the static (calm) zones, 2 type 0
         * (ripples), 3 type 1 (main wake trails), 4 type 3 (projectile
         * splashes), 5 type 4 (the boats' line), 6 type 5 (sinking) */
        volatile uint8_t *b = (volatile uint8_t *)GP(WW_VA + 0xCCB0u + (uint32_t)s_nodist);
        uint8_t old = *b;
        *b = 0;
        nb_eval();
        *b = old;
        return;
    }
    if (!s_tint && !s_flatten) {
        nb_eval();
        return;
    }
    pos = GU32(WW_VA + 0xCCBCu);  pstride = GU32(WW_VA + 0xCCD8u);
    nrm = GU32(WW_VA + 0xCCC0u);  nstride = GU32(WW_VA + 0xCCDCu);
    nv = GU32(g_esp + 8);
    {
        uint32_t hp = GU32(WW_VA + 0xCCB8u), hs = GU32(WW_VA + 0xCCD4u);
        nb_eval();
        if (s_flatten && nv >= 2u && nv <= 64u) {
            /* BW_OCEAN_FLATTEN=<m> (diagnostic): tiles at least that size
             * get height 0 at every vertex (normals kept) */
            float x0, x1, zero = 0.0f;
            memcpy(&x0, GP(pos), 4);
            memcpy(&x1, GP(pos + (nv - 1u) * pstride), 4);
            if (fabsf(x1 - x0) >= (float)s_flatten - 0.01f)
                for (i = 0; i < nv; i++)
                    memcpy(GP(hp + i * hs), &zero, 4);
        }
    }
    if (s_tint && nv >= 2u && nv <= 64u) {
        float x0, x1, size;
        int level;
        memcpy(&x0, GP(pos), 4);
        memcpy(&x1, GP(pos + (nv - 1u) * pstride), 4);
        size = fabsf(x1 - x0);
        level = size > 0.0f ? (int)floor(log2(size / 3.125) + 0.5) : 0;
        if (level & 1)
            for (i = 0; i < nv; i++) {
                float nx;
                memcpy(&nx, GP(nrm + i * nstride), 4);
                nx += 1.5f;
                memcpy(GP(nrm + i * nstride), &nx, 4);
            }
    }
}
