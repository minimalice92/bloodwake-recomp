/**
 * Show the guest framebuffer in a window.
 *
 * The title renders into its own framebuffer in guest RAM and tells the kernel
 * where it is through AvSetDisplayMode; on hardware the CRTC scans that memory
 * out. Nothing here scans anything out, so however much of the GPU is
 * implemented, none of it is observable. This is the other half: a window that
 * reads that memory and puts it on screen.
 *
 * Deliberately plain GDI rather than the D3D8 layer. The point is to display
 * whatever the guest actually wrote, so the fewer stages between guest memory
 * and the screen the better -- and it must keep working while the D3D8 layer
 * is busy with something else, such as the FMV player's own window.
 * RECOMP_D3D_PRESENT=1 shows the same frames through a D3D11 swap chain of
 * the window's own instead (gpu_present.c): scaled on the GPU, vsync-paced.
 *
 * The window opens at RECOMP_WINDOW_SCALE times the frame (default 2, made
 * smaller until it fits the desktop) and can be resized; the frame is
 * letterboxed to its 4:3 shape. F11 or Alt+Enter toggles borderless
 * fullscreen, RECOMP_FULLSCREEN=1 starts in it. Closing the window ends the
 * process, as the SDL window does.
 *
 * Off unless RECOMP_FB_WINDOW is set.
 */
#include <stdint.h>

#if defined(_WIN32)
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* Which keys are down, for the pad stand-in in src/input: this window
 * records them in the shared key table (src/input/keyboard.c).
 *
 * GetAsyncKeyState looked like the cheaper way to ask and does not work
 * here: it reads a state Wine keeps for the X server, and a guest process
 * drawing through GDI never sees it change. The window that has the focus
 * is the thing that receives the keys, so that is what has to remember
 * them. */
#include "keyboard.h"   /* src/input */
#include "../input/bw_input_ext.h"   /* Blood Wake: rumble off, focus (0.2.5 item 74) */
#include "gpu_present.h"
#include "recomp_exit_hook.h"   /* src/platform */
#include <stdbool.h>
#include "pvideo_scanout.h"
#include "../nv2a/nv2a_mmio_hook.h"

extern ptrdiff_t xbox_GetMemoryOffset(void);

static volatile LONG s_fb_running;
static uint32_t      s_fb_va, s_fb_pitch, s_fb_width = 640, s_fb_height = 480;
static uint32_t     *s_rgb;           /* converted 32-bit copy for GDI */

/* A finished frame, taken at the flip and shown until the next one.
 *
 * The window used to convert straight out of guest memory every 16 ms. Even
 * pointed at the buffer the title had just finished, that races the executor
 * drawing the next frame into the other one and, whenever the two swap, puts
 * a half-drawn image on the screen -- which is the flicker. Copying the
 * finished frame once per flip means the window never reads memory the
 * rasteriser is writing, so what it shows cannot be half of anything.
 *
 * Two buffers and an index, swapped after the copy completes, so the window
 * thread is never reading the one being filled. */
static uint32_t     *s_present[2];
static volatile LONG s_present_idx = -1;   /* -1 until the first flip */

/* The last PVIDEO frame shown. A frame stays on screen until the next one or
 * STOP, but once its bank is consumed the title may reuse the memory, so the
 * pixels are kept here rather than read again. */
static uint8_t      *s_pvideo_source;
static size_t        s_pvideo_bytes;
static unsigned      s_pvideo_bank;
static uint32_t      s_pvideo_regs[0x1000 / 4];

/* Draw the video overlay over s_rgb: a newly submitted bank if there is one
 * (copied, drawn, then handed back to the title), else the last frame. */
static void fb_overlay(void)
{
    uint32_t regs[0x1000 / 4];
    unsigned bank;
    bool presented = false;

    if (!s_rgb || !nv2a_hook_pvideo_snapshot(regs)) return;
    if ((regs[NV_PVIDEO_STOP / 4] & 1u) ||
        (s_pvideo_source &&
         regs[(NV_PVIDEO_SIZE_IN + s_pvideo_bank * 4) / 4] == UINT32_MAX)) {
        free(s_pvideo_source);
        s_pvideo_source = NULL;
        if (regs[NV_PVIDEO_STOP / 4] & 1u) return;
    }
    for (bank = 0; bank < 2; ++bank) {
        uint64_t physical = (uint64_t)regs[(NV_PVIDEO_BASE + bank * 4) / 4] +
                            regs[(NV_PVIDEO_OFFSET + bank * 4) / 4];
        unsigned pitch = regs[(NV_PVIDEO_FORMAT + bank * 4) / 4] & NV_PVIDEO_FORMAT_PITCH;
        unsigned height = (regs[(NV_PVIDEO_SIZE_IN + bank * 4) / 4] >> 16) & 0x7ff;
        uint64_t bytes = (uint64_t)pitch * height;
        uintptr_t va;
        MEMORY_BASIC_INFORMATION mapping;
        uint8_t *snapshot;

        if (!(regs[NV_PVIDEO_BUFFER / 4] & (1u << (bank * 4u)))) continue;
        if (!bytes || physical + bytes > 0x04000000u) continue;
        va = (uintptr_t)(0x80000000u + (uint32_t)physical) +
             (uintptr_t)xbox_GetMemoryOffset();
        if (!VirtualQuery((const void *)va, &mapping, sizeof(mapping)) ||
            mapping.State != MEM_COMMIT ||
            (mapping.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
            bytes > mapping.RegionSize - (size_t)(va - (uintptr_t)mapping.BaseAddress))
            continue;
        snapshot = (uint8_t *)malloc((size_t)bytes);
        if (!snapshot) continue;
        memcpy(snapshot, (const void *)va, (size_t)bytes);
        if (!pvideo_scanout(regs, snapshot, (size_t)bytes, s_rgb,
                            s_fb_width, s_fb_height, bank)) {
            free(snapshot);
            continue;
        }
        free(s_pvideo_source);
        s_pvideo_source = snapshot;
        s_pvideo_bytes = (size_t)bytes;
        s_pvideo_bank = bank;
        memcpy(s_pvideo_regs, regs, sizeof(s_pvideo_regs));
        presented = true;
        nv2a_hook_pvideo_consume(bank);
    }
    if (!presented && s_pvideo_source)
        pvideo_scanout(s_pvideo_regs, s_pvideo_source, s_pvideo_bytes, s_rgb,
                       s_fb_width, s_fb_height, s_pvideo_bank);
}

void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch)
{
    /* RECOMP_FB_VA pins the window to one guest address instead of following
     * whichever surface is being drawn into. A black window cannot distinguish
     * "the read path is broken" from "the title rendered black", and pointing
     * it at memory known to have content settles that. */
    const char *pin = getenv("RECOMP_FB_VA");

    s_fb_va = pin ? (uint32_t)strtoul(pin, NULL, 0) : fb_va;
    if (pitch)
        s_fb_pitch = pitch;
}

