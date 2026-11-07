/* bw_hdr.c - the screen in HDR mode, for every edition (0.2.5 item 97,
 * D-026). See bw_hdr.h. Moved here from bw_vfx_post.c (item 45's screen
 * check and watcher), so the plain present (every edition) and Remastered's
 * visual suite share one view of the screen and one set of formulas
 * (docs/remaster/hdr_standards.md). */
#include "bw_hdr.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* ---- one look at a screen ----------------------------------------------------- */

typedef struct {
    BwHdrScreen s;              /* as used */
    HMONITOR mon;
    /* DXGI's own figures, logged for comparison */
    int dxgi_ok;
    float d_peak, d_black, d_maxfall, d_prim[8];
    DXGI_COLOR_SPACE_TYPE d_cs;
    /* the EDID (the registry copy) */
    int edid_ok, edid_cta_hdr;  /* read and checksummed; a CTA-861.3 HDR static metadata block found */
    unsigned edid_eotfs;        /* its EOTF bits: 1 SDR, 2 HDR gamma, 4 ST 2084 (PQ), 8 HLG */
    float e_peak, e_maxfall, e_black, e_prim[8];    /* < 0: not given */
    int e_prim_ok;
    /* the HDR calibration profile Windows has for the screen (its Advanced
     * Color profile; with an MHC2 tag Windows applies it to everything on
     * that screen, which an app can't bypass: inferred, per Microsoft) */
    int prof_ok, prof_mhc2;
    float p_peak, p_black, p_maxfall;   /* < 0: not given */
    WCHAR prof_name[96];
    /* DisplayConfig: the source and target of the window's screen */
    LUID src_adapter, tgt_adapter;
    UINT32 src_id, tgt_id;
    int tgt_ok;
    UINT32 sig[4];              /* the target's advanced-colour state (watcher) */
    int hdr_supported;          /* DisplayConfig: the screen can do HDR (or the EDID says PQ) */
    WCHAR friendly[64], devpath[160];
} BwHdrProbe;

/* The DisplayConfig advanced-colour info: own copy of the structure; type 15
 * (Windows 11 24H2) first, then type 9. */
typedef struct {
    DISPLAYCONFIG_DEVICE_INFO_HEADER header;
    UINT32 value, encoding, bits, mode;
} BwAdvColor;

static void find_target(BwHdrProbe *p)
{
    UINT32 np = 0, nm = 0, i;
    DISPLAYCONFIG_PATH_INFO *paths;
    DISPLAYCONFIG_MODE_INFO *modes;
    p->tgt_ok = 0;
    if (!p->s.name[0] || GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS || !np)
        return;
    paths = (DISPLAYCONFIG_PATH_INFO *)calloc(np, sizeof *paths);
    modes = (DISPLAYCONFIG_MODE_INFO *)calloc(nm ? nm : 1, sizeof *modes);
    if (paths && modes && QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths, &nm, modes, NULL) == ERROR_SUCCESS)
        for (i = 0; i < np && !p->tgt_ok; i++) {
            DISPLAYCONFIG_SOURCE_DEVICE_NAME sn;
            memset(&sn, 0, sizeof sn);
            sn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            sn.header.size = sizeof sn;
            sn.header.adapterId = paths[i].sourceInfo.adapterId;
            sn.header.id = paths[i].sourceInfo.id;
            if (DisplayConfigGetDeviceInfo(&sn.header) == ERROR_SUCCESS && !wcscmp(sn.viewGdiDeviceName, p->s.name)) {
                DISPLAYCONFIG_TARGET_DEVICE_NAME tn;
                p->src_adapter = paths[i].sourceInfo.adapterId;
                p->src_id = paths[i].sourceInfo.id;
                p->tgt_adapter = paths[i].targetInfo.adapterId;
                p->tgt_id = paths[i].targetInfo.id;
                p->tgt_ok = 1;
                memset(&tn, 0, sizeof tn);
                tn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
                tn.header.size = sizeof tn;
                tn.header.adapterId = p->tgt_adapter;
                tn.header.id = p->tgt_id;
                if (DisplayConfigGetDeviceInfo(&tn.header) == ERROR_SUCCESS) {
                    wcsncpy(p->friendly, tn.monitorFriendlyDeviceName, 63);
                    p->friendly[63] = 0;
                    wcsncpy(p->devpath, tn.monitorDevicePath, 159);
                    p->devpath[159] = 0;
                }
            }
        }
    free(paths);
    free(modes);
}

/* The target's advanced-colour state as a signature; 0 if it can't be read. */
static int target_sig(const BwHdrProbe *p, UINT32 sig[4], int *supported)
{
    BwAdvColor a;
    int typ;
    if (!p->tgt_ok)
        return 0;
    for (typ = 15; typ >= 9; typ -= 6) {
        memset(&a, 0, sizeof a);
        a.header.type = (DISPLAYCONFIG_DEVICE_INFO_TYPE)typ;
        a.header.size = typ == 15 ? sizeof a : sizeof a - sizeof a.mode;
        a.header.adapterId = p->tgt_adapter;
        a.header.id = p->tgt_id;
        if (DisplayConfigGetDeviceInfo(&a.header) == ERROR_SUCCESS) {
            sig[0] = a.value; sig[1] = a.encoding; sig[2] = a.bits; sig[3] = a.mode;
            if (supported)      /* type 15: highDynamicRangeSupported (bit 4); type 9: advancedColorSupported */
                *supported = typ == 15 ? (a.value >> 4) & 1 : a.value & 1;
            return 1;
        }
    }
    return 0;
}

/* ---- the EDID: the registry's copy of the monitor's own ----------------------- */

