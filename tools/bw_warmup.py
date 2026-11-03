#!/usr/bin/env python3
"""Prepare graphics (one time): play a few short scenes, hidden, so the game's
pixel shaders are compiled and cached before the first real play.

    python tools/bw_warmup.py --exe <install>/build/Release/bloodwake.exe
        --game-dir <install>/game/disc [--work-dir <dir>] [--parallel 2]
        [--only <scene,...>] [--cache <folder>] [--list] [--progress json]

Why: the PC back end turns each of the game's graphics combiner states into a
pixel shader and compiles it the first time a frame needs it (15-60 ms each,
and a new scene needs a dozen at once), so a player's first minutes stutter.
Every compiled shader is kept in the shader cache (src/gpu/bw_pscache.c,
%LOCALAPPDATA%/xboxrecomp/shadercache), so this only has to happen once per
PC. This script makes that once happen during setup instead of during play.

How: each scene is a boot of the installed game with its window hidden and
its sound at -120 dB, straight into a level by a launch page this script
writes (below), for a fixed time (the game's own BW_RUN_SECONDS, with a hard
limit on top); a scene may follow the game's own reboot, as tools/play.py
does. Saved games go to the scene's own folder, deleted afterwards, so the
player's saves, settings and progress are never touched. (Original, like any
Original play, opens the console disk images and utility-drive caches in
%LOCALAPPDATA%/xboxrecomp; the toolkit keeps them there whatever the save
folder.) Only one game at a time opens those images for writing, so two
Original scenes never run at once. Besides those it writes only the shader
cache. Nothing it makes comes from the game: the launch pages are built from
the fields below.

Settings: the shader text depends on some settings (anti-aliasing, the
Remastered sharpening of text and HUD edges...). The scenes play with a copy
in the work folder: of the player's settings file if it exists (where the
game itself looks: the "file" of --settings-schema) without its [advanced]
raw switches, else of what a fresh launcher starts with (the default preset's
values, else each setting's default). BASE_ENV below pins the window, pads
and sound and wins over the file (fullscreen included, so the scenes stay
hidden).

Counts: the headline numbers come from the shader cache folder itself (its
ps_*.bin files before and after), so they count distinct shaders and include
a scene stopped for running over time. The per-scene counts come from each
boot's log and are only a guide (the game stops logging every shader after
its first 200).

--progress json prints {"step": "warmup", "pct": 0-100, "msg": ...} lines
(pct of the warm-up). It never fails a build: errors are reported and the
scene is skipped (exit 0; 1 only for bad arguments). Stdlib only.
"""
import argparse
import configparser
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import time

# ── The launch page ───────────────────────────────────────────────────────
# Blood Wake loads a level by rebooting into itself with a 3072-byte launch
# page (the port's main.c rebuilds it from a "BWRL" file, BW_LAUNCH_DATA_FILE).
# Its fields, as the port's own runs recorded them (meanings in brackets are
# our reading; "?" = not known, the value the menus wrote):
PAGE_SIZE = 3072
TAG = (0, b"SPLASH")            # the page's tag
MODE = 8                        # u32: 1 boots a level
SAVE_DEVICE = (24, b"U:\\")     # the save device
LEVEL = 600                     # ASCII: "01".."28" Story Mode missions, "sm_<arena>" Battle
HUMANS = 640                    # u8: human players (split screen)
STORY_FLAGS = {2556: 1, 2560: 1, 2680: 1, 2992: 5}     # u8s (?)
BATTLE_FLAGS = {544: 1, 2560: 1, 2680: 1, 2992: 5}     # u8s (?)
BATTLE_RULES = 592              # u8: the battle game (1, ...)
BATTLE_ARENA2 = 558             # ASCII: the arena again
BOATS = 644                     # boat records, 132 bytes each, one per human player:
BOAT_STRIDE = 132               #   +0 class (ASCII, 32), +32 model (ASCII, 32),
BOAT_TEAM = 128                 #   +64 the shown name (UTF-16, 32 chars), +128 u8 team
PADS = 1975                     # u8 per player: the controller port (0xFF none)
MENU_FLAGS = {544: 1, 625: 1, 628: 3, 632: 1, 676: 1}  # u8s (?): the attract demo's return
MENU_FROM = (636, b"attract_01")                       # the level it returns from


