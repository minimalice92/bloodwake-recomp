#!/usr/bin/env python3
"""Build Blood Wake (the PC port) from your own disc: the one command.

    python tools/build_from_disc.py --disc <folder|image.iso> --install-dir <dir>
        [--toolkit <dir>] [--work-dir <dir>] [--jobs N] [--check-only] [--no-warmup]
        [--progress json] [--allow-unverified]

You bring your own copy of the game: a folder of the disc's files, an XISO, or
a full disc image. This script checks that it is an unmodified retail copy
(hashes of its files, fingerprints/retail.json), translates the game's code to
C on your PC with the xboxrecomp toolkit at its pinned commit, checks that the
generated C is exactly what the maintainers get (hashes only,
fingerprints/generated.json; the code itself is never published), and builds
it with Microsoft's compiler. Nothing from the game leaves your PC.

Steps (each prints one progress line with --progress json):
  identify     what --disc is: a folder, an XISO or a full (Redump) image
  toolkit      xboxrecomp at the pinned commit: --toolkit (checked), or
               GitHub's archive of that commit (SHA-256 checked)
  unpack       an image's files into <install-dir>/game/disc (a folder is copied there)
  fingerprint  every file against fingerprints/retail.json
  pipeline     tools/run_pipeline.py (no Ghidra) into <work-dir>/gen
  verify       the generated C against fingerprints/generated.json
  build        CMake + Visual Studio 2022 (x64, Release). <work-dir>/build/
               bw_build_source.json records what the last good build was made
               from (a digest of the sources, not file times); a build folder
               without it, or from other sources (another version, a build
               that failed), is compiled again from scratch
  stage       the exe, the launcher and the files it runs with, install.json
  warmup       "Preparing graphics (one time)": a few short scenes of the built
               game, hidden and silent, so its pixel shaders are compiled into
               the shader cache before the first play (tools/bw_warmup.py; about
               2 minutes). --no-warmup skips it. It never fails the build: a
               problem is reported and the step skipped.
  done

--check-only stops after the fingerprint (nothing is unpacked or copied).
--allow-unverified builds a copy that isn't the recognised retail one, after
the player accepted Setup's warning (0.2.5 item 3): a different program or
differing files are reported and the build goes on, and a generated-C mismatch
is listed in logs/generated_differs.txt. The verdict ("verified", "files
differ" or "unverified") goes into install.json's "copy" and, for anything but
verified, one line into build/Release/bw_copy.txt (docs: CONTRACTS.md 9).
--progress json prints one JSON object per line on stdout:
  {"step": "<step>", "pct": 0-100, "msg": "..."}
pct is the whole job's progress. A failure adds "error": true and "exit": <code>.
Tool output goes to <work-dir>/logs/.

Exit codes: 0 ok; 2 not a supported unmodified retail copy (without
--allow-unverified; its JSON line adds "reason": "files differ" when the main
program is the retail one, or "program differs"); 3 missing
dependency (the message says which); 4 pipeline failed; 5 the generated code
doesn't match ours; 6 build failed; 7 disk space or I/O.

Needs: Python 3.13 (3.10+ runs; the fingerprints were made with 3.13),
capstone 5.0.9, CMake 3.20+ (Visual Studio's own is fine) and Visual Studio
2022 or its Build Tools with the C++ workload; about 2.6 GB free, and an
internet connection for the toolkit unless --toolkit is given. No admin
rights, no `py` launcher, paths with spaces are fine. About 15 minutes on a
recent desktop (most of it the translation), plus about 2 for the warm-up.
Stdlib only.
"""
import argparse
import datetime as _dt
import glob
import hashlib
import http.client
import json
import os
import queue
import re
import shutil
import subprocess
import sys
import threading
import time
import urllib.request
import zipfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FINGERPRINTS = os.path.join(REPO, "fingerprints")

TOOLKIT_COMMIT = "f0f8a461496a4f23d9b0aeaa284b8d70ba7ab82b"      # xboxrecomp v0.16.0 (D-002)
TOOLKIT_URL = "https://github.com/sp00nznet/xboxrecomp/archive/%s.zip" % TOOLKIT_COMMIT
TOOLKIT_ZIP_SHA256 = "64b27ee84e70d7fbbcce901716e8ca77b37f5c1c050a8455f376ecc2ee378cd5"
# The archive's files (paths and contents), in case GitHub ever re-compresses
# the same commit: see tree_digest().
TOOLKIT_TREE_SHA256 = "5ee0e96d5cf95da95e0212afe32fdca7b535c7264c49fadd0e65724cfda0ca33"
TOOLKIT_MARKER = ".bw_toolkit.json"                              # read by tools/run_pipeline.py

CAPSTONE = "5.0.9"
PYTHON_PIN = (3, 13)
SECTOR = 2048
GB = 1 << 30
# Space needed (bytes, with margin): the unpacked disc and the staged exe
# (measured 1.97 GB + 31 MB), and the work folder (generated C, toolkit, build
# tree, logs: measured 211 MB). Added up when both are on one drive.
NEED_INSTALL = int(2.2 * GB)
NEED_WORK = int(0.4 * GB)

EXIT_OK, EXIT_DISC, EXIT_DEP, EXIT_PIPELINE, EXIT_GEN, EXIT_BUILD, EXIT_IO = 0, 2, 3, 4, 5, 6, 7

# Overall progress: where each step starts and ends (pct).
# (The installer reads fingerprint lines at pct 20 and up as the check result.)
SPAN = {"identify": (0, 1), "toolkit": (1, 3), "unpack": (3, 18), "fingerprint": (18, 20),
        "pipeline": (20, 62), "verify": (62, 63), "build": (63, 83), "stage": (83, 84),
        "warmup": (84, 99), "done": (100, 100)}
# Rough seconds per pipeline step on a 2020s desktop, for the bar between lines.
PIPE_SECONDS = {"parse": 2, "disasm": 520, "funcid": 3, "abi": 3, "ghidra": 1, "recomp": 75}


class Fail(Exception):
    def __init__(self, code, step, msg, **extra):
        Exception.__init__(self, msg)
        self.code, self.step, self.msg, self.extra = code, step, msg, extra


# What a copy is (contract 9 in docs/025/CONTRACTS.md: install.json's "copy",
# and bw_copy.txt beside the exe for anything but a verified copy).
VERIFIED, FILES_DIFFER, UNVERIFIED = "verified", "files differ", "unverified"
COPY_WORDS = {FILES_DIFFER: "copy's files differ from the retail disc", UNVERIFIED: "unverified copy"}
# An unexpected error's exit code, by the step it happened in.
STEP_EXIT = {"identify": EXIT_IO, "toolkit": EXIT_DEP, "unpack": EXIT_IO, "fingerprint": EXIT_DISC,
             "pipeline": EXIT_PIPELINE, "verify": EXIT_GEN, "build": EXIT_BUILD, "stage": EXIT_IO,
             "warmup": EXIT_IO, "done": EXIT_IO}
