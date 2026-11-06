cmake_minimum_required(VERSION 3.20)

# ============================================================================
# YOUR_GAME_NAME - Static Recompilation
#
# Copy this template to your game project root and customize:
#   1. Change the project name below
#   2. Set XBOXRECOMP_DIR to where you cloned xboxrecomp
#   3. Adjust source file lists as you add game-specific code
#   4. Add any game-specific compile definitions or link libraries
# ============================================================================

project(bloodwake C)
if(MSVC)
    enable_language(RC)       # src/bloodwake.rc: the icon
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Common definitions
add_compile_definitions(
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)


# The generated C (tools/run_pipeline.py --gen-dir). By default in the source
# tree, as always; tools/build_from_disc.py points it at its work folder.
set(BW_GEN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/recomp/gen" CACHE PATH
    "Folder holding the generated C (recomp_*.c, recomp_funcs.h, recomp_types.h)")

# Build-time translators (tools/bw_ocean_xlate.py) run with the Python CMake
# finds, or the one given as -DPython3_EXECUTABLE=... (no `py` launcher needed).
find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)

# ── xboxrecomp toolkit ──────────────────────────────────────────
#
# Option A: xboxrecomp as a subdirectory (simplest)
#   Place xboxrecomp/ next to your game project and use a relative path.
#
# Option B: find_package (if you install xboxrecomp system-wide)
#   find_package(xboxrecomp REQUIRED)
#
# Adjust this path to point to your xboxrecomp clone:
set(XBOXRECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../xboxrecomp" CACHE PATH
    "Path to the xboxrecomp toolkit root directory")

# Blood Wake: debug symbols in Release (no change to the generated machine code;
# the linker's REF/ICF optimisations are kept), so the profiler (BW_PROFILE,
# src/bw_profile.c) and the toolkit's fault handler can name functions. Set
# before the toolkit is added so its libraries carry symbols too.
if(MSVC)
    string(APPEND CMAKE_C_FLAGS_RELEASE " /Zi /FS")
    string(APPEND CMAKE_EXE_LINKER_FLAGS_RELEASE " /DEBUG /OPT:REF /OPT:ICF")
endif()

# Add xboxrecomp as a subdirectory -- this builds all 6 runtime libraries
# and provides the 'xboxrecomp' umbrella target to link against.
add_subdirectory(${XBOXRECOMP_DIR} ${CMAKE_BINARY_DIR}/xboxrecomp)

# ── Recompiled game executable ──────────────────────────────────

# Collect generated recompiled code.
# The recomp pipeline outputs files into src/recomp/gen/.
# With --split 1000, you get gen_0000.c, gen_0001.c, etc.
# CONFIGURE_DEPENDS so re-running the recompiler picks up added/removed files
# without a manual re-configure. A plain GLOB is evaluated once at configure
# time, so a newly generated file (an extra chunk, recomp_stubs_unresolved.c)
# is silently left out of the build and shows up as unresolved externals.
file(GLOB RECOMP_GEN_SOURCES CONFIGURE_DEPENDS "${BW_GEN_DIR}/*.c")
if(NOT RECOMP_GEN_SOURCES)
    message(FATAL_ERROR "No generated C in ${BW_GEN_DIR}. Run tools/run_pipeline.py first "
                        "(or tools/build_from_disc.py), or pass -DBW_GEN_DIR=<its --gen-dir>.")
endif()

