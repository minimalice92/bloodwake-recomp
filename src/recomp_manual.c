/**
 * Manual function overrides and ICALL diagnostics
 *
 * This file provides:
 *   - recomp_lookup_manual()  : intercept specific Xbox VAs with hand-written code
 *   - recomp_icall_fail_log() : log when an indirect call target can't be resolved
 *   - ICALL trace ring buffer  : globals used by the RECOMP_ICALL macro
 *
 * The recomp pipeline generates an auto-dispatch table (recomp_lookup) that
 * resolves most function addresses. recomp_lookup_manual() is called FIRST,
 * giving you a chance to override any function with a custom implementation.
 *
 * Common reasons to add manual overrides:
 *   - Trace a function to understand call flow (wrap the generated version)
 *   - Fix a function the lifter translated incorrectly
 *   - Stub out a function that crashes (return early, set eax to a safe value)
 *   - Redirect a function to a native implementation (e.g., skip CRT init)
 *   - Intercept D3D/audio calls for custom rendering or sound
 */

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>   /* ptrdiff_t */
#include <stdlib.h>   /* getenv */
#include <string.h>   /* strcmp */

/* ── ICALL trace ring buffer ───────────────────────────────── */

/*
 * These globals are written by the RECOMP_ICALL macro (defined in
 * recomp_types.h) every time an indirect call is dispatched. When a
 * crash occurs, the VEH handler or recomp_icall_fail_log() can dump
 * the last 16 call targets to help you trace what happened.
 *
 * The runtime owns them: xbox_kernel defines all three in
 * src/kernel/xbox_memory_layout.c, and recomp_types.h declares them extern.
 * Declare, do not define -- a definition here as well is a duplicate symbol,
 * and a project copied from this template failed to link on all three:
 *
 *   xbox_memory_layout.obj : error LNK2005: g_icall_count already defined
 *                            in recomp_manual.obj
 */
extern volatile uint32_t g_icall_trace[16];
extern volatile uint32_t g_icall_trace_idx;
extern volatile uint64_t g_icall_count;

typedef void (*recomp_func_t)(void);

/* ── Register state (defined in xbox_memory_layout.c) ──────── */

/* Blood Wake: the template declared `extern uint32_t g_eax;` here. The runtime
 * defines the guest registers thread-local, and main.c explains why a plain
 * extern then reads the wrong copy. Nothing here uses g_eax yet. An override
 * that needs it must declare it `extern RECOMP_TLS uint32_t g_eax;` (with
 * <xbox/xboxrecomp.h>), as main.c does. TOOLKIT_ISSUES TK-7. */
extern ptrdiff_t g_xbox_mem_offset;

/* ── Manual function overrides ─────────────────────────────── */

/*
 * Return a function pointer to override the given Xbox VA, or NULL
 * to fall through to the auto-generated dispatch table.
 *
 * This is called on every indirect call (RECOMP_ICALL) and every
 * direct call through the dispatch table, so keep it fast. A chain
 * of if-statements on uint32_t compiles to a simple comparison
 * sequence; for large override tables, consider a sorted array
 * with binary search.
 *
 * Examples of common override patterns:
 *
 *   // Trace wrapper: log entry/exit around the generated function
 *   extern void sub_00012345(void);
 *   static void traced_sub_00012345(void) {
 *       fprintf(stderr, "[TRACE] sub_00012345 entered, eax=0x%08X\n", g_eax);
 *       sub_00012345();
 *       fprintf(stderr, "[TRACE] sub_00012345 returned, eax=0x%08X\n", g_eax);
 *   }
 *
 *   // Stub: skip a function entirely (return 0 in eax)
 *   static void stub_00067890(void) {
 *       g_eax = 0;
 *   }
 *
 *   // Fix: replace a broken lifted function with correct C
 *   static void fixed_sub_000ABCDE(void) {
 *       // Read arguments from stack/registers per calling convention
 *       uint32_t arg1 = g_ecx;
 *       uint32_t arg2 = MEM32(g_esp + 4);
 *       // ... correct implementation ...
 *       g_eax = result;
 *   }
 */
recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
    /*
     * TODO: Add your overrides here. Examples:
     *
     * if (xbox_va == 0x00012345) return traced_sub_00012345;
     * if (xbox_va == 0x00067890) return stub_00067890;
     * if (xbox_va == 0x000ABCDE) return fixed_sub_000ABCDE;
     */

    (void)xbox_va;
    return (recomp_func_t)0;
}

/* ── ICALL failure logging ─────────────────────────────────── */

/*
 * Called when RECOMP_ICALL cannot resolve a target address.
 * This usually means one of:
 *   - A vtable dispatch to an address not in the dispatch table
 *   - A function pointer loaded from uninitialized or corrupt memory
 *   - A kernel thunk address that the bridge doesn't handle
 *
 * During early bring-up you will see many of these. Most are harmless
 * (the ICALL macro pops the dummy return address and continues).
 * Focus on the ones that cause crashes or incorrect behavior.
 */
void recomp_icall_fail_log(uint32_t va)
{
    fprintf(stderr, "[ICALL] Failed to resolve VA 0x%08X (total calls: %llu)\n",
            va, (unsigned long long)g_icall_count);

    /* Dump last 16 call targets from the ring buffer */
    fprintf(stderr, "  Recent ICALL targets:\n");
    for (int i = 0; i < 16; i++) {
        int idx = (g_icall_trace_idx - 16 + i) & 15;
        if (g_icall_trace[idx])
            fprintf(stderr, "    [%2d] 0x%08X\n", i, g_icall_trace[idx]);
    }
    fflush(stderr);
}

/* An indirect call whose target is not code: a null or wild function pointer.
 *
 * Skipping these is right -- calling a data address is worse -- but skipping
 * them *silently* is not. They almost always arrive inside a loop, so the
 * symptom is a hang with no output rather than a diagnosable null vtable call.
 *
 * Rate-limited per address: a spin can produce millions of these, and the
 * useful information is which addresses occur, not how often.
 */
extern __declspec(thread) uint32_t g_esp;   /* the guest stack pointer (RECOMP_TLS) */

