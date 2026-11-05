# xemu's MCPX DSP (vendored for D-012)

- **Source:** xemu v0.8.139 (`hw/xbox/mcpx/apu/dsp/`: `dsp.c`, `dsp.h`, `dsp_c.c`, `dsp_dma.*`, `dsp_internal.h`, `debug.h`, `interp/`). The JIT is not vendored.
- **License:** GPL-2.0-or-later as the files' headers say (`dsp_dma.*`: LGPL-2.0-or-later). Our additions below are GPL-2.0-or-later too (SPDX line at the top of each).
- **Our additions:**
  - `shim/`: stand-ins for the few QEMU/GLib pieces it uses (`g_new0`, `g_free`, `ARRAY_SIZE`, `MIN`, `ssize_t`, `ldl_le_p`/`stl_le_p`, trace hooks, `g_config`);
  - `bw_dsp.[ch]`: an opaque wrapper, because the toolkit's APU headers also define a `DSPState`. It also holds the engine-neutral whole state (`BwDspState`), the callback trampolines and observation hooks for `BW_DSP_TRACE`, the interpreter as an engine, and the engine switch (`BW_DSP_ENGINE`) with the lockstep check (`BW_DSP_LOCKSTEP`);
  - `bw_dsp_engine.h`: the interface a DSP engine implements (the interpreter, or the audio rework's translated engine);
  - `bw_dsp_trace_fmt.[ch]`: the DSP trace format's constants, hashes and chunk codec, shared with the offline replayer (`tools/dsp_replay`, built from this library; `docs/audio/trace_format.md`);
  - `bw_dsp_fast.[ch]` (lane au-dsp): the translated engine. It `#include`s `interp/dsp_cpu.c` (with its five extern functions renamed) to reuse the interpreter's own helpers, and adds handlers whose semantics follow xemu's routines (adapted from them, so GPL-2.0-or-later). `bw_dsp_jit.inc` (its x64 block JIT, included by it) and `bw_dsp_x64.inc` (the JIT's small x64 encoder) are part of it, under the same licence;
  - `CMakeLists.txt`.
- **Changes to xemu's files:** none.
- **The glue** (xemu's `gp_ep.c` equivalent) is `src/toolkit_patches/apu/bw_apu_xdsp.c`.