/* Vertical anti-aliasing factor of the surface being shown: 2 for 2x2, where
 * the surface has twice the rows as well as twice the columns, and 1 for
 * everything else (2x1 included). Only the pushbuffer executor knows it. */
static uint32_t s_fb_aa_sy = 1;

void xbox_FramebufferWindowSetAA(uint32_t sy)
{
    s_fb_aa_sy = sy ? sy : 1;
}

/* Called by the pushbuffer executor when the title flips. */
/* Blood Wake carried patch: while the GPU back end presents the frame itself
 * (bw_fb_external_present) and no window dump is asked for, this window
 * shows nothing of its own, so the per-flip copy of the frame (two per flip:
 * the executor's and the back end's) is skipped: 4.7% of the NV2A thread at
 * high frame rates (profile run 20261027-023720-r6-gpu-d7hfprof2). The
 * output is unchanged; the window's title (FPS) still updates. */
static volatile LONG s_bw_external;
/* Blood Wake carried patch (P1), test runs only: BW_FB_WINDOW_COPY=0 skips
 * the copy (and its CPU gamma) even when the GPU does not present, so an
 * off-screen frame-rate run pays what real play pays. With readback off the
 * guest surface holds no finished picture anyway; the window shows nothing.
 * It was 16% of the NV2A thread in such runs (profile -132820-p1swprof15). */
static int bw_fb_nocopy(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("BW_FB_WINDOW_COPY");
        v = e && e[0] == '0';
    }
    return v;
}
static int bw_fb_copy_needed(void)
{
    static int dump = -1, pinned = -1;
    if (dump < 0)
        dump = getenv("RECOMP_FB_DUMP") != NULL;
    if (pinned < 0)
        pinned = getenv("RECOMP_FB_VA") != NULL;
    return pinned ? -1 : ((!s_bw_external && !bw_fb_nocopy()) || dump);
}

/* Blood Wake carried patch (TK-51): resolve a 2x-wide frame with the title's
 * quincunx filter (BW_GPU_CONV, default on). */
static int bw_resolve_conv(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("BW_GPU_CONV");
        on = !(e && e[0] == '0');
    }
    return on;
}

void xbox_FramebufferWindowPresent(uint32_t fb_va, uint32_t pitch)
{
    const uint8_t *src;
    LONG next;
    uint32_t bpp, x, y;
    int need;

    if (!s_fb_running || !fb_va || !pitch)
        return;
    need = bw_fb_copy_needed();
    if (need < 0)
        return;                       /* pinned: leave the old path alone */
    if (!need)
        return;                       /* Blood Wake: nothing here shows it */
    next = (s_present_idx == 0) ? 1 : 0;
    if (!s_present[next]) {
        s_present[next] = (uint32_t *)calloc((size_t)s_fb_width * s_fb_height,
                                             4);
        if (!s_present[next])
            return;
    }
    bpp = pitch / s_fb_width;
    src = (const uint8_t *)((uintptr_t)fb_va + xbox_GetMemoryOffset());
    for (y = 0; y < s_fb_height; y++) {
        const uint8_t *row = src + (size_t)y * pitch * (bpp == 8 ? s_fb_aa_sy : 1);
        uint32_t *dst = s_present[next] + (size_t)y * s_fb_width;

        if (bpp == 4) {
            memcpy(dst, row, (size_t)s_fb_width * 4);
        } else if (bpp == 8 && s_fb_aa_sy == 1 && bw_resolve_conv()) {
            /* Blood Wake carried patch (TK-51): the title's own resolve,
             * D3DTEXF_QUINCUNX, as the GPU back end draws it (xemu's 3x3
             * Gaussian of bilinear taps one texel apart, centred on each
             * pixel's two samples): across, [1 3 3 1]/8 over samples
             * 2x-1..2x+2; down, [1 2 1]/4 over rows y-1..y+1; edges clamp.
             * BW_GPU_CONV=0 takes every other sample, as before. */
            const uint32_t *rows[3];
            uint32_t w2 = s_fb_width * 2, k, ch;
            rows[0] = (const uint32_t *)(src + (size_t)(y ? y - 1 : 0) * pitch);
            rows[1] = (const uint32_t *)row;
            rows[2] = (const uint32_t *)(src + (size_t)(y + 1 < s_fb_height ? y + 1 : y) * pitch);
            for (x = 0; x < s_fb_width; x++) {
                uint32_t i0 = x ? 2 * x - 1 : 0, i3 = 2 * x + 2 < w2 ? 2 * x + 2 : w2 - 1, out = 0;
                for (ch = 0; ch < 24; ch += 8) {
                    uint32_t acc = 0;
                    for (k = 0; k < 3; k++) {
                        const uint32_t *p = rows[k];
                        uint32_t hsum = ((p[i0] >> ch) & 0xFF) + 3 * ((p[2 * x] >> ch) & 0xFF)
                                      + 3 * ((p[2 * x + 1] >> ch) & 0xFF) + ((p[i3] >> ch) & 0xFF);
                        acc += hsum * (k == 1 ? 2 : 1);
                    }
                    out |= ((acc + 16) / 32) << ch;
                }
                dst[x] = out;
            }
        } else if (bpp == 8) {
            /* 2x-wide anti-aliased 32-bit surface: every other pixel, as in
             * fb_convert. */
            const uint32_t *p = (const uint32_t *)row;
            for (x = 0; x < s_fb_width; x++)
                dst[x] = p[x * 2];
        } else if (bpp == 2) {
            const uint16_t *p = (const uint16_t *)row;
            for (x = 0; x < s_fb_width; x++) {
                uint16_t v = p[x];
                uint32_t r = (uint32_t)((v >> 11) & 0x1F) * 255u / 31u;
                uint32_t g = (uint32_t)((v >>  5) & 0x3F) * 255u / 63u;
                uint32_t b = (uint32_t)( v        & 0x1F) * 255u / 31u;
                dst[x] = (r << 16) | (g << 8) | b;
            }
        } else {
            memset(dst, 0, (size_t)s_fb_width * 4);
        }
    }
    /* Blood Wake carried patch: the title's DAC gamma ramp (src/gpu/bw_gamma.c,
     * BW_GPU_GAMMA), as the console's video output applied it. Not needed
     * while the GPU back end presents into this window itself. */
    if (!s_bw_external) {
        extern void bw_gamma_apply_xrgb(uint32_t *px, size_t n);
        bw_gamma_apply_xrgb(s_present[next], (size_t)s_fb_width * s_fb_height);
    }
    /* Published only once it is whole. */
    InterlockedExchange(&s_present_idx, next);
}