void recomp_icall_not_code_log(uint32_t va)
{
    enum { SLOTS = 16 };
    static uint32_t seen[SLOTS];
    static uint64_t hits[SLOTS];
    static int count;
    int i;

    for (i = 0; i < count; i++)
        if (seen[i] == va)
            break;
    if (i == count) {
        if (count == SLOTS)
            return;
        seen[count] = va;
        hits[count] = 0;
        count++;
    }
    hits[i]++;
    /* Report at 1, 10, 100, 1000 ... rather than once. A single line says a
     * wild pointer was skipped; the progression says it is being skipped in a
     * loop, which is the difference between a curiosity and the reason the
     * title is hung. */
    {
        uint64_t n = hits[i];
        while (n >= 10 && n % 10 == 0)
            n /= 10;
        if (n != 1)
            return;
    }
    fprintf(stderr, "[ICALL] target 0x%08X is not code -- skipped %llu time(s) "
                    "(null or wild function pointer, at call #%llu)\n",
            va, (unsigned long long)hits[i],
            (unsigned long long)g_icall_count);
    /* Blood Wake (M2): where it came from. RECOMP_ICALL's contract is that the
     * caller has already pushed the guest return address, so [esp] names the
     * call site (the address just after the call). The next code-looking words
     * up the stack sketch the chain, as the watchdog's scan does: an over-
     * report, since stale return addresses linger, so read the first few. */
    if (g_xbox_mem_offset && g_esp) {
        const uint32_t *sp = (const uint32_t *)((uintptr_t)g_esp + g_xbox_mem_offset);
        int k, shown = 0;
        fprintf(stderr, "        args [esp+4]=0x%08X [esp+8]=0x%08X guest esp=0x%08X\n",
                sp[1], sp[2], g_esp);
        fprintf(stderr, "        return address 0x%08X; up the stack:", sp[0]);
        for (k = 1; k < 64 && shown < 8; k++) {
            if (sp[k] >= 0x00011000u && sp[k] < 0x0017459Cu) {   /* .text .. XPP */
                fprintf(stderr, " %08X", sp[k]);
                shown++;
            }
        }
        fprintf(stderr, "\n");
    }
    fflush(stderr);
}

/* ── Blood Wake: hooks around generated game functions (D-011) ───────────
 *
 * Each hook is a wrapper: it observes or tells the runtime something, then
 * runs the game's own generated body (sub_X_gen) unchanged. No game logic is
 * re-implemented here. run_pipeline.py passes this file to tools.recomp with
 * --exclude-manual, which emits the wrapped body as sub_X_gen and leaves
 * every caller calling sub_X. */

/* Guest memory, read exactly as the generated MEM32 does (recomp_types.h). */
#define BW_MEM32(va) (*(volatile uint32_t *)((uintptr_t)(uint32_t)(va) + g_xbox_mem_offset))

/* D3DDevice_SetGammaRamp, 0x00124F80 (flags, D3DGAMMARAMP *), stdcall
 * `ret 8`; only caller Graphics_SetGamma. Seen in its code: it copies the
 * 768-byte ramp (red, green, blue planes) into the miniport and writes it to
 * the DAC palette byte by byte (D3D_WriteGammaRampToDac). The toolkit's NV2A
 * registers are plain memory, so the ramp never reached anything. This hands
 * a copy to the presenter (src/gpu/bw_gamma.c) and then runs the body as
 * before. The missions fade in through this ramp (see bw_gamma.c). */
extern __declspec(thread) uint32_t g_esp;
extern void sub_00124F80_gen(void);
extern void bw_gamma_from_title(const uint8_t *ramp);

void sub_00124F80(void)
{
    uint32_t ramp = BW_MEM32(g_esp + 8);
    if (ramp)
        bw_gamma_from_title((const uint8_t *)((uintptr_t)ramp + g_xbox_mem_offset));
    sub_00124F80_gen();
}

/* D3D's wait-for-GPU-time, 0x0012CB40 (XDK 3948, statically linked).
 *
 * Seen in its code and in run 20261025-214952-r1-boot (ANALYSIS "First run"):
 * it reads D3D's device pointer from 0x00136B00 and spins on the GPU time at
 * *(device + 0x3F0) (0x80001000 in that run) until it reaches the fence. The
 * GPU advances that word with BACK_END_WRITE_SEMAPHORE_RELEASE (0x1D70), and
 * the toolkit's pushbuffer executor writes those releases only to an address
 * the project has handed it (nv2a_backend.h). So on the first call, hand it
 * over, then wait exactly as the game does. Burnout 3 replaced this function
 * for the same reason (toolkit d3d8ltcg-device-context.md); its D3D keeps
 * the pointer at device+0x30. Without RECOMP_PB_EXEC nothing writes the
 * releases, and the wait spins as before. */
#define BW_D3D_DEVICE_PTR_VA   0x00136B00u
#define BW_D3D_GPU_TIME_OFFSET 0x3F0u
extern void sub_0012CB40_gen(void);
extern void nv2a_pb_set_semaphore_target(uint32_t guest_va);

#include "bw_framelog.h"   /* P2b: BW_FRAMELOG test aid */
void sub_0012CB40(void)
{
    static uint32_t registered;
    unsigned long long fl_t0 = 0;
    uint32_t fl_fence = 0, fl_gpu = 0;
    if (!registered) {
        uint32_t dev = BW_MEM32(BW_D3D_DEVICE_PTR_VA);
        uint32_t sem = dev ? BW_MEM32(dev + BW_D3D_GPU_TIME_OFFSET) : 0;
        if (sem) {
            nv2a_pb_set_semaphore_target(sem);
            registered = sem;
            fprintf(stderr, "[BW] D3D GPU time at 0x%08X (device 0x%08X): "
                    "handed to the pushbuffer executor as its semaphore target\n",
                    sem, dev);
            fflush(stderr);
        }
    }
    if (g_bw_fl_on) {
        uint32_t dev = BW_MEM32(BW_D3D_DEVICE_PTR_VA);
        fl_fence = BW_MEM32(g_esp + 4);
        fl_gpu = dev && BW_MEM32(dev + BW_D3D_GPU_TIME_OFFSET)
                 ? BW_MEM32(BW_MEM32(dev + BW_D3D_GPU_TIME_OFFSET)) : 0;
        fl_t0 = bw_fl_ticks();
    }
    sub_0012CB40_gen();
    if (fl_t0)
        bw_fl_event(BW_FL_G_BOT, bw_fl_us_since(fl_t0), fl_fence, fl_gpu);
}

