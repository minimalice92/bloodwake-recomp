/**
 * Blood Wake - Recompiled Game Entry Point
 *
 * Copied from xboxrecomp v0.16.0 templates/new-game (session 1) and adapted:
 * the XBE constants below, and the "Blood Wake run control" section for
 * unattended evidence runs (M0.7, docs/FIRST_RUN_PLAN.md).
 *
 * This is the Windows executable that hosts the recompiled game code.
 * It performs the following initialization sequence:
 *
 * 1. Load the original XBE file from disk
 * 2. Initialize the Xbox memory layout (map data sections to original VAs)
 * 3. Initialize the Xbox kernel replacement layer
 * 4. Initialize the kernel bridge (thunk table in Xbox memory)
 * 5. Set up game file paths for I/O redirection
 * 6. Initialize the stack pointer
 * 7. Install VEH crash handler for diagnostics
 * 8. Call the game's original entry point (recompiled)
 *
 * Customize this file for your game:
 *   - Set YOUR_GAME_ENTRY_POINT to the XBE entry point address
 *   - Set YOUR_GAME_XBE_PATH to where the XBE file lives
 *   - Set YOUR_GAME_DIR to the game data directory
 *   - Add any CRT global pre-initialization your game needs
 *   - Customize the VEH handler for game-specific crash diagnosis
 *
 * XBE Details (xbe_parser, session 1; see docs/ANALYSIS.md):
 *   Title:       Blood Wake (NA retail, disc 0 v1, built 2001-11-20)
 *   Title ID:    0x4D530010
 *   Base addr:   0x00010000
 *   Entry point: 0x000DE15F
 *   Code size:   ~931 KB (.text)
 *   Sections:    24 (.text DSOUND WMADEC D3D D3DX XGRPH 12x BINK* XPP .rdata
 *                .data DOLBY BINKDATA $$XTIMAGE)
 *   Kernel imports: 118
 */

#include <windows.h>
#include <dbghelp.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <wchar.h>

/* xboxrecomp runtime headers */
#include <xbox/xboxrecomp.h>

/*
 * If xboxrecomp.h is not an umbrella header in your setup, include
 * the individual headers directly:
 *
 * #include "kernel.h"
 * #include "xbox_memory_layout.h"
 * #include "d3d8_xbox.h"
 * #include "dsound_xbox.h"
 * #include "xinput_xbox.h"
 */

/* ── Global register state (defined in xbox_memory_layout.c) ── */

/* RECOMP_TLS is not optional here. The runtime defines these thread-local, and
 * a plain `extern` referencing a __declspec(thread) variable does not resolve
 * to the calling thread's copy -- it resolves to the image's TLS template. The
 * host side then writes g_esp somewhere the generated code never reads, so the
 * guest starts with every register at zero and faults immediately, having
 * apparently ignored the setup that visibly ran. */
extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
extern RECOMP_TLS uint32_t g_ebx, g_esi, g_edi;
extern RECOMP_TLS uint32_t g_seh_ebp;
/* x87 and SSE state. Global for the same reason the volatile GPRs are: one
 * guest routine can lift to several C functions, so a value written in one
 * body is read in the next. Defined in xbox_memory_layout.c like the rest of
 * the register file. */
extern RECOMP_TLS double g_fp_stack[8];
extern RECOMP_TLS int g_fp_top;
extern RECOMP_TLS uint16_t g_fp_control_word;
extern RECOMP_TLS uint32_t g_mxcsr;
extern RECOMP_TLS int g_fp_cmp;
extern RECOMP_TLS RecompXmm g_xmm0, g_xmm1, g_xmm2, g_xmm3;
extern RECOMP_TLS RecompXmm g_xmm4, g_xmm5, g_xmm6, g_xmm7;
extern ptrdiff_t g_xbox_mem_offset;

/* ── XBE Constants ─────────────────────────────────────────── */

/*
 * TODO: Set these from your xbe_parser output.
 * Run: py -3 -m tools.xbe_parser game/default.xbe
 */
#define YOUR_GAME_ENTRY_POINT   0x000DE15F  /* XBE entry point VA */
#define YOUR_GAME_XBE_PATH      "game\\disc\\default.xbe"
#define YOUR_GAME_XBE_PATH_W    L"game\\disc\\default.xbe"
#define YOUR_GAME_DIR            "game\\disc"

/* BW_GAME_DIR=<folder>: the disc folder from anywhere, so a run can work in
 * its own folder (tools/bw_run.py --own-cwd): everything the port writes to
 * its working folder (bw_relaunch.bin, the indirect-call dumps, the kernel
 * log) then lands there, and two runs no longer share those files.
 *
 * Read wide from the process block, and handed to the toolkit as UTF-8 (which
 * xbox_path_init decodes): the CRT's narrow copy is in the ANSI code page,
 * which mangles a folder named like "Zoë" and loses a Cyrillic one (0.2.5
 * item 53). */
static const wchar_t *bw_game_dir_w(void)
{
    static wchar_t dir[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"BW_GAME_DIR", dir, MAX_PATH);
    return n && n < MAX_PATH ? dir : NULL;
}

static const char *bw_game_dir(void)
{
    static char dir[3 * MAX_PATH];
    const wchar_t *w = bw_game_dir_w();
    if (!w || !WideCharToMultiByte(CP_UTF8, 0, w, -1, dir, sizeof dir, NULL, NULL))
        return YOUR_GAME_DIR;
    return dir;
}

static const wchar_t *bw_game_xbe(void)
{
    static wchar_t path[MAX_PATH];
    const wchar_t *e = bw_game_dir_w();
    if (!e)
        return YOUR_GAME_XBE_PATH_W;
    _snwprintf(path, MAX_PATH, L"%ls\\default.xbe", e);
    path[MAX_PATH - 1] = 0;
    return path;
}

/* ── Forward declarations ──────────────────────────────────── */

static BOOL load_xbe(const wchar_t *path, void **out_data, size_t *out_size);

/* ── Blood Wake run control (M0.7) ─────────────────────────────
 *
 * For unattended evidence runs driven by tools/bw_run.py. Everything here is
 * inert unless its variable is set, so a plain launch behaves as before.
 *
 *   BW_UNATTENDED=1          no message boxes and no Windows error dialogs:
 *                            errors go to stderr, and a crash ends the
 *                            process at once with the exception code
 *   BW_RUN_SECONDS=<n>       end the run after n seconds: dump the evidence,
 *                            print "[BW] run limit", exit with code 124
 *   BW_ICALL_DUMP_EVERY=<n>  rewrite the indirect-call dump every n seconds
 *                            (default 5 when it is compiled in; 0 = never).
 *                            The toolkit's watchdog ends a hung run with
 *                            _exit(3) and skips atexit, so without this a
 *                            hang would lose every target it reached.
 *
 * The dump (RECOMP_ICALL_FEEDBACK, CMake option BW_ICALL_FEEDBACK) is written
 * to icall_targets.dump in the working folder; bw_run.py moves it into the
 * run folder. */
#include "recomp_icall_feedback.h"
#include "bw_profile.h"                            /* BW_PROFILE sampler */
#include "bw_host.h"                               /* BW_HOST: play launcher window */
#include "gpu/bw_d3d11.h"                          /* BW_GPU=d3d11 back end */
#include "gpu/bw_gfxcap.h"                         /* BW_GFXCAP: frame capture and replay (test aid) */
#include "bw_edition.h"                            /* BW_EDITION: D-007/D-015 */
#include "bw_widescreen.h"                         /* Remastered widescreen */
#include "bw_settings.h"                           /* the launcher's settings file */
#include "bw_keys.h"                               /* settings.ini [keys] (0.2.5 item 76) */
#include "bw_hookcheck.h"                          /* item 62: the code each hook attaches to */
extern void xbox_missing_summary(const char *why);  /* kernel_missing.h */
extern int  apu_wav_close_nowait(int wait_ms);      /* apu_wav.h */
extern void bw_unimpl_summary(void);                /* recomp_manual.c */