# Core game sources
set(BW_EDITION_EXTRA_SOURCES "")
set(GAME_SOURCES
    src/main.c
    src/recomp_manual.c
    src/bw_profile.c
    src/bw_framelog.c         # BW_FRAMELOG: per-frame events and long-frame stacks (P2b, test aid)
    src/bw_latlog.c           # BW_LATLOG: controller-latency timestamps (lat-m, test aid)
    src/bw_padwatch.c         # BW_PADWATCH: when the game sees each pad (au-iso, test aid)
    src/bw_host.c
    src/bw_wait.c
    src/bw_state.c
    src/bw_debugflags.c
    src/bw_edition.c          # BW_EDITION: the editions (D-015)
    src/bw_hookcheck.c        # item 62: hooks check the code they attach to (bw_hookcheck_table.h)
    src/bw_settings.c         # the launcher's settings file (bw_settings.h)
    src/bw_keys.c             # settings.ini [keys]: key bindings, steering ramp, deadzones (0.2.5)
    src/bw_hifps_hooks.c      # Remastered high frame rate (D-016): D-011 wrappers, see the file
    src/bw_widescreen.c       # Remastered widescreen: the picture's shape
    src/bw_monitor.c          # the monitor the game is on (0.2.5 item 34, contract 6)
    src/bw_remaster_hooks.c   # edition hooks (D-011 wrappers; run_pipeline scans it)
    ${BW_EDITION_EXTRA_SOURCES}
    src/bw_audio_hooks.c      # audio D-011 wrappers: the speaker configuration (docs/day1/D2.md)
    src/bw_fix_hooks.c        # Remastered (D-016): the original game's own bugs fixed (docs/day1/D11.md)
    src/bw_visextra_hooks.c   # Remastered: faithful game-side visual extras (docs/day1/T3.md)
    src/bw_ocean_native.c     # Remastered: the native ocean (docs/day1/P2a.md, OCEAN_NATIVE)
    src/bw_cheat_hooks.c      # settings.ini [cheats] on the first boot (0.2.5 item 77; D-011 wrapper)
    src/bw_menu_hooks.c       # the menus on a PC: Quit Game, the mouse (0.2.5 items 78-79; D-011 wrappers)
    src/gpu/bw_d3d11.c
    src/gpu/bw_rc_hlsl.c
    src/gpu/bw_vsh_hlsl.c
    src/gpu/bw_gpu_audit.c    # BW_GPU_AUDIT=1: what the game asks of the GPU, counted
    src/gpu/bw_ab.c           # BW_GPU_AB: same-run A/B for GPU fixes (test aid)
    src/gpu/bw_gamma.c        # the title's DAC gamma ramp (D3DDevice_SetGammaRamp hook)
    src/gpu/bw_gpu_failtest.c # BW_GPU_FAILTEST, BW_GPU_DEBUG: failure switch, debug layer (0.2.5 item 25, test aids)
    src/gpu/bw_vfx.c          # Remastered visual suite (D-016): settings, render scale, helpers
    src/gpu/bw_vfx_post.c     # Remastered visual suite: the frame through its effects to the screen
    src/gpu/bw_hdr.c          # a screen in HDR mode, every edition: scRGB, BT.2408 mapping, the EDID (0.2.5 item 97)
    src/gpu/bw_texrestore.c   # Remastered texture restoration (T1, VFX_TEX_RESTORE)
    src/gpu/bw_pscache.c      # persistent pixel-shader cache (T1, every edition)
    src/gpu/bw_gfxcap.c       # BW_GFXCAP: frame capture and its replay (test aid, lane gfx-cap)
    src/gpu/core/bwg_core.c   # the renderer's API-neutral core (docs/gfx/arch.md 3.2, lane gfx-core): shared basics
    src/gpu/core/bwg_pace.c   #   the present's pacing, the screen wait, the letterbox
    src/gpu/core/bwg_gate.c   #   the latency mode's shared parts: the GPU's progress and time, the horizon (gfx-lat11)
    src/gpu/core/bwg_state.c  #   NV2A states to blend, depth, raster and sampler descriptions
    src/gpu/core/bwg_surf.c   #   the guest surfaces and depth buffers by address
    src/gpu/core/bwg_tex.c    #   the texture cache policy
    src/gpu/core/bwg_draw.c   #   the per-draw preparation without API calls
    src/gpu/bw_gfxcls.c       # BW_GFXCAP_CLASSES: the replay's region masks (test aid, lane gfx-capef)
    src/gpu/bw_gfxcap_draws.c # BW_GFXCAP_DRAWS: the capture's draws decoder in C (test aid, lane gfx-capef)
    src/gpu/core/bwg_ideas.c  #   the adopted ideas' shared parts (arch.md 3.14, lane gfx-ideas): I5's feedback counter
    src/gpu/bw_edgeprobe.c    # BW_GPU_EDGEPROBE: the edge-pixel residual's probe (test aid, lane gfx-ideas)
    src/gpu/core/bwg_latch.c  #   I1, the CPU-write latch: locks into the GPU's copy (lane gfx-regen)
    src/gpu/bw_vshpre.c       # the game's vertex programs compiled at load time (lane gfx-regen)
    # Add game-specific sources here, e.g.:
    # src/asset_loader.c
    # src/custom_renderer.c
)

