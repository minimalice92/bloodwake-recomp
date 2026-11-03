#!/usr/bin/env python3
"""Blood Wake Recomp's setup helper. The installer runs it with the private
Python it adds to the install folder (python\\python.exe -I -u setup_helper.py ...).

    setup_helper.py deps --wheels <dir>
        pip from Python's own bundled wheel (offline), then the wheels Setup
        downloaded and SHA-256-checked (capstone, pefile, and CMake when
        Visual Studio has none), with no index and no other packages.
    setup_helper.py build --source <dir> [--stub] [--cancel-file <file>] -- <build_from_disc.py arguments>
        tools/build_from_disc.py from the installed source, with a clean
        environment (no PYTHONHOME/PYTHONPATH, no per-user site-packages, so
        another Python on this PC can't change what runs). Its JSON progress
        lines pass straight through; the exit code is the script's. The build
        runs in a job object: when the cancel file appears (Setup's Cancel),
        the build and every process it started are stopped, and the exit code
        is 8.
        --stub (testing only) prints a short fake run instead and writes
        placeholder files, so the installer's pages, shortcuts and uninstaller
        can be tested in seconds.
    setup_helper.py record --install-json <file> key=value ...
        adds an "installer" block to install.json (what Setup added, so the
        uninstaller and a later Setup know).

Every line it prints itself is one JSON object, like build_from_disc.py's
--progress json: {"step": ..., "pct": 0-100, "msg": ...}. Stdlib only.
"""
import glob
import json
import os
import shutil
import subprocess
import sys
import time

PINS = {"capstone": "5.0.9", "pefile": "2024.8.26"}


PROGRESS_FILE = None        # --progress-file: Setup reads the lines from here (item 10)


def emit(line):
    """One JSON line to stdout and, when Setup asked for it, to the progress
    file. The file is opened for each line, so Setup can read it in between."""
    sys.stdout.write(line + "\n")
    sys.stdout.flush()
    if PROGRESS_FILE:
        for _ in range(100):
            try:
                with open(PROGRESS_FILE, "a", encoding="utf-8") as f:
                    f.write(line + "\n")
                return
            except OSError:
                time.sleep(0.05)       # Setup is reading it this moment


def say(step, pct, msg, **extra):
    d = {"step": step, "pct": int(pct), "msg": msg}
    d.update(extra)
    emit(json.dumps(d))


EXIT_CANCELLED = 8          # the player stopped the build (Setup's exit code 108)


class Job:
    """A Windows job object that ends every process in it when it's closed or
    stopped: the build and everything it starts (the translation, CMake,
    MSBuild, the compilers, the warm-up's game runs). Setup's Cancel stops it
    (0.2.5 item 10)."""

    def __init__(self):
        import ctypes
        from ctypes import wintypes as W
        self.k = ctypes.WinDLL("kernel32", use_last_error=True)
        self.k.CreateJobObjectW.restype = W.HANDLE
        self.k.CreateJobObjectW.argtypes = [ctypes.c_void_p, W.LPCWSTR]
        self.k.SetInformationJobObject.argtypes = [W.HANDLE, ctypes.c_int, ctypes.c_void_p, W.DWORD]
        self.k.AssignProcessToJobObject.argtypes = [W.HANDLE, W.HANDLE]
        self.k.TerminateJobObject.argtypes = [W.HANDLE, W.UINT]
        self.k.OpenProcess.restype = W.HANDLE
        self.k.OpenProcess.argtypes = [W.DWORD, W.BOOL, W.DWORD]
        self.k.CloseHandle.argtypes = [W.HANDLE]
        self.h = self.k.CreateJobObjectW(None, None)

        class BASIC(ctypes.Structure):
            _fields_ = [("PerProcessUserTimeLimit", ctypes.c_int64), ("PerJobUserTimeLimit", ctypes.c_int64),
                        ("LimitFlags", W.DWORD), ("MinimumWorkingSetSize", ctypes.c_size_t),
                        ("MaximumWorkingSetSize", ctypes.c_size_t), ("ActiveProcessLimit", W.DWORD),
                        ("Affinity", ctypes.c_size_t), ("PriorityClass", W.DWORD), ("SchedulingClass", W.DWORD)]

        class IO(ctypes.Structure):
            _fields_ = [(n, ctypes.c_uint64) for n in ("r", "w", "o", "rb", "wb", "ob")]

        class EXT(ctypes.Structure):
            _fields_ = [("Basic", BASIC), ("Io", IO), ("ProcessMemoryLimit", ctypes.c_size_t),
                        ("JobMemoryLimit", ctypes.c_size_t), ("PeakProcessMemoryUsed", ctypes.c_size_t),
                        ("PeakJobMemoryUsed", ctypes.c_size_t)]
        info = EXT()
        info.Basic.LimitFlags = 0x2000          # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        if not self.h or not self.k.SetInformationJobObject(self.h, 9, ctypes.byref(info), ctypes.sizeof(info)):
            self.h = None                       # no job: Cancel then stops only the build's own process

    def add(self, pid):
        if not self.h:
            return False
        p = self.k.OpenProcess(0x0101, False, pid)  # PROCESS_TERMINATE | PROCESS_SET_QUOTA
        ok = bool(p) and bool(self.k.AssignProcessToJobObject(self.h, p))
        if p:
            self.k.CloseHandle(p)
        return ok

    def stop(self):
        if self.h:
            self.k.TerminateJobObject(self.h, EXIT_CANCELLED)


