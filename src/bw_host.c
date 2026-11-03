/* Blood Wake: the play launcher's host window.
 *
 * Blood Wake reboots itself between the title, the attract demo and a match
 * (it relaunches its own default.xbe with launch data; see sub_000DB8B9 in
 * recomp_manual.c). The port ends the process there and the next boot is a
 * new one, so each reboot used to open a new window.
 *
 * With BW_HOST=1 (tools/play.py sets it), this process runs no game. It opens
 * one "Blood Wake" window and runs each boot as a child process of the same
 * exe, with BW_PARENT_HWND naming the window. The toolkit's frame window
 * (carried fb_present.c) is then a child filling it. Between boots the window
 * is black, as the console's screen is during a reboot.
 *
 *   - one session at a time: the named mutex BloodWakeRecomp.Running is held
 *     for the session (docs/025/CONTRACTS.md, 1); a second start brings the
 *     running window to the front and exits with code 0 (0.2.5 item 56).
 *     BW_TEST_MUTEX=<word> (tests) names the mutex
 *     BloodWakeRecomp.Running.<word>, so test sessions keep apart;
 *   - follows the game's reboots: bw_relaunch.bin -> bw_launch_next.bin ->
 *     BW_LAUNCH_DATA_FILE for the next boot, as tools/play.py did. A move
 *     that fails (an antivirus holding the file) is retried for a second,
 *     then the next boot is a cold boot, with a log line (item 57);
 *   - Alt+Enter / F11: borderless fullscreen (the game's window forwards them);
 *   - closing the window closes the game (WM_CLOSE to its frame window,
 *     which ends that process through the toolkit's exit hook);
 *   - the boots' output goes to play.log (BW_HOST_LOG), one header per boot;
 *   - if the game stops without asking to reboot, the window says so, with
 *     the log's full path and where to report it (item 59), and stays until
 *     closed (BW_HOST_LINGER=0: exit at once, for scripts). Exit codes 20 and
 *     21 (no usable graphics device, device lost) already showed the game's
 *     own message, so the session just ends, with a log line;
 *   - a cold boot that crashes before the title (the logo movies, item 51's
 *     rare Bink worker crash) is started once more, with a log line;
 *   - start_state.ini beside the settings file records each start, and
 *     whether it reached the title, for the launcher's safe start
 *     (CONTRACTS.md, 4; item 60). BW_TEST_FAILSTART=1 ends each boot before
 *     the title (test switch);
 *   - bw_copy.txt beside the exe (an unverified or differing copy, from
 *     Setup) is logged at every start (CONTRACTS.md, 9);
 *   - BW_HOST_BOOTS=<n> caps the boots, for test runs only: unset, a session
 *     has no cap (item 55; a player idling on the title reboots into the
 *     attract demo and back every minute or so).
 *
 * Paths are wide throughout (item 53): the install folder and the settings
 * folder sit under the user's profile, whose name can hold any letter.
 *
 * The child inherits everything else in the environment. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "bw_host.h"
#include "bw_edition.h"
#include "bw_settings.h"
#include "bw_widescreen.h"
#include "toolkit_patches/input/bw_input_ext.h"   /* xbox_InputRumbleAllOff (C3, item 74) */

#define BW_HOST_FULLSCREEN (WM_APP + 0x42)   /* from the carried fb_present.c */
#define BW_HOST_RAISE      (WM_APP + 0x43)   /* a second start asks the session to come forward */
#define BW_FRAME_CLASS     "XboxRecompFramebuffer"
#define BW_HOST_CLASS      L"BloodWakeHost"
#define BW_HOST_MUTEX      L"BloodWakeRecomp.Running"
#define BW_ISSUES_URL      L"https://github.com/minimalice92/bloodwake-recomp/issues"
#define BW_EXIT_NO_GPU     20                 /* src/gpu: no usable Direct3D 11 device (item 26) */
#define BW_EXIT_GPU_LOST   21                 /* src/gpu: the device was lost (item 28) */

static HWND s_host;
static HANDLE s_child;
static int s_quit;
static DWORD s_quit_at;
static wchar_t s_msg[1024];