/* CIE 1931 2-degree spectral locus, 380-700 nm (closed by the purple line). */
static const float s_locus[][2] = {
    { 0.1741f, 0.0050f }, { 0.1440f, 0.0297f }, { 0.1241f, 0.0578f }, { 0.0913f, 0.1327f },
    { 0.0687f, 0.2007f }, { 0.0454f, 0.2950f }, { 0.0235f, 0.4127f }, { 0.0082f, 0.5384f },
    { 0.0039f, 0.6548f }, { 0.0139f, 0.7502f }, { 0.0389f, 0.8120f }, { 0.0743f, 0.8338f },
    { 0.1142f, 0.8262f }, { 0.1547f, 0.8059f }, { 0.1929f, 0.7816f }, { 0.2296f, 0.7543f },
    { 0.2658f, 0.7243f }, { 0.3016f, 0.6923f }, { 0.3373f, 0.6589f }, { 0.3731f, 0.6245f },
    { 0.4087f, 0.5896f }, { 0.4441f, 0.5547f }, { 0.4788f, 0.5202f }, { 0.5125f, 0.4866f },
    { 0.5448f, 0.4544f }, { 0.5752f, 0.4242f }, { 0.6029f, 0.3965f }, { 0.6270f, 0.3725f },
    { 0.6482f, 0.3514f }, { 0.6658f, 0.3340f }, { 0.6801f, 0.3197f }, { 0.6915f, 0.3083f },
    { 0.7006f, 0.2993f }, { 0.7079f, 0.2920f }, { 0.7140f, 0.2859f }, { 0.7190f, 0.2809f },
    { 0.7230f, 0.2770f }, { 0.7260f, 0.2740f }, { 0.7347f, 0.2653f },
};

static int in_locus(float x, float y)
{
    int i, n = (int)(sizeof s_locus / sizeof s_locus[0]), in = 0;
    for (i = 0; i < n; i++) {
        const float *a = s_locus[i], *b = s_locus[(i + 1) % n];
        if (((a[1] > y) != (b[1] > y)) && (x < (b[0] - a[0]) * (y - a[1]) / (b[1] - a[1]) + a[0]))
            in = !in;
    }
    return in;
}

/* Primaries and white point a screen could have: each inside the locus, a
 * triangle of some size around the white, red to the right of green and
 * blue, the white near the daylight whites. */
static int prim_plausible(const float p[8])
{
    float area;
    int i;
    for (i = 0; i < 4; i++)
        if (!(p[i * 2] > 0.0f && p[i * 2 + 1] > 0.0f) || !in_locus(p[i * 2], p[i * 2 + 1]))
            return 0;
    area = 0.5f * fabsf((p[2] - p[0]) * (p[5] - p[1]) - (p[4] - p[0]) * (p[3] - p[1]));
    if (area < 0.05f || p[0] < p[2] || p[0] < p[4] || p[3] < p[5])
        return 0;
    return p[6] > 0.28f && p[6] < 0.36f && p[7] > 0.28f && p[7] < 0.38f;
}

/* Does the panel's gamut (its R, G, B triangle) hold BT.709's primaries?
 * Reported only: the game's colours go out as BT.709 in scRGB and Windows
 * applies BT.2087's matrix; nothing is stretched toward the panel (D-026). */
static float tri_side(float ax, float ay, float bx, float by, float px, float py)
{
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

static int covers_709(const float p[8])
{
    static const float r709[6] = { 0.640f, 0.330f, 0.300f, 0.600f, 0.150f, 0.060f };
    int i;
    for (i = 0; i < 3; i++) {
        float x = r709[i * 2], y = r709[i * 2 + 1];
        float d1 = tri_side(p[0], p[1], p[2], p[3], x, y), d2 = tri_side(p[2], p[3], p[4], p[5], x, y);
        float d3 = tri_side(p[4], p[5], p[0], p[1], x, y);
        /* a margin of about three of the EDID's 10-bit chromaticity steps
         * (1/1024; on this PC BT.709's blue lies 0.0006 outside the panel's
         * blue-red edge, inside that precision) */
        const float m = 0.0015f;
        int neg = d1 < -m || d2 < -m || d3 < -m, pos = d1 > m || d2 > m || d3 > m;
        if (neg && pos)
            return 0;
    }
    return 1;
}

static int edid_read(const WCHAR *devpath, uint8_t *buf, DWORD *len)
{
    /* \\?\DISPLAY#<hardware id>#<instance>#{guid} */
    WCHAR key[400], hw[96], inst[160];
    const WCHAR *p = wcsstr(devpath, L"DISPLAY#"), *q;
    size_t n;
    if (!p)
        return 0;
    p += 8;
    if (!(q = wcschr(p, L'#')) || (n = (size_t)(q - p)) >= 96)
        return 0;
    wmemcpy(hw, p, n); hw[n] = 0;
    p = q + 1;
    if (!(q = wcschr(p, L'#')) || (n = (size_t)(q - p)) >= 160)
        return 0;
    wmemcpy(inst, p, n); inst[n] = 0;
    _snwprintf(key, 400, L"SYSTEM\\CurrentControlSet\\Enum\\DISPLAY\\%ls\\%ls\\Device Parameters", hw, inst);
    key[399] = 0;
    return RegGetValueW(HKEY_LOCAL_MACHINE, key, L"EDID", RRF_RT_REG_BINARY, NULL, buf, len) == ERROR_SUCCESS;
}

/* CTA-861.3: desired content max (and max frame-average) luminance
 * 50 x 2^(CV/32) cd/m2, 0 meaning not given; min = max x (CV/255)^2 / 100. */
static void edid_parse(BwHdrProbe *p, const uint8_t *e, DWORD n)
{
    static const uint8_t hd[8] = { 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0 };
    unsigned sum = 0, i, b, ext;
    uint8_t lo1, lo2;
    p->e_peak = p->e_maxfall = p->e_black = -1.0f;
    if (n < 128 || memcmp(e, hd, 8))
        return;
    for (i = 0; i < 128; i++)
        sum += e[i];
    if (sum & 0xFF)
        return;
    p->edid_ok = 1;
    lo1 = e[25]; lo2 = e[26];
    p->e_prim[0] = (float)((e[27] << 2) | ((lo1 >> 6) & 3)) / 1024.0f;
    p->e_prim[1] = (float)((e[28] << 2) | ((lo1 >> 4) & 3)) / 1024.0f;
    p->e_prim[2] = (float)((e[29] << 2) | ((lo1 >> 2) & 3)) / 1024.0f;
    p->e_prim[3] = (float)((e[30] << 2) | (lo1 & 3)) / 1024.0f;
    p->e_prim[4] = (float)((e[31] << 2) | ((lo2 >> 6) & 3)) / 1024.0f;
    p->e_prim[5] = (float)((e[32] << 2) | ((lo2 >> 4) & 3)) / 1024.0f;
    p->e_prim[6] = (float)((e[33] << 2) | ((lo2 >> 2) & 3)) / 1024.0f;
    p->e_prim[7] = (float)((e[34] << 2) | (lo2 & 3)) / 1024.0f;
    p->e_prim_ok = prim_plausible(p->e_prim);
    ext = e[126];
    if (n >= 256 && e[128] == 0x02 && e[130] >= 4 && e[130] <= 127) {
        /* HDMI Forum's EDID Extension Override Data Block (extended tag
         * 0x78) in the first CTA block gives the real extension count
         * (seen on this PC: byte 126 says 1, the EDID has 3 blocks;
         * hdr_standards.md section 9) */
        const uint8_t *x = e + 128;
        unsigned d = x[2], k = 4;
        while (k < d) {
            unsigned tag = x[k] >> 5, len = x[k] & 31;
            if (k + 1 + len > d)
                break;
            if (tag == 7 && len >= 2 && x[k + 1] == 0x78 && x[k + 2])
                ext = x[k + 2];
            k += 1 + len;
        }
    }
    for (b = 1; b <= ext && (b + 1) * 128 <= n; b++) {
        const uint8_t *x = e + b * 128;
        unsigned d = x[2], k = 4;
        if (x[0] != 0x02 || d < 4 || d > 127)
            continue;                   /* CTA-861 extensions only */
        while (k < d) {
            unsigned tag = x[k] >> 5, len = x[k] & 31;
            if (k + 1 + len > d)
                break;
            if (tag == 7 && len >= 2 && x[k + 1] == 6) {     /* extended tag 6: HDR static metadata */
                float mx = -1.0f;
                p->edid_cta_hdr = 1;
                p->edid_eotfs = x[k + 2];
                if (len >= 4 && x[k + 4])
                    mx = 50.0f * powf(2.0f, x[k + 4] / 32.0f);
                if (len >= 5 && x[k + 5])
                    p->e_maxfall = 50.0f * powf(2.0f, x[k + 5] / 32.0f);
                if (len >= 6 && mx > 0.0f)
                    p->e_black = mx * (x[k + 6] / 255.0f) * (x[k + 6] / 255.0f) / 100.0f;
                p->e_peak = mx;
            }
            k += 1 + len;
        }
    }
}

/* ---- the HDR calibration profile (Windows' Advanced Color ICC profile) -------- */

typedef HRESULT (WINAPI *PFN_ColorProfileGetDisplayDefault)(int scope, LUID adapter, UINT32 source,
                                                            int type, int subtype, LPWSTR *name);
typedef BOOL (WINAPI *PFN_GetColorDirectoryW)(PCWSTR machine, PWSTR dir, PDWORD size);

static uint32_t be32(const uint8_t *b) { return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3]; }
static float s15f16(const uint8_t *b) { return (float)(int32_t)be32(b) / 65536.0f; }

