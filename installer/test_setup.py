#!/usr/bin/env python3
"""Drive Blood Wake Recomp's Setup window for tests (stdlib only, Windows).

    py -3 installer/test_setup.py <scenario> --setup <BloodWakeRecomp-Setup.exe>
        [--shots <dir>] [--disc <image or folder>] [--dir <install folder>] [-- <more Setup switches>]

It starts Setup with /LOG, finds its wizard window, reads and sets controls,
presses buttons, and saves a PNG of the window after each step (PrintWindow,
written with zlib: no Pillow). Every scenario cancels Setup before anything is
installed unless it says otherwise. Scenarios:

  disc-page     item 1 and 2: the disc page's three outcomes and Next's state
  pages         every page up to Ready, with screenshots (then Cancel)

The window appears on screen while a scenario runs (a few seconds per page).
"""
import argparse
import ctypes
import ctypes.wintypes as W
import os
import struct
import subprocess
import sys
import time
import zlib

user32 = ctypes.WinDLL("user32", use_last_error=True)
gdi32 = ctypes.WinDLL("gdi32", use_last_error=True)

WM_SETTEXT, WM_GETTEXT, WM_GETTEXTLENGTH = 0x000C, 0x000D, 0x000E
BM_CLICK, BM_GETCHECK = 0x00F5, 0x00F0
WNDENUMPROC = ctypes.WINFUNCTYPE(W.BOOL, W.HWND, W.LPARAM)

user32.SendMessageW.restype = ctypes.c_ssize_t
user32.SendMessageW.argtypes = [W.HWND, W.UINT, W.WPARAM, W.LPARAM]
user32.EnumWindows.argtypes = [WNDENUMPROC, W.LPARAM]
user32.EnumChildWindows.argtypes = [W.HWND, WNDENUMPROC, W.LPARAM]
user32.GetWindowThreadProcessId.argtypes = [W.HWND, ctypes.POINTER(W.DWORD)]
user32.PrintWindow.argtypes = [W.HWND, W.HDC, W.UINT]
user32.GetDC.restype = W.HDC
user32.GetDC.argtypes = [W.HWND]
user32.ReleaseDC.argtypes = [W.HWND, W.HDC]
gdi32.CreateCompatibleDC.restype = W.HDC
gdi32.CreateCompatibleDC.argtypes = [W.HDC]
gdi32.CreateCompatibleBitmap.restype = W.HBITMAP
gdi32.CreateCompatibleBitmap.argtypes = [W.HDC, ctypes.c_int, ctypes.c_int]
gdi32.SelectObject.restype = W.HGDIOBJ
gdi32.SelectObject.argtypes = [W.HDC, W.HGDIOBJ]
gdi32.DeleteObject.argtypes = [W.HGDIOBJ]
gdi32.DeleteDC.argtypes = [W.HDC]
gdi32.GetDIBits.argtypes = [W.HDC, W.HBITMAP, W.UINT, W.UINT, ctypes.c_void_p, ctypes.c_void_p, W.UINT]


user32.SendMessageTimeoutW.restype = ctypes.c_ssize_t
user32.SendMessageTimeoutW.argtypes = [W.HWND, W.UINT, W.WPARAM, W.LPARAM, W.UINT, W.UINT, ctypes.POINTER(ctypes.c_size_t)]


def text_of(h):
    """A window's text. Never waits on a hung window (any program's): 1 s at most."""
    n = ctypes.c_size_t(0)
    if not user32.SendMessageTimeoutW(h, WM_GETTEXTLENGTH, 0, 0, 0x0002, 1000, ctypes.byref(n)):   # SMTO_ABORTIFHUNG
        return ""
    buf = ctypes.create_unicode_buffer(n.value + 1)
    r = ctypes.c_size_t(0)
    if not user32.SendMessageTimeoutW(h, WM_GETTEXT, n.value + 1, ctypes.addressof(buf), 0x0002, 1000, ctypes.byref(r)):
        return ""
    return buf.value


def class_of(h):
    buf = ctypes.create_unicode_buffer(256)
    user32.GetClassNameW(h, buf, 256)
    return buf.value


def top_windows(pid=None):
    out = []

    def cb(h, _):
        if user32.IsWindowVisible(h):
            p = W.DWORD()
            user32.GetWindowThreadProcessId(h, ctypes.byref(p))
            if pid is None or p.value == pid:
                out.append(h)
        return True
    user32.EnumWindows(WNDENUMPROC(cb), 0)
    return out


def children(h):
    out = []

    def cb(c, _):
        out.append(c)
        return True
    user32.EnumChildWindows(h, WNDENUMPROC(cb), 0)
    return out


def visible(h):
    return bool(user32.IsWindowVisible(h))


def enabled(h):
    return bool(user32.IsWindowEnabled(h))


def png_bytes(width, height, bgra):
    """A PNG (RGB) from top-down BGRA rows."""
    raw = bytearray()
    stride = width * 4
    for y in range(height):
        row = bgra[y * stride:(y + 1) * stride]
        raw.append(0)
        rgb = bytearray(width * 3)
        rgb[0::3] = row[2::4]
        rgb[1::3] = row[1::4]
        rgb[2::3] = row[0::4]
        raw += rgb

    def chunk(kind, data):
        c = struct.pack(">I", len(data)) + kind + data
        return c + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))


def capture(h, path):
    """Save window h (its whole frame) as a PNG."""
    r = W.RECT()
    user32.GetWindowRect(h, ctypes.byref(r))
    w, ht = r.right - r.left, r.bottom - r.top
    if w <= 0 or ht <= 0:
        return False
    sdc = user32.GetDC(None)
    mdc = gdi32.CreateCompatibleDC(sdc)
    bmp = gdi32.CreateCompatibleBitmap(sdc, w, ht)
    old = gdi32.SelectObject(mdc, bmp)
    ok = user32.PrintWindow(h, mdc, 2)          # PW_RENDERFULLCONTENT
    bih = struct.pack("<IiiHHIIiiII", 40, w, -ht, 1, 32, 0, 0, 0, 0, 0, 0)
    bi = ctypes.create_string_buffer(bih, 40 + 16)
    buf = ctypes.create_string_buffer(w * ht * 4)
    gdi32.GetDIBits(mdc, bmp, 0, ht, buf, bi, 0)
    gdi32.SelectObject(mdc, old)
    gdi32.DeleteObject(bmp)
    gdi32.DeleteDC(mdc)
    user32.ReleaseDC(None, sdc)
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "wb") as f:
        f.write(png_bytes(w, ht, buf.raw))
    return bool(ok)


