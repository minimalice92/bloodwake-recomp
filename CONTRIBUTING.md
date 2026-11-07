# Contributing

Thanks for wanting to help. Bug reports, test results and fixes to our own code are all welcome.

## Issues
- **Welcome:** build problems, crashes, things that look or sound different from the original game, settings that don't do what they say, and documentation that's unclear. Say what you did, what you expected, what you saw, and your Windows version, graphics card and the build script's exit code if there was one. The build log is in `<work-dir>\logs\`.
- **Not welcome:** requests for the game, its files, disc images, builds or the generated C, and links to any of them. Such issues are closed and links removed. Please don't ask how to rip a disc or get around any protection, either: the project doesn't help with that. <!-- audit:allow off-limits -->
- **Screenshots:** please don't post screenshots or recordings of the game. Describe what you see instead (for example "the water in mission 3 flickers at the horizon").

## Pull requests
- Change only this project's own files, and keep each change small and focused.
- **Never commit anything from the game**, or anything made from it: no game files, extracted assets, generated C (`src/recomp/gen/` or your work folder), executables, captures or screenshots. Turn the commit guard on in your clone first: `git config core.hooksPath tools/git-hooks`.
- **Original stays exactly as shipped.** Any change to what the game does goes behind the edition switch (Remastered) with its own on/off setting. Fixes for problems the port itself causes are the exception.
- By contributing you agree your work is released under this project's MIT licence (or, for files in `src/xemu_dsp/`, the GPL-2.0-or-later).
- **How an accepted pull request lands:** this repository is published from a private working copy, one sync per release. An accepted fix is applied there and comes back with the next release, and the release notes name the pull request. So pull requests are closed rather than merged; that's not a rejection.

## Forks and ports
Ports to other systems are welcome. They're the fork's own work, and this project stays Windows-only. Here's where to look:
- **The renderer's shared core,** `src/gpu/core/`: guest surfaces, the texture cache, render-state translation, per-draw preparation and frame pacing. It makes no graphics-API call; a back end supplies the API objects. The Windows back end is `src/gpu/bw_d3d11.c`, with the shaders generated as HLSL (`bw_vsh_hlsl.c`, `bw_rc_hlsl.c`).
- **Windows-specific pieces:**
  - the Direct3D back end and visual suite (`src/gpu/`, outside `core/`);
  - sound output (`src/toolkit_patches/apu/`, XAudio2 and WASAPI);
  - window, video and input (`src/toolkit_patches/video/`, `input/` and `usb/`);
  - the launcher's Windows calls (`tools/launcher.pyw`, `tools/bw_hdr.py`);
  - Setup (`installer/`, Inno Setup);
  - `play.cmd`.
- **Portable as it stands:** the game's own code (the generated C, made on your PC), our hooks and edition fixes (`src/*.c`, registered in `src/bw_fixes.def`), and the sound chip's DSP interpreter (`src/xemu_dsp/`, Clang-clean since 0.3.2).
- **Check your generation:** `fingerprints/generated.json` records the public generation's file hashes, with the toolkit commit, Python and capstone versions pinned. If yours match, you're building the same game code we test.
- **Keep the guard on.** `tools/git-hooks/pre-commit` is executable since 0.3.2, so `core.hooksPath` works on macOS and Linux too.

## Takedown
If you represent the rights holders, see the [README](README.md#contact-and-takedown).