static void profile_read(BwHdrProbe *p)
{
    static HMODULE m;
    static PFN_ColorProfileGetDisplayDefault getdef;
    static PFN_GetColorDirectoryW getdir;
    LPWSTR name = NULL;
    WCHAR dir[MAX_PATH], path[MAX_PATH * 2];
    DWORD dl = sizeof dir;
    FILE *f;
    uint8_t *icc = NULL;
    long sz;
    int scope;
    p->p_peak = p->p_black = p->p_maxfall = -1.0f;
    if (!p->tgt_ok)
        return;
    if (!m && (m = LoadLibraryW(L"mscms.dll")) != NULL) {
        getdef = (PFN_ColorProfileGetDisplayDefault)(void *)GetProcAddress(m, "ColorProfileGetDisplayDefault");
        getdir = (PFN_GetColorDirectoryW)(void *)GetProcAddress(m, "GetColorDirectoryW");
    }
    if (!getdef || !getdir)
        return;
    /* the current user's, else the system-wide; ICC, the extended (advanced
     * colour) display mode */
    for (scope = 1; scope >= 0 && !name; scope--)
        if (FAILED(getdef(scope, p->src_adapter, p->src_id, 0, 8, &name)))
            name = NULL;
    if (!name || !name[0] || !getdir(NULL, dir, &dl)) {
        if (name) LocalFree(name);
        return;
    }
    _snwprintf(path, MAX_PATH * 2, L"%ls\\%ls", dir, name);
    path[MAX_PATH * 2 - 1] = 0;
    wcsncpy(p->prof_name, name, 95);
    p->prof_name[95] = 0;
    LocalFree(name);
    if (!(f = _wfopen(path, L"rb")))
        return;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz >= 132 && sz < (16 << 20) && (icc = (uint8_t *)malloc((size_t)sz)) != NULL
            && fread(icc, 1, (size_t)sz, f) == (size_t)sz) {
        uint32_t nt = be32(icc + 128), i;
        p->prof_ok = 1;
        for (i = 0; i < nt && 132 + (i + 1) * 12 <= (uint32_t)sz; i++) {
            const uint8_t *t = icc + 132 + i * 12;
            uint32_t sig = be32(t), off = be32(t + 4), len = be32(t + 8);
            if (off + len > (uint32_t)sz)
                continue;
            if (sig == 0x4D484332u && len >= 20) {          /* 'MHC2': min at +12, peak at +16 (s15Fixed16) */
                p->prof_mhc2 = 1;
                p->p_black = s15f16(icc + off + 12);
                p->p_peak = s15f16(icc + off + 16);
            } else if (sig == 0x6C756D69u && len >= 20)     /* 'lumi': Y, the max full-frame (section 9) */
                p->p_maxfall = s15f16(icc + off + 12);
        }
    }
    free(icc);
    fclose(f);
}

/* ---- checks and choices ------------------------------------------------------- */

static const float s_bt709[8] = { 0.640f, 0.330f, 0.300f, 0.600f, 0.150f, 0.060f, 0.3127f, 0.3290f };

static int peak_ok(float v) { return v >= 80.0f && v <= 10000.0f; }