def watch_cancel(flag, on_cancel):
    """Calls on_cancel() once the flag file appears (Setup's Cancel writes it)."""
    import threading

    def loop():
        while True:
            if os.path.exists(flag):
                on_cancel()
                return
            time.sleep(0.5)
    t = threading.Thread(target=loop, daemon=True)
    t.start()
    return t


def clean_env():
    """This process's environment without anything that points Python elsewhere."""
    env = {k: v for k, v in os.environ.items()
           if not (k.upper() in ("PYTHONHOME", "PYTHONPATH", "PYTHONSTARTUP", "PYTHONUSERBASE",
                                 "PYTHONEXECUTABLE", "VIRTUAL_ENV", "CONDA_PREFIX", "__PYVENV_LAUNCHER__")
                   or k.upper().startswith("PIP_"))}
    env["PYTHONNOUSERSITE"] = "1"
    env["PYTHONIOENCODING"] = "utf-8"
    env["PYTHONDONTWRITEBYTECODE"] = "1"
    env["MSBUILDDISABLENODEREUSE"] = "1"   # MSBuild's worker processes end with the build
    scripts = os.path.join(os.path.dirname(sys.executable), "Scripts")
    env["PATH"] = scripts + os.pathsep + env.get("PATH", "")      # pip's cmake.exe, when used
    return env


def run_quiet(cmd, log):
    with open(log, "a", encoding="utf-8", errors="replace") as f:
        f.write("$ %s\n" % subprocess.list2cmdline(cmd))
        f.flush()
        return subprocess.run(cmd, stdout=f, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                              env=clean_env()).returncode


def installed(name):
    try:
        from importlib.metadata import version
        return version(name)
    except Exception:
        return None