def _put(page, off, data):
    page[off:off + len(data)] = data


def _boat(page, i, cls, model, team):
    o = BOATS + BOAT_STRIDE * i
    _put(page, o, cls.encode("ascii"))
    _put(page, o + 32, model.encode("ascii"))
    _put(page, o + 64, model.encode("utf-16-le"))
    page[o + BOAT_TEAM] = team


def story_page(level):
    """Story Mode mission `level` ("01".."28"), one player, the first save slot."""
    p = bytearray(PAGE_SIZE)
    _put(p, *TAG)
    struct.pack_into("<I", p, MODE, 1)
    _put(p, *SAVE_DEVICE)
    _put(p, LEVEL, level.encode("ascii"))
    p[HUMANS] = 1
    for off, v in STORY_FLAGS.items():
        p[off] = v
    return bytes(p)


def title_page():
    """The title screen (mode 0, nothing else)."""
    p = bytearray(PAGE_SIZE)
    _put(p, *TAG)
    struct.pack_into("<I", p, MODE, 0)
    return bytes(p)


def menus_page():
    """Straight to the main menu, as the attract demo returns to it."""
    p = bytearray(title_page())
    for off, v in MENU_FLAGS.items():
        p[off] = v
    _put(p, *MENU_FROM)
    return bytes(p)


def battle_page(arena, humans, rules=1, boat=("Catamaran", "Hellcat")):
    """A Battle Mode game in `arena` for `humans` split-screen players, each in
    `boat`, alternate teams, controller ports 0..humans-1."""
    p = bytearray(PAGE_SIZE)
    _put(p, *TAG)
    struct.pack_into("<I", p, MODE, 1)
    _put(p, *SAVE_DEVICE)
    _put(p, LEVEL, arena.encode("ascii"))
    _put(p, BATTLE_ARENA2, arena.encode("ascii"))
    p[HUMANS] = humans
    p[BATTLE_RULES] = rules
    for off, v in BATTLE_FLAGS.items():
        p[off] = v
    for i in range(humans):
        _boat(p, i, boat[0], boat[1], i % 2)
    for i in range(4):
        p[PADS + i] = i if i < humans else 0xFF
    return bytes(p)


def launch_file(page, path):
    """The port's launch file (main.c): "BWRL", version 1, the image path, the page."""
    img = b"D:\\default.xbe"
    data = b"BWRL" + struct.pack("<II", 1, len(img)) + img + struct.pack("<I", len(page)) + page
    with open(path, "wb") as f:
        f.write(data)
    return path


# ── The scenes ────────────────────────────────────────────────────────────
# Chosen from per-scene shader sets measured with an empty cache: every
# mission, the Battle games, 2- and 4-player split screen and the menus, in
# both editions, 60 s each (68 boots, 44 different shaders, 25 of them the
# same in both editions, so Original only has to add its own 4). Counting one
# more of the title's that no 60 s scene showed, 45 have ever been seen; these
# seven scenes compile 44 of the 45 in the least play time (docs/day1/W2.md).
# The missing one came once, from enemy action against an idle boat, and the
# first play compiles it if it ever comes.
# `seconds` is the scene's limit from its first boot's start (so it includes
# the loads), with room for a slower PC: the last needed shader came at
# 3-17 s (title, menus, split), 33-38 s (missions 27 and 23) and, in
# mission 23 Original, right after the game's own reboot at the mission's
# end (about 45 s). `follow`: how many of the game's own reboots to follow.
SCENES = [
    # name, edition, page, pads, pad script, seconds, follow
    ("mission23", "original", story_page("23"), 1, None, 65, 1),
    ("mission23", "remastered", story_page("23"), 1, None, 50, 0),
    ("mission27", "original", story_page("27"), 1, None, 45, 0),
    ("title", "remastered", title_page(), 1, None, 25, 0),
    ("split4", "remastered", battle_page("sm_octopusbay", 4, rules=2), 4, "9000:start", 20, 0),
    ("menus", "remastered", menus_page(), 1, None, 15, 0),
    ("title", "original", title_page(), 1, None, 15, 0),
]