/* The values used: EDID first, then the profile for what the EDID lacks or
 * gets wrong, then the standards' defaults. BW_HDR_TEST_NOEDID=1 (test aid)
 * pretends the EDID gives nothing. */
static void choose(BwHdrProbe *p)
{
    static int noedid = -1;
    BwHdrScreen *s = &p->s;
    if (noedid < 0) {
        const char *e = getenv("BW_HDR_TEST_NOEDID");
        noedid = e && e[0] == '1';
    }
    if (noedid) {
        p->e_peak = p->e_maxfall = p->e_black = -1.0f;
        p->e_prim_ok = 0;
    }
    s->peak = 1000.0f; s->peak_src = "none";
    if (peak_ok(p->e_peak)) { s->peak = p->e_peak; s->peak_src = "edid"; }
    else if (peak_ok(p->p_peak)) { s->peak = p->p_peak; s->peak_src = "profile"; }
    s->black = 0.0f; s->black_src = "default";
    if (p->e_black >= 0.0f && p->e_black < s->peak / 10.0f && p->e_black < 5.0f) {
        s->black = p->e_black; s->black_src = "edid";
    } else if (p->p_black >= 0.0f && p->p_black < s->peak / 10.0f && p->p_black < 5.0f) {
        s->black = p->p_black; s->black_src = "profile";
    }
    s->maxfall = s->peak; s->maxfall_src = "default";
    if (p->e_maxfall > 0.0f) {
        s->maxfall = p->e_maxfall > s->peak ? s->peak : p->e_maxfall;
        s->maxfall_src = "edid";
    } else if (p->p_maxfall > 0.0f) {
        s->maxfall = p->p_maxfall > s->peak ? s->peak : p->p_maxfall;
        s->maxfall_src = "profile";
    }
    if (p->e_prim_ok) {
        memcpy(s->prim, p->e_prim, sizeof s->prim);
        s->prim_src = "edid";
    } else {
        memcpy(s->prim, s_bt709, sizeof s->prim);
        s->prim_src = "default";
    }
}

/* Look at the screen `mon` now: DXGI (a fresh factory, so its outputs read
 * the screen as it is now), DisplayConfig, the EDID and the profile. */
static void probe(HMONITOR mon, BwHdrProbe *p)
{
    IDXGIFactory1 *fac = NULL;
    UINT a, o;
    int found = 0;
    uint8_t edid[2048];
    DWORD el = sizeof edid;
    memset(p, 0, sizeof *p);
    p->mon = mon;
    p->s.hdr = -2;
    p->e_peak = p->e_maxfall = p->e_black = p->p_peak = p->p_black = -1.0f;
    if (SUCCEEDED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&fac)) && fac) {
        /* every adapter: the window's screen may hang off another GPU than ours */
        for (a = 0; !found; a++) {
            IDXGIAdapter1 *ad = NULL;
            if (IDXGIFactory1_EnumAdapters1(fac, a, &ad) != S_OK)
                break;
            for (o = 0; !found; o++) {
                IDXGIOutput *out = NULL;
                IDXGIOutput6 *o6 = NULL;
                DXGI_OUTPUT_DESC od;
                DXGI_OUTPUT_DESC1 d1;
                if (IDXGIAdapter1_EnumOutputs(ad, o, &out) != S_OK)
                    break;
                if (SUCCEEDED(IDXGIOutput_GetDesc(out, &od)) && od.Monitor == mon) {
                    found = 1;
                    p->s.hdr = -1;
                    memcpy(p->s.name, od.DeviceName, sizeof p->s.name);
                    p->s.name[31] = 0;
                    if (SUCCEEDED(IDXGIOutput_QueryInterface(out, &IID_IDXGIOutput6, (void **)&o6))) {
                        if (SUCCEEDED(IDXGIOutput6_GetDesc1(o6, &d1))) {
                            p->dxgi_ok = 1;
                            p->s.hdr = d1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
                            p->d_cs = d1.ColorSpace;
                            p->d_peak = d1.MaxLuminance;
                            p->d_black = d1.MinLuminance;
                            p->d_maxfall = d1.MaxFullFrameLuminance;
                            p->d_prim[0] = d1.RedPrimary[0];   p->d_prim[1] = d1.RedPrimary[1];
                            p->d_prim[2] = d1.GreenPrimary[0]; p->d_prim[3] = d1.GreenPrimary[1];
                            p->d_prim[4] = d1.BluePrimary[0];  p->d_prim[5] = d1.BluePrimary[1];
                            p->d_prim[6] = d1.WhitePoint[0];   p->d_prim[7] = d1.WhitePoint[1];
                            p->s.bits = d1.BitsPerColor;
                        }
                        IDXGIOutput6_Release(o6);
                    }
                }
                IDXGIOutput_Release(out);
            }
            IDXGIAdapter1_Release(ad);
        }
        IDXGIFactory1_Release(fac);
    }
    if (found) {
        find_target(p);
        if (!target_sig(p, p->sig, &p->hdr_supported))
            p->tgt_ok = p->tgt_ok ? 1 : 0;
        if (p->devpath[0] && edid_read(p->devpath, edid, &el))
            edid_parse(p, edid, el);
        if (p->edid_eotfs & 4)
            p->hdr_supported = 1;
        profile_read(p);                /* reported always; used only for what the EDID lacks */
    }
    choose(p);
}

/* ---- the swap chain and the watcher ------------------------------------------- */

static BwHdrProbe s_pr;                 /* device thread's view */
static HWND s_hwnd;
static HMONITOR s_mon;
static UINT s_w, s_h;
static DXGI_FORMAT s_fmt;               /* the swap chain's back buffer now */
static int s_scrgb;                     /* its colour space is scRGB (G10, P709) */
static int s_recreate;

static CRITICAL_SECTION s_cs;
static HANDLE s_wake, s_thread;
static BwHdrProbe s_pub;                /* the watcher's newest (under s_cs) */
static volatile LONG s_seq, s_reset;
static LONG s_seen;
static HWND volatile s_whwnd;