/* How far the GPU is behind a fence, 0x0012C8D0 (XDK 3948), called only by
 * the wait above. It returns the pushbuffer bytes between the GPU's read
 * position and the fence (0 once passed).
 *
 * Seen in run 20261026-145009-r6-gpu-oc2pW (2-player Open Contest, which froze
 * seconds in): below 0x8000 the wait polls the GPU time until it reaches the
 * fence. At 0x8000 or more it takes the "GPU far behind" path instead: it
 * patches WAIT_FOR_IDLE and NO_OPERATION 0x310 into the fence, then sleeps on
 * the event at device + 0x2500 (KeWaitForSingleObject). On the console the
 * GPU traps that NOP, and D3D's graphics interrupt (0x0012EE80, software
 * method handler 0x0012EC20, case 0x310) sets the event. Nothing here
 * delivers that interrupt (the toolkit holds PGRAPH_INTR at 0), so the title
 * slept for good. Two split-screen views plus AI boats make a frame's
 * pushbuffer long enough to reach the far path; one view never did.
 *
 * Both paths return at the same moment: when the GPU time passes the fence.
 * So a far reading is reported as near (0x7FFF), and the wait polls. The
 * title's own code does all the waiting. BW_D3D_FARWAIT=event restores the
 * original reading (and the freeze). */
extern void sub_0012C8D0_gen(void);
extern __declspec(thread) uint32_t g_eax;   /* RECOMP_TLS: TK-7 */

void sub_0012C8D0(void)
{
    static int mode = -1;               /* 1: report far as near */
    static long clamps;
    sub_0012C8D0_gen();
    if (mode < 0) {
        const char *e = getenv("BW_D3D_FARWAIT");
        mode = !(e && !strcmp(e, "event"));
    }
    if (mode && g_eax >= 0x8000u) {
        long n = ++clamps;              /* the game's main thread only */
        if (n <= 3 || (n & (n - 1)) == 0) {
            fprintf(stderr, "[BW] D3D fence wait: GPU %u bytes behind; polling the GPU "
                    "time instead of sleeping on its interrupt (%ld so far)\n", g_eax, n);
            fflush(stderr);
        }
        g_eax = 0x7FFFu;
    }
}

/* crt_srand, 0x000E00B6 (cdecl: seed at [esp+4]). System_Init (0x000AB970)
 * seeds rand() from the UTC time of day in milliseconds (seen 2026-10-26,
 * agent R6: the ocean's random wave table recovered to the two boot moments),
 * so no two runs are alike. BW_SEED=<n> replaces the seed, so tests with the
 * same inputs can be compared exactly. Test aid only: unset, the game's own
 * seed passes through and Original is unchanged. */
extern void sub_000E00B6_gen(void);
extern __declspec(thread) uint32_t g_esp;

void sub_000E00B6(void)
{
    static int init;
    static int have;
    static uint32_t seed;
    if (!init) {
        const char *e = getenv("BW_SEED");
        init = 1;
        if (e && *e) {
            have = 1;
            seed = (uint32_t)strtoul(e, NULL, 0);
            fprintf(stderr, "[BW] BW_SEED: rand() seeded with %u instead of the time of day\n", seed);
            fflush(stderr);
        }
    }
    if (have)
        BW_MEM32(g_esp + 4) = seed;
    sub_000E00B6_gen();
}

/* The system library's launch-a-new-image, 0x000DB8B9 (path, launch data),
 * stdcall: `ret 8`. Callers: the game's own relaunch (0x000AAE70), game
 * main's exit (0x000AB42A) and the boot-to-dashboard path (0x000DEA79).
 *
 * Seen in run 20261025-222206-r2-pixels: after its first stage Blood Wake
 * relaunches its own default.xbe with launch data "SPLASH", 1 (the studio's
 * project name; see the XBE debug path). A console reboots into the title with
 * the launch data page intact. The toolkit prints the page and exits, so this
 * records the request in bw_relaunch.bin (working folder) before the game's
 * own code runs and exits. tools/bw_run.py then starts the next stage with
 * BW_LAUNCH_DATA_FILE, and main.c rebuilds the page from it.
 *
 * File: "BWRL", u32 version 1, u32 path length, path bytes, u32 data length
 * (3072, or 0 for no launch data), data bytes. */
extern __declspec(thread) uint32_t g_esp;
extern void sub_000DB8B9_gen(void);
#define BW_LAUNCH_DATA_SIZE 3072u
__declspec(dllimport) void __stdcall Sleep(unsigned long ms);
__declspec(dllimport) unsigned long __stdcall GetLastError(void);
__declspec(dllimport) int __stdcall DeleteFileA(const char *name);
__declspec(dllimport) int __stdcall MoveFileExA(const char *from, const char *to,
                                                unsigned long flags);