class Wizard:
    """A running Setup (or uninstaller) and its main window."""

    def __init__(self, exe, args, shots=None, title_part="Blood Wake Recomp"):
        self.shots = shots
        self.n = 0
        self.proc = subprocess.Popen([exe] + list(args))
        self.title_part = title_part
        self.hwnd = self.wait_window(lambda h: title_part in text_of(h) and class_of(h).startswith("TWizardForm"), 60)

    def wait_window(self, test, timeout):
        t0 = time.time()
        while time.time() - t0 < timeout:
            for h in top_windows():
                try:
                    if test(h):
                        return h
                except OSError:
                    pass
            time.sleep(0.2)
        raise RuntimeError("no window appeared within %d s" % timeout)

    def controls(self, cls=None):
        return [c for c in children(self.hwnd) if (cls is None or class_of(c) == cls)]

    def button(self, caption):
        want = caption.replace("&", "").lower()
        for c in self.controls():
            if visible(c) and "Button" in class_of(c) and text_of(c).replace("&", "").lower().startswith(want):
                return c
        raise LookupError("no visible button %r (buttons: %s)" % (caption, [
            text_of(c) for c in self.controls() if "Button" in class_of(c) and visible(c)]))

    def texts(self):
        """Every visible control's text (static texts, labels, check boxes)."""
        return [text_of(c) for c in self.controls() if visible(c) and text_of(c)]

    def has_text(self, part):
        return any(part in t for t in self.texts())

    def edit(self, index=0):
        eds = [c for c in self.controls() if visible(c) and "Edit" in class_of(c)]
        return eds[index]

    def set_edit(self, value, index=0):
        h = self.edit(index)
        buf = ctypes.create_unicode_buffer(value)
        user32.SendMessageW(h, WM_SETTEXT, 0, ctypes.addressof(buf))
        time.sleep(0.3)

    def click(self, caption, wait=0.8):
        h = self.button(caption)
        user32.PostMessageW(h, BM_CLICK, 0, 0)
        time.sleep(wait)

    def check(self, caption, state=True):
        for c in self.controls():
            if visible(c) and text_of(c).replace("&", "").startswith(caption):
                if bool(user32.SendMessageW(c, BM_GETCHECK, 0, 0)) != state:
                    user32.PostMessageW(c, BM_CLICK, 0, 0)
                    time.sleep(0.4)
                return
        raise LookupError("no check box %r" % caption)

    def is_checked(self, caption):
        for c in self.controls():
            if visible(c) and text_of(c).replace("&", "").startswith(caption):
                return bool(user32.SendMessageW(c, BM_GETCHECK, 0, 0))
        raise LookupError("no check box %r" % caption)

    def next_enabled(self):
        return enabled(self.button("Next"))

    def shot(self, name, hwnd=None):
        if not self.shots:
            return None
        self.n += 1
        path = os.path.join(self.shots, "%02d_%s.png" % (self.n, name))
        capture(hwnd or self.hwnd, path)
        return path

    def dialog(self, timeout=5):
        """A message box or dialog that Setup opened, or None. Inno 6.7 draws its
        own (TTaskDialogForm); system ones are #32770."""
        pid = self.wizard_pid()
        t0 = time.time()
        while time.time() - t0 < timeout:
            for h in top_windows(pid):
                if h != self.hwnd and class_of(h) in ("#32770", "TTaskDialogForm", "TMsgBoxForm", "TSetupForm"):
                    return h
            time.sleep(0.2)
        return None

    def wizard_pid(self):
        p = W.DWORD()
        user32.GetWindowThreadProcessId(self.hwnd, ctypes.byref(p))
        return p.value

    def dialog_text(self, h):
        return "\n".join(text_of(c) for c in children(h) if text_of(c))

    def dialog_click(self, h, caption):
        want = caption.replace("&", "").lower()
        for c in children(h):
            if "Button" in class_of(c) and text_of(c).replace("&", "").lower().startswith(want):
                user32.PostMessageW(c, BM_CLICK, 0, 0)
                time.sleep(0.6)
                return True
        return False

    def cancel(self):
        """Cancel and confirm 'exit Setup?'."""
        self.click("Cancel", 0.6)
        d = self.dialog(5)
        if d:
            self.dialog_click(d, "Yes")
        try:
            return self.proc.wait(30)
        except subprocess.TimeoutExpired:
            return None


def page_title(wz):
    """The wizard's page heading: the first visible static text near the top."""
    return [t for t in wz.texts()][:3]


# ── Scenarios ─────────────────────────────────────────────────────────────

def expect(cond, what, results):
    results.append(("PASS" if cond else "FAIL", what))
    print("%s  %s" % ("PASS" if cond else "FAIL", what))
    sys.stdout.flush()


def go_to_disc_page(wz):
    wz.click("Next")                     # welcome -> disclaimer
    wz.check("I own Blood Wake")
    wz.click("Next")                     # -> disc page