# The recomp pipeline also generates a dispatch header and function declarations.
# These are included by the generated .c files:
#   src/recomp/gen/recomp_funcs.h    - extern declarations for all recompiled functions
#   src/recomp/recomp_dispatch.h     - binary search dispatch table
#   src/recomp/recomp_types.h        - register model, ICALL macros, MEM helpers

add_executable(${PROJECT_NAME} WIN32
    ${GAME_SOURCES}
    ${RECOMP_GEN_SOURCES}
)

# The native ocean (docs/day1/P2a.md): tools/bw_ocean_xlate.py translates the
# water's render-vertex functions from the generated C into
# src/recomp/gen/bw_ocean_native.inc (git-ignored with the rest of the
# generated C), which src/bw_ocean_native.c includes. Rebuilt whenever the
# generated C or the translator changes.
# The translator rewrites the .inc only when it changes, so its output can stay
# older than its inputs; a stamp it touches on every run is what tells the build
# it's up to date (0.2.5 item 8: it used to rerun on every build).
set(BW_OCEAN_NATIVE_INC ${BW_GEN_DIR}/bw_ocean_native.inc)
set(BW_OCEAN_NATIVE_STAMP ${CMAKE_CURRENT_BINARY_DIR}/bw_ocean_native.stamp)
add_custom_command(
    OUTPUT ${BW_OCEAN_NATIVE_STAMP}
    BYPRODUCTS ${BW_OCEAN_NATIVE_INC}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/bw_ocean_xlate.py
            --gen ${BW_GEN_DIR} --out ${BW_OCEAN_NATIVE_INC} --stamp ${BW_OCEAN_NATIVE_STAMP}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tools/bw_ocean_xlate.py ${RECOMP_GEN_SOURCES}
    COMMENT "native ocean: translating the water's render-vertex functions"
    VERBATIM)
target_sources(${PROJECT_NAME} PRIVATE ${BW_OCEAN_NATIVE_STAMP} ${BW_OCEAN_NATIVE_INC})
set_source_files_properties(${BW_OCEAN_NATIVE_INC} PROPERTIES HEADER_FILE_ONLY TRUE GENERATED TRUE)
set_source_files_properties(src/bw_ocean_native.c PROPERTIES OBJECT_DEPENDS ${BW_OCEAN_NATIVE_INC})

# The icon (0.2.5 item 14, docs/025/STYLE.md): tools/bw_icon.py draws it into
# the build folder (no image file is kept in the repository), src/bloodwake.rc
# embeds it in the exe, and a copy goes beside the exe for the launcher's
# window. Redrawn only when the script changes.
if(MSVC)
    set(BW_ICON_DIR ${CMAKE_BINARY_DIR}/bw_icon)
    set(BW_ICON ${BW_ICON_DIR}/bloodwake.ico)
    add_custom_command(
        OUTPUT ${BW_ICON}
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/bw_icon.py ico --out ${BW_ICON}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tools/bw_icon.py
        COMMENT "the icon: drawing it (tools/bw_icon.py)"
        VERBATIM)
    target_sources(${PROJECT_NAME} PRIVATE src/bloodwake.rc ${BW_ICON})
    # rc finds bloodwake.ico there (on the .rc alone: the C files' flags don't change)
    set_source_files_properties(src/bloodwake.rc PROPERTIES
        OBJECT_DEPENDS ${BW_ICON} INCLUDE_DIRECTORIES ${BW_ICON_DIR})
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${BW_ICON} $<TARGET_FILE_DIR:${PROJECT_NAME}>/bloodwake.ico
        VERBATIM)
    # The application manifest (0.2.5 item 33): per-monitor DPI aware, so
    # Windows display scaling isn't stretched. A .manifest source is merged
    # with the linker's own manifest (an RT_MANIFEST in the .rc would clash).
    target_sources(${PROJECT_NAME} PRIVATE src/bloodwake.manifest)
endif()

# Link against the xboxrecomp umbrella target (all 6 runtime libraries)
target_link_libraries(${PROJECT_NAME} PRIVATE xboxrecomp)

# Include paths for generated recomp headers
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/recomp
    ${BW_GEN_DIR}                   # src/bw_ocean_native.c: recomp_types.h, bw_ocean_native.inc
    ${XBOXRECOMP_DIR}/include
)