STEP_WORDS = {"identify": "looking at the copy", "toolkit": "getting the toolkit", "unpack": "copying the "
              "game's files", "fingerprint": "checking the copy", "pipeline": "translating the game's code",
              "verify": "checking the translated code", "build": "compiling", "stage": "installing",
              "warmup": "preparing graphics", "done": "finishing"}


# ── Progress ──────────────────────────────────────────────────────────────

class Progress:
    def __init__(self, json_mode):
        self.json = json_mode
        self.last = 0
        self.step = "identify"         # the step under way, for an unexpected error

    def __call__(self, step, frac, msg, **extra):
        self.step = step
        lo, hi = SPAN[step]
        pct = int(round(lo + (hi - lo) * max(0.0, min(1.0, frac))))
        pct = max(pct, self.last) if not extra.get("error") else pct
        self.last = pct
        if self.json:
            d = {"step": step, "pct": pct, "msg": msg}
            d.update(extra)
            sys.stdout.write(json.dumps(d) + "\n")
        else:
            sys.stdout.write("[%3d%%] %-11s %s\n" % (pct, step, msg))
        sys.stdout.flush()


# ── Hashing ───────────────────────────────────────────────────────────────

def sha256_file(path, h=None):
    h = h or hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def path_key(rel):
    """The fingerprint's name for a disc file: SHA-256 of its lower-case path
    with forward slashes (the published list carries no file names)."""
    return hashlib.sha256(rel.replace("\\", "/").strip("/").lower().encode("utf-8")).hexdigest()


def total_digest(pairs):
    """One hash for a set of (name, sha256) pairs, order-independent."""
    h = hashlib.sha256()
    for name, digest in sorted(pairs):
        h.update(("%s\0%s\n" % (name, digest)).encode("utf-8"))
    return h.hexdigest()


def tree_digest(files):
    """files: (relative path, sha256 of contents). The toolkit's tree hash."""
    return total_digest((p.replace("\\", "/"), d) for p, d in files)


GEN_SKIP = {"bw_ocean_native.inc"}      # written by the build (tools/bw_ocean_xlate.py), not the pipeline


def gen_fingerprint(gen_dir):
    """{file: {"size", "sha256"}} for the pipeline's output files."""
    out = {}
    for name in sorted(os.listdir(gen_dir)):
        p = os.path.join(gen_dir, name)
        if name in GEN_SKIP or not os.path.isfile(p):
            continue
        out[name] = {"size": os.path.getsize(p), "sha256": sha256_file(p)}
    return out


# Tracked files whose contents decide the generated C (besides the XBE and the
# toolkit), recorded so a mismatch can say which input differs.
GEN_INPUTS = ["tools/run_pipeline.py", "tools/xbr_patches.py",
              "names/icall_seeds.json", "src/recomp_manual.c"]


def gen_inputs(repo):
    files = list(GEN_INPUTS) + sorted(
        os.path.relpath(p, repo).replace(os.sep, "/")
        for p in glob.glob(os.path.join(repo, "src", "bw_*_hooks.c")))
    out = {}
    for rel in files:
        p = os.path.join(repo, rel)
        out[rel] = sha256_file(p) if os.path.isfile(p) else None
    return out


def load_json(path, what):
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except OSError:
        raise Fail(EXIT_IO, "identify", "%s is missing (%s). Get the complete source." % (what, path))
    except ValueError as e:
        raise Fail(EXIT_IO, "identify", "%s is damaged (%s): %s" % (what, path, e))


# ── Dependencies ──────────────────────────────────────────────────────────

def module_version(name):
    try:
        from importlib.metadata import version
        return version(name)
    except Exception:
        return None


def check_python():
    notes = []
    if sys.version_info < (3, 10):
        raise Fail(EXIT_DEP, "identify", "Python 3.13 is needed (this is %s)." % sys.version.split()[0])
    if sys.version_info[:2] != PYTHON_PIN:
        notes.append("Python %s: the fingerprints were made with 3.13; if the check of the "
                     "generated code fails, use 3.13." % sys.version.split()[0])
    cap = module_version("capstone")
    if cap is None:
        raise Fail(EXIT_DEP, "identify", "capstone is not installed for this Python (%s). "
                   "Install it: %s -m pip install capstone==%s" % (sys.executable, sys.executable, CAPSTONE))
    if cap != CAPSTONE:
        raise Fail(EXIT_DEP, "identify", "capstone %s is needed (found %s): "
                   "%s -m pip install capstone==%s" % (CAPSTONE, cap, sys.executable, CAPSTONE))
    return notes