void sub_000DB8B9(void)
{
    uint32_t path_va = BW_MEM32(g_esp + 4);
    uint32_t data_va = BW_MEM32(g_esp + 8);
    char path[260];
    uint32_t n = 0;
    FILE *f;

    while (path_va && n < sizeof(path) - 1) {
        char c = *(volatile char *)((uintptr_t)(path_va + n) + g_xbox_mem_offset);
        if (!c)
            break;
        path[n++] = c;
    }
    path[n] = 0;

    /* Every write is checked (0.2.5 item 58): a failed one used to be logged
     * as recorded, and the host then found no request and said the game
     * stopped. The request goes to a temporary file first and is renamed into
     * place, so the host never finds half of one; a held file (an antivirus
     * scan) or a full disk is tried again for up to 10 s. */
    {
        uint32_t ver = 1, dlen = data_va ? BW_LAUNCH_DATA_SIZE : 0;
        int tries, ok = 0, warned = 0;
        unsigned long err = 0;
        const char *what = "";
        for (tries = 1; tries <= 100; tries++) {
            if (tries > 1)
                Sleep(100);
            f = fopen("bw_relaunch.tmp", "wb");
            if (!f) {
                err = GetLastError();
                what = "creating bw_relaunch.tmp";
            } else {
                ok = fwrite("BWRL", 1, 4, f) == 4 && fwrite(&ver, 4, 1, f) == 1
                     && fwrite(&n, 4, 1, f) == 1 && fwrite(path, 1, n, f) == n
                     && fwrite(&dlen, 4, 1, f) == 1
                     && (!dlen || fwrite((const void *)((uintptr_t)data_va + g_xbox_mem_offset),
                                         1, dlen, f) == dlen);
                if (fclose(f) != 0)
                    ok = 0;
                if (!ok) {
                    err = GetLastError();
                    what = "writing bw_relaunch.tmp";
                    DeleteFileA("bw_relaunch.tmp");
                } else if (!MoveFileExA("bw_relaunch.tmp", "bw_relaunch.bin", 1 /* REPLACE_EXISTING */)) {
                    err = GetLastError();
                    what = "renaming it to bw_relaunch.bin";
                    ok = 0;
                }
            }
            if (ok)
                break;
            if (!warned++)
                fprintf(stderr, "[BW] relaunch request: %s failed (Windows error %lu); trying "
                        "again for up to 10 s\n", what, err);
        }
        if (ok)
            fprintf(stderr, "[BW] relaunch requested: path='%s' launch data %s"
                    " (first bytes %08X %08X %08X), recorded in bw_relaunch.bin%s\n",
                    path, data_va ? "present" : "none",
                    data_va ? BW_MEM32(data_va) : 0, data_va ? BW_MEM32(data_va + 4) : 0,
                    data_va ? BW_MEM32(data_va + 8) : 0,
                    tries > 1 ? " (on a retry)" : "");
        else {
            DeleteFileA("bw_relaunch.tmp");
            fprintf(stderr, "[BW] relaunch requested: path='%s', but bw_relaunch.bin could not "
                    "be written (%s failed, Windows error %lu, 100 tries over 10 s); the reboot "
                    "is lost, so the play host will say the game stopped\n", path, what, err);
        }
    }
    fflush(stderr);
    /* The launch ends in HalReturnToFirmware -> ExitProcess, which skips
     * atexit: write the sampling profile (BW_PROFILE) now. */
    {
        extern void bw_profile_dump(const char *why);
        extern void bw_edition_summary(void);
        bw_profile_dump("relaunch");
        bw_edition_summary();             /* "[ED] summary" lines (bw_edition.c) */
    }
    sub_000DB8B9_gen();
}

/* D-011 hook: sub_000FE041 -- DirectSound's wait for a voice's idle report.
 * It spins while [voice+8] has bit 0 and bit 28 (0x10000000) set; bit 28 is
 * cleared by the APU interrupt's idle-voice path (0x000FD27A -> 0x000FC6F8).
 * The loop runs at raised IRQL and makes no kernel call. Under the toolkit's
 * one-CPU dispatch gate (RECOMP_DISPATCH_GATE=1), an interrupt that arrives
 * while a thread is raised is posted to that thread and runs at its next
 * safe point, which this loop never reaches: the game hung (ANALYSIS, "the
 * DirectSound stall"). On the console the interrupt preempts the loop. Here
 * the wait becomes a safe point: while the flag is up, what is posted to
 * this thread runs (xbox_IrqSafePoint), as the CPU would take it; then the
 * game's own body runs, and returns at once. Without the gate there is
 * nothing posted and this adds one check. */
extern __declspec(thread) uint32_t g_ecx;
extern void sub_000FE041_gen(void);
extern void xbox_IrqSafePoint(void);
__declspec(dllimport) int __stdcall SwitchToThread(void);
__declspec(dllimport) unsigned long long __stdcall GetTickCount64(void);
extern void DSound_ApuOnVoiceIdle_000FD27A(void);         /* generated (0xFD27A) */
/* Without names/bw_names.json (the public build) it is the address name alone;
 * the linker falls back to it (split so the hook scans don't count it). */
#pragma comment(linker, "/alternatename:DSound_ApuOnVoiceIdle_000FD27A=sub" "_000FD27A")
extern void xbox_DispatchGateEnter(void);
extern void xbox_DispatchGateLeave(void);
extern __declspec(thread) uint32_t g_edx;
extern void *g_state;                                     /* the APU (apu_core.c) */
extern uint64_t mcpx_apu_mmio_read(void *d, uint64_t addr, unsigned int size);

/* Safety net (D3, 2026-10-27): a voice whose idle report can never come.
 *
 * The report comes from the VP's walk of the hardware voice list the voice is
 * on. If the list was cut (seen in run 20261027-024836-r6-gpu-D1m05b's dump:
 * the stuck voice, handle 0x6A, sat on an orphaned chain no list register
 * reached, after two threads edited DirectSound's voice lists at once; cause
 * TK-37), nothing ever walks the voice and the game froze here for good.
 *
 * After 250 ms of waiting, the voice's handle is looked up in its hardware
 * list (DirectSound's list index +0xDC, list top NV_PAPU 0x2054 + 12 * index,
 * links at voice memory [apu+0x30] + handle * 128 + 0x7C). If it is not
 * there, or after 3 s in any case, the wait ends the way DirectSound's own
 * idle path (DSound_ApuUnlinkIdleVoice 0xFC6F8) ends it, minus the hardware
 * relink (the voice is not on a reachable list): the soft-list entry +0xBC
 * is removed and reset, +0xDC = 0xFF (on no list), and the wait bit 28 is
 * cleared. Logged loudly. This only runs in a state the game cannot get out
 * of on its own; it never changes a wait that ends normally.
 * BW_VOICE_WAIT_RECOVER=0 restores waiting forever. */
static int bw_voice_on_hw_list(uint32_t obj, uint32_t *handle, uint32_t *list, uint32_t *top)
{
    uint32_t apu = BW_MEM32(obj + 0xC), vmem, v, k;
    *handle = BW_MEM32(obj + 0x10) & 0xFFFF;
    *list = BW_MEM32(obj + 0xDC) & 0xFF;
    *top = 0xFFFF;
    if (*list > 2 || !apu || !g_state)
        return -1;                                      /* unknown */
    vmem = BW_MEM32(apu + 0x30);
    *top = (uint32_t)mcpx_apu_mmio_read(g_state, 0x2054 + 12 * *list, 4) & 0xFFFF;
    for (v = *top, k = 0; v != 0xFFFF && k < 256; k++) {
        if (v == *handle)
            return 1;
        v = BW_MEM32(vmem + v * 128 + 0x7C) & 0xFFFF;
    }
    return 0;
}

