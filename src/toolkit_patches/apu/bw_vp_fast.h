/* bw_vp_fast.h - lane au-vp (docs/audio/vp.md): the voice processor's two
 * hot loops made faster, bit for bit the same. Blood Wake file, included by
 * the carried apu_vp.c (after the toolkit's apu_state.h) and by the offline
 * check tools/vp_fast_check, so both run this very code.
 *
 * The rule for both: the same float operations, on the same values, in the
 * same order, for every output. Only the bookkeeping around them changes,
 * and several outputs' chains run side by side in SSE lanes. SSE2 only (any
 * x64 CPU); no FMA can appear (the VP is built without /arch:AVX2, so the
 * compiler can't contract a multiply and an add, and intrinsics are emitted
 * as written).
 *
 * 1. The HRTF filter (the toolkit's hrtf_filter_process, xemu's hrtf.h):
 *    - lanes: 4 consecutive output samples of one ear at once; each lane is
 *      that sample's 31-tap sum, taps 0 to 30 in order, acc = acc + c*s;
 *    - the history copied once a frame into a straight buffer, so a tap is
 *      an index, not a modulo per tap (the circular buffer is written back
 *      exactly as the filter leaves it);
 *    - the per-sample smoothing (cur += 0.01 (tar - cur)) run 4 samples at a
 *      time over the taps, then turned into per-sample columns; and skipped
 *      for the whole frame when one step leaves every coefficient and the ITD
 *      unchanged, bit for bit (a fixed point: each later step gives the same
 *      bits again; -0, which a step turns into +0, is not a fixed point, so
 *      it takes the normal path);
 *    - the targets read once a frame (the game writes them without a lock;
 *      au-hot's proof reads them the same way, call AH-5).
 * 2. The windowed-sinc resampler (Remastered, apu_vp.c): 4 output samples at
 *    once, each lane its own taps lo..hi in order, the weight computed with
 *    the scalar's double and float operations, acc0/acc1/wsum as the scalar.
 *    The scalar loop lives here too (bw_sinc_out_scalar), unchanged, so the
 *    offline check and the in-game proof compare the two directly.
 */
#ifndef BW_VP_FAST_H
#define BW_VP_FAST_H

#include <emmintrin.h>
#include <string.h>
#include <math.h>
#include <limits.h>

#if HRTF_NUM_TAPS != 31 || HRTF_BUFLEN != 73 || HRTF_SAMPLES_PER_FRAME != 32
#error "bw_vp_fast.h: written for the toolkit's HRTF filter (31 taps, 73 of history, 32 samples a frame)"
#endif

/* ---- 1. the HRTF filter ---------------------------------------------------- */

#define BW_HF_N HRTF_SAMPLES_PER_FRAME
#define BW_HF_K HRTF_NUM_TAPS
#define BW_HF_L HRTF_BUFLEN

static inline __m128 bw_hf_sel(__m128 m, __m128 a, __m128 b)   /* m ? a : b, bitwise */
{
    return _mm_or_ps(_mm_and_ps(m, a), _mm_andnot_ps(m, b));
}

/* What the frame did, for the proof aid's counts: 1 the fixed point (no
 * smoothing), 0 the moving path; blocks that needed the lane gather (the
 * ITD's whole sample changing within 4 samples). */
typedef struct { int fixed, gather_blocks; } BwHfInfo;