def find_vs():
    """Visual Studio 2022 / Build Tools 17.x with the x64 C++ compiler: (path, version)."""
    roots = [os.environ.get("ProgramFiles(x86)"), os.environ.get("ProgramFiles")]
    vswhere = None
    for r in roots:
        if r:
            c = os.path.join(r, "Microsoft Visual Studio", "Installer", "vswhere.exe")
            if os.path.isfile(c):
                vswhere = c
                break
    if not vswhere:
        return None, None
    try:
        out = subprocess.run([vswhere, "-products", "*", "-version", "[17.0,18.0)",
                              "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                              "-format", "json", "-utf8"], capture_output=True, timeout=60)
        found = json.loads(out.stdout.decode("utf-8", "replace") or "[]")
    except (OSError, ValueError, subprocess.SubprocessError):
        return None, None
    if not found:
        return None, None
    found.sort(key=lambda d: d.get("installationVersion", ""), reverse=True)
    return found[0].get("installationPath"), found[0].get("installationVersion")


def find_cmake(vs_path):
    cands = [shutil.which("cmake")]
    if vs_path:
        cands.append(os.path.join(vs_path, "Common7", "IDE", "CommonExtensions", "Microsoft",
                                  "CMake", "CMake", "bin", "cmake.exe"))
    for c in cands:
        if c and os.path.isfile(c):
            try:
                v = subprocess.run([c, "--version"], capture_output=True, text=True, timeout=60).stdout
            except (OSError, subprocess.SubprocessError):
                continue
            m = re.search(r"cmake version (\d+)\.(\d+)(\.\d+)?", v)
            if m and (int(m.group(1)), int(m.group(2))) >= (3, 20):
                return c, m.group(0).split()[-1]
    return None, None


def check_space(path, need, step, what):
    probe = os.path.abspath(path)
    while not os.path.exists(probe):
        parent = os.path.dirname(probe)
        if parent == probe:
            break
        probe = parent
    try:
        free = shutil.disk_usage(probe).free
    except OSError as e:
        raise Fail(EXIT_IO, step, "Can't check free space at %s: %s" % (probe, e))
    if free < need:
        raise Fail(EXIT_IO, step, "Not enough free space for %s at %s: %.1f GB free, %.1f GB needed."
                   % (what, probe, free / GB, need / GB))


# ── The toolkit ───────────────────────────────────────────────────────────

def zip_tree(zf):
    """(relative path, sha256) of every file in the toolkit archive."""
    out = []
    for info in zf.infolist():
        if info.is_dir():
            continue
        rel = info.filename.split("/", 1)[1] if "/" in info.filename else info.filename
        out.append((rel, hashlib.sha256(zf.read(info)).hexdigest()))
    return out


class CutStream:
    """BW_TEST_CUT_DOWNLOAD=<bytes> (testing only): the download ends after that
    many bytes, as a dropped connection does (http.client.IncompleteRead)."""

    def __init__(self, r, limit):
        self.r, self.left = r, limit

    def read(self, n=-1):
        if self.left <= 0:
            raise http.client.IncompleteRead(b"", None)
        data = self.r.read(min(n if n and n > 0 else 1 << 20, self.left))
        self.left -= len(data)
        return data


class SlowStream:
    """BW_TEST_DOWNLOAD_DELAY=<seconds> (testing only): the download takes about
    that long, in 64 KB pieces, so a test can press Setup's Cancel during it."""

    def __init__(self, r, seconds, size=2300000):
        self.r, self.pause = r, max(0.0, seconds) * 65536.0 / size

    def read(self, n=-1):
        time.sleep(self.pause)
        return self.r.read(65536)


def download_toolkit(zpath, prog, tries=2):
    """The toolkit's archive into zpath. A cut-off or failed download is tried
    once more; then it ends in the usual "couldn't download" (exit 3)."""
    cut = os.environ.get("BW_TEST_CUT_DOWNLOAD")
    tmp = zpath + ".part"
    for attempt in range(1, tries + 1):
        prog("toolkit", 0.1, "downloading xboxrecomp %s from GitHub%s" % (
            TOOLKIT_COMMIT[:7], "" if attempt == 1 else " (second try)"))
        try:
            with urllib.request.urlopen(TOOLKIT_URL, timeout=120) as r, open(tmp, "wb") as f:
                src = CutStream(r, int(cut)) if cut else r
                if os.environ.get("BW_TEST_DOWNLOAD_DELAY"):
                    src = SlowStream(src, float(os.environ["BW_TEST_DOWNLOAD_DELAY"]))
                shutil.copyfileobj(src, f, 1 << 20)
            with zipfile.ZipFile(tmp) as z:          # a short file without an error is cut off too
                z.namelist()
            os.replace(tmp, zpath)
            return
        except (OSError, http.client.HTTPException, zipfile.BadZipFile) as e:
            why = "%s: %s" % (type(e).__name__, e) if str(e) else type(e).__name__
            try:
                os.remove(tmp)
            except OSError:
                pass
            if attempt < tries:
                prog("toolkit", 0.1, "the toolkit's download was cut off or failed (%s); trying once more" % why)
                time.sleep(3)
                continue
            raise Fail(EXIT_DEP, "toolkit", "Couldn't download the toolkit (%s): %s. Check the "
                       "internet connection, or pass --toolkit <a clean checkout of %s>."
                       % (TOOLKIT_URL, why, TOOLKIT_COMMIT))


def get_toolkit(a, work, prog):
    """A clean toolkit folder at the pinned commit."""
    if a.toolkit:
        tk = os.path.abspath(a.toolkit)
        if not os.path.isdir(os.path.join(tk, "tools", "recomp")):
            raise Fail(EXIT_DEP, "toolkit", "--toolkit %s is not an xboxrecomp folder." % tk)
        if os.path.exists(os.path.join(tk, ".git")):
            head = run_text(["git", "rev-parse", "HEAD"], tk)
            dirty = run_text(["git", "status", "--porcelain", "--untracked-files=no"], tk)
            if head != TOOLKIT_COMMIT:
                raise Fail(EXIT_DEP, "toolkit", "--toolkit %s is at %s, not the pinned commit %s "
                           "(git checkout %s)." % (tk, head, TOOLKIT_COMMIT[:7], TOOLKIT_COMMIT))
            if dirty:
                raise Fail(EXIT_DEP, "toolkit", "--toolkit %s has local changes; use a clean "
                           "checkout of %s." % (tk, TOOLKIT_COMMIT[:7]))
            prog("toolkit", 1, "xboxrecomp %s (git checkout %s)" % (TOOLKIT_COMMIT[:7], tk))
            return tk
        marker = os.path.join(tk, TOOLKIT_MARKER)
        try:
            with open(marker, encoding="utf-8") as f:
                m = json.load(f)
            files = m["files"]
        except (OSError, ValueError, KeyError):
            raise Fail(EXIT_DEP, "toolkit", "--toolkit %s is neither a git checkout nor a folder "
                       "this script extracted. Leave --toolkit out to download it." % tk)
        got = [(rel, sha256_file(os.path.join(tk, rel))) for rel in files
               if os.path.isfile(os.path.join(tk, rel))]
        if len(got) != len(files) or tree_digest(got) != TOOLKIT_TREE_SHA256:
            raise Fail(EXIT_DEP, "toolkit", "--toolkit %s doesn't match the pinned commit's files." % tk)
        prog("toolkit", 1, "xboxrecomp %s (checked folder %s)" % (TOOLKIT_COMMIT[:7], tk))
        return tk

    tdir = os.path.join(work, "toolkit")
    os.makedirs(tdir, exist_ok=True)
    zpath = os.path.join(tdir, "xboxrecomp-%s.zip" % TOOLKIT_COMMIT)
    if not (os.path.isfile(zpath) and sha256_file(zpath) == TOOLKIT_ZIP_SHA256):
        download_toolkit(zpath, prog)
    zsha = sha256_file(zpath)
    try:
        zf = zipfile.ZipFile(zpath)
    except zipfile.BadZipFile:
        os.remove(zpath)
        raise Fail(EXIT_DEP, "toolkit", "The toolkit download is damaged; run again.")
    with zf:
        files = zip_tree(zf)
        if zsha != TOOLKIT_ZIP_SHA256:
            # GitHub builds archives on the fly; the same commit's files are what count.
            if tree_digest(files) != TOOLKIT_TREE_SHA256:
                os.remove(zpath)
                raise Fail(EXIT_DEP, "toolkit", "The toolkit download doesn't match the pinned "
                           "commit (SHA-256 %s). Nothing was used." % zsha)
        tk = os.path.join(tdir, "xboxrecomp")
        if os.path.isdir(tk):
            shutil.rmtree(tk)              # a fresh copy: the pipeline writes into the toolkit
        os.makedirs(tk)
        prefix = zf.namelist()[0].split("/", 1)[0] + "/"
        for info in zf.infolist():
            if info.is_dir() or not info.filename.startswith(prefix):
                continue
            rel = info.filename[len(prefix):]
            dest = os.path.join(tk, *rel.split("/"))
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            with zf.open(info) as src, open(dest, "wb") as out:
                shutil.copyfileobj(src, out, 1 << 20)
    with open(os.path.join(tk, TOOLKIT_MARKER), "w", encoding="utf-8") as f:
        json.dump({"commit": TOOLKIT_COMMIT, "describe": "v0.16.0", "url": TOOLKIT_URL,
                   "archive_sha256": zsha, "tree_sha256": tree_digest(files),
                   "files": sorted(p for p, _ in files)}, f, indent=1)
    prog("toolkit", 1, "xboxrecomp %s (archive SHA-256 %s...)" % (TOOLKIT_COMMIT[:7], zsha[:12]))
    return tk


def run_text(cmd, cwd):
    try:
        r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, timeout=120)
        return r.stdout.strip() if r.returncode == 0 else None
    except (OSError, subprocess.SubprocessError):
        return None


# ── The disc ──────────────────────────────────────────────────────────────

def xiso_module(tk):
    if tk not in sys.path:
        sys.path.insert(0, tk)
    from tools.xiso import xdvdfs          # the pinned toolkit's reader
    return xdvdfs


def identify(disc):
    """("folder" | "image", absolute path)."""
    p = os.path.abspath(disc)
    if os.path.isdir(p):
        if not any(n.lower() == "default.xbe" for n in os.listdir(p)):
            raise Fail(EXIT_DISC, "identify", "%s has no default.xbe: pick the folder with the "
                       "disc's files in it." % p)
        return "folder", p
    if os.path.isfile(p):
        return "image", p
    raise Fail(EXIT_DISC, "identify", "%s doesn't exist." % p)


def folder_files(root):
    """(relative path, absolute path) of every file under root."""
    out = []
    for d, dirs, files in os.walk(root):
        dirs.sort()
        for n in sorted(files):
            full = os.path.join(d, n)
            out.append((os.path.relpath(full, root).replace(os.sep, "/"), full))
    return out


def image_files(iso):
    return [((d + "/" + e.name).lstrip("/"), e) for d, e in iso.walk()]


def copy_hashed(read_chunks, dest):
    """Write the chunks to dest (if given) and return their SHA-256."""
    h = hashlib.sha256()
    out = None
    if dest:
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        out = open(dest + ".part", "wb")
    try:
        for chunk in read_chunks:
            h.update(chunk)
            if out:
                out.write(chunk)
    finally:
        if out:
            out.close()
    if dest:
        os.replace(dest + ".part", dest)
    return h.hexdigest()


def image_chunks(iso, e, xd):
    iso._f.seek(iso.base + e.sector * xd.SECTOR)
    left = e.size
    while left > 0:
        chunk = iso._f.read(min(1 << 20, left))
        if not chunk:
            raise xd.XisoError("the image ends inside %s" % e.name)
        left -= len(chunk)
        yield chunk


def file_chunks(path):
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            yield chunk


class Retail:
    def __init__(self, data):
        self.data = data
        self.by_key = {f["name_sha256"]: f for f in data["files"]}

    def compare(self, got):
        """got: {relative path: (size, sha256)}. Returns (ok, plain-words summary, details)."""
        seen, bad, extra = set(), [], []
        for rel, (size, digest) in sorted(got.items()):
            k = path_key(rel)
            ref = self.by_key.get(k)
            if ref is None:
                extra.append(rel)
                continue
            seen.add(k)
            if (size, digest) != (ref["size"], ref["sha256"]) and ref.get("required", True):
                bad.append(rel)
        missing = [f for k, f in self.by_key.items() if k not in seen and f.get("required", True)]
        ok = not bad and not missing
        if ok:
            # "an unmodified retail copy (North America, 450 files checked)": the
            # label's region only, so Setup's Finished line doesn't say it twice
            label = self.data.get("label", "")
            region = label.split(", ", 1)[1].split(" (")[0] if ", " in label else ""
            msg = "an unmodified retail copy (%s%d files checked)" % (
                region + ", " if region else "", len(seen))
        else:
            parts = []
            if bad:
                parts.append("%d file%s differ%s from the retail disc (%s%s)" % (
                    len(bad), "s" if len(bad) > 1 else "", "" if len(bad) > 1 else "s",
                    ", ".join(bad[:3]), ", ..." if len(bad) > 3 else ""))
            if missing:
                parts.append("%d file%s the retail disc has %s missing" % (
                    len(missing), "s" if len(missing) > 1 else "", "are" if len(missing) > 1 else "is"))
            msg = ("This isn't an unmodified retail copy of %s (%s): %s. Only an unmodified "
                   "copy of the supported version works." % (
                       self.data.get("game", "the game"), self.data.get("label", "retail"),
                       "; ".join(parts)))
        return ok, msg, {"differ": bad, "missing": len(missing), "extra": extra}


def check_disc(a, kind, src, tk, game_dir, retail, prog):
    """Hash (and, unless --check-only, unpack or copy) the disc. Returns the summary."""
    got = {}
    if kind == "image":
        xd = xiso_module(tk)
        try:
            iso = xd.Xiso(src)
        except (xd.XisoError, OSError) as e:
            raise Fail(EXIT_DISC, "identify", "%s isn't an Xbox disc image (%s)." % (src, e))
        with iso:
            layout = "full disc image (Redump layout)" if iso.base == 0x18300000 else \
                     "XISO" if iso.base == 0 else "Xbox image (partition at 0x%X)" % iso.base
            prog("identify", 1, "%s: %s" % (os.path.basename(src), layout))
            entries = image_files(iso)
            # The XBE first: a wrong disc stops here, before 2 GB are written.
            xbe = [(r, e) for r, e in entries if r.lower() == "default.xbe"]
            if not xbe:
                raise Fail(EXIT_DISC, "fingerprint", "The image has no default.xbe.")
            ref = retail.by_key.get(path_key("default.xbe"))
            xsha = copy_hashed(image_chunks(iso, xbe[0][1], xd), None)
            if not ref or (xbe[0][1].size, xsha) != (ref["size"], ref["sha256"]):
                if not a.allow_unverified:
                    raise Fail(EXIT_DISC, "fingerprint", "This isn't the supported retail version of "
                               "%s: its default.xbe differs (%s). Setup can build it anyway once you've "
                               "accepted its warning (--allow-unverified)." % (
                                   retail.data.get("game"), retail.data.get("label")), reason="program differs")
                prog("fingerprint", 0, "its main program isn't the retail one (SHA-256 %s...): building it "
                     "as an unverified copy, as accepted" % xsha[:12])
            total = sum(e.size for _, e in entries) or 1
            done = 0
            step = "fingerprint" if a.check_only else "unpack"
            last = 0
            for rel, e in entries:
                dest = None if a.check_only else os.path.join(game_dir, *rel.split("/"))
                try:
                    digest = copy_hashed(image_chunks(iso, e, xd), dest)
                except xd.XisoError as err:
                    raise Fail(EXIT_DISC, step, "The image is damaged or cut short: %s." % err)
                got[rel] = (e.size, digest)
                done += e.size
                if time.time() - last > 2:
                    last = time.time()
                    prog(step, done / total, "%s %d of %d files (%.1f of %.1f GB)" % (
                        "checking" if a.check_only else "unpacking", len(got), len(entries),
                        done / GB, total / GB))
    else:
        prog("identify", 1, "a folder of the disc's files: %s" % src)
        files = folder_files(src)
        total = sum(os.path.getsize(p) for _, p in files) or 1
        done, last = 0, 0
        same = os.path.normcase(os.path.abspath(src)) == os.path.normcase(os.path.abspath(game_dir))
        step = "fingerprint" if (a.check_only or same) else "unpack"
        for rel, full in files:
            k = path_key(rel)
            dest = None
            # Only the disc's own files; an unverified copy's program may need others too.
            if not (a.check_only or same) and (k in retail.by_key or a.allow_unverified):
                dest = os.path.join(game_dir, *rel.split("/"))
            got[rel] = (os.path.getsize(full), copy_hashed(file_chunks(full), dest))
            done += got[rel][0]
            if time.time() - last > 2:
                last = time.time()
                prog(step, done / total, "%s %d of %d files (%.1f of %.1f GB)" % (
                    "checking" if dest is None else "copying", len(got), len(files),
                    done / GB, total / GB))
    ok, msg, details = retail.compare(got)
    xref = retail.by_key.get(path_key("default.xbe"))
    xgot = next((v for r, v in got.items() if r.lower() == "default.xbe"), None)
    xbe_ok = bool(xref and xgot and xgot == (xref["size"], xref["sha256"]))
    verdict = VERIFIED if ok else (FILES_DIFFER if xbe_ok else UNVERIFIED)
    parts = []                              # which files differ, in a few words (Setup's question shows it)
    if details["differ"]:
        n = len(details["differ"])
        parts.append("%d of its files differ%s (%s%s)" % (n, "s" if n == 1 else "", ", ".join(details["differ"][:3]),
                                                           ", ..." if n > 3 else ""))
    if details["missing"]:
        parts.append("%d file%s missing" % (details["missing"], " is" if details["missing"] == 1 else "s are"))
    if not ok and not a.allow_unverified:
        # Only a copy whose program matches gets here (an image stops at the XBE, above).
        raise Fail(EXIT_DISC, "fingerprint", msg + " Setup can build it anyway once you've accepted "
                   "its warning (--allow-unverified).", reason=FILES_DIFFER if xbe_ok else "program differs",
                   detail=" and ".join(parts))
    if ok:
        prog("fingerprint", 1, msg, verdict=verdict)
    elif verdict == FILES_DIFFER:
        prog("fingerprint", 1, "its main program is the retail one, but %s: building it as accepted, "
             "marked \"files differ\"" % " and ".join(parts), verdict=verdict)
    else:
        prog("fingerprint", 1, "an unverified copy (its main program isn't the retail one), built as accepted",
             verdict=verdict)
    files = sorted((path_key(r), d) for r, (s, d) in got.items()     # as retail.json's total
                   if retail.by_key.get(path_key(r), {}).get("required", False))
    return {"kind": kind if kind == "folder" else ("redump" if "Redump" in layout else "xiso"),
            "label": retail.data.get("label"), "files_total_sha256": total_digest(files),
            "extra_files_ignored": len(details["extra"]), "verdict": verdict,
            "xbe_sha256": xgot[1] if xgot else None,
            "files_differ": details["differ"][:50], "files_missing": details["missing"]}


# ── Running tools ─────────────────────────────────────────────────────────

def run_logged(cmd, log_path, cwd=None, env=None, on_line=None, tick=None):
    """Run cmd with its output in log_path; on_line(line) per line, tick() every 5 s."""
    os.makedirs(os.path.dirname(log_path), exist_ok=True)
    q = queue.Queue()
    with open(log_path, "w", encoding="utf-8", errors="replace") as log:
        log.write("$ %s\n(cwd %s)\n\n" % (subprocess.list2cmdline(cmd), cwd or os.getcwd()))
        log.flush()
        try:
            p = subprocess.Popen(cmd, cwd=cwd, env=env, stdin=subprocess.DEVNULL,
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                 text=True, encoding="utf-8", errors="replace")
        except OSError as e:
            log.write("could not start: %s\n" % e)
            return 127

        def pump():
            for line in p.stdout:
                q.put(line)
            q.put(None)
        threading.Thread(target=pump, daemon=True).start()
        while True:
            try:
                line = q.get(timeout=5)
            except queue.Empty:
                if tick:
                    tick()
                continue
            if line is None:
                break
            log.write(line)
            log.flush()
            if on_line:
                on_line(line)
        return p.wait()


def tail(path, n=12):
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            return "".join(f.readlines()[-n:])
    except OSError:
        return ""


def run_pipeline(repo, tk, xbe, gen, work, prog):
    """tools/run_pipeline.py from a clean tree: parse..recomp, no Ghidra."""
    if os.path.isdir(gen):
        shutil.rmtree(gen)
    evid = os.path.join(work, "pipeline")
    log = os.path.join(work, "logs", "pipeline.log")
    cmd = [sys.executable, os.path.join(repo, "tools", "run_pipeline.py"),
           "--toolkit", tk, "--xbe", xbe, "--gen-dir", gen, "--to", "recomp",
           "--seeds", os.path.join(repo, "names", "icall_seeds.json"),
           "--evidence-dir", evid, "--no-cached-names", "--no-names"]
    order = ["parse", "disasm", "funcid", "abi", "ghidra", "recomp"]
    total = float(sum(PIPE_SECONDS[s] for s in order))
    state = {"step": "parse", "t0": time.time()}

    def frac():
        before = sum(PIPE_SECONDS[s] for s in order[:order.index(state["step"])])
        within = min(time.time() - state["t0"], PIPE_SECONDS[state["step"]] * 0.95)
        return (before + within) / total

    def on_line(line):
        m = re.match(r"^\[(\w+)\] (\S.*)$", line.strip())
        if m and m.group(1) in order and m.group(1) != state["step"]:
            state["step"], state["t0"] = m.group(1), time.time()
            prog("pipeline", frac(), "translating the game's code: part %d of %d" % (
                order.index(state["step"]) + 1, len(order)))

    def tick():
        prog("pipeline", frac(), "translating the game's code: part %d of %d (%d s)" % (
            order.index(state["step"]) + 1, len(order), time.time() - state["t0"]))

    env = dict(os.environ, PYTHONIOENCODING="utf-8", PYTHONDONTWRITEBYTECODE="1")
    prog("pipeline", 0, "translating the game's code (5 to 10 minutes)")
    t0 = time.time()
    rc = run_logged(cmd, log, cwd=repo, env=env, on_line=on_line, tick=tick)
    if rc != 0 or not glob.glob(os.path.join(gen, "*.c")):
        raise Fail(EXIT_PIPELINE, "pipeline", "The translation failed (exit %d). Log: %s\n%s"
                   % (rc, log, tail(log)))
    prog("pipeline", 1, "translated in %d s" % (time.time() - t0))
    return time.time() - t0


def verify_gen(repo, gen, prog, allow_unverified=False, log_dir=None):
    """The generated C against fingerprints/generated.json: (total, differing files).
    With allow_unverified a mismatch is logged (logs/generated_differs.txt) and
    the build goes on, marked unverified."""
    ref = load_json(os.path.join(FINGERPRINTS, "generated.json"), "fingerprints/generated.json")
    got = gen_fingerprint(gen)
    total = total_digest((n, f["sha256"]) for n, f in got.items())
    if total != ref.get("total_sha256"):
        want = ref.get("files", {})
        differ = sorted(n for n in set(got) | set(want)
                        if got.get(n, {}).get("sha256") != want.get(n, {}).get("sha256"))
        if allow_unverified:
            listing = None
            if log_dir:
                os.makedirs(log_dir, exist_ok=True)
                listing = os.path.join(log_dir, "generated_differs.txt")
                with open(listing, "w", encoding="utf-8") as f:
                    f.write("Generated files that differ from fingerprints/generated.json (%d of %d):\n"
                            % (len(differ), len(want)))
                    for n in differ:
                        f.write("  %s: ours %s, this copy's %s\n" % (
                            n, want.get(n, {}).get("sha256", "(none)"), got.get(n, {}).get("sha256", "(none)")))
            prog("verify", 1, "the generated code differs from ours in %d of %d files (%s%s), as expected "
                 "for an unverified copy: building it anyway%s" % (
                     len(differ), len(want), ", ".join(differ[:3]), ", ..." if len(differ) > 3 else "",
                     "; list: %s" % listing if listing else ""))
            return total, differ
        inputs = gen_inputs(repo)
        changed = sorted(k for k, v in inputs.items() if ref.get("inputs", {}).get(k) != v)
        hint = (" Source files that differ from the ones the fingerprints were made with: %s."
                % ", ".join(changed)) if changed else ""
        raise Fail(EXIT_GEN, "verify", "The generated code doesn't match ours: %d of %d files "
                   "differ (%s%s).%s Check Python 3.13 and capstone %s, and that the source is "
                   "complete and unchanged." % (len(differ), len(want), ", ".join(differ[:4]),
                                                 ", ..." if len(differ) > 4 else "", hint, CAPSTONE))
    prog("verify", 1, "the generated code matches ours (%d files, SHA-256 %s...)" % (len(got), total[:12]))
    return total, []


BUILD_SOURCE = "bw_build_source.json"      # in the build folder: what its last good build was made from


def build_source(repo, tk, gen, work, cfg, tools):
    """A digest of everything the build reads except the regenerated C (which is
    written fresh each time): the source tree's build files (CMakeLists.txt,
    cmake/, src/ and the tools CMakeLists.txt runs), the toolkit's pinned commit
    and folder, CMake's options and the tools' versions. File times play no
    part: an older version's objects can be newer than this version's sources
    (Setup keeps the times the files had when its source was exported)."""
    skip = {os.path.normcase(os.path.abspath(p)) for p in (work, gen, os.path.join(repo, "src", "recomp", "gen"))}
    rels = []
    for top in ("CMakeLists.txt", "cmake", "src"):
        p = os.path.join(repo, top)
        if os.path.isfile(p):
            rels.append(top)
        elif os.path.isdir(p):
            for d, dirs, names in os.walk(p):
                dirs[:] = sorted(x for x in dirs if x != "__pycache__"
                                 and os.path.normcase(os.path.abspath(os.path.join(d, x))) not in skip)
                for n in sorted(names):
                    rels.append(os.path.relpath(os.path.join(d, n), repo).replace(os.sep, "/"))
    try:
        with open(os.path.join(repo, "CMakeLists.txt"), encoding="utf-8", errors="replace") as f:
            rels += sorted(set(re.findall(r"tools/[\w.-]+\.py", f.read())))       # bw_icon.py, bw_ocean_xlate.py
    except OSError:
        pass
    h = hashlib.sha256()
    for rel in rels:
        p = os.path.join(repo, *rel.split("/"))
        if os.path.isfile(p):
            h.update(("%s\0%s\n" % (rel, sha256_file(p))).encode("utf-8"))
    h.update(("toolkit %s %s %s\n" % (TOOLKIT_COMMIT, TOOLKIT_TREE_SHA256, os.path.abspath(tk))).encode("utf-8"))
    h.update(("cmake %s\n" % subprocess.list2cmdline(cfg[1:])).encode("utf-8"))
    h.update(("tools %s\n" % json.dumps(tools, sort_keys=True)).encode("utf-8"))
    return {"format": 1, "digest": h.hexdigest(), "files": len(rels), "toolkit": TOOLKIT_COMMIT,
            "cmake_args": cfg[1:], "tools": tools}


def plan_build(bdir, want):
    """(clean, why): build everything again unless the build folder's last good
    build was made from exactly this source (a repair or a re-run of the same
    version stays incremental)."""
    if not os.path.isfile(os.path.join(bdir, "CMakeCache.txt")):
        return False, "a new build folder"
    try:
        with open(os.path.join(bdir, BUILD_SOURCE), encoding="utf-8") as f:
            old = json.load(f)
    except (OSError, ValueError):
        return True, "the last build here was another version, or didn't finish"
    if old.get("digest") != want["digest"]:
        return True, "the source changed since the last build"
    return False, "the same source as the last build"


def build(repo, tk, gen, work, cmake, jobs, prog, tools=None):
    bdir = os.path.join(work, "build")
    logc = os.path.join(work, "logs", "cmake_configure.log")
    logb = os.path.join(work, "logs", "cmake_build.log")
    cfg = [cmake, "-S", repo, "-B", bdir, "-G", "Visual Studio 17 2022", "-A", "x64",
           "-DXBOXRECOMP_DIR=" + tk.replace("\\", "/"), "-DBW_GEN_DIR=" + gen.replace("\\", "/"),
           "-DPython3_EXECUTABLE=" + sys.executable.replace("\\", "/"),
           # The indirect-call recorder is a development aid: players don't need
           # icall_targets.dump written beside the game (0.2.5 item 6).
           "-DBW_ICALL_FEEDBACK=OFF"]
    # Item 101: a build folder from another version (or from a build that
    # failed) is built again from scratch; the record of what a build was made
    # from is written only once it has succeeded.
    want = build_source(repo, tk, gen, work, cfg, tools or {})
    clean, why = plan_build(bdir, want)
    stamp = os.path.join(bdir, BUILD_SOURCE)
    if os.path.exists(stamp):
        os.remove(stamp)
    prog("build", 0, "configuring (CMake, Visual Studio 2022 x64)")
    if run_logged(cfg, logc, cwd=repo) != 0:
        raise Fail(EXIT_BUILD, "build", "CMake couldn't configure the build. Log: %s\n%s" % (logc, tail(logc)))
    srcs = (len(glob.glob(os.path.join(gen, "*.c"))) + len(glob.glob(os.path.join(repo, "src", "*.c")))
            + len(glob.glob(os.path.join(repo, "src", "*", "*.c")))
            + len(glob.glob(os.path.join(repo, "src", "*", "*", "*.c")))
            + len(glob.glob(os.path.join(tk, "src", "**", "*.c"), recursive=True)))
    state = {"n": 0, "t0": time.time()}

    def on_line(line):
        if re.match(r"^\s+[\w.-]+\.c\s*$", line):
            state["n"] += 1
            if state["n"] % 10 == 0:
                prog("build", min(0.95, state["n"] / float(srcs or 1)),
                     "compiling (%d of about %d files)" % (state["n"], srcs))

    def tick():
        prog("build", min(0.95, state["n"] / float(srcs or 1)),
             "compiling (%d of about %d files, %d s)" % (state["n"], srcs, time.time() - state["t0"]))

    cmd = [cmake, "--build", bdir, "--config", "Release", "--parallel", str(jobs)]
    if clean:
        cmd.append("--clean-first")
        prog("build", 0, "compiling everything again (%s)" % why)
    else:
        prog("build", 0, "compiling" if why == "a new build folder" else "compiling what changed (%s)" % why)
    if run_logged(cmd, logb, cwd=repo, on_line=on_line, tick=tick) != 0:
        raise Fail(EXIT_BUILD, "build", "The build failed. Log: %s\n%s" % (logb, tail(logb, 20)))
    exe = os.path.join(bdir, "Release", "bloodwake.exe")
    if not os.path.isfile(exe):
        raise Fail(EXIT_BUILD, "build", "The build finished without bloodwake.exe. Log: %s" % logb)
    want["built"] = _dt.datetime.now().isoformat(timespec="seconds")
    want["clean"] = clean
    with open(stamp, "w", encoding="utf-8") as f:
        json.dump(want, f, indent=1)
    prog("build", 1, "built in %d s" % (time.time() - state["t0"]))
    return exe, time.time() - state["t0"]


CMD_LAUNCHER = """@echo off
rem Blood Wake: opens the launcher (settings and a Play button; tools\\launcher.pyw).
cd /d "%%~dp0"
start "" "%s" tools\\launcher.pyw
"""
CMD_PLAY = """@echo off
rem Play Blood Wake. With no options the launcher opens; with options it plays
rem straight away:  play.cmd --edition remastered    play.cmd --keyboard
cd /d "%%~dp0"
if "%%~1"=="" (
  start "" "%s" tools\\launcher.pyw
) else (
  "%s" tools\\play.py %%*
)
"""


def copy_note(record):
    """bw_copy.txt's one line (contract 9), or None for a verified copy."""
    c = record.get("copy") or {}
    if c.get("verdict") in (None, VERIFIED):
        return None
    line = COPY_WORDS[c["verdict"]]
    if c.get("accepted"):
        line += ", accepted in %s %s" % (c.get("accepted_in") or "the build command",
                                          c["accepted"].replace("T", " ")[:16])
    return line


STAGED_TOOLS = ("launcher.pyw", "play.py", "bw_warmup.py", "bw_hdr.py")   # into <install>/tools


def stage(repo, exe, inst, record, prog):
    rel = os.path.join(inst, "build", "Release")
    os.makedirs(rel, exist_ok=True)
    # the exe, its symbols, and the icon the build drew beside it (the launcher's window)
    for src in (exe, os.path.splitext(exe)[0] + ".pdb", os.path.join(os.path.dirname(exe), "bloodwake.ico")):
        if os.path.isfile(src):
            shutil.copy2(src, os.path.join(rel, os.path.basename(src)))
    note, note_path = copy_note(record), os.path.join(rel, "bw_copy.txt")
    if note:
        with open(note_path, "w", encoding="utf-8") as f:
            f.write(note + "\n")
    elif os.path.exists(note_path):
        os.remove(note_path)               # an earlier install's, from another copy
    os.makedirs(os.path.join(inst, "tools"), exist_ok=True)
    # The launcher and what it imports from its own folder: bw_warmup (the
    # Prepare-graphics offer, item 21) and bw_hdr (Automatic HDR, items 97,
    # 100). Without them those two features silently do nothing (V, RC4).
    for name in STAGED_TOOLS:
        shutil.copy2(os.path.join(repo, "tools", name), os.path.join(inst, "tools", name))
    py = sys.executable
    pyw = os.path.join(os.path.dirname(py), "pythonw.exe")
    pyw = pyw if os.path.isfile(pyw) else py
    with open(os.path.join(inst, "Blood Wake.cmd"), "w", newline="\r\n") as f:
        f.write(CMD_LAUNCHER % pyw)
    with open(os.path.join(inst, "play.cmd"), "w", newline="\r\n") as f:
        f.write(CMD_PLAY % (pyw, py))
    with open(os.path.join(inst, "install.json"), "w", encoding="utf-8") as f:
        json.dump(record, f, indent=2)
    prog("stage", 1, "installed in %s" % inst)


def warmup(exe, game_dir, work, prog):
    """tools/bw_warmup.py's scenes with the installed exe. Never raises: the game
    works without it (its first play just compiles the shaders as it goes)."""
    t0 = time.time()
    out = {"seconds": 0, "new_shaders": 0, "in_cache": None, "scenes": 0, "problems": []}
    try:
        import bw_warmup                    # tools/, this script's own folder
        prog("warmup", 0, "preparing graphics (one time, about 2 minutes): short scenes of the "
             "game, hidden and silent")
        w = bw_warmup.warmup(exe, game_dir, os.path.join(work, "warmup"),
                             prog=lambda f, m: prog("warmup", f, m))
        n, total, bad = bw_warmup.summary(w)
        out.update(new_shaders=n, in_cache=total, scenes=len(w["results"]), settings=w["settings"],
                   problems=["%s %s: %s" % (r["edition"], r["scene"], r["error"] or "no shader report")
                             for r in bad])
        msg = "graphics ready: %d new shaders prepared%s in %d s" % (
            n, "" if total is None else " (%d in the shader cache)" % total, time.time() - t0)
        if bad:
            msg += " (%d of %d scenes had a problem: %s; log: %s)" % (
                len(bad), len(w["results"]), "; ".join(out["problems"][:2]),
                os.path.join(work, "warmup", "logs"))
        prog("warmup", 1, msg)
    except Exception as e:                  # report and go on: never fail the install here
        out["problems"].append("%s: %s" % (type(e).__name__, e))
        prog("warmup", 1, "preparing graphics was skipped (%s); the first play compiles the "
             "shaders as it goes" % e)
    out["seconds"] = round(time.time() - t0)
    return out


# ── Main ──────────────────────────────────────────────────────────────────

def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--disc", required=True, help="a folder of the disc's files, an XISO or a full disc image")
    ap.add_argument("--install-dir", required=True, help="where the game goes (created if needed)")
    ap.add_argument("--toolkit", help="a clean xboxrecomp checkout at %s (default: download it)" % TOOLKIT_COMMIT[:7])
    ap.add_argument("--work-dir", help="for the toolkit, the generated C and the build (default <install-dir>/work)")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4, help="parallel build jobs")
    ap.add_argument("--check-only", action="store_true", help="only identify and check the disc")
    ap.add_argument("--no-warmup", action="store_true",
                    help="skip preparing the graphics (the first play then compiles its shaders as it goes)")
    ap.add_argument("--progress", choices=("text", "json"), default="text")
    ap.add_argument("--allow-unverified", action="store_true",
                    help="build a copy that isn't the recognised retail one (another region, modified or "
                         "damaged), after the player accepted Setup's warning: it's recorded as such")
    ap.add_argument("--accepted-in", default=None, help=argparse.SUPPRESS)   # "Setup", for bw_copy.txt
    a = ap.parse_args(argv)
    prog = Progress(a.progress == "json")
    inst = os.path.abspath(a.install_dir)
    work = os.path.abspath(a.work_dir) if a.work_dir else os.path.join(inst, "work")
    game_dir = os.path.join(inst, "game", "disc")
    t_start = time.time()
    times = {}
    try:
        notes = check_python()
        retail = Retail(load_json(os.path.join(FINGERPRINTS, "retail.json"), "fingerprints/retail.json"))
        if not a.check_only:
            load_json(os.path.join(FINGERPRINTS, "generated.json"), "fingerprints/generated.json")
            vs, vs_ver = find_vs()
            if not vs:
                raise Fail(EXIT_DEP, "identify", "Visual Studio 2022 (or the Build Tools for Visual "
                           "Studio 2022) with the 'Desktop development with C++' workload is needed.")
            cmake, cmake_ver = find_cmake(vs)
            if not cmake:
                raise Fail(EXIT_DEP, "identify", "CMake 3.20 or newer is needed (Visual Studio's "
                           "C++ CMake tools component, or cmake.org).")
            if os.path.splitdrive(inst)[0].lower() == os.path.splitdrive(work)[0].lower():
                check_space(inst, NEED_INSTALL + NEED_WORK, "identify", "the game's files and the build")
            else:
                check_space(inst, NEED_INSTALL, "identify", "the game's files")
                check_space(work, NEED_WORK, "identify", "the build")
        for n in notes:
            prog("identify", 0, n)
        kind, src = identify(a.disc)
        os.makedirs(work, exist_ok=True)
        tk = None
        if kind == "image" or not a.check_only:
            t0 = time.time()
            tk = get_toolkit(a, work, prog)
            times["toolkit"] = time.time() - t0
        t0 = time.time()
        disc = check_disc(a, kind, src, tk, game_dir, retail, prog)
        times["unpack_and_check"] = time.time() - t0
        if a.check_only:
            if disc["verdict"] == VERIFIED:
                prog("done", 1, "the disc is fine: an unmodified %s copy" % disc["label"], disc=disc)
            else:
                prog("done", 1, "the disc can be built as accepted: %s" % COPY_WORDS[disc["verdict"]], disc=disc)
            return EXIT_OK
        accepted = _dt.datetime.now().isoformat(timespec="seconds") if disc["verdict"] != VERIFIED else None

        def hint(f):
            """A failure of a copy that isn't the retail one says so."""
            if disc["verdict"] == UNVERIFIED:
                f.msg += ("\nThis copy's main program isn't the retail one Setup knows, which is the likely "
                          "cause: other versions may not build.")
            return f

        xbe_dir = os.path.join(work, "xbe")         # the pipeline writes its analysis beside the XBE
        os.makedirs(xbe_dir, exist_ok=True)
        xbe = os.path.join(xbe_dir, "default.xbe")
        shutil.copy2(next(p for r, p in folder_files(game_dir) if r.lower() == "default.xbe"), xbe)
        gen = os.path.join(work, "gen")
        try:
            times["pipeline"] = run_pipeline(REPO, tk, xbe, gen, work, prog)
            gen_total, gen_differ = verify_gen(REPO, gen, prog, a.allow_unverified, os.path.join(work, "logs"))
            exe, times["build"] = build(REPO, tk, gen, work, cmake, a.jobs, prog,
                                        {"python": sys.version.split()[0], "cmake": cmake_ver,
                                         "visual_studio": vs_ver, "capstone": module_version("capstone")})
        except Fail as f:
            raise hint(f)
        copy = {"verdict": UNVERIFIED if gen_differ else disc["verdict"], "accepted": accepted}
        if accepted:
            copy["accepted_in"] = a.accepted_in or "the build command"
        if gen_differ:
            copy["generated_files_differ"] = len(gen_differ)
        record = {
            "format": 1,
            "installed": _dt.datetime.now().isoformat(timespec="seconds"),
            "editions": ["original", "remastered"],
            "source": {"commit": run_text(["git", "rev-parse", "HEAD"], REPO)
                       if os.path.exists(os.path.join(REPO, ".git")) else None,
                       "generated_inputs": gen_inputs(REPO)},
            "toolkit": {"commit": TOOLKIT_COMMIT, "path": tk},
            "tools": {"python": sys.version.split()[0], "python_exe": sys.executable,
                      "capstone": module_version("capstone"), "cmake": cmake_ver,
                      "visual_studio": vs_ver},
            "copy": copy,
            "disc": disc,
            "generated_total_sha256": gen_total,
            "paths": {"game": game_dir, "work": work, "exe": os.path.join(inst, "build", "Release", "bloodwake.exe")},
            "seconds": {k: round(v) for k, v in times.items()},
        }
        stage(REPO, exe, inst, record, prog)
        if not a.no_warmup:
            record["warmup"] = warmup(record["paths"]["exe"], game_dir, work, prog)
            record["seconds"]["warmup"] = record["warmup"]["seconds"]
            try:                            # the game is installed: never fail it here
                with open(os.path.join(inst, "install.json"), "w", encoding="utf-8") as f:
                    json.dump(record, f, indent=2)
            except OSError as e:
                prog("warmup", 1, "the warm-up's results weren't added to install.json (%s); "
                     "the game is installed and works" % e)
        prog("done", 1, "Blood Wake is built. Start it with \"Blood Wake.cmd\" in %s (%d min)."
             % (inst, (time.time() - t_start) / 60))
        return EXIT_OK
    except Fail as f:
        prog(f.step, 0, f.msg, error=True, exit=f.code, **f.extra)
        return f.code
    except OSError as e:
        prog("done", 0, "File error: %s" % e, error=True, exit=EXIT_IO)
        return EXIT_IO
    except Exception as e:                  # never a raw traceback: a plain line and the usual codes
        import traceback
        step = prog.step
        log = os.path.join(work, "logs", "build_from_disc_error.log")
        try:
            os.makedirs(os.path.dirname(log), exist_ok=True)
            with open(log, "w", encoding="utf-8") as f:
                traceback.print_exc(file=f)
        except OSError:
            log = "(not written)"
        code = STEP_EXIT.get(step, EXIT_IO)
        prog(step, 0, "Something unexpected went wrong while %s: %s: %s. Details: %s" % (
            STEP_WORDS.get(step, step), type(e).__name__, e, log), error=True, exit=code)
        return code


if __name__ == "__main__":
    sys.exit(main())
