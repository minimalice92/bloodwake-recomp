# Third-party notices

This repository's own files are under the MIT licence (`LICENSE`). These parts are other people's work, kept with their notices and under their own licences. The full licence texts are in `LICENSES/`.

| Where | What | Licence | Copyright |
|---|---|---|---|
| `src/xemu_dsp/` (except our shims) | The MCPX DSP56300 interpreter from [xemu](https://github.com/xemu-project/xemu) v0.8.139 (`hw/xbox/mcpx/apu/dsp/`), unchanged | GPL-2.0-or-later; `dsp_dma.*` LGPL-2.0-or-later | espes; Matt Borgerson; adapted from Hatari's DSP emulation (the ARAnyM developer team; Thomas Huth). See each file's header. |
| `src/xemu_dsp/shim/`, `bw_dsp.*`, `CMakeLists.txt` | Our stand-ins for the few QEMU pieces the DSP uses, and its wrapper | GPL-2.0-or-later (they exist only to host the DSP) | the bloodwake-recomp contributors |
| `src/xemu_dsp/bw_dsp_fast.*`, `src/xemu_dsp/bw_dsp_jit.inc`, `src/xemu_dsp/bw_dsp_x64.inc`, `tools/dsp_bench/` | Our translated DSP engine (with its x64 JIT) and its benchmark, adapted from xemu's interpreter (whose source it includes) | GPL-2.0-or-later | the bloodwake-recomp contributors; xemu's authors as above |
| `src/toolkit_patches/apu/apu_core.c`, `apu_dsp.c`, `apu_vp.c` | Patched copies of xboxrecomp's MCPX APU files, which xboxrecomp extracted from xemu | LGPL-2.1-or-later | espes; Jannik Vogel; Matt Borgerson; changes by the bloodwake-recomp contributors |
| `src/toolkit_patches/` (the other files) | Patched copies of xboxrecomp runtime files, each with a `.diff` showing exactly what changed | MIT (`LICENSES/MIT-xboxrecomp.txt`) | sp00nz; changes by the bloodwake-recomp contributors |

Downloaded at build time, not part of this repository:

| What | Licence |
|---|---|
| [xboxrecomp](https://github.com/sp00nznet/xboxrecomp) at the pinned commit (the static recompiler and its runtime), which has its own third-party notices | MIT, with LGPL-2.1-or-later parts (its `NOTICE` and `LICENSES/`) |
| Python packages from `tools/requirements.txt` (capstone, pefile and others) | their own licences |

Microsoft's compiler, Windows SDK, Direct3D 11, XInput and XAudio2 are used through the tools you install; nothing of theirs is copied here.

## What the GPL means here
The game executable you build links the GPL-2.0-or-later DSP, so that executable as a whole falls under the GPL-2.0-or-later. You build it yourself on your own PC. If you ever pass a build on, the GPL's terms apply to it; but a build also contains code translated from the game, which nobody may pass on. Please don't share builds.
