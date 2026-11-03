/* bw_gpu_failtest.h - the GPU failure switch and the D3D11 debug layer
 * (0.2.5 item 25, docs/025/CONTRACTS.md contract 5). Test aids: every
 * edition, off by default, never in a preset or the launcher's table. With
 * neither switch set, nothing here changes a code path.
 *
 * BW_GPU_FAILTEST=<kind>:<n>[,<kind>:<n>...] makes the n-th operation of
 * that kind fail (n = 1 is the first); <kind>:<n>+ fails the n-th and every
 * later one. The kinds:
 *   device      D3D11CreateDevice (reports DXGI_ERROR_UNSUPPORTED)
 *   resource    any texture, buffer or view create at a hooked site; every
 *               surface, depth and texture call counts as a resource too
 *   surface     a colour render target (bw_d3d11.c create_surf*)
 *   depth       a depth buffer (get_depth)
 *   texture     a game texture or cube texture (get_texture_*)
 *   post        a post-processing target's view (bw_vfx_post.c rt_ensure)
 *   texrestore  a texture-restore view (bw_texrestore.c)
 *   vfxshader   a visual-suite pixel shader (bw_vfx.c)
 *   present     the next Present reports DXGI_ERROR_DEVICE_REMOVED
 *   swapchain   a swap chain create attempt (E_ACCESSDENIED) or a ResizeBuffers
 *               (E_OUTOFMEMORY) fails (item 29; added to contract 5, call B1-5)
 * Each failure made on request is logged ("[GPU-FAILTEST] ..."), once per
 * rule (a rule with + logs its first three and then counts).
 *
 * BW_GPU_DEBUG=1 creates the device with D3D11_CREATE_DEVICE_DEBUG and
 * copies the debug layer's corruption, error and warning messages into the
 * log at every flip ("[GPU-DEBUG] ..."); BW_GPU_DEBUG=2 keeps its info
 * messages too. Repeats of one message are counted rather than printed.
 * Without the Windows optional feature "Graphics Tools" the layer is not
 * there: the device is then made without it, with one log line. */
#ifndef BW_GPU_FAILTEST_H
#define BW_GPU_FAILTEST_H

/* 1 = make this operation fail now (and log it); 0 = carry on. */
int bw_gpu_failtest(const char *kind);

/* For one-line call sites: hr = BW_GPU_FT("surface", E_OUTOFMEMORY, Create(...));
 * the create is not made when the switch fails it. */
#define BW_GPU_FT(kind, fail_hr, call) (bw_gpu_failtest(kind) ? (fail_hr) : (call))

/* BW_GPU_DEBUG: 0 off, 1 errors and warnings, 2 everything. */
int bw_gpu_debug_level(void);

/* After a device made with the debug flag: take its info queue. */
struct ID3D11Device;
void bw_gpu_debug_attach(struct ID3D11Device *dev);

/* Copy the stored debug messages into the log (cheap no-op when off). */
void bw_gpu_debug_drain(void);

#endif