def scene_list():
    """The scenes, longest first (so two workers finish together; warmup()
    never runs two Original scenes at once)."""
    return [dict(zip(("name", "edition", "page", "pads", "script", "seconds", "follow"), s))
            for s in sorted(SCENES, key=lambda s: -s[5])]


# ── Settings ──────────────────────────────────────────────────────────────

def local_appdata():
    """The folder the game keeps its files in (SHGetFolderPath CSIDL_LOCAL_APPDATA),
    else the environment's LOCALAPPDATA."""
    try:
        import ctypes
        buf = ctypes.create_unicode_buffer(260)
        if ctypes.windll.shell32.SHGetFolderPathW(None, 0x1C, None, 0, buf) == 0 and buf.value:
            return buf.value
    except (ImportError, AttributeError, OSError):
        pass
    return os.environ.get("LOCALAPPDATA")


def read_schema(exe, cwd):
    """The game's settings list (bloodwake.exe --settings-schema)."""
    r = subprocess.run([exe, "--settings-schema"], capture_output=True, timeout=30,
                       cwd=cwd, creationflags=_NO_WINDOW)
    return json.loads(r.stdout.decode("utf-8", "replace"))


WINDOWED = re.compile(r"(?i)^(\s*mode\s*=\s*)\S.*?(\r?\n?)$")
FORMAT = re.compile(r"(?i)^(\s*format\s*=\s*)\S.*?(\r?\n?)$")


def without_advanced(src, out):
    """Copy the settings file `src` to `out` without its [advanced] section: raw
    switches, any of which could show a window or change a scene. Sections are
    found as the game finds them (bw_settings.c read_file); bytes kept as they are,
    but for [display] mode, which always says window (item 102: a player's
    Fullscreen must never reach a hidden scene; RECOMP_FULLSCREEN=0 in BASE_ENV
    wins over the file anyway, this keeps the file itself saying so), and
    [launcher] format, which always says 2, so that window is read as a choice
    (call 85 reads a format-1 window as Fullscreen)."""
    with open(src, encoding="latin-1", newline="") as f:
        lines = f.read().splitlines(True)
    keep, sec, launcher = [], "", False
    for ln in lines:
        t = ln.strip()
        if t.startswith("[") and "]" in t:
            sec = t[1:t.index("]")].lower()
            launcher = launcher or sec == "launcher"
        if sec == "display" and WINDOWED.match(ln):
            ln = WINDOWED.sub(lambda m: m.group(1) + "window" + m.group(2), ln)
        if sec == "launcher" and FORMAT.match(ln):
            ln = FORMAT.sub(lambda m: m.group(1) + "2" + m.group(2), ln)
        if sec != "advanced":
            keep.append(ln)
    if not launcher:
        keep.append("\n[launcher]\nformat = 2\n")
    with open(out, "w", encoding="latin-1", newline="") as f:
        f.write("; Blood Wake settings for the graphics warm-up (tools/bw_warmup.py):\n"
                "; the player's file without [advanced]. Not the player's file.\n\n")
        f.writelines(keep)
    return out


def scene_settings(exe, out):
    """Write the settings file the scenes play with to `out`. Returns (its path,
    or None for the game's own defaults; what it is; the game's settings list,
    or None if it couldn't be read)."""
    schema = None
    try:
        schema = read_schema(exe, os.path.dirname(out))
    except (OSError, ValueError, subprocess.SubprocessError):
        pass
    player = (schema or {}).get("file")
    if not player:
        base = local_appdata()
        player = os.path.join(base, "BloodWake", "settings.ini") if base else None
    if player and os.path.isfile(player):
        return without_advanced(player, out), "the player's settings, without [advanced]", schema
    if schema:
        try:
            return default_settings(schema, out), "a new launcher's settings", schema
        except (KeyError, TypeError, ValueError):
            pass
    return None, "the game's defaults (its settings list couldn't be read)", schema


