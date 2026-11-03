#!/usr/bin/env python3
"""Play Blood Wake (the port), with a controller.

    py -3 tools/play.py              # GPU renderer, sound, controller in port 1
    py -3 tools/play.py --keyboard   # also let the keyboard act as the pad
    py -3 tools/play.py --cpu        # the CPU renderer instead (slow, about 5 fps)
    py -3 tools/play.py --windows    # the old way: a new window for each reboot
    py -3 tools/play.py --pads 2     # two controllers, whatever is plugged in
    py -3 tools/play.py --edition remastered   # Remastered (default: the launcher's choice)
    pyw -3 tools/launcher.pyw        # the settings window (play.cmd with no options)

What it does:
  - starts build/Release/bloodwake.exe with the settings the GPU path needs
    (the same as bw_run.py's r6-gpu preset, minus the diagnostics);
  - Blood Wake reboots itself between the title, the attract demo and a
    match (it relaunches its own default.xbe with launch data). The port
    records each such request in bw_relaunch.bin and the next boot starts
    with it, so it plays on like the console would;
  - one window for the whole session (src/bw_host.c, BW_HOST=1): each boot
    draws inside it, and it goes black during a reboot, as the console does.
    Alt+Enter or F11 for fullscreen. Closing it closes the game;
  - stops when the game exits without asking to reboot (the window then
    says so), or when you close it;
  - the launcher's choices (tools/launcher.pyw, saved in
    %LOCALAPPDATA%/BloodWake/settings.ini) apply to every boot: the game reads
    the file itself (src/bw_settings.c). Options given here win over it;
    --no-settings ignores it, --settings FILE reads another one;
  - --edition picks Original (the game exactly as shipped) or Remastered
    (D-015); without it, the launcher's choice, else Original. It is passed to every boot as
    BW_EDITION; each edition keeps its own saves (Original's stay where they
    always were; a new edition starts with a copy of them). --fix tunes the
    edition's changes (BW_FIX, e.g. --fix=-WIDESCREEN);
  - gives the game as many controllers as are plugged in (Xbox-style pads,
    up to 4), as a console would see them: each boot counts them again and
    one switched on while playing joins (RECOMP_USB_PADS=auto). Each player
    takes the next connected controller, whichever Windows slot it is in.
    --pads overrides with a fixed count.

The game's log goes to play.log in the repo root (all boots of the session,
one header each). Nothing here is a diagnostic run: no frame dumps, no WAV.
"""
import argparse
import itertools
import os
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(REPO, "build", "Release", "bloodwake.exe")
RELAUNCH = os.path.join(REPO, "bw_relaunch.bin")
LAUNCH_COPY = os.path.join(REPO, "bw_launch_next.bin")

BASE_ENV = {
    "RECOMP_PB_EXEC": "1",          # the pushbuffer executor
    "RECOMP_FB_WINDOW": "1",        # the game window
    "RECOMP_VBLANK": "paced",       # 60 Hz vertical blank
    "RECOMP_USB": "1",              # the USB host controller with a pad in port 1
    "RECOMP_TITLE_KEVENTS": "1",    # the game's own kernel events
    "RECOMP_AC97_READY": "1",       # sound
    "RECOMP_TRACE_BUDGET": "0",     # no trace lines
    "BW_STDERR_BUFFER": "1048576",  # buffered log
}
GPU_ENV = {"BW_GPU": "d3d11", "RECOMP_DISPATCH_GATE": "1", "RECOMP_FLIP_PACING": "1"}