/* The two waits (ms): before the reachability check (250), and before a
 * reachable voice gets its report delivered (3000). Test switches
 * BW_VOICE_WAIT_CHECK_MS / BW_VOICE_WAIT_REACH_MS (0 exercises the paths on
 * every wait that spins). */
static unsigned long long bw_voice_wait_ms(int which)
{
    static long long ms[2] = { -1, -1 };
    if (ms[which] < 0) {
        const char *e = getenv(which ? "BW_VOICE_WAIT_REACH_MS" : "BW_VOICE_WAIT_CHECK_MS");
        ms[which] = e && *e ? atoll(e) : (which ? 3000 : 250);
    }
    return (unsigned long long)ms[which];
}

static int bw_voice_wait_recover_locked(uint32_t obj, unsigned long long waited_ms)
{
    static int on = -1;
    static long recovered;
    uint32_t handle, list, top, e, fl, bl;
    int reach;

    if (on < 0) {
        const char *s = getenv("BW_VOICE_WAIT_RECOVER");
        on = !(s && s[0] == '0');
    }
    if (!on)
        return 0;
    reach = bw_voice_on_hw_list(obj, &handle, &list, &top);
    if (reach == 1 && waited_ms < bw_voice_wait_ms(1))
        return 0;                                       /* still walked: its report can come */
    if (reach == 1) {
        /* Still on its hardware list after 3 s: hand DirectSound the idle
         * report itself, through its own handler (DSound_ApuOnVoiceIdle
         * 0xFD27A, ecx = the APU object, the voice's last channel-pair
         * handle), under the one-CPU gate as the interrupt would run. It
         * unlinks the voice from both lists consistently and clears the wait
         * bit, or declines while DirectSound holds the lists (+0x1C8), and
         * the wait goes on to try again. A soft-only removal here would leave
         * the voice in the hardware list. */
        static unsigned long long last_try;
        static uint32_t last_obj;
        uint32_t apu = BW_MEM32(obj + 0xC), buf = BW_MEM32(obj + 0xE0), last;
        uint32_t s_eax = g_eax, s_ecx = g_ecx, s_edx = g_edx, s_esp = g_esp;
        unsigned pairs;
        if (!apu || !buf || (obj == last_obj && waited_ms - last_try < 100))
            return 0;
        last_obj = obj;
        last_try = waited_ms;
        pairs = (BW_MEM32(BW_MEM32(buf + 0x10)) >> 16) & 0xFFFF;   /* channels */
        pairs = pairs ? (pairs - 1) / 2 + 1 : 1;
        last = BW_MEM32(obj + 0x10 + (pairs - 1) * 4) & 0xFFFF;
        {   /* Only for a voice the VP has stopped: an idle report for one
             * still ACTIVE (PAR_STATE +0x54 bit 21; a stop that lets the voice
             * finish, a release envelope) would unlink a playing voice. Seen
             * in the stress run -D3vT1 (thresholds at 0): orphans. */
            uint32_t vmem = BW_MEM32(apu + 0x30), k;
            for (k = 0; k < pairs; k++) {
                uint32_t h = BW_MEM32(obj + 0x10 + k * 4) & 0xFFFF;
                if (h < 256 && (BW_MEM32(vmem + h * 128 + 0x54) & (1u << 21)))
                    return 0;                           /* still playing: wait on */
            }
        }
        g_esp -= 4; BW_MEM32(g_esp) = last;             /* the handle argument */
        g_esp -= 4; BW_MEM32(g_esp) = 0;                /* return address; ret 4 */
        g_ecx = apu;
        DSound_ApuOnVoiceIdle_000FD27A();
        g_eax = s_eax; g_ecx = s_ecx; g_edx = s_edx; g_esp = s_esp;
        fprintf(stderr, "[BW] DirectSound voice-idle wait: voice 0x%08X (handle 0x%X) still "
                "on hardware list %u after %llu ms with no idle report; delivered one "
                "through DirectSound's handler: %s\n", obj, last, list, waited_ms,
                (BW_MEM32(obj + 8) & 0x10000000u) ? "declined (lists held), retrying"
                                                  : "unlinked, wait over");
        fflush(stderr);
        return !(BW_MEM32(obj + 8) & 0x10000000u);
    }
    e = obj + 0xBC;                                     /* DSound_ListRemoveInit (0xFC29F) */
    fl = BW_MEM32(e);
    bl = BW_MEM32(e + 4);
    if (fl != e && fl && bl) {
        BW_MEM32(bl) = fl;
        BW_MEM32(fl + 4) = bl;
        BW_MEM32(e) = e;
        BW_MEM32(e + 4) = e;
    }
    *(volatile uint8_t *)((uintptr_t)(uint32_t)(obj + 0xDC) + g_xbox_mem_offset) = 0xFF;
    BW_MEM32(obj + 8) &= ~0x10000000u;
    fprintf(stderr, "[BW] DirectSound voice-idle wait: voice 0x%08X (handle 0x%X, list %u, "
            "list top 0x%X) waited %llu ms and %s; ended the wait as DirectSound's idle "
            "path would (%ld so far; BW_VOICE_WAIT_RECOVER=0 waits forever)\n",
            obj, handle, list, top, waited_ms,
            reach == 0 ? "is on no reachable hardware list, so its idle report can never come"
                       : "never got its idle report", ++recovered);
    fflush(stderr);
    return 1;
}

/* The decision and the repair under the one-CPU gate, so DirectSound's
 * interrupt (which edits the same lists) can't run beside them: a snapshot
 * taken while its unlink was half done read as "on no list" (stress run
 * -D3vT2, thresholds at 0). The wait bit is checked again inside. */
static int bw_voice_wait_recover(uint32_t obj, unsigned long long waited_ms)
{
    int r;
    xbox_DispatchGateEnter();
    if (!(BW_MEM32(obj + 8) & 0x10000000u))
        r = 1;                                          /* reported meanwhile */
    else
        r = bw_voice_wait_recover_locked(obj, waited_ms);
    xbox_DispatchGateLeave();
    return r;
}