def launcher_values(schema, path):
    """Each setting's choice as tools/launcher.pyw shows it for the file at
    `path`: the file's, if it is one of the setting's choices, else the default
    preset's, else the setting's default. {"section.key": value}."""
    cp = configparser.ConfigParser(interpolation=None, strict=False)
    cp.optionxform = str
    try:
        cp.read(path, encoding="utf-8")
    except (OSError, configparser.Error, UnicodeDecodeError):
        pass
    pst = next((s for s in schema["settings"] if s["section"] == "edition" and s["key"] == "preset"), None)
    first = next((p for p in schema.get("presets", []) if pst and p["value"] == pst["default"]), None)
    first = dict(first["settings"]) if first else {}
    out = {}
    for st in schema["settings"]:
        key = st["section"] + "." + st["key"]
        val = cp.get(st["section"], st["key"], fallback=None)
        out[key] = val if val in [o["value"] for o in st["options"]] else first.get(key, st["default"])
    return out


# ── What a warm-up prepared (item 21) ─────────────────────────────────────
# The pixel shaders' text depends on a few Remastered settings (the game's
# "shader_settings" list, src/bw_settings.c). After each warm-up its
# settings' signature is kept in the shader cache folder (warmup.ini), so the
# launcher can tell when a change has made shaders this cache hasn't got, and
# offer to prepare them. A cache folder cleared by hand loses the record too.
STAMP = "warmup.ini"
STAMP_KEEP = 16                 # signatures kept per section, newest last


def shader_signature(schema, values):
    """The shader settings' choices as one line ("id=class ..."), or None if
    the game names none. Choices that make the same shaders share a class."""
    sets = (schema or {}).get("shader_settings")
    if not sets:
        return None
    return " ".join("%s=%s" % (s["id"], (s.get("classes") or {}).get(values.get(s["id"]),
                                                                      values.get(s["id"])))
                    for s in sorted(sets, key=lambda s: s["id"]))


def read_stamp(folder):
    """{"warmed": [signature, ...], "declined": [...]} for a cache folder (empty
    lists if there's no record)."""
    out = {"warmed": [], "declined": []}
    if not folder:
        return out
    cp = configparser.ConfigParser(interpolation=None, delimiters=("=",), strict=False)
    try:
        cp.read(os.path.join(folder, STAMP), encoding="utf-8")
    except (OSError, configparser.Error, UnicodeDecodeError):
        return out
    for sec in out:
        if cp.has_section(sec):
            out[sec] = [v for _k, v in cp.items(sec) if v]
    return out


def add_stamp(folder, section, signature):
    """Add `signature` to `section` ("warmed" or "declined") of the folder's
    record, newest last; written whole, through a temporary file. Returns
    True if it was written."""
    if not folder or not signature:
        return False
    st = read_stamp(folder)
    st[section] = ([s for s in st[section] if s != signature] + [signature])[-STAMP_KEEP:]
    if section == "warmed":             # prepared now: no longer a declined offer
        st["declined"] = [s for s in st["declined"] if s != signature]
    lines = ["; Blood Wake: the settings the graphics warm-up (tools/bw_warmup.py) prepared",
             "; shaders for in this folder, and the changes the launcher was told not to",
             "; offer again (Not now). Newest last. Deleting this file only resets that.", ""]
    for sec in ("warmed", "declined"):
        lines.append("[%s]" % sec)
        lines += ["%d = %s" % (i + 1, s) for i, s in enumerate(st[sec])]
        lines.append("")
    try:
        os.makedirs(folder, exist_ok=True)
        tmp = os.path.join(folder, STAMP + ".tmp")
        with open(tmp, "w", encoding="utf-8") as f:
            f.write("\n".join(lines))
        os.replace(tmp, os.path.join(folder, STAMP))
        return True
    except OSError:
        return False