/* Title bar, the way ps3recomp's window shows it. Written by the flip,
 * read once a second by the window thread; a torn read shows one stale
 * number for a second, which nobody can tell apart from a real one. */
static wchar_t       s_title[48] = L"Xbox Recomp";
static volatile LONG s_flips;
static volatile LONG s_frame_draws;

void xbox_FramebufferWindowSetTitle(const uint16_t *name, int max_chars)
{
    int i;
    for (i = 0; i < max_chars && i < 47 && name[i]; i++)
        s_title[i] = (wchar_t)name[i];
    if (i)
        s_title[i] = 0;
}

void xbox_FramebufferWindowFrameStats(uint32_t draws)
{
    InterlockedIncrement(&s_flips);
    InterlockedExchange(&s_frame_draws, (LONG)draws);
}

/* Blood Wake (carried patch): BW_PARENT_HWND=<handle> is the launcher's
 * window (src/bw_host.c), which stays up across the game's self-reboots. The
 * frame is drawn into a child window filling it; fullscreen and the title
 * bar are the host's. Without it, the toolkit's own top-level window. */
#define BW_HOST_FULLSCREEN (WM_APP + 0x42)    /* child -> host: Alt+Enter / F11 */
static HWND s_bw_parent;

/* Blood Wake carried patch: presenting from the GPU (src/gpu/bw_d3d11.c,
 * BW_GPU_PRESENT). The back end draws each flipped frame into this window
 * through its own swap chain; while it does, this thread stops painting the
 * window (GDI stretched every frame on the CPU: 70% of this thread). */
static HWND volatile s_bw_hwnd;

void *bw_fb_window(void) { return (void *)s_bw_hwnd; }

/* Blood Wake carried patch (Remastered widescreen, src/bw_widescreen.h): the
 * window's shape, width / height. 0, as in Original, keeps the frame's own
 * 640x480. main.c sets it before the title runs, so before the window opens. */
static float s_bw_aspect;
void bw_fb_set_window_aspect(float aspect) { s_bw_aspect = aspect; }
void bw_fb_external_present(int on) { InterlockedExchange(&s_bw_external, on ? 1 : 0); }

static HWND bw_parent_window(void)
{
    const char *e = getenv("BW_PARENT_HWND");
    HWND h;
    if (!e || !*e)
        return NULL;
    h = (HWND)(uintptr_t)_strtoui64(e, NULL, 0);
    return IsWindow(h) ? h : NULL;
}

/* Alt+Enter / F11: borderless fullscreen on the window's monitor, and back.
 * (DXGI's own Alt+Enter, exclusive mode at the frame's 640x480, is off --
 * gpu_present.c; both presenters letterbox to any size.) */