def scenario_disc_page(a, results):
    """Item 1: Next never goes on without a checked, usable copy."""
    args = ["/LOG=" + os.path.join(a.shots, "setup.log"), "/DIR=" + a.dir] + a.rest
    wz = Wizard(a.setup, args, a.shots)
    try:
        go_to_disc_page(wz)
        wz.set_edit("")
        wz.shot("disc_empty")
        expect(not wz.next_enabled(), "empty box: Next is greyed out", results)
        expect(wz.has_text("Choose your copy of Blood Wake to go on"), "empty box: the status says what's needed", results)
        wz.set_edit(os.path.join(a.work, "no_such_folder"))
        wz.shot("disc_missing")
        expect(not wz.next_enabled(), "a path that doesn't exist: Next is greyed out", results)
        empty_dir = os.path.join(a.work, "empty_folder")
        os.makedirs(empty_dir, exist_ok=True)
        wz.set_edit(empty_dir)
        wz.shot("disc_no_xbe")
        expect(not wz.next_enabled(), "a folder without default.xbe: Next is greyed out", results)
        expect(wz.has_text("has no default.xbe"), "a folder without default.xbe: refused in plain words", results)
        notiso = os.path.join(a.work, "notes.iso")
        with open(notiso, "w") as f:
            f.write("this is a text file, not a disc image\n" * 100)
        wz.set_edit(notiso)
        wz.shot("disc_text_iso")
        expect(not wz.next_enabled(), "a text file renamed .iso: Next is greyed out", results)
        if a.disc:
            wz.set_edit(a.disc)
            time.sleep(2)
            wz.shot("disc_good")
            expect(wz.next_enabled(), "the retail copy: Next works", results)
            expect(wz.has_text("Recognised"), "the retail copy: recognised", results)
            wz.click("Next")
            expect(wz.has_text("Select Destination Location") or wz.has_text("Where should"),
                   "the retail copy: Next leads to the folder page", results)
            wz.shot("folder_page")
    finally:
        rc = wz.cancel()
        expect(rc is not None, "Setup closed after Cancel (exit %s)" % rc, results)


def mark_of(wz):
    """The mark beside the path: its text, or '' when hidden."""
    for c in wz.controls("TNewStaticText"):
        t = text_of(c)
        if visible(c) and t in ("✔", "!", "✖"):
            return t
    return ""


def scenario_unverified(a, results):
    """Item 2: the three outcomes, the warning page, Accept, Back and Decline.
    --disc is the retail copy; --unverified a copy whose program differs."""
    args = ["/LOG=" + os.path.join(a.shots, "setup.log"), "/DIR=" + a.dir] + a.rest
    wz = Wizard(a.setup, args, a.shots)
    rc = None
    try:
        go_to_disc_page(wz)
        notiso = os.path.join(a.work, "notes.iso")
        with open(notiso, "w") as f:
            f.write("this is a text file, not a disc image\n" * 100)
        wz.set_edit(notiso)
        wz.shot("unusable")
        expect(mark_of(wz) == "✖" and not wz.next_enabled(), "unusable: a cross, Next greyed", results)
        wz.set_edit(a.disc)
        time.sleep(2)
        wz.shot("verified")
        expect(mark_of(wz) == "✔" and wz.next_enabled(), "verified: a tick, Next works", results)
        expect(wz.has_text("Recognised: Blood Wake, Xbox retail, North America"), "verified: 'Recognised: Blood Wake, Xbox retail, North America'", results)
        wz.click("Next")
        expect(wz.has_text("Select Destination Location") or wz.has_text("Where should"),
               "verified: no warning page, straight to the folder page", results)
        wz.click("Back")
        wz.set_edit(a.unverified)
        time.sleep(1)
        wz.shot("unverified")
        expect(mark_of(wz) == "!" and wz.next_enabled(), "unverified: a ! mark, Next works", results)
        expect(wz.has_text("doesn't recognise this copy"), "unverified: the status says Setup doesn't recognise it", results)
        wz.click("Next")
        wz.shot("warning")
        expect(wz.has_text("Ownership") or any("Ownership" in t for t in wz.texts()),
               "unverified: Next leads to the warning page", results)
        expect(text_of(wz.button("Accept")) == "&Accept" and text_of(wz.button("Decline")) == "&Decline",
               "the warning page's buttons are Accept and Decline", results)
        wz.click("Back")
        expect(wz.has_text("Choose your disc") or wz.has_text("doesn't recognise this copy"), "Back returns to the disc page", results)
        expect(text_of(wz.button("Cancel")) == "Cancel", "Cancel is Cancel again off the warning page", results)
        wz.click("Next")
        wz.click("Accept")
        expect(wz.has_text("Select Destination Location") or wz.has_text("Where should"), "Accept goes on to the folder page", results)
        wz.shot("after_accept")
        wz.click("Next")                 # -> needs
        wz.click("Next")                 # -> shortcuts
        wz.click("Next")                 # -> ready
        wz.shot("ready")
        expect(any("Not recognised: you accepted the warning" in t for t in ready_text(wz)),
               "the Ready page says it's an unverified copy, accepted", results)
        rc = wz.cancel()
        expect(rc == 2, "Cancel on the Ready page: exit 2 (nothing installed)", results)
        # Decline: a fresh Setup, straight to the warning page
        wz = Wizard(a.setup, args, a.shots)
        go_to_disc_page(wz)
        wz.set_edit(a.unverified)
        time.sleep(1)
        wz.click("Next")
        wz.click("Decline", 1.0)
        d = wz.dialog(2)
        expect(d is None, "Decline asks nothing more", results)
        try:
            rc = wz.proc.wait(20)
        except subprocess.TimeoutExpired:
            rc = None
        expect(rc is not None, "Decline stops Setup (exit %s)" % rc, results)
        expect(not os.path.exists(a.dir), "Decline: nothing was installed", results)
    finally:
        if wz.proc.poll() is None:
            wz.cancel()


# ── Silent installs and uninstalls (stub builds) ──────────────────────────

APPID_KEY = r"Software\Microsoft\Windows\CurrentVersion\Uninstall\{0FB930E5-4F64-4720-9243-3E2404D38AB5}_is1"
LINK = "Blood Wake Recomp.lnk"


def reg_present():
    import winreg
    try:
        winreg.CloseKey(winreg.OpenKey(winreg.HKEY_CURRENT_USER, APPID_KEY))
        return True
    except OSError:
        return False


def links():
    sm = os.path.join(os.environ["APPDATA"], r"Microsoft\Windows\Start Menu\Programs", LINK)
    buf = ctypes.create_unicode_buffer(260)
    ctypes.windll.shell32.SHGetFolderPathW(None, 0x10, None, 0, buf)     # CSIDL_DESKTOPDIRECTORY
    return os.path.exists(sm), os.path.exists(os.path.join(buf.value, LINK))


def silent_setup(a, log, *extra, env=None):
    args = [a.setup, "/VERYSILENT", "/SUPPRESSMSGBOXES", "/IOWNTHEGAME", "/STUBBUILD", "/DISC=" + a.disc,
            "/DIR=" + a.dir, "/LOG=" + log] + list(extra)
    e = dict(os.environ)
    e.update(env or {})
    return subprocess.run(args, env=e).returncode


