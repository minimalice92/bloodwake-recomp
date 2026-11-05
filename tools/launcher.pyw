"""Blood Wake launcher: the settings window and a big Play button.

Open it with "Blood Wake.cmd" (or play.cmd with no options) in the repo folder.

  - Edition, Display, Graphics, Audio, Fixes and Controls tabs (and any
    others the game lists), built from the game's own table of settings
    (bloodwake.exe --settings-schema; src/bw_settings.c), so this window and
    the game always agree on what each choice means. Hover over anything for
    a short explanation.
  - The Controls tab also remaps the keyboard and sets its sensitivity
    ([keys]); the Cheats tab turns the game's own cheats on ([cheats]); the
    Advanced tab takes raw switches.
  - Remastered's presets (the Edition tab: Best Quality, the default; Balanced
    and Original; Best Performance) set the Display and Graphics tabs in one
    go. Change any of those settings afterwards and the preset shows Custom.
  - Your choices are saved to %LOCALAPPDATA%/BloodWake/settings.ini, which every
    boot of the game reads, and are remembered for next time.
  - Play saves and starts the game (tools/play.py: one window for the whole
    session, controllers detected). The launcher hides while you play and
    comes back when the game closes.
  - After a change to a setting the game's shaders depend on (anti-aliasing
    on or off, sharp text, smooth HUD edges, texture restoration), Play offers
    to prepare graphics first (tools/bw_warmup.py, as Setup runs it), so the
    new shaders don't compile as small hitches during play. The Advanced tab
    has the same action as a button.
  - If the last two starts didn't reach the title (the game's own record,
    start_state.ini beside the settings), Play offers safe mode: Original, in
    a window, nothing extra, from its own settings_safe.ini. The Advanced tab
    has Start in safe mode, and Make a bug report: one zip on the desktop with
    the game's log, the settings and a short summary of the PC.
  - Play does nothing but bring the game's window to the front when Blood
    Wake is already running.

Stdlib only (tkinter). Nothing here changes the game: it writes the settings
file and starts tools/play.py (or tools/bw_warmup.py).
"""
import configparser
import json
import os
import queue
import subprocess
import sys
import threading
import time
import tkinter as tk
import tkinter.font as tkfont
from tkinter import messagebox, ttk

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(REPO, "build", "Release", "bloodwake.exe")
# Test switch: BW_LAUNCHER_EXE=<a copy of the exe> for the settings table and
# the warm-up (tools/launcher_check.py), so a rebuild isn't blocked. Play
# always starts the build (tools/play.py).
EXE = os.environ.get("BW_LAUNCHER_EXE") or EXE
PLAY = os.path.join(REPO, "tools", "play.py")
WARMUP = os.path.join(REPO, "tools", "bw_warmup.py")
GAME_DIR = os.path.join(REPO, "game", "disc")
CREATE_NO_WINDOW = 0x08000000
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
try:
    import bw_warmup                    # the warm-up's own record of what it prepared
except ImportError:                     # (a launcher without it just never offers)
    bw_warmup = None
try:
    import bw_hdr                       # Windows HDR on a screen (Automatic HDR, items 97, 100)
except (ImportError, OSError):          # (without it, Automatic HDR never switches)
    bw_hdr = None

EDITION_TEXT = {
    "original": "Exactly as it shipped in 2001: the console's look, 4:3 at 30 frames a second, "
                "quirks and bugs included.",
    "remastered": "The same game on a modern PC: bugs fixed, widescreen, a sharper picture, a "
                  "smoother frame rate and cleaner sound, with the original art.",
}
APPLIES = {"all": "every edition", "remastered": "Remastered"}
LATER = "Remastered"            # the editions that start from a copy of Original's saves
SCALE = 1.0     # display scaling (125% = 1.25), set in main(): sizes below are at 100%

# ── The look (item 20): docs/025/STYLE.md's palette, by its names ──────────
NIGHT_900, NIGHT_800, NIGHT_700 = "#050a13", "#0a1828", "#0f2740"
SEA_800, SEA_700, SEA_600, SEA_500 = "#0b2034", "#0e2e4b", "#11406a", "#165684"
HORIZON, SWELL_LIGHT, RECOMP_BLUE = "#3d78a8", "#7cc4ff", "#8ccfff"
TITLE_WHITE, TITLE_RED = "#f4f7fb", "#ff4a5f"
WAKE_BRIGHT, WAKE_RED, WAKE_DEEP, MOON = "#ff3b54", "#d7263d", "#b3122e", "#dcecff"
# The colour roles, for Windows' dark and light app modes (STYLE.md, "Colour roles").
ROLES = {
    "dark": {"bg": NIGHT_800, "panel": NIGHT_700, "input": SEA_700, "border": HORIZON,
             "text": TITLE_WHITE, "quiet": RECOMP_BLUE, "focus": SWELL_LIGHT,
             "off": "#6f8aa3", "ok": "#3fcf8e", "warn": "#f5b041", "bad": TITLE_RED},
    "light": {"bg": TITLE_WHITE, "panel": "#ffffff", "input": "#ffffff", "border": HORIZON,
              "text": NIGHT_800, "quiet": SEA_500, "focus": SEA_500,
              "off": "#7d8a99", "ok": "#1b7f4e", "warn": "#9a5b00", "bad": "#c0182f"},
}
C = dict(ROLES["light"])        # the roles in use, set by apply_theme()


def windows_dark():
    """Whether Windows' apps are in dark mode. Test switch: BW_LAUNCHER_THEME=dark|light."""
    forced = os.environ.get("BW_LAUNCHER_THEME", "").lower()
    if forced in ("dark", "light"):
        return forced == "dark"
    try:
        import winreg
        k = winreg.OpenKey(winreg.HKEY_CURRENT_USER,
                           r"Software\Microsoft\Windows\CurrentVersion\Themes\Personalize")
        return winreg.QueryValueEx(k, "AppsUseLightTheme")[0] == 0
    except (OSError, ImportError):
        return False


def blend(a, b, t):
    """Colour a mixed towards b by t (0..1): Tk has no transparency."""
    a, b = [int(a[i:i + 2], 16) for i in (1, 3, 5)], [int(b[i:i + 2], 16) for i in (1, 3, 5)]
    return "#%02x%02x%02x" % tuple(int(round(x + (y - x) * t)) for x, y in zip(a, b))


def apply_theme(root):
    """Set the palette's roles for Windows' mode on every ttk widget. Built on
    ttk's "clam" theme, the one whose colours can all be set."""
    mode = "dark" if windows_dark() else "light"
    C.clear()
    C.update(ROLES[mode])
    C["mode"] = mode
    st = ttk.Style(root)
    st.theme_use("clam")
    base = ("Segoe UI", 9)
    # Not "*Font" in the option database: ttk widgets read it too, and it
    # would win over their styles (the headings' Semibold).
    root.option_add("*TCombobox*Listbox.font", base)
    st.configure(".", background=C["bg"], foreground=C["text"], fieldbackground=C["input"],
                 bordercolor=C["border"], lightcolor=C["panel"], darkcolor=C["panel"],
                 troughcolor=C["bg"], focuscolor=C["focus"], selectbackground=SEA_500,
                 selectforeground=TITLE_WHITE, insertcolor=C["text"], font=base)
    # clam turns anything under the mouse a light grey ("active"): the palette instead.
    st.map(".", foreground=[("disabled", C["off"])],
           background=[("disabled", C["panel"]), ("active", C["input"])])
    # Inside the tabs and the dialogs (the default) the panel colour; the
    # window around them ("Bg.") the background.
    st.configure("TFrame", background=C["panel"])
    st.configure("Bg.TFrame", background=C["bg"])
    st.configure("TLabel", background=C["panel"], foreground=C["text"])
    st.configure("Bg.TLabel", background=C["bg"], foreground=C["text"])
    st.configure("Quiet.TLabel", background=C["panel"], foreground=C["quiet"])
    st.configure("Head.TLabel", background=C["panel"], font=("Segoe UI Semibold", 11))
    st.configure("Status.TLabel", background=C["bg"], foreground=C["quiet"])
    st.configure("Warn.TLabel", background=C["panel"], foreground=C["warn"],
                 font=("Segoe UI Semibold", 10))   # a status: with a word and a mark, not colour alone
    st.configure("TNotebook", background=C["bg"], bordercolor=C["border"], tabmargins=(0, 0, 0, 0))
    st.configure("TNotebook.Tab", background=C["bg"], foreground=C["quiet"], bordercolor=C["border"],
                 padding=(px(10), px(4)))
    st.map("TNotebook.Tab", background=[("selected", C["panel"])],
           foreground=[("selected", C["text"])], lightcolor=[("selected", C["panel"])])
    st.configure("TButton", background=C["panel"], foreground=C["text"], bordercolor=C["border"],
                 padding=(px(10), px(3)), focusthickness=1)
    st.map("TButton", background=[("pressed", C["input"]), ("active", C["input"])],
           foreground=[("disabled", C["off"])])
    st.configure("Key.TButton", padding=(px(6), px(1)))      # the Controls tab's key boxes
    # Wake red is for the one main action (Play), and for warnings.
    st.configure("Play.TButton", background=WAKE_RED, foreground="#ffffff", bordercolor=WAKE_DEEP,
                 lightcolor=WAKE_RED, darkcolor=WAKE_RED, font=("Segoe UI Semibold", 11),
                 padding=(px(22), px(4)))
    st.map("Play.TButton", background=[("pressed", WAKE_DEEP), ("active", WAKE_BRIGHT)],
           lightcolor=[("pressed", WAKE_DEEP), ("active", WAKE_BRIGHT)],
           darkcolor=[("pressed", WAKE_DEEP), ("active", WAKE_BRIGHT)])
    st.configure("TCombobox", fieldbackground=C["input"], background=C["panel"], foreground=C["text"],
                 arrowcolor=C["text"], bordercolor=C["border"], padding=(px(4), px(2)))
    st.map("TCombobox", background=[("disabled", C["panel"]), ("pressed", C["input"]),
                                    ("active", C["input"])],
           bordercolor=[("focus", C["focus"])],
           fieldbackground=[("readonly", C["input"]), ("disabled", C["panel"])],
           foreground=[("disabled", C["off"])], arrowcolor=[("disabled", C["off"])],
           selectbackground=[("readonly", C["input"])], selectforeground=[("readonly", C["text"])])
    root.option_add("*TCombobox*Listbox.background", C["input"])
    root.option_add("*TCombobox*Listbox.foreground", C["text"])
    root.option_add("*TCombobox*Listbox.selectBackground", SEA_500)
    root.option_add("*TCombobox*Listbox.selectForeground", TITLE_WHITE)
    for kind in ("TRadiobutton", "TCheckbutton"):
        st.configure(kind, background=C["panel"], foreground=C["text"], indicatorbackground=C["input"],
                     indicatorforeground=C["text"], upperbordercolor=C["border"],
                     lowerbordercolor=C["border"])
        st.map(kind, background=[("active", C["panel"])], foreground=[("disabled", C["off"])],
               indicatorbackground=[("selected", C["input"]), ("disabled", C["panel"])],
               indicatorforeground=[("disabled", C["off"])])
    st.configure("Vertical.TScrollbar", background=C["panel"], troughcolor=C["bg"],
                 bordercolor=C["border"], arrowcolor=C["text"])
    st.configure("Horizontal.TProgressbar", background=WAKE_RED, troughcolor=C["input"],
                 bordercolor=C["border"], lightcolor=WAKE_RED, darkcolor=WAKE_RED)
    st.configure("Horizontal.TScale", background=C["panel"], troughcolor=C["input"])
    root.configure(background=C["bg"])
    return mode


def dark_title_bar(win):
    """Windows' own title bar in dark mode too, when the app mode is dark."""
    if C.get("mode") != "dark":
        return
    try:
        import ctypes
        win.update_idletasks()
        hwnd = ctypes.c_void_p(int(win.wm_frame(), 16))
        on = ctypes.c_int(1)
        ctypes.windll.dwmapi.DwmSetWindowAttribute(hwnd, 20, ctypes.byref(on), 4)  # immersive dark
    except (OSError, AttributeError, ValueError, tk.TclError):
        pass


def set_icon(root):
    """The program's icon (A1's, drawn at build time beside the exe); Tk's
    own if this build has none."""
    ico = os.path.join(os.path.dirname(EXE), "bloodwake.ico")
    if not os.path.isfile(ico):
        ico = os.path.join(REPO, "build", "Release", "bloodwake.ico")
    try:
        if os.path.isfile(ico):
            root.iconbitmap(default=ico)
    except tk.TclError:
        pass


