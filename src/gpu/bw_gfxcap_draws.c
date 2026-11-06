/*
 * bw_gfxcap_draws.c - the capture's `draws` decoder in C (capture step G,
 * lane gfx-capef; docs/gfx/capture.md section 10). A test aid: the same
 * output as `tools/bw_gfxcap.py draws`, line for line, many times faster.
 *
 *   BW_GFXCAP_DRAWS=<file.gcap> [BW_GFXCAP_DRAWS_ARGS="--flips A-B --small
 *   --nan --max N --dump"] [BW_GFXCAP_DRAWS_OUT=<file>]  bloodwake.exe
 *
 * WinMain runs it before anything of the game or the runtime starts, writes
 * the listing (to the file, or stdout) and exits. Unset, nothing here runs.
 *
 * It keeps what bw_gfxcap.py's Decoder keeps: the executor's registers as the
 * methods leave them, the vertex attribute formats, the program memory, the
 * current batch's indices, inline words and immediate vertices, and guest
 * memory as the GPU side read it (PAGE and DIFF in stream order).
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <compressapi.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- options ------------------------------------------------------------------ */
static uint32_t o_lo = 0, o_hi = 999999999u, o_max = 200;
static int o_small, o_nan, o_dump;

static void parse_args(const char *s)
{
    while (s && *s) {
        while (*s == ' ')
            s++;
        if (!strncmp(s, "--flips", 7)) {
            s += 7;
            while (*s == ' ' || *s == '=')
                s++;
            o_lo = (uint32_t)strtoul(s, (char **)&s, 10);
            o_hi = o_lo;
            if (*s == '-') {
                s++;
                o_hi = (*s >= '0' && *s <= '9') ? (uint32_t)strtoul(s, (char **)&s, 10) : o_lo;
            }
        } else if (!strncmp(s, "--max", 5)) {
            s += 5;
            while (*s == ' ' || *s == '=')
                s++;
            o_max = (uint32_t)strtoul(s, (char **)&s, 10);
        } else if (!strncmp(s, "--small", 7)) {
            o_small = 1;
            s += 7;
        } else if (!strncmp(s, "--nan", 5)) {
            o_nan = 1;
            s += 5;
        } else if (!strncmp(s, "--dump", 6)) {
            o_dump = 1;
            s += 6;
        } else {
            while (*s && *s != ' ')
                s++;
        }
    }
}

/* ---- guest memory as recorded: 4 KB pages over the 32-bit space ------------------ */
static uint8_t **s_pages;               /* 1M entries */

static uint8_t *page_get(uint32_t pva, int make)
{
    uint8_t **e = &s_pages[pva >> 12];
    if (!*e && make)
        *e = (uint8_t *)calloc(1, 4096);
    return *e;
}

/* d.mem(va, n): NULL if any page isn't recorded */
static int mem(uint64_t va, uint32_t n, uint8_t *out)
{
    while (n > 0) {
        uint64_t p = va & ~(uint64_t)4095, off = va & 4095;
        uint32_t k = (uint32_t)(4096 - off) < n ? (uint32_t)(4096 - off) : n;
        uint8_t *pg;
        if (p > 0xFFFFF000ull || !(pg = page_get((uint32_t)p, 0)))
            return 0;
        memcpy(out, pg + off, k);
        out += k;
        va += k;
        n -= k;
    }
    return 1;
}

/* ---- the decoder's state ------------------------------------------------------------ */
static uint32_t s_reg[0x10000 / 4];
static uint8_t s_reg_set[0x10000 / 4];
static uint32_t s_attr[16][4];          /* offset, type, size, stride */
static uint32_t s_prog[512][4];
static uint8_t s_prog_set[512];
static uint32_t s_load, s_lsub, s_start, s_prim, s_gen = 1, s_batch, s_hwm;
static double s_cur[16][4];
static uint32_t *s_idx, s_nidx, s_cidx;
static uint32_t *s_inl, s_ninl, s_cinl;
static double (*s_imm)[4];
static uint32_t s_nimm, s_cimm;
static uint32_t s_shown;
static struct { uint32_t len, n; } s_nanb[64];
static int s_nnanb;
static FILE *s_out;