def default_settings(schema, out):
    """What tools/launcher.pyw starts a new install with: the default preset's
    value for each setting, else the setting's default."""
    pst = next((s for s in schema["settings"] if s["section"] == "edition" and s["key"] == "preset"), None)
    first = next((p for p in schema.get("presets", []) if pst and p["value"] == pst["default"]), None)
    first = dict(first["settings"]) if first else {}
    cp = configparser.ConfigParser(interpolation=None)
    cp.optionxform = str
    for st in schema["settings"]:
        if not cp.has_section(st["section"]):
            cp.add_section(st["section"])
        cp.set(st["section"], st["key"], first.get(st["section"] + "." + st["key"], st["default"]))
    if cp.has_option("display", "mode"):        # item 102: the scenes are always windowed
        cp.set("display", "mode", "window")
    if not cp.has_section("launcher"):          # 0.2.5's format: that window is a choice (call 85)
        cp.add_section("launcher")
    cp.set("launcher", "format", "2")
    with open(out, "w", encoding="utf-8") as f:
        f.write("; Blood Wake settings for the graphics warm-up (tools/bw_warmup.py):\n"
                "; what a new launcher starts with (windowed). Not the player's file.\n\n")
        cp.write(f)
    return out


# ── The shader cache ──────────────────────────────────────────────────────

def cache_dir(cache=None):
    """The folder the game keeps its shaders in (bw_pscache.c); None when off."""
    if cache:
        return None if cache == "0" else cache
    base = local_appdata()
    return os.path.join(base, "xboxrecomp", "shadercache") if base else None


def cache_files(folder):
    """The shader files in `folder`, as a set of names; None if it can't be read."""
    if not folder:
        return None
    if not os.path.exists(folder):
        return set()
    try:
        return {n.lower() for n in os.listdir(folder) if re.match(r"(?i)ps_[0-9a-f]+\.bin$", n)}
    except OSError:
        return None


# ── Running a scene ───────────────────────────────────────────────────────

_NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)
BASE_ENV = {                            # as tools/play.py plays
    "RECOMP_PB_EXEC": "1", "RECOMP_FB_WINDOW": "1", "RECOMP_VBLANK": "paced", "RECOMP_USB": "1",
    "RECOMP_TITLE_KEVENTS": "1", "RECOMP_AC97_READY": "1", "RECOMP_TRACE_BUDGET": "0",
    "BW_STDERR_BUFFER": "1048576",
    "BW_GPU": "d3d11", "RECOMP_DISPATCH_GATE": "1", "RECOMP_FLIP_PACING": "1",
    "RECOMP_FULLSCREEN": "0",           # never fullscreen: that would show the hidden window
    # (it wins over the settings file's choice: bw_settings_apply keeps a
    # switch the environment already sets; the scenes' file says window too)
    "BW_TEST_XINPUT": "0:-0",           # item 102: no real controller read, no rumble sent
}
PS_LINE = re.compile(r"this run: (\d+) compiled in game \((\d+) ms\), (\d+) from the cache")
BOOT_MARK = "\n==== warm-up boot %d (%d s left)\n"