static HWND child_window(void)
{
    return s_host ? FindWindowExA(s_host, NULL, BW_FRAME_CLASS, NULL) : NULL;
}

static void fit_child(void)
{
    HWND c = child_window();
    RECT r;
    if (!c)
        return;
    GetClientRect(s_host, &r);
    SetWindowPos(c, NULL, 0, 0, r.right - r.left, r.bottom - r.top,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
}

/* Borderless fullscreen on the window's monitor, and back (as the toolkit's
 * own window does it). */
static int s_full;                  /* borderless fullscreen (toggle_fullscreen) */

static void toggle_fullscreen(void)
{
    static RECT saved;
    static LONG_PTR saved_style;
    HWND h = s_host;
    if (!s_full) {
        MONITORINFO mi;
        mi.cbSize = sizeof mi;
        if (!GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi))
            return;
        GetWindowRect(h, &saved);
        saved_style = GetWindowLongPtrW(h, GWL_STYLE);
        SetWindowLongPtrW(h, GWL_STYLE, (saved_style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        SetWindowPos(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        s_full = 1;
    } else {
        SetWindowLongPtrW(h, GWL_STYLE, saved_style);
        SetWindowPos(h, NULL, saved.left, saved.top, saved.right - saved.left,
                     saved.bottom - saved.top, SWP_FRAMECHANGED | SWP_NOZORDER | SWP_SHOWWINDOW);
        s_full = 0;
    }
}

static void request_quit(void)
{
    HWND c = child_window();
    if (!s_quit) {
        s_quit = 1;
        s_quit_at = GetTickCount();
    }
    if (c)
        PostMessageA(c, WM_CLOSE, 0, 0);
    else if (s_child)
        TerminateProcess(s_child, 0);
}

static void raise_window(HWND h)
{
    if (IsIconic(h))
        ShowWindow(h, SW_RESTORE);
    SetForegroundWindow(h);
}

static LRESULT CALLBACK host_wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case BW_HOST_FULLSCREEN:
        toggle_fullscreen();
        return 0;
    case BW_HOST_RAISE:
        raise_window(h);
        return 0;
    case WM_SYSKEYDOWN:
        if (w == VK_RETURN && (l & (1 << 29))) {
            if (!(l & (1 << 30)))
                toggle_fullscreen();
            return 0;
        }
        break;
    case WM_KEYDOWN:
        if (w == VK_F11 && !(l & (1 << 30))) {
            toggle_fullscreen();
            return 0;
        }
        break;
    case WM_SIZE:
        fit_child();
        return 0;
    case 0x02E0:                        /* WM_DPICHANGED (0.2.5 item 33) */
        /* moved to a monitor with other scaling: take Windows' suggested
         * rect, which keeps the window the same size to the eye (the exe is
         * per-monitor DPI aware, so sizes are the panel's own pixels) */
        if (!s_full) {
            const RECT *n = (const RECT *)l;
            SetWindowPos(h, NULL, n->left, n->top, n->right - n->left, n->bottom - n->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;
    case WM_SETFOCUS: {
        HWND c = child_window();
        if (c)
            SetFocus(c);
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT r;
        GetClientRect(h, &r);
        FillRect(dc, &r, (HBRUSH)GetStockObject(BLACK_BRUSH));
        if (s_msg[0]) {
            /* Centred both ways: DT_VCENTER works only on a single line, so
             * the text's height is measured first. */
            RECT t;
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(220, 220, 220));
            InflateRect(&r, -24, -24);
            t = r;
            DrawTextW(dc, s_msg, -1, &t, DT_CENTER | DT_WORDBREAK | DT_CALCRECT);
            if (t.bottom - t.top < r.bottom - r.top)
                r.top += ((r.bottom - r.top) - (t.bottom - t.top)) / 2;
            DrawTextW(dc, s_msg, -1, &r, DT_CENTER | DT_WORDBREAK);
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_CLOSE:
        if (s_child)
            request_quit();
        else
            s_quit = 1, DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        s_host = NULL;
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static int env_int(const char *name, int dflt)
{
    const char *e = getenv(name);
    return e && *e ? atoi(e) : dflt;
}

int bw_host_wanted(void)
{
    const char *e = getenv("BW_HOST");
    return e && e[0] == '1';
}

/* 0.2.5 item 33: a monitor's DPI (96 = 100%), or 96 when it can't be read.
 * Looked up, so the exe still starts where shcore has no GetDpiForMonitor. */
static UINT monitor_dpi(HMONITOR m)
{
    typedef HRESULT (WINAPI *DpiFn)(HMONITOR, int, UINT *, UINT *);
    HMODULE sh = LoadLibraryA("shcore.dll");
    DpiFn f = sh ? (DpiFn)(void *)GetProcAddress(sh, "GetDpiForMonitor") : NULL;
    UINT x = 96, y = 96;
    if (!f || FAILED(f(m, 0 /* MDT_EFFECTIVE_DPI */, &x, &y)) || x < 48 || x > 960)
        x = 96;
    return x;
}

static HWND make_window(const wchar_t *title, float aspect)
{
    WNDCLASSEXW wc;
    RECT r, work;
    int scale = env_int("RECOMP_WINDOW_SCALE", 2);
    POINT origin = { 0, 0 };
    UINT dpi = monitor_dpi(MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY));   /* item 33 */
    int fw = aspect > 4.0f / 3.0f + 0.001f ? (int)(480.0f * aspect + 0.5f) : 640;

    memset(&wc, 0, sizeof wc);
    wc.cbSize        = sizeof wc;
    wc.lpfnWndProc   = host_wndproc;
    wc.hInstance     = GetModuleHandleW(NULL);
    wc.hCursor       = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = BW_HOST_CLASS;
    /* The exe's own icon (resource 1, src/bloodwake.rc; 0.2.5 item 14) on the
     * title bar and the taskbar button, not Windows' generic one. */
    wc.hIcon         = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));
    wc.hIconSm       = (HICON)LoadImageW(wc.hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                         GetSystemMetrics(SM_CXSMICON),
                                         GetSystemMetrics(SM_CYSMICON), 0);
    RegisterClassExW(&wc);
    if (scale < 1 || scale > 8)
        scale = 2;
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) {
        work.left = work.top = 0;
        work.right = 1920;
        work.bottom = 1080;
    }
    /* The game's frame is 640x480 (Remastered widescreen: 480 high at the
     * picture's aspect); the same sizing as the toolkit's window. Item 33:
     * in the monitor's scaling, so the window looks the size it always did
     * (the work area and the window are in the panel's own pixels now). */
    for (;; scale--) {
        r.left = r.top = 0;
        r.right = MulDiv(fw * scale, (int)dpi, 96);
        r.bottom = MulDiv(480 * scale, (int)dpi, 96);
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        if (scale <= 1 || (r.right - r.left <= work.right - work.left
                           && r.bottom - r.top <= work.bottom - work.top))
            break;
    }
    if (dpi != 96)
        fprintf(stderr, "[HOST] window %ldx%ld for %u%% display scaling\n",
                r.right - r.left, r.bottom - r.top, (dpi * 100 + 48) / 96);
    return CreateWindowExW(0, BW_HOST_CLASS, title,
                           WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_VISIBLE,
                           work.left + ((work.right - work.left) - (r.right - r.left)) / 2,
                           work.top + ((work.bottom - work.top) - (r.bottom - r.top)) / 2,
                           r.right - r.left, r.bottom - r.top,
                           NULL, NULL, GetModuleHandleW(NULL), NULL);
}

static void pump(void)
{
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

/* play.log takes UTF-8 lines (the boots print UTF-8 paths too). */
static HANDLE s_log = INVALID_HANDLE_VALUE;

static void log_line(const char *text)
{
    DWORD n;
    if (s_log != INVALID_HANDLE_VALUE)
        WriteFile(s_log, text, (DWORD)strlen(text), &n, NULL);
}

static void logf_host(const char *fmt, ...)
{
    char line[1600];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    line[sizeof line - 1] = 0;
    log_line(line);
}

static const char *u8(const wchar_t *w, char *out, int n)
{
    if (!w || !WideCharToMultiByte(CP_UTF8, 0, w, -1, out, n, NULL, NULL))
        out[0] = 0;
    out[n - 1] = 0;
    return out;
}

/* ── One session at a time (item 56, contract 1) ──────────────────────── */

static HANDLE s_mutex;

/* 1 if this process now holds the session; 0 if another one does, after
 * bringing its window to the front. */
static int take_session(void)
{
    DWORD e;
    HWND w;
    wchar_t name[96], suffix[32];
    /* BW_TEST_MUTEX=<word>: tests only. The session's mutex becomes
     * BloodWakeRecomp.Running.<word>, so test sessions in different lanes or
     * scripts don't stop each other, or a player's. Unset: the contract's
     * name, which Setup and the launcher check. */
    DWORD sn = GetEnvironmentVariableW(L"BW_TEST_MUTEX", suffix, 32);
    if (sn && sn < 32)
        _snwprintf(name, 96, L"%ls.%ls", BW_HOST_MUTEX, suffix);
    else
        _snwprintf(name, 96, L"%ls", BW_HOST_MUTEX);
    name[95] = 0;
    s_mutex = CreateMutexW(NULL, FALSE, name);   /* not inherited by the boots */
    e = GetLastError();
    if (s_mutex && e != ERROR_ALREADY_EXISTS)
        return 1;
    if (!s_mutex && e != ERROR_ACCESS_DENIED) {
        /* Can't make a mutex at all (not expected): play on without one
         * rather than refuse to start. */
        fprintf(stderr, "[HOST] could not create the %ls mutex (Windows error %lu); "
                "starting without the one-session check\n", name, (unsigned long)e);
        return 1;
    }
    if (s_mutex)
        CloseHandle(s_mutex);
    s_mutex = NULL;
    w = FindWindowW(BW_HOST_CLASS, NULL);
    if (w) {
        DWORD pid = 0;
        GetWindowThreadProcessId(w, &pid);
        if (pid)
            AllowSetForegroundWindow(pid);
        raise_window(w);
        PostMessageW(w, BW_HOST_RAISE, 0, 0);
        printf("Blood Wake is already running: brought its window to the front.\n");
    } else
        printf("Blood Wake is already running (its window isn't up yet).\n");
    fflush(stdout);
    return 0;
}

/* ── start_state.ini (item 60, contract 4) ────────────────────────────── */

#define BW_STATE_MAX 8
static wchar_t s_state[MAX_PATH];          /* empty: no settings file in use, so none */
static char s_hist[BW_STATE_MAX + 1];      /* '0'/'1' per start, oldest first */
static char s_last[96];
static int s_reached;

static void state_write(void)
{
    wchar_t tmp[MAX_PATH + 8];
    char body[256];
    FILE *f;
    int i, n = 0;
    if (!s_state[0])
        return;
    n += _snprintf(body + n, sizeof body - n, "[start]\n; one digit per start, oldest first: "
                   "1 = reached the title (or a launched mission), 0 = didn't\nhistory = ");
    for (i = 0; s_hist[i]; i++)
        n += _snprintf(body + n, sizeof body - n, "%s%c", i ? "," : "", s_hist[i]);
    _snprintf(body + n, sizeof body - n, "\nlast = %s\n", s_last);
    body[sizeof body - 1] = 0;
    _snwprintf(tmp, MAX_PATH + 8, L"%ls.tmp", s_state);
    tmp[MAX_PATH + 7] = 0;
    f = _wfopen(tmp, L"wb");
    if (!f || fputs(body, f) < 0 || fclose(f) != 0
            || !MoveFileExW(tmp, s_state, MOVEFILE_REPLACE_EXISTING)) {
        char p[3 * MAX_PATH];
        logf_host("[HOST] start state: could not write %s (Windows error %lu)\r\n",
                  u8(s_state, p, sizeof p), (unsigned long)GetLastError());
        DeleteFileW(tmp);
    }
}

static void state_begin(const char *edition)
{
    const wchar_t *set = bw_settings_path_w();
    wchar_t *slash;
    FILE *f;
    char line[256], p[3 * MAX_PATH];
    SYSTEMTIME st;
    size_t n = 0;

    s_state[0] = 0;
    if (!set)
        return;                           /* BW_SETTINGS=none: tests, no file in use */
    _snwprintf(s_state, MAX_PATH, L"%ls", set);
    s_state[MAX_PATH - 1] = 0;
    slash = wcsrchr(s_state, L'\\');
    if (!slash)
        slash = wcsrchr(s_state, L'/');
    if (slash)
        slash[1] = 0;
    else
        s_state[0] = 0;                   /* a bare file name: the working folder */
    if (wcslen(s_state) + 16 >= MAX_PATH) {
        s_state[0] = 0;
        return;
    }
    if (s_state[0])
        CreateDirectoryW(s_state, NULL);  /* the launcher's folder; usually there already */
    wcscat(s_state, L"start_state.ini");

    s_hist[0] = 0;
    f = _wfopen(s_state, L"rb");
    if (f) {
        while (fgets(line, sizeof line, f)) {
            char *v = line, *c;
            while (*v == ' ' || *v == '\t')
                v++;
            if (_strnicmp(v, "history", 7))
                continue;
            c = strchr(v, '=');
            if (!c)
                continue;
            n = 0;
            for (c++; *c && *c != ';' && *c != '\r' && *c != '\n'; c++)
                if ((*c == '0' || *c == '1') && n < sizeof s_hist - 1)
                    s_hist[n++] = *c;
            s_hist[n] = 0;
        }
        fclose(f);
    }
    n = strlen(s_hist);
    if (n >= BW_STATE_MAX) {              /* keep the newest: drop the oldest */
        memmove(s_hist, s_hist + (n - (BW_STATE_MAX - 1)), BW_STATE_MAX - 1);
        n = BW_STATE_MAX - 1;
    }
    s_hist[n++] = '0';
    s_hist[n] = 0;
    GetLocalTime(&st);
    _snprintf(s_last, sizeof s_last, "%04u-%02u-%02u %02u:%02u:%02u edition=%s", st.wYear,
              st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, edition);
    s_last[sizeof s_last - 1] = 0;
    state_write();
    logf_host("[HOST] start state: %s, history %s (the last digit becomes 1 when the title "
              "shows)\r\n", u8(s_state, p, sizeof p), s_hist);
}

static void state_reached(int boot)
{
    size_t n = strlen(s_hist);
    if (s_reached)
        return;
    s_reached = boot;
    logf_host("\r\n=== [HOST] boot %d reached the title (or its mission) ===\r\n", boot);
    if (!s_state[0] || !n)
        return;
    s_hist[n - 1] = '1';
    state_write();
}

/* ── Small helpers ─────────────────────────────────────────────────────── */

/* contract 9: bw_copy.txt beside the exe holds one line for an unverified or
 * differing copy. */
static void log_copy_verdict(const wchar_t *exe)
{
    wchar_t p[MAX_PATH], *slash;
    char line[512];
    FILE *f;
    _snwprintf(p, MAX_PATH, L"%ls", exe);
    p[MAX_PATH - 1] = 0;
    slash = wcsrchr(p, L'\\');
    if (!slash || (size_t)(slash - p) + 14 >= MAX_PATH)
        return;
    wcscpy(slash + 1, L"bw_copy.txt");
    f = _wfopen(p, L"rb");
    if (!f)
        return;
    if (fgets(line, sizeof line, f)) {
        size_t n = strlen(line);
        char *s = line;
        if (n >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB
                && (unsigned char)s[2] == 0xBF)
            s += 3;                       /* a UTF-8 byte-order mark */
        while ((n = strlen(s)) > 0 && (s[n - 1] == '\r' || s[n - 1] == '\n'))
            s[n - 1] = 0;
        if (*s)
            logf_host("[HOST] copy: %s\r\n", s);
    }
    fclose(f);
}

/* item 57: the reboot request out of the next boot's way. A held file (an
 * antivirus scan) is tried again for about a second. */
static int move_request(const wchar_t *next, int boot)
{
    DWORD e = 0;
    int i;
    for (i = 0; i < 11; i++) {
        if (i)
            Sleep(100);
        if (MoveFileExW(L"bw_relaunch.bin", next, MOVEFILE_REPLACE_EXISTING)) {
            if (i)
                logf_host("[HOST] boot %d: moved the reboot request to bw_launch_next.bin "
                          "after %d tries (it was held: Windows error %lu)\r\n", boot, i + 1,
                          (unsigned long)e);
            return 1;
        }
        e = GetLastError();
    }
    logf_host("\r\n=== [HOST] boot %d: could not move bw_relaunch.bin to bw_launch_next.bin "
              "(Windows error %lu, 11 tries over 1 s); the next boot is a cold boot, so the "
              "game starts again at the title ===\r\n", boot, (unsigned long)e);
    return 0;
}

static int crash_code(DWORD code)
{
    return code >= 0xC0000000u;           /* an NTSTATUS error: access violation and the like */
}

/* ── The session ───────────────────────────────────────────────────────── */

int bw_host_main(void)
{
    wchar_t exe[MAX_PATH], cmd[MAX_PATH + 4], next[MAX_PATH], logname[MAX_PATH],
            logfull[MAX_PATH];
    char line[600], a8[3 * MAX_PATH];
    int boots = env_int("BW_HOST_BOOTS", 0), linger = env_int("BW_HOST_LINGER", 1);
    int failstart = env_int("BW_TEST_FAILSTART", 0);
    int boot, result = 0, start_none = 0, cold, retried = 0;
    DWORD n;
    SECURITY_ATTRIBUTES sa;
    HANDLE nul, title_ev;
    bw_edition_t ed;

    /* First, before the log or the state file is touched: is a session
     * already running? */
    if (!take_session())
        return 0;

    n = GetModuleFileNameW(NULL, exe, MAX_PATH);
    if (!n || n >= MAX_PATH) {
        fprintf(stderr, "[HOST] the exe's path is too long (Windows error %lu)\n",
                (unsigned long)GetLastError());
        return 1;
    }
    _snwprintf(cmd, MAX_PATH + 4, L"\"%ls\"", exe);
    cmd[MAX_PATH + 3] = 0;
    GetFullPathNameW(L"bw_launch_next.bin", MAX_PATH, next, NULL);
    n = GetEnvironmentVariableW(L"BW_HOST_LOG", logname, MAX_PATH);
    if (!n || n >= MAX_PATH)
        wcscpy(logname, L"play.log");
    if (!GetFullPathNameW(logname, MAX_PATH, logfull, NULL))
        wcscpy(logfull, logname);
    cold = GetEnvironmentVariableW(L"BW_LAUNCH_DATA_FILE", NULL, 0) == 0;

    /* The edition (D-015): every boot inherits BW_EDITION from this
     * process's environment. Check it here once, so a wrong name says so in
     * the window instead of failing boot after boot. Original keeps the
     * plain title. */
    {
        wchar_t title[64];
        char err[256];
        int bad = bw_edition_init(err, sizeof err) != 0;   /* BW_EDITION and BW_FIX */
        ed = bw_edition();
        if (!bad)
            bw_ws_init(0, 0);           /* the window's shape (Remastered widescreen) */
        if (bad || ed == BW_ED_ORIGINAL)
            _snwprintf(title, 64, L"Blood Wake");
        else
            _snwprintf(title, 64, L"Blood Wake (%hs)", bw_edition_title(ed));
        title[63] = 0;
        s_host = make_window(title, bw_ws_aspect());
        {
            /* RECOMP_FULLSCREEN=1 (the launcher's "Fullscreen"): start in
             * borderless fullscreen, as the toolkit's own window does. */
            const char *fs = getenv("RECOMP_FULLSCREEN");
            if (s_host && fs && *fs && *fs != '0')
                toggle_fullscreen();
        }
        if (s_host && bad) {
            _snwprintf(s_msg, 1024, L"%hs\n\n"
                       L"Editions: original or remastered (play.cmd --edition remastered).\n"
                       L"Close this window to finish.", err);
            s_msg[1023] = 0;
            start_none = 1;                  /* start no boot; the window says why */
        }
    }
    if (!s_host) {
        fprintf(stderr, "[HOST] could not open the window\n");
        return 1;
    }
    {
        char hv[32];
        _snprintf(hv, sizeof hv, "%llu", (unsigned long long)(uintptr_t)s_host);
        hv[sizeof hv - 1] = 0;
        SetEnvironmentVariableA("BW_HOST", NULL);
        SetEnvironmentVariableA("BW_PARENT_HWND", hv);
    }

    memset(&sa, 0, sizeof sa);
    sa.nLength = sizeof sa;
    sa.bInheritHandle = TRUE;
    s_log = CreateFileW(logname, GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, NULL);
    nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                      OPEN_EXISTING, 0, NULL);
    /* item 60: each boot signals this once it reaches the title (main.c) */
    title_ev = CreateEventW(&sa, TRUE, FALSE, NULL);
    if (title_ev) {
        char hv[32];
        _snprintf(hv, sizeof hv, "%llu", (unsigned long long)(uintptr_t)title_ev);
        hv[sizeof hv - 1] = 0;
        SetEnvironmentVariableA("BW_HOST_TITLE_EVENT", hv);
    }
    log_copy_verdict(exe);
    if (!start_none)
        state_begin(bw_edition_name(ed));

    for (boot = 1; !start_none && !s_quit && (boots <= 0 || boot <= boots); boot++) {
        STARTUPINFOW si;
        PROCESS_INFORMATION pi;
        DWORD code = 0;
        int this_cold = cold;

        DeleteFileW(L"bw_relaunch.bin");
        _snprintf(line, sizeof line, "\r\n=== [HOST] boot %d%s ===\r\n", boot,
                  retried == 1 ? " (starting the crashed boot once more)"
                  : boot > 1 ? " (the game's own reboot)" : "");
        line[sizeof line - 1] = 0;
        log_line(line);
        printf("Boot %d%s ...\n", boot, boot > 1 ? " (the game's own reboot)" : "");
        fflush(stdout);
        if (retried == 1)
            retried = 2;

        memset(&si, 0, sizeof si);
        si.cb = sizeof si;
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = nul;
        si.hStdOutput = s_log;
        si.hStdError = s_log;
        if (!CreateProcessW(exe, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
            DWORD e = GetLastError();
            _snwprintf(s_msg, 1024, L"Could not start the game (Windows error %lu).\n\n%ls",
                       (unsigned long)e, exe);
            s_msg[1023] = 0;
            logf_host("\r\n=== [HOST] could not start %s (Windows error %lu) ===\r\n",
                      u8(exe, a8, sizeof a8), (unsigned long)e);
            result = 1;
            break;
        }
        CloseHandle(pi.hThread);
        s_child = pi.hProcess;
        if (failstart) {
            /* BW_TEST_FAILSTART=1: a start that never reaches the title
             * (contract 4's test switch, for the launcher's safe start). */
            DWORD t0 = GetTickCount();
            while (WaitForSingleObject(s_child, 0) == WAIT_TIMEOUT && GetTickCount() - t0 < 1000) {
                pump();
                Sleep(20);
            }
            TerminateProcess(s_child, 1);
            logf_host("\r\n=== [HOST] BW_TEST_FAILSTART=1: ended boot %d before the title "
                      "(test switch) ===\r\n", boot);
        }
        for (;;) {
            HANDLE w[2];
            DWORD r, nw = 1;
            w[0] = s_child;
            if (title_ev && !s_reached)
                w[nw++] = title_ev;
            r = MsgWaitForMultipleObjects(nw, w, FALSE, s_quit ? 250 : INFINITE, QS_ALLINPUT);
            if (r == WAIT_OBJECT_0)
                break;
            if (nw == 2 && r == WAIT_OBJECT_0 + 1) {
                state_reached(boot);
                continue;
            }
            pump();
            if (s_quit && GetTickCount() - s_quit_at > 5000)
                TerminateProcess(s_child, 0);      /* it didn't close itself */
        }
        GetExitCodeProcess(s_child, &code);
        CloseHandle(s_child);
        s_child = NULL;
        /* A pad keeps its last rumble until someone sets it, so a boot that
         * ends mid-rumble (a crash above all) could leave it buzzing. */
        xbox_InputRumbleAllOff("boot ended");
        logf_host("[HOST] boot %d ended (exit code 0x%08lX): rumble off on every controller\r\n",
                  boot, (unsigned long)code);
        if (title_ev && !s_reached && WaitForSingleObject(title_ev, 0) == WAIT_OBJECT_0)
            state_reached(boot);                   /* signalled just before it ended */
        if (s_quit)
            break;
        if (GetFileAttributesW(L"bw_relaunch.bin") == INVALID_FILE_ATTRIBUTES) {
            if (this_cold && !s_reached && crash_code(code) && !retried) {
                /* item 51: the rare crash in a movie worker during the logo
                 * movies (docs/day1/D3.md) is a port bug and doesn't repeat:
                 * a cold boot that crashed before the title is started once
                 * more. */
                logf_host("\r\n=== [HOST] boot %d crashed during start-up (exit code 0x%08lX), "
                          "before the title; starting it once more ===\r\n", boot,
                          (unsigned long)code);
                printf("The game crashed while starting (code 0x%08lX); starting it once more.\n",
                       (unsigned long)code);
                retried = 1;
                continue;
            }
            if (code == BW_EXIT_NO_GPU || code == BW_EXIT_GPU_LOST) {
                /* The game already said why in its own message box (src/gpu,
                 * items 26 and 28): no second message, the session ends. */
                logf_host("\r\n=== [HOST] boot %d ended: graphics device %s (exit %lu); the game "
                          "showed its message ===\r\n", boot,
                          code == BW_EXIT_NO_GPU ? "unsupported" : "lost", (unsigned long)code);
                printf("The game ended: graphics device %s (exit code %lu).\n",
                       code == BW_EXIT_NO_GPU ? "unsupported" : "lost", (unsigned long)code);
                result = (int)code;
                s_msg[0] = 0;
                break;
            }
            /* The game stopped without asking to reboot: a console never
             * does that, so it is a crash or a failure. */
            _snwprintf(s_msg, 1024,
                       L"The game stopped (exit code 0x%08lX) without asking to reboot.\n\n"
                       L"Its log is:\n%ls\n\n"
                       L"To report the problem, open an issue at\n%ls\n"
                       L"and attach that file.\n\n"
                       L"Close this window to finish.",
                       (unsigned long)code, logfull, BW_ISSUES_URL);
            s_msg[1023] = 0;
            logf_host("\r\n=== [HOST] boot %d stopped, exit code 0x%08lX ===\r\n",
                      boot, (unsigned long)code);
            printf("The game exited (code 0x%08lX). Its log: %s\n", (unsigned long)code,
                   u8(logfull, a8, sizeof a8));
            result = code ? (int)code : 0;
            break;
        }
        /* Keep the request out of the next boot's way: it writes a new one. */
        if (move_request(next, boot)) {
            SetEnvironmentVariableW(L"BW_LAUNCH_DATA_FILE", next);
            cold = 0;
        } else {
            SetEnvironmentVariableW(L"BW_LAUNCH_DATA_FILE", NULL);
            cold = 1;
        }
    }
    if (!start_none && !s_quit && boots > 0 && boot > boots && !s_msg[0]) {
        _snwprintf(s_msg, 1024, L"Stopped after %d boots (BW_HOST_BOOTS).", boots);
        logf_host("\r\n=== [HOST] stopped after %d boots (BW_HOST_BOOTS, a test cap) ===\r\n",
                  boots);
    }

    if (s_log != INVALID_HANDLE_VALUE)
        CloseHandle(s_log);
    s_log = INVALID_HANDLE_VALUE;
    if (nul != INVALID_HANDLE_VALUE)
        CloseHandle(nul);
    if (title_ev)
        CloseHandle(title_ev);
    if (s_host && !s_quit && s_msg[0] && linger) {
        SetWindowTextW(s_host, L"Blood Wake - stopped");
        InvalidateRect(s_host, NULL, TRUE);
        while (s_host && !s_quit) {
            MSG msg;
            if (GetMessageW(&msg, NULL, 0, 0) <= 0)
                break;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (s_host)
        DestroyWindow(s_host);
    if (s_mutex)
        CloseHandle(s_mutex);
    return result;
}