# Windows SDK libraries needed at link time
target_link_libraries(${PROJECT_NAME} PRIVATE
    d3d11
    dxgi
    dxguid
    d3dcompiler     # src/gpu/bw_d3d11.c compiles its shaders at start-up
    $<IF:$<BOOL:${MINGW}>,xinput1_4,xinput>   # see src/input/CMakeLists.txt
    winmm
    shell32         # src/bw_edition.c: SHGetFolderPathA, SHCreateDirectoryExA
    # Lets the fault handler resolve the host RIP back to the generated
    # function's symbol, i.e. to the guest function that faulted.
    dbghelp
)

# MSVC-specific: large address aware, increase section limit for big generated files
if(MSVC)
    target_link_options(${PROJECT_NAME} PRIVATE
        /LARGEADDRESSAWARE
    )
    # The generated code files can be very large. Increase object file section limit.
    target_compile_options(${PROJECT_NAME} PRIVATE /bigobj)

    # Every call to an undeclared function is an error in our own code too (a community
    # build failed: fabsf called without <math.h> links on some toolsets, not others).
    target_compile_options(${PROJECT_NAME} PRIVATE /we4013)

    # Blood Wake (D-010, TOOLKIT_ISSUES TK-3): Ghidra can name a guest function after
    # one of the CRT's legacy POSIX names (flushall, itoa, strlwr...), which the UCRT
    # headers also declare, and the build stops on C2371. The generated code never
    # calls those, so it doesn't get them declared. /we4013 makes any call that would
    # have relied on one a hard error, instead of an implicit int that truncates a
    # pointer on x64.
    set_source_files_properties(${RECOMP_GEN_SOURCES} PROPERTIES
        COMPILE_DEFINITIONS "_CRT_DECLARE_NONSTDC_NAMES=0"
        COMPILE_OPTIONS "/we4013")
endif()

# Blood Wake (M0.7, FIRST_RUN_PLAN): record every indirect-branch target the
# game reaches, for the toolkit's feedback loop (docs/technical/indirect-calls.md).
# Both sides need the define: the generated code stores into the array, and the
# toolkit's kernel library (icall_feedback.c) defines it and writes the dump.
# Set from here so the toolkit's files stay untouched (D-010). Costs one byte
# store per indirect call; turn it off for a release-style build.
# Blood Wake (D-010): carried patches. A bug inside one of the toolkit's runtime
# libraries is fixed in a patched copy of that one source file under
# src/toolkit_patches/, compiled in place of the toolkit's copy. The toolkit's
# checkout stays untouched. Each entry checks the SHA-256 of the original it was
# made from, so a toolkit upgrade stops the build until the patch is re-checked
# (dropped if upstream fixed it, remade if not). The .diff beside each copy is
# the exact change.
function(bw_carried_patch target toolkit_rel_src original_sha256)
    set(_orig "${XBOXRECOMP_DIR}/${toolkit_rel_src}")
    # Hashed with LF line endings, so a git checkout (CRLF on Windows) and
    # GitHub's archive of the same commit (LF; tools/build_from_disc.py) agree.
    file(READ "${_orig}" _text)
    string(REPLACE "\r\n" "\n" _text "${_text}")
    string(SHA256 _sha "${_text}")
    if(NOT _sha STREQUAL original_sha256)
        message(FATAL_ERROR "Carried patch for ${toolkit_rel_src}: the toolkit's copy changed "
                            "(sha256 ${_sha}). Re-check it against docs/TOOLKIT_ISSUES.md.")
    endif()
    get_filename_component(_name "${toolkit_rel_src}" NAME)
    get_filename_component(_dir "${toolkit_rel_src}" DIRECTORY)
    string(REGEX REPLACE "^src/" "" _sub "${_dir}")
    set(_patched "${CMAKE_CURRENT_SOURCE_DIR}/src/toolkit_patches/${_sub}/${_name}")
    get_target_property(_srcs ${target} SOURCES)
    list(FIND _srcs "${_name}" _i)
    if(_i LESS 0)
        message(FATAL_ERROR "Carried patch: ${_name} is not a source of ${target}")
    endif()
    list(REMOVE_AT _srcs ${_i})
    list(APPEND _srcs "${_patched}")
    set_target_properties(${target} PROPERTIES SOURCES "${_srcs}")
    message(STATUS "Carried patch: ${target} builds ${_patched}")
endfunction()

# TK-13: OHCI resolved heap buffers named by MmGetPhysicalAddress into the
# contiguous window, so the pad's input reports never reached the game.
bw_carried_patch(xbox_usb "src/usb/ohci.c"
                 "df8e1c265db219b58c4d3e632d694d9da5fc81ff99d636dc2cb176e03ab02ea9")