def run_scene(sc, exe, game_dir, work, settings, cache, log_dir):
    """One scene: a hidden boot, then (sc["follow"]) the game's own reboots, as
    tools/play.py follows them, until the scene's time is up. Returns a result
    dict; never raises."""
    res = {"scene": sc["name"], "edition": sc["edition"], "compiled": 0, "cached": 0,
           "seconds": 0.0, "boots": 0, "error": None}
    t0 = time.time()
    reported = False
    cwd = None
    try:
        cwd = tempfile.mkdtemp(prefix="bw_warmup_", dir=work)
        env = {k: v for k, v in os.environ.items() if not k.startswith(("RECOMP_", "BW_"))}
        env.update(BASE_ENV)
        env.update({"BW_UNATTENDED": "1", "BW_GAME_DIR": game_dir, "BW_EDITION": sc["edition"],
                    "RECOMP_SAVE_DIR": os.path.join(cwd, "saves"),
                    "BW_EDITION_SAVE_ROOT": os.path.join(cwd, "saves"),
                    "RECOMP_USB_PADS": str(sc["pads"]),
                    "BW_AUDIO_GAIN_DB": "-120",           # silent: setup is no place for game sound
                    "BW_SEED": "1"})                      # the same scenes on every PC
        if settings:
            env["BW_SETTINGS"] = settings
        if cache:
            env["BW_GPU_PSCACHE"] = cache
        if sc["script"]:
            env["RECOMP_PAD_SCRIPT"] = sc["script"]
        launch = launch_file(sc["page"], os.path.join(cwd, "launch.bin")) if sc["page"] else None
        relaunch = os.path.join(cwd, "bw_relaunch.bin")   # the game's own reboot request (main.c)
        si = subprocess.STARTUPINFO()
        si.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        si.wShowWindow = 0                                   # SW_HIDE: no window on the player's screen
        log = os.path.join(log_dir, "warmup_%s_%s.log" % (sc["edition"], sc["name"]))
        with open(log, "w", encoding="utf-8", errors="replace") as lf:
            for boot in range(1 + sc["follow"]):
                left = int(sc["seconds"] - (time.time() - t0))
                if left < 5:
                    break
                if launch:
                    env["BW_LAUNCH_DATA_FILE"] = launch
                env["BW_RUN_SECONDS"] = str(left)
                if os.path.exists(relaunch):
                    os.remove(relaunch)
                lf.write(BOOT_MARK % (boot + 1, left))
                lf.flush()
                p = subprocess.Popen([exe], cwd=cwd, env=env, stdin=subprocess.DEVNULL, stdout=lf,
                                     stderr=subprocess.STDOUT, startupinfo=si, creationflags=_NO_WINDOW)
                try:
                    code = p.wait(timeout=left + 45)
                except subprocess.TimeoutExpired:
                    p.kill()
                    p.wait()
                    code = None
                    res["error"] = "ran over time (stopped after %d s; its shaders so far are kept)" % (
                        left + 45)
                res["boots"] += 1
                lf.flush()
                if code not in (0, 124, None) and not res["error"]:   # 124: the run limit (main.c)
                    res["error"] = "exit code %s" % code
                if code is None or not os.path.exists(relaunch):
                    break
                launch = os.path.join(cwd, "launch_next.bin")
                os.replace(relaunch, launch)
        res["seconds"] = time.time() - t0
        with open(log, encoding="utf-8", errors="replace") as lf:
            text = lf.read()
        for boot_text in text.split(BOOT_MARK.split("%")[0])[1:]:    # each boot reports its own totals
            hits = PS_LINE.findall(boot_text)
            if hits:
                reported = True
                res["compiled"] += int(hits[-1][0])
                res["cached"] += int(hits[-1][2])
        if not reported:
            res["compiled"] = res["cached"] = None
    except Exception as e:                  # a warm-up never breaks the install
        res["error"] = "%s: %s" % (type(e).__name__, e)
        res["seconds"] = time.time() - t0
    finally:
        if cwd:
            shutil.rmtree(cwd, ignore_errors=True)
    return res


def warmup(exe, game_dir, work, parallel=2, only=None, cache=None, prog=None):
    """Run the scenes, `parallel` at a time but never two Original scenes at
    once (they share the Original disk images). prog(frac, msg). Returns
    {"results": [...], "settings": what the scenes played with, "cache": the
    folder, "new": distinct shaders added to it, "total": shaders in it after}
    ("new" and "total" are None if the folder can't be read)."""
    prog = prog or (lambda f, m: None)
    scenes = [s for s in scene_list() if not only or s["name"] in only]
    folder = cache_dir(cache)
    out = {"results": [], "settings": None, "cache": folder, "new": None, "total": None,
           "signature": None, "recorded": False}
    if not scenes:
        return out
    os.makedirs(work, exist_ok=True)
    log_dir = os.path.join(work, "logs")
    os.makedirs(log_dir, exist_ok=True)
    settings, out["settings"], schema = scene_settings(exe, os.path.join(work, "warmup_settings.ini"))
    if not settings:
        prog(0, "the settings couldn't be read; the scenes use the game's defaults")
    before = cache_files(folder)
    workers = max(1, parallel)
    total = max(float(sum(s["seconds"] for s in scenes)) / workers,
                sum(s["seconds"] for s in scenes if s["edition"] == "original"))
    t0 = time.time()
    results, cond, todo = out["results"], threading.Condition(), list(scenes)
    original_running = [False]

    def worker():
        while True:
            with cond:
                sc = None
                while todo:
                    sc = next((s for s in todo
                               if s["edition"] != "original" or not original_running[0]), None)
                    if sc:
                        break
                    cond.wait()
                if not sc:
                    return
                todo.remove(sc)
                if sc["edition"] == "original":
                    original_running[0] = True
            try:
                r = run_scene(sc, exe, game_dir, work, settings, cache, log_dir)
            finally:
                with cond:
                    if sc["edition"] == "original":
                        original_running[0] = False
                    cond.notify_all()
            with cond:
                results.append(r)

    threads = [threading.Thread(target=worker, daemon=True) for _ in range(workers)]
    for t in threads:
        t.start()
    while any(t.is_alive() for t in threads):
        for t in threads:
            t.join(timeout=5)
        with cond:
            done = len(results)
        prog(min(0.97, (time.time() - t0) / (total + 20)),
             "preparing graphics: %d of %d scenes (%d s)" % (done, len(scenes), time.time() - t0))
    after = cache_files(folder)
    if before is not None and after is not None:
        out["new"], out["total"] = len(after - before), len(after)
    # Item 21: what this cache now holds shaders for, when every Remastered
    # scene ran without a problem (a scene stopped for time still counts: its
    # shaders so far are kept, and the scenes have room to spare).
    if settings and schema:
        try:
            out["signature"] = shader_signature(schema, launcher_values(schema, settings))
        except (KeyError, TypeError):
            out["signature"] = None
    rem = [r for r in results if r["edition"] != "original"]
    clean = rem and len(rem) == sum(1 for s in scenes if s["edition"] != "original") and all(
        not r["error"] or r["error"].startswith("ran over time") for r in rem)
    if out["signature"] and clean and after is not None:
        out["recorded"] = add_stamp(folder, "warmed", out["signature"])
    return out


