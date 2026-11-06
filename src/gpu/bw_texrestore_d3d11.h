/* bw_texrestore_d3d11.h - Remastered texture restoration (agent T1,
 * VFX_TEX_RESTORE): its D3D11 side. Split from bw_texrestore.h (GC-3), so
 * the shared core (src/gpu/core) compiles without d3d11.h. */
#ifndef BW_TEXRESTORE_D3D11_H
#define BW_TEXRESTORE_D3D11_H

#include "bw_texrestore.h"
#include <d3d11.h>

/* A slot's restored chain as D3D11 objects (read; a slot is written with
 * s->api[0] = texture, s->api[1] = its view). */
#define BW_TR_TEX(s) ((ID3D11Texture2D *)(s)->api[0])
#define BW_TR_SRV(s) ((ID3D11ShaderResourceView *)(s)->api[1])

/* Once per frame on the device thread (the flip): finish the textures the
 * worker has ready, within BW_TEXRESTORE_FRAME_MS (0.3 ms; the first texture always). Changes pipeline state;
 * returns 1 if it drew, whether or not each texture then came out (0.2.5
 * item 47: a failure after the enlargement drew used to return 0), and the
 * caller then rebinds its targets. */
int  bw_tr_pump(ID3D11Device *dev, ID3D11DeviceContext *ctx);

/* Menu and title photos (kind 1, BW_TEXRESTORE_MENUS: the JPEG screens) and
 * movie frames (kind 2, BW_TEXRESTORE_MOVIES, item 11): the same deblock on
 * the `grid` (8: JPEG and Bink blocks), on the GPU, from src (w x h) into a
 * new B8G8R8A8 texture returned with its view; 0 if it couldn't. Device
 * thread; changes pipeline state (the caller rebinds its targets). */
int  bw_tr_deblock_gpu(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11ShaderResourceView *src,
                       uint32_t w, uint32_t h, int grid, int kind, ID3D11Texture2D **out_tex,
                       ID3D11ShaderResourceView **out_srv);

#endif