def cmd_deps(a):
    wheels = sorted(glob.glob(os.path.join(a.wheels, "*.whl")))
    log = os.path.join(a.wheels, "pip.log")
    if not any(os.path.basename(w).lower().startswith("capstone-") for w in wheels):
        say("deps", 0, "capstone's wheel is missing from %s" % a.wheels, error=True, exit=3)
        return 3
    if installed("pip") is None:
        say("deps", 0, "setting up pip (from Python's own copy, offline)")
        if run_quiet([sys.executable, "-I", "-m", "ensurepip", "--default-pip"], log) != 0:
            say("deps", 0, "Python's pip setup failed. Log: %s" % log, error=True, exit=3)
            return 3
    say("deps", 50, "installing %s" % ", ".join(os.path.basename(w).split("-")[0] for w in wheels))
    rc = run_quiet([sys.executable, "-I", "-m", "pip", "install", "--isolated", "--no-index", "--no-deps",
                    "--disable-pip-version-check", "--no-warn-script-location", "--no-cache-dir",
                    "--upgrade"] + wheels, log)
    if rc != 0:
        say("deps", 0, "Installing the Python packages failed. Log: %s" % log, error=True, exit=3)
        return 3
    # A fresh process, so the check sees what pip just wrote.
    out = subprocess.run([sys.executable, "-I", "-c",
                          "import json; from importlib.metadata import version as v\n"
                          "print(json.dumps({n: v(n) for n in %r}))" % sorted(PINS)],
                         capture_output=True, text=True, env=clean_env())
    try:
        got = json.loads(out.stdout)
    except ValueError:
        got = {}
    bad = ["%s %s (found %s)" % (n, want, got.get(n)) for n, want in sorted(PINS.items()) if got.get(n) != want]
    if bad:
        say("deps", 0, "Wrong package versions after install: %s. Log: %s" % (", ".join(bad), log),
            error=True, exit=3)
        return 3
    say("deps", 100, "Python packages ready: %s" % ", ".join("%s %s" % kv for kv in sorted(got.items())))
    return 0


def stub_build(argv, cancel_file=None):
    """A fake build_from_disc run: the same JSON lines, placeholder files. Testing only.
    BW_SETUP_STUB_SECONDS stretches it (for testing Cancel)."""
    inst = argv[argv.index("--install-dir") + 1]
    pause = float(os.environ.get("BW_SETUP_STUB_SECONDS", "5")) / 13.0
    fail = os.environ.get("BW_SETUP_STUB_FAIL")
    reason = os.environ.get("BW_SETUP_STUB_REASON")       # e.g. "files differ": fails until --allow-unverified
    if reason and "--allow-unverified" in argv:
        fail = None
    steps = [("identify", 1, "a folder of the disc's files (stub)"), ("toolkit", 3, "xboxrecomp (stub)"),
             ("unpack", 10, "copying 3 of 17 files (stub)"), ("unpack", 18, "copying 17 of 17 files (stub)"),
             ("fingerprint", 20, "an unmodified retail copy of Blood Wake (stub)"),
             ("pipeline", 40, "translating the game's code: disasm (stub)"),
             ("pipeline", 68, "translated in 0 s (stub)"), ("verify", 70, "the generated code matches ours (stub)"),
             ("build", 85, "compiling (stub)"), ("build", 97, "built in 0 s (stub)"),
             ("stage", 84, "installed in %s (stub)" % inst),
             ("warmup", 90, "preparing graphics: 1 of 4 scenes (stub)"),
             ("warmup", 99, "graphics ready: 0 shaders prepared in 0 s (stub)")]
    for step, pct, msg in steps:
        if fail and step == "fingerprint":
            extra = {"reason": reason, "detail": "1 of its files differs (stub.txt)"} if reason else {}
            say("fingerprint", 0, "This isn't an unmodified retail copy of Blood Wake (stub failure): 1 file "
                "differs from the retail disc (stub.txt).", error=True, exit=int(fail), **extra)
            return int(fail)
        say(step, pct, msg)
        t0 = time.time()
        while time.time() - t0 < pause:
            if cancel_file and os.path.exists(cancel_file):
                say("cancelled", 0, "You stopped the build (stub).", error=True, exit=EXIT_CANCELLED)
                return EXIT_CANCELLED
            time.sleep(0.1)
    for rel in ("build/Release/bloodwake.exe", "game/disc/stub.txt", "work/logs/stub.log"):
        p = os.path.join(inst, *rel.split("/"))
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "w") as f:
            f.write("placeholder written by setup_helper.py --stub\n")
    src = argv[argv.index("--source") + 1] if "--source" in argv else None
    os.makedirs(os.path.join(inst, "tools"), exist_ok=True)
    if src:
        for name in ("launcher.pyw", "play.py"):
            if os.path.isfile(os.path.join(src, "tools", name)):
                shutil.copy2(os.path.join(src, "tools", name), os.path.join(inst, "tools", name))
    with open(os.path.join(inst, "install.json"), "w", encoding="utf-8") as f:
        json.dump({"format": 1, "stub": True}, f, indent=2)
    say("done", 100, "Blood Wake is built (stub).")
    return 0