static inline void bw_hrtf_fast(HrtfFilter *f, float in[][2], float out[][2], BwHfInfo *info)
{
    __declspec(align(16)) float cpad[2][32], tpad[2][32];
    __declspec(align(16)) float dfv[2][BW_HF_N];
    __declspec(align(16)) float lin[2][BW_HF_L + BW_HF_N + 4];
    __declspec(align(16)) __m128 colf[2][32];        /* the fixed point's columns */
    int div_[2][BW_HF_N];
    const __m128 alpha = _mm_set1_ps(HRTF_PARAM_SMOOTH_ALPHA), zero = _mm_setzero_ps(), one = _mm_set1_ps(1.0f);
    __m128 cq[2][8], tq[2][8];
    float itd_t, itd;
    int fixed = 1, ch, q, n, k, blk, bp, gathers = 0;

    /* the targets, once (a snapshot), and the current values */
    for (ch = 0; ch < 2; ch++) {
        memcpy(tpad[ch], f->ch[ch].hrir_coeff_tar, sizeof f->ch[ch].hrir_coeff_tar);
        memcpy(cpad[ch], f->ch[ch].hrir_coeff_cur, sizeof f->ch[ch].hrir_coeff_cur);
        tpad[ch][31] = cpad[ch][31] = 0.0f;              /* a padding lane, never read out */
        for (q = 0; q < 8; q++) {
            tq[ch][q] = _mm_load_ps(&tpad[ch][4 * q]);
            cq[ch][q] = _mm_load_ps(&cpad[ch][4 * q]);
        }
    }
    itd_t = f->itd_tar;
    itd = f->itd_cur;

    /* a fixed point? one step, compared bit for bit */
    for (ch = 0; ch < 2 && fixed; ch++)
        for (q = 0; q < 8; q++) {
            __m128 g = _mm_add_ps(cq[ch][q], _mm_mul_ps(alpha, _mm_sub_ps(tq[ch][q], cq[ch][q])));
            if (_mm_movemask_epi8(_mm_cmpeq_epi32(_mm_castps_si128(g), _mm_castps_si128(cq[ch][q]))) != 0xFFFF) {
                fixed = 0;
                break;
            }
        }
    if (fixed) {
        float gi = hrtf_filter_smooth_param(itd, itd_t);
        uint32_t a, b;
        memcpy(&a, &gi, 4);
        memcpy(&b, &itd, 4);
        if (a != b)
            fixed = 0;
    }

    /* the ITD's path over the frame, and each sample's delay per ear, as the
     * filter computes them */
    for (n = 0; n < BW_HF_N; n++) {
        if (!fixed)
            itd = hrtf_filter_smooth_param(itd, itd_t);
        for (ch = 0; ch < 2; ch++) {
            float d = itd * (ch == 0 ? +1.0f : -1.0f);
            if (d < 0.0f) d = 0.0f;
            int di = (int)d;
            float dfrac = d - di;
            div_[ch][n] = di;
            dfv[ch][n] = dfrac;
        }
    }

    /* the history in time order, then this frame's input after it:
     * lin[j] = buf[(buf_pos + j) % 73], lin[73 + n] = in[n] */
    bp = f->buf_pos;
    for (ch = 0; ch < 2; ch++) {
        const float *b = f->ch[ch].buf;
        memcpy(lin[ch], b + bp, sizeof(float) * (BW_HF_L - bp));
        memcpy(lin[ch] + (BW_HF_L - bp), b, sizeof(float) * bp);
        for (n = 0; n < BW_HF_N; n++)
            lin[ch][BW_HF_L + n] = in[n][ch];
        lin[ch][BW_HF_L + BW_HF_N] = lin[ch][BW_HF_L + BW_HF_N + 1] = 0.0f;
        lin[ch][BW_HF_L + BW_HF_N + 2] = lin[ch][BW_HF_L + BW_HF_N + 3] = 0.0f;
    }
    if (fixed)
        for (ch = 0; ch < 2; ch++)
            for (k = 0; k < BW_HF_K; k++)
                colf[ch][k] = _mm_set1_ps(cpad[ch][k]);

    for (blk = 0; blk < BW_HF_N / 4; blk++) {
        const int n0 = blk * 4;
        __declspec(align(16)) __m128 colm[2][32];
        __m128 (*col)[32] = fixed ? colf : colm;
        __m128 acc[2];
        if (!fixed) {
            /* 4 more smoothing steps, 4 taps at a time, turned into columns:
             * col[ch][k] = that tap's value at samples n0..n0+3 */
            for (ch = 0; ch < 2; ch++)
                for (q = 0; q < 8; q++) {
                    __m128 r0 = _mm_add_ps(cq[ch][q], _mm_mul_ps(alpha, _mm_sub_ps(tq[ch][q], cq[ch][q])));
                    __m128 r1 = _mm_add_ps(r0, _mm_mul_ps(alpha, _mm_sub_ps(tq[ch][q], r0)));
                    __m128 r2 = _mm_add_ps(r1, _mm_mul_ps(alpha, _mm_sub_ps(tq[ch][q], r1)));
                    __m128 r3 = _mm_add_ps(r2, _mm_mul_ps(alpha, _mm_sub_ps(tq[ch][q], r2)));
                    cq[ch][q] = r3;
                    _MM_TRANSPOSE4_PS(r0, r1, r2, r3);
                    colm[ch][4 * q + 0] = r0;
                    colm[ch][4 * q + 1] = r1;
                    colm[ch][4 * q + 2] = r2;
                    colm[ch][4 * q + 3] = r3;
                }
        }
        for (ch = 0; ch < 2; ch++) {
            const int d0 = div_[ch][n0];
            const __m128 df = _mm_load_ps(&dfv[ch][n0]);
            const __m128 nd = _mm_sub_ps(one, df);            /* 1 - dfrac, per lane */
            const __m128 im = _mm_cmpgt_ps(df, zero);         /* dfrac > 0: interpolate */
            const int imm = _mm_movemask_ps(im);
            const float *L = lin[ch];
            __m128 a = zero;
            if (div_[ch][n0 + 1] == d0 && div_[ch][n0 + 2] == d0 && div_[ch][n0 + 3] == d0) {
                /* one whole delay for the block: lane j's tap k is p[j - k] */
                const float *p = L + BW_HF_L + n0 - d0;
                if (!imm) {
                    for (k = 0; k < BW_HF_K; k++)
                        a = _mm_add_ps(a, _mm_mul_ps(col[ch][k], _mm_loadu_ps(p - k)));
                } else if (imm == 0xF) {
                    for (k = 0; k < BW_HF_K; k++) {
                        __m128 s1 = _mm_loadu_ps(p - k), s2 = _mm_loadu_ps(p - k - 1);
                        __m128 s = _mm_add_ps(_mm_mul_ps(s1, nd), _mm_mul_ps(s2, df));
                        a = _mm_add_ps(a, _mm_mul_ps(col[ch][k], s));
                    }
                } else {
                    for (k = 0; k < BW_HF_K; k++) {
                        __m128 s1 = _mm_loadu_ps(p - k), s2 = _mm_loadu_ps(p - k - 1);
                        __m128 s = bw_hf_sel(im, _mm_add_ps(_mm_mul_ps(s1, nd), _mm_mul_ps(s2, df)), s1);
                        a = _mm_add_ps(a, _mm_mul_ps(col[ch][k], s));
                    }
                }
            } else {
                /* the delay's whole part changes inside the block: each lane
                 * its own index */
                const int b0 = BW_HF_L + n0 - d0, b1 = BW_HF_L + n0 + 1 - div_[ch][n0 + 1];
                const int b2 = BW_HF_L + n0 + 2 - div_[ch][n0 + 2], b3 = BW_HF_L + n0 + 3 - div_[ch][n0 + 3];
                gathers++;
                for (k = 0; k < BW_HF_K; k++) {
                    __m128 s1 = _mm_setr_ps(L[b0 - k], L[b1 - k], L[b2 - k], L[b3 - k]);
                    __m128 s2 = _mm_setr_ps(L[b0 - k - 1], L[b1 - k - 1], L[b2 - k - 1], L[b3 - k - 1]);
                    __m128 s = bw_hf_sel(im, _mm_add_ps(_mm_mul_ps(s1, nd), _mm_mul_ps(s2, df)), s1);
                    a = _mm_add_ps(a, _mm_mul_ps(col[ch][k], s));
                }
            }
            acc[ch] = a;
        }
        /* out[n][0], out[n][1] for n0..n0+3 (the input is all in lin already) */
        _mm_storeu_ps(&out[n0][0], _mm_unpacklo_ps(acc[0], acc[1]));
        _mm_storeu_ps(&out[n0 + 2][0], _mm_unpackhi_ps(acc[0], acc[1]));
    }

    /* the state as the filter leaves it */
    if (!fixed) {
        for (ch = 0; ch < 2; ch++) {
            for (q = 0; q < 8; q++)
                _mm_store_ps(&cpad[ch][4 * q], cq[ch][q]);
            memcpy(f->ch[ch].hrir_coeff_cur, cpad[ch], sizeof f->ch[ch].hrir_coeff_cur);
        }
        f->itd_cur = itd;
    }
    for (ch = 0; ch < 2; ch++)
        for (n = 0; n < BW_HF_N; n++)
            f->ch[ch].buf[(bp + n) % BW_HF_L] = lin[ch][BW_HF_L + n];
    f->buf_pos = (bp + BW_HF_N) % BW_HF_L;
    if (info) {
        info->fixed = fixed;
        info->gather_blocks = gathers;
    }
}