class Banner(tk.Canvas):
    """The header band: the banner's night sea, its moon and the red wake
    behind a small boat (public/docs/images/banner.svg, our own art), with the
    title. Drawn by Tk at any width, so it stays crisp at any scaling."""

    def __init__(self, parent, height):
        super().__init__(parent, height=height, highlightthickness=0, borderwidth=0,
                         background=NIGHT_800)
        self.h = height
        self.bind("<Configure>", lambda e: self.draw(e.width))

    def draw(self, w):
        self.delete("all")
        h = self.h
        hz = int(h * 214 / 360)                 # the horizon, where the banner has it

        def sky(y):                             # Night 900 to Night 800 to Night 700
            t = max(0.0, min(1.0, y / float(hz)))
            return blend(NIGHT_900, NIGHT_800, t / 0.55) if t < 0.55 else                 blend(NIGHT_800, NIGHT_700, (t - 0.55) / 0.45)
        for y in range(0, hz, 2):               # 2-pixel steps: no visible bands
            self.create_rectangle(0, y, w, y + 2, fill=sky(y + 1), width=0)
        # The moon and its halo, towards the right. The halo is drawn row by
        # row in rings, each blended with that row's own sky, so it has no
        # edge and no visible bands.
        mx, my, mr = w - max(px(70), int(w * 0.10)), int(h * 0.30), max(3, int(h * 0.075))
        halo, rings = int(h * 0.42), 14
        for y in range(max(0, my - halo), min(hz, my + halo)):
            dy = abs(y + 0.5 - my)
            for k in range(rings):
                r = halo - (halo - mr) * k / float(rings)
                if r <= dy:
                    continue
                t = (1 - (r - mr) / float(halo - mr)) ** 2
                hw = (r * r - dy * dy) ** 0.5
                self.create_line(mx - hw, y, mx + hw, y, fill=blend(sky(y), "#cfe6ff", 0.20 * t))
        self.create_rectangle(0, hz, w, h, fill=SEA_800, width=0)
        self.create_oval(mx - mr, my - mr, mx + mr, my + mr, width=0, fill=blend(sky(my), MOON, 0.85))
        # The sea and its three swells.
        self.create_line(0, hz, w, hz, fill=blend(SEA_800, HORIZON, 0.45))
        for frac, col, hi in ((0.18, SEA_700, None), (0.48, SEA_600, SWELL_LIGHT),
                              (0.78, SEA_500, "#9fd6ff")):
            y, amp, pts = hz + (h - hz) * frac, max(1.5, (h - hz) * 0.07), []
            for k in range(-1, 18):             # past both edges: the curve ends inside
                pts += [w * k / 16.0, y + (amp if k % 2 else -amp)]
            self.create_polygon(pts + [w, h, 0, h], fill=blend(SEA_800, col, 0.88), smooth=True, width=0)
            if hi:
                self.create_line(pts, fill=blend(col, hi, 0.28), smooth=True)
        # The wake: a widening red V behind a small boat heading for the
        # horizon, bright at the boat and fading into the sea.
        bx, top = mx - int(h * 0.55), hz + max(2, int(h * 0.06))
        spread, layers = h * 0.75, 18
        for i in range(layers):
            t0, t1 = i / float(layers), (i + 1) / float(layers)
            y0, y1 = top + (h - top) * t0, top + (h - top) * t1
            s0, s1 = 2 + spread * t0 ** 1.15, 2 + spread * t1 ** 1.15
            col = blend(WAKE_BRIGHT, WAKE_RED, t0 / 0.45) if t0 < 0.45 else                 blend(WAKE_RED, WAKE_DEEP, (t0 - 0.45) / 0.55)
            self.create_polygon(bx - s0, y0, bx + s0, y0, bx + s1, y1, bx - s1, y1, width=0,
                                fill=blend(SEA_600, col, 0.85 * (1 - t0) ** 0.8))
            for side in (-1, 1):                # the foam along its edges
                self.create_line(bx + side * s0, y0, bx + side * s1, y1,
                                 fill=blend(SEA_600, TITLE_WHITE, 0.6 * (1 - t0)))
        # The boat: a dark hull on the wake's point.
        bw, bh = max(3, int(h * 0.05)), max(5, int(h * 0.13))
        self.create_polygon(bx, top - bh, bx + bw, top - bh * 0.35, bx + bw * 0.8, top + 1,
                            bx - bw * 0.8, top + 1, bx - bw, top - bh * 0.35, width=1,
                            fill="#1c2633", outline=blend(SEA_800, TITLE_WHITE, 0.55))
        # The title: BLOOD in Title white, WAKE in Title red, RECOMP below in Recomp blue.
        x, ty = px(20), h * 0.40
        big = ("Segoe UI Black", max(14, int(h * 0.26 / SCALE * 0.75)))
        a = self.create_text(x, ty, text="BLOOD ", anchor="w", font=big, fill=TITLE_WHITE)
        bb = self.bbox(a)
        self.create_text(bb[2], ty, text="WAKE", anchor="w", font=big, fill=TITLE_RED)
        self.create_text(x + px(2), ty + h * 0.27, text="R E C O M P", anchor="w",
                         font=("Segoe UI Semibold", max(8, int(h * 0.10 / SCALE * 0.75))),
                         fill=RECOMP_BLUE)


def px(n):
    """A size in pixels at 100% display scaling, scaled to this display."""
    return int(n * SCALE + 0.5)


def work_area():
    """The usable part of the screen the launcher opens on (the monitor under
    the mouse, less the taskbar), as (x, y, width, height) in pixels; None if
    Windows can't say.

    Test switch: BW_LAUNCHER_SCREEN=1366x768 or 1920x1080@150 pretends the
    screen is that size (and that display scaling, in percent), with a 48-pixel
    taskbar at 100%, so the fit can be checked on any monitor."""
    fake = fake_screen()
    try:
        import ctypes
        from ctypes import wintypes

        class MonitorInfo(ctypes.Structure):
            _fields_ = [("cbSize", wintypes.DWORD), ("rcMonitor", wintypes.RECT),
                        ("rcWork", wintypes.RECT), ("dwFlags", wintypes.DWORD)]
        u32 = ctypes.windll.user32
        u32.MonitorFromPoint.restype = wintypes.HMONITOR
        u32.MonitorFromPoint.argtypes = [wintypes.POINT, wintypes.DWORD]
        u32.GetMonitorInfoW.argtypes = [wintypes.HMONITOR, ctypes.c_void_p]
        pt = wintypes.POINT()
        u32.GetCursorPos(ctypes.byref(pt))
        mi = MonitorInfo()
        mi.cbSize = ctypes.sizeof(mi)
        if not u32.GetMonitorInfoW(u32.MonitorFromPoint(pt, 2), ctypes.byref(mi)):  # 2: nearest
            raise OSError("GetMonitorInfoW")
        r = mi.rcWork
        area = (r.left, r.top, r.right - r.left, r.bottom - r.top)
    except (OSError, AttributeError, ImportError):
        area = None
    if fake:
        w, h, scale = fake
        x, y = area[:2] if area else (0, 0)
        return (x, y, w, h - int(48 * scale + 0.5))
    return area


FIT_SLACK = 12          # pixels at 100% kept free around the window inside the work area
MAX_OPEN_H = 820        # the tallest a window opens at 100%, however long its longest tab


def frame_size():
    """What Windows puts around a window's client area at this display
    scaling (title bar and borders, its invisible resize borders included,
    so it errs large), as (width, height) in pixels. A faked scaling
    (BW_LAUNCHER_SCREEN) scales it too."""
    try:
        import ctypes
        from ctypes import wintypes
        u32 = ctypes.windll.user32
        r = wintypes.RECT(0, 0, 0, 0)
        style = 0x00CF0000                          # WS_OVERLAPPEDWINDOW, as Tk's toplevel
        try:
            dpi = u32.GetDpiForSystem()             # this process is system-DPI aware
            ok = u32.AdjustWindowRectExForDpi(ctypes.byref(r), style, False, 0, dpi)
        except AttributeError:                      # before Windows 10 1607
            dpi = 96
            ok = u32.AdjustWindowRectEx(ctypes.byref(r), style, False, 0)
        if ok and dpi:
            k = SCALE / (dpi / 96.0)
            return int((r.right - r.left) * k + 0.5), int((r.bottom - r.top) * k + 0.5)
    except (OSError, AttributeError):
        pass
    return px(16), px(48)


def visible_rect(hwnd):
    """A window's visible rectangle (left, top, right, bottom): DWM's frame
    bounds, without the invisible resize borders; else GetWindowRect."""
    import ctypes
    from ctypes import wintypes
    r = wintypes.RECT()
    try:
        if ctypes.windll.dwmapi.DwmGetWindowAttribute(wintypes.HWND(hwnd), 9, ctypes.byref(r),
                                                     ctypes.sizeof(r)) == 0:
            return r.left, r.top, r.right, r.bottom
    except OSError:
        pass
    ctypes.windll.user32.GetWindowRect(wintypes.HWND(hwnd), ctypes.byref(r))
    return r.left, r.top, r.right, r.bottom


def fake_screen():
    """BW_LAUNCHER_SCREEN (see work_area) as (width, height, scale), or None."""
    import re
    m = re.match(r"^\s*(\d+)\s*x\s*(\d+)\s*(?:@\s*(\d+)\s*%?)?\s*$",
                 os.environ.get("BW_LAUNCHER_SCREEN", ""))
    if not m:
        return None
    return int(m.group(1)), int(m.group(2)), (int(m.group(3)) if m.group(3) else 100) / 100.0


class Scroll(ttk.Frame):
    """A tab's page: its contents in `body`, with a scroll bar that appears
    only when the window is too short to show them all."""

    def __init__(self, parent, padding=12):
        super().__init__(parent)
        self.canvas = tk.Canvas(self, highlightthickness=0, borderwidth=0, background=C["panel"])
        self.bar = ttk.Scrollbar(self, orient="vertical", command=self.canvas.yview)
        self.body = ttk.Frame(self.canvas, padding=padding)
        self.item = self.canvas.create_window(0, 0, window=self.body, anchor="nw")
        self.canvas.configure(yscrollcommand=self.bar.set)
        self.canvas.pack(side="left", fill="both", expand=True)
        self.shown = False
        self.body.bind("<Configure>", self.fit, add="+")
        self.canvas.bind("<Configure>", self.fit, add="+")

    def natural(self):
        """Ask for the contents' own size, so the window opens big enough for
        them when the screen has room."""
        self.body.update_idletasks()
        self.canvas.configure(width=self.body.winfo_reqwidth(),
                              height=self.body.winfo_reqheight())

    def overflow(self):
        return self.body.winfo_reqheight() > self.canvas.winfo_height() + 1

    def fit(self, _e=None):
        cw, ch = self.canvas.winfo_width(), self.canvas.winfo_height()
        if cw <= 1 or ch <= 1:
            return
        need = self.body.winfo_reqheight()
        # The contents fill the page when they fit (so a box that grows, like
        # the Advanced tab's, gets the room), and scroll when they don't.
        self.canvas.itemconfigure(self.item, width=cw, height=max(need, ch))
        self.canvas.configure(scrollregion=(0, 0, cw, max(need, ch)))
        if need > ch + 1 and not self.shown:
            self.bar.pack(side="right", fill="y", before=self.canvas)
            self.shown = True
        elif need <= ch + 1 and self.shown:
            self.bar.pack_forget()
            self.canvas.yview_moveto(0)
            self.shown = False

    def wheel(self, e):
        if self.shown:
            self.canvas.yview_scroll(int(-e.delta / 120) or (-1 if e.delta > 0 else 1), "units")


# ── The running game, the start record, safe mode (item 22) ───────────────
MUTEX = "BloodWakeRecomp.Running"       # held by the running game's host (CONTRACTS.md, 1)
HOST_CLASS = "BloodWakeHost"            # its window (src/bw_host.c)
START_STATE = "start_state.ini"         # beside the settings file (CONTRACTS.md, 4)
SAFE_FILE = "settings_safe.ini"         # safe mode's settings, beside them too
SETTINGS_FORMAT = 2     # [launcher] format: 2 from 0.2.5 on; a file without it is older (src/bw_settings.c)
# Defaults 0.2.5 changed: in a file from before 0.2.5 the old one reads as the
# new one (calls 85 and 89; the game's s_old_default holds the same pairs).
OLD_DEFAULTS = {("display", "mode"): ("window", "fullscreen"),
                ("controls", "keyboard"): ("off", "on")}

# ── Smart App Control (item 4, the launcher's half) ───────────────────────
# Setup's own help text (A1, item 4), word for word, so the two match.
SAC_TEXT = ("Smart App Control is blocking the game. The game is built on this PC, so it "
            "isn't code-signed, and Smart App Control blocks programs that aren't. To play, "
            "turn it off: Windows Security > App & browser control > Smart App Control "
            "settings > Off. It's a setting for the whole PC (on some versions of Windows it "
            "can't be turned back on without reinstalling), so it's your choice.")
SAC_QUICK = 10          # seconds: a game that ends this soon after Play didn't really start


def smart_app_control_on():
    """Whether Windows' Smart App Control is on and enforcing, as Setup reads
    it (VerifiedAndReputablePolicyState = 1; 0 is off, 2 evaluation, which
    doesn't block). Read only: the launcher never changes it. Test switch:
    BW_TEST_SAC=1 reads as on."""
    if os.environ.get("BW_TEST_SAC") == "1":
        return True
    try:
        import winreg
        k = winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"SYSTEM\CurrentControlSet\Control\CI\Policy",
                           0, winreg.KEY_READ)
        try:
            return winreg.QueryValueEx(k, "VerifiedAndReputablePolicyState")[0] == 1
        finally:
            winreg.CloseKey(k)
    except (OSError, ImportError):
        return False


def start_failure_text(what, err):
    """What to say when the game couldn't be run: Setup's Smart App Control
    help when that's on, else the error itself."""
    if smart_app_control_on():
        return SAC_TEXT
    return "%s:\n%s" % (what, err)


# ── The screen and Automatic HDR (items 97, 100; D-026) ──────────────────────────────
WINHDR_MARKER = "windows_hdr_restore.json"  # beside the settings: Windows HDR to put back
SESSION_FILE = "settings_session.ini"        # a session's own settings (HDR off for SDR)


def screen_info(settings_schema=None, x=None, y=None):
    """What the game knows about the screen at x, y (the launcher window's
    centre; else the primary): bloodwake.exe --screen-info[=x,y] (B3, D-026)
    gives "device" (the GDI name), "hdr_capable", "hdr_on", "source" (edid,
    profile or none), "peak" and "paper_white" ({"cd_m2": ..., -1 when not
    given}), "mapping" (what Auto does: hdr, sdr-direct or sdr) and
    "mapping_if_forced_on" (what On does: hdr, hdr-defaults or sdr). None
    when this exe can't say. Asked only of an exe that knows the flag (one
    without it would start the game). Test switch: BW_TEST_SCREEN_INFO=<the
    JSON answer> stands in for it."""
    fake = os.environ.get("BW_TEST_SCREEN_INFO")
    if fake:
        try:
            return json.loads(fake)
        except ValueError:
            return None
    if not ((settings_schema or {}).get("screen_info") or exe_has_flag(EXE, "--screen-info")):
        return None
    cmd = [EXE, "--screen-info" + ("=%d,%d" % (x, y) if x is not None and y is not None else "")]
    try:
        out = subprocess.run(cmd, capture_output=True, timeout=20, creationflags=CREATE_NO_WINDOW,
                             cwd=REPO)
        d = json.loads(out.stdout.decode("utf-8", "replace"))
        return d if isinstance(d, dict) else None
    except (OSError, ValueError, subprocess.SubprocessError):
        return None


def _nits(v):
    """A brightness from --screen-info ({"cd_m2": ...} or a number), None if not given."""
    if isinstance(v, dict):
        v = v.get("cd_m2")
    try:
        v = float(v)
    except (TypeError, ValueError):
        return None
    return v if v > 0 else None


def screen_line(info):
    """The Graphics tab's line: what Automatic HDR would do on this screen
    (item 100), in plain words; None when the game can't say. (Never DXGI's
    own figures: with Windows HDR off they are SDR ones.)"""
    if not info:
        return None
    if info.get("source") == "none":
        return "This screen doesn't report its HDR range, so Automatic HDR would play in SDR."
    if not info.get("hdr_capable"):
        return "This screen doesn't support HDR, so Automatic HDR would play in SDR."
    peak = _nits(info.get("peak"))
    return "Automatic HDR would play in HDR here" + (
        " (peak {:,} nits)".format(int(round(peak))) if peak else "") + "."