def cmd_build(a, rest):
    script = os.path.join(a.source, "tools", "build_from_disc.py")
    if a.cancel_file and os.path.exists(a.cancel_file):
        os.remove(a.cancel_file)                 # an earlier run's
    if a.stub:
        return stub_build(rest + ["--source", a.source], a.cancel_file)
    if not os.path.isfile(script):
        say("identify", 0, "The installed source is incomplete (no %s). Run Setup again." % script,
            error=True, exit=7)
        return 7
    cmd = [sys.executable, "-u", script] + rest + ["--progress", "json"]
    job = Job()
    p = subprocess.Popen(cmd, cwd=a.source, env=clean_env(), stdin=subprocess.DEVNULL,
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         text=True, encoding="utf-8", errors="replace")
    in_job = job.add(p.pid)
    cancelled = []

    def cancel():
        cancelled.append(time.time())
        say("log", -1, "Cancel pressed: stopping the build and everything it started")
        if in_job:
            job.stop()
        else:
            p.kill()
    if a.cancel_file:
        watch_cancel(a.cancel_file, cancel)
    for line in p.stdout:
        line = line.rstrip("\r\n")
        if line.startswith("{"):
            emit(line)
        elif line.strip():                       # anything else (a traceback): keep it, as JSON
            say("log", -1, line)
    rc = p.wait()
    if cancelled:
        say("cancelled", 0, "You stopped the build. Nothing of it is left running.", error=True,
            exit=EXIT_CANCELLED)
        return EXIT_CANCELLED
    return rc


def cmd_record(a):
    try:
        with open(a.install_json, encoding="utf-8") as f:
            d = json.load(f)
    except (OSError, ValueError):
        d = {"format": 1}
    block = d.setdefault("installer", {})
    for kv in a.pairs:
        k, _, v = kv.partition("=")
        block[k] = True if v == "true" else False if v == "false" else v
    block["python"] = sys.version.split()[0]
    with open(a.install_json, "w", encoding="utf-8") as f:
        json.dump(d, f, indent=2)
    say("record", 100, "recorded in %s" % a.install_json)
    return 0


def main(argv=None):
    import argparse
    argv = list(sys.argv[1:] if argv is None else argv)
    rest = []
    if "--" in argv:
        i = argv.index("--")
        argv, rest = argv[:i], argv[i + 1:]
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    d = sub.add_parser("deps")
    d.add_argument("--wheels", required=True)
    b = sub.add_parser("build")
    b.add_argument("--source", required=True)
    b.add_argument("--stub", action="store_true")
    b.add_argument("--cancel-file", help="Setup's Cancel creates this file: the build and everything it "
                                         "started stop, and the exit code is %d" % EXIT_CANCELLED)
    b.add_argument("--progress-file", help="also write every progress line here, the first one with this "
                                           "process's id (Setup polls it, so its window stays live)")
    r = sub.add_parser("record")
    r.add_argument("--install-json", required=True)
    r.add_argument("pairs", nargs="*")
    a = ap.parse_args(argv)
    if a.cmd == "deps":
        return cmd_deps(a)
    if a.cmd == "build":
        global PROGRESS_FILE
        if a.progress_file:
            PROGRESS_FILE = a.progress_file
            if os.path.exists(PROGRESS_FILE):
                os.remove(PROGRESS_FILE)
            say("pid", -1, "", pid=os.getpid())
        rc = cmd_build(a, rest)
        say("exit", -1, "", code=rc)
        return rc
    return cmd_record(a)


if __name__ == "__main__":
    sys.exit(main())