/* ---- 2. the windowed-sinc resampler ---------------------------------------- */

#define BW_SINC_ZC 8                   /* zero crossings a side at full band */
#define BW_SINC_RES 512                 /* table points per zero crossing */
#define BW_SINC_MAXHALF 32              /* taps a side at the narrowest */
#define BW_SINC_HIST 256

typedef struct {
    int have;
    int n;                              /* samples in hist */
    double pos;                         /* next output, in source samples from hist[0] */
    float hist[BW_SINC_HIST][2];
} BwSinc;

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* The kernel's table: a Kaiser-windowed sinc (beta 8), BW_SINC_RES points a
 * zero crossing, 0 from BW_SINC_ZC on (moved here from apu_vp.c unchanged) */
static inline void bw_sinc_build_table(float *tab)
{
    int i, k;
    double beta = 8.0, i0b = 1.0, t = 1.0;
    for (k = 1; k < 50; k++) {           /* I0(beta) by its series */
        t *= (beta / 2.0 / k) * (beta / 2.0 / k);
        i0b += t;
    }
    for (i = 0; i <= BW_SINC_ZC * BW_SINC_RES + 1; i++) {
        double x = (double)i / BW_SINC_RES, r = x / BW_SINC_ZC, a, w, s, term;
        if (r >= 1.0) {
            tab[i] = 0.0f;
            continue;
        }
        a = beta * sqrt(1.0 - r * r);
        for (k = 1, w = 1.0, term = 1.0; k < 50; k++) {
            term *= (a / 2.0 / k) * (a / 2.0 / k);
            w += term;
        }
        s = x == 0.0 ? 1.0 : sin(M_PI * x) / (M_PI * x);
        tab[i] = (float)(s * w / i0b);
    }
}