static void fb_toggle_fullscreen(HWND h)
{
    static int full;
    static RECT saved;
    static LONG_PTR saved_style;
    if (!full) {
        MONITORINFO mi;
        mi.cbSize = sizeof mi;
        if (!GetMonitorInfoA(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi))
            return;
        GetWindowRect(h, &saved);
        saved_style = GetWindowLongPtrA(h, GWL_STYLE);
        SetWindowLongPtrA(h, GWL_STYLE, (saved_style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        SetWindowPos(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        full = 1;
    } else {
        SetWindowLongPtrA(h, GWL_STYLE, saved_style);
        SetWindowPos(h, NULL, saved.left, saved.top, saved.right - saved.left,
                     saved.bottom - saved.top, SWP_FRAMECHANGED | SWP_NOZORDER | SWP_SHOWWINDOW);
        full = 0;
    }
}

static void bw_key_delay_note(void);    /* Blood Wake (item 75), below */

/* Blood Wake carried patch (0.2.5 item 79): the mouse over the frame window,
 * for the menus' mouse (src/bw_menu_hooks.c, MENU_MOUSE). Written by this
 * window's thread, read by the game's; each value is one aligned LONG. */
static volatile LONG s_bw_mx = -1, s_bw_my = -1, s_bw_cw, s_bw_ch;
static volatile LONG s_bw_moves, s_bw_wheel, s_bw_wheel_rest;
static volatile LONG s_bw_lclicks, s_bw_rclicks;
/* 0.3.3: the pointer's last place on the screen, to tell a move from a
 * pointer at rest (this window's thread only). */
static POINT s_bw_last_pt;
static int s_bw_have_pt;

/* Left and right button presses since the last call (each press counted once,
 * however short). */
void bw_fb_mouse_clicks_take(int *left, int *right)
{
    *left = (int)InterlockedExchange(&s_bw_lclicks, 0);
    *right = (int)InterlockedExchange(&s_bw_rclicks, 0);
}

/* The cursor in client pixels and the client size; *moves counts the
 * pointer's real moves over the window so far (a caller tells a moved mouse
 * from one at rest; WM_MOUSEMOVE below).
 * 0 when the cursor isn't over the window. */
int bw_fb_mouse(int *x, int *y, int *cw, int *ch, int *moves)
{
    *x = (int)s_bw_mx;
    *y = (int)s_bw_my;
    *cw = (int)s_bw_cw;
    *ch = (int)s_bw_ch;
    *moves = (int)s_bw_moves;
    return *x >= 0 && *y >= 0 && *cw > 0 && *ch > 0;
}

/* Whole wheel notches turned since the last call: positive away from the
 * player (up), negative towards. */
int bw_fb_mouse_wheel_take(void)
{
    return (int)InterlockedExchange(&s_bw_wheel, 0);
}

/* src/bw_menu_hooks.c: 1 show the arrow over the window, 0 hide it, -1 the
 * window's own cursor as always (Original, MENU_MOUSE off). */
extern int bw_menu_cursor_wanted(void);

/* Blood Wake carried patch (0.3.3): the display stays awake while the game is
 * in front. The Xbox never slept, so a screen that goes dark during pad play
 * (no keyboard or mouse input for the display-off time) is the port's doing.
 * While this window's top-level window (the play host's, in a host) is the
 * foreground window and isn't minimised, this thread holds
 * ES_CONTINUOUS | ES_DISPLAY_REQUIRED; it lets go when the window goes to the
 * back, is minimised or closes, and Windows drops it when the process ends
 * (a reboot of the game). Each change is logged once. No Windows setting is
 * changed. Test runs (BW_UNATTENDED) leave the display's own timer alone;
 * BW_DISPLAY_AWAKE=1 holds it there too (tests of this), 0 never holds it. */
static int s_bw_awake_held;

static int bw_awake_wanted(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("BW_DISPLAY_AWAKE");
        if (e && *e)
            on = e[0] != '0';
        else
            on = getenv("BW_UNATTENDED") == NULL;
        fprintf(stderr, "  [POWER] the display stays awake while the game is in front: %s\n",
                on ? "on" : (e && *e) ? "off (BW_DISPLAY_AWAKE=0)"
                   : "off in a test run (BW_UNATTENDED; BW_DISPLAY_AWAKE=1 holds it)");
    }
    return on;
}

static void bw_awake_set(int hold, const char *why)
{
    static int logged;
    EXECUTION_STATE prev;
    if (hold == s_bw_awake_held)
        return;
    prev = SetThreadExecutionState(hold ? (ES_CONTINUOUS | ES_DISPLAY_REQUIRED) : ES_CONTINUOUS);
    s_bw_awake_held = hold;
    if (logged++ < 64)
        fprintf(stderr, "  [POWER] %s: %s%s\n", hold ? "display kept awake" : "display may sleep again",
                why, prev ? "" : " (the call failed)");
    else if (logged == 65)
        fprintf(stderr, "  [POWER] further changes not logged\n");
}

/* Each pass of the window's loop. */
static void bw_awake_tick(HWND h)
{
    HWND top;
    if (!bw_awake_wanted())
        return;
    top = GetAncestor(h, GA_ROOT);
    if (!top)
        top = h;
    if (IsIconic(top))
        bw_awake_set(0, "the window is minimised");
    else if (GetForegroundWindow() != top)
        bw_awake_set(0, "the window is in the back");
    else
        bw_awake_set(1, "the game is in front");
}

static LRESULT CALLBACK fb_wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    /* First press only (bit 30: the key was already down), and with Alt held
     * (bit 29) for Enter; neither reaches the key table. */
    if ((m == WM_SYSKEYDOWN && w == VK_RETURN && (l & (1 << 29)) && !(l & (1 << 30)))
        || (m == WM_KEYDOWN && w == VK_F11 && !(l & (1 << 30)))) {
        if (s_bw_parent)
            PostMessageA(s_bw_parent, BW_HOST_FULLSCREEN, 0, 0);
        else
            fb_toggle_fullscreen(h);
        return 0;
    }
    if (m == WM_SYSKEYDOWN && w == VK_RETURN)
        return 0;                       /* auto-repeat of Alt+Enter */
    switch (m) {
    case WM_CLOSE:
    case WM_DESTROY:
        /* Closing the window ends the run. Only stopping this thread left
         * every other one running -- audio played on, and the process had
         * to be ended from Task Manager. The guest has no exit path of its
         * own to ask. TerminateProcess rather than ExitProcess: the guest
         * threads are still running, and ExitProcess's cleanup can wait on
         * a lock one of them held. The exit hook does the reporting that
         * would otherwise be skipped. RECOMP_CLOSE_KEEPS_RUNNING=1 only
         * closes the window, as before. */
        InterlockedExchange(&s_fb_running, 0);
        bw_awake_set(0, "the game is closing");     /* Blood Wake (0.3.3) */
        if (!getenv("RECOMP_CLOSE_KEEPS_RUNNING")) {
            fprintf(stderr, "  [FBWIN] window closed: exiting\n");
            /* Blood Wake carried patch (0.2.5 item 74): a pad left rumbling
             * by the last explosion stays on after the process ends. */
            xbox_InputRumbleAllOff("window closed");
            recomp_exit_hook_run("window");
            fflush(stdout);
            fflush(stderr);
            {
                /* Blood Wake carried patch (au-vp): the game's log goes
                 * through a ring and a writer thread (src/main.c); what is
                 * still in them reaches the file before the process ends.
                 * Every closing of the window comes here: its close box, the
                 * play host's quit and the menu's Quit Game. */
                extern void bw_log_drain(int wait_ms);
                bw_log_drain(500);
            }
            TerminateProcess(GetCurrentProcess(), 0);
        }
        return 0;

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        xbox_InputKeySet((int)w, 1);
        if (!(l & (1 << 30)))
            bw_key_delay_note();        /* Blood Wake (item 75) */
        /* A Shift, Ctrl or Alt is also its side's key, for bindings to
         * "lshift" and the like; wParam stays the either-side one. */
        {
            int side = xbox_InputWin32SideVk((int)w, (uint32_t)l);
            if (side)
                xbox_InputKeySet(side, 1);
        }
        /* RECOMP_KEY_TRACE: each key as it arrives, edge-triggered.
         *
         * The obvious diagnostic -- sampling which keys are held, once a
         * second, from the input path -- cannot tell a key that was never
         * pressed from one that was tapped: a 100 ms press is caught about
         * one time in ten. That ambiguity is expensive when the only way
         * to test is to ask someone to press a key and describe what
         * happened. This answers "did it arrive" on its own. */
        if (getenv("RECOMP_KEY_TRACE")) {
            static unsigned n;
            if (n++ < 40) {
                fprintf(stderr, "  [KEY] down vk=0x%02X\n", (unsigned)w);
                fflush(stderr);
            }
        }
        /* System keys still go to Windows, or Alt+F4 stops closing us. */
        return m == WM_SYSKEYDOWN ? DefWindowProcA(h, m, w, l) : 0;

    case WM_KEYUP:
    case WM_SYSKEYUP:
        xbox_InputKeySet((int)w, 0);
        {
            int side = xbox_InputWin32SideVk((int)w, (uint32_t)l);
            if (side)
                xbox_InputKeySet(side, 0);
            /* With both Shifts down Windows sends one key-up, for the
             * last released, so the other side would stay held. Autorepeat
             * sets a Shift still down again. */
            if (w == VK_SHIFT) {
                xbox_InputKeySet(VK_LSHIFT, 0);
                xbox_InputKeySet(VK_RSHIFT, 0);
            }
        }
        return m == WM_SYSKEYUP ? DefWindowProcA(h, m, w, l) : 0;

    /* Mouse buttons go in the same table, for bindings that name them.
     * Capturing the mouse while one is down delivers its release even
     * outside the window. */
    case WM_LBUTTONDOWN: case WM_LBUTTONUP:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP:
    case WM_XBUTTONDOWN: case WM_XBUTTONUP:
        {
            int down = 0;
            int vk = xbox_InputWin32MouseVk(m, (uint32_t)w, &down);
            if (vk)
                xbox_InputKeySet(vk, down);
            if (m == WM_LBUTTONDOWN)            /* Blood Wake (item 79): the menus' mouse */
                InterlockedIncrement(&s_bw_lclicks);
            else if (m == WM_RBUTTONDOWN)
                InterlockedIncrement(&s_bw_rclicks);
            if (down)
                SetCapture(h);
            else if (!(w & (MK_LBUTTON | MK_RBUTTON | MK_MBUTTON | MK_XBUTTON1 | MK_XBUTTON2)))
                ReleaseCapture();
        }
        return m == WM_XBUTTONDOWN || m == WM_XBUTTONUP ? TRUE : 0;

    /* Blood Wake carried patch (0.2.5 item 79): where the mouse is, and the
     * wheel, for the menus' mouse; the cursor shown or hidden as it asks. */
    case WM_MOUSEMOVE:
        {
            RECT c;
            POINT pt;
            LONG nx = (LONG)(short)LOWORD(l), ny = (LONG)(short)HIWORD(l);
            GetClientRect(h, &c);
            InterlockedExchange(&s_bw_cw, c.right - c.left);
            InterlockedExchange(&s_bw_ch, c.bottom - c.top);
            /* Windows also sends WM_MOUSEMOVE with the pointer at rest: when
             * the window appears under it, and when the window moves, resizes
             * or is uncovered under it. Only a real move counts, so a resting
             * pointer never takes the highlight: the pointer's place on the
             * screen has to change, and the window's first mouse message is
             * only where the pointer starts (0.3.3; the 0.3.1 gate's finding
             * 12: a window opening under a resting pointer counted a move, as
             * s_bw_mx started at -1, and took the main menu's highlight). */
            pt.x = nx;
            pt.y = ny;
            ClientToScreen(h, &pt);
            if (s_bw_have_pt && (pt.x != s_bw_last_pt.x || pt.y != s_bw_last_pt.y))
                InterlockedIncrement(&s_bw_moves);
            else if (!s_bw_have_pt && getenv("BW_MENU_MOUSE_TRACE"))
                fprintf(stderr, "[MENU] mouse trace: the window's first mouse message, at %ld,%ld"
                        " in the window: where the pointer starts, not a move\n", (long)nx, (long)ny);
            s_bw_last_pt = pt;
            s_bw_have_pt = 1;
            InterlockedExchange(&s_bw_mx, nx);
            InterlockedExchange(&s_bw_my, ny);
            {
                TRACKMOUSEEVENT t;
                t.cbSize = sizeof t;
                t.dwFlags = TME_LEAVE;
                t.hwndTrack = h;
                t.dwHoverTime = 0;
                TrackMouseEvent(&t);
            }
        }
        return 0;
    case WM_MOUSELEAVE:
        /* BW_TEST_MOUSE=1 (tests): moves posted by tools/bw_keypost.py stay
         * put, though the real cursor is elsewhere and Windows says so. */
        if (!getenv("BW_TEST_MOUSE")) {
            InterlockedExchange(&s_bw_mx, -1);
            InterlockedExchange(&s_bw_my, -1);
        }
        return 0;
    case WM_MOUSEWHEEL:
        {
            LONG d = s_bw_wheel_rest + (LONG)(short)HIWORD(w);
            InterlockedExchangeAdd(&s_bw_wheel, d / WHEEL_DELTA);
            s_bw_wheel_rest = d % WHEEL_DELTA;
        }
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT) {
            int want = bw_menu_cursor_wanted();
            if (want >= 0) {
                SetCursor(want ? LoadCursorA(NULL, IDC_ARROW) : NULL);
                return TRUE;
            }
        }
        break;

    /* Alt-tabbing away with a key held would leave it held for ever. */
    case WM_KILLFOCUS:
        xbox_InputKeysClear();
        xbox_InputSetFocus(0);          /* Blood Wake (item 74): rumble stops */
        return 0;
    case WM_SETFOCUS:
        xbox_InputSetFocus(1);          /* Blood Wake (item 74): and comes back */
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

/* Blood Wake carried patch (0.2.5 item 75): the window's loop slept 16 ms a
 * pass while the GPU back end presents (bw_fb_external_present), so a key
 * press waited in the message queue for up to 16 ms before it reached the key
 * table the game's pad reads. It now waits on the queue: a message wakes it
 * at once and is handled, and it still goes round once per 16 ms for the
 * title bar and the dumps. Messages other threads send to the window (DXGI's
 * resize and mode changes) are answered at once too, instead of blocking the
 * sender for up to 16 ms. BW_FB_INPUT_WAIT=0 restores the plain Sleep. */
static int bw_input_wait(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("BW_FB_INPUT_WAIT");
        on = !(e && e[0] == '0');
    }
    return on;
}

static int fb_pump(void)
{
    MSG msg;
    int n = 0;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
        n++;
    }
    return n;
}

