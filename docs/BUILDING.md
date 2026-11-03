# Building by hand

The installer does all of this for you. This page is for people who'd rather run the steps themselves, or want to know what the installer does.

Everything happens on your own PC. Your disc's files, the C code generated from them and the executable you build never leave it.

## 1. Install the tools
| Tool | Version | Where from |
|---|---|---|
| Windows | 10 or 11, 64-bit | |
| Python | 3.13, 64-bit, with tkinter (the launcher uses it). 3.10 or later runs, but the fingerprints were made with 3.13. | [python.org](https://www.python.org/downloads/windows/) |
| Python packages | exactly the versions in `tools/requirements.txt` (`capstone==5.0.9`, `pefile==2024.8.26`, ...) | `python -m pip install -r tools/requirements.txt` |
| Visual Studio 2022 Build Tools | 17.x, with the **Desktop development with C++** workload (it includes the Windows SDK). Visual Studio 2022 Community/Professional works too. **Not** Visual Studio 2026. | [visualstudio.microsoft.com](https://visualstudio.microsoft.com/downloads/) |
| CMake | 3.20 or later. The copy that comes with Visual Studio is fine. | [cmake.org](https://cmake.org/download/) |
| Git | any recent version (only to clone this repository; you can also download it as a zip) | |

You don't need the `py` launcher, administrator rights, Ghidra, or a copy of the toolkit: the build script fetches the toolkit itself.

## 2. Get this repository
```
git clone https://github.com/minimalice92/bloodwake-recomp.git
cd bloodwake-recomp
python -m pip install -r tools/requirements.txt
```

## 3. Build from your disc
```
python tools/build_from_disc.py --disc "<your copy>" --install-dir "<install folder>"
```
`<your copy>` is one of: a folder with your disc's files, an Xbox ISO (XISO), or a full disc image. Paths with spaces are fine (keep the quotes).

### Options
| Option | What it does |
|---|---|
| `--disc <folder or image>` | Your copy of the game (required). |
| `--install-dir <folder>` | Where the game goes; created if needed (required). |
| `--toolkit <folder>` | Use a clean xboxrecomp checkout you already have. It must be at the pinned commit; the script checks. Without this option the script downloads GitHub's archive of that commit and checks its SHA-256. |
| `--work-dir <folder>` | Where the toolkit, the generated C and the build go (default: `<install-dir>\work`). Logs are in `<work-dir>\logs\`. |
| `--jobs N` | Parallel compile jobs (default: your number of CPU threads). |
| `--check-only` | Only identify your copy and check its fingerprint. Nothing is unpacked or copied. |
| `--allow-unverified` | Build a copy that isn't the recognised retail one (another version, or a modified or damaged copy), as Setup does once you've accepted its warning. A differing program or differing files are reported and the build goes on; it's marked as unverified in `install.json` and in `build\Release\bw_copy.txt`. Use only a copy you made from your own disc: a copy from an unknown source could be tampered with. |
| `--no-warmup` | Skip "Preparing graphics (one time)"; your first game then compiles its shaders as it goes. |
| `--progress json` | One JSON object per line on standard output, for front ends such as the installer: `{"step": "...", "pct": 0-100, "msg": "..."}`. A failure adds `"error": true` and `"exit": <code>`. The default is plain text. |

### What it does, step by step
1. **identify:** works out whether `--disc` is a folder, an XISO or a full disc image.
2. **toolkit:** gets xboxrecomp at the pinned commit (from `--toolkit`, checked, or downloaded and hash-checked).
3. **unpack:** copies your disc's files into `<install-dir>\game\disc` (from an image, with the toolkit's own reader).
4. **fingerprint:** checks every file against `fingerprints/retail.json`. Only an unmodified retail copy passes, unless `--allow-unverified` was given.
5. **pipeline:** translates the game's code to C with the toolkit (`tools/run_pipeline.py`), into `<work-dir>\gen`.
6. **verify:** checks the generated C against `fingerprints/generated.json`. These are hashes of our own run's output, so a match proves your build is the same as ours. The code itself is never published.
7. **build:** CMake and Visual Studio 2022, 64-bit, Release.
8. **stage:** copies the executable, the launcher and the files it runs with into `<install-dir>`, with a small `install.json` that records the versions and your copy's fingerprint.

### Exit codes
| Code | Meaning |
|---|---|
| 0 | Done. |
| 2 | Not a supported, unmodified retail copy (`--allow-unverified` builds it anyway). |
| 3 | A dependency is missing; the message says which. |
| 4 | The translation (pipeline) step failed. |
| 5 | The generated code doesn't match ours (usually a different package or toolkit version). |
| 6 | The build failed. |
| 7 | Not enough disk space, or a file couldn't be read or written. |

## 4. Play
Run the launcher from the install folder (`Blood Wake.cmd`, or `python tools\launcher.pyw`). Pick an edition and your settings, then press Play. See [SETTINGS.md](SETTINGS.md) for every option.

## Building again, or after an update
Pull the new version of this repository and run the same command again. A rebuild doesn't touch your saves (`%LOCALAPPDATA%\xboxrecomp\`, Remastered's in `editions\remastered`) or the launcher's settings (`%LOCALAPPDATA%\BloodWake\settings.ini`). The compiled shader cache is in `%LOCALAPPDATA%\xboxrecomp\shadercache` and rebuilds itself if needed.

## For developers
- The generated C lives in the work folder and must never be committed or shared. The commit guard blocks it, along with game files, images and executables: enable it in your clone with `git config core.hooksPath tools/git-hooks`.
- `src/toolkit_patches/` holds patched copies of a few toolkit runtime files, compiled in place of the toolkit's own. Each has a `.diff` showing the exact change, and CMake checks the SHA-256 of the toolkit file it was made from, so a toolkit change stops the build until the patch has been checked again.
- The CMake option `BW_GEN_DIR` points the build at generated C outside the source tree; `build_from_disc.py` sets it for you.
