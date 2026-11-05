/* SPDX-License-Identifier: GPL-2.0-or-later (with xemu's DSP it hosts; see NOTICE.md) */
/*
 * bw_dsp_trace_fmt.c - the trace chunk codec (see bw_dsp_trace_fmt.h): the
 * Windows compression API's XPRESS Huffman in buffer mode (Cabinet.dll, in
 * every Windows since 8), with a stored fallback when it doesn't help.
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <compressapi.h>
#include <stdlib.h>
#include "bw_dsp_trace_fmt.h"

struct BwDtrCodec {
    COMPRESSOR_HANDLE comp;
    DECOMPRESSOR_HANDLE decomp;
};

BwDtrCodec *bw_dtr_codec_new(void)
{
    BwDtrCodec *c = (BwDtrCodec *)calloc(1, sizeof *c);
    if (!c)
        return NULL;
    if (!CreateCompressor(COMPRESS_ALGORITHM_XPRESS_HUFF, NULL, &c->comp))
        c->comp = NULL;
    if (!CreateDecompressor(COMPRESS_ALGORITHM_XPRESS_HUFF, NULL, &c->decomp))
        c->decomp = NULL;
    return c;
}

void bw_dtr_codec_free(BwDtrCodec *c)
{
    if (!c)
        return;
    if (c->comp)
        CloseCompressor(c->comp);
    if (c->decomp)
        CloseDecompressor(c->decomp);
    free(c);
}

size_t bw_dtr_bound(size_t len)
{
    return len + len / 8 + 4096;
}

size_t bw_dtr_compress(BwDtrCodec *c, const uint8_t *in, size_t len, uint8_t *out, size_t cap,
                       uint32_t *codec)
{
    SIZE_T got = 0;
    if (c && c->comp && Compress(c->comp, in, len, out, cap, &got) && got > 0 && got < len) {
        *codec = BW_DTR_CODEC_XPRESS_HUFF;
        return (size_t)got;
    }
    if (len > cap)
        return 0;
    memcpy(out, in, len);
    *codec = BW_DTR_CODEC_STORED;
    return len;
}

int bw_dtr_decompress(BwDtrCodec *c, uint32_t codec, const uint8_t *in, size_t len,
                      uint8_t *out, size_t raw_len)
{
    SIZE_T got = 0;
    if (codec == BW_DTR_CODEC_STORED) {
        if (len != raw_len)
            return -1;
        memcpy(out, in, len);
        return 0;
    }
    if (codec != BW_DTR_CODEC_XPRESS_HUFF || !c || !c->decomp)
        return -1;
    if (!Decompress(c->decomp, in, len, out, raw_len, &got) || got != raw_len)
        return -1;
    return 0;
}