def profile_warning(info):
    """A warning when an active Windows HDR calibration profile caps the
    screen below its EDID peak: the game still maps to the EDID's peak
    (D-026), so Windows clips the highlights above the cap."""
    if not info or not info.get("profile_active"):
        return None
    cap, peak = _nits(info.get("profile_peak")), _nits(info.get("peak"))
    if not cap or not peak or cap >= peak:
        return None
    return ("A Windows HDR calibration profile limits this screen to {:,} nits, so Windows cuts "
            "off the brightest highlights. For the full range, remove it in Windows' Color "
            "Management (Advanced tab).".format(int(round(cap))))


def winhdr_screen(device=None):
    """The screen Windows HDR would be switched on (the one named, else the
    primary) as a dict with "device", "supported" and "on"; None if Windows
    can't say. Test switch: BW_TEST_WINHDR=<a JSON file> stands in for
    Windows, so a test never changes this PC's HDR."""
    fake = os.environ.get("BW_TEST_WINHDR")
    if fake:
        try:
            with open(fake, encoding="utf-8") as f:
                d = json.load(f)
            d.setdefault("device", device or "\\\\.\\DISPLAY1")
            return d
        except (OSError, ValueError):
            return None
    if bw_hdr is None:
        return None
    try:
        return bw_hdr.screen(device)
    except OSError:
        return None


def winhdr_set(scr, on):
    """Switch Windows HDR on or off for that screen; True if it reads back so."""
    fake = os.environ.get("BW_TEST_WINHDR")
    if fake:
        try:
            with open(fake, encoding="utf-8") as f:
                d = json.load(f)
            d["on"] = bool(on)
            d["changes"] = d.get("changes", 0) + 1
            with open(fake, "w", encoding="utf-8") as f:
                json.dump(d, f)
            return True
        except (OSError, ValueError):
            return False
    if bw_hdr is None or not scr or "target" not in scr:
        return False
    try:
        bw_hdr.set_state(scr["target"], scr["api"], on)
        for _i in range(30):                    # it takes a moment to take effect
            now = bw_hdr.screen(scr["device"])
            if now and now["on"] == bool(on):
                return True
            time.sleep(0.1)
    except OSError:
        pass
    return False


def winhdr_restore(folder):
    """Put Windows HDR back as the marker says (a session that switched it
    on), then remove the marker. Returns what was done, or None."""
    path = os.path.join(folder, WINHDR_MARKER)
    try:
        with open(path, encoding="utf-8") as f:
            m = json.load(f)
    except (OSError, ValueError):
        return None
    scr = winhdr_screen(m.get("device"))
    done = None
    if scr and scr.get("on") != bool(m.get("was_on")):
        done = "restored" if winhdr_set(scr, bool(m.get("was_on"))) else "failed"
    else:
        done = "already"
    if done != "failed":
        try:
            os.remove(path)
        except OSError:
            pass
    return done


def mutex_name(adv=()):
    """The running game's mutex. Test switch BW_TEST_MUTEX=<word> (the host's
    own rule, CONTRACTS.md 1) makes it "<name>.<word>", so test sessions don't
    meet each other. Taken from the launcher's environment, else from the
    Advanced tab's lines (a settings file's [advanced] reaches the host too)."""
    word = os.environ.get("BW_TEST_MUTEX") or dict(adv).get("BW_TEST_MUTEX")
    return MUTEX + "." + word if word else MUTEX


def game_running(adv=()):
    """Whether Blood Wake is running now: its host holds the mutex."""
    try:
        import ctypes
        k32 = ctypes.windll.kernel32
        k32.OpenMutexW.restype = ctypes.c_void_p
        k32.OpenMutexW.argtypes = [ctypes.c_uint32, ctypes.c_int, ctypes.c_wchar_p]
        k32.CloseHandle.argtypes = [ctypes.c_void_p]
        h = k32.OpenMutexW(0x00100000, False, mutex_name(adv))   # SYNCHRONIZE
        if h:
            k32.CloseHandle(h)
            return True
    except (OSError, AttributeError):
        pass
    return False


def bring_game_to_front():
    """Show the running game's window, restored if it was minimised."""
    try:
        import ctypes
        u32 = ctypes.windll.user32
        u32.FindWindowW.restype = ctypes.c_void_p
        u32.IsIconic.argtypes = u32.SetForegroundWindow.argtypes = [ctypes.c_void_p]
        u32.ShowWindow.argtypes = [ctypes.c_void_p, ctypes.c_int]
        h = u32.FindWindowW(HOST_CLASS, None)
        if h:
            if u32.IsIconic(h):
                u32.ShowWindow(h, 9)                        # SW_RESTORE
            u32.SetForegroundWindow(h)
            return True
    except (OSError, AttributeError):
        pass
    return False


def start_history(folder):
    """The last starts, oldest first, from start_state.ini: "1" reached the
    title (or a launched mission's first frame), "0" didn't. "" if none."""
    cp = configparser.ConfigParser(interpolation=None, strict=False)
    try:
        cp.read(os.path.join(folder, START_STATE), encoding="utf-8")
    except (OSError, configparser.Error, UnicodeDecodeError):
        return ""
    return "".join(c for c in cp.get("start", "history", fallback="") if c in "01")


def write_safe_settings(path, test_mutex=None):
    """Safe mode's settings: the Original edition, in a window, the keyboard
    on as controller 1 (call 89: otherwise a player without a pad couldn't
    press anything in safe mode; a pad still works beside it), nothing else
    (so no visual suite, HDR or frame-rate change, and no [advanced]
    switches, keys or cheats). The player's own file is untouched.
    test_mutex: the test switch BW_TEST_MUTEX, kept so a test session stays
    apart from a real one."""
    with open(path, "w", encoding="utf-8") as f:
        f.write("; Blood Wake safe mode, written by the launcher (Start in safe mode).\n"
                "; Your own settings stay in settings.ini; this file is only for safe mode.\n\n"
                "[edition]\nedition = original\n\n[display]\nmode = window\n\n"
                "[controls]\nkeyboard = on\n\n"
                # 0.2.5's format, so this Window is a choice, not 0.2.4's
                # default (call 85 would read that as Fullscreen).
                "[launcher]\nformat = %d\n" % SETTINGS_FORMAT)
        if test_mutex:
            f.write("\n[advanced]\nBW_TEST_MUTEX=%s\n" % test_mutex)
    return path


# ── The bug report (item 22) ──────────────────────────────────────────────
REPORT_LOG_MAX = 16 << 20               # the end of a longer log is kept (it says so)
REPORT_TAIL = 400                       # lines kept from each build log


def desktop_folder():
    """The player's desktop (wherever Windows keeps it, OneDrive included).
    Test switch: BW_BUGREPORT_DIR=<folder> puts the report there instead."""
    if os.environ.get("BW_BUGREPORT_DIR"):
        return os.environ["BW_BUGREPORT_DIR"]
    try:
        import ctypes
        buf = ctypes.create_unicode_buffer(260)
        if ctypes.windll.shell32.SHGetFolderPathW(None, 0x10, None, 0, buf) == 0 and buf.value:
            return buf.value                                # CSIDL_DESKTOPDIRECTORY
    except (OSError, AttributeError):
        pass
    return os.path.join(os.path.expanduser("~"), "Desktop")


def windows_version():
    try:
        import winreg
        k = winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Microsoft\Windows NT\CurrentVersion")

        def val(n, d=""):
            try:
                return winreg.QueryValueEx(k, n)[0]
            except OSError:
                return d
        build = int(val("CurrentBuild", "0") or 0)
        name = "Windows 11" if build >= 22000 else "Windows 10" if build >= 10240 else val("ProductName")
        return "%s %s, %s edition (build %s.%s)" % (name, val("DisplayVersion"), val("EditionID"),
                                                   build, val("UBR", 0))
    except (OSError, ImportError, ValueError):
        import platform
        return platform.platform()


def graphics_cards():
    """The graphics adapters Windows lists, once each."""
    names = []
    try:
        import ctypes
        from ctypes import wintypes

        class DisplayDevice(ctypes.Structure):
            _fields_ = [("cb", wintypes.DWORD), ("DeviceName", wintypes.WCHAR * 32),
                        ("DeviceString", wintypes.WCHAR * 128), ("StateFlags", wintypes.DWORD),
                        ("DeviceID", wintypes.WCHAR * 128), ("DeviceKey", wintypes.WCHAR * 128)]
        i = 0
        while i < 32:
            d = DisplayDevice()
            d.cb = ctypes.sizeof(d)
            if not ctypes.windll.user32.EnumDisplayDevicesW(None, i, ctypes.byref(d), 0):
                break
            if d.DeviceString and d.DeviceString not in names:
                names.append(d.DeviceString)
            i += 1
    except (OSError, AttributeError):
        pass
    return names


def screens():
    """Each screen: size, refresh rate, scaling, and which is the main one."""
    out = []
    try:
        import ctypes
        from ctypes import wintypes

        class MonitorInfoEx(ctypes.Structure):
            _fields_ = [("cbSize", wintypes.DWORD), ("rcMonitor", wintypes.RECT),
                        ("rcWork", wintypes.RECT), ("dwFlags", wintypes.DWORD),
                        ("szDevice", wintypes.WCHAR * 32)]

        class DevMode(ctypes.Structure):          # DEVMODEW, display fields
            _fields_ = [("dmDeviceName", wintypes.WCHAR * 32), ("dmSpecVersion", wintypes.WORD),
                        ("dmDriverVersion", wintypes.WORD), ("dmSize", wintypes.WORD),
                        ("dmDriverExtra", wintypes.WORD), ("dmFields", wintypes.DWORD),
                        ("dmPositionX", wintypes.LONG), ("dmPositionY", wintypes.LONG),
                        ("dmDisplayOrientation", wintypes.DWORD), ("dmDisplayFixedOutput", wintypes.DWORD),
                        ("dmColor", ctypes.c_short), ("dmDuplex", ctypes.c_short),
                        ("dmYResolution", ctypes.c_short), ("dmTTOption", ctypes.c_short),
                        ("dmCollate", ctypes.c_short), ("dmFormName", wintypes.WCHAR * 32),
                        ("dmLogPixels", wintypes.WORD), ("dmBitsPerPel", wintypes.DWORD),
                        ("dmPelsWidth", wintypes.DWORD), ("dmPelsHeight", wintypes.DWORD),
                        ("dmDisplayFlags", wintypes.DWORD), ("dmDisplayFrequency", wintypes.DWORD),
                        ("dmICMMethod", wintypes.DWORD), ("dmICMIntent", wintypes.DWORD),
                        ("dmMediaType", wintypes.DWORD), ("dmDitherType", wintypes.DWORD),
                        ("dmReserved1", wintypes.DWORD), ("dmReserved2", wintypes.DWORD),
                        ("dmPanningWidth", wintypes.DWORD), ("dmPanningHeight", wintypes.DWORD)]
        u32 = ctypes.windll.user32
        mons = []
        proc = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HMONITOR, wintypes.HDC,
                                  ctypes.POINTER(wintypes.RECT), wintypes.LPARAM)

        def each(h, _dc, _r, _l):
            mons.append(h)
            return True
        cb = proc(each)
        u32.EnumDisplayMonitors(None, None, cb, 0)
        for h in mons:
            mi = MonitorInfoEx()
            mi.cbSize = ctypes.sizeof(mi)
            u32.GetMonitorInfoW(wintypes.HMONITOR(h), ctypes.byref(mi))
            dm = DevMode()
            dm.dmSize = ctypes.sizeof(dm)
            hz = ""
            if u32.EnumDisplaySettingsW(mi.szDevice, -1, ctypes.byref(dm)):   # ENUM_CURRENT_SETTINGS
                hz = " at %d Hz" % dm.dmDisplayFrequency
            dpi = ctypes.c_uint(96)
            try:
                ctypes.windll.shcore.GetDpiForMonitor(wintypes.HMONITOR(h), 0, ctypes.byref(dpi),
                                                      ctypes.byref(ctypes.c_uint()))
            except (OSError, AttributeError):
                pass
            r = mi.rcMonitor
            out.append("%dx%d%s, scaling %d%%%s" % (r.right - r.left, r.bottom - r.top, hz,
                                                    round(dpi.value * 100 / 96),
                                                    ", main screen" if mi.dwFlags & 1 else ""))
    except (OSError, AttributeError):
        pass
    return out