static int s_unattended;

static unsigned env_uint(const char *name, unsigned dflt)
{
    const char *v = getenv(name);
    return (v && *v) ? (unsigned)strtoul(v, NULL, 10) : dflt;
}

static void bw_fail(const char *msg)
{
    fprintf(stderr, "[BW] FATAL: %s\n", msg);
    fflush(stderr);
    if (!s_unattended)
        MessageBoxA(NULL, msg, "Blood Wake", MB_ICONERROR);
}

/* Everything a run should leave behind when it ends abnormally. Safe to call
 * more than once. */
/* ==== Blood Wake (lane au-vp, docs/audio/vp.md): the log off every game thread ====
 *
 * The game's log (stderr: play.log for a player, the run's stderr.log in a
 * test) was written by whichever thread printed or flushed it. A file write
 * can block (seen on this PC: 6 ms to over a second, "waiting on Executive"),
 * and the toolkit flushes the log every 2 s from inside whatever kernel call
 * comes next (kernel_thunk_dispatch_body's "[KERNEL] summary"): the APU
 * frame thread inside the guest's sound interrupt (58.8 ms, a short pass),
 * the USB controller's thread inside its interrupt with the one-CPU dispatch
 * gate held (117 ms: no sound made for 88 ms). So now stderr is a pipe
 * inside the process: a reader thread (highest priority) moves what arrives
 * into an 8 MB ring and never waits for the disk; a writer thread writes the
 * ring to the real log. A game thread's print costs a pipe write. If the
 * ring is ever full (the disk 8 MB behind), what doesn't fit is dropped and
 * counted, and one line in the log says how many bytes; nothing waits.
 *   - The order is the order the pipe received the bytes: as before.
 *   - Drained (bw_log_drain) on every way out: bw_dump_evidence (the run
 *     limit, the crash filter), the game's relaunch and its own exits
 *     (HalReturnToFirmware, HalInitiateShutdown in kernel_bridge.c), the
 *     menu's Quit, a normal exit (atexit), and, for an ExitProcess nobody
 *     drained (KeBugCheck), a last pass from the exe's TLS callback.
 *   - A console (an interactive run) is left as it was.
 *   - BW_LOG_ASYNC=0: written by the printing thread, as before (the undo).
 *   - Test aids: BW_TEST_LOG_STALL=<s>[,<ms>] stalls the writer once (2000 ms
 *     by default) s seconds in; BW_TEST_CRASH_AT=<s> crashes on purpose s
 *     seconds in (the crash filter's path, with the drain);
 *     BW_TEST_LOG_QUIET_DRAIN=<s> drains with every other thread stopped.
 */
#include <io.h>
#include <fcntl.h>
#include <tlhelp32.h>

#define BW_LOG_RING (8u << 20)
static char *s_lg_ring;
static volatile LONG64 s_lg_w, s_lg_r, s_lg_lost, s_lg_lost_said;
static HANDLE s_lg_rd, s_lg_out, s_lg_ev;
static volatile LONG s_lg_on, s_lg_rbusy, s_lg_wbusy, s_lg_final;
/* The drain's marker: a NUL byte written into the pipe after everything
 * printed so far; the reader strips it and notes where the ring stood
 * (nothing peeks at the pipe while the reader is blocked on it: a
 * synchronous handle's I/O is serialized). */
static volatile LONG64 s_lg_sync_w, s_lg_sync_n;
static HANDLE s_lg_wr;                   /* the pipe's write end, as fd 2 has it */
/* The writer is inside a write to the file; the drain has taken over. */
static volatile LONG s_lg_inwrite, s_lg_takeover;
static unsigned s_lg_stall_at_ms, s_lg_stall_ms;
static ULONGLONG s_lg_t0;

static void bw_lg_put(const char *p, DWORD n)   /* the real log, whole */
{
    while (n) {
        DWORD done = 0;
        if (!WriteFile(s_lg_out, p, n, &done, NULL) || !done)
            return;
        p += done;
        n -= done;
    }
}

static void bw_lg_lost_line(void)
{
    LONG64 lost = s_lg_lost;
    if (lost != s_lg_lost_said) {
        char line[200];
        int k = _snprintf(line, sizeof line, "\r\n[LOG] %lld bytes of the log were dropped here (its writer fell"
                                             " 8 MB behind the game); no game thread waited for it\r\n",
                          lost - s_lg_lost_said);
        if (k > 0)
            bw_lg_put(line, (DWORD)k);
        s_lg_lost_said = lost;
    }
}

/* the ring's bytes to the log, all that is there; 1 if it wrote any. The
 * writer thread (writer = 1) stops when the drain has taken over. */
static int bw_lg_write_ring(int writer)
{
    int any = 0;
    for (;;) {
        LONG64 r = s_lg_r, w = InterlockedCompareExchange64(&s_lg_w, 0, 0);
        DWORD at, n;
        if (r == w)
            return any;
        at = (DWORD)(r % BW_LOG_RING);
        n = (DWORD)(w - r);
        if (n > BW_LOG_RING - at) n = BW_LOG_RING - at;
        if (n > 4096) n = 4096;          /* small steps: an abrupt end repeats at most 4 KB */
        if (writer) {
            InterlockedExchange(&s_lg_inwrite, 1);
            if (s_lg_takeover) {
                InterlockedExchange(&s_lg_inwrite, 0);
                return any;
            }
        }
        bw_lg_put(s_lg_ring + at, n);
        InterlockedExchange64(&s_lg_r, r + n);
        if (writer)
            InterlockedExchange(&s_lg_inwrite, 0);
        any = 1;
    }
}

/* the pipe's bytes into the ring; never waits for room */
static void bw_lg_take_raw(const char *buf, DWORD n)
{
    if (!n)
        return;
    LONG64 w = s_lg_w, r = InterlockedCompareExchange64(&s_lg_r, 0, 0);
    if ((LONG64)BW_LOG_RING - (w - r) < (LONG64)n) {
        InterlockedAdd64(&s_lg_lost, (LONG64)n);
        return;
    }
    {
        DWORD at = (DWORD)(w % BW_LOG_RING), k = n < BW_LOG_RING - at ? n : BW_LOG_RING - at;
        memcpy(s_lg_ring + at, buf, k);
        memcpy(s_lg_ring, buf + k, n - k);
        InterlockedExchange64(&s_lg_w, w + n);
    }
}

/* the same, a drain's NUL marker taken out and noted */
static void bw_lg_take(const char *buf, DWORD n)
{
    for (;;) {
        const char *z = (const char *)memchr(buf, 0, n);
        if (!z) {
            bw_lg_take_raw(buf, n);
            return;
        }
        bw_lg_take_raw(buf, (DWORD)(z - buf));
        InterlockedExchange64(&s_lg_sync_w, InterlockedCompareExchange64(&s_lg_w, 0, 0));
        InterlockedIncrement64(&s_lg_sync_n);
        n -= (DWORD)(z - buf) + 1;
        buf = z + 1;
    }
}

static DWORD WINAPI bw_lg_reader(LPVOID a)
{
    static char buf[64 * 1024];
    (void)a;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    for (;;) {
        DWORD n = 0;
        if (!ReadFile(s_lg_rd, buf, sizeof buf, &n, NULL) || !n)
            return 0;
        InterlockedExchange(&s_lg_rbusy, 1);
        bw_lg_take(buf, n);
        InterlockedExchange(&s_lg_rbusy, 0);
        SetEvent(s_lg_ev);
    }
}