static void fb_wait(DWORD ms)
{
    LARGE_INTEGER f, t0, t;
    if (!bw_input_wait()) {
        Sleep(ms);
        return;
    }
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    for (;;) {
        LONGLONG el;
        QueryPerformanceCounter(&t);
        el = (t.QuadPart - t0.QuadPart) * 1000 / f.QuadPart;
        if (el >= (LONGLONG)ms || !InterlockedCompareExchange(&s_fb_running, 1, 1))
            return;
        if (MsgWaitForMultipleObjectsEx(0, NULL, (DWORD)(ms - el), QS_ALLINPUT,
                                        MWMO_INPUTAVAILABLE) != WAIT_OBJECT_0)
            return;                          /* the 16 ms are up */
        if (!fb_pump())
            Sleep(1);                        /* woke with nothing to read: don't spin */
    }
}

/* Blood Wake carried patch (item 75): how long key presses waited in the
 * window's queue before reaching the key table, from each WM_KEYDOWN's own
 * time stamp. With BW_GPU_STATS=1 (or BW_INPUT_STATS=1), one line every 5 s
 * that had presses. GetTickCount's 15.6 ms steps make one press's figure
 * coarse; the mean over many is the measure. */
static void bw_key_delay_note(void)
{
    static int on = -1;
    static unsigned n, maxd;
    static unsigned long long sum;
    static DWORD since;
    DWORD now = GetTickCount(), d = now - (DWORD)GetMessageTime();
    if (on < 0)
        on = getenv("BW_GPU_STATS") != NULL || getenv("BW_INPUT_STATS") != NULL;
    if (!on)
        return;
    if (d > 10000)
        d = 0;                              /* a stamp from before a wrap */
    if (!n)
        since = now;
    n++;
    sum += d;
    if (d > maxd)
        maxd = d;
    if (now - since >= 5000) {
        fprintf(stderr, "[INPUT] keys: %u presses in %lu s, waited in the window queue %.1f ms on average (max %u ms)\n",
                n, (unsigned long)((now - since) / 1000), (double)sum / n, maxd);
        n = 0;
        sum = 0;
        maxd = 0;
    }
}