void sub_000FE041(void)
{
    uint32_t obj = g_ecx;
    unsigned spins = 0;
    unsigned long long t0 = 0;

    while ((BW_MEM32(obj + 8) & 1u) && (BW_MEM32(obj + 8) & 0x10000000u)) {
        xbox_IrqSafePoint();
        if (++spins > 64) {
            SwitchToThread();
            if (!t0) {
                t0 = GetTickCount64();
            } else if ((spins & 63) == 0) {
                unsigned long long w = GetTickCount64() - t0;
                if (w >= bw_voice_wait_ms(0) && bw_voice_wait_recover(obj, w))
                    break;
            }
        }
    }
    sub_000FE041_gen();
}

/* D-011 hook: sub_00036540 -- the scene-graph walk (ANALYSIS, "The mission
 * crash"). For each node in the sibling list it was given (2nd stack
 * argument) it draws the node's drawable (+0x4C), walks its child list (+0x44,
 * recursing through here) and follows +0x48 to the next sibling. About one
 * mission run in 30 crashed in it (or in the drawable code under it) on a
 * link holding non-pointer data -- the same value in two runs, so stale data,
 * cause unknown. This checks the list first: a link that cannot be a guest
 * pointer is logged once with its node's bytes and cut (set to 0), so the walk
 * skips what was garbage instead of faulting on it. It changes nothing for a
 * well-formed list. BW_NODE_GUARD=0 turns it off. */
#include <stdlib.h>   /* getenv */
extern void sub_00036540_gen(void);

/* D-011 replacement: sub_0012C9F0 -- XDK D3D's KickOff (D3D section). It
 * sets the PFB flush bit (NV2A register +0x100410, bit 16) and spins until
 * the "hardware" clears it, then writes the push buffer's PUT. The toolkit's
 * ack thread clears that bit -- except while a flip waits for its vblank
 * (RECOMP_FLIP_PACING), which is how the title feels the console's flip
 * stall. So the main thread spun a whole host core for most of every frame
 * (75% of its profile; one core pinned, fans at full). Same register
 * work, instruction for instruction (transcribed from 0x0012C9F0..0x0012CA96),
 * but the wait backs off: a few pause hints, then yields, then 1 ms sleeps,
 * with a safe point for posted interrupts each turn. The registers end as the
 * original leaves them (eax, edx; ecx and esp restored). BW_KICKOFF_SPIN=1
 * runs the original. */
#include <immintrin.h>
#include "bw_wait.h"
#define BW_MEM8(va) (*(volatile uint8_t *)((uintptr_t)(uint32_t)(va) + g_xbox_mem_offset))
extern __declspec(thread) uint32_t g_eax, g_edx;
extern void sub_0012C9F0_gen(void);
/* Segments the game may run ahead of the NV2A walk (TK-56); 0 = the KickOff
 * waits for its own flush, as the toolkit did. Set at boot by bw_hifps_boot
 * in every edition but Original (BW_HIFPS_ASYNC, default 3); otherwise at the first
 * KickOff from BW_KICKOFF_ASYNC (default 3, every edition). -1 = not set. */
int g_bw_kickoff_async = -1;

void sub_0012C9F0(void)
{
    static int orig = -1;
    uint32_t dev = g_ecx, put, regs, n = 0;

    if (orig < 0) {
        const char *e = getenv("BW_KICKOFF_SPIN");
        orig = e && e[0] == '1';
        /* TK-56 (every edition): on the console the PFB flush bit clears
         * after a memory flush, so the KickOff returns at once; the toolkit
         * clears it only after the walk, flip stall included. Waiting for
         * each flush made the game's frame rhythm slip every second or two
         * (60 ms, then 10 ms; 14 double steps in 108 s of mission 1, seen
         * with BW_STEPLOG + BW_KICKLOG). Up to 3 segments in flight keeps
         * every frame at 25-40 ms (4 double steps: the accumulator's own
         * 30.0 against 29.97 Hz); 15 lets the fast host CPU burst two
         * frames ahead. BW_KICKOFF_ASYNC=0 restores the wait. */
        if (g_bw_kickoff_async < 0) {
            e = getenv("BW_KICKOFF_ASYNC");
            g_bw_kickoff_async = (e && *e) ? atoi(e) : 3;
            if (g_bw_kickoff_async < 0)
                g_bw_kickoff_async = 0;
            if (g_bw_kickoff_async > 15)
                g_bw_kickoff_async = 15;
            fprintf(stderr, "[BW] KickOff: up to %d segments in flight "
                    "(TK-56; BW_KICKOFF_ASYNC=0 waits for each flush)\n", g_bw_kickoff_async);
        }
    }
    if (orig) {
        sub_0012C9F0_gen();
        return;
    }
    if (BW_MEM32(0x001338E8u) != 0) {
        /* the other path (0x0012CA5A): no flush */
        uint32_t v = BW_MEM32(BW_MEM32(dev + 0x2304) + 0x44) | 0x80000000u;
        BW_MEM32(dev) = v;
        BW_MEM32(dev + 0x18) = BW_MEM32(dev);
        g_edx = BW_MEM32(dev + 0x1C) - 2;
        BW_MEM32(BW_MEM32(dev + 0x3F0)) = g_edx;
        BW_MEM32(dev + 0x2518) = BW_MEM32(dev + 0x2B60);
        g_eax = dev;
        g_esp += 4;                              /* ret */
        return;
    }
    put = (BW_MEM8(dev + 0xC) & 4) ? BW_MEM32(dev + 0x400) : BW_MEM32(dev);
    regs = BW_MEM32(BW_MEM32(0x00136B00u) + 0x2308);
    if (g_bw_kickoff_async > 0) {
        /* Remastered high frame rate only (bw_hifps_hooks.c sets it; D-016): up to
         * g_bw_kickoff_async segments in flight. The segment is handed over
         * (PUT, then the flush bit) and the KickOff returns as soon as no
         * more than that many of its segments are still unwalked (GET, which
         * the walk sets to the PUT it reached, against the last 16 PUTs), so
         * the game encodes the next segments while the NV2A thread walks
         * these, as the console's CPU and GPU overlap. Measured with the
         * wait-for-each-flush KickOff: ~10 KickOffs a frame and the game
         * thread waiting ~500 ms a second (run 20261027-031012-r6-gpu-d7ko).
         * A held flip stops the walk, so GET stops and the KickOff after the
         * next few waits: the pacing is kept, a few segments later. D3D
         * reuses pushbuffer space only behind GET, and its resource locks
         * wait on the GPU-time fences the walk writes, so nothing is
         * overwritten before it is read. Same registers at the end as below. */
        static uint32_t ring[16];
        static unsigned seq;
        uint32_t user = BW_MEM32(dev + 0x2304), p = put & 0x03FFFFFFu;
        unsigned out, out0 = 99;
        unsigned long long fl_t0 = g_bw_fl_on ? bw_fl_ticks() : 0;
        BW_MEM32(user + 0x40) = p;
        BW_MEM32(dev + 0x18) = put;
        ring[++seq & 15u] = p;
        bw_put_queue_push(p);                    /* P1: the walk per segment (BW_PB_SEGWALK) */
        _mm_sfence();
        BW_MEM32(regs + 0x100410) = BW_MEM32(regs + 0x100410) | 0x10000u;
        bw_ack_kick();
        for (;;) {
            uint32_t get = BW_MEM32(user + 0x44) & 0x03FFFFFFu;
            for (out = 0; out < 16u && ring[(seq - out) & 15u] != get; out++)
                ;
            if (out0 == 99)
                out0 = out;
            if (out <= (unsigned)g_bw_kickoff_async)
                break;
            xbox_IrqSafePoint();
            if (++n < 64)
                _mm_pause();
            else
                bw_flush_wait();
        }
        if (fl_t0)
            bw_fl_event(BW_FL_G_KICK, bw_fl_us_since(fl_t0), out0, seq);
        g_eax = user;
        g_edx = dev;
        g_esp += 4;                              /* ret */
        return;
    }
    {
        /* BW_KICKLOG=1 (diagnostic, every edition): each KickOff whose wait
         * for the NV2A thread is over 5 ms, on the host performance counter
         * (the guest's KeQueryPerformanceCounter is the same clock), so long
         * game frames can be matched against the step log (BW_STEPLOG). */
        static int kicklog = -1;
        double k0 = 0.0;
        if (kicklog < 0) {
            const char *e = getenv("BW_KICKLOG");
            kicklog = e && e[0] == '1';
        }
        if (kicklog)
            k0 = bw_now_ms();
        _mm_sfence();
        BW_MEM32(regs + 0x100410) = BW_MEM32(regs + 0x100410) | 0x10000u;
        bw_ack_kick();                            /* wake the NV2A thread if it naps */
        while (BW_MEM32(regs + 0x100410) & 0x10000u) {
            xbox_IrqSafePoint();
            if (++n < 64)
                _mm_pause();
            else
                bw_flush_wait();      /* the ack's event or 200 us, no spin; Sleep(1)
                                         overshot (34 -> 29 fps); bw_wait.c */
        }
        if (kicklog) {
            double w = bw_now_ms() - k0;
            if (w > 5.0)
                fprintf(stderr, "[KICK] t=%.1f wait=%.1f put=%08X\n",
                        k0, w, put);
        }
    }
    g_eax = BW_MEM32(dev + 0x2304);
    BW_MEM32(g_eax + 0x40) = put & 0x03FFFFFFu;
    BW_MEM32(dev + 0x18) = put;
    g_edx = dev;
    g_esp += 4;                                  /* ret */
}