static DWORD WINAPI bw_lg_writer(LPVOID a)
{
    (void)a;
    for (;;) {
        WaitForSingleObject(s_lg_ev, 50);
        if (s_lg_stall_ms && GetTickCount64() - s_lg_t0 >= s_lg_stall_at_ms) {
            unsigned ms = s_lg_stall_ms;     /* BW_TEST_LOG_STALL: the disk "stalls" once */
            s_lg_stall_ms = 0;
            InterlockedExchange(&s_lg_wbusy, 1);
            Sleep(ms);
            InterlockedExchange(&s_lg_wbusy, 0);
        }
        InterlockedExchange(&s_lg_wbusy, 1);
        if (bw_lg_write_ring(1) && !s_lg_takeover)
            bw_lg_lost_line();
        InterlockedExchange(&s_lg_wbusy, 0);
    }
}

/* Everything printed so far into the log, waiting at most wait_ms: the C
 * library's buffer into the pipe, the pipe into the ring, the ring to the
 * file. Called on the ways out; harmless anywhere else. */
static int bw_log_drain_core(int wait_ms);

void bw_log_drain(int wait_ms)
{
    if (!s_lg_on || s_lg_final)
        return;
    fflush(stdout);
    fflush(stderr);
    bw_log_drain_core(wait_ms);
}

/* The drain past the C library's flush; 1 when it got there in time. */
static int bw_log_drain_core(int wait_ms)
{
    static const char nul = 0;
    ULONGLONG t0, lim = (ULONGLONG)(wait_ms > 0 ? wait_ms : 0);
    LONG64 n0, target, left;
    DWORD done = 0;
    t0 = GetTickCount64();
    n0 = s_lg_sync_n;
    WriteFile(s_lg_wr, &nul, 1, &done, NULL);   /* the marker, after everything printed so far */
    SetEvent(s_lg_ev);
    while (s_lg_sync_n == n0) {          /* the reader reaches it */
        if (GetTickCount64() - t0 >= lim)
            goto late;
        Sleep(1);
    }
    target = s_lg_sync_w;
    while (InterlockedCompareExchange64(&s_lg_r, 0, 0) < target) {   /* the writer writes up to it */
        SetEvent(s_lg_ev);
        if (GetTickCount64() - t0 >= lim)
            goto late;
        Sleep(1);
    }
    return 1;
late:
    /* Not in time. If the writer isn't inside a file write (a stalled
     * thread, not a stalled disk), the drain writes the rest itself and
     * says so; inside one, nothing more can be written without waiting for
     * the disk, and the bound wins. Then the writer may go on. */
    InterlockedExchange(&s_lg_takeover, 1);
    if (!s_lg_inwrite) {
        char line[260];
        int k;
        left = InterlockedCompareExchange64(&s_lg_w, 0, 0) - s_lg_r;
        bw_lg_write_ring(0);
        bw_lg_lost_line();
        k = _snprintf(line, sizeof line, "\r\n[LOG] the log's writer thread was held when the game ended; the last %lld"
                                         " bytes were written by the drain itself (%lld bytes of the log dropped in"
                                         " all)\r\n", left, s_lg_lost);
        if (k > 0)
            bw_lg_put(line, (DWORD)k);
    }
    InterlockedExchange(&s_lg_takeover, 0);
    return 0;
}

static void bw_lg_atexit(void)
{
    bw_log_drain(1000);
}

/* An ExitProcess nobody drained: the other threads are gone by now. What
 * the pipe and the ring still hold goes to the file from here, without the
 * C library (its locks may have died with a thread). */
static void NTAPI bw_lg_tls(PVOID h, DWORD reason, PVOID r)
{
    (void)h; (void)r;
    if (reason != DLL_PROCESS_DETACH || !s_lg_on || s_lg_final)
        return;
    s_lg_final = 1;
    for (;;) {
        static char buf[64 * 1024];
        DWORD avail = 0, n = 0;
        if (!PeekNamedPipe(s_lg_rd, NULL, 0, NULL, &avail, NULL) || !avail)
            break;
        if (!ReadFile(s_lg_rd, buf, avail < sizeof buf ? avail : sizeof buf, &n, NULL) || !n)
            break;
        bw_lg_take(buf, n);
    }
    bw_lg_write_ring(0);
    bw_lg_lost_line();
}
#pragma comment(linker, "/INCLUDE:_tls_used")
#pragma comment(linker, "/INCLUDE:bw_lg_tls_ptr")
#pragma const_seg(".CRT$XLB")
#ifdef __cplusplus
extern "C"
#endif
const PIMAGE_TLS_CALLBACK bw_lg_tls_ptr = bw_lg_tls;
#pragma const_seg()

static DWORD s_lg_reader_tid, s_lg_writer_tid;

/* BW_TEST_LOG_QUIET_DRAIN=<s> (a test aid): at s seconds, every thread of the
 * game but this one and the log's two is suspended, so nothing else can
 * print, and the drain runs (past the C library: a suspended thread may hold
 * its lock); its time is logged after they are resumed. The condition a
 * drain that peeked at the pipe could not leave. */
static DWORD WINAPI bw_lg_quiet_thread(LPVOID a)
{
    HANDLE snap, held[512];
    THREADENTRY32 te;
    DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
    int n = 0, i, ok;
    LARGE_INTEGER f, c0, c1;
    static const char line[] = "[LOG] BW_TEST_LOG_QUIET_DRAIN: every other thread is stopped now; this line and"
                               " everything before it must reach the log by the drain alone\n";
    DWORD done;
    Sleep((DWORD)(uintptr_t)a);
    snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return 0;
    te.dwSize = sizeof te;
    for (ok = Thread32First(snap, &te); ok && n < 512; ok = Thread32Next(snap, &te)) {
        HANDLE h;
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == me || te.th32ThreadID == s_lg_reader_tid ||
            te.th32ThreadID == s_lg_writer_tid)
            continue;
        h = OpenThread(THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
        if (h && SuspendThread(h) != (DWORD)-1)
            held[n++] = h;
        else if (h)
            CloseHandle(h);
    }
    CloseHandle(snap);
    WriteFile(s_lg_wr, line, sizeof line - 1, &done, NULL);   /* straight into the pipe: no C library */
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c0);
    ok = bw_log_drain_core(500);
    QueryPerformanceCounter(&c1);
    for (i = 0; i < n; i++) {
        ResumeThread(held[i]);
        CloseHandle(held[i]);
    }
    fprintf(stderr, "[LOG] BW_TEST_LOG_QUIET_DRAIN: with %d other threads stopped, the drain %s in %.2f ms (bound 500)\n",
            n, ok ? "got there" : "did NOT get there", (double)(c1.QuadPart - c0.QuadPart) * 1000.0 / f.QuadPart);
    return 0;
}

static DWORD WINAPI bw_lg_crash_thread(LPVOID a)
{
    Sleep((DWORD)(uintptr_t)a);
    fprintf(stderr, "[BW] BW_TEST_CRASH_AT: crashing on purpose now (a test switch); the lines up to here must"
                    " reach the log\n");
    *(volatile uint32_t *)(uintptr_t)8 = 0;
    return 0;
}