def silent_uninstall(a, log, *extra, timeout=180):
    """Runs the install's uninstaller and waits for it to finish (its first
    process hands over to a copy in %TEMP% and exits at once)."""
    exe = os.path.join(a.dir, "unins000.exe")
    subprocess.run([exe, "/VERYSILENT", "/SUPPRESSMSGBOXES", "/LOG=" + log,
                    "/USERDATAROOT=" + a.userdata] + list(extra))
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            with open(log, encoding="utf-8", errors="replace") as f:
                text = f.read()
            if "Log closed" in text or "Uninstall process exiting" in text or "Stopped:" in text:
                time.sleep(1.5)
                return text
        except OSError:
            pass
        time.sleep(0.5)
    return ""


def log_text(path):
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def finished_line(text):
    for line in text.splitlines():
        if "Finished page:" in line:
            return line.split("Finished page:", 1)[1].strip()
    return ""


RUNTIME = ("play.log", "xbox_kernel.log", "icall_targets.dump", "icall_sites.dump", "bw_launch_next.bin", "bw_relaunch.bin")


def scenario_install_cycle(a, results):
    """Items 11 and 13, silent: every shortcut combination, reinstalling into
    the same folder after uninstalls that keep the Python or the game files."""
    os.makedirs(a.userdata, exist_ok=True)
    L = lambda n: os.path.join(a.shots, n)
    steps = [("startmenuicon,desktopicon", (True, True), "Start menu or the desktop", ["/KEEPPYTHON"]),
             ("startmenuicon", (True, False), "Start it from the Start menu", ["/KEEPPYTHON", "/KEEPGAME"]),
             ("desktopicon", (False, True), "Start it from the desktop", []),
             ("", (False, False), "You chose no shortcuts", [])]
    for i, (tasks, want, words, keep) in enumerate(steps, 1):
        rc = silent_setup(a, L("install%d.log" % i), "/TASKS=" + tasks)
        text = log_text(L("install%d.log" % i))
        expect(rc == 0, "install %d (/TASKS=%s): exit %d" % (i, tasks or '""', rc), results)
        expect(links() == want, "install %d: shortcuts (Start menu, desktop) = %s" % (i, links()), results)
        expect(words in finished_line(text), "install %d: the Finished page says %r" % (i, words), results)
        if i > 1 and "/KEEPPYTHON" in steps[i - 2][3]:
            reused = os.path.isdir(os.path.join(a.dir, "python")) and "python-3.13.15-amd64.zip" not in text
            expect(reused, "install %d: the kept Python was reused (no download)" % i, results)
        for n in RUNTIME:                    # as if the game had been played
            with open(os.path.join(a.dir, n), "w") as f:
                f.write("written while playing (test)\n")
        text = silent_uninstall(a, L("uninstall%d.log" % i), *keep)
        left = sorted(os.listdir(a.dir)) if os.path.isdir(a.dir) else []
        expect(not reg_present(), "uninstall %d %s: the Settings > Apps entry is gone" % (i, " ".join(keep)), results)
        expect(links() == (False, False), "uninstall %d: no shortcuts left" % i, results)
        expect(not any(n in left for n in RUNTIME), "uninstall %d: the runtime files are gone (left: %s)" % (i, left), results)
        if keep:
            expect("kept_by_uninstall.txt" in left, "uninstall %d: kept %s, marker written" % (i, [k[5:].lower() for k in keep]), results)
        else:
            expect(not os.path.isdir(a.dir), "uninstall %d: the folder is gone" % i, results)


def find_dialog(part, timeout=20, shots=None, name=None):
    """A dialog (any process) whose text holds part: (hwnd, text) or (None, '')."""
    t0 = time.time()
    while time.time() - t0 < timeout:
        for h in top_windows():
            if class_of(h) in ("#32770", "TTaskDialogForm", "TSetupForm", "TMsgBoxForm"):
                text = "\n".join(text_of(c) for c in children(h) if text_of(c)) + "\n" + text_of(h)
                if part in text:
                    if shots and name:
                        capture(h, os.path.join(shots, name + ".png"))
                    return h, text
        time.sleep(0.3)
    return None, ""


def press(h, caption):
    want = caption.replace("&", "").lower()
    for c in children(h):
        if "Button" in class_of(c) and text_of(c).replace("&", "").lower().startswith(want):
            user32.PostMessageW(c, BM_CLICK, 0, 0)
            time.sleep(0.8)
            return True
    return False


MUTEX_HOLDER = ("import ctypes, time\n"
                "h = ctypes.windll.kernel32.CreateMutexW(None, False, 'BloodWakeRecomp.Running')\n"
                "time.sleep(600)\n")


def scenario_running(a, results):
    """Items 11 (0.2.4's leftovers) and 12: nothing is uninstalled or updated while
    Blood Wake runs from the install folder or holds its mutex."""
    os.makedirs(a.userdata, exist_ok=True)
    L = lambda n: os.path.join(a.shots, n)
    before = sorted(os.listdir(a.dir)) if os.path.isdir(a.dir) else []
    rc = silent_setup(a, L("install.log"), "/TASKS=startmenuicon")
    expect(rc == 0, "install into a folder holding only an earlier version's leftovers %s: exit %d" % (before, rc), results)
    holder = subprocess.Popen([os.path.join(a.dir, "python", "pythonw.exe"), "-c", "import time; time.sleep(600)"])
    try:
        time.sleep(2)
        text = silent_uninstall(a, L("uninstall_running.log"))
        expect("Stopped: close Blood Wake first" in text and reg_present(),
               "silent uninstall while a program runs from the folder: stops, still installed", results)
        rc = silent_setup(a, L("update_running.log"), "/TASKS=startmenuicon")
        text = log_text(L("update_running.log"))
        expect(rc != 0 and "still running" in text, "silent update while it runs: stops (exit %d)" % rc, results)
        subprocess.Popen([os.path.join(a.dir, "unins000.exe")])
        h, text = find_dialog("Close Blood Wake first", 30, a.shots, "uninstall_running_dialog")
        expect(h is not None and "pythonw.exe" in text, "the uninstaller says to close Blood Wake first, naming pythonw.exe", results)
        if h:
            press(h, "Cancel")
            time.sleep(2)
        expect(reg_present(), "Cancel there: still installed", results)
    finally:
        holder.kill()
        holder.wait()
    mutex = subprocess.Popen([sys.executable, "-c", MUTEX_HOLDER])
    try:
        time.sleep(2)
        subprocess.Popen([os.path.join(a.dir, "unins000.exe")])
        h, text = find_dialog("Close Blood Wake first", 30, a.shots, "uninstall_mutex_dialog")
        expect(h is not None and "the game is running" in text,
               "with the game's mutex held, the uninstaller asks to close Blood Wake", results)
        if h:
            press(h, "Cancel")
            time.sleep(3)
        expect(reg_present(), "Cancel there: still installed", results)
        subprocess.Popen([a.setup, "/LOG=" + L("setup_mutex.log")])
        h, text = find_dialog("currently running", 30, a.shots, "setup_mutex_dialog")
        expect(h is not None, "with the game's mutex held, Setup asks to close Blood Wake at its start", results)
        if h:
            press(h, "Cancel")
            time.sleep(3)
    finally:
        mutex.kill()
        mutex.wait()
    time.sleep(1)
    text = silent_uninstall(a, L("uninstall.log"))
    expect(not reg_present() and not os.path.isdir(a.dir), "once closed, the uninstall is complete: entry and folder gone", results)