/* The display formats an Xbox front buffer is actually set to. The pitch says
 * how wide a row is in bytes, so pitch/width gives the pixel size; the exact
 * component layout only matters for 16-bit, where 5:6:5 and 1:5:5:5 differ. */
static void fb_convert(const uint8_t *src, uint32_t bpp)
{
    uint32_t x, y;

    for (y = 0; y < s_fb_height; y++) {
        const uint8_t *row = src + (size_t)y * s_fb_pitch
                           * (bpp == 8 ? s_fb_aa_sy : 1);
        uint32_t *dst = s_rgb + (size_t)y * s_fb_width;

        if (bpp == 4) {
            memcpy(dst, row, (size_t)s_fb_width * 4);
        } else if (bpp == 8) {
            /* A 2x-wide anti-aliased 32-bit surface (pitch = 2 * width * 4):
             * take every other pixel. X-Men Legends renders its 640-wide
             * frame into a 1280-wide surface, which read as "8 bytes per
             * pixel" and showed black. */
            const uint32_t *p = (const uint32_t *)row;
            for (x = 0; x < s_fb_width; x++)
                dst[x] = p[x * 2];
        } else if (bpp == 2) {
            const uint16_t *p = (const uint16_t *)row;
            for (x = 0; x < s_fb_width; x++) {
                uint16_t v = p[x];
                uint32_t r = (uint32_t)((v >> 11) & 0x1F) * 255u / 31u;
                uint32_t g = (uint32_t)((v >>  5) & 0x3F) * 255u / 63u;
                uint32_t b = (uint32_t)( v        & 0x1F) * 255u / 31u;
                dst[x] = (r << 16) | (g << 8) | b;
            }
        } else {
            memset(dst, 0, (size_t)s_fb_width * 4);
        }
    }
}

/* Write what the window is currently showing to a 24-bit BMP.
 *
 * A black window is ambiguous: it means either that the read path is wrong or
 * that the title really did render black. Dumping the same converted pixels
 * the window draws settles which, and does it without a screenshot. */
int xbox_FramebufferDumpBmp(const char *path)
{
    FILE *f;
    uint32_t row = ((s_fb_width * 3u) + 3u) & ~3u;
    uint32_t img = row * s_fb_height, total = 54u + img, y, x;
    uint8_t hdr[54], *line;

    if (!s_rgb || !s_fb_va)
        return -1;
    f = fopen(path, "wb");
    if (!f)
        return -1;
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &total, 4);
    hdr[10] = 54; hdr[14] = 40;
    memcpy(hdr + 18, &s_fb_width, 4);
    memcpy(hdr + 22, &s_fb_height, 4);
    hdr[26] = 1; hdr[28] = 24;
    memcpy(hdr + 34, &img, 4);
    fwrite(hdr, 1, sizeof(hdr), f);

    line = (uint8_t *)calloc(1, row);
    for (y = 0; y < s_fb_height; y++) {
        const uint32_t *src = s_rgb + (size_t)(s_fb_height - 1 - y) * s_fb_width;
        for (x = 0; x < s_fb_width; x++) {
            line[x * 3 + 0] = (uint8_t)(src[x] & 0xFF);
            line[x * 3 + 1] = (uint8_t)((src[x] >> 8) & 0xFF);
            line[x * 3 + 2] = (uint8_t)((src[x] >> 16) & 0xFF);
        }
        fwrite(line, 1, row, f);
    }
    free(line);
    fclose(f);
    fprintf(stderr, "  [FBWIN] wrote %s (%ux%u from 0x%08X)\n",
            path, s_fb_width, s_fb_height, s_fb_va);
    return 0;
}

#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")

static int bw_dwm_sync(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("BW_FB_DWMSYNC");
        on = !(e && e[0] == '0');
    }
    return on;
}

/* GDI: the frame letterboxed into the client area, like the D3D11
 * presenter; the borders are cleared when the size changes. */