static void bw_log_pipe_init(void)
{
    const char *e = getenv("BW_LOG_ASYNC"), *t = getenv("BW_TEST_LOG_STALL"), *c = getenv("BW_TEST_CRASH_AT");
    HANDLE cur = GetStdHandle(STD_ERROR_HANDLE), wr = NULL;
    DWORD type;
    int fd;
    if (e && e[0] == '0')
        return;
    if (!cur || cur == INVALID_HANDLE_VALUE)
        return;
    type = GetFileType(cur);
    if (type != FILE_TYPE_DISK && type != FILE_TYPE_PIPE)
        return;                          /* a console (an interactive run): as before */
    if (!DuplicateHandle(GetCurrentProcess(), cur, GetCurrentProcess(), &s_lg_out, 0, FALSE, DUPLICATE_SAME_ACCESS))
        return;
    s_lg_ring = (char *)VirtualAlloc(NULL, BW_LOG_RING, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    s_lg_ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!s_lg_ring || !s_lg_ev || !CreatePipe(&s_lg_rd, &wr, NULL, 4u << 20)) {
        CloseHandle(s_lg_out);
        return;
    }
    fflush(stderr);
    fd = _open_osfhandle((intptr_t)wr, _O_WRONLY | _O_TEXT);
    if (fd < 0 || _dup2(fd, 2) != 0) {
        fprintf(stderr, "[LOG] the writer thread could not take the log (fd %d): written as before\n", fd);
        return;
    }
    _close(fd);
    s_lg_wr = (HANDLE)_get_osfhandle(2);
    SetStdHandle(STD_ERROR_HANDLE, s_lg_wr);
    s_lg_t0 = GetTickCount64();
    if (t && *t) {
        double at = atof(t), ms = 2000;
        const char *k = strchr(t, ',');
        if (k && atof(k + 1) > 0)
            ms = atof(k + 1);
        s_lg_stall_at_ms = (unsigned)(at * 1000.0);
        s_lg_stall_ms = (unsigned)ms;
    }
    s_lg_on = 1;
    CloseHandle(CreateThread(NULL, 0, bw_lg_reader, NULL, 0, &s_lg_reader_tid));
    CloseHandle(CreateThread(NULL, 0, bw_lg_writer, NULL, 0, &s_lg_writer_tid));
    atexit(bw_lg_atexit);
    fprintf(stderr, "[LOG] the game's log goes through a writer thread of its own (BW_LOG_ASYNC; au-vp): no game"
                    " thread writes the disk%s\n", s_lg_stall_ms ? "; BW_TEST_LOG_STALL: the writer stalls once" : "");
    if (c && atof(c) > 0)
        CloseHandle(CreateThread(NULL, 0, bw_lg_crash_thread, (LPVOID)(uintptr_t)(atof(c) * 1000.0), 0, NULL));
    {
        const char *q = getenv("BW_TEST_LOG_QUIET_DRAIN");
        if (q && atof(q) > 0)
            CloseHandle(CreateThread(NULL, 0, bw_lg_quiet_thread, (LPVOID)(uintptr_t)(atof(q) * 1000.0), 0, NULL));
    }
}

static void bw_dump_evidence(const char *why)
{
    fprintf(stderr, "[BW] dumping evidence (%s)\n", why);
    RECOMP_ICALL_FEEDBACK_DUMP();
    bw_profile_dump(why);
    {
        extern void bw_fl_dump(const char *why);   /* P2b: BW_FRAMELOG */
        bw_fl_dump(why);
    }
    {
        extern void bw_lat_dump(const char *why);  /* lat-m: BW_LATLOG */
        bw_lat_dump(why);
    }
    xbox_missing_summary(why);
    bw_unimpl_summary();
    bw_edition_summary();
    {
        extern void bw_wav_close_nowait(int wait_ms);   /* au-vp: the WAV writer's ring first */
        bw_wav_close_nowait(400);
    }
    apu_wav_close_nowait(500);
    fflush(stdout);
    fflush(stderr);
    bw_log_drain(500);                   /* au-vp: the log's ring to the file before the process ends */
}

static LONG WINAPI bw_unhandled_filter(PEXCEPTION_POINTERS ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    fprintf(stderr, "[BW] unhandled exception 0x%08lX, ending the run\n",
            (unsigned long)code);
    bw_dump_evidence("crash");
    TerminateProcess(GetCurrentProcess(), (UINT)code);
    return EXCEPTION_EXECUTE_HANDLER;
}

static DWORD WINAPI bw_run_control_thread(LPVOID arg)
{
    unsigned limit = env_uint("BW_RUN_SECONDS", 0);
#ifdef RECOMP_ICALL_FEEDBACK
    unsigned every = env_uint("BW_ICALL_DUMP_EVERY", 5);
#else
    unsigned every = 0;
#endif
    ULONGLONG start = GetTickCount64();
    ULONGLONG next_dump = every ? start + every * 1000ULL : 0;

    (void)arg;
    for (;;) {
        ULONGLONG now;
        Sleep(250);
        now = GetTickCount64();
        if (next_dump && now >= next_dump) {
            RECOMP_ICALL_FEEDBACK_DUMP();
            next_dump = now + every * 1000ULL;
        }
        if (limit && now - start >= limit * 1000ULL) {
            fprintf(stderr, "[BW] run limit %u s reached\n", limit);
            bw_dump_evidence("run limit");
            TerminateProcess(GetCurrentProcess(), 124);
        }
    }
}

static void bw_run_control_init(void)
{
    s_unattended = getenv("BW_UNATTENDED") != NULL;
    if (s_unattended) {
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX |
                     SEM_NOOPENFILEERRORBOX);
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
        SetUnhandledExceptionFilter(bw_unhandled_filter);
    }
    RECOMP_ICALL_FEEDBACK_INIT();   /* atexit dump; no-op when not compiled in */

    {
        int want = env_uint("BW_RUN_SECONDS", 0) != 0;
#ifdef RECOMP_ICALL_FEEDBACK
        want = want || env_uint("BW_ICALL_DUMP_EVERY", 5) != 0;
#endif
        if (want) {
            HANDLE h = CreateThread(NULL, 0, bw_run_control_thread, NULL, 0, NULL);
            if (h)
                CloseHandle(h);
        }
    }
    fprintf(stderr, "[BW] run control: unattended=%d run_seconds=%u icall_feedback=%s\n",
            s_unattended, env_uint("BW_RUN_SECONDS", 0),
#ifdef RECOMP_ICALL_FEEDBACK
            "on"
#else
            "off"
#endif
            );
}

/* ── "Reached the title", for the play host (0.2.5 item 60) ────────────
 *
 * The host (bw_host.c) records in start_state.ini whether each start got as
 * far as the title, so the launcher can offer a safe start after two that
 * didn't (docs/025/CONTRACTS.md, 4). It hands each boot an inheritable event
 * (BW_HOST_TITLE_EVENT); this thread sets it once the game is up:
 *
 *   - the front end's state, the dword at 0x001C8180, leaves 0: 0 is the
 *     startup movies, and IntroMovies_Update (0x57880) writes 2, the title,
 *     when they end or are skipped (seen in the code, and in run
 *     20261103-013816-r6-gpu-c1fe: 0 until 33.4 s, then 2); menu boots
 *     after a mission start in another state;
 *   - or the player list (0x001CEE58 + 0xC0, as bw_state.c reads it) counts
 *     1 to 4: a gameplay boot (a launched mission, battle or the attract
 *     demo; 1 at 1.3 s in the same run's second stage).
 *
 * Read-only, every 100 ms, and only under the host: test runs never start
 * it, so it can't touch an A/B. */
static HANDLE s_title_event;

static DWORD WINAPI bw_title_watch(LPVOID arg)
{
    const volatile uint32_t *fe =
        (const volatile uint32_t *)((uintptr_t)g_xbox_mem_offset + 0x001C8180u);
    const volatile uint32_t *players =
        (const volatile uint32_t *)((uintptr_t)g_xbox_mem_offset + 0x001CEE58u + 0xC0u);
    ULONGLONG t0 = GetTickCount64();
    (void)arg;
    for (;;) {
        uint32_t s = *fe, p = *players;
        if (s != 0 || (p >= 1 && p <= 4)) {
            fprintf(stderr, "[BW] start-up done after %.1f s: %s (front-end state %u, players %u)\n",
                    (double)(GetTickCount64() - t0) / 1000.0,
                    s ? "the title or a menu is up" : "gameplay is running", s, p);
            fflush(stderr);
            SetEvent(s_title_event);
            return 0;
        }
        Sleep(100);
    }
}

static void bw_title_watch_start(void)
{
    char v[32];
    DWORD n = GetEnvironmentVariableA("BW_HOST_TITLE_EVENT", v, sizeof v);
    HANDLE h;
    if (!n || n >= sizeof v || !g_xbox_mem_offset)
        return;
    s_title_event = (HANDLE)(uintptr_t)_strtoui64(v, NULL, 10);
    if (!s_title_event)
        return;
    h = CreateThread(NULL, 0, bw_title_watch, NULL, 0, NULL);
    if (h)
        CloseHandle(h);
}