static DWORD WINAPI watch(LPVOID arg)
{
    BwHdrProbe cur;
    (void)arg;
    EnterCriticalSection(&s_cs);
    cur = s_pub;
    LeaveCriticalSection(&s_cs);
    for (;;) {
        HWND w;
        HMONITOR m;
        UINT32 sig[4];
        int again;
        WaitForSingleObject(s_wake, 2000);
        if (InterlockedExchange(&s_reset, 0)) {
            EnterCriticalSection(&s_cs);
            cur = s_pub;
            LeaveCriticalSection(&s_cs);
        }
        if (!(w = s_whwnd))
            continue;
        m = MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST);
        again = m != cur.mon || !target_sig(&cur, sig, NULL) || memcmp(sig, cur.sig, sizeof sig);
        if (again) {
            BwHdrProbe n;
            probe(m, &n);
            if (n.s.hdr != cur.s.hdr || n.mon != cur.mon || n.s.peak != cur.s.peak || n.s.black != cur.s.black
                    || wcscmp(n.s.name, cur.s.name)) {
                EnterCriticalSection(&s_cs);
                s_pub = n;
                LeaveCriticalSection(&s_cs);
                InterlockedIncrement(&s_seq);
            }
            cur = n;
        }
    }
}

static void watch_from(HWND hwnd)
{
    static int once;
    if (!once) {
        once = 1;
        InitializeCriticalSection(&s_cs);
        s_wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    }
    EnterCriticalSection(&s_cs);
    s_pub = s_pr;
    LeaveCriticalSection(&s_cs);
    s_seen = s_seq;
    s_whwnd = hwnd;
    InterlockedExchange(&s_reset, 1);
    if (!s_thread && s_wake) {
        s_thread = CreateThread(NULL, 0, watch, NULL, 0, NULL);
        if (!s_thread)
            fprintf(stderr, "[HDR] the screen watcher could not start; the screen is checked only at start\n");
    } else if (s_wake)
        SetEvent(s_wake);
}

static const char *cs_name(DXGI_COLOR_SPACE_TYPE cs)
{
    return cs == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ? "BT.2100 PQ (HDR mode)"
         : cs == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709 ? "sRGB (SDR)"
         : cs == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709 ? "scRGB" : "other";
}

static void log_screen(const BwHdrProbe *p)
{
    const BwHdrScreen *s = &p->s;
    if (s->hdr == -2) {
        fprintf(stderr, "[HDR] screen: not found among the GPU outputs; treated as SDR\n");
        return;
    }
    fprintf(stderr, "[HDR] screen %ls \"%ls\": desktop %s; EDID %s%s%s; calibration profile %ls%s\n",
            s->name, p->friendly[0] ? p->friendly : L"?", p->dxgi_ok ? cs_name(p->d_cs) : "unknown (no IDXGIOutput6)",
            !p->edid_ok ? "not readable" : p->edid_cta_hdr ? "with a CTA-861.3 HDR block" : "without an HDR block",
            p->edid_ok && (p->edid_eotfs & 4) ? " (PQ supported)" : "",
            p->e_prim_ok || !p->edid_ok ? "" : " (its colours are implausible)",
            p->prof_name[0] ? p->prof_name : L"none read", p->prof_ok ? "" : (p->prof_name[0] ? " (unreadable)" : ""));
    fprintf(stderr, "[HDR] screen values used: peak %.0f cd/m2 (%s), black %.4f (%s), full-frame %.0f (%s);"
                    " primaries R %.4f,%.4f G %.4f,%.4f B %.4f,%.4f, white %.4f,%.4f (%s; %s BT.709, which the game"
                    " sends unstretched); %u bits per colour. DXGI's own figures: peak %.0f, black %.4f, full-frame"
                    " %.0f; EDID's: peak %.0f, black %.4f, max frame-average %.0f; profile's: peak %.0f, black %.4f\n",
            s->peak, s->peak_src, s->black, s->black_src, s->maxfall, s->maxfall_src,
            s->prim[0], s->prim[1], s->prim[2], s->prim[3], s->prim[4], s->prim[5], s->prim[6], s->prim[7],
            s->prim_src, covers_709(s->prim) ? "covers" : "doesn't fully cover", s->bits, p->d_peak, p->d_black,
            p->d_maxfall, p->e_peak, p->e_black, p->e_maxfall, p->p_peak, p->p_black);
    if (!strcmp(s->peak_src, "none"))
        fprintf(stderr, "[HDR] neither the EDID nor a calibration profile gives this screen's peak: no HDR"
                        " highlights with HDR on Auto; the SDR picture at BT.2408's 203 cd/m2 reference\n");
    if (p->prof_mhc2 && p->p_peak > 0.0f && !strcmp(s->peak_src, "edid") && p->p_peak < s->peak - 1.0f)
        fprintf(stderr, "[HDR] a Windows HDR calibration profile (%ls) is active on this screen and caps it at"
                        " %.0f cd/m2; Blood Wake maps to the screen's own %.0f cd/m2 (its EDID), so Windows clips"
                        " highlights above %.0f\n", p->prof_name, p->p_peak, s->peak, p->p_peak);
    else if (p->prof_mhc2)
        fprintf(stderr, "[HDR] a Windows HDR calibration profile (%ls) is active on this screen (peak %.0f cd/m2"
                        " in it); Blood Wake maps to the screen's %s values\n", p->prof_name, p->p_peak, s->peak_src);
}

static int env_off(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("BW_GPU_HDR");
        v = e && e[0] == '0';
        if (v)
            fprintf(stderr, "[HDR] off (BW_GPU_HDR=0): an 8-bit swap chain on every screen, as before 0.2.5\n");
    }
    return v;
}

DXGI_FORMAT bw_hdr_swap_format(HWND hwnd, DXGI_FORMAT sdr)
{
    s_hwnd = hwnd;
    s_mon = hwnd ? MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST) : NULL;
    probe(s_mon, &s_pr);
    return (s_pr.s.hdr == 1 && !env_off()) ? DXGI_FORMAT_R16G16B16A16_FLOAT : sdr;
}

int bw_hdr_window_hdr(HWND hwnd)
{
    POINT pt = { 0, 0 };
    HMONITOR m = hwnd ? MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST)
                      : MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    probe(m, &s_pr);
    return env_off() ? 0 : s_pr.s.hdr;
}