static void gdi_blit(HWND hwnd, HDC hdc, const BITMAPINFO *bi)
{
    static LONG lw, lh;
    RECT c;
    LONG cw, ch, w, h;
    GetClientRect(hwnd, &c);
    cw = c.right - c.left; ch = c.bottom - c.top;
    if (cw <= 0 || ch <= 0)
        return;
    if (cw != lw || ch != lh) {
        FillRect(hdc, &c, (HBRUSH)GetStockObject(BLACK_BRUSH));
        lw = cw; lh = ch;
    }
    if ((long long)cw * s_fb_height <= (long long)ch * s_fb_width) {
        w = cw; h = (LONG)((long long)cw * s_fb_height / s_fb_width);
    } else {
        h = ch; w = (LONG)((long long)ch * s_fb_width / s_fb_height);
    }
    if (bw_dwm_sync()) {
        /* Blood Wake (carried patch): the slow HALFTONE stretch goes into a
         * memory bitmap, then, right after the compositor's pass
         * (DwmFlush), one plain BitBlt puts it in the window. Stretched
         * straight into the window it took milliseconds, and DWM could
         * compose it half drawn: a frame torn across sky and water,
         * worst in the cut-scenes' camera pans. BW_FB_DWMSYNC=0 reverts. */
        static HDC mdc;
        static HBITMAP bmp;
        static LONG bw_, bh_;
        if (!mdc)
            mdc = CreateCompatibleDC(hdc);
        if (!bmp || bw_ != w || bh_ != h) {
            if (bmp)
                DeleteObject(bmp);
            bmp = CreateCompatibleBitmap(hdc, w, h);
            SelectObject(mdc, bmp);
            bw_ = w; bh_ = h;
        }
        SetStretchBltMode(mdc, HALFTONE);
        SetBrushOrgEx(mdc, 0, 0, NULL);
        StretchDIBits(mdc, 0, 0, w, h, 0, 0, (int)s_fb_width, (int)s_fb_height,
                      s_rgb, bi, DIB_RGB_COLORS, SRCCOPY);
        DwmFlush();
        BitBlt(hdc, (cw - w) / 2, (ch - h) / 2, w, h, mdc, 0, 0, SRCCOPY);
        return;
    }
    SetStretchBltMode(hdc, HALFTONE);
    SetBrushOrgEx(hdc, 0, 0, NULL);     /* HALFTONE wants it after the mode */
    StretchDIBits(hdc, (cw - w) / 2, (ch - h) / 2, w, h,
                  0, 0, (int)s_fb_width, (int)s_fb_height,
                  s_rgb, bi, DIB_RGB_COLORS, SRCCOPY);
}

/* The converted frame in s_rgb, on screen: D3D11 when it is up (which waits
 * for vsync, so returns 1 when the loop is already paced), else GDI. */
static int fb_show(HWND hwnd, HDC hdc, const BITMAPINFO *bi)
{
    if (s_bw_external)
        return 0;           /* the GPU back end presents */
    if (gpu_present_active())
        return gpu_present_frame(s_rgb);
    gdi_blit(hwnd, hdc, bi);
    if (bw_dwm_sync()) {
        Sleep(1);           /* DwmFlush paced it; this only stops a spin if DWM is off */
        return 1;
    }
    return 0;
}