# Test input: live pad lines starting with '!' take effect at once instead of
# queueing behind a 200 ms release (tools/bw_autopilot2.py). Not a toolkit bug;
# a test-harness extension, carried like the fixes.
bw_carried_patch(xbox_usb "src/usb/usb_gamepad.c"
                 "36a29f6c38739bf1b8aa31f299fef483f1d0cc6fe03ef1e65c46de80ae512424")

# 0.2.5 (lane C3, docs/025/C3.md): players take the connected controllers in
# order whatever their Windows slot, rumble stops on focus loss and on demand,
# optional stick deadzones, and a BW_TEST_XINPUT test switch (item 73, 74, 76).
bw_carried_patch(xbox_input "src/input/xinput_device.c"
                 "dba2708b822e5269748558e0d55898082d7478197324b52a56bd9224409ec05f")
# 0.2.5 (lane C3): the player's own key bindings, steering ramp and strength
# (settings.ini [keys], read by src/bw_keys.c; item 76).
bw_carried_patch(xbox_input "src/input/keyboard.c"
                 "cf4cc96718246341823e40e2ae276e49f1ec2fc550f2a115cabec81c1a1d1424")

# TK-14: the APU's sequencer sample-limit tick (ISTS bit 3) fires every 2048
# samples; Blood Wake's DirectSound answers each one with a hardware restart,
# leaving periods of silence in the audio ~20 times a second.
bw_carried_patch(xbox_apu "src/apu/apu_core.c"
                 "c15d0b1a599f220d662d85f3a45f141cd128acecb383fba6fa03ce9479331b46")

# TK-16: the stubbed DSP's mixdown (every mixbin at unity, even bins left, odd
# bins right). Diagnostics first (BW_APU_BINSTAT).
bw_carried_patch(xbox_apu "src/apu/apu_dsp.c"
                 "cff1bcb52ab2cad2019e803cb5ad96c5e47e52bce7a0f91731b71bcd33ff8d86")

# TK-17: one physical number can name a heap page and a window page; the APU
# read the heap copy of the music stream's window buffer (noise). The kernel
# records each translation's kind, and the APU tags posted buffer pages and
# stream segments with it.
bw_carried_patch(xbox_kernel "src/kernel/kernel_memory.c"
                 "caf09aa153563e0b8ddf55be27eb31beca450e3d6906926375d1b0b4f47e87d0")
bw_carried_patch(xbox_apu "src/apu/apu_vp.c"
                 "f3690aaf836eb5584b0c8e100bd1650c051c7c8d92ce25ad7a5f35a76f60b894")

# Remastered surround (D-016, docs/day1/D2.md part 3): a 6-channel (5.1)
# source voice when the device has 6+ channels; stereo exactly as before
# otherwise. Not a toolkit bug; an output feature, carried like the fixes.
bw_carried_patch(xbox_apu "src/apu/apu_xaudio2.c"
                 "2ce2b436500784ac158fb2270190fd78f748c0b0ce9ca685f1d9e991f05f46c1")

# D-003 B: the executor hands transformed batches to the GPU back end with
# everything its CPU combiner path reads (src/toolkit_patches/kernel/
# bw_backend_ext.h), so the back end can draw the register combiners.
bw_carried_patch(xbox_kernel "src/kernel/nv2a_pb_exec.c"
                 "407369da9e1447c1a5dea9af343fd66877cb31e85a86cda30768f47b2625d573")

# Point sprites: the vertex-program interpreter hands out the program's point
# size (oPts), which it computed and dropped, so the executor can draw points
# as sprites (both renderers dropped every point; BW_GPU_AUDIT, 2026-10-27).
bw_carried_patch(xbox_kernel "src/kernel/nv2a_vsh_interp.c"
                 "d1e1085b5ca3ed7afad5500e7e6c06b0f918545321f8b85e0a73b9e11933eb35")

# The NV2A thread's loop ended each pass with Sleep(0), holding a whole core
# between frames; it now naps when idle and the KickOff replacement wakes it
# (src/bw_wait.c, bw_ack_idle / bw_ack_kick; BW_ACK_NAP=0 restores the spin).
bw_carried_patch(xbox_kernel "src/kernel/xbox_memory_layout.c"
                 "9b62ad9fc0f1c6061faea61f71d2a58ee01d27b3e487e01601e8d44a251cea3d")