/* Recompiled entry point (generated by recomp pipeline) */
extern void xbe_entry_point(void);

/* ── VEH crash handler ─────────────────────────────────────── */

/*
 * Vectored Exception Handler for crash diagnostics.
 *
 * When the recompiled game hits an access violation, this handler prints
 * the faulting address, all Xbox register values, and a native stack trace.
 * This is your primary debugging tool during bring-up.
 *
 * Customize this for your game:
 *   - Add game-specific address checks (GPU register probes, etc.)
 *   - Add dumps of game-specific globals (heap handles, state flags)
 *   - Add SEH simulation if your game uses __try/__except
 */
/* Name the guest function a fault happened in, and recover the call chain.
 *
 * Recompiled code faults as ordinary native code, so the exception record
 * carries a host RIP and nothing else -- there is no guest program counter to
 * report, and the host address changes every build. Two things recover the
 * guest view:
 *
 *   - every generated function is a real symbol in the image (sub_005A03C0
 *     and so on), so the linker's PDB already maps host address back to guest
 *     function. dbghelp turns an anonymous RIP into that name.
 *
 *   - every lifted call pushes its guest return address onto the guest stack
 *     before jumping, so the stack still holds the chain. Scanning up from esp
 *     for values inside the code sections recovers it.
 *
 * ponytail: the stack scan is a scan, not a frame walk -- these are FPO frames
 * with no reliable ebp chain, so there is nothing to walk. It over-reports,
 * since addresses from returned-from calls linger below esp, but naming the
 * guest function is the whole question at a fault.
 *
 * Requires linking dbghelp and keeping the .pdb beside the .exe.
 */
static void print_guest_context(void *rip)
{
    /* SYMBOL_INFO is variable-length: the name is written past the struct, so
     * it must be over-allocated with MaxNameLen set to the slack. */
    char buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
    DWORD64 disp = 0;

    memset(buf, 0, sizeof(buf));
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    if (SymFromAddr(GetCurrentProcess(), (DWORD64)(uintptr_t)rip, &disp, sym))
        fprintf(stderr, "  in %s+0x%llX\n",
                sym->Name, (unsigned long long)disp);

    if (g_xbox_mem_offset && g_esp) {
        const uint32_t *sp =
            (const uint32_t *)((uintptr_t)g_xbox_mem_offset + g_esp);
        int shown = 0, i;
        fprintf(stderr, "  guest stack (return addresses, innermost first):\n");
        for (i = 0; i < 256 && shown < 24; i++) {
            uint32_t v = sp[i];
            if (v > g_xbox_code_lo && v < g_xbox_code_hi) {
                fprintf(stderr, "    [esp+%-4d] 0x%08X\n", i * 4, v);
                shown++;
            }
        }
        /* When the frame is gone, say so, and fall back on what is left.
         *
         * A fault inside a block copy leaves no return address in range: the
         * copy is a host memmove and its caller's frame is further up than
         * this reaches. Worse, a copy that ran past its buffer overwrites the
         * stack itself, so the list comes out empty exactly when it is most
         * needed -- and an empty list under a promising heading reads like
         * "nothing to see" rather than "the evidence is destroyed". */
        if (!shown)
            fprintf(stderr, "    (no return addresses in range)\n");

        /* Everything below only pays for itself when the scan came back
         * empty. A crash where the chain was recovered already says what
         * happened, and burying that under another twenty lines makes the
         * common report worse to read. */
        if (!shown) {
            uint32_t k;

            /* The indirect-call history, which the stack cannot overwrite.
             * recomp_manual.c prints this too, but only from
             * recomp_icall_fail_log -- a page fault inside a block copy
             * never reaches that path, and this is the same information by
             * the route the fault actually took.
             *
             * Declared here rather than included: main.c does not pull in
             * recomp_types.h, and recomp_manual.c spells it the same way. */
            extern volatile uint32_t g_icall_trace[16];
            extern volatile uint32_t g_icall_trace_idx;

            fprintf(stderr, "  recent ICALL targets:");
            for (k = 0; k < 16; k++)
                fprintf(stderr, " %08X",
                        g_icall_trace[(g_icall_trace_idx + k) & 15]);
            fprintf(stderr, "\n");

            /* The frame unfiltered. The pointers and lengths handed to
             * whatever faulted live here and look nothing like code, so the
             * filter above drops exactly what is wanted. On the crash this
             * was written for, one of these words was the end of the mapped
             * RAM mirror -- which is what identified the fault as a copy
             * running off its buffer rather than a stray pointer. */
            for (i = 0; i < 20; i++)
                fprintf(stderr, "    raw[esp+%-4d] 0x%08X\n", i * 4, sp[i]);
        }
    }
}

/* The emulated APU.
 *
 * Two entry points that had no callers anywhere in the tree. apu_mmio_hook.c
 * documents apu_hook_handle_mmio as "called from VEH in main.c" and no main.c
 * called it; apu_decode_and_handle's first line is `if (!g_apu_state) return
 * false` and nothing assigned g_apu_state. So with RECOMP_AC97_READY set the
 * APU's registers were unmapped to be trapped, and every trap was then
 * declined and surfaced as an access violation on the first register
 * DirectSound touched. The VEH below now reaches apu_hook_handle_mmio
 * through the toolkit's xbox_VehMmioFault.
 *
 * Declared rather than included so this does not depend on src/apu being on
 * the include path. */
typedef struct MCPXAPUState MCPXAPUState;
extern MCPXAPUState *mcpx_apu_init_standalone(uint8_t *ram_ptr);
extern MCPXAPUState *g_apu_state;