def helpers_left(inst):
    """Processes still running that the test install's build started (their PIDs):
    programs run from the install folder (its Python, the game), and anything
    naming its work or setup folders (CMake, MSBuild, the compilers). Setup and
    this test only name the folder itself (/DIR, --dir), so they don't count."""
    q = inst.replace("'", "''")
    ps = ("$d = '%s'; Get-CimInstance Win32_Process | Where-Object { $_.ProcessId -ne $PID -and ("
          "($_.ExecutablePath -and $_.ExecutablePath.StartsWith($d + '\\', 'OrdinalIgnoreCase')) -or "
          "($_.CommandLine -and ($_.CommandLine.IndexOf($d + '\\work', [StringComparison]::OrdinalIgnoreCase) -ge 0 -or "
          "$_.CommandLine.IndexOf($d + '\\setup', [StringComparison]::OrdinalIgnoreCase) -ge 0))) } "
          "| ForEach-Object { $_.ProcessId }" % q)
    out = subprocess.run(["powershell", "-NoProfile", "-Command", ps], capture_output=True, text=True).stdout
    return [int(x) for x in out.split() if x.strip().isdigit()]


def scenario_cancel(a, results):
    """Item 10: Cancel stays live during the build and stops it cleanly (stub build)."""
    os.makedirs(a.userdata, exist_ok=True)
    args = ["/LOG=" + os.path.join(a.shots, "setup.log"), "/DIR=" + a.dir, "/STUBBUILD", "/TASKS=startmenuicon"]
    os.environ["BW_SETUP_STUB_SECONDS"] = "120"
    wz = Wizard(a.setup, args, a.shots)
    try:
        go_to_disc_page(wz)
        wz.set_edit(a.disc)
        time.sleep(2)
        for _ in range(4):                      # to the folder, needs, shortcuts and Ready pages
            wz.click("Next")
        wz.shot("ready")
        wz.click("Install", 2)
        t0 = time.time()
        while time.time() - t0 < 240 and not wz.has_text("(stub)"):
            time.sleep(1)
        wz.shot("building")
        expect(wz.has_text("(stub)"), "the build is running", results)
        expect(enabled(wz.button("Cancel")), "Cancel is live during the build", results)
        wz.click("Cancel", 1)
        h, text = find_dialog("Stop building Blood Wake?", 10, a.shots, "cancel_confirm")
        expect(h is not None, "Cancel asks first: 'Stop building Blood Wake?'", results)
        if h:
            press(h, "Yes")
        t0 = time.time()
        while time.time() - t0 < 60 and not wz.has_text("wasn't built"):
            time.sleep(1)
        wz.shot("finished_cancelled")
        expect(wz.has_text("You stopped the build"), "the Finished page says the build was stopped", results)
        left = helpers_left(os.path.normpath(a.dir))
        expect(not left, "no build processes left (%s)" % left, results)
        wz.click("Finish", 2)
        try:
            rc = wz.proc.wait(30)
        except subprocess.TimeoutExpired:
            rc = None
        expect(rc == 108, "Setup's exit code: %s (108 = you stopped the build)" % rc, results)
    finally:
        if wz.proc.poll() is None:
            wz.cancel()
    if reg_present():
        silent_uninstall(a, os.path.join(a.shots, "uninstall.log"))
    expect(not reg_present(), "cleaned up: uninstalled", results)


def to_needs_page(wz, disc):
    go_to_disc_page(wz)
    wz.set_edit(disc)
    time.sleep(2)
    wz.click("Next")                     # -> folder
    wz.click("Next")                     # -> what's needed


def scenario_sac(a, results):
    """Item 4: the Smart App Control page, with /SIMULATESAC (never turned on for real)."""
    os.makedirs(a.userdata, exist_ok=True)
    log = lambda n: "/LOG=" + os.path.join(a.shots, n)
    wz = Wizard(a.setup, [log("gui_off.log"), "/DIR=" + a.dir], a.shots)
    try:
        to_needs_page(wz, a.disc)
        wz.click("Next")
        expect(not wz.has_text("Smart App Control") and wz.has_text("Select Additional Tasks"),
               "Smart App Control off: no page, straight to the shortcuts", results)
    finally:
        wz.cancel()
    wz = Wizard(a.setup, [log("gui_on.log"), "/DIR=" + a.dir, "/SIMULATESAC"], a.shots)
    try:
        to_needs_page(wz, a.disc)
        wz.click("Next")
        wz.shot("sac_page")
        expect(wz.has_text("Smart App Control will block the game"), "on: the page appears after 'What Blood Wake needs'", results)
        expect(text_of(wz.button("Continue")) == "Continue &anyway", "its buttons: Continue anyway and Cancel", results)
        wz.click("Continue")
        expect(wz.has_text("Select Additional Tasks"), "Continue anyway goes on to the shortcuts", results)
        wz.click("Back")
        rc = wz.cancel()
        expect(rc == 2 and not os.path.exists(os.path.join(a.dir, "unins000.exe")),
               "Cancel on it stops Setup, nothing installed (exit %s)" % rc, results)
    finally:
        if wz.proc.poll() is None:
            wz.cancel()
    rc = silent_setup(a, os.path.join(a.shots, "silent_on.log"), "/SIMULATESAC", "/TASKS=")
    text = log_text(os.path.join(a.shots, "silent_on.log"))
    expect(rc == 0 and "Smart App Control is on: the game will be blocked" in text,
           "silent with it on: goes on, with a log line (exit %d)" % rc, results)
    expect("Smart App Control is on, so Windows will block the game" in finished_line(text),
           "the Finished page says so too", results)
    silent_uninstall(a, os.path.join(a.shots, "uninstall.log"))
    expect(not reg_present(), "cleaned up: uninstalled", results)