/* the kernel at x zero crossings (x >= 0), from the table */
static inline float bw_sinc_kt(const float *tab, double x)
{
    double fi = x * BW_SINC_RES;
    int i = (int)fi;
    float f = (float)(fi - i);
    if (i >= BW_SINC_ZC * BW_SINC_RES)
        return 0.0f;
    return tab[i] + f * (tab[i + 1] - tab[i]);
}

/* The resampler's output loop as it always was: up to requested_num outputs
 * from s's history, s->pos moved on; returns how many were made. taps, if
 * given, counts the taps taken (au-hot's aid). */
static inline int bw_sinc_out_scalar(BwSinc *s, float samples[][2], int requested_num, double step,
                                     double fc, double half, const float *tab, int *taps)
{
    int out, lo, hi, j;
    for (out = 0; out < requested_num; out++) {
        double p = s->pos;
        float acc0 = 0.0f, acc1 = 0.0f, wsum = 0.0f;
        lo = (int)ceil(p - half);
        hi = (int)floor(p + half);
        if (hi >= s->n)
            break;                       /* the source isn't there yet */
        if (lo < 0) lo = 0;
        if (taps)
            *taps += hi - lo + 1;
        for (j = lo; j <= hi; j++) {
            float w = bw_sinc_kt(tab, fabs(p - j) * fc);
            acc0 += w * s->hist[j][0];
            acc1 += w * s->hist[j][1];
            wsum += w;
        }
        if (wsum != 0.0f) {
            samples[out][0] = acc0 / wsum;
            samples[out][1] = acc1 / wsum;
        } else {
            samples[out][0] = samples[out][1] = 0.0f;
        }
        s->pos += step;
    }
    return out;
}

