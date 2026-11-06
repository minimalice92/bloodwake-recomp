/* bw_vfx_int.h - shared between bw_vfx.c and bw_vfx_post.c only. */
#ifndef BW_VFX_INT_H
#define BW_VFX_INT_H

#include "bw_vfx_d3d11.h"

extern ID3D11Device *g_vfx_dev;
extern ID3D11DeviceContext *g_vfx_ctx;
/* I3 (BW_GPU_DELTA, bw_d3d11.c): bumped by anything that sets pipeline state
 * outside the back end's draw paths, so their bindings shadow is forgotten */
extern unsigned g_bw_ctx_epoch;

ID3D11PixelShader *bw_vfx_compile_ps(const char *src, size_t len, const char *entry);
ID3D11Buffer *bw_vfx_make_cb(UINT bytes);
void bw_vfx_update_cb(ID3D11Buffer *b, const void *data, UINT bytes);
ID3D11SamplerState *bw_vfx_point_sampler(void);
/* As bw_vfx_fullscreen, with a blend state (NULL: none). */
void bw_vfx_fullscreen_bs(ID3D11RenderTargetView *dst, const D3D11_VIEWPORT *vp,
                          ID3D11PixelShader *ps, ID3D11Buffer *cb,
                          ID3D11ShaderResourceView *const *srv, UINT nsrv, ID3D11BlendState *bs);
/* One full-screen triangle into dst with ps; samplers s0 linear, s1 point. */
void bw_vfx_fullscreen(ID3D11RenderTargetView *dst, const D3D11_VIEWPORT *vp,
                       ID3D11PixelShader *ps, ID3D11Buffer *cb,
                       ID3D11ShaderResourceView *const *srv, UINT nsrv);

#endif