def ready_text(wz):
    """The Ready page's memo text."""
    out = []
    for c in wz.controls():
        if visible(c) and ("Memo" in class_of(c) or "RichEdit" in class_of(c)):
            out.append(text_of(c))
    return out


def scenario_pages(a, results):
    """Item 15: a picture of every page, Setup's and the uninstaller's (stub build).
    --unverified adds the warning page; /SIMULATESAC adds Smart App Control's."""
    os.makedirs(a.userdata, exist_ok=True)
    os.environ["BW_SETUP_STUB_SECONDS"] = "25"
    wz = Wizard(a.setup, ["/LOG=" + os.path.join(a.shots, "setup.log"), "/DIR=" + a.dir, "/STUBBUILD",
                          "/SIMULATESAC", "/TASKS=startmenuicon,desktopicon"], a.shots)
    try:
        time.sleep(1)
        wz.shot("welcome")
        wz.click("Next")
        wz.check("I own Blood Wake")
        wz.shot("disclaimer")
        wz.click("Next")
        wz.set_edit(a.disc)
        time.sleep(2)
        wz.shot("disc")
        if a.unverified:
            wz.set_edit(a.unverified)
            time.sleep(1)
            wz.shot("disc_unverified")
            wz.click("Next")
            wz.shot("warning")
            wz.click("Back")
            wz.set_edit(a.disc)
            time.sleep(2)
        wz.click("Next")
        wz.shot("folder")
        wz.click("Next")
        wz.shot("needs")
        wz.click("Next")
        wz.shot("smart_app_control")
        wz.click("Continue")
        wz.shot("shortcuts")
        wz.click("Next")
        wz.shot("ready")
        wz.click("Install", 2)
        t0 = time.time()
        while time.time() - t0 < 240 and not wz.has_text("Translating"):
            time.sleep(0.5)
        wz.shot("installing")
        t0 = time.time()
        while time.time() - t0 < 240 and not wz.has_text("built and ready") and not wz.has_text("wasn't built"):
            time.sleep(1)
        wz.shot("finished")
        expect(wz.has_text("built and ready"), "the stub install finished", results)
        for c in wz.controls():                 # untick "Open the launcher now": click its box
            if visible(c) and "CheckListBox" in class_of(c):
                pos = (9 << 16) | 9
                user32.PostMessageW(c, 0x0201, 1, pos)      # WM_LBUTTONDOWN
                user32.PostMessageW(c, 0x0202, 0, pos)      # WM_LBUTTONUP
        time.sleep(0.8)
        wz.shot("finished_unticked")
        wz.click("Finish", 3)
        for pid in helpers_left(os.path.normpath(a.dir)):  # a launcher it opened anyway
            subprocess.run(["taskkill", "/PID", str(pid), "/F"], capture_output=True)
    finally:
        if wz.proc.poll() is None:
            wz.cancel()
    subprocess.Popen([os.path.join(a.dir, "unins000.exe"), "/USERDATAROOT=" + a.userdata])
    h, _ = find_dialog("The game you built", 30, a.shots, "%02d_uninstall_options" % (wz.n + 1))
    if h:
        press(h, "Uninstall")
    h, _ = find_dialog("with the choices you made", 20, a.shots, "%02d_uninstall_confirm" % (wz.n + 2))
    if h:
        press(h, "Yes")
    h, text = find_dialog("removed", 120, a.shots, "%02d_uninstall_done" % (wz.n + 3))
    expect(h is not None, "the uninstaller's three windows were captured", results)
    if h:
        press(h, "OK")
    time.sleep(2)
    expect(not reg_present(), "cleaned up: uninstalled", results)


def scenario_buildtools(a, results):
    """Item 16: a PC without Visual Studio, as far as this PC allows: the Build
    Tools path in plan-only mode, and a declined permission prompt (simulated:
    the real prompt is on the secure desktop, and saying yes would install software)."""
    os.makedirs(a.userdata, exist_ok=True)
    L = lambda n: os.path.join(a.shots, n)
    rc = silent_setup(a, L("plan.log"), "/SIMULATENOVS", "/VSPLANONLY", "/TASKS=")
    text = log_text(L("plan.log"))
    plan = os.path.join(a.dir, "deps", "vs_plan.txt")
    expect(rc == 0 and os.path.isfile(plan), "no Visual Studio, plan only: installs, the command is written, not run (exit %d)" % rc, results)
    if os.path.isfile(plan):
        with open(plan, encoding="utf-8", errors="replace") as f:
            expect("matched the pinned SHA-256" in f.read(), "the bootstrapper matched its pinned SHA-256", results)
    silent_uninstall(a, L("uninstall_plan.log"))
    rc = silent_setup(a, L("declined.log"), "/SIMULATENOVS", "/SIMULATEUACDECLINE", "/INSTALLVS", "/TASKS=")
    text = log_text(L("declined.log"))
    expect(rc == 110, "a declined permission prompt: Setup ends with exit 110 (got %d)" % rc, results)
    expect("permission prompt was declined" in finished_line(text),
           "the Finished page says the build tools weren't installed because the prompt was declined", results)
    silent_uninstall(a, L("uninstall_declined.log"))
    expect(not reg_present(), "cleaned up: uninstalled", results)