/* D-011 diagnostic hook: sub_00036260, the drawable renderer the scene walk
 * calls between saving a node's child list on its stack ([esp+0x18] of the
 * walk) and recursing with it. The bad lists are float data, so either a
 * callee pops the wrong number of bytes (the walk then reads its slots from
 * the wrong place) or something writes past its frame into the walk's.
 * After each call: the guest stack pointer must be back exactly 12 bytes up
 * (return address + 2 arguments, ret 8), and the 24 words of the caller's
 * frame above the arguments are compared with a copy taken before (word 6
 * is the walk's saved child list; the matrix the caller passed by pointer
 * may change legitimately). The first 16 differences of each kind are
 * logged. BW_DRAW_CHECK=0 turns it off. */
extern void sub_00036260_gen(void);

void sub_00036260(void)
{
    static int check = -1, drift_reports, frame_reports;
    uint32_t sp0 = g_esp, snap[24], k;

    if (check < 0) {
        const char *e = getenv("BW_DRAW_CHECK");
        check = !(e && e[0] == '0');
    }
    if (!check) {
        sub_00036260_gen();
        return;
    }
    for (k = 0; k < 24; k++)
        snap[k] = BW_MEM32(sp0 + 12 + k * 4);
    sub_00036260_gen();
    if (g_esp != sp0 + 12 && drift_reports < 16) {
        drift_reports++;
        fprintf(stderr, "[BW-DRAW] stack drift: esp 0x%08X before, 0x%08X after (expected 0x%08X);"
                        " drawable 0x%08X\n", sp0, g_esp, sp0 + 12, BW_MEM32(sp0 + 4));
        fflush(stderr);
    }
    for (k = 0; k < 24 && frame_reports < 16; k++) {
        uint32_t now = BW_MEM32(sp0 + 12 + k * 4);
        if (now != snap[k] && k < 8) {          /* past the walk's own slots: its matrix copy */
            frame_reports++;
            fprintf(stderr, "[BW-DRAW] caller frame word %u (esp+0x%X) changed 0x%08X -> 0x%08X%s;"
                            " drawable 0x%08X\n", k, 12 + k * 4, snap[k], now,
                    k == 6 ? " (the walk's saved child list)" : "", BW_MEM32(sp0 + 4));
            fflush(stderr);
        }
    }
}


static int bw_guest_ptr_ok(uint32_t p)
{
    if (p & 3u)
        return 0;
    return (p >= 0x00010000u && p < 0x08000000u)      /* image and heap (128 MB map) */
        || (p >= 0x80000000u && p < 0x84000000u);     /* the contiguous window */
}

static void bw_node_report(uint32_t node, uint32_t off, uint32_t bad)
{
    static int reports;
    int i;
    if (reports >= 16)
        return;
    reports++;
    fprintf(stderr, "[BW-NODE] scene node 0x%08X: link +0x%02X = 0x%08X is not a pointer;"
                    " cut. Node bytes:", node, off, bad);
    for (i = 0; i < 0x60; i += 4)
        fprintf(stderr, "%s%08X", (i % 32) ? " " : "\n    ", BW_MEM32(node + i));
    fprintf(stderr, "\n");
    fflush(stderr);
}

