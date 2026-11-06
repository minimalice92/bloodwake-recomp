/* bw_vfx_d3d11.h - Remastered's visual-enhancement suite (D-016, agent D9):
 * the GPU helpers the D3D11 back end (bw_d3d11.c) calls at a few marked
 * hooks. Split from bw_vfx.h (GC-3), so the shared core (src/gpu/core),
 * which reads only the settings, compiles without d3d11.h. */
#ifndef BW_VFX_D3D11_H
#define BW_VFX_D3D11_H

#include "bw_vfx.h"
#include <d3d11.h>
#include <dxgi1_4.h>    /* IDXGISwapChain3: HDR colour space */

/* ---- GPU helpers ----------------------------------------------------------
 * All on the back end's device thread, with its device and context. They
 * change pipeline state; the back end rebinds its own afterwards. */
int  bw_vfx_gpu_init(ID3D11Device *dev, ID3D11DeviceContext *ctx);
/* Draw src (sw x sh texels) into dst (dw x dh pixels): mode 0 bilinear
 * stretch, 1 box average (downsampling by any factor). */
void bw_vfx_blit(ID3D11ShaderResourceView *src, UINT sw, UINT sh,
                 ID3D11RenderTargetView *dst, UINT dw, UINT dh, int mode);
/* Lane gfx-regen (I1): src stretched bilinear over the rectangle vp of dst,
 * as the seed draws a whole surface (a CPU write's rows at the render scale). */
void bw_vfx_blit_vp(ID3D11ShaderResourceView *src, UINT sw, UINT sh,
                    ID3D11RenderTargetView *dst, const D3D11_VIEWPORT *vp);

/* ---- the frame ------------------------------------------------------------
 * The finished frame (src, sw x sh physical pixels) through the enabled
 * effects into `dst` (the swap chain's view, vp its letterbox rectangle), or
 * with dst NULL into an off-screen output of the output size (BW_GPU_PRESENT=0
 * runs). Writes the output shots. Returns 1 if it drew. */
int  bw_vfx_present(ID3D11ShaderResourceView *src, UINT sw, UINT sh,
                    ID3D11RenderTargetView *dst, const D3D11_VIEWPORT *vp);
/* The scene before the HUD for the next bw_vfx_present (NULL: none this
 * frame). Bloom reads it, and OLED care dims only where the frame differs. */
void bw_vfx_set_scene(ID3D11ShaderResourceView *scene);

/* The title's DAC gamma ramp as D10's 256x1 lookup (bw_d3d11.c gamma_lut:
 * the fade-in at every mission start, the brightness slider), applied at the
 * end of the chain; NULL for none. Set before each bw_vfx_present. */
void bw_vfx_set_gamma_lut(ID3D11ShaderResourceView *lut);

/* The suite's note when a swap chain is made (its format and colour space
 * are bw_hdr.c's, every edition). */
void bw_vfx_swap_created(IDXGISwapChain1 *swap);

/* Ambient occlusion (VFX_AO), per view before its HUD: depth (D24 as
 * R24_UNORM_X8, multisampled or not) of the target, the view rectangle,
 * the target's colour view (multiplied by the occlusion), the near and far
 * planes and the view's focal length in pixels. */
void bw_vfx_ao_apply(ID3D11ShaderResourceView *depth, int depth_ms, UINT w, UINT h,
                     ID3D11RenderTargetView *target, const D3D11_RECT *rect,
                     float zn, float zf, float focal);

/* A linear, clamped sampler (the text filter binds it at s4). */
ID3D11SamplerState *bw_vfx_linear_sampler(void);

#endif