# TK-38: the segment scan lost commands at every pushbuffer ring wrap (it
# guessed the ring's ends from the PUTs seen) and cut methods whose parameters
# ran past PUT. The ring walk follows the JUMP and keeps the split method
# (BW_PB_RINGWALK=0 restores the old scan; BW_PB_SEGSTAT=1 counts).
bw_carried_patch(xbox_kernel "src/kernel/nv2a_pb_scan.c"
                 "a365519c4c96b5e75a9a2e57eed74ecd0521129eae42fba3ce24c5176fccec90")

# D-012: the GP/EP DSPs on xemu's DSP56300 interpreter (vendored in
# src/xemu_dsp, xemu v0.8.139, GPL-2.0-or-later), switched on with
# BW_APU_DSP=xemu; without it the stub's fold-down runs as before.
add_subdirectory(src/xemu_dsp)
target_sources(xbox_apu PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/toolkit_patches/apu/bw_apu_xdsp.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/toolkit_patches/apu/bw_apu_wasapi.c    # lat-a: the WASAPI output (Remastered)
    ${CMAKE_CURRENT_SOURCE_DIR}/src/toolkit_patches/apu/bw_apu_out.c       # au-out: MMCSS, the rebuild worker, the device line
    ${CMAKE_CURRENT_SOURCE_DIR}/src/toolkit_patches/apu/bw_apu_hot.c       # au-hot: BW_APU_HOTSTAT (a test aid, inert when off)
    ${CMAKE_CURRENT_SOURCE_DIR}/src/toolkit_patches/apu/bw_apu_gain.c      # au-gain: the clean-level output gain (GAIN-1), BW_AUDIO_PEAKLOG
    # BW_DSP_TRACE=<dir>: the DSP trace recorder (au-trace; a test aid, inert
    # when unset; docs/audio/trace_format.md).
    ${CMAKE_CURRENT_SOURCE_DIR}/src/toolkit_patches/apu/bw_dsp_trace.c)
target_link_libraries(xbox_apu PUBLIC bw_xemu_dsp)
# TK-36: PsCreateSystemThreadEx ignored CreateSuspended, so Bink's workers ran
# before their mutex and exit event existed (BW_THREAD_SUSPEND=0 restores).
# Also the D3 wait/handle diagnostics ([BWD3] lines; BW_D3_DIAG=0 silences).
bw_carried_patch(xbox_kernel "src/kernel/kernel_bridge.c"
                 "a065bdd6d96791f546e849d3d7b752e17a93a002f61234e36d1388a98b2de3b0")

# 0.2.5 item 53: RECOMP_SAVE_DIR was decoded as UTF-8 from the CRT's ANSI copy
# of the environment, so a save folder named with letters outside plain ASCII
# fell back to the default one. Read wide from the process block now.
bw_carried_patch(xbox_kernel "src/kernel/kernel_path.c"
                 "cf6828ef163b3804de9877ce9db2ca18b46fd50c7601cfefa82c63710cdd30a9")

# One window for a play session (src/bw_host.c): with BW_PARENT_HWND set, the
# frame window is a child of the launcher's host window, so the game's
# self-reboots no longer open a new window each.
bw_carried_patch(xbox_video "src/video/fb_present.c"
                 "f0a3f553ccf4afc2e1bcfc1020030bfe76a1fc584caefc3d2367ba87f8ed9782")

option(BW_ICALL_FEEDBACK "Record indirect-call targets (icall_targets.dump)" ON)
if(BW_ICALL_FEEDBACK)
    target_compile_definitions(${PROJECT_NAME} PRIVATE RECOMP_ICALL_FEEDBACK)
    target_compile_definitions(xbox_kernel PRIVATE RECOMP_ICALL_FEEDBACK)
endif()

# ── Game-specific additions ─────────────────────────────────────
#
# Add any game-specific libraries, loaders, or custom renderers below.
# Examples:
#
# # Custom texture loader for your game's format
# target_sources(${PROJECT_NAME} PRIVATE src/texture_loader.c)
#
# # If your game uses RenderWare, you may need a RW bridge layer
# target_sources(${PROJECT_NAME} PRIVATE src/rw_bridge.c)
#
# # Additional compile definitions for debugging
# target_compile_definitions(${PROJECT_NAME} PRIVATE
#     ENABLE_ICALL_TRACE=1
#     VERBOSE_KERNEL_LOG=1
# )