/* The same, 4 outputs at a time in SSE lanes (lane m: output out + m, its
 * own p, lo, hi), then the scalar for the rest. */
static inline int bw_sinc_out_simd(BwSinc *s, float samples[][2], int requested_num, double step,
                                   double fc, double half, const float *tab, int *taps)
{
    const __m128d absm = _mm_castsi128_pd(_mm_set1_epi64x(0x7FFFFFFFFFFFFFFFLL));
    const __m128d vfc = _mm_set1_pd(fc), vres = _mm_set1_pd((double)BW_SINC_RES);
    const __m128i lim = _mm_set1_epi32(BW_SINC_ZC * BW_SINC_RES);
    const __m128 zero = _mm_setzero_ps();
    int out = 0;
    while (out + 4 <= requested_num) {
        double p[4];
        int lo[4], hi[4], cnt[4], m, t, tmin = INT_MAX, tmax = 0;
        __declspec(align(16)) int ii[4], jj[4];
        __m128d p01, p23;
        __m128i vlo, vhi, vcnt;
        __m128 a01 = zero, a23 = zero, ws = zero;
        p[0] = s->pos;
        for (m = 0; m < 4; m++) {
            if (m)
                p[m] = p[m - 1] + step;  /* the scalar's s->pos += step, in order */
            lo[m] = (int)ceil(p[m] - half);
            hi[m] = (int)floor(p[m] + half);
        }
        if (hi[3] >= s->n)
            break;                       /* the last of the four isn't there yet: the scalar finishes */
        for (m = 0; m < 4; m++) {
            if (lo[m] < 0) lo[m] = 0;
            cnt[m] = hi[m] - lo[m] + 1;
            if (taps)
                *taps += cnt[m];
            if (cnt[m] < tmin) tmin = cnt[m];
            if (cnt[m] > tmax) tmax = cnt[m];
        }
        p01 = _mm_set_pd(p[1], p[0]);
        p23 = _mm_set_pd(p[3], p[2]);
        vlo = _mm_setr_epi32(lo[0], lo[1], lo[2], lo[3]);
        vhi = _mm_setr_epi32(hi[0], hi[1], hi[2], hi[3]);
        vcnt = _mm_setr_epi32(cnt[0], cnt[1], cnt[2], cnt[3]);
        for (t = 0; t < tmax; t++) {
            __m128i vt = _mm_set1_epi32(t), vj = _mm_add_epi32(vlo, vt), vi, live = _mm_setzero_si128();
            __m128d j01, j23, x01, x23, fi01, fi23;
            __m128i i01, i23;
            __m128 fr, ta, tb, t0, t1, w, ha, hb, w01, w23, na, nb, nw;
            if (t >= tmin) {
                /* a lane past its last tap: no change to its sums, and an
                 * index that stays in the history */
                live = _mm_cmpgt_epi32(vcnt, vt);
                vj = _mm_or_si128(_mm_and_si128(live, vj), _mm_andnot_si128(live, vhi));
            }
            /* w = kernel(fabs(p - j) * fc), the scalar's operations */
            j01 = _mm_cvtepi32_pd(vj);
            j23 = _mm_cvtepi32_pd(_mm_shuffle_epi32(vj, _MM_SHUFFLE(3, 2, 3, 2)));
            x01 = _mm_and_pd(_mm_sub_pd(p01, j01), absm);
            x23 = _mm_and_pd(_mm_sub_pd(p23, j23), absm);
            fi01 = _mm_mul_pd(_mm_mul_pd(x01, vfc), vres);
            fi23 = _mm_mul_pd(_mm_mul_pd(x23, vfc), vres);
            i01 = _mm_cvttpd_epi32(fi01);
            i23 = _mm_cvttpd_epi32(fi23);
            fr = _mm_movelh_ps(_mm_cvtpd_ps(_mm_sub_pd(fi01, _mm_cvtepi32_pd(i01))),
                               _mm_cvtpd_ps(_mm_sub_pd(fi23, _mm_cvtepi32_pd(i23))));
            vi = _mm_unpacklo_epi64(i01, i23);
            _mm_store_si128((__m128i *)ii, vi);
            ta = _mm_loadh_pi(_mm_loadl_pi(zero, (const __m64 *)(tab + ii[0])), (const __m64 *)(tab + ii[1]));
            tb = _mm_loadh_pi(_mm_loadl_pi(zero, (const __m64 *)(tab + ii[2])), (const __m64 *)(tab + ii[3]));
            t0 = _mm_shuffle_ps(ta, tb, _MM_SHUFFLE(2, 0, 2, 0));   /* tab[i] */
            t1 = _mm_shuffle_ps(ta, tb, _MM_SHUFFLE(3, 1, 3, 1));   /* tab[i + 1] */
            w = _mm_add_ps(t0, _mm_mul_ps(fr, _mm_sub_ps(t1, t0)));
            w = _mm_and_ps(w, _mm_castsi128_ps(_mm_cmplt_epi32(vi, lim)));   /* past the table: 0.0f */
            /* the sums: acc0 += w * hist[j][0], acc1 += w * hist[j][1], wsum += w */
            _mm_store_si128((__m128i *)jj, vj);
            ha = _mm_loadh_pi(_mm_loadl_pi(zero, (const __m64 *)s->hist[jj[0]]), (const __m64 *)s->hist[jj[1]]);
            hb = _mm_loadh_pi(_mm_loadl_pi(zero, (const __m64 *)s->hist[jj[2]]), (const __m64 *)s->hist[jj[3]]);
            w01 = _mm_unpacklo_ps(w, w);
            w23 = _mm_unpackhi_ps(w, w);
            na = _mm_add_ps(a01, _mm_mul_ps(w01, ha));
            nb = _mm_add_ps(a23, _mm_mul_ps(w23, hb));
            nw = _mm_add_ps(ws, w);
            if (t < tmin) {
                a01 = na;
                a23 = nb;
                ws = nw;
            } else {
                __m128 l4 = _mm_castsi128_ps(live), l01 = _mm_unpacklo_ps(l4, l4), l23 = _mm_unpackhi_ps(l4, l4);
                a01 = bw_hf_sel(l01, na, a01);
                a23 = bw_hf_sel(l23, nb, a23);
                ws = bw_hf_sel(l4, nw, ws);
            }
        }
        {
            /* acc / wsum, or 0 where wsum is 0 */
            __m128 ws01 = _mm_unpacklo_ps(ws, ws), ws23 = _mm_unpackhi_ps(ws, ws);
            __m128 q01 = _mm_and_ps(_mm_div_ps(a01, ws01), _mm_cmpneq_ps(ws01, zero));
            __m128 q23 = _mm_and_ps(_mm_div_ps(a23, ws23), _mm_cmpneq_ps(ws23, zero));
            _mm_storeu_ps(&samples[out][0], q01);
            _mm_storeu_ps(&samples[out + 2][0], q23);
        }
        s->pos = p[3] + step;
        out += 4;
    }
    return out + bw_sinc_out_scalar(s, samples + out, requested_num - out, step, fc, half, tab, taps);
}

#endif