static DWORD WINAPI fb_thread(LPVOID unused)
{
    HWND hwnd;
    HDC hdc;
    BITMAPINFO bi;
    RECT r;
    int fb_pinned;

    (void)unused;

    {
        /* Blood Wake carried patch (0.2.5 item 14): the exe's own icon
         * (resource 1, src/bloodwake.rc) on the title bar and the taskbar
         * button of the standalone frame window, not Windows' generic one. */
        WNDCLASSEXA wc;
        memset(&wc, 0, sizeof(wc));
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = fb_wndproc;
        wc.hInstance     = GetModuleHandleA(NULL);
        wc.hCursor       = LoadCursorA(NULL, IDC_ARROW);
        wc.lpszClassName = "XboxRecompFramebuffer";
        wc.hIcon         = LoadIconA(wc.hInstance, MAKEINTRESOURCEA(1));
        wc.hIconSm       = (HICON)LoadImageA(wc.hInstance, MAKEINTRESOURCEA(1), IMAGE_ICON,
                                             GetSystemMetrics(SM_CXSMICON),
                                             GetSystemMetrics(SM_CYSMICON), 0);
        RegisterClassExA(&wc);
    }
    {
        /* RECOMP_WINDOW_SCALE (1-8, default 2, as in the SDL window): the
         * client area is the frame times this, made smaller until the window
         * fits the desktop work area, and centred on it. */
        const char *e = getenv("RECOMP_WINDOW_SCALE");
        int scale = e ? atoi(e) : 2;
        RECT work;
        if (scale < 1 || scale > 8)
            scale = 2;
        if (!SystemParametersInfoA(SPI_GETWORKAREA, 0, &work, 0)) {
            work.left = work.top = 0; work.right = 1920; work.bottom = 1080;
        }
        for (;; scale--) {
            r.left = 0; r.top = 0;
            r.right = (LONG)s_fb_width * scale; r.bottom = (LONG)s_fb_height * scale;
            if (s_bw_aspect > 0.0f)               /* Blood Wake: widescreen */
                r.right = (LONG)((float)s_fb_height * s_bw_aspect + 0.5f) * scale;
            AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
            if (scale <= 1 || (r.right - r.left <= work.right - work.left
                               && r.bottom - r.top <= work.bottom - work.top))
                break;
        }
        s_bw_parent = bw_parent_window();
        if (s_bw_parent) {
            RECT pc;
            GetClientRect(s_bw_parent, &pc);
            hwnd = CreateWindowExA(0, "XboxRecompFramebuffer", "Blood Wake",
                                   WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                                   0, 0, pc.right - pc.left, pc.bottom - pc.top,
                                   s_bw_parent, NULL, GetModuleHandleA(NULL), NULL);
            if (hwnd)
                SetFocus(hwnd);
            else
                s_bw_parent = NULL;
        } else
        hwnd = CreateWindowExA(0, "XboxRecompFramebuffer", "Xbox Recomp - Framebuffer",
                               WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                               work.left + ((work.right - work.left) - (r.right - r.left)) / 2,
                               work.top + ((work.bottom - work.top) - (r.bottom - r.top)) / 2,
                               r.right - r.left, r.bottom - r.top,
                               NULL, NULL, GetModuleHandleA(NULL), NULL);
    }
    if (!hwnd) {
        InterlockedExchange(&s_fb_running, 0);
        return 0;
    }
    hdc = GetDC(hwnd);
    s_bw_hwnd = hwnd;
    {
        const char *fs = getenv("RECOMP_FULLSCREEN");
        if (fs && *fs && *fs != '0' && !s_bw_parent)    /* the host's, in a host */
            fb_toggle_fullscreen(hwnd);
    }

    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth       = (LONG)s_fb_width;
    bi.bmiHeader.biHeight      = -(LONG)s_fb_height;   /* top-down */
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    s_rgb = (uint32_t *)calloc((size_t)s_fb_width * s_fb_height, 4);
    /* A window pinned to one address shows that memory and nothing else. */
    fb_pinned = getenv("RECOMP_FB_VA") != NULL;

    fprintf(stderr, "  [FBWIN] framebuffer window open (%ux%u)\n",
            s_fb_width, s_fb_height);
    if (gpu_present_enabled())
        gpu_present_init(hwnd, s_fb_width, s_fb_height);

    while (InterlockedCompareExchange(&s_fb_running, 1, 1)) {
        int paced = 0;
        fb_pump();
        bw_awake_tick(hwnd);            /* Blood Wake (0.3.3): the display stays awake in front */
        if ((s_bw_external || bw_fb_nocopy()) && bw_fb_copy_needed() == 0) {
            /* Blood Wake: the GPU back end presents; nothing to copy here */
        } else if (s_present_idx >= 0 && s_rgb) {
            /* A finished frame, published by the flip. Copied into s_rgb so
             * the dump path and GDI see one consistent image even if the
             * next flip lands mid-blit. */
            LONG idx = s_present_idx;
            if (s_present[idx])
                memcpy(s_rgb, s_present[idx],
                       (size_t)s_fb_width * s_fb_height * 4);
            if (!fb_pinned)
                fb_overlay();
            paced = fb_show(hwnd, hdc, &bi);
        } else if (s_fb_va && s_fb_pitch && s_rgb) {
            /* No flip yet, or pinned with RECOMP_FB_VA: read guest memory as
             * before, which is also what a title that never flips needs. */
            const uint8_t *src =
                (const uint8_t *)((uintptr_t)s_fb_va + xbox_GetMemoryOffset());
            fb_convert(src, s_fb_pitch / s_fb_width);
            if (!fb_pinned)
                fb_overlay();
            paced = fb_show(hwnd, hdc, &bi);
        }
        {
            /* One dump a few seconds in, so the title has had time to render
             * something rather than catching the first blank frame.
             *
             * RECOMP_FB_WINDOW_DUMP_EVERY=<frames> dumps repeatedly instead.
             * This window follows the address AvSetDisplayMode gave, which is
             * what the CRTC scans and therefore what a person sees; the
             * pushbuffer executor's own dump follows its draw surface. With
             * double buffering those are different buffers, and measuring
             * progress from the executor's dump reports a blank screen while
             * the window is showing the title's logo. Ask the window. */
            const char *dump = getenv("RECOMP_FB_DUMP");
            const char *every = getenv("RECOMP_FB_WINDOW_DUMP_EVERY");
            static int frames;
            int period = every ? atoi(every) : 0;
            frames++;
            if (dump && period > 0) {
                if (frames % period == 0)
                    xbox_FramebufferDumpBmp(dump);
            } else if (dump && frames == 600) {
                xbox_FramebufferDumpBmp(dump);
            }
        }
        {
            static DWORD t0;
            static LONG f0;
            DWORD now = GetTickCount();
            if (now - t0 >= 1000) {
                LONG f = s_flips;
                wchar_t tb[128];
                /* Blood Wake carried patch (0.2.5 item 96): the frame stats in
                 * the title are for tests (BW_UNATTENDED, or BW_TITLE_STATS=1;
                 * BW_TITLE_STATS=0 hides them there too). In play the host's
                 * window keeps the title the host gave it ("Blood Wake",
                 * "Blood Wake (Remastered)"), and a window of its own shows
                 * the game's name, as before the stats reached the host. */
                static int stats = -1;
                static wchar_t shown[48];
                if (stats < 0) {
                    const char *e = getenv("BW_TITLE_STATS");
                    stats = (e && *e) ? e[0] == '1' : getenv("BW_UNATTENDED") != NULL;
                }
                if (stats) {
                    _snwprintf(tb, 127, L"%ls | FPS: %.1f | draws: %ld", s_title,
                               t0 ? (f - f0) * 1000.0 / (now - t0) : 0.0,
                               (long)s_frame_draws);
                    tb[127] = 0;
                    if (s_bw_parent)
                        SendMessageTimeoutW(s_bw_parent, WM_SETTEXT, 0, (LPARAM)tb,
                                            SMTO_ABORTIFHUNG | SMTO_BLOCK, 100, NULL);
                    else
                        SetWindowTextW(hwnd, tb);
                } else if (!s_bw_parent && wcscmp(shown, s_title)) {
                    wcsncpy(shown, s_title, 47);
                    shown[47] = 0;
                    SetWindowTextW(hwnd, shown);
                }
                t0 = now; f0 = f;
            }
        }
        if (!paced)
            fb_wait(16);                /* Blood Wake (item 75): wakes for input */
    }

    bw_awake_set(0, "the window closed");      /* Blood Wake (0.3.3) */
    gpu_present_shutdown();
    s_bw_hwnd = NULL;
    ReleaseDC(hwnd, hdc);
    DestroyWindow(hwnd);
    free(s_rgb);
    s_rgb = NULL;
    free(s_pvideo_source);
    s_pvideo_source = NULL;
    return 0;
}

void xbox_FramebufferWindowStart(void)
{
    HANDLE th;

    if (!getenv("RECOMP_FB_WINDOW"))
        return;
    if (InterlockedCompareExchange(&s_fb_running, 1, 0) != 0)
        return;
    th = CreateThread(NULL, 0, fb_thread, NULL, 0, NULL);
    if (th)
        CloseHandle(th);
    else
        InterlockedExchange(&s_fb_running, 0);
}

#endif /* _WIN32: POSIX hosts get the SDL2 window in fb_present_sdl.c */
