"""Automated check of the play launcher's host window (src/bw_host.c).

Starts the exe as tools/play.py does (BW_HOST=1, the same env), then for
--seconds: counts the visible top-level windows of the game (want: one
BloodWakeHost, no top-level XboxRecompFramebuffer), notes the title bar and
whether the frame child is inside, screenshots the host, toggles fullscreen
and back, then closes the window as a person would (WM_CLOSE) and checks
every bloodwake.exe is gone. Screenshots stay local (D-005).

    py -3 tools/bw_host_test.py --seconds 110 --out <dir>   (no other bloodwake.exe running)
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import os
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "tools"))
import play  # noqa: E402
from bw_frames import write_png  # noqa: E402

u32 = ctypes.WinDLL("user32", use_last_error=True)
g32 = ctypes.WinDLL("gdi32")
WNDENUMPROC = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
u32.FindWindowExA.restype = wt.HWND
u32.FindWindowExA.argtypes = [wt.HWND, wt.HWND, ctypes.c_char_p, ctypes.c_char_p]
u32.GetDC.restype = wt.HDC
u32.GetDC.argtypes = [wt.HWND]
u32.ReleaseDC.argtypes = [wt.HWND, wt.HDC]
u32.PrintWindow.argtypes = [wt.HWND, wt.HDC, wt.UINT]
u32.PostMessageA.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
u32.GetClientRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
u32.GetWindowRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
u32.IsWindow.argtypes = [wt.HWND]
g32.CreateCompatibleDC.restype = wt.HDC
g32.CreateCompatibleDC.argtypes = [wt.HDC]
g32.CreateDIBSection.restype = wt.HBITMAP
g32.CreateDIBSection.argtypes = [wt.HDC, ctypes.c_void_p, wt.UINT, ctypes.POINTER(ctypes.c_void_p),
                                 wt.HANDLE, wt.DWORD]
g32.SelectObject.restype = wt.HGDIOBJ
g32.SelectObject.argtypes = [wt.HDC, wt.HGDIOBJ]
g32.DeleteObject.argtypes = [wt.HGDIOBJ]
g32.DeleteDC.argtypes = [wt.HDC]
g32.BitBlt.argtypes = [wt.HDC, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                       wt.HDC, ctypes.c_int, ctypes.c_int, wt.DWORD]


def text_of(h):
    b = ctypes.create_unicode_buffer(256)
    u32.GetWindowTextW(h, b, 256)
    return b.value


def class_of(h):
    b = ctypes.create_unicode_buffer(128)
    u32.GetClassNameW(h, b, 128)
    return b.value


def game_pids():
    out = subprocess.run(["tasklist", "/fo", "csv", "/nh", "/fi", "imagename eq bloodwake.exe"],
                         capture_output=True, text=True).stdout
    return [int(l.split('","')[1]) for l in out.splitlines() if l.startswith('"bloodwake')]


def top_windows(pids):
    found = []

    def cb(h, _):
        pid = wt.DWORD()
        u32.GetWindowThreadProcessId(h, ctypes.byref(pid))
        if pid.value in pids and u32.IsWindowVisible(h):
            found.append((h, class_of(h), text_of(h), pid.value))
        return True
    u32.EnumWindows(WNDENUMPROC(cb), 0)
    return found


def rect(h, client=False):
    r = wt.RECT()
    (u32.GetClientRect if client else u32.GetWindowRect)(h, ctypes.byref(r))
    return (r.left, r.top, r.right, r.bottom)


def shot(h, path):
    """The host's client area: PrintWindow with PW_RENDERFULLCONTENT (DWM's
    copy, children included), else the screen."""
    l, t, r, b = rect(h, client=True)
    w, hh = r - l, b - t
    if w <= 0 or hh <= 0:
        return "empty"

    class BIH(ctypes.Structure):
        _fields_ = [("biSize", wt.DWORD), ("biWidth", wt.LONG), ("biHeight", wt.LONG),
                    ("biPlanes", wt.WORD), ("biBitCount", wt.WORD), ("biCompression", wt.DWORD),
                    ("biSizeImage", wt.DWORD), ("biXPelsPerMeter", wt.LONG),
                    ("biYPelsPerMeter", wt.LONG), ("biClrUsed", wt.DWORD), ("biClrImportant", wt.DWORD)]
    bih = BIH(ctypes.sizeof(BIH), w, -hh, 1, 32, 0, 0, 0, 0, 0, 0)
    sdc = u32.GetDC(h)
    mdc = g32.CreateCompatibleDC(sdc)
    bits = ctypes.c_void_p()
    bmp = g32.CreateDIBSection(mdc, ctypes.byref(bih), 0, ctypes.byref(bits), None, 0)
    old = g32.SelectObject(mdc, bmp)
    how = "PrintWindow"
    if not u32.PrintWindow(h, mdc, 3):    # PW_CLIENTONLY | PW_RENDERFULLCONTENT
        g32.BitBlt(mdc, 0, 0, w, hh, sdc, 0, 0, 0x00CC0020)
        how = "BitBlt"
    raw = ctypes.string_at(bits, w * hh * 4)
    if not any(raw[i] > 24 or raw[i + 1] > 24 or raw[i + 2] > 24 for i in range(0, len(raw) - 3, 4 * 997)):
        # A D3D11 swap chain owned by the game process is invisible to
        # PrintWindow: take what DWM put on screen instead (window on top).
        pt = wt.POINT(0, 0)
        u32.ClientToScreen(h, ctypes.byref(pt))
        u32.SetForegroundWindow(h)
        time.sleep(0.3)
        scr = u32.GetDC(None)
        g32.BitBlt(mdc, 0, 0, w, hh, scr, pt.x, pt.y, 0x00CC0020)
        u32.ReleaseDC(None, scr)
        how = "screen"
        raw = ctypes.string_at(bits, w * hh * 4)
    rows = []
    nonblack = 0
    for y in range(hh):
        line = raw[y * w * 4:(y + 1) * w * 4]
        rgb = bytearray(w * 3)
        rgb[0::3] = line[2::4]
        rgb[1::3] = line[1::4]
        rgb[2::3] = line[0::4]
        if y % 8 == 0:
            nonblack += sum(1 for v in rgb[::24] if v > 24)
        rows.append(bytes(rgb))
    write_png(path, w, hh, rows)
    g32.SelectObject(mdc, old)
    g32.DeleteObject(bmp)
    g32.DeleteDC(mdc)
    u32.ReleaseDC(h, sdc)
    return "%s %dx%d, non-black samples %d" % (how, w, hh, nonblack)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=int, default=100)
    ap.add_argument("--out", required=True)
    ap.add_argument("--pad", help="RECOMP_PAD_SCRIPT for the boots")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    if game_pids():
        print("another bloodwake.exe is running; stop it first")
        return 2

    env = {k: v for k, v in os.environ.items() if not k.startswith(("RECOMP_", "BW_"))}
    env.update(play.BASE_ENV)
    env.update(play.GPU_ENV)
    env.update(BW_HOST="1", BW_HOST_LINGER="0")
    # A test boot never uses the maintainer's saves or game disk (lane au-iso):
    # its own under --out, as tools/bw_run.py gives every run.
    env.update(RECOMP_SAVE_DIR=os.path.abspath(os.path.join(a.out, "saves")),
               BW_EDITION_SAVE_ROOT=os.path.abspath(os.path.join(a.out, "disk")))
    if a.pad:
        env["RECOMP_PAD_SCRIPT"] = a.pad
    host = subprocess.Popen([play.EXE], cwd=REPO, env=env)
    t0 = time.time()
    hostwin = None
    problems = []
    seen_pids = set()
    shots = {int(a.seconds * 0.4), a.seconds - 10}
    did_full = False
    last = None
    while time.time() - t0 < a.seconds:
        time.sleep(2)
        t = int(time.time() - t0)
        if host.poll() is not None:
            problems.append("host exited early (code %s) at %d s" % (host.returncode, t))
            break
        pids = set(game_pids())
        seen_pids |= pids
        wins = top_windows(pids)
        hosts = [w for w in wins if w[1] == "BloodWakeHost"]
        frames = [w for w in wins if w[1] == "XboxRecompFramebuffer"]
        if frames:
            problems.append("%d s: a top-level frame window: %s" % (t, frames))
        if len(hosts) != 1:
            problems.append("%d s: %d host windows" % (t, len(hosts)))
            continue
        hostwin = hosts[0][0]
        child = u32.FindWindowExA(hostwin, None, b"XboxRecompFramebuffer", None)
        state = (len(pids), bool(child), hosts[0][2][:60])
        if state != last:
            print("%4d s: processes %d, frame child %s, title %r" % ((t,) + state))
            last = state
        if child:
            hc, cc = rect(hostwin, True), rect(child, True)
            if (hc[2] - hc[0], hc[3] - hc[1]) != (cc[2] - cc[0], cc[3] - cc[1]):
                problems.append("%d s: child %s does not fill host %s" % (t, cc, hc))
        for s in sorted(shots):
            if t >= s:
                shots.discard(s)
                print("%4d s: screenshot: %s" % (t, shot(hostwin, os.path.join(a.out, "host_%03d.png" % t))))
        if child and not did_full and t >= a.seconds * 0.6:
            did_full = True
            before = rect(hostwin)
            u32.PostMessageA(hostwin, 0x8000 + 0x42, 0, 0)
            time.sleep(1.5)
            full = rect(hostwin)
            c2 = u32.FindWindowExA(hostwin, None, b"XboxRecompFramebuffer", None)
            cfull = rect(c2, True) if c2 else None
            print("%4d s: fullscreen: host %s -> %s, child client %s" % (t, before, full, cfull))
            print("%4d s: screenshot: %s" % (t, shot(hostwin, os.path.join(a.out, "host_full.png"))))
            u32.PostMessageA(hostwin, 0x8000 + 0x42, 0, 0)
            time.sleep(1.5)
            back = rect(hostwin)
            print("%4d s: windowed again: %s" % (t, back))
            if back != before:
                problems.append("window rect not restored: %s vs %s" % (back, before))
    # Close it as a person would.
    if hostwin and u32.IsWindow(hostwin):
        tc = time.time()
        u32.PostMessageA(hostwin, 0x0010, 0, 0)        # WM_CLOSE
        try:
            host.wait(15)
            print("closed: host exited in %.1f s (code %s)" % (time.time() - tc, host.returncode))
        except subprocess.TimeoutExpired:
            problems.append("host did not exit within 15 s of WM_CLOSE")
            host.kill()
    else:
        host.kill()
    time.sleep(1)
    left = game_pids()
    if left:
        problems.append("processes left after close: %s" % left)
        for p in left:
            subprocess.run(["taskkill", "/f", "/pid", str(p)], capture_output=True)
    print("game processes seen:", len(seen_pids))
    log = os.path.join(REPO, "play.log")
    if os.path.exists(log):
        with open(log, "rb") as f:
            heads = [l.decode("ascii", "replace").strip() for l in f if l.startswith(b"=== [HOST]")]
        print("play.log:", "; ".join(heads))
    print("PROBLEMS:" if problems else "no problems", *problems, sep="\n  ")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