def summary(w):
    """(new shaders, shaders in the cache or None, the results with a problem),
    from warmup()'s result. If the cache folder couldn't be read, "new" is the
    logs' count instead."""
    res = w["results"]
    n = w["new"] if w["new"] is not None else sum(r["compiled"] or 0 for r in res)
    bad = [r for r in res if r["error"] or r["compiled"] is None]
    return n, w["total"], bad


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--exe")
    ap.add_argument("--game-dir")
    ap.add_argument("--work-dir", help="for the scenes' temporary folders and logs (default: next to the exe)")
    ap.add_argument("--parallel", type=int, default=2)
    ap.add_argument("--only", help="these scenes (comma-separated names)")
    ap.add_argument("--cache", help="the shader cache folder (default: the game's, %%LOCALAPPDATA%%/xboxrecomp/shadercache)")
    ap.add_argument("--list", action="store_true", help="list the scenes and stop")
    ap.add_argument("--progress", choices=("text", "json"), default="text")
    a = ap.parse_args(argv)
    if a.list:
        for s in scene_list():
            print("%-14s %-10s %2d s  %d pad(s)" % (s["name"], s["edition"], s["seconds"], s["pads"]))
        return 0
    if not a.exe or not a.game_dir:
        ap.error("--exe and --game-dir are needed")

    def say(frac, msg, **extra):
        if a.progress == "json":
            d = {"step": "warmup", "pct": int(round(100 * frac)), "msg": msg}
            d.update(extra)
            print(json.dumps(d), flush=True)
        else:
            print("[%3d%%] %s" % (100 * frac, msg), flush=True)

    work = os.path.abspath(a.work_dir or os.path.join(os.path.dirname(os.path.abspath(a.exe)), "warmup"))
    t0 = time.time()
    w = warmup(os.path.abspath(a.exe), os.path.abspath(a.game_dir), work, a.parallel,
               set(a.only.split(",")) if a.only else None, a.cache, say)
    n, total, bad = summary(w)
    say(1, "settings: %s" % w["settings"])
    for r in sorted(w["results"], key=lambda r: (r["edition"], r["scene"])):
        say(1, "%s %s: %s compiled, %s loads from the cache (from its log), %d s%s" % (
            r["edition"], r["scene"], r["compiled"], r["cached"], r["seconds"],
            " (%s)" % r["error"] if r["error"] else ""))
    say(1, "graphics ready: %d new shaders prepared%s, in %d s%s" % (
        n, "" if total is None else " (%d in the shader cache)" % total, time.time() - t0,
        "; %d scene(s) had a problem (logs: %s)" % (len(bad), os.path.join(work, "logs")) if bad else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