static LONG CALLBACK veh_handler(PEXCEPTION_POINTERS ep)
{
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        uintptr_t fault_addr = ep->ExceptionRecord->ExceptionInformation[1];

        /*
         * GPU register probe at 0xFD000000 range.
         * Some games probe NV2A registers directly. On real hardware this
         * returns GPU state; here we just skip the instruction.
         * TODO: Implement mini x86-64 decoder for instruction skipping,
         * or connect to the xbox_nv2a library for proper handling.
         */
        if (fault_addr >= 0xFD000000 && fault_addr < 0xFE000000) {
            return EXCEPTION_CONTINUE_SEARCH;
        }

        /* Trapped hardware registers (APU, USB OHCI) -> their models.
         *
         * The other half of RECOMP_AC97_READY: reporting the codec ready is
         * what lets DirectSoundCreate past its first gate, and from there it
         * drives the APU directly, so the registers have to fault to be seen.
         * "Handled" means the access was decoded and the instruction stepped
         * over, so execution resumes instead of unwinding. */
        if (xbox_VehMmioFault(ep))
            return EXCEPTION_CONTINUE_EXECUTION;

        fprintf(stderr, "[CRASH] Access violation at RIP=0x%llX, fault addr=0x%llX (%s)\n",
            (unsigned long long)ep->ContextRecord->Rip,
            (unsigned long long)fault_addr,
            ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read");
        fprintf(stderr, "  Xbox regs: eax=0x%08X ecx=0x%08X edx=0x%08X esp=0x%08X\n",
            g_eax, g_ecx, g_edx, g_esp);
        fprintf(stderr, "  Xbox regs: ebx=0x%08X esi=0x%08X edi=0x%08X\n",
            g_ebx, g_esi, g_edi);
        fprintf(stderr, "  Xbox VA of fault: 0x%08X\n",
            (uint32_t)(fault_addr - (uintptr_t)g_xbox_mem_offset));
        print_guest_context((void *)ep->ContextRecord->Rip);

        /*
         * TODO: Add game-specific diagnostics here. Examples:
         *
         * Dump CRT heap handle:
         *   uint32_t heap = *(uint32_t *)((uint8_t *)g_xbox_mem_offset + HEAP_HANDLE_VA);
         *   fprintf(stderr, "  CRT heap handle: 0x%08X\n", heap);
         *
         * Dump game state:
         *   uint32_t state = *(uint32_t *)((uint8_t *)g_xbox_mem_offset + GAME_STATE_VA);
         *   fprintf(stderr, "  Game state: %u\n", state);
         */

        /* Print native stack return addresses for debugging */
        {
            uintptr_t *sp = (uintptr_t *)ep->ContextRecord->Rsp;
            fprintf(stderr, "  Native stack (first 8 return addrs):\n");
            for (int i = 0; i < 64 && sp[i]; i++) {
                if (sp[i] >= 0x140000000ULL && sp[i] < 0x150000000ULL) {
                    fprintf(stderr, "    [%d] 0x%llX\n", i, (unsigned long long)sp[i]);
                }
            }
        }
        fflush(stderr);

        /* A title that dies mid-boot is the one whose indirect-call targets
         * are most worth having. No-op unless RECOMP_ICALL_FEEDBACK. */
        RECOMP_ICALL_FEEDBACK_DUMP();
        /* au-vp: the log's ring to the file now. A player's run has no
         * unhandled-exception filter (that is for unattended runs), so
         * Windows ends the process straight after this. */
        bw_log_drain(300);
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

/* ── WinMain ───────────────────────────────────────────────── */

extern int recomp_dispatch_init(void);   /* generated: src/recomp/gen/recomp_dispatch.c */

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nCmdShow)
{
    void *xbe_data = NULL;
    size_t xbe_size = 0;

    (void)hInstance;
    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nCmdShow;

    {
        /* 0.2.5 item 97: --screen-info[=<x>,<y>], the screen under the point
         * (else the primary) as one JSON object for the launcher (bw_hdr.c) */
        extern int bw_hdr_screen_info(const char *arg);
        const char *si = strstr(GetCommandLineA(), "--screen-info");
        if (si)
            return bw_hdr_screen_info(si[13] == '=' ? si + 14 : NULL);
    }
    if (getenv("BW_GFXCAP_DRAWS")) {    /* test aid: a capture's draws, decoded (bw_gfxcap_draws.c) */
        extern int bw_gfxcap_draws_main(void);
        return bw_gfxcap_draws_main();
    }
    if (bw_keys_schema_main())    /* --keys-schema: the default key table (bw_keys.c) */
        return 0;
    /* The launcher asks for the settings table (tools/launcher.pyw). The exe
     * enters here (WinMain), whatever main() below says, so the process's
     * own command line is checked. */
    if (strstr(GetCommandLineA(), "--settings-schema")) {
        bw_settings_print_schema(stdout);
        fflush(stdout);
        return 0;
    }

    /* The settings file (tools/launcher.pyw; bw_settings.h): its choices
     * become environment defaults before anything reads a switch, in the
     * host window's process too, whose environment every boot inherits.
     * The environment wins; no file (or BW_SETTINGS=none) changes nothing. */
    bw_settings_apply();

    /* BW_HOST=1 (tools/play.py): this process is the play session's window
     * and runs each boot of the game as a child (bw_host.c). Nothing of the
     * game starts here. */
    if (bw_host_wanted())
        return bw_host_main();

    /* Unbuffered output for immediate visibility during debugging */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    /* BW_STDERR_BUFFER=<bytes>: buffer stderr for speed runs. The toolkit logs
     * every file read and every traced call, and unbuffered each line is a
     * system call: 15% of the main thread during the logo movies (profile,
     * 2026-10-26). Off by default, so a diagnostic run never loses its last
     * lines; the exit paths (crash, run limit, relaunch) flush. */
    {
        static char errbuf[1 << 20];
        unsigned n = env_uint("BW_STDERR_BUFFER", 0);
        if (n)
            setvbuf(stderr, errbuf, _IOFBF, n < sizeof(errbuf) ? n : sizeof(errbuf));
    }
    bw_log_pipe_init();                  /* au-vp: the log off every game thread (BW_LOG_ASYNC) */

    printf("=== Blood Wake - Static Recompilation ===\n");
    bw_run_control_init();
    /* The edition (D-015): read once per boot from BW_EDITION, which every
     * launcher passes to every boot. Original is the default and the zero
     * state; a name that isn't an edition refuses to start, so a typo never
     * quietly gives Original. */
    {
        char err[256];
        if (bw_edition_init(err, sizeof err)) {
            bw_fail(err);
            return 1;
        }
    }
    bw_keys_apply();          /* settings.ini [keys]: bindings, ramp, deadzones */
    bw_profile_init();
    {
        extern void bw_fl_init(void);   /* P2b: BW_FRAMELOG test aid (bw_framelog.c) */
        extern void bw_lat_init(void);  /* lat-m: BW_LATLOG test aid (bw_latlog.c) */
        extern void bw_padwatch_init(void);  /* au-iso: BW_PADWATCH test aid (bw_padwatch.c) */
        bw_fl_init();
        bw_lat_init();
        bw_padwatch_init();
    }
    bw_gfxcap_init();         /* BW_GFXCAP / BW_GFXCAP_REPLAY: test aid, inert when unset */
    bw_d3d11_register();      /* BW_GPU=d3d11: GPU back end (D-003 B prototype) */
    printf("Loading XBE...\n");

    /* Install VEH handler (first handler in chain) */
    /* Load symbols up front rather than from inside the handler: at fault
     * time the process is already in a bad way, and SymInitialize
     * allocates. Failure is not fatal -- the handler prints no name. */
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    SymInitialize(GetCurrentProcess(), NULL, TRUE);
    AddVectoredExceptionHandler(1, veh_handler);

    /* Step 1: Load XBE */
    if (!load_xbe(bw_game_xbe(), &xbe_data, &xbe_size)) {
        bw_fail("Failed to load game\\disc\\default.xbe. Run from the repo root, "
                "with the disc extracted to game\\disc (SESSION1_RUNBOOK Phase B).");
        return 1;
    }
    printf("XBE loaded: %zu bytes\n", xbe_size);
    /* Item 62: every place the port attaches by address still holds the
     * retail program's code; a fix whose code differs stays off this boot.
     * Before the IMAGE fixes and before any guest thread. */
    bw_hookcheck_run(xbe_data, xbe_size);

    /* Step 2: Initialize Xbox memory layout */
    printf("Initializing Xbox memory layout...\n");
    {
        /* BW_MAP_MB=<n>: map n MB of guest address space while the title is
         * still told it has 64 MB of RAM (the toolkit's xbox_SetMapSize).
         * The toolkit backs every NtAllocateVirtualMemory reserve with heap
         * at once, where the console reserves address space only, so mission
         * 2's 16 MB reserves ran its ~49 MB heap out and it hung loading
         * (ANALYSIS, "Mission 2: out of memory"). The extra space only gives
         * the user heap room; the GPU/APU's contiguous window is separate and
         * unchanged. */
        extern void xbox_SetMapSize(size_t bytes);
        const char *e = getenv("BW_MAP_MB");
        int mb = e && *e ? atoi(e) : 128;   /* BW_MAP_MB=64: the toolkit default */
        if (mb > 64) {
            xbox_SetMapSize((size_t)mb << 20);
            printf("Guest address space: %d MB mapped (RAM stays 64 MB)\n", mb);
        }
    }
    if (!xbox_MemoryLayoutInit(xbe_data, xbe_size)) {
        bw_fail("Failed to initialize the Xbox memory layout. "
                "The required virtual address range may be unavailable.");
        free(xbe_data);
        return 1;
    }

    g_xbox_mem_offset = xbox_GetMemoryOffset();
    printf("Xbox memory mapped. Offset: 0x%llX\n", (unsigned long long)g_xbox_mem_offset);
    {
        extern void bw_state_start(void);
        bw_state_start();                  /* BW_STATE: read-only state file for tests */
        extern void bw_debugflags_start(void);
        bw_debugflags_start();             /* BW_DEBUG_BYTES: developers' debug switches, tests only */
    }
    bw_title_watch_start();                /* play host only: tell it when the title is up */
    /* Edition data fixes (IMAGE kind): none in Original, which only counts
     * the image bytes that differ from the XBE (0 expected) and logs it. */
    bw_edition_apply_image_fixes(xbe_data, xbe_size);

    /* Bring up the emulated APU.
     *
     * Gated on the same variable that unmaps its registers, because the two
     * halves are useless apart: a trap with no model declines every access,
     * and a model nothing traps into never sees a register. The APU walks
     * Xbox physical RAM to find voice buffers, so it gets the guest RAM
     * base. */
    if (getenv("RECOMP_AC97_READY")) {
        g_apu_state = mcpx_apu_init_standalone((uint8_t *)xbox_GetMemoryBase());
        fprintf(stderr, "[BOOT] emulated APU %s\n",
                g_apu_state ? "up" : "FAILED to initialise");
    }

    /* Blood Wake: the emulated USB host controllers (the toolkit's ohci.c).
     * The game reaches its gamepad through its own XAPI/XPP USB driver, which
     * drives the OHCI registers at 0xFED00000 directly; the model answers
     * them through the VEH above (xbox_VehMmioFault). Nothing in the toolkit
     * calls this -- the host must, after guest memory is mapped (ohci.h) --
     * and the template main.c didn't, so RECOMP_USB alone did nothing (run
     * 20261025-225038-r4-input: no OHCI activity). Inert unless RECOMP_USB. */
    {
        extern void xbox_OhciInit(void);
        xbox_OhciInit();
    }

    /* Step 3: Initialize Xbox kernel */
    printf("Initializing Xbox kernel replacement...\n");
    xbox_kernel_init();

    /* Step 4: Set game directory for file I/O path translation */
    {
        extern void xbox_path_init(const char *game_dir, const char *save_dir);
        /* NULL in Original: the toolkit's default save folder, as before
         * the edition switch. Other editions get their own (bw_edition.c). */
        xbox_path_init(bw_game_dir(), bw_edition_prepare_saves());
    }

    /* Step 5: Initialize kernel bridge (thunk table in Xbox memory) */
    printf("Initializing kernel bridge...\n");
    xbox_kernel_bridge_init();
    {
        /* TK-8 (0.2.5 item 61): xbox_kernel_init's older thunk layer writes
         * "N kernel imports are unresolved - game may crash!" to
         * xbox_kernel.log, counting its static list's empty slots (ordinal
         * 0) and one ordinal it lacks. The game never calls through that
         * table: every import goes through the bridge just set up, which
         * prints its own count to the game's log. Rewording the line itself
         * would mean carrying kernel_thunks.c, whose export table this
         * project leaves alone, so the facts go in beside it instead. */
        uint32_t tb = 0, tc = 0;
        xbox_kernel_get_thunk_address(&tb, &tc);
        xbox_log(XBOX_LOG_INFO, XBOX_LOG_THUNK,
                 "Blood Wake: the 'unresolved' lines above are the older thunk layer's static "
                 "list (mostly empty slots); the game's %u kernel imports (table at 0x%08X) all "
                 "go through the kernel bridge, which logs its own count to the game's log "
                 "('Kernel thunk bridge: n/n resolved'). Not a fault (TK-8).", tc, tb);
    }

    /* Blood Wake: the game relaunches itself (run 20261025-222206-r2-pixels,
     * ANALYSIS "First runs"). A console reboots into the title with the
     * launch data page the old image left; tools/bw_run.py does the reboot
     * and passes what recomp_manual.c recorded (bw_relaunch.bin) here.
     * Built after xbox_kernel_bridge_init, which zeroes the slot. Layout as
     * XGetLaunchInfo reads it and as the toolkit printed it at the exit:
     * +0 type (0 for a title launch), +4 title id, +8 the path (520 bytes,
     * "\Device\CdRom0;default.xbe" for "D:\default.xbe"), +0x210 flags,
     * +0x400 the 3072 bytes of launch data. */
    int bw_gameplay_boot = 0;   /* launch data SPLASH mode 1 (below) */
    {
        /* The whole file is read and checked before any of it is used: a
         * file cut short (a full disk, a copy interrupted) used to install a
         * half-read page (0.2.5 item 54). Now it is ignored with a log line,
         * and the boot starts at the title as a cold boot does. Wide path:
         * the host passes a full path, which carries the user's folder name
         * (item 53). */
        wchar_t lfw[MAX_PATH];
        DWORD lfn = GetEnvironmentVariableW(L"BW_LAUNCH_DATA_FILE", lfw, MAX_PATH);
        if (lfn && lfn < MAX_PATH) {
            static uint8_t buf[16 + 260 + 3072 + 1];   /* the largest file, and one byte more */
            char lf[3 * MAX_PATH], path[261] = {0}, dev[520] = {0};
            const char *why = NULL;
            size_t got = 0, want = 0;
            uint32_t ver = 0, plen = 0, dlen = 0, page = 0;
            uint8_t *mem = (uint8_t *)g_xbox_mem_offset;
            FILE *f = _wfopen(lfw, L"rb");
            if (!WideCharToMultiByte(CP_UTF8, 0, lfw, -1, lf, sizeof lf, NULL, NULL))
                lf[0] = 0;
            if (!f)
                why = "could not be opened";
            else {
                got = fread(buf, 1, sizeof buf, f);
                fclose(f);
                if (got < 16 || memcmp(buf, "BWRL", 4))
                    why = "is not a launch file (no BWRL header)";
                else {
                    memcpy(&ver, buf + 4, 4);
                    memcpy(&plen, buf + 8, 4);
                    if (ver != 1)
                        why = "has an unknown version";
                    else if (plen >= sizeof path)
                        why = "has a path longer than 260 bytes";
                    else if (got < 16 + (size_t)plen)
                        why = "is cut short";
                    else {
                        memcpy(&dlen, buf + 12 + plen, 4);
                        want = 16 + (size_t)plen + dlen;
                        if (dlen != 0 && dlen != 3072)
                            why = "has a launch data length other than 0 or 3072";
                        else if (got < want)
                            why = "is cut short";
                        else if (got > want)
                            why = "is longer than its header says";
                    }
                }
            }
            if (!why) {
                page = xbox_HeapAlloc(0x1000, 0x1000);
                if (!page)
                    why = "could not be placed (no guest memory for the page)";
            }
            if (why) {
                fprintf(stderr, "[BW] WARNING: the launch file %s %s (%u bytes read, %u expected); "
                        "ignored, so this boot starts at the title\n", lf, why,
                        (unsigned)got, (unsigned)want);
                fflush(stderr);
            } else {
                memcpy(path, buf + 12, plen);
                memset(mem + page, 0, 0x1000);
                if (dlen)
                    memcpy(mem + page + 0x400, buf + 16 + plen, dlen);
                if (!_strnicmp(path, "D:\\", 3))
                    snprintf(dev, sizeof dev, "\\Device\\CdRom0;%s", path + 3);
                else
                    snprintf(dev, sizeof dev, "%s", path);
                *(uint32_t *)(mem + page + 0) = 0;              /* title launch */
                *(uint32_t *)(mem + page + 4) = 0x4D530010u;    /* Blood Wake */
                memcpy(mem + page + 8, dev, strlen(dev) + 1);
                *(uint32_t *)(mem + XBOX_KERNEL_DATA_BASE + KDATA_LAUNCH_DATA_PAGE) = page;
                fprintf(stderr, "[BW] launch data page 0x%08X from %s: path '%s', "
                        "data %08X %08X %08X\n", page, lf, dev,
                        *(uint32_t *)(mem + page + 0x400), *(uint32_t *)(mem + page + 0x404),
                        *(uint32_t *)(mem + page + 0x408));
            }
            if (page) {
                /* Launch data "SPLASH", then a dword: 1 boots gameplay
                 * (missions, battles, the attract demo; the level name at
                 * offset 600: "01".."28", "sm_...", "attract_01"), 0 the
                 * title and menus (offset 600 can hold leftovers then).
                 * Gameplay is held to two vblanks a frame, as the console's
                 * draw time held it: the game steps its clock 1/30 s per
                 * frame (nv2a_pb_exec.c, bw_set_min_flip_interval).
                 * BW_GAMEPLAY_30=0 turns it off. */
                extern void bw_set_min_flip_interval(unsigned n);
                const uint8_t *ld = mem + page + 0x400;
                const char *lvl = (const char *)(ld + 600);
                const char *e = getenv("BW_GAMEPLAY_30");
                int gameplay = dlen && !memcmp(ld, "SPLASH", 6)
                               && *(const uint32_t *)(ld + 8) == 1;
                bw_gameplay_boot = gameplay;
                /* Every edition but Original: the high frame rate instead
                 * (src/bw_hifps_hooks.c, D-016): the game keeps its own 30 Hz
                 * step and the extra frames are interpolated, so the D-013
                 * hold isn't needed. In Original bw_hifps_boot returns 0. */
                extern unsigned bw_hifps_boot(int gameplay);
                if (gameplay && bw_hifps_boot(gameplay)) {
                    fprintf(stderr, "[BW] gameplay boot (level '%.16s'): high frame rate, "
                            "game speed from the game's own 30 Hz step\n", lvl);
                } else if (gameplay && !(e && e[0] == '0')) {
                    fprintf(stderr, "[BW] gameplay boot (level '%.16s'): 30 fps, as on the console\n", lvl);
                    bw_set_min_flip_interval(2);
                }
            }
        }
    }

    /* Remastered widescreen (bw_widescreen.h): this boot's shape. Gameplay
     * boots are Hor+ at the display's aspect; the title and menus stay 4:3,
     * pillarboxed in the wide window. In Original, and with -WIDESCREEN,
     * nothing changes: k stays 1 and no hook or back-end path departs from
     * the old code. */
    bw_ws_init(bw_gameplay_boot, 1);
    bw_d3d11_ws_start();
    if (bw_ws_aspect() > 4.0f / 3.0f + 0.001f) {
        extern void bw_fb_set_window_aspect(float aspect);   /* carried fb_present.c */
        bw_fb_set_window_aspect(bw_ws_aspect());
    }

    /* Blood Wake: D3D's swap throttle (ANALYSIS "First runs", run
     * 20261025-221347-r2-pixels). D3D's present (0x00129630) waits while
     * frames submitted (device+0x2B60) minus frames completed (device+0x2518)
     * is 2 or more; the device pointer is at 0x00136B00. On hardware the
     * "completed" count moves through D3D's own GPU interrupt path: the GPU
     * traps a NO_OPERATION software method, D3D's handler marks the flip
     * pending, and the next vblank completes it. The toolkit doesn't deliver
     * software-method traps (TOOLKIT_ISSUES TK-9), so the count never moved.
     * Its counter mirror keeps completed = submitted instead (it can't run
     * ahead, so the unsigned difference never underflows). Approximation:
     * the throttle no longer holds the game back, so pacing comes from
     * elsewhere (FIRST_RUN_PLAN W10). */
    {
        extern int xbox_Nv2aMirrorCounter(uint32_t device_ptr_va,
                                          uint32_t src_off, uint32_t dst_off);
        if (!getenv("BW_NO_SWAP_MIRROR"))
            xbox_Nv2aMirrorCounter(0x00136B00u, 0x2B60u, 0x2518u);
    }

    /* Step 6: Initialize stack */
    g_esp = XBOX_STACK_TOP;

    /*
     * TODO: Pre-initialize CRT globals if needed.
     *
     * Many Xbox games use the MSVC CRT. The CRT's __heap_init sets up a
     * heap descriptor at a game-specific address. You may need to
     * pre-initialize __active_heap to avoid small-block heap issues:
     *
     *   uint32_t *active_heap = (uint32_t *)((uint8_t *)g_xbox_mem_offset + ACTIVE_HEAP_VA);
     *   *active_heap = 1;  // 1 = system heap (HeapAlloc), avoids SBH init
     *
     * Find ACTIVE_HEAP_VA by searching the disassembly for __heap_init
     * or by looking for the CRT's __active_heap global in the data section.
     */

    printf("\n=== Initialization complete ===\n");
    printf("Entry point: 0x%08X\n", YOUR_GAME_ENTRY_POINT);
    printf("ESP: 0x%08X\n", g_esp);

    /* Build the flat dispatch table before the guest runs.
     *
     * Without this recomp_lookup falls back to a binary search over the
     * whole function table -- roughly log2(n) branches on *every*
     * indirect call, which for a 45,000-function C++ title is about 16
     * every time the game goes through a vtable. Half-Life 2 spent most
     * of its static initialisation inside recomp_lookup for exactly this
     * reason, and it read as a hang.
     *
     * Optional by design: if the allocation fails the search still works,
     * so a failure is worth one line and not a fatal error. */
    if (!recomp_dispatch_init())
        fprintf(stderr, "[BOOT] flat dispatch unavailable; "
                        "indirect calls will use the binary search\n");

    /* Arm the hang watchdog. Does nothing unless RECOMP_WATCHDOG_SECS is set,
     * and must be called from this thread -- the guest registers it samples are
     * thread-local, so it has to be handed the copies belonging to the thread
     * that runs guest code.
     *
     * Not optional boilerplate: without this call RECOMP_WATCHDOG_SECS is
     * silently inert, and the one diagnostic that tells a hang from slowness
     * does nothing while appearing to be set. */
    xbox_WatchdogStart();

    /* BW_TEST_CRASHSTART=1: crash here, before the game's first instruction,
     * as a crash inside the logo movies would (test switch for the play
     * host's one retry, 0.2.5 item 51). Inert unless set. */
    {
        const char *e = getenv("BW_TEST_CRASHSTART");
        if (e && e[0] == '1') {
            fprintf(stderr, "[BW] BW_TEST_CRASHSTART=1: crashing on purpose (test switch)\n");
            fflush(stderr);
            *(volatile uint32_t *)(uintptr_t)8 = 0;
        }
    }

    /* BW_GFXCAP_REPLAY=<file> (lane gfx-cap, docs/gfx/capture.md): the
     * runtime is set up as for a run, and the capture is fed to the GPU side
     * instead of starting the game. No game code runs. Test aid. */
    if (g_bw_gfxcap == 2) {
        int rc = bw_gfxcap_replay_main();
        fflush(stdout);
        fflush(stderr);
        ExitProcess((UINT)rc);
    }

    /* Step 7: Call the recompiled entry point */
    printf("\nStarting game...\n");
    fflush(stdout);

    xbe_entry_point();

    printf("\nGame returned. Cleaning up...\n");

    /* Cleanup */
    xbox_kernel_shutdown();
    xbox_MemoryLayoutShutdown();
    free(xbe_data);

    return 0;
}

/* ── XBE Loading ───────────────────────────────────────────── */

static BOOL load_xbe(const wchar_t *path, void **out_data, size_t *out_size)
{
    FILE *f = _wfopen(path, L"rb");
    if (!f) {
        char p8[3 * MAX_PATH];
        if (!WideCharToMultiByte(CP_UTF8, 0, path, -1, p8, sizeof p8, NULL, NULL))
            p8[0] = 0;
        fprintf(stderr, "Cannot open XBE: %s (errno %d)\n", p8, errno);
        return FALSE;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0) {
        fclose(f);
        return FALSE;
    }

    void *data = malloc((size_t)size);
    if (!data) {
        fclose(f);
        return FALSE;
    }

    if (fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data);
        fclose(f);
        return FALSE;
    }

    fclose(f);
    *out_data = data;
    *out_size = (size_t)size;
    return TRUE;
}

/* Console entry point (for debugging -- lets you see printf output) */
int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return WinMain(GetModuleHandle(NULL), NULL, GetCommandLineA(), SW_SHOW);
}
