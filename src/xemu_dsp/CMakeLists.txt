# xemu's MCPX DSP interpreter (v0.8.139, hw/xbox/mcpx/apu/dsp, GPL-2.0-or-later),
# vendored for D-012. Built as its own library with QEMU stand-in headers in
# shim/; the carried apu_dsp.c reaches it only through bw_dsp.h. The same
# library builds the offline DSP replayer (tools/dsp_replay).
add_library(bw_xemu_dsp STATIC
    bw_dsp.c
    bw_dsp_fast.c
    bw_dsp_trace_fmt.c
    dsp.c
    dsp_c.c
    dsp_dma.c
    interp/dsp_cpu.c
    interp/debug.c
)
target_include_directories(bw_xemu_dsp PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/shim
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/interp)
target_include_directories(bw_xemu_dsp INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
# The trace codec (bw_dsp_trace_fmt.c): the Windows compression API.
target_link_libraries(bw_xemu_dsp PUBLIC Cabinet)
if(MSVC)
    target_compile_options(bw_xemu_dsp PRIVATE /W2 /wd4244 /wd4267 /wd4146 /wd4018)
    # The interpreter is the audio thread's main cost (D-012): no stack
    # cookies, aggressive inlining, speed over size.
    target_compile_options(bw_xemu_dsp PRIVATE $<$<CONFIG:Release>:/GS- /Ob3 /Oi /Ot>)
endif()