def browse_to(wz, path, folder=False, shots=None, name=None):
    """Press Browse and pick path in Windows' own dialog (typed into its name box)."""
    wz.click("Browse", 1.5)
    t0 = time.time()
    h = None
    while time.time() - t0 < 20 and not h:
        for w in top_windows(wz.wizard_pid()):
            if class_of(w) == "#32770" and any(class_of(c) == "Edit" for c in children(w)):
                h = w
        time.sleep(0.3)
    if not h:
        return False
    if shots and name:
        capture(h, os.path.join(shots, name + ".png"))
    edits = [c for c in children(h) if class_of(c) == "Edit" and visible(c)]
    if not edits:
        return False
    buf = ctypes.create_unicode_buffer(os.path.normpath(path))      # Windows' dialogs want backslashes
    user32.SendMessageW(edits[0], WM_SETTEXT, 0, ctypes.addressof(buf))
    time.sleep(0.5)
    for c in children(h):
        t = text_of(c).replace("&", "")
        if "Button" in class_of(c) and visible(c) and t in ("Open", "Select Folder", "OK"):
            user32.PostMessageW(c, BM_CLICK, 0, 0)
            break
    time.sleep(2.5)
    return True


def scenario_browse(a, results):
    """Item 16: both Browse dialogs on the disc page (Windows' own pickers)."""
    wz = Wizard(a.setup, ["/LOG=" + os.path.join(a.shots, "setup.log"), "/DIR=" + a.dir], a.shots)
    try:
        go_to_disc_page(wz)
        wz.set_edit("")
        ok = browse_to(wz, a.disc, False, a.shots, "browse_image_dialog")
        wz.shot("after_browse_image")
        expect(ok and wz.has_text("Recognised") and text_of(wz.edit()).lower() == a.disc.replace("/", "\\").lower(),
               "Browse (image): Windows' file dialog, the image is picked and checked (%s)" % text_of(wz.edit()), results)
        user32.PostMessageW([c for c in wz.controls() if text_of(c).startswith("A folder")][0], BM_CLICK, 0, 0)
        time.sleep(0.5)
        ok = browse_to(wz, a.folder, True, a.shots, "browse_folder_dialog")
        wz.shot("after_browse_folder")
        expect(ok and wz.has_text("Recognised") and wz.has_text("a folder of the disc's files"),
               "Browse (folder): Windows' folder dialog, the folder is picked and checked (%s)" % text_of(wz.edit()), results)
    finally:
        rc = wz.cancel()
        expect(rc == 2, "Cancel: Setup exits 2", results)


def wait_text(wz, part, timeout):
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            if wz.has_text(part):
                return True
        except OSError:
            pass
        time.sleep(1)
    return False


def real_run(a, results, stage, tag):
    """One real GUI install of the actual game (Setup's full build), cancelled at
    stage ('toolkit', 'compile', 'warmup') or, with stage None, run to the end."""
    os.environ.pop("BW_SETUP_STUB_SECONDS", None)
    if stage == "toolkit":                      # the 2 MB download takes ~2 s: stretch it to press Cancel
        os.environ["BW_TEST_DOWNLOAD_DELAY"] = "90"
    else:
        os.environ.pop("BW_TEST_DOWNLOAD_DELAY", None)
    wz = Wizard(a.setup, ["/LOG=" + os.path.join(a.shots, tag + ".log"), "/DIR=" + a.dir,
                          "/TASKS=startmenuicon,desktopicon"], a.shots)
    rc = None
    try:
        go_to_disc_page(wz)
        wz.set_edit(a.disc)
        time.sleep(3)
        for _ in range(4):
            wz.click("Next", 1.2)
        wz.click("Install", 2)
        want = {"toolkit": "Getting the xboxrecomp toolkit", "compile": "Compiling the game",
                "warmup": "Preparing graphics"}.get(stage)
        if want:
            seen = wait_text(wz, want, 1800)
            if stage == "compile":
                time.sleep(60)                  # well into the compile
            elif stage == "warmup":
                time.sleep(25)                  # the game's hidden scenes are running
            wz.shot(tag + "_before_cancel")
            expect(seen, "%s: reached '%s'" % (tag, want), results)
            wz.click("Cancel", 1)
            h, _ = find_dialog("Stop building Blood Wake?", 10)
            if h:
                press(h, "Yes")
            done = wait_text(wz, "wasn't built", 120)
            wz.shot(tag + "_finished")
            expect(done and wz.has_text("You stopped the build"), "%s: stopped, and the Finished page says so" % tag, results)
            left = helpers_left(os.path.normpath(a.dir))
            for _ in range(20):                 # processes take a moment to exit
                if not left:
                    break
                time.sleep(1)
                left = helpers_left(os.path.normpath(a.dir))
            expect(not left, "%s: nothing of the build is left running (%s)" % (tag, left), results)
            wz.click("Finish", 2)
        else:
            done = wait_text(wz, "built and ready", 3600) or wait_text(wz, "wasn't built", 5)
            wz.shot(tag + "_finished")
            expect(wz.has_text("built and ready"), "%s: the full build finished" % tag, results)
            for c in wz.controls():
                if visible(c) and "CheckListBox" in class_of(c):
                    pos = (9 << 16) | 9
                    user32.PostMessageW(c, 0x0201, 1, pos)
                    user32.PostMessageW(c, 0x0202, 0, pos)
            time.sleep(0.8)
            wz.click("Finish", 2)
        try:
            rc = wz.proc.wait(60)
        except subprocess.TimeoutExpired:
            rc = None
    finally:
        if wz.proc.poll() is None:
            wz.cancel()
    return rc


def scenario_real(a, results):
    """Items 10 and 16: the real build, cancelled three ways, then run to the end."""
    for stage, tag, want in (("toolkit", "a_toolkit", 108), ("compile", "b_compile", 108),
                             ("warmup", "c_warmup", 108), (None, "d_full", 0)):
        if a.only and tag[0] not in a.only:
            continue
        rc = real_run(a, results, stage, tag)
        expect(rc == want, "%s: Setup's exit code %s (want %d)" % (tag, rc, want), results)


def registered_location():
    import winreg
    try:
        k = winreg.OpenKey(winreg.HKEY_CURRENT_USER, APPID_KEY)
        return os.path.normcase(os.path.normpath(winreg.QueryValueEx(k, "InstallLocation")[0]))
    except OSError:
        return None