int bw_hdr_swap_created(IDXGISwapChain1 *swap)
{
    DXGI_SWAP_CHAIN_DESC1 d;
    IDXGISwapChain3 *s3 = NULL;
    const char *why = NULL;
    s_scrgb = 0;
    s_w = s_h = 0;
    if (!swap || FAILED(IDXGISwapChain1_GetDesc1(swap, &d)))
        return 0;
    s_fmt = d.Format;
    log_screen(&s_pr);
    if (d.Format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
        if (FAILED(IDXGISwapChain1_QueryInterface(swap, &IID_IDXGISwapChain3, (void **)&s3)))
            s3 = NULL;
        if (d.SwapEffect != DXGI_SWAP_EFFECT_FLIP_DISCARD && d.SwapEffect != DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL)
            why = "the window has the older (blt) swap chain, which can't take scRGB";
        else if (s3) {
            UINT sup = 0;
            if (SUCCEEDED(IDXGISwapChain3_CheckColorSpaceSupport(s3, DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709, &sup))
                    && (sup & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT)
                    && SUCCEEDED(IDXGISwapChain3_SetColorSpace1(s3, DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709)))
                s_scrgb = 1;
            else
                why = "the swap chain refused the scRGB colour space";
        } else
            why = "IDXGISwapChain3 is missing";
        if (s_scrgb)
            fprintf(stderr, "[HDR] the screen is in HDR mode: the game presents scRGB itself (linear BT.709;"
                            " Windows makes the BT.709 to BT.2020 step, BT.2087), so Windows' SDR composition"
                            " and its SDR brightness slider don't touch the picture\n");
        else {
            HRESULT hr = IDXGISwapChain1_ResizeBuffers(swap, 0, d.Width, d.Height, DXGI_FORMAT_B8G8R8A8_UNORM, d.Flags);
            if (SUCCEEDED(hr)) {
                s_fmt = DXGI_FORMAT_B8G8R8A8_UNORM;
                if (s3)
                    IDXGISwapChain3_SetColorSpace1(s3, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
            }
            fprintf(stderr, "[HDR] the screen is in HDR mode but %s: an 8-bit swap chain%s\n", why,
                    SUCCEEDED(hr) ? "" : " could not be made either; it stays as it is");
        }
        if (s3) IDXGISwapChain3_Release(s3);
    }
    if (s_hwnd)
        watch_from(s_hwnd);
    return s_scrgb;
}

void bw_hdr_follow(UINT w, UINT h)
{
    HMONITOR m;
    if (!s_hwnd)
        return;
    m = MonitorFromWindow(s_hwnd, MONITOR_DEFAULTTONEAREST);
    if ((m != s_mon || ((w != s_w || h != s_h) && s_w)) && s_wake) {
        s_mon = m;
        SetEvent(s_wake);
    }
    s_w = w;
    s_h = h;
    if (s_thread && s_seq != s_seen) {
        BwHdrProbe was = s_pr;
        DXGI_FORMAT want;
        EnterCriticalSection(&s_cs);
        s_pr = s_pub;
        s_seen = s_seq;
        LeaveCriticalSection(&s_cs);
        if (was.s.hdr != s_pr.s.hdr || was.mon != s_pr.mon || was.s.peak != s_pr.s.peak)
            log_screen(&s_pr);
        want = (s_pr.s.hdr == 1 && !env_off()) ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_B8G8R8A8_UNORM;
        if (want != s_fmt) {
            s_recreate = 1;
            fprintf(stderr, "[HDR] the screen %s HDR mode: the swap chain is made again (%s)\n",
                    s_pr.s.hdr == 1 ? "entered" : "left", want == DXGI_FORMAT_R16G16B16A16_FLOAT ? "scRGB" : "8-bit");
        }
    }
}

int bw_hdr_recreate(void)
{
    int r = s_recreate;
    s_recreate = 0;
    return r;
}

int bw_hdr_scrgb(void)
{
    return s_scrgb && s_fmt == DXGI_FORMAT_R16G16B16A16_FLOAT && s_pr.s.hdr == 1;
}

const BwHdrScreen *bw_hdr_screen(void)
{
    return &s_pr.s;
}

/* ---- the mapping --------------------------------------------------------------- */

static float env_f(const char *name, float *v)
{
    const char *e = getenv(name);
    if (e && atof(e) > 0) { *v = (float)atof(e); return 1; }
    return 0;
}

/* HDR forced on (BW_VFX_HDR=on) with neither EDID nor profile: the HDR1000
 * defaults (D-026): peak 1000 cd/m2, paper white 203 (Table 4), black
 * 0.05 and sustained full-screen 600, per VESA DisplayHDR 1000 (CTS 1.2)
 * (docs/remaster/hdr_standards.md section 9). */
static int s_forced;

void bw_hdr_set_forced(int on)
{
    s_forced = on;
}

int bw_hdr_have_data(void)
{
    return s_pr.s.peak_src && strcmp(s_pr.s.peak_src, "none");
}

static int use_hdr1000(void)
{
    return s_forced && !bw_hdr_have_data();
}

float bw_hdr_peak(const char **from)
{
    float v;
    if (env_f("BW_VFX_HDR_PEAK", &v)) { if (from) *from = "BW_VFX_HDR_PEAK"; return v; }
    if (use_hdr1000()) { if (from) *from = "HDR1000 default (HDR forced on, no EDID or profile)"; return 1000.0f; }
    if (from) *from = s_pr.s.peak_src ? s_pr.s.peak_src : "none";
    return s_pr.s.peak_src ? s_pr.s.peak : 1000.0f;
}

/* BW_VFX_HDR_BLACK=<cd/m2> overrides, like the peak and paper (also a test
 * aid: an off-screen run can use a screen's own black). */
float bw_hdr_black(const char **from)
{
    float v;
    if (getenv("BW_VFX_HDR_BLACK") && atof(getenv("BW_VFX_HDR_BLACK")) >= 0.0 && *getenv("BW_VFX_HDR_BLACK")) {
        v = (float)atof(getenv("BW_VFX_HDR_BLACK"));
        if (from) *from = "BW_VFX_HDR_BLACK";
        return v;
    }
    if (use_hdr1000()) { if (from) *from = "HDR1000 default (VESA DisplayHDR 1000, CTS 1.2)"; return 0.05f; }
    if (from) *from = s_pr.s.black_src ? s_pr.s.black_src : "default";
    return s_pr.s.black;
}

/* BT.2408-9 Table 4 by the formula it comes from (BT.2100's HLG OOTF at 75%
 * HLG): white = peak x 0.26496^(1.2 + 0.42 log10(peak/1000)), the peak held
 * to the table's 400-2000; 400 -> 101, 1000 -> 203, 2000 -> 343. */
float bw_hdr_paper(const char **from)
{
    static char buf[80];
    float v, pk;
    const char *src;
    if (env_f("BW_VFX_HDR_PAPER", &v)) { if (from) *from = "BW_VFX_HDR_PAPER"; return v; }
    pk = bw_hdr_peak(&src);             /* BW_VFX_HDR_PEAK, the screen's, or HDR1000's 1000 */
    if (!strcmp(src, "none")) {
        if (from) *from = "BT.2408's 203 (Table 4's 1000-cd/m2 row): no screen peak known";
        return 203.0f;
    }
    _snprintf(buf, sizeof buf, "BT.2408 Table 4 for a %.0f-cd/m2 screen (%s)", pk, src);
    buf[sizeof buf - 1] = 0;
    if (from) *from = buf;
    if (pk < 400.0f) pk = 400.0f;
    if (pk > 2000.0f) pk = 2000.0f;
    return pk * powf(0.26496f, 1.2f + 0.42f * log10f(pk / 1000.0f));
}

static int eotf_old(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("BW_VFX_HDR_EOTF");
        v = e && atof(e) > 2.0 && atof(e) < 2.3;
    }
    return v;
}