static uint32_t reg(uint32_t m) { return s_reg_set[m >> 2] ? s_reg[m >> 2] : 0; }

static void idx_push(uint32_t v)
{
    if (s_nidx == s_cidx) {
        s_cidx = s_cidx ? s_cidx * 2 : 65536;
        s_idx = (uint32_t *)realloc(s_idx, (size_t)s_cidx * 4);
    }
    s_idx[s_nidx++] = v;
}

static uint32_t resolve(uint32_t off)
{
    return off < s_hwm ? 0x80000000u + off : off;
}

static float f32(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

/* d.elem: n values (0: None) */
static int elem(const uint8_t *data, uint32_t typ, uint32_t size, double *v)
{
    uint32_t i, k = size < 4 ? size : 4;
    if (typ == 2) {
        for (i = 0; i < k; i++) { float f; memcpy(&f, data + 4 * i, 4); v[i] = f; }
        return (int)k;
    }
    if (typ == 0) {
        v[0] = data[2] / 255.0; v[1] = data[1] / 255.0; v[2] = data[0] / 255.0; v[3] = data[3] / 255.0;
        return 4;
    }
    if (typ == 4) {
        for (i = 0; i < k; i++) v[i] = data[i] / 255.0;
        return (int)k;
    }
    if (typ == 1 || typ == 5) {
        for (i = 0; i < k; i++) {
            int16_t s;
            memcpy(&s, data + 2 * i, 2);
            v[i] = typ == 1 ? s / 32767.0 : (double)s;
        }
        return (int)k;
    }
    return -1;
}

static uint32_t program(uint32_t (*out)[4])
{
    uint32_t n = 0, s = s_start;
    while (s < 136 && n < 136) {
        if (s < 512 && s_prog_set[s])
            memcpy(out[n], s_prog[s], 16);
        else
            memset(out[n], 0, 16);
        if (out[n++][3] & 1)
            break;
        s++;
    }
    return n;
}

static uint32_t inputs_used(uint32_t (*prog)[4], uint32_t n)
{
    uint32_t m = 0, i;
    for (i = 0; i < n; i++) {
        uint32_t *ins = prog[i];
        uint32_t mac = (ins[1] >> 21) & 15, ilu = (ins[1] >> 25) & 7;
        int ua = mac != 0, ub = mac == 2 || (mac >= 4 && mac <= 12), uc = mac == 3 || mac == 4 || ilu != 0;
        if ((ua && ((ins[2] >> 26) & 3) == 2) || (ub && ((ins[2] >> 11) & 3) == 2)
                || (uc && ((ins[3] >> 28) & 3) == 2))
            m |= 1u << ((ins[1] >> 9) & 15);
    }
    return m;
}

static void fnum(double x, char *b, size_t n)
{
    if (x != x)
        snprintf(b, n, "nan");                       /* Python's %.4f of any NaN */
    else if (isinf(x))
        snprintf(b, n, x > 0 ? "inf" : "-inf");
    else
        snprintf(b, n, "%.4f", x);
}

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

static void hexs(const uint8_t *p, uint32_t n, int ok)
{
    uint32_t i;
    if (ok)
        for (i = 0; i < n; i++)
            fprintf(s_out, "%02x", p[i]);
}

/* batch_end(kind): kind 0 indexed, 1 inline, 2 imm */
static void batch_end(int kind)
{
    static const char *const kn[3] = { "indexed", "inline", "imm" };
    static uint32_t prog[136][4];
    static uint32_t *uniq;
    static uint32_t cuniq;
    uint32_t np = 0, used, nv = 0, nuniq = 0, i, k;
    double verts[6][4];
    int vn[6], nverts = 0;              /* vn: values in each (-2: "?") */
    struct { uint32_t i; int n; uint32_t va, t, s, st; } nans[16];
    struct { uint32_t i; uint64_t va; uint32_t lo, hi; } miss[16];
    int nn = 0, nm = 0;
    char surf[96];

    s_batch++;
    if (!(o_lo <= s_gen && s_gen <= o_hi))
        return;
    if ((reg(0x1E94) & 3) == 2)
        np = program(prog);
    used = np ? inputs_used(prog, np) : 0xFFFFu;
    if (kind == 2) {
        nv = s_nimm;
        for (i = 0; i < s_nimm && nverts < 6; i++) {
            memcpy(verts[nverts], s_imm[i], sizeof verts[0]);
            vn[nverts++] = 4;
        }
    } else if (kind == 1) {
        uint32_t off = 0, nl = 0, lay[16][4];
        for (i = 0; i < 16; i++) {
            uint32_t t = s_attr[i][1], s = s_attr[i][2], b;
            if (!s)
                continue;
            b = t == 0 ? 4 : (t == 4 ? s : 4 * s);
            lay[nl][0] = i; lay[nl][1] = off; lay[nl][2] = t; lay[nl][3] = s;
            nl++;
            off += (b + 3) & ~3u;
        }
        nv = off ? (s_ninl * 4) / off : 0;
        for (k = 0; k < nv; k++)
            for (i = 0; i < nl; i++)
                if (lay[i][0] == 0 && nverts < 6) {
                    uint8_t tmp[16];
                    uint32_t at = k * off + lay[i][1], have = s_ninl * 4;
                    memset(tmp, 0, sizeof tmp);
                    if (at < have)
                        memcpy(tmp, (const uint8_t *)s_inl + at, have - at < 16 ? have - at : 16);
                    vn[nverts] = elem(tmp, lay[i][2], lay[i][3], verts[nverts]);
                    if (vn[nverts] <= 0)
                        vn[nverts] = -2;
                    nverts++;
                }
    } else
        nv = s_nidx;
    if (kind == 0 && s_nidx) {
        uint32_t vlo = 0xFFFFFFFFu, vhi = 0;
        if (cuniq < s_nidx) {
            cuniq = s_nidx;
            uniq = (uint32_t *)realloc(uniq, (size_t)cuniq * 4);
        }
        memcpy(uniq, s_idx, (size_t)s_nidx * 4);
        qsort(uniq, s_nidx, 4, cmp_u32);
        for (i = 0; i < s_nidx; i++)
            if (!nuniq || uniq[nuniq - 1] != uniq[i])
                uniq[nuniq++] = uniq[i];
        vlo = uniq[0];
        vhi = uniq[nuniq - 1];
        for (i = 0; i < 16; i++) {
            uint32_t o = s_attr[i][0], t = s_attr[i][1], s = s_attr[i][2], st = s_attr[i][3], va;
            int n_nan = 0;
            if (!s || !st || !o || !((used >> i) & 1))
                continue;
            va = resolve(o);
            for (k = 0; k < nuniq && k < 4096; k++) {
                uint8_t raw[16];
                double e[4];
                int ne, q;
                if (!mem((uint64_t)va + (uint64_t)uniq[k] * st, 16, raw)) {
                    n_nan = -1;
                    if (nm < 16) {
                        miss[nm].i = i; miss[nm].va = (uint64_t)va + (uint64_t)uniq[k] * st;
                        miss[nm].lo = vlo; miss[nm].hi = vhi;
                        nm++;
                    }
                    break;
                }
                ne = elem(raw, t, s, e);
                for (q = 0; q < ne; q++)
                    if (e[q] != e[q]) {
                        n_nan++;
                        break;
                    }
            }
            if (n_nan) {
                if (nn < 16) {
                    nans[nn].i = i; nans[nn].n = n_nan; nans[nn].va = va;
                    nans[nn].t = t; nans[nn].s = s; nans[nn].st = st;
                    nn++;
                }
                if (o_dump) {
                    for (k = 0; k < nuniq && k < 4; k++) {
                        uint8_t a1[64], a2[256];
                        uint64_t at = (uint64_t)va + (uint64_t)uniq[k] * st;
                        int ok1 = 4 * s <= sizeof a1 && mem(at, 4 * s, a1);
                        int ok2 = s_attr[0][2] && st <= sizeof a2
                                  && mem((uint64_t)va - ((int64_t)o - (int64_t)s_attr[0][0]) + (uint64_t)uniq[k] * st,
                                         st, a2);
                        fprintf(s_out, "    dump v%u vertex %u at %08llX: ", i, uniq[k], (unsigned long long)at);
                        hexs(a1, 4 * s, ok1);
                        fprintf(s_out, " | vertex: ");
                        hexs(a2, st, ok2);
                        fprintf(s_out, "\n");
                    }
                    for (k = 0; k < np; k++)
                        fprintf(s_out, "    prog[%u] %08X %08X %08X %08X\n", k, prog[k][0], prog[k][1],
                                prog[k][2], prog[k][3]);
                }
            }
        }
        if (nuniq <= 6) {
            uint32_t o = s_attr[0][0], t = s_attr[0][1], s = s_attr[0][2], st = s_attr[0][3];
            uint32_t va = resolve(o);
            for (k = 0; k < s_nidx && k < 6; k++) {
                uint8_t raw[16];
                if (mem((uint64_t)va + (uint64_t)s_idx[k] * st, 16, raw)) {
                    vn[nverts] = elem(raw, t, s, verts[nverts]);
                    if (vn[nverts] <= 0)
                        vn[nverts] = -2;
                } else
                    vn[nverts] = -2;
                nverts++;
            }
        }
    }
    {
        uint32_t ch = reg(0x200), cv = reg(0x204);
        snprintf(surf, sizeof surf, "surf %08X %ux%u+%u+%u fmt %08X", reg(0x210), ch >> 16, cv >> 16,
                 ch & 0xFFFF, cv & 0xFFFF, reg(0x208));
    }
    if (nn) {
        int j;
        for (j = 0; j < s_nnanb && s_nanb[j].len != np; j++)
            ;
        if (j == s_nnanb && s_nnanb < 64)
            s_nanb[s_nnanb++].len = np;
        if (j < 64)
            s_nanb[j].n++;
    }
    if (o_nan && !nn)
        return;
    if (o_small && nv > 6)
        return;
    if (s_shown >= o_max)
        return;
    s_shown++;
    fprintf(s_out, "f%u b%u %s prim %u n %u %s", s_gen, s_batch, kn[kind], s_prim, nv, surf);
    if (np)
        fprintf(s_out, " prog@%u len %u in %04X", s_start, np, used);
    fprintf(s_out, "\n");
    for (i = 0; i < (uint32_t)nn; i++)
        fprintf(s_out, "    v%u: %d of the batch's vertices read NaN (array %08X type %u size %u stride %u)\n",
                nans[i].i, nans[i].n, nans[i].va, nans[i].t, nans[i].s, nans[i].st);
    for (i = 0; i < (uint32_t)nm; i++)
        fprintf(s_out, "    v%u: no recorded bytes at %08llX (indices %u..%u)\n", miss[i].i,
                (unsigned long long)miss[i].va, miss[i].lo, miss[i].hi);
    if (nverts && (o_small || nv <= 6)) {
        int j;
        fprintf(s_out, "    pos ");
        for (j = 0; j < nverts && j < 6; j++) {
            int q;
            if (j)
                fprintf(s_out, "  ");
            if (vn[j] <= 0) {
                fprintf(s_out, "?");
                continue;
            }
            fprintf(s_out, "(");
            for (q = 0; q < vn[j]; q++) {
                char b[64];
                fnum(verts[j][q], b, sizeof b);
                fprintf(s_out, "%s%s", q ? ", " : "", b);
            }
            fprintf(s_out, ")");
        }
        fprintf(s_out, "\n");
    }
}

static void method(uint32_t sm, uint32_t prm)
{
    uint32_t sub = sm >> 16, m = sm & 0xFFFF;
    if (sub != 0)
        return;
    s_reg[m >> 2] = prm;
    s_reg_set[m >> 2] = 1;
    if (m >= 0x1720 && m < 0x1760)
        s_attr[(m - 0x1720) / 4][0] = prm;
    else if (m >= 0x1760 && m < 0x17A0) {
        uint32_t *at = s_attr[(m - 0x1760) / 4];
        at[1] = prm & 15; at[2] = (prm >> 4) & 15; at[3] = (prm >> 8) & 255;
        if (at[2] == 7)
            at[2] = 3;
    } else if (m == 0x1E9C) {
        s_load = prm;
        s_lsub = 0;
    } else if (m == 0x1EA0)
        s_start = prm;
    else if (m >= 0x0B00 && m < 0x0B80) {
        if (s_load < 512) {
            if (!s_prog_set[s_load]) {
                memset(s_prog[s_load], 0, 16);
                s_prog_set[s_load] = 1;
            }
            s_prog[s_load][s_lsub] = prm;
        }
        if (++s_lsub == 4) {
            s_lsub = 0;
            s_load++;
        }
    } else if (m == 0x17FC) {
        if (prm) {
            s_prim = prm;
            s_nidx = s_ninl = s_nimm = 0;
        } else {
            int kind = s_nimm ? 2 : (s_ninl ? 1 : 0);
            if (s_prim)
                batch_end(kind);
            s_prim = 0;
        }
    } else if (m == 0x1800 && s_prim) {
        idx_push(prm & 0xFFFF);
        idx_push(prm >> 16);
    } else if (m == 0x1808 && s_prim)
        idx_push(prm);
    else if (m == 0x1810 && s_prim) {
        uint32_t st = prm & 0xFFFFFF, c = ((prm >> 24) & 255) + 1, i;
        for (i = 0; i < c; i++)
            idx_push(st + i);
    } else if (m == 0x1818 && s_prim) {
        if (s_ninl == s_cinl) {
            s_cinl = s_cinl ? s_cinl * 2 : 65536;
            s_inl = (uint32_t *)realloc(s_inl, (size_t)s_cinl * 4);
        }
        s_inl[s_ninl++] = prm;
    } else if (m >= 0x1500 && m < 0x1528) {
        int push = 0;
        if (m < 0x150C) {
            s_cur[0][(m - 0x1500) / 4] = f32(prm);
            s_cur[0][3] = 1.0;
            push = m == 0x1508 && s_prim;
        } else if (m >= 0x1518) {
            s_cur[0][(m - 0x1518) / 4] = f32(prm);
            push = m == 0x1524 && s_prim;
        }
        if (push) {
            if (s_nimm == s_cimm) {
                s_cimm = s_cimm ? s_cimm * 2 : 4096;
                s_imm = (double (*)[4])realloc(s_imm, (size_t)s_cimm * sizeof *s_imm);
            }
            memcpy(s_imm[s_nimm++], s_cur[0], sizeof s_imm[0]);
        }
    } else if (m >= 0x1A00 && m < 0x1B00)
        s_cur[(m - 0x1A00) / 16][((m - 0x1A00) % 16) / 4] = f32(prm);
}

static void records(const uint8_t *raw, uint32_t len)
{
    uint32_t at = 0;
    while (at + 5 <= len) {
        uint8_t t = raw[at];
        uint32_t n, i;
        const uint8_t *p;
        memcpy(&n, raw + at + 1, 4);
        p = raw + at + 5;
        if (at + 5 + n > len)
            break;
        at += 5 + n;
        switch (t) {
        case 3: {                       /* PAGE */
            uint32_t va;
            memcpy(&va, p, 4);
            memcpy(page_get(va & ~4095u, 1), p + 4, 4096);
            break;
        }
        case 4: {                       /* DIFF: inside its page (the recorder's rule; bytes past
                                           the page's end are dropped, as the Python decoder does) */
            uint32_t va, off, k = n - 4;
            memcpy(&va, p, 4);
            off = va & 4095;
            memcpy(page_get(va & ~4095u, 1) + off, p + 4, k < 4096 - off ? k : 4096 - off);
            break;
        }
        case 2: {                       /* IDX16 */
            uint32_t c;
            memcpy(&c, p, 4);
            if (s_prim)
                for (i = 0; i < c; i++) {
                    uint32_t w;
                    memcpy(&w, p + 4 + 4 * i, 4);
                    idx_push(w & 0xFFFF);
                    idx_push(w >> 16);
                }
            break;
        }
        case 7:                         /* FLIP */
            memcpy(&s_gen, p, 4);
            s_gen++;
            break;
        case 11:                        /* CONTIG */
            memcpy(&s_hwm, p, 4);
            break;
        case 9: {                       /* a mid-scene OPEN (step B): its first frame, the
                                           allocator's mark, the state as its method preamble */
            uint32_t why, slen, npre;
            memcpy(&why, p, 4);
            if (n < 28 || why != 1)
                break;
            memcpy(&s_gen, p + 4, 4);
            memcpy(&s_hwm, p + 8, 4);
            memcpy(&slen, p + 20, 4);
            if (28 + (uint64_t)slen > n)
                break;
            memcpy(&npre, p + 24 + slen, 4);
            if (28 + (uint64_t)slen + (uint64_t)npre * 8 > n)
                break;
            for (i = 0; i < npre; i++) {
                uint32_t sm, prm;
                memcpy(&sm, p + 28 + slen + 8 * i, 4);
                memcpy(&prm, p + 32 + slen + 8 * i, 4);
                method(sm, prm);
            }
            break;
        }
        case 1: {                       /* METHODS */
            uint32_t c;
            memcpy(&c, p, 4);
            for (i = 0; i < c; i++) {
                uint32_t sm, prm;
                memcpy(&sm, p + 4 + 8 * i, 4);
                memcpy(&prm, p + 8 + 8 * i, 4);
                method(sm, prm);
            }
            break;
        }
        default:
            break;
        }
    }
}

int bw_gfxcap_draws_main(void)
{
    const char *path = getenv("BW_GFXCAP_DRAWS"), *outp = getenv("BW_GFXCAP_DRAWS_OUT");
    DECOMPRESSOR_HANDLE dec = NULL;
    uint8_t hd[64], *stored = NULL, *raw = NULL;
    size_t cs = 0, cr = 0;
    uint32_t text_len, chunks = 0;
    LARGE_INTEGER t0, t1, fq;
    FILE *f;
    int j;

    QueryPerformanceCounter(&t0);
    parse_args(getenv("BW_GFXCAP_DRAWS_ARGS"));
    s_out = outp && *outp ? fopen(outp, "w") : stdout;
    if (!s_out)
        return 2;
    setvbuf(s_out, NULL, _IOFBF, 1 << 20);
    if (!(f = fopen(path, "rb")) || fread(hd, 1, 64, f) != 64 || memcmp(hd, "BWGCAP01", 8)) {
        fprintf(stderr, "[GFXCAP] draws: %s is not a capture\n", path);
        return 2;
    }
    memcpy(&text_len, hd + 20, 4);
    fseek(f, (long)text_len, SEEK_CUR);
    s_pages = (uint8_t **)calloc(1u << 20, sizeof *s_pages);
    if (!s_pages || !CreateDecompressor(COMPRESS_ALGORITHM_XPRESS_HUFF, NULL, &dec))
        return 2;
    for (;;) {
        uint32_t ch[4];
        SIZE_T got = 0;
        if (fread(ch, 4, 4, f) != 4)
            break;
        if (ch[1] > cs) { free(stored); cs = ch[1]; stored = (uint8_t *)malloc(cs); }
        if (ch[0] > cr) { free(raw); cr = ch[0]; raw = (uint8_t *)malloc(cr); }
        if (!stored || !raw || fread(stored, 1, ch[1], f) != ch[1])
            break;                      /* cut short: bw_gfxcap.py stops there too */
        if (ch[2] == 0)
            memcpy(raw, stored, ch[0]);
        else if (!Decompress(dec, stored, ch[1], raw, ch[0], &got) || got != ch[0])
            break;
        records(raw, ch[0]);
        chunks++;
    }
    if (s_nnanb) {
        fprintf(s_out, "batches with NaN inputs, by program length: {");
        for (j = 0; j < s_nnanb; j++)
            fprintf(s_out, "%s%u: %u", j ? ", " : "", s_nanb[j].len, s_nanb[j].n);
        fprintf(s_out, "}\n");
    }
    fflush(s_out);
    if (s_out != stdout)
        fclose(s_out);
    fclose(f);
    CloseDecompressor(dec);
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&fq);
    fprintf(stderr, "[GFXCAP] draws: %s, %u chunks, %u batches, in %.1f s\n", path, chunks, s_batch,
            (double)(t1.QuadPart - t0.QuadPart) / fq.QuadPart);
    return 0;
}