void sub_00036540(void)
{
    static int guard = -1;
    uint32_t node, n = 0;

    if (guard < 0) {
        const char *e = getenv("BW_NODE_GUARD");
        guard = !(e && e[0] == '0');
    }
    node = guard ? BW_MEM32(g_esp + 8) : 0;
    if (node && !bw_guest_ptr_ok(node)) {
        /* the list itself is garbage: hand the walk an empty one */
        bw_node_report(g_esp + 8, 0, node);
        BW_MEM32(g_esp + 8) = 0;
        node = 0;
    }
    while (node && n++ < 100000) {
        uint32_t next = BW_MEM32(node + 0x48);
        uint32_t child = BW_MEM32(node + 0x44);
        uint32_t draw = BW_MEM32(node + 0x4C);
        if (child && !bw_guest_ptr_ok(child)) {
            bw_node_report(node, 0x44, child);
            BW_MEM32(node + 0x44) = 0;
        }
        if (draw && !bw_guest_ptr_ok(draw))
            bw_node_report(node, 0x4C, draw);       /* logged only: a parameter */
        if (next && !bw_guest_ptr_ok(next)) {
            bw_node_report(node, 0x48, next);
            BW_MEM32(node + 0x48) = 0;
            break;
        }
        node = next;
    }
    sub_00036540_gen();
}

/* ── Untranslated instructions ───────────────────────────────────────────
 *
 * The lifter emits RECOMP_UNIMPL(text, va) at every instruction it has no
 * translation for, in place of the bare comment it used to leave. The
 * instruction is still a no-op; this only stops the omission being silent.
 * RECOMP_UNIMPL_TRAP=1 aborts at the first hit, at the guest address of the
 * cause rather than wherever the damage surfaces. */
#include <stdlib.h>
#include <intrin.h>

/* Blood Wake (M0.7): count every hit per site. The line below prints only the
 * first 50 hits in total, and FIRST_RUN_PLAN W2 needs the count of D3D's
 * `wbinvd` (one per GPU sync) for the whole run. bw_unimpl_summary() prints
 * "[UNIMPL] summary ..." lines; main.c calls it when a run ends, and it's
 * registered with atexit on the first hit. */
#define BW_UNIMPL_SLOTS 64
static volatile long s_unimpl_va[BW_UNIMPL_SLOTS];
static volatile long s_unimpl_n[BW_UNIMPL_SLOTS];
static const char *volatile s_unimpl_text[BW_UNIMPL_SLOTS];

void bw_unimpl_summary(void)
{
    int i, any = 0;
    for (i = 0; i < BW_UNIMPL_SLOTS; i++) {
        if (!s_unimpl_va[i])
            continue;
        any = 1;
        fprintf(stderr, "[UNIMPL] summary 0x%08lX `%s` x%ld\n",
                (unsigned long)s_unimpl_va[i],
                s_unimpl_text[i] ? s_unimpl_text[i] : "?", s_unimpl_n[i]);
    }
    if (!any)
        fprintf(stderr, "[UNIMPL] summary: none reached\n");
    fflush(stderr);
}

static void bw_unimpl_count(const char *text, uint32_t va)
{
    static volatile long registered;
    int i;
    if (!_InterlockedCompareExchange(&registered, 1, 0))
        atexit(bw_unimpl_summary);
    for (i = 0; i < BW_UNIMPL_SLOTS; i++) {
        long cur = s_unimpl_va[i];
        if (cur == (long)va) {
            _InterlockedIncrement(&s_unimpl_n[i]);
            return;
        }
        if (!cur) {
            long was = _InterlockedCompareExchange(&s_unimpl_va[i], (long)va, 0);
            if (was == 0)
                s_unimpl_text[i] = text;
            if (was == 0 || was == (long)va) {
                _InterlockedIncrement(&s_unimpl_n[i]);
                return;
            }
        }
    }
}

void recomp_unimpl(const char *text, uint32_t va)
{
    static int printed;
    const char *trap = getenv("RECOMP_UNIMPL_TRAP");
    int stop = trap && *trap && *trap != '0';

    bw_unimpl_count(text, va);

    if (printed < 50 || stop) {
        printed++;
        fprintf(stderr,
                "[UNIMPL] untranslated instruction REACHED: `%s` at 0x%08X"
                " (a no-op; set RECOMP_UNIMPL_TRAP=1 to stop here)\n",
                text, va);
        fflush(stderr);
    }
    if (stop) abort();
}

/* D-011 hook, Remastered only (D-016 amendment; VFX_DRAW_DISTANCE, agent D9):
 * RenderItem_IsVisible 0x000308E0. It returns (in st(0)) the item's camera
 * depth plus its radius, or a negative value when the item is outside the
 * view (seen, docs/formats/models.md). Its three callers turn that into
 * d = result - 2 x radius and pick the mesh LOD by d (LOD 0/1/2 while d <
 * mesh +0x34/+0x38/+0x3C), cut the mesh at d >= +0x40, and use d as the
 * depth-sort key: RenderMgr_CullItemTree, World_DrawGridCells and
 * ShadowMap_Render (its fixed 100/200/300 m shadow-caster LODs). With a
 * factor k > 1 this hands back (d / k) + 2 x radius, so every LOD switch and
 * mesh draw distance happens k times further away; the view test, the far
 * plane and fog are untouched (an item outside the view still returns its
 * negative value), up close nothing changes (d < the first switch draws LOD 0
 * either way), and the sort order is kept (d / k keeps the order). Original
 * (and BW_FIX=-VFX_DRAW_DISTANCE): bw_vfx_lod_scale() is 1 and the game's
 * own result is returned untouched. */
extern void sub_000308E0_gen(void);
extern __declspec(thread) double g_fp_stack[8];
extern __declspec(thread) int g_fp_top;
extern float bw_vfx_lod_scale(void);

void sub_000308E0(void)
{
    uint32_t item = g_ecx;
    float k = bw_vfx_lod_scale();
    sub_000308E0_gen();
    if (k > 1.0f) {
        double *st0 = &g_fp_stack[g_fp_top & 7];
        float radius = *(volatile float *)((uintptr_t)(item + 0xC) + g_xbox_mem_offset);
        double r2 = 2.0 * (double)radius;
        if (*st0 > r2)
            *st0 = (*st0 - r2) / (double)k + r2;
    }
}