def scenario_two_folders(a, results):
    """Item 98: one install per Windows user. Installed in A, Setup defaults to A;
    another folder B is refused (update A instead, or cancel); a silent /DIR=B
    stops; A again updates. An install whose uninstaller is gone doesn't block."""
    os.makedirs(a.userdata, exist_ok=True)
    A, B = os.path.normpath(a.dir), os.path.normpath(a.dir2)
    L = lambda n: os.path.join(a.shots, n)
    norm = lambda p: os.path.normcase(os.path.normpath(p))
    rc = silent_setup(a, L("install_A.log"), "/TASKS=startmenuicon")
    expect(rc == 0 and registered_location() == norm(A), "install into A: exit %d, the Apps entry is A's" % rc, results)
    # In the window: the folder page offers A; B is refused
    for choice in ("Install it there instead", "Cancel Setup"):
        wz = Wizard(a.setup, ["/LOG=" + L("gui_%s.log" % choice.split()[0].lower()), "/STUBBUILD"], a.shots)
        try:
            go_to_disc_page(wz)
            wz.set_edit(a.disc)
            time.sleep(2)
            wz.click("Next", 1.5)                               # -> the folder page
            shown = text_of(wz.edit())
            expect(norm(shown) == norm(A), "the folder page offers the installed folder (%s)" % shown, results)
            wz.set_edit(B)
            wz.click("Next", 1.5)
            h, text = find_dialog("already installed", 10, a.shots, "already_installed_%s" % choice.split()[0].lower())
            expect(h is not None and A.lower() in text.lower(), "another folder: 'Blood Wake Recomp is already installed' in A", results)
            if not h:
                continue
            press(h, choice)
            if choice.startswith("Install"):
                time.sleep(1.5)
                expect(wz.has_text("What Blood Wake needs"), "'Install it there instead' goes on", results)
                wz.click("Back", 1.5)
                expect(norm(text_of(wz.edit())) == norm(A), "...with the folder set to A", results)
            else:
                try:
                    rc = wz.proc.wait(20)
                except subprocess.TimeoutExpired:
                    rc = None
                expect(rc is not None and wz.dialog(1) is None, "'Cancel Setup' ends Setup at once (exit %s)" % rc, results)
        finally:
            if wz.proc.poll() is None:
                wz.cancel()
    expect(not os.path.exists(B) and registered_location() == norm(A), "after both: nothing in B, the entry still A's", results)
    args = [a.setup, "/VERYSILENT", "/SUPPRESSMSGBOXES", "/IOWNTHEGAME", "/STUBBUILD", "/DISC=" + a.disc,
            "/DIR=" + B, "/LOG=" + L("silent_B.log"), "/TASKS=startmenuicon"]
    rc = subprocess.run(args).returncode
    text = log_text(L("silent_B.log"))
    expect(rc != 0 and "already installed in" in text and not os.path.exists(B) and registered_location() == norm(A),
           "silent /DIR=B: stops (exit %d) with a log line; nothing in B; the entry still A's" % rc, results)
    rc = silent_setup(a, L("update_A.log"), "/TASKS=startmenuicon")
    expect(rc == 0 and registered_location() == norm(A), "silent into A again: updates (exit %d)" % rc, results)
    # An install whose uninstaller is gone (deleted by hand) doesn't block another folder
    os.replace(os.path.join(A, "unins000.exe"), os.path.join(A, "unins000.exe.moved"))
    args[7] = "/LOG=" + L("silent_B_stale.log")
    rc = subprocess.run(args).returncode
    text = log_text(L("silent_B_stale.log"))
    expect(rc == 0 and registered_location() == norm(B) and "uninstaller is gone" in text,
           "A's uninstaller gone: B installs (exit %d), and the log says why" % rc, results)
    a2 = argparse.Namespace(**vars(a))
    a2.dir = B
    silent_uninstall(a2, L("uninstall_B.log"))
    os.replace(os.path.join(A, "unins000.exe.moved"), os.path.join(A, "unins000.exe"))
    expect(not reg_present(), "cleaned up: B uninstalled, no Apps entry left", results)
    import shutil
    shutil.rmtree(A, ignore_errors=True)        # A's entry was taken over by B: removed by hand (scratch)
    expect(not os.path.exists(A) and not os.path.exists(B) and links() == (False, False),
           "cleaned up: both scratch folders and the shortcuts are gone", results)


SCENARIOS = {"disc-page": scenario_disc_page, "unverified": scenario_unverified, "buildtools": scenario_buildtools,
             "install-cycle": scenario_install_cycle, "running": scenario_running,
             "cancel": scenario_cancel, "sac": scenario_sac, "pages": scenario_pages,
             "browse": scenario_browse, "real": scenario_real, "two-folders": scenario_two_folders}


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    rest = []
    if "--" in argv:
        i = argv.index("--")
        argv, rest = argv[:i], argv[i + 1:]
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("scenario", choices=sorted(SCENARIOS))
    ap.add_argument("--setup", required=True)
    ap.add_argument("--shots", required=True, help="a folder for the screenshots, the log and test files")
    ap.add_argument("--disc", help="a usable copy (image or folder) for the positive cases")
    ap.add_argument("--unverified", help="a copy whose main program Setup doesn't recognise (unverified)")
    ap.add_argument("--folder", help="a folder of the disc's files (the browse scenario)")
    ap.add_argument("--only", default="", help="the real scenario: only these runs (letters a-d)")
    ap.add_argument("--dir2", help="a second install folder (the two-folders scenario)")
    ap.add_argument("--dir", help="the install folder Setup is shown (nothing is installed by these scenarios)")
    a = ap.parse_args(argv)
    a.rest = rest
    a.shots = os.path.abspath(a.shots)
    a.work = os.path.join(a.shots, "work")
    os.makedirs(a.work, exist_ok=True)
    a.dir = a.dir or os.path.join(a.work, "install")
    a.userdata = os.path.join(a.work, "userdata")      # uninstalls act on this, never on real saves
    results = []
    SCENARIOS[a.scenario](a, results)
    bad = [w for s, w in results if s != "PASS"]
    print("%d of %d checks passed" % (len(results) - len(bad), len(results)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