void bw_hdr_direct_consts(float k[4], int with_black)
{
    float paper = bw_hdr_paper(NULL), lb = with_black ? bw_hdr_black(NULL) : 0.0f, s;
    k[3] = paper / 80.0f;
    if (eotf_old()) {
        k[0] = 2.2f; k[1] = 1.0f; k[2] = 0.0f;
        return;
    }
    if (lb > paper / 10.0f) lb = 0.0f;
    s = powf(lb / paper, 1.0f / 2.4f);  /* BT.1886: a = (Lw^(1/g) - Lb^(1/g))^g, b = Lb^(1/g) / (...) */
    k[0] = 2.4f;
    k[1] = powf(1.0f - s, 2.4f);
    k[2] = s / (1.0f - s);
}

/* BT.2100 PQ inverse EOTF for Y = L / 10000. */
static float pq(float y)
{
    float p = powf(y < 0.0f ? 0.0f : y, 0.1593017578125f);
    return powf((0.8359375f + 18.8515625f * p) / (1.0f + 18.6875f * p), 78.84375f);
}

/* BT.2408-9 Annex 5 (hdr_standards.md section 8). The source range is our
 * own picture's: LB 0 and LW = the up-mapped peak (step 1 allows a known
 * range; with LW = 10,000 the knee on a 400-cd/m2 screen would start at
 * 74.5 and bring 203 down to 170, N's catch). The up-mapping rises to the
 * screen's peak, so LW = Lmax: maxLum = 1, KS = 1 and the spline never
 * engages; what the EETF adds is its black lift b(1 - E2)^4 to the screen's
 * black (Lmin). The caller clamps to Lmax afterwards (the lift overshoots
 * slightly when Lmin > 0). x b = minLum, y maxLum, z KS, w PQ(LW) - PQ(LB). */
void bw_hdr_eetf_consts(float k[4])
{
    float lw = bw_hdr_peak(NULL), lmin = bw_hdr_black(NULL), lb = pq(0.0f), span = pq(lw / 10000.0f) - lb;
    k[0] = (pq(lmin / 10000.0f) - lb) / span;
    k[1] = (pq(lw / 10000.0f) - lb) / span;
    k[2] = 1.5f * k[1] - 0.5f;
    k[3] = span;
}

void bw_hdr_log_mapping(const char *who)
{
    const char *pf, *kf, *bf;
    float paper = bw_hdr_paper(&pf), peak = bw_hdr_peak(&kf), black = bw_hdr_black(&bf);
    fprintf(stderr, "[HDR] %s: reference white (the HUD and text) %.0f cd/m2 (%s); peak %.0f cd/m2 (%s);"
                    " black %.4f cd/m2 (%s); the game's SDR picture decoded with %s\n",
            who, paper, pf, peak, kf, black, bf,
            eotf_old() ? "0.2.4's 2.2 power (BW_VFX_HDR_EOTF=2.2)" : "BT.1886 (gamma 2.4 with the screen's black)");
}

/* ---- HLSL ------------------------------------------------------------------------ */

const char bw_hdr_hlsl[] =
"static const float bwh_m1 = 0.1593017578125, bwh_m2 = 78.84375;\n"
"static const float bwh_c1 = 0.8359375, bwh_c2 = 18.8515625, bwh_c3 = 18.6875;\n"
/* BT.2100 PQ: inverse EOTF of Y = L/10000, and the EOTF */
"float3 bwh_pq(float3 y) { float3 p = pow(max(y, 0.0), bwh_m1); return pow((bwh_c1 + bwh_c2 * p) / (1.0 + bwh_c3 * p), bwh_m2); }\n"
"float3 bwh_pq_eotf(float3 e) { float3 p = pow(saturate(e), 1.0 / bwh_m2); return pow(max(p - bwh_c1, 0.0) / (bwh_c2 - bwh_c3 * p), 1.0 / bwh_m1); }\n"
/* BT.2087: linear BT.709 to BT.2020, and back */
"float3 bwh_709to2020(float3 c) { return float3(dot(c, float3(0.6274, 0.3293, 0.0433)), dot(c, float3(0.0691, 0.9195, 0.0114)), dot(c, float3(0.0164, 0.0880, 0.8956))); }\n"
"float3 bwh_2020to709(float3 c) { return float3(dot(c, float3(1.6605, -0.5876, -0.0728)), dot(c, float3(-0.1246, 1.1329, -0.0083)), dot(c, float3(-0.0182, -0.1006, 1.1187))); }\n"
/* BT.1886 relative to Lw (k from bw_hdr_direct_consts), or 0.2.4's 2.2 power */
"float3 bwh_decode(float3 c, float4 k) { return k.x > 2.3 ? k.y * pow(max(c + k.z, 0.0), 2.4) : pow(max(c, 0.0), 2.2); }\n"
/* The EETF of BT.2408-9 Annex 5 (hdr_standards.md section 8) on PQ values
 * normalised to the source range: e.x b = minLum, e.y maxLum, e.z KS */
