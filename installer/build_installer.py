#!/usr/bin/env python3
"""Build Blood Wake Recomp's Setup (installer/BloodWakeRecomp.iss) with Inno Setup 6.7.3.

    python installer/build_installer.py [--source <public tree>] [--out <dir>]
        [--iscc <ISCC.exe>] [--version 0.1.0]

--source is the public source tree the installer carries and builds from
(default: this repository). It must be a git checkout: only its tracked files
are taken, and installer/ itself and .git are left out. The tree is refused if
it holds anything that must never ship: game files (by type, and any file whose
SHA-256 is one of the retail disc's, whatever its name), Xbox programs, disc
images, executables, generated C, or any file over 2 MB.

The retail fingerprint of the game's main program (for the disc page's instant
check) is read from <source>/fingerprints/retail.json.

Setup contains only this project's files and Inno Setup's own runtime. Python,
the Python packages, the toolkit and Microsoft's Build Tools are downloaded by
Setup on the player's PC from their official sources, each checked against a
SHA-256 pinned in BloodWakeRecomp.iss.
"""
import argparse
import glob
import hashlib
import json
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
ISS = os.path.join(HERE, "BloodWakeRecomp.iss")

NEVER_EXT = {".xbe", ".iso", ".xiso", ".exe", ".dll", ".pdb", ".obj", ".lib", ".bin", ".img",
             ".png", ".jpg", ".bmp", ".wav", ".wma", ".bik", ".xpr", ".xwb"}
NEVER_DIRS = ("src/recomp/gen/", "game/", "build/", "analysis/", "installer/")
MAX_BYTES = 2 * 1024 * 1024


def tree_files(src):
    """Relative paths (forward slashes) of the files to ship: a git checkout's
    tracked files only. A plain folder is refused: anything dropped into it
    (a disc file, a build) would ship (0.2.5 item 9)."""
    if not os.path.exists(os.path.join(src, ".git")):
        sys.exit("Refusing %s: the source must be a git checkout (only its tracked files ship). "
                 "Clone the repository, or make the folder a repository with one commit." % src)
    out = subprocess.run(["git", "-C", src, "ls-files", "-z"], capture_output=True, check=True).stdout
    files = [p for p in out.decode("utf-8").split("\0") if p]
    # docs/images/ holds the README's pictures (game screenshots among them, D-019): they
    # belong on the repository page only and never ship inside Setup. Every other image
    # anywhere is still refused below.
    return sorted(f for f in files if not f.startswith("installer/") and not f.startswith(".git")
                  and not f.startswith("docs/images/"))


def disc_hashes(src):
    """The SHA-256 of every file on the retail disc (fingerprints/retail.json)."""
    try:
        with open(os.path.join(src, "fingerprints", "retail.json"), encoding="utf-8") as f:
            return {x["sha256"].lower() for x in json.load(f)["files"]}
    except (OSError, ValueError, KeyError) as e:
        sys.exit("fingerprints/retail.json can't be read (%s): Setup can't check the files it ships" % e)


def refuse(src, files):
    bad = []
    game = disc_hashes(src)
    for rel in files:
        low = rel.lower()
        p = os.path.join(src, *rel.split("/"))
        if os.path.splitext(low)[1] in NEVER_EXT:
            bad.append("%s (file type)" % rel)
        elif any(low.startswith(d) for d in NEVER_DIRS):
            bad.append("%s (folder that never ships)" % rel)
        elif os.path.isfile(p) and os.path.getsize(p) > MAX_BYTES:
            bad.append("%s (over 2 MB)" % rel)
        elif os.path.isfile(p):
            # Whatever its name: a file from the game's disc, or an Xbox program.
            h = hashlib.sha256()
            with open(p, "rb") as f:
                head = f.read(4)
                h.update(head)
                for chunk in iter(lambda: f.read(1 << 20), b""):
                    h.update(chunk)
            if h.hexdigest() in game:
                bad.append("%s (a file from the game's disc)" % rel)
            elif head == b"XBEH":
                bad.append("%s (an Xbox program)" % rel)
    return bad