def sound_device_mmdevice():
    """Windows' default sound output (render, console) in full, from its own
    device API (IMMDeviceEnumerator, through ctypes): "<name> (<n> channels,
    <rate> Hz)", or None. The name is the one Windows' sound settings show,
    not cut short (the old wave API gives 31 characters)."""
    import ctypes
    from ctypes import wintypes

    class GUID(ctypes.Structure):
        _fields_ = [("d1", ctypes.c_uint32), ("d2", ctypes.c_uint16), ("d3", ctypes.c_uint16),
                    ("d4", ctypes.c_ubyte * 8)]

        def __init__(self, s):
            super().__init__()
            h = s.replace("-", "")
            self.d1, self.d2, self.d3 = int(h[0:8], 16), int(h[8:12], 16), int(h[12:16], 16)
            for i in range(8):
                self.d4[i] = int(h[16 + 2 * i:18 + 2 * i], 16)

    class PROPERTYKEY(ctypes.Structure):
        _fields_ = [("fmtid", GUID), ("pid", wintypes.DWORD)]

    class PROPVARIANT(ctypes.Structure):
        _fields_ = [("vt", ctypes.c_ushort), ("r1", ctypes.c_ushort), ("r2", ctypes.c_ushort),
                    ("r3", ctypes.c_ushort), ("p1", ctypes.c_void_p), ("p2", ctypes.c_void_p)]

    ole32 = ctypes.OleDLL("ole32")
    hr_init = ctypes.windll.ole32.CoInitializeEx(None, 0x2)    # apartment; S_FALSE or a mode clash is fine
    HRESULT = ctypes.HRESULT

    def method(obj, index, *argtypes):
        vtbl = ctypes.cast(obj, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
        return ctypes.WINFUNCTYPE(HRESULT, ctypes.c_void_p, *argtypes)(vtbl[index])

    def release(obj):
        if obj:
            vtbl = ctypes.cast(obj, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
            ctypes.WINFUNCTYPE(wintypes.ULONG, ctypes.c_void_p)(vtbl[2])(obj)

    en, dev, ps = ctypes.c_void_p(), ctypes.c_void_p(), ctypes.c_void_p()
    try:
        ole32.CoCreateInstance(ctypes.byref(GUID("BCDE0395-E52F-467C-8E3D-C4579291692E")), None, 0x17,
                               ctypes.byref(GUID("A95664D2-9614-4F35-A746-DE8DB63617E6")), ctypes.byref(en))
        # IMMDeviceEnumerator::GetDefaultAudioEndpoint(eRender, eConsole)
        method(en, 4, ctypes.c_int, ctypes.c_int, ctypes.POINTER(ctypes.c_void_p))(en, 0, 0, ctypes.byref(dev))
        # IMMDevice::OpenPropertyStore(STGM_READ)
        method(dev, 4, wintypes.DWORD, ctypes.POINTER(ctypes.c_void_p))(dev, 0, ctypes.byref(ps))
        get = method(ps, 5, ctypes.POINTER(PROPERTYKEY), ctypes.POINTER(PROPVARIANT))
        pv = PROPVARIANT()
        # PKEY_Device_FriendlyName
        get(ps, ctypes.byref(PROPERTYKEY(GUID("A45C254E-DF1C-4EFD-8020-67D146A850E0"), 14)), ctypes.byref(pv))
        name = ctypes.wstring_at(pv.p1) if pv.vt == 31 and pv.p1 else None          # VT_LPWSTR
        ctypes.windll.ole32.PropVariantClear(ctypes.byref(pv))
        if not name:
            return None
        fmt = ""
        pv = PROPVARIANT()
        try:
            # PKEY_AudioEngine_DeviceFormat: the shared engine's format (a WAVEFORMATEX blob)
            get(ps, ctypes.byref(PROPERTYKEY(GUID("F19F064D-082C-4E27-BC73-6882A1BB8E4C"), 0)), ctypes.byref(pv))
            if pv.vt == 65 and pv.p2:                                                # VT_BLOB
                raw = ctypes.string_at(pv.p2, 8)
                ch = int.from_bytes(raw[2:4], "little")
                rate = int.from_bytes(raw[4:8], "little")
                fmt = " (%d channels, %d Hz)" % (ch, rate)
        except OSError:
            pass
        ctypes.windll.ole32.PropVariantClear(ctypes.byref(pv))
        return name + fmt
    finally:
        release(ps)
        release(dev)
        release(en)
        if hr_init in (0, 1):
            ctypes.windll.ole32.CoUninitialize()


def sound_device():
    """Windows' default sound output: its full name from Windows' device API,
    or, if that fails, the old wave API's (at most 31 characters); or None."""
    try:
        name = sound_device_mmdevice()
        if name:
            return name
    except (OSError, AttributeError, ValueError):
        pass
    try:
        import ctypes
        from ctypes import wintypes

        class WaveOutCaps(ctypes.Structure):
            _fields_ = [("wMid", wintypes.WORD), ("wPid", wintypes.WORD), ("vDriverVersion", wintypes.UINT),
                        ("szPname", wintypes.WCHAR * 32), ("dwFormats", wintypes.DWORD),
                        ("wChannels", wintypes.WORD), ("wReserved1", wintypes.WORD),
                        ("dwSupport", wintypes.DWORD)]
        wm = ctypes.windll.winmm
        dev, flags = wintypes.DWORD(), wintypes.DWORD()
        wm.waveOutMessage.argtypes = [ctypes.c_void_p, wintypes.UINT, ctypes.c_void_p, ctypes.c_void_p]
        if wm.waveOutMessage(ctypes.c_void_p(0xFFFFFFFF), 0x2015,      # DRVM_MAPPER_PREFERRED_GET
                             ctypes.byref(dev), ctypes.byref(flags)) != 0:
            dev.value = 0
        caps = WaveOutCaps()
        if wm.waveOutGetDevCapsW(dev.value, ctypes.byref(caps), ctypes.sizeof(caps)) == 0:
            return "%s (%d channels)" % (caps.szPname, caps.wChannels)
    except (OSError, AttributeError):
        pass
    return None


def controllers():
    try:
        import ctypes
        st = ctypes.create_string_buffer(16)
        for dll in ("xinput1_4", "xinput1_3", "xinput9_1_0"):
            try:
                xi = getattr(ctypes.windll, dll)
                break
            except OSError:
                xi = None
        if xi:
            return sum(1 for i in range(4) if xi.XInputGetState(i, st) == 0)
    except (OSError, AttributeError):
        pass
    return None


def sound_line(log_path):
    """The game's own output-device line from its log (the last open or
    rebuild: "[XA2] output device: ..."), or None."""
    try:
        with open(log_path, encoding="utf-8", errors="replace") as f:
            hit = None
            for line in f:
                if line.startswith("[XA2] output device: "):
                    hit = line[len("[XA2] output device: "):].strip()
            return hit
    except OSError:
        return None


def adapter_line(log_path):
    """The game's own graphics line from its log (CONTRACTS.md, 8), or None."""
    try:
        with open(log_path, encoding="utf-8", errors="replace") as f:
            hit = None
            for line in f:
                if line.startswith("[GPU] adapter "):
                    hit = line.strip()
            return hit
    except OSError:
        return None


def install_record():
    """Setup's record of this install (install.json in the launcher's folder),
    or {} for a build from the source folder."""
    try:
        with open(os.path.join(REPO, "install.json"), encoding="utf-8") as f:
            d = json.load(f)
        return d if isinstance(d, dict) else {}
    except (OSError, ValueError):
        return {}


# Contract 9: what the Edition tab says about a copy Setup couldn't verify
# (install.json "copy": {"verdict": ..., "accepted": ...}); nothing for a
# verified copy or none.
COPY_TEXT = {
    "unverified": ("Unverified copy",
                   "Setup didn't recognise this copy as the retail disc, and you chose to "
                   "install it anyway%s. Fixes that don't fit this copy are off and marked on "
                   "the Fixes tab."),
    "files differ": ("Copy's files differ",
                     "Some of this copy's files differ from the retail disc's, and you chose "
                     "to install it anyway%s. Fixes that don't fit this copy are off and marked "
                     "on the Fixes tab."),
}


def system_summary(settings_path, edition):
    """The bug report's system.txt: plain facts about this PC and the game's
    set-up, nothing from the game itself."""
    info = install_record()
    copy = (info.get("copy") or {}).get("verdict")

    def fact(fn, none="unknown"):
        """One probe; a probe that fails on some PC says so and never stops the report."""
        try:
            v = fn()
        except Exception as e:              # noqa: BLE001 - any failure is just "unknown"
            return "unknown (%s)" % type(e).__name__
        if isinstance(v, list):
            v = "; ".join(v)
        return none if v is None or v == "" else v
    lines = [
        "Blood Wake bug report, made %s by the launcher." % time.strftime("%Y-%m-%d %H:%M"),
        "",
        "Windows: %s" % fact(windows_version),
        "Game: %s" % ("installed by Setup %s" % (info.get("version") or info.get("installed") or "")
                      if info else "built from the source folder"),
        "Edition chosen in the launcher: %s" % edition.capitalize(),
        "Copy: %s" % (copy or "no record"),
        "Graphics (the game's log): %s" % fact(lambda: adapter_line(os.path.join(REPO, "play.log")),
                                               "not in the log (play once first)"),
        "Graphics cards (Windows): %s" % fact(graphics_cards),
        "Screens: %s" % fact(screens),
        "Sound (Windows' default output): %s" % fact(sound_device),
        "Sound (the game's log): %s" % fact(lambda: sound_line(os.path.join(REPO, "play.log")),
                                            "not in the log (play once first)"),
        "Controllers connected (XInput): %s" % fact(controllers),
        "Starts (oldest first; 1 = reached the title): %s" % fact(
            lambda: ",".join(start_history(os.path.dirname(settings_path))), "no record"),
        "",
        "In this zip: the game's log (play.log), the launcher's settings, the start",
        "record, Setup's record and logs when there are any, and this summary. File",
        "paths in them include your Windows user name. Nothing from the game's disc.",
    ]
    return "\n".join(lines) + "\n"


def newest_setup_log(folder=REPO):
    """Inno Setup's newest log (in %TEMP%) of the install in `folder`, the one
    this launcher runs from, or None (a build from the source folder has none:
    another install's log would only mislead)."""
    tmp = os.environ.get("TEMP") or os.environ.get("TMP")
    if not tmp or not os.path.isdir(tmp):
        return None
    want = os.path.normcase(os.path.abspath(folder)).rstrip("\\/")
    try:
        logs = sorted((os.path.join(tmp, n) for n in os.listdir(tmp)
                       if n.lower().startswith("setup log") and n.lower().endswith(".txt")),
                      key=os.path.getmtime, reverse=True)
    except OSError:
        return None
    for p in logs[:12]:
        try:
            with open(p, encoding="utf-8", errors="replace") as f:
                head = os.path.normcase(f.read(1 << 20))
            if want + "\\" in head or head.rstrip().endswith(want):
                return p
        except OSError:
            continue
    return None


def make_bug_report(out_path, settings_path, edition):
    """Write the bug report zip to out_path. Returns the names inside it."""
    import zipfile
    names = []
    with zipfile.ZipFile(out_path, "w", zipfile.ZIP_DEFLATED) as z:
        def add_file(src, arc, tail_lines=None, max_bytes=None):
            if not src or not os.path.isfile(src):
                return
            try:
                if tail_lines:
                    with open(src, encoding="utf-8", errors="replace") as f:
                        lines = f.readlines()
                    text = "".join(lines[-tail_lines:])
                    if len(lines) > tail_lines:
                        text = "(the last %d of %d lines)\n" % (tail_lines, len(lines)) + text
                    z.writestr(arc, text)
                elif max_bytes and os.path.getsize(src) > max_bytes:
                    with open(src, "rb") as f:
                        f.seek(-max_bytes, 2)
                        z.writestr(arc, b"(the last %d MB of the log)\n" % (max_bytes >> 20) + f.read())
                else:
                    z.write(src, arc)
                names.append(arc)
            except OSError:
                pass
        z.writestr("system.txt", system_summary(settings_path, edition))
        names.append("system.txt")
        add_file(os.path.join(REPO, "play.log"), "play.log", max_bytes=REPORT_LOG_MAX)
        folder = os.path.dirname(settings_path)
        add_file(settings_path, "settings/settings.ini")
        add_file(os.path.join(folder, SAFE_FILE), "settings/" + SAFE_FILE)
        add_file(os.path.join(folder, START_STATE), "settings/" + START_STATE)
        add_file(os.path.join(REPO, "install.json"), "setup/install.json")
        add_file(os.path.join(REPO, "build", "Release", "bw_copy.txt"), "setup/bw_copy.txt")
        add_file(newest_setup_log(), "setup/setup_log.txt")
        logs = os.path.join(REPO, "work", "logs")
        for n in ("pipeline.log", "cmake_configure.log", "cmake_build.log"):
            add_file(os.path.join(logs, n), "setup/" + n, tail_lines=REPORT_TAIL)
    return names


def python_console():
    """A console Python for play.py (hidden), even when this runs under pythonw."""
    exe = sys.executable
    if os.path.basename(exe).lower() == "pythonw.exe":
        cand = os.path.join(os.path.dirname(exe), "python.exe")
        if os.path.isfile(cand):
            return cand
    return exe


def load_schema():
    out = subprocess.run([EXE, "--settings-schema"], capture_output=True, timeout=20,
                         creationflags=CREATE_NO_WINDOW, cwd=REPO)
    return json.loads(out.stdout.decode("utf-8", "replace"))


# ── Keys (item 23; the [keys] contract, docs/025/CONTRACTS.md 2) ───────────
# The key names the game reads (the toolkit's xbox_InputMap_KeyFromName, plus
# the mouse names our patch adds), by Windows virtual-key code: the name
# written for a key pressed in the launcher, and what the launcher calls it.
KEY_NAMES = {
    1: ("mouse_left", "Left mouse button"), 2: ("mouse_right", "Right mouse button"),
    4: ("mouse_middle", "Middle mouse button"), 5: ("mouse4", "Mouse button 4"),
    6: ("mouse5", "Mouse button 5"),
    8: ("backspace", "Backspace"), 9: ("tab", "Tab"), 13: ("enter", "Enter"), 16: ("shift", "Shift"),
    17: ("ctrl", "Ctrl"), 18: ("alt", "Alt"), 19: ("pause", "Pause"), 20: ("capslock", "Caps Lock"),
    27: ("escape", "Esc"), 32: ("space", "Space"), 33: ("pageup", "Page Up"),
    34: ("pagedown", "Page Down"), 35: ("end", "End"), 36: ("home", "Home"),
    37: ("left", "Left arrow"), 38: ("up", "Up arrow"), 39: ("right", "Right arrow"),
    40: ("down", "Down arrow"), 45: ("insert", "Insert"), 46: ("delete", "Delete"),
    106: ("numpadmultiply", "Numpad *"), 107: ("numpadadd", "Numpad +"),
    109: ("numpadsubtract", "Numpad -"), 110: ("numpaddecimal", "Numpad ."),
    111: ("numpaddivide", "Numpad /"),
    160: ("lshift", "Left Shift"), 161: ("rshift", "Right Shift"), 162: ("lctrl", "Left Ctrl"),
    163: ("rctrl", "Right Ctrl"), 164: ("lalt", "Left Alt"), 165: ("ralt", "Right Alt"),
    186: ("semicolon", ";"), 187: ("equals", "="), 188: ("comma", ","), 189: ("minus", "-"),
    190: ("period", "."), 191: ("slash", "/"), 192: ("grave", "`"), 219: ("leftbracket", "["),
    220: ("backslash", "\\"), 221: ("rightbracket", "]"), 222: ("quote", "'"),
}
KEY_NAMES.update({c: (chr(c), chr(c)) for c in range(ord("A"), ord("Z") + 1)})
KEY_NAMES.update({c: (chr(c), chr(c)) for c in range(ord("0"), ord("9") + 1)})
KEY_NAMES.update({0x70 + i: ("F%d" % (i + 1), "F%d" % (i + 1)) for i in range(12)})
KEY_NAMES.update({0x60 + i: ("Numpad%d" % i, "Numpad %d" % i) for i in range(10)})
KEY_ALIASES = {"return": 13, "control": 17, "esc": 27, "mouse1": 1, "mouse2": 2, "mouse3": 4}
_BY_NAME = {n.lower(): vk for vk, (n, _l) in KEY_NAMES.items()}
_BY_NAME.update(KEY_ALIASES)


def key_vk(name):
    """A key name's virtual-key code (case-insensitive), or None."""
    return _BY_NAME.get((name or "").strip().lower())


def key_label(name):
    """What the launcher calls a key: "Numpad 8", "Left mouse button" ..."""
    vk = key_vk(name)
    return KEY_NAMES[vk][1] if vk in KEY_NAMES else name


# TEMPORARY COPY (item 23), until C3's `bloodwake.exe --keys-schema` lands: the
# toolkit's shipped table (xboxrecomp/src/input/keyboard.c:92-120), in its
# binding names and control order (input_map.c:13-19). load_keys_schema()
# uses the exe's own table whenever the exe has one.
KEYS_FALLBACK = {
    "controls": [
        {"name": "dpad_up", "label": "D-pad up", "keys": ["up"]},
        {"name": "dpad_down", "label": "D-pad down", "keys": ["down"]},
        {"name": "dpad_left", "label": "D-pad left", "keys": ["left"]},
        {"name": "dpad_right", "label": "D-pad right", "keys": ["right"]},
        {"name": "start", "label": "Start", "keys": ["enter"]},
        {"name": "back", "label": "Back", "keys": ["backspace"]},
        {"name": "left_thumb", "label": "Left stick click", "keys": ["shift"]},
        {"name": "right_thumb", "label": "Right stick click", "keys": ["ctrl"]},
        {"name": "a", "label": "A", "keys": ["Z"]},
        {"name": "b", "label": "B", "keys": ["X"]},
        {"name": "x", "label": "X", "keys": ["C"]},
        {"name": "y", "label": "Y", "keys": ["V"]},
        {"name": "black", "label": "Black", "keys": ["E"]},
        {"name": "white", "label": "White", "keys": ["Q"]},
        {"name": "left_trigger", "label": "Left trigger", "keys": ["1"]},
        {"name": "right_trigger", "label": "Right trigger", "keys": ["3"]},
        {"name": "left_stick_up", "label": "Left stick up", "keys": ["W", "Numpad8"]},
        {"name": "left_stick_down", "label": "Left stick down", "keys": ["S", "Numpad2"]},
        {"name": "left_stick_left", "label": "Left stick left", "keys": ["A", "Numpad4"]},
        {"name": "left_stick_right", "label": "Left stick right", "keys": ["D", "Numpad6"]},
        {"name": "right_stick_up", "label": "Right stick up", "keys": ["I"]},
        {"name": "right_stick_down", "label": "Right stick down", "keys": ["K"]},
        {"name": "right_stick_left", "label": "Right stick left", "keys": ["J"]},
        {"name": "right_stick_right", "label": "Right stick right", "keys": ["L"]},
    ],
    "ramp_ms": {"default": 0, "min": 0, "max": 2000},
    "strength_pct": {"default": 100, "min": 10, "max": 100},
    "deadzone_left": {"default": 0, "min": 0, "max": 95},
    "deadzone_right": {"default": 0, "min": 0, "max": 95},
    "temporary": True,
}
KEYS_MAX = 4                    # keys per control (the contract)
SENSITIVITY = ("ramp_ms", "strength_pct", "deadzone_left", "deadzone_right")


def exe_has_flag(exe, flag):
    """Whether the exe knows a command-line flag: its own bytes name it. An
    exe without it would treat the flag as nothing and start the game."""
    try:
        with open(exe, "rb") as f:
            return flag.encode("ascii") in f.read()
    except OSError:
        return False


def load_keys_schema(settings_schema=None):
    """The game's default key table (bloodwake.exe --keys-schema), else the
    temporary copy above. Never runs an exe that doesn't know the flag."""
    known = (settings_schema or {}).get("keys_schema") or exe_has_flag(EXE, "--keys-schema")
    if known:
        try:
            out = subprocess.run([EXE, "--keys-schema"], capture_output=True, timeout=20,
                                 creationflags=CREATE_NO_WINDOW, cwd=REPO)
            d = json.loads(out.stdout.decode("utf-8", "replace"))
            if d.get("controls"):
                for k in SENSITIVITY:
                    d.setdefault(k, KEYS_FALLBACK[k])
                return d
        except (OSError, ValueError, subprocess.SubprocessError):
            pass
    return KEYS_FALLBACK


def same_keys(a, b):
    """Two key lists bind the same keys (order and spelling aside)."""
    return {key_vk(k) or k.lower() for k in a} == {key_vk(k) or k.lower() for k in b}


class Tip:
    """A short explanation that appears while the mouse is over a widget."""

    def __init__(self, widget, text):
        self.w, self.text, self.top = widget, text, None
        widget.bind("<Enter>", self.show, add="+")
        widget.bind("<Leave>", self.hide, add="+")

    def show(self, _e=None):
        if self.top or not self.text:
            return
        x = self.w.winfo_rootx() + 12
        y = self.w.winfo_rooty() + self.w.winfo_height() + 4
        self.top = tk.Toplevel(self.w)
        self.top.wm_overrideredirect(True)
        self.top.wm_geometry("+%d+%d" % (x, y))
        tk.Label(self.top, text=self.text, justify="left", wraplength=px(380), background=C["input"],
                 font=("Segoe UI", 9),
                 foreground=C["text"], highlightthickness=1, highlightbackground=C["border"],
                 borderwidth=0, padx=px(8), pady=px(6)).pack()

    def hide(self, _e=None):
        if self.top:
            self.top.destroy()
            self.top = None


class Ask:
    """A question with two answers, on top of the launcher: ask() returns
    True for the first button, False for the second or for closing it."""

    def __init__(self, root, title, text, yes, no):
        self.root, self.answer = root, False
        self.top = top = tk.Toplevel(root, background=C["panel"])
        top.title(title)
        top.transient(root)
        top.resizable(False, False)
        dark_title_bar(top)
        body = ttk.Frame(top, padding=px(16))
        body.pack(fill="both", expand=True)
        ttk.Label(body, text=title, style="Head.TLabel").pack(anchor="w")
        ttk.Label(body, text=text, wraplength=px(420), justify="left").pack(anchor="w", pady=(px(6), 0))
        row = ttk.Frame(body)
        row.pack(fill="x", pady=(px(14), 0))
        if no:                                      # no second answer: one button
            self.no_btn = ttk.Button(row, text=no, command=self.close)
            self.no_btn.pack(side="right")
        self.yes_btn = ttk.Button(row, text=yes, command=self.yes)
        self.yes_btn.pack(side="right", padx=(0, px(8)) if no else 0)
        top.protocol("WM_DELETE_WINDOW", self.close)
        top.bind("<Escape>", lambda _e: self.close())
        top.update_idletasks()
        x = root.winfo_rootx() + max(0, (root.winfo_width() - top.winfo_reqwidth()) // 2)
        y = root.winfo_rooty() + max(0, (root.winfo_height() - top.winfo_reqheight()) // 3)
        top.geometry("+%d+%d" % (x, y))
        self.yes_btn.focus_set()

    def yes(self):
        self.answer = True
        self.close()

    def close(self):
        self.top.grab_release()
        self.top.destroy()

    def ask(self):
        self.top.grab_set()
        self.root.wait_window(self.top)
        return self.answer


class Progress:
    """A job the launcher runs for the player (the graphics warm-up), with a
    progress bar fed by its --progress json lines and one button that stops it.
    done(code, last message, stopped) is called once, when it ends."""

    def __init__(self, root, title, cmd, button, done):
        self.root, self.done, self.lines, self.stopped = root, done, queue.Queue(), False
        self.msg, self.title = "", title
        self.top = top = tk.Toplevel(root, background=C["panel"])
        top.title(title)
        top.transient(root)
        top.resizable(False, False)
        dark_title_bar(top)
        body = ttk.Frame(top, padding=px(16))
        body.pack(fill="both", expand=True)
        ttk.Label(body, text=title, style="Head.TLabel").pack(anchor="w")
        self.label = ttk.Label(body, text="Starting...", wraplength=px(420), justify="left")
        self.label.pack(anchor="w", fill="x", pady=(px(6), 0))
        self.bar = ttk.Progressbar(body, length=px(420), maximum=100, mode="determinate")
        self.bar.pack(fill="x", pady=(px(10), 0))
        row = ttk.Frame(body)
        row.pack(fill="x", pady=(px(12), 0))
        self.button = ttk.Button(row, text=button, command=self.stop)
        self.button.pack(side="right")
        top.protocol("WM_DELETE_WINDOW", self.stop)
        top.update_idletasks()
        x = root.winfo_rootx() + max(0, (root.winfo_width() - top.winfo_reqwidth()) // 2)
        y = root.winfo_rooty() + max(0, (root.winfo_height() - top.winfo_reqheight()) // 3)
        top.geometry("+%d+%d" % (x, y))
        top.grab_set()
        self.proc = subprocess.Popen(cmd, cwd=REPO, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT, creationflags=CREATE_NO_WINDOW)
        threading.Thread(target=self.read, daemon=True).start()
        root.after(200, self.poll)

    def read(self):
        for raw in self.proc.stdout:
            self.lines.put(raw.decode("utf-8", "replace").strip())
        self.lines.put(None)

    def poll(self):
        ended = False
        while True:
            try:
                line = self.lines.get_nowait()
            except queue.Empty:
                break
            if line is None:
                ended = True
                break
            try:
                d = json.loads(line)
            except ValueError:
                continue
            self.msg = d.get("msg") or self.msg
            shown = self.msg
            if shown.lower().startswith(self.title.lower() + ": "):
                shown = shown[len(self.title) + 2:]          # "3 of 6 scenes", under the title
            self.label.configure(text=shown[:1].upper() + shown[1:])
            self.bar.configure(value=max(0, min(100, int(d.get("pct", 0)))))
        if ended:
            self.finish(self.proc.wait())
        elif self.top.winfo_exists():
            self.root.after(200, self.poll)

    def stop(self):
        """Stop the job and everything it started (the hidden game scenes)."""
        if self.proc.poll() is None:
            self.stopped = True
            subprocess.run(["taskkill", "/PID", str(self.proc.pid), "/T", "/F"],
                           capture_output=True, creationflags=CREATE_NO_WINDOW)

    def finish(self, code):
        if self.top.winfo_exists():
            self.top.grab_release()
            self.top.destroy()
        self.done(code, self.msg, self.stopped)


class KeysPanel:
    """The Controls tab's keyboard keys and sensitivity (item 23): every
    control with its keys, the defaults marked; click a control's keys and
    press a key or mouse button to change them, + to add one more, right-click
    to clear. Writes [keys]: only what differs from the defaults."""

    def __init__(self, app, frame, row, table, ini):
        self.app, self.root, self.table = app, app.root, table
        self.defaults = {c["name"]: list(c["keys"]) for c in table["controls"]}
        self.keys = {n: list(k) for n, k in self.defaults.items()}
        self.extra = {}                     # [keys] lines the launcher doesn't know: kept as written
        if ini.has_section("keys"):
            for name, val in ini.items("keys"):
                n = name.strip().lower()
                if n in self.keys:
                    self.keys[n] = [] if val.strip().lower() == "none" else \
                        [v.strip() for v in val.split(",") if v.strip()][:KEYS_MAX]
                elif n not in SENSITIVITY:
                    self.extra[name] = val
        self.capture = None
        box = ttk.Frame(frame)
        box.grid(row=row, column=0, columnspan=3, sticky="we", pady=(px(14), 0))
        box.columnconfigure(4, weight=1)          # spare room goes past the marks
        ttk.Label(box, text="Keyboard keys", style="Head.TLabel").grid(row=0, column=0, columnspan=4,
                                                                       sticky="w")
        note = ("Click a control's keys, then press a new key or click the box with a mouse "
                "button; Esc cancels. + adds another key, and a right click clears them.")
        if table.get("temporary"):
            note += " (This build lists the toolkit's standard keys.)"
        ttk.Label(box, text=note, style="Quiet.TLabel", wraplength=px(560)).grid(
            row=1, column=0, columnspan=4, sticky="w", pady=(0, px(6)))
        self.rows = {}
        r = 2
        for c in table["controls"]:
            lab = ttk.Label(box, text=c.get("label") or c["name"])
            lab.grid(row=r, column=0, sticky="w", padx=(0, px(16)), pady=px(1))
            btn = ttk.Button(box, width=28, style="Key.TButton",
                             command=lambda n=c["name"]: self.start(n, False))
            btn.grid(row=r, column=1, sticky="w", pady=px(1))
            btn.bind("<ButtonPress>", lambda e, n=c["name"]: self.on_click(n, e))
            add = ttk.Button(box, text="+", width=3, style="Key.TButton",
                             command=lambda n=c["name"]: self.start(n, True))
            add.grid(row=r, column=2, sticky="w", padx=(px(4), 0))
            mark = ttk.Label(box, text="", style="Quiet.TLabel")
            mark.grid(row=r, column=3, sticky="w", padx=(px(8), 0))
            self.rows[c["name"]] = (btn, add, mark)
            r += 1
        ttk.Button(box, text="Default keys", command=self.reset_keys).grid(
            row=r, column=0, sticky="w", pady=(px(6), 0))
        r += 1
        ttk.Label(box, text="Sensitivity", style="Head.TLabel").grid(row=r, column=0, columnspan=4,
                                                                     sticky="w", pady=(px(14), 0))
        r += 1
        self.sens = {}
        labels = {
            "ramp_ms": ("Steering ramp", "ms", 50,
                        "How long a held steering key takes to reach a full turn; 0 is "
                        "instant."),
            "strength_pct": ("Key strength", "%", 5,
                             "How far a key moves the stick; 100% is all the way."),
            "deadzone_left": ("Left stick deadzone", "%", 1,
                              "How far a controller's left stick can move before the game "
                              "notices; 0% leaves it to the game."),
            "deadzone_right": ("Right stick deadzone", "%", 1,
                               "The same for the right stick."),
        }
        for k in SENSITIVITY:
            spec = table[k]
            text, unit, step, tip = labels[k]
            var = tk.IntVar(value=self.read_int(ini, k, spec))
            lab = ttk.Label(box, text=text)
            lab.grid(row=r, column=0, sticky="w", padx=(0, px(16)), pady=px(2))
            val = ttk.Label(box, text="")
            sc = ttk.Scale(box, from_=spec["min"], to=spec["max"], orient="horizontal", length=px(220),
                           command=lambda v, kk=k, st=step, vv=var, lb=val, un=unit:
                           self.slide(kk, v, st, vv, lb, un))
            sc.set(var.get())
            sc.grid(row=r, column=1, sticky="w", pady=px(2))
            val.grid(row=r, column=2, columnspan=3, sticky="w", padx=(px(8), 0))
            Tip(lab, tip)
            Tip(sc, tip)
            self.sens[k] = (var, sc, val, unit)
            self.slide(k, var.get(), step, var, val, unit)
            r += 1
        ttk.Button(box, text="Default sensitivity", command=self.reset_sensitivity).grid(
            row=r, column=0, sticky="w", pady=(px(6), 0))
        for n in self.keys:
            self.show(n)

    @staticmethod
    def read_int(ini, k, spec):
        try:
            v = int(ini.get("keys", k, fallback=spec["default"]))
        except ValueError:
            v = spec["default"]
        return max(spec["min"], min(spec["max"], v))

    def slide(self, k, v, step, var, lab, unit):
        v = int(round(float(v) / step) * step)
        var.set(v)
        extra = " (default)" if v == self.table[k]["default"] else ""
        lab.configure(text="%d %s%s" % (v, unit, extra))

    # ---- the key list ---------------------------------------------------------------
    def show(self, n):
        btn, add, mark = self.rows[n]
        keys = self.keys[n]
        btn.configure(text=", ".join(key_label(k) for k in keys) if keys else "(none)")
        add.configure(state="normal" if len(keys) < KEYS_MAX else "disabled")
        # Item 106: only a changed control is marked (20 rows of "default" were noise).
        mark.configure(text="" if same_keys(keys, self.defaults[n]) else "changed")

    def set_keys(self, n, keys):
        self.keys[n] = keys[:KEYS_MAX]
        self.show(n)

    # Tk's mouse buttons (2 is the middle one; the side buttons are 4 and 5,
    # or 8 and 9 in later Tk) as Windows' virtual-key codes.
    MOUSE_VK = {1: 1, 2: 4, 3: 2, 4: 5, 5: 6, 8: 5, 9: 6}

    def start(self, n, add):
        """Wait for the next key, or a mouse button clicked on the box, for
        control n. The window takes the keyboard meanwhile, so Space or Enter
        can't also press a button; a click anywhere else cancels."""
        self.stop()
        self.capture = (n, add)
        self.rows[n][0].configure(text="Press a key, or click here...")
        self.root.focus_set()
        self.root.bind("<KeyPress>", self.on_key)
        self.root.bind("<ButtonPress>", self.on_other_click)

    def stop(self):
        if self.capture:
            n = self.capture[0]
            self.capture = None
            self.root.unbind("<KeyPress>")
            self.root.unbind("<ButtonPress>")
            self.show(n)

    def take(self, vk):
        n, add = self.capture
        name = KEY_NAMES[vk][0]
        keys = [k for k in self.keys[n] if key_vk(k) != vk] + [name] if add else [name]
        self.stop()
        self.set_keys(n, keys)
        return "break"

    def on_key(self, e):
        if not self.capture:
            return None
        if e.keysym == "Escape":
            self.stop()
            return "break"
        if e.keycode in KEY_NAMES:
            return self.take(e.keycode)
        return "break"                      # a key the game can't read: keep waiting

    def on_click(self, n, e):
        """A mouse button on a control's keys: while waiting for that control,
        it's the new button; otherwise right-click clears, left-click starts."""
        if self.capture and self.capture[0] == n:
            vk = self.MOUSE_VK.get(e.num)
            return self.take(vk) if vk else "break"
        if e.num == 3:
            self.stop()
            self.set_keys(n, [])
            return "break"
        return None

    def on_other_click(self, e):
        if self.capture and e.widget is not self.rows[self.capture[0]][0]:
            self.stop()

    def reset_keys(self):
        self.stop()
        for n in self.keys:
            self.set_keys(n, list(self.defaults[n]))

    def reset_sensitivity(self):
        for k, (var, sc, val, unit) in self.sens.items():
            sc.set(self.table[k]["default"])

    def defaults_all(self):
        self.reset_keys()
        self.reset_sensitivity()
        self.extra = {}

    # ---- the file ----------------------------------------------------------------------
    def lines(self):
        """The [keys] lines that differ from the defaults, in the toolkit's order."""
        out = []
        for c in self.table["controls"]:
            n = c["name"]
            if not same_keys(self.keys[n], self.defaults[n]):
                out.append((n, ", ".join(self.keys[n]) if self.keys[n] else "none"))
        for k in SENSITIVITY:
            v = self.sens[k][0].get()
            if v != self.table[k]["default"]:
                out.append((k, str(v)))
        out += list(self.extra.items())
        return out


# ── Cheats (item 24; the [cheats] contract, docs/025/CONTRACTS.md 3) ───────
# The cheat dword that rides the launch data (docs/GAME.md, "The cheat bits").
# (bits, name, what it does, editions: "all" or "remastered" = not Original)
CHEATS = [
    (0x001, "Unlimited ammo", "Weapons never run out.", "all"),
    (0x002, "Unlimited turbo", "Turbo never runs out.", "all"),
    (0x004, "Invincibility", "Your boat takes no damage.", "all"),
    (0x010, "All boats", "Every boat to choose from, without unlocking them.", "all"),
    (0x020, "All battle modes", "Every Battle Mode game.", "all"),
    (0x040, "All arenas", "Every Battle Mode arena.", "all"),
    (0x080 | 0x400, "Rubber duck", "Rubber-duck boats in multiplayer battles, and a hidden "
                                   "game against them.", "all"),
    (0x100, "Import boat", "Adds a tuner-style speedboat to the boats.", "all"),
    (0x200, "Blood Ball", "A ball game for multiplayer battles.", "all"),
    (0x800, "Puffer Fish", "Adds an explosive sampan for multiplayer battles.", "all"),
    (0x008, "All missions", "Every Story Mode mission and act to choose from. Saving is off "
                            "while this is on.", "remastered"),
]


class CheatsPanel:
    """The Cheats tab (item 24): a Cheats on switch above one box per cheat.
    Off greys the boxes but keeps their ticks, so a set survives being
    switched off and on. Writes [cheats] enabled = 0|1, bits = 0x...; off
    and nothing ticked writes nothing (a missing section means off)."""

    def __init__(self, app, frame, ini):
        self.app = app
        try:
            bits = int(ini.get("cheats", "bits", fallback="0"), 0)
        except ValueError:
            bits = 0
        on = ini.get("cheats", "enabled", fallback="0").strip() == "1"
        self.known = 0
        for b, *_rest in CHEATS:
            self.known |= b
        self.unknown = bits & ~self.known           # bits the launcher doesn't list: kept
        self.on = tk.BooleanVar(value=on)
        ttk.Label(frame, text="Cheats", style="Head.TLabel").pack(anchor="w")
        ttk.Label(frame, text="The game's own cheat codes, without typing them at the title "
                              "screen. Switching cheats off keeps your ticks for next time.",
                  style="Quiet.TLabel", wraplength=px(560)).pack(anchor="w", pady=(0, px(8)))
        sw = ttk.Checkbutton(frame, text="Cheats on", variable=self.on, command=self.refresh)
        sw.pack(anchor="w", pady=(0, px(6)))
        self.switch = sw
        self.boxes = []
        for b, name, text, eds in CHEATS:
            var = tk.BooleanVar(value=(bits & b) == b)
            row = ttk.Frame(frame)
            row.pack(anchor="w", fill="x", padx=(px(20), 0), pady=px(1))
            cb = ttk.Checkbutton(row, text=name, variable=var)
            cb.pack(side="left")
            note = ttk.Label(row, text="", style="Quiet.TLabel")
            note.pack(side="left", padx=(px(8), 0))
            desc = ttk.Label(frame, text=text, wraplength=px(520))
            desc.pack(anchor="w", padx=(px(44), 0), pady=(0, px(4)))
            Tip(cb, text)
            self.boxes.append((b, eds, var, cb, note, desc))
        self.refresh()

    def refresh(self):
        """Grey the boxes when cheats are off, and the not-in-Original one in
        Original; the ticks stay either way."""
        ed = self.app.edition() if "edition" in self.app.vars else "original"
        for b, eds, var, cb, note, desc in self.boxes:
            edition_ok = eds == "all" or ed != "original"
            cb.configure(state="normal" if self.on.get() and edition_ok else "disabled")
            note.configure(text="" if edition_ok else "(%s only)" % APPLIES["remastered"])
            desc.configure(foreground=C["text"] if self.on.get() and edition_ok else C["off"])

    def bits(self):
        out = self.unknown
        for b, eds, var, cb, note, desc in self.boxes:
            if var.get():
                out |= b
        return out

    def defaults_all(self):
        """Restore defaults: cheats off (the ticks are kept, as the switch keeps them)."""
        self.on.set(False)
        self.refresh()

    def lines(self):
        bits = self.bits()
        if not self.on.get() and not bits:
            return []
        return [("enabled", "1" if self.on.get() else "0"), ("bits", "0x%03x" % bits)]


class Launcher:
    def __init__(self, root, schema):
        self.root, self.schema = root, schema
        self.path = schema.get("file") or os.path.join(
            os.environ.get("LOCALAPPDATA", REPO), "BloodWake", "settings.ini")
        self.settings = schema["settings"]
        self.vars, self.widgets, self.proc, self.busy = {}, {}, None, False
        self.keys_panel = self.cheats_panel = None
        # Remastered's presets (D-021). A setting the file doesn't hold yet (a
        # new install) starts at the default preset's choice (Best Quality).
        self.presets = schema.get("presets", [])
        self.pst = next((s for s in self.settings
                         if s["section"] == "edition" and s["key"] == "preset"), None)
        first = next((p for p in self.presets if self.pst and p["value"] == self.pst["default"]),
                     None)
        self.first = dict(first["settings"]) if first else {}
        self.ini = configparser.ConfigParser(interpolation=None)
        self.ini.optionxform = str
        try:
            self.ini.read(self.path, encoding="utf-8")
        except (OSError, configparser.Error):
            pass
        # The file's format: 2 from 0.2.5's launcher on ([launcher] format), 1
        # before (0.2.4 wrote no such line). See saved() for what it changes.
        try:
            self.format = int(self.ini.get("launcher", "format", fallback="1"))
        except ValueError:
            self.format = 1

        root.title("Blood Wake")
        # Item 20: the banner's band on top, as Setup has it (docs/025/STYLE.md).
        Banner(root, px(84)).pack(side="top", fill="x")
        head = ttk.Frame(root, padding=(px(16), px(8), px(16), 0), style="Bg.TFrame")
        head.pack(side="top", fill="x")
        sub = ttk.Label(head, text="Choose your settings and press Play. Hover over a setting "
                                   "for details.",
                        style="Status.TLabel", wraplength=px(740))
        sub.pack(anchor="w", fill="x")
        head.bind("<Configure>", lambda e: sub.configure(wraplength=max(px(200), e.width - px(32))))

        # The button row is packed before the tabs, at the bottom, so however
        # small the window gets it keeps its place: Tk takes the room away
        # from whatever was packed last, which is now the tabs (they scroll).
        foot = ttk.Frame(root, padding=(px(12), px(8), px(12), px(10)), style="Bg.TFrame")
        foot.pack(side="bottom", fill="x")
        self.play_btn = ttk.Button(foot, text="Play", style="Play.TButton", command=self.play)
        self.play_btn.pack(side="right")
        self.defaults_btn = ttk.Button(foot, text="Restore defaults", command=self.defaults)
        self.defaults_btn.pack(side="right", padx=(0, px(8)))
        self.status = ttk.Label(foot, text="", style="Status.TLabel")
        self.status.pack(side="left", fill="x", expand=True)

        self.nb = ttk.Notebook(root)
        self.nb.pack(side="top", fill="both", expand=True, padx=px(12), pady=(px(8), 0))
        self.pages = []
        for tab in schema["tabs"]:
            page = Scroll(self.nb, padding=px(12))
            self.nb.add(page, text=tab)
            self.pages.append(page)
            if tab == "Edition":
                self.build_edition(page.body)
            else:
                row = self.build_tab(page.body, tab)
                if tab == "Controls":               # item 23: keys and sensitivity below
                    self.keys_panel = KeysPanel(self, page.body, row,
                                                load_keys_schema(schema), self.ini)
                if tab == "Graphics":               # item 97: what the screen reports
                    # Filled once the window is placed (the game is asked
                    # about the screen under the window's centre).
                    self.screen_label = ttk.Label(page.body, text="", wraplength=px(560),
                                                  style="Quiet.TLabel")
                    self.screen_label.grid(row=row, column=0, columnspan=3, sticky="w",
                                           pady=(px(12), 0))
                    self.screen_warn = ttk.Label(page.body, text="", wraplength=px(560),
                                                 style="Warn.TLabel")
                    self.screen_warn.grid(row=row + 1, column=0, columnspan=3, sticky="w",
                                          pady=(px(4), 0))
        page = Scroll(self.nb, padding=px(12))      # item 24: the Cheats tab, before Advanced
        self.nb.add(page, text="Cheats")
        self.pages.append(page)
        self.cheats_panel = CheatsPanel(self, page.body, self.ini)
        self.build_advanced()
        root.bind_all("<MouseWheel>", self.wheel)
        self.match_preset()
        self.refresh()
        self.place_window()
        dark_title_bar(root)
        if self.safe_offer():
            self.status.configure(text="The last two starts didn't reach the title screen: Play "
                                       "will offer safe mode.")
        # Item 97: a launcher that was ended while Automatic HDR had Windows HDR on
        # left a marker: put Windows HDR back as it was. Then ask the game
        # about the screen under the window (after that, so it reads the
        # screen as it really is).
        done = winhdr_restore(os.path.dirname(self.path))
        self.screen = screen_info(schema, *self.window_centre())
        self.update_screen_line()
        if done == "restored":
            self.status.configure(text="Windows HDR is back as it was before the last session.")
        elif done == "failed":
            self.status.configure(text="Couldn't put Windows HDR back as it was: switch it in "
                                       "Windows' display settings.")

    def place_window(self):
        """Open at the size the tabs need, shrunk to fit the screen's work area
        (a 1366x768 laptop, or 1080p at 150%), centred on it. The minimum
        size fits too; anything taller than the window scrolls."""
        for page in self.pages:
            page.natural()
        self.root.update_idletasks()
        want_w = max(px(800), self.root.winfo_reqwidth())
        # The tallest tab's height, up to a comfortable window: a longer tab
        # (the Controls tab's key list) scrolls rather than stretching the
        # window for every tab.
        want_h = min(self.root.winfo_reqheight(), px(MAX_OPEN_H))
        # Item 106: never narrower than the tab row (at 560 pixels the tab
        # names were cut short, at every display scaling).
        min_w, min_h = max(px(560), self.tabs_width()), px(400)
        area = work_area()
        if area:
            x0, y0, aw, ah = area
            # The window's title bar and borders, which the size given to Tk
            # leaves out: Windows' own figure for this display scaling (with
            # its invisible resize borders, so it errs large), plus slack.
            fw, fh = frame_size()
            fw, fh = fw + px(FIT_SLACK), fh + px(FIT_SLACK)
            want_w, want_h = min(want_w, aw - fw), min(want_h, ah - fh)
            min_w, min_h = min(min_w, want_w), min(min_h, want_h)
            x = x0 + max(0, (aw - fw - want_w) // 2)
            y = y0 + max(0, (ah - fh - want_h) // 2)
            self.root.geometry("%dx%d+%d+%d" % (want_w, want_h, x, y))
            # Once it's on screen, measure the real frame and shrink once if
            # it still sticks out (a frame can differ from Windows' figure,
            # for example while the display scaling is being changed).
            self.root.after(150, lambda: self.refit(area))
        else:
            self.root.geometry("%dx%d" % (want_w, min(want_h, px(680))))
        self.root.minsize(min_w, min_h)

    def tabs_width(self):
        """The tab row's natural width with the notebook's side margins: each
        name in the tab font, plus the tab's padding (TNotebook.Tab, 10 a side)
        and border, plus slack for the selected tab, which clam draws wider."""
        try:
            font = tkfont.nametofont("TkDefaultFont")
            n = self.nb.index("end")
            row = sum(font.measure(self.nb.tab(i, "text")) + px(20) + 2 for i in range(n))
        except tk.TclError:
            return 0
        return row + 2 * px(12) + px(8)

    def refit(self, area):
        x0, y0, aw, ah = area
        try:
            self.root.update_idletasks()
            l, t, r, b = visible_rect(int(self.root.wm_frame(), 16))
        except (tk.TclError, ValueError, OSError):
            return
        cw, ch = self.root.winfo_width(), self.root.winfo_height()
        fw, fh = (r - l) - cw, (b - t) - ch          # the frame as it really is
        if fw < 0 or fh < 0 or (r - l) <= 1:
            return
        slack = px(FIT_SLACK)
        nw, nh = min(cw, aw - fw - slack), min(ch, ah - fh - slack)
        if (nw, nh) == (cw, ch) and l >= x0 and t >= y0 and r <= x0 + aw and b <= y0 + ah:
            return
        # The top-left the size given to Tk is placed by, against the visible
        # frame's (Windows' invisible borders sit between them).
        dx, dy = self.root.winfo_x() - l, self.root.winfo_y() - t
        nx = x0 + max(0, (aw - fw - nw) // 2) + dx
        ny = y0 + max(0, (ah - fh - nh) // 2) + dy
        mw, mh = self.root.minsize()
        self.root.minsize(min(mw, nw), min(mh, nh))
        self.root.geometry("%dx%d+%d+%d" % (nw, nh, nx, ny))

    def wheel(self, e):
        """The mouse wheel scrolls the tab under it (a long tab on a small
        screen), except in the Advanced box and an open list, which scroll
        themselves."""
        try:
            cls = self.root.tk.call("winfo", "class", str(e.widget))
        except tk.TclError:
            return None
        if cls in ("Text", "Listbox"):
            return None
        page = self.nb.nametowidget(self.nb.select())
        if isinstance(page, Scroll):
            page.wheel(e)
        return "break"

    def no_wheel(self, box):
        """A choice box: the wheel scrolls the page instead of changing the
        choice by accident."""
        box.bind("<MouseWheel>", self.wheel)

    # ---- building the tabs ---------------------------------------------------
    def saved(self, st):
        val = self.ini.get(st["section"], st["key"], fallback=None)
        values = [o["value"] for o in st["options"]]
        # Calls 85 and 89: 0.2.4 wrote its defaults into every file, so in a
        # file from before 0.2.5 an old default reads as 0.2.5's, as the game
        # reads it (src/bw_settings.c, s_old_default); a choice made since stays.
        was_now = OLD_DEFAULTS.get((st["section"], st["key"]))
        if was_now and val == was_now[0] and self.format < SETTINGS_FORMAT and was_now[1] in values:
            val = was_now[1]
        return val if val in values else self.first.get(st["section"] + "." + st["key"],
                                                        st["default"])

    @staticmethod
    def label_of(st, value):
        return next((o["label"] for o in st["options"] if o["value"] == value), value)

    def current(self, key):
        st, var = self.vars[key]
        return next((o["value"] for o in st["options"] if o["label"] == var.get()), None)

    def build_edition(self, frame):
        st = next(s for s in self.settings if s["key"] == "edition")
        var = tk.StringVar(value=self.saved(st))
        self.vars["edition"] = (st, var)
        ttk.Label(frame, text="Edition", style="Head.TLabel").pack(anchor="w")
        for o in st["options"]:
            rb = ttk.Radiobutton(frame, text=o["label"], value=o["value"], variable=var,
                                 command=self.refresh)
            rb.pack(anchor="w", pady=(px(10), 0))
            ttk.Label(frame, text=EDITION_TEXT.get(o["value"], ""),
                      wraplength=px(520)).pack(anchor="w", padx=px(24))
        copy = install_record().get("copy") or {}
        if copy.get("verdict") in COPY_TEXT:            # contract 9
            word, text = COPY_TEXT[copy["verdict"]]
            when = " (%s)" % str(copy["accepted"]).replace("T", " ")[:16] if copy.get("accepted") else ""
            self.copy_label = ttk.Label(frame, text="! " + word, style="Warn.TLabel")
            self.copy_label.pack(anchor="w", pady=(px(14), 0))
            ttk.Label(frame, text=text % when, wraplength=px(520)).pack(anchor="w", padx=px(24))
        if self.pst and self.presets:
            st = self.pst
            lab = ttk.Label(frame, text=st["label"], style="Head.TLabel")
            lab.pack(anchor="w", pady=(px(18), 0))
            row = ttk.Frame(frame)
            row.pack(anchor="w", pady=(6, 0))
            var = tk.StringVar()
            box = ttk.Combobox(row, values=[self.label_of(st, p["value"]) for p in self.presets],
                               textvariable=var, state="readonly", width=24)
            box.pack(side="left")
            box.bind("<<ComboboxSelected>>", self.pick_preset)
            self.no_wheel(box)
            note = ttk.Label(row, text="", style="Quiet.TLabel")
            note.pack(side="left", padx=px(8))
            tip = st["tip"] + "\n\nApplies to: %s." % APPLIES.get(st["editions"], st["editions"])
            Tip(lab, tip)
            Tip(box, tip)
            self.preset_text = ttk.Label(frame, text="", wraplength=px(520))
            self.preset_text.pack(anchor="w", padx=px(24), pady=(px(4), 0))
            self.vars["edition.preset"] = (st, var)
            self.widgets["edition.preset"] = (lab, box, note)
        ttk.Label(frame, text="\nEach edition keeps its own saved games. The first time you play "
                             "%s, it starts with a copy of your Original saved games." % LATER,
                  wraplength=px(540), style="Quiet.TLabel").pack(anchor="w")

    def build_tab(self, frame, tab):
        frame.columnconfigure(1, weight=1)
        row = 0
        for st in self.settings:
            if st["tab"] != tab:
                continue
            labels = [o["label"] for o in st["options"]]
            cur = self.saved(st)
            var = tk.StringVar(value=next(o["label"] for o in st["options"] if o["value"] == cur))
            lab = ttk.Label(frame, text=st["label"])
            lab.grid(row=row, column=0, sticky="w", pady=px(4), padx=(0, px(16)))
            vals = [o["value"] for o in st["options"]]
            if st.get("kind") == "check" and sorted(vals) == ["off", "on"]:
                # Item 100: an on/off setting the table marks as a checkbox.
                on = next(o["label"] for o in st["options"] if o["value"] == "on")
                off = next(o["label"] for o in st["options"] if o["value"] == "off")
                box = ttk.Checkbutton(frame, variable=var, onvalue=on, offvalue=off,
                                      command=self.match_preset)
            else:
                box = ttk.Combobox(frame, values=labels, textvariable=var, state="readonly", width=30)
                box.bind("<<ComboboxSelected>>", self.match_preset)
                self.no_wheel(box)
            box.grid(row=row, column=1, sticky="w", pady=px(4))
            note = ttk.Label(frame, text="", style="Quiet.TLabel")
            note.grid(row=row, column=2, sticky="w", padx=px(8))
            tip = st["tip"] + "\n\nApplies to: %s." % APPLIES.get(st["editions"], st["editions"])
            Tip(lab, tip)
            Tip(box, tip)
            self.vars[st["section"] + "." + st["key"]] = (st, var)
            self.widgets[st["section"] + "." + st["key"]] = (lab, box, note)
            row += 1
        if row == 0 and tab != "Controls":
            ttk.Label(frame, text="Nothing here yet.").grid(row=0, column=0)
        return row

    def build_advanced(self):
        page = Scroll(self.nb, padding=px(12))
        self.nb.add(page, text="Advanced")
        self.pages.append(page)
        frame = page.body
        ttk.Label(frame, text="Extra switches for testing, one NAME=value per line. Leave this "
                             "empty if unsure.",
                  wraplength=px(560)).pack(anchor="w")
        self.adv = tk.Text(frame, height=10, width=70, font=("Consolas", 9), background=C["input"],
                           foreground=C["text"], insertbackground=C["text"], relief="flat",
                           borderwidth=0, highlightthickness=1, highlightbackground=C["border"],
                           highlightcolor=C["focus"], selectbackground=SEA_500,
                           selectforeground=TITLE_WHITE, padx=px(6), pady=px(4))
        self.adv.pack(fill="both", expand=True, pady=px(8))
        if self.ini.has_section("advanced"):
            self.adv.insert("1.0", "\n".join("%s=%s" % kv for kv in self.ini.items("advanced")))
        acts = ttk.Frame(frame)
        acts.pack(fill="x", pady=(0, px(8)))
        self.warm_btn = ttk.Button(acts, text="Prepare graphics (about 2 minutes)",
                                   command=self.prepare_graphics)
        self.warm_btn.pack(side="left")
        Tip(self.warm_btn, "Plays a few short scenes, hidden and silent, so the game doesn't "
                           "stutter the first time it draws something new. Worth doing after "
                           "changing anti-aliasing, sharp text, smooth HUD edges or texture "
                           "restoration.")
        if bw_warmup is None or not os.path.isfile(WARMUP):
            self.warm_btn.configure(state="disabled")
        safe = ttk.Button(acts, text="Start in safe mode", command=self.start_safe)
        safe.pack(side="left", padx=(px(8), 0))
        Tip(safe, "For when the game won't start: plays Original in a window with nothing "
                  "extra, not even the switches above. Your settings are kept.")
        report = ttk.Button(acts, text="Make a bug report", command=self.bug_report)
        report.pack(side="left", padx=(px(8), 0))
        Tip(report, "Saves a zip to your desktop with the game's log, your settings and a "
                    "summary of this PC, for whoever is helping you. Nothing from the game "
                    "goes in it.")
        btns = ttk.Frame(frame)
        btns.pack(fill="x")
        ttk.Button(btns, text="Open the settings folder",
                   command=lambda: self.open_folder(os.path.dirname(self.path))).pack(side="left")
        ttk.Button(btns, text="Open the saved games folder",
                   command=self.open_saves).pack(side="left", padx=px(8))
        ttk.Button(btns, text="Open the game's log",
                   command=lambda: self.open_file(os.path.join(REPO, "play.log"))).pack(side="left")
        ttk.Label(frame, text="\nSettings file: %s" % self.path, style="Quiet.TLabel",
                  wraplength=px(560)).pack(anchor="w")

    # ---- state ------------------------------------------------------------------
    def edition(self):
        return self.vars["edition"][1].get()

    def refresh(self):
        ed = self.edition()
        for key, (lab, box, note) in self.widgets.items():
            st = self.vars[key][0]
            ok = (st["editions"] == "all" or (st["editions"] == "remastered" and ed != "original")
                  or st["editions"] == ed)          # a setting for one edition only
            un = st.get("unavailable")                # contract 7: off on this copy
            live = "readonly" if isinstance(box, ttk.Combobox) else "normal"   # a checkbox (item 100)
            box.configure(state=live if ok and not un else "disabled")
            lab.configure(foreground=C["text"] if ok and not un else C["off"])
            note.configure(text=un if un else "" if ok else "(%s only)" % APPLIES[st["editions"]])
        if self.cheats_panel:
            self.cheats_panel.refresh()             # the not-in-Original cheat
        self.update_screen_line()                   # item 106: none in Original

    def defaults(self):
        for key, (st, var) in self.vars.items():
            if key == "edition":
                continue
            var.set(self.label_of(st, self.first.get(key, st["default"])))
        self.adv.delete("1.0", "end")
        if self.keys_panel:
            self.keys_panel.defaults_all()          # item 23: the [keys] section goes
        if self.cheats_panel:
            self.cheats_panel.defaults_all()        # item 24: cheats off, ticks kept
        self.match_preset()
        self.status.configure(text="Defaults restored (the edition is kept).")
        self.refresh()

    def pick_preset(self, _e=None):
        """A preset was picked: set every setting it names."""
        st, var = self.vars["edition.preset"]
        p = next((p for p in self.presets if self.label_of(st, p["value"]) == var.get()), None)
        for key, value in (p["settings"].items() if p else ()):
            if key in self.vars:
                var2 = self.vars[key][1]
                var2.set(self.label_of(self.vars[key][0], value))
        self.match_preset()
        self.status.configure(text="%s: the Display and Graphics tabs are set." % var.get())

    def window_centre(self):
        """The launcher window's centre on the desktop, or (None, None)."""
        import re
        m = re.match(r"(\d+)x(\d+)([+-]-?\d+)([+-]-?\d+)", self.root.geometry())
        if not m:
            return None, None
        w, h, x, y = (int(g) for g in m.groups())
        return x + w // 2, y + h // 2

    def update_screen_line(self):
        """The Graphics tab's line about the screen (what Automatic HDR would
        do here) and the calibration-profile warning (items 97 and 100)."""
        lab, warn = getattr(self, "screen_label", None), getattr(self, "screen_warn", None)
        if lab is None:
            return
        line = screen_line(getattr(self, "screen", None))
        cap = profile_warning(getattr(self, "screen", None))
        if "edition" in self.vars and self.edition() == "original":
            line = cap = None           # item 106: Automatic HDR is greyed in Original

        lab.configure(text=line or "")
        warn.configure(text=("! " + cap) if cap else "")
        for w, text in ((lab, line), (warn, cap)):
            if text:
                w.grid()
            else:
                w.grid_remove()

    def match_preset(self, _e=None):
        """Show the preset the settings match, or Custom."""
        self.update_screen_line()                   # item 97: it follows the HDR choices
        if "edition.preset" not in self.vars:
            return
        st, var = self.vars["edition.preset"]
        hit = next((p for p in self.presets
                    if all(k in self.vars and self.current(k) == v for k, v in p["settings"].items())),
                   None)
        var.set(self.label_of(st, hit["value"] if hit else "custom"))
        self.preset_text.configure(text=hit["text"] if hit else
                                   "Your own mix: a setting was changed after picking a preset.")

    def save(self):
        cp = configparser.ConfigParser(interpolation=None)
        cp.optionxform = str
        for key, (st, var) in self.vars.items():
            if key == "edition":
                value = var.get()
            else:
                value = next(o["value"] for o in st["options"] if o["label"] == var.get())
            if not cp.has_section(st["section"]):
                cp.add_section(st["section"])
            cp.set(st["section"], st["key"], value)
        adv = self.adv_lines()
        if adv:
            cp.add_section("advanced")
            for k, v in adv:
                cp.set("advanced", k, v)
        # The launcher's own sections beyond the table: [keys] (item 23).
        for sec, lines in self.own_sections():
            if lines:
                cp.add_section(sec)
                for k, v in lines:
                    cp.set(sec, k, v)
        # Sections the launcher doesn't know (a newer game's, or written by
        # hand) are kept as they were.
        owned = {st["section"] for st in self.settings} | {"advanced"} | {s for s, _l in
                                                                          self.own_sections()}
        for sec in self.ini.sections():
            if sec.lower() not in owned and not cp.has_section(sec):
                cp.add_section(sec)
                for k, v in self.ini.items(sec):
                    cp.set(sec, k, v)
        os.makedirs(os.path.dirname(self.path), exist_ok=True)
        with open(self.path, "w", encoding="utf-8") as f:
            f.write("; Blood Wake settings, written by the launcher (tools/launcher.pyw).\n"
                    "; Every boot of the game reads this file; environment variables win.\n\n")
            cp.write(f)

    def own_sections(self):
        """The launcher's sections beyond the table and [advanced], each as
        (name, [(key, value), ...]); an empty list means the section goes."""
        out = [("launcher", [("format", str(SETTINGS_FORMAT))])]   # 0.2.5's files on
        if self.keys_panel:
            out.append(("keys", self.keys_panel.lines()))
        if self.cheats_panel:
            out.append(("cheats", self.cheats_panel.lines()))
        return out

    def adv_lines(self):
        """The Advanced tab's switches as [(NAME, value), ...]."""
        lines = [l.strip() for l in self.adv.get("1.0", "end").splitlines()]
        return [(k.strip(), v.strip()) for k, v in
                (l.split("=", 1) for l in lines if "=" in l and not l.startswith((";", "#")))]

    # ---- the graphics warm-up (item 21) ---------------------------------------------
    def cache_folder(self):
        """The shader cache folder the game will use: the Advanced tab's
        BW_GPU_PSCACHE if it sets one (None for 0: no cache), else the game's
        own (src/gpu/bw_pscache.c)."""
        if bw_warmup is None:
            return None
        v = dict(self.adv_lines()).get("BW_GPU_PSCACHE")
        if v is not None:
            return None if v == "0" else v
        return bw_warmup.cache_dir(None)

    def shader_signature(self):
        """What the shaders depend on, as the window now stands; None in
        Original (its shaders never change) or if the game names nothing."""
        if bw_warmup is None or self.edition() == "original":
            return None
        values = {k: self.current(k) for k in self.vars if k != "edition"}
        return bw_warmup.shader_signature(self.schema, values)

    def warmup_offer(self):
        """The signature to offer a warm-up for when Play is pressed, or None:
        only when the cache has a warm-up record (Setup leaves one), that
        record lacks these settings, and the player hasn't said Not now to
        them already."""
        sig, folder = self.shader_signature(), self.cache_folder()
        if not sig or not folder or not os.path.isfile(WARMUP):
            return None
        st = bw_warmup.read_stamp(folder)
        if not st["warmed"] or sig in st["warmed"] or sig in st["declined"]:
            return None
        return sig

    def ask(self, title, text, yes, no):
        return Ask(self.root, title, text, yes, no).ask()

    def run_warmup(self, then=None):
        """Prepare graphics with the saved settings (tools/bw_warmup.py, as
        Setup runs it), in a progress window. then(): what to do afterwards,
        whether it finished or the player stopped it (start the game)."""
        cmd = [python_console(), WARMUP, "--exe", EXE, "--game-dir", GAME_DIR, "--progress", "json"]
        v = dict(self.adv_lines()).get("BW_GPU_PSCACHE")
        if v:
            cmd += ["--cache", v]
        self.busy = True

        def done(code, msg, stopped):
            self.busy = False
            if stopped:
                self.status.configure(text="Preparing graphics stopped; the game prepares the "
                                           "rest as it plays.")
            elif code == 0 and msg.startswith("graphics ready"):
                self.status.configure(text=msg[:1].upper() + msg[1:].split(" (")[0] + ".")
            else:
                self.status.configure(text="Preparing graphics didn't finish; the game prepares "
                                           "the rest as it plays.")
            if then:
                then()

        try:
            Progress(self.root, "Preparing graphics", cmd, "Play now" if then else "Stop", done)
        except OSError as e:
            self.busy = False
            messagebox.showerror("Blood Wake", "Couldn't prepare graphics:\n%s" % e)
            if then:
                then()

    def prepare_graphics(self):
        """The Advanced tab's button."""
        if self.busy or (self.proc and self.proc.poll() is None):
            return
        if game_running(self.adv_lines()):
            # Its scenes are the game itself: one at a time (they share the
            # console's disk images with a running Original game).
            self.status.configure(text="Close Blood Wake first: preparing graphics plays short "
                                       "scenes of the game.")
            return
        try:
            self.save()
        except OSError as e:
            messagebox.showerror("Blood Wake", "Couldn't save the settings:\n%s" % e)
            return
        self.run_warmup()

    # ---- actions ------------------------------------------------------------------
    def play(self):
        if self.busy or (self.proc and self.proc.poll() is None):
            return
        if game_running(self.adv_lines()):          # contract 1: one game at a time
            bring_game_to_front()
            self.status.configure(text="Blood Wake is already running: its window is in front.")
            return
        try:
            self.save()
        except OSError as e:
            messagebox.showerror("Blood Wake", "Couldn't save the settings:\n%s" % e)
            return
        if self.safe_offer():
            if self.ask("Start in safe mode?",
                        "Blood Wake didn't reach its title screen the last two times. Safe mode "
                        "plays Original in a window with nothing extra; your settings are kept "
                        "for next time.\n\nIf it still won't start, use Make a bug report on the "
                        "Advanced tab.",
                        "Start in safe mode", "Start normally"):
                self.start_game(safe=True)
                return
        sig = self.warmup_offer()
        if sig:
            if self.ask("Prepare graphics first?",
                        "Your new graphics settings change how the game draws. Preparing "
                        "graphics now takes about 2 minutes and saves small stutters in play; "
                        "then the game starts.\n\nNot now plays straight away and won't ask "
                        "again for these settings.",
                        "Prepare graphics", "Not now"):
                self.run_warmup(then=self.start_game)
                return
            bw_warmup.add_stamp(self.cache_folder(), "declined", sig)
        self.start_game()

    def start_game(self, safe=False):
        cmd = [python_console(), PLAY]
        if safe:
            # Safe mode's own file, beside the player's (so the start record,
            # CONTRACTS.md 4, is the same one); the player's file is untouched.
            path = os.path.join(os.path.dirname(self.path), SAFE_FILE)
            try:
                write_safe_settings(path, dict(self.adv_lines()).get("BW_TEST_MUTEX"))
            except OSError as e:
                messagebox.showerror("Blood Wake", "Couldn't write the safe-mode settings:\n%s" % e)
                return
            cmd += ["--settings", path, "--edition", "original"]
        note = None
        if not safe:                                # item 97: HDR for this session
            sdr, scr, note = self.hdr_plan()
            if sdr:
                path = self.session_settings(hdr="off")
                if path:
                    cmd += ["--settings", path]
            if scr and not self.winhdr_on(scr):
                note = "Couldn't switch Windows HDR on; playing with Windows' own setting."
        self.status.configure(text="Starting in safe mode..." if safe else note or "Starting...")
        self.started = time.time()
        try:
            self.proc = subprocess.Popen(cmd, cwd=REPO, creationflags=CREATE_NO_WINDOW)
        except OSError as e:                        # item 4: Smart App Control, perhaps
            self.proc = None
            winhdr_restore(os.path.dirname(self.path))
            if smart_app_control_on():
                self.sac_help()
            else:
                self.status.configure(text="")
                messagebox.showerror("Blood Wake", start_failure_text("Couldn't start the game", e))
            return
        self.root.after(1500, self.root.withdraw)
        self.root.after(2000, self.watch)

    # ---- HDR for a session (item 97) ------------------------------------------
    def hdr_plan(self):
        """For this session: (play in SDR, the screen to switch Windows HDR on
        for or None, a line for the status). Only with Automatic HDR ticked
        (item 100) and where it applies (not Original): a screen that reports
        no HDR data plays in SDR; a screen that does HDR, reports its data and
        has Windows HDR off gets it switched on."""
        if self.edition() == "original" or self.current("graphics.hdr") != "on":
            return False, None, None
        info = screen_info(self.schema, *self.window_centre())
        if info and info.get("source") == "none":
            return True, None, "This screen doesn't report HDR data: playing in SDR."
        if not info or not info.get("hdr_capable"):
            return False, None, None
        scr = winhdr_screen(info.get("device"))
        if not scr or not scr.get("supported") or scr.get("on"):
            return False, None, None
        return False, scr, "Windows HDR is on while you play; it goes back off when the game closes."

    def winhdr_on(self, scr):
        """Switch Windows HDR on for this session, writing the marker first, so
        a launcher that is ended puts it back at its next start."""
        folder = os.path.dirname(self.path)
        try:
            with open(os.path.join(folder, WINHDR_MARKER), "w", encoding="utf-8") as f:
                json.dump({"device": scr.get("device"), "was_on": bool(scr.get("on")),
                           "since": time.strftime("%Y-%m-%d %H:%M:%S")}, f)
        except OSError:
            return False                            # no marker, no switch
        if winhdr_set(scr, True):
            return True
        winhdr_restore(folder)
        return False

    def session_settings(self, hdr):
        """The settings file for one session, beside the player's (so the
        start record is the same one): the saved settings with HDR as given.
        The player's own file is untouched."""
        cp = configparser.ConfigParser(interpolation=None, strict=False)
        cp.optionxform = str
        path = os.path.join(os.path.dirname(self.path), SESSION_FILE)
        try:
            cp.read(self.path, encoding="utf-8")
            if not cp.has_section("graphics"):
                cp.add_section("graphics")
            cp.set("graphics", "hdr", hdr)
            if not cp.has_section("launcher"):
                cp.add_section("launcher")
            cp.set("launcher", "format", str(SETTINGS_FORMAT))
            with open(path, "w", encoding="utf-8") as f:
                f.write("; Blood Wake: this session's settings, written by the launcher (your own "
                        "are in settings.ini).\n\n")
                cp.write(f)
            return path
        except (OSError, configparser.Error):
            return None

    def sac_help(self):
        """Setup's Smart App Control help (item 4), when the game was blocked."""
        self.status.configure(text="The game didn't start: Smart App Control is blocking it.")
        self.ask("Blood Wake can't start", SAC_TEXT, "OK", None)

    def start_safe(self):
        """The Advanced tab's Start in safe mode."""
        if self.busy or (self.proc and self.proc.poll() is None):
            return
        if game_running(self.adv_lines()):
            bring_game_to_front()
            self.status.configure(text="Blood Wake is already running: its window is in front.")
            return
        self.start_game(safe=True)

    def safe_offer(self):
        """Whether Play should offer safe mode: the last two starts didn't reach
        the title (the host's record, CONTRACTS.md 4)."""
        return start_history(os.path.dirname(self.path)).endswith("00")

    def bug_report(self):
        """The Advanced tab's Make a bug report: one zip on the desktop."""
        folder = desktop_folder()
        path = os.path.join(folder, time.strftime("Blood Wake bug report %Y-%m-%d %H%M%S.zip"))
        try:
            os.makedirs(folder, exist_ok=True)
            make_bug_report(path, self.path, self.edition())
        except OSError as e:
            messagebox.showerror("Blood Wake", "Couldn't make the bug report:\n%s" % e)
            return
        self.status.configure(text="Bug report saved: %s" % os.path.basename(path))
        if self.ask("Bug report saved",
                    "It's on your desktop: %s\n\nIt holds the game's log, your settings, the "
                    "start record, Setup's record and logs (when there are any), and a short "
                    "summary of this PC: the Windows version, graphics card, screens, sound "
                    "device and controllers. File paths in it include your Windows user name. "
                    "Nothing from the game's disc is in it.\n\nShare it only if you want to, "
                    "with whoever is helping you." % os.path.basename(path),
                    "Show it", "Close"):
            subprocess.Popen(["explorer", "/select,", path])
        return path

    def watch(self):
        if self.proc and self.proc.poll() is None:
            self.root.after(1000, self.watch)
            return
        # Item 97: the session is over: Windows HDR back as it was.
        done = winhdr_restore(os.path.dirname(self.path))
        self.root.deiconify()
        # Item 4: a game that ended within seconds of Play, with Smart App
        # Control on, was almost certainly blocked by it.
        if time.time() - getattr(self, "started", 0) < SAC_QUICK and smart_app_control_on():
            self.sac_help()
            return
        self.status.configure(text="The game closed. Press Play to start again." if done != "failed"
                              else "The game closed, but Windows HDR couldn't be put back: switch "
                                   "it in Windows' display settings.")

    @staticmethod
    def open_folder(path):
        os.makedirs(path, exist_ok=True)
        os.startfile(path)

    @staticmethod
    def open_file(path):
        if os.path.isfile(path):
            os.startfile(path)
        else:
            messagebox.showinfo("Blood Wake", "There's no log yet: play once first.")

    def open_saves(self):
        base = os.path.join(os.environ.get("LOCALAPPDATA", ""), "xboxrecomp")
        ed = self.edition()
        self.open_folder(base if ed == "original" else os.path.join(base, "editions", ed))


def make_root():
    """The Tk root, aware of display scaling (SCALE is set from it), in the
    palette for Windows' light or dark mode, with the program's icon."""
    global SCALE
    try:                                    # crisp text on a scaled display
        import ctypes
        ctypes.windll.shcore.SetProcessDpiAwareness(1)
    except (OSError, AttributeError):
        pass
    root = tk.Tk()
    SCALE = max(1.0, root.winfo_fpixels("1i") / 96.0)
    fake = fake_screen()
    if fake:                                # test switch: pretend this display scaling too
        SCALE = fake[2]
        root.tk.call("tk", "scaling", SCALE * 96.0 / 72.0)
    apply_theme(root)
    set_icon(root)
    return root


def main():
    root = make_root()
    if not os.path.isfile(EXE):
        root.withdraw()
        messagebox.showerror("Blood Wake", "The game isn't built yet:\n%s\n\nBuild it first "
                                           "(docs/SETUP.md)." % EXE)
        return 1
    try:
        schema = load_schema()
    except (OSError, ValueError, subprocess.SubprocessError) as e:
        root.withdraw()             # the game's first run: Smart App Control would block it here
        messagebox.showerror("Blood Wake", start_failure_text("Couldn't read the game's settings "
                                                              "table", e))
        return 1
    Launcher(root, schema)
    root.mainloop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