"float3 bwh_eetf(float3 e1, float4 e) {\n"
"  float3 t = saturate((e1 - e.z) / max(1.0 - e.z, 1e-6)), t2 = t * t, t3 = t2 * t;\n"
"  float3 p = (2.0 * t3 - 3.0 * t2 + 1.0) * e.z + (t3 - 2.0 * t2 + t) * (1.0 - e.z) + (-2.0 * t3 + 3.0 * t2) * e.y;\n"
"  float3 e2 = e1 < e.z ? e1 : p;\n"
"  return e2 + e.x * pow(saturate(1.0 - e2), 4.0); }\n";

/* ---- bloodwake.exe --screen-info[=<x>,<y>] ---------------------------------------- */

static void json_w(FILE *f, const WCHAR *w)
{
    for (; *w; w++) {
        if (*w == L'"' || *w == L'\\') fprintf(f, "\\%c", (char)*w);
        else if (*w < 0x20) fprintf(f, "\\u%04x", (unsigned)*w);
        else if (*w < 0x80) fputc((char)*w, f);
        else fprintf(f, "\\u%04x", (unsigned)*w);
    }
}

int bw_hdr_screen_info(const char *arg)
{
    BwHdrProbe p;
    POINT pt = { 0, 0 };
    HMONITOR m;
    MONITORINFOEXW mi;
    const BwHdrScreen *s = &p.s;
    const char *mapping, *forced, *by = "primary";
    float paper;
    int x, y;
    if (arg && sscanf(arg, "%d,%d", &x, &y) == 2) {
        pt.x = x; pt.y = y;
        m = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
        by = "point";
    } else
        m = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    probe(m, &p);
    s_pr = p;                           /* for the paper white below */
    paper = bw_hdr_paper(NULL);
    /* what Auto does, and what On does */
    mapping = s->hdr != 1 ? "sdr" : bw_hdr_have_data() ? "hdr" : "sdr-direct";
    forced = s->hdr != 1 ? "sdr" : bw_hdr_have_data() ? "hdr" : "hdr-defaults";
    memset(&mi, 0, sizeof mi);
    mi.cbSize = sizeof mi;
    if (!GetMonitorInfoW(m, (MONITORINFO *)&mi))
        mi.szDevice[0] = 0;
    printf("{\"name\": \"");
    json_w(stdout, p.friendly[0] ? p.friendly : L"");
    printf("\", \"device\": \"");
    json_w(stdout, mi.szDevice[0] ? mi.szDevice : s->name);
    printf("\", \"chosen_by\": \"%s\", \"hdr_capable\": %s, \"hdr_on\": %s, \"source\": \"%s\",\n", by,
           p.hdr_supported ? "true" : "false", s->hdr == 1 ? "true" : "false", s->peak_src);
    printf(" \"peak\": {\"cd_m2\": %.1f, \"source\": \"%s\"},\n", s->peak, s->peak_src);
    printf(" \"min\": {\"cd_m2\": %.5f, \"source\": \"%s\"},\n", s->black, s->black_src);
    printf(" \"full_frame\": {\"cd_m2\": %.1f, \"source\": \"%s\"},\n", s->maxfall, s->maxfall_src);
    printf(" \"primaries\": {\"r\": [%.4f, %.4f], \"g\": [%.4f, %.4f], \"b\": [%.4f, %.4f], \"source\": \"%s\"},\n",
           s->prim[0], s->prim[1], s->prim[2], s->prim[3], s->prim[4], s->prim[5], s->prim_src);
    printf(" \"white_point\": {\"xy\": [%.4f, %.4f], \"source\": \"%s\"},\n", s->prim[6], s->prim[7], s->prim_src);
    printf(" \"bits\": {\"value\": %u, \"source\": \"dxgi\"},\n", s->bits);
    printf(" \"paper_white\": {\"cd_m2\": %.1f, \"source\": \"%s\"},\n", paper,
           strcmp(s->peak_src, "none") ? "bt2408_table4" : "bt2408_default_203");
    printf(" \"mapping\": \"%s\", \"mapping_if_forced_on\": \"%s\",\n", mapping, forced);
    printf(" \"dxgi\": {\"peak\": %.1f, \"min\": %.5f, \"full_frame\": %.1f},\n", p.d_peak, p.d_black, p.d_maxfall);
    printf(" \"edid\": {\"read\": %s, \"hdr_block\": %s, \"pq\": %s, \"peak\": %.1f, \"min\": %.5f, \"max_frame_average\": %.1f},\n",
           p.edid_ok ? "true" : "false", p.edid_cta_hdr ? "true" : "false", (p.edid_eotfs & 4) ? "true" : "false",
           p.e_peak, p.e_black, p.e_maxfall);
    printf(" \"covers_bt709\": %s,\n", covers_709(s->prim) ? "true" : "false");
    printf(" \"profile_active\": %s, \"profile_peak\": %.1f,\n", p.prof_mhc2 ? "true" : "false",
           p.prof_mhc2 ? p.p_peak : -1.0f);
    printf(" \"profile\": {\"name\": \"");
    json_w(stdout, p.prof_name);
    printf("\", \"read\": %s, \"mhc2\": %s, \"peak\": %.1f, \"min\": %.5f, \"full_frame\": %.1f}}\n",
           p.prof_ok ? "true" : "false", p.prof_mhc2 ? "true" : "false", p.p_peak, p.p_black, p.p_maxfall);
    fflush(stdout);
    return 0;
}