def connected_pads():
    """How many XInput controllers are plugged in now (0-4)."""
    import ctypes

    class State(ctypes.Structure):
        _fields_ = [("packet", ctypes.c_uint32), ("buttons", ctypes.c_uint16),
                    ("lt", ctypes.c_uint8), ("rt", ctypes.c_uint8),
                    ("lx", ctypes.c_int16), ("ly", ctypes.c_int16),
                    ("rx", ctypes.c_int16), ("ry", ctypes.c_int16)]
    for dll in ("xinput1_4", "xinput1_3", "xinput9_1_0"):
        try:
            xi = getattr(ctypes.windll, dll)
            break
        except OSError:
            xi = None
    if xi is None:
        return 0
    st = State()
    return sum(1 for i in range(4) if xi.XInputGetState(i, ctypes.byref(st)) == 0)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--cpu", action="store_true", help="the CPU renderer instead of the GPU")
    ap.add_argument("--keyboard", action="store_true", help="the keyboard also acts as the pad")
    ap.add_argument("--boots", type=int, default=None,
                    help="stop after this many boots (tests; default: no limit)")
    ap.add_argument("--pads", type=int, default=0,
                    help="controllers the game sees (1-4; default: as many as are plugged in)")
    ap.add_argument("--windows", action="store_true",
                    help="a new window for each reboot (the launcher before the host window)")
    editions = ["original", "remastered"]
    ap.add_argument("--edition", choices=editions,
                    help="the edition (default: the launcher's choice, else original)")
    ap.add_argument("--settings", metavar="FILE",
                    help="read this settings file instead of the launcher's")
    ap.add_argument("--no-settings", action="store_true",
                    help="ignore the launcher's settings file (the game's defaults)")
    ap.add_argument("--fix", metavar="LIST",
                    help="not Original: changes on or off, e.g. +BOAT_SPLASH_RANGE "
                         "or -WIDESCREEN (BW_FIX; see src/bw_fixes.def)")
    ap.add_argument("--start", metavar="FILE",
                    help="first boot with this launch data (a bw_relaunch.bin), e.g. straight into a battle")
    a = ap.parse_args(argv)
    if a.start and not os.path.isfile(a.start):
        print("No launch data file at %s." % a.start)
        return 1

    if not os.path.isfile(EXE):
        print("No build at %s. Build first (see docs/SETUP.md)." % EXE)
        return 1
    env = {k: v for k, v in os.environ.items() if not k.startswith(("RECOMP_", "BW_"))}
    if os.environ.get("BW_SETTINGS"):
        # Which settings file the game reads (src/bw_settings.c) passes
        # through: the launcher started with BW_SETTINGS=<file> (tests) and
        # the game it starts must agree on it.
        env["BW_SETTINGS"] = os.environ["BW_SETTINGS"]
    if os.environ.get("BW_TEST_MUTEX"):
        # Tests: the session's own mutex name (src/bw_host.c, contract 1), so a
        # test launcher's Play and its host agree on it.
        env["BW_TEST_MUTEX"] = os.environ["BW_TEST_MUTEX"]
    env.update(BASE_ENV)
    if not a.cpu:
        env.update(GPU_ENV)
    if a.keyboard:
        env["RECOMP_KEYBOARD"] = "1"
    if a.pads:
        pads = max(1, min(4, a.pads))
        if pads > 1:
            env["RECOMP_USB_PADS"] = str(pads)
        print("Controllers: %d (--pads)" % pads)
    else:
        # Every boot counts the controllers itself (0.2.5 item 73, carried
        # ohci.c), and one switched on later joins the boot it lands in, so a
        # pad turned on after Play is there by the next menu.
        env["RECOMP_USB_PADS"] = "auto"
        print("Controllers: %d now; one switched on later joins the game" % connected_pads())
    # The edition travels in the environment to every boot (D-015). play.py
    # drops inherited BW_* variables above, so it is set here or not at all.
    if a.edition:
        env["BW_EDITION"] = a.edition      # set even for original: it wins over the file
    if a.no_settings:
        env["BW_SETTINGS"] = "none"
    elif a.settings:
        env["BW_SETTINGS"] = os.path.abspath(a.settings)
    if a.fix:
        env["BW_FIX"] = a.fix
    print("Edition: %s%s" % (a.edition.capitalize() if a.edition
                             else "from the settings file (Original if it has none)",
                             " (%s)" % a.fix if a.fix else ""))
    if a.start:
        # The first boot reads it (main.c); later boots get the game's own
        # requests, from the host window or the loop below.
        env["BW_LAUNCH_DATA_FILE"] = os.path.abspath(a.start)
        print("Starting from %s" % a.start)

    if not a.windows:
        env["BW_HOST"] = "1"
        if a.boots:
            # A cap is for test runs only (0.2.5 item 55): a player idling on
            # the title reboots into the attract demo and back about once a
            # minute, so the old default of 200 ended long sessions.
            env["BW_HOST_BOOTS"] = str(a.boots)
        return subprocess.call([EXE], cwd=REPO, env=env)

    launch = os.path.abspath(a.start) if a.start else None
    for boot in (range(1, a.boots + 1) if a.boots else itertools.count(1)):
        if os.path.exists(RELAUNCH):
            os.remove(RELAUNCH)
        run_env = dict(env)
        if launch:
            run_env["BW_LAUNCH_DATA_FILE"] = launch
        print("Boot %d%s ..." % (boot, " (the game's own reboot)" if launch else ""))
        with open(os.path.join(REPO, "play.log"), "w") as log:
            code = subprocess.call([EXE], cwd=REPO, env=run_env, stdout=log, stderr=log)
        if not os.path.exists(RELAUNCH):
            print("The game exited (code %d)." % code)
            return 0
        # Keep the request out of the next boot's way: it writes a new one.
        if os.path.exists(LAUNCH_COPY):
            os.remove(LAUNCH_COPY)
        os.replace(RELAUNCH, LAUNCH_COPY)
        launch = LAUNCH_COPY
    print("Stopped after %d boots." % a.boots)
    return 0


if __name__ == "__main__":
    sys.exit(main())