def find_iscc(given):
    cands = [given, os.environ.get("ISCC"), shutil.which("ISCC")]
    cands += sorted(glob.glob(r"C:\tools\innosetup\innosetup-6.7.3\ISCC.exe"))
    for base in (os.environ.get("LOCALAPPDATA"), os.environ.get("ProgramFiles(x86)"), os.environ.get("ProgramFiles")):
        if base:
            cands.append(os.path.join(base, "Programs" if base == os.environ.get("LOCALAPPDATA") else "",
                                      "Inno Setup 6", "ISCC.exe"))
    for c in cands:
        if c and os.path.isfile(c):
            return c
    sys.exit("ISCC.exe (Inno Setup 6.7.3) not found: pass --iscc or set ISCC.")


def retail_xbe(src):
    with open(os.path.join(src, "fingerprints", "retail.json"), encoding="utf-8") as f:
        d = json.load(f)
    key = hashlib.sha256(b"default.xbe").hexdigest()
    ent = [x for x in d["files"] if x["name_sha256"] == key]
    if not ent:
        sys.exit("fingerprints/retail.json has no entry for default.xbe")
    label = d.get("label", "Xbox retail").replace('"', "'")
    return ent[0]["size"], ent[0]["sha256"].lower(), label


def draw_pictures(stage, out):
    """Setup's icon and pictures, drawn by the staged tools/bw_icon.py (the copy
    Setup ships), so they always match the source it carries. No image file is
    kept in the repository."""
    script = os.path.join(stage, "tools", "bw_icon.py")
    if not os.path.isfile(script):
        sys.exit("%s is missing: the source tree must carry tools/bw_icon.py" % script)
    if os.path.isdir(out):
        shutil.rmtree(out)
    r = subprocess.run([sys.executable, "-I", script, "setup", "--out", out])
    if r.returncode != 0:
        sys.exit("drawing Setup's pictures failed (exit %d)" % r.returncode)
    pics = sorted(os.listdir(out))
    side = [os.path.join(out, n) for n in pics if n.startswith("setup_side_")]
    small = [os.path.join(out, n) for n in pics if n.startswith("setup_small_")]
    if not side or not small:
        sys.exit("tools/bw_icon.py drew no Setup pictures in %s" % out)
    return {"icon": os.path.join(out, "bloodwake.ico"), "side": ",".join(side), "small": ",".join(small)}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--source", default=REPO, help="the public source tree (default: this repository)")
    ap.add_argument("--out", default=os.path.join(HERE, "out"), help="where Setup is written (default installer/out)")
    ap.add_argument("--iscc", help="ISCC.exe (Inno Setup 6.7.3)")
    ap.add_argument("--version", default="0.1.0", help="Setup's version")
    ap.add_argument("--wizard-style", help=argparse.SUPPRESS)   # testing: e.g. "modern light windows11"
    a = ap.parse_args()
    src = os.path.abspath(a.source)
    out = os.path.abspath(a.out)
    files = tree_files(src)
    if not files:
        sys.exit("no files in %s" % src)
    bad = refuse(src, files)
    if bad:
        sys.exit("Refusing to package %s; these must never ship:\n  %s" % (src, "\n  ".join(bad)))
    stage = os.path.join(out, "stage", "source")
    if os.path.isdir(os.path.dirname(stage)):
        shutil.rmtree(os.path.dirname(stage))
    for rel in files:
        dst = os.path.join(stage, *rel.split("/"))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copy2(os.path.join(src, *rel.split("/")), dst)
    size, sha, label = retail_xbe(src)
    pics = draw_pictures(stage, os.path.join(out, "stage", "pictures"))
    iscc = find_iscc(a.iscc)
    cmd = [iscc, "/Q", "/O" + out, "/DSrcTree=" + stage, "/DOutDir=" + out, "/DAppVer=" + a.version,
           "/DXbeSize=%d" % size, "/DXbeSha256=" + sha, "/DRetailLabel=" + label,
           "/DIconFile=" + pics["icon"], "/DWizSide=" + pics["side"], "/DWizSmall=" + pics["small"]]
    if a.wizard_style:
        cmd.append("/DWizStyle=" + a.wizard_style)
    cmd.append(ISS)
    print("%d files from %s; compiling with %s" % (len(files), src, iscc))
    r = subprocess.run(cmd)
    if r.returncode != 0:
        sys.exit("ISCC failed (exit %d)" % r.returncode)
    exe = os.path.join(out, "BloodWakeRecomp-Setup-%s.exe" % a.version)
    h = hashlib.sha256()
    with open(exe, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    print("%s\n  %.1f MB, SHA-256 %s" % (exe, os.path.getsize(exe) / 1048576.0, h.hexdigest()))


if __name__ == "__main__":
    main()
