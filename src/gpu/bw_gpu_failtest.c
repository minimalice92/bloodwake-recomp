/* bw_gpu_failtest.c - the GPU failure switch and the D3D11 debug layer's
 * messages in the log (0.2.5 item 25). See bw_gpu_failtest.h. */
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bw_gpu_failtest.h"

/* ---- the failure switch -------------------------------------------------- */

enum { K_DEVICE, K_RESOURCE, K_SURFACE, K_DEPTH, K_TEXTURE, K_POST, K_TEXRESTORE,
       K_VFXSHADER, K_PRESENT, K_SWAPCHAIN, K_N };
static const char *const s_kind_name[K_N] = {
    "device", "resource", "surface", "depth", "texture", "post", "texrestore",
    "vfxshader", "present", "swapchain"
};

#define FT_MAX_RULES 16
typedef struct { int kind; long n; int plus; volatile LONG hits; } FtRule;
static FtRule s_rule[FT_MAX_RULES];
static int s_nrule;
static volatile LONG s_count[K_N];      /* operations of each kind so far */
static volatile LONG s_init;            /* 0 not yet, 1 parsing, 2 done */

static int kind_index(const char *k, size_t len)
{
    int i;
    for (i = 0; i < K_N; i++)
        if (strlen(s_kind_name[i]) == len && !_strnicmp(k, s_kind_name[i], len))
            return i;
    return -1;
}

static void ft_parse(void)
{
    const char *e = getenv("BW_GPU_FAILTEST"), *p;
    char armed[256];
    size_t al = 0;
    if (!e || !*e)
        return;
    armed[0] = 0;
    for (p = e; *p && s_nrule < FT_MAX_RULES; ) {
        const char *item = p, *colon, *end;
        int k;
        long n = 1;
        int plus = 0;
        while (*p && *p != ',')
            p++;
        end = p;
        if (*p == ',')
            p++;
        for (colon = item; colon < end && *colon != ':'; colon++)
            ;
        k = kind_index(item, (size_t)(colon - item));
        if (colon < end) {
            char *q;
            n = strtol(colon + 1, &q, 10);
            if (q < end && *q == '+')
                plus = 1;
        }
        if (k < 0 || n < 1) {
            fprintf(stderr, "[GPU-FAILTEST] ignored \"%.*s\": kinds are device, resource, surface,"
                            " depth, texture, post, texrestore, vfxshader, present, swapchain; n from 1\n",
                    (int)(end - item), item);
            continue;
        }
        s_rule[s_nrule].kind = k;
        s_rule[s_nrule].n = n;
        s_rule[s_nrule].plus = plus;
        s_nrule++;
        if (al < sizeof armed - 32)
            al += (size_t)_snprintf(armed + al, sizeof armed - al, "%s%s:%ld%s", al ? ", " : "",
                                    s_kind_name[k], n, plus ? "+" : "");
    }
    armed[sizeof armed - 1] = 0;
    if (s_nrule)
        fprintf(stderr, "[GPU-FAILTEST] armed (test aid): %s\n", armed);
}

static void ft_init(void)
{
    if (s_init == 2)
        return;
    if (InterlockedCompareExchange(&s_init, 1, 0) == 0) {
        ft_parse();
        InterlockedExchange(&s_init, 2);
    } else {
        while (s_init != 2)
            Sleep(0);
    }
}

/* Does a rule for kind k fire at its count c? */
static int ft_rules(int k, long c, const char *asked)
{
    int i, fail = 0;
    for (i = 0; i < s_nrule; i++) {
        FtRule *r = &s_rule[i];
        if (r->kind != k || c < r->n || (!r->plus && c != r->n))
            continue;
        fail = 1;
        {
            LONG h = InterlockedIncrement(&r->hits);
            if (h <= 3)
                fprintf(stderr, "[GPU-FAILTEST] %s #%ld (%s) failed on request (BW_GPU_FAILTEST %s:%ld%s)%s\n",
                        s_kind_name[k], c, asked, s_kind_name[k], r->n, r->plus ? "+" : "",
                        r->plus && h == 3 ? "; later ones counted, not logged" : "");
        }
    }
    return fail;
}

int bw_gpu_failtest(const char *kind)
{
    int k, fail;
    if (s_init != 2)
        ft_init();
    if (!s_nrule || !kind)
        return 0;
    k = kind_index(kind, strlen(kind));
    if (k < 0)
        return 0;
    fail = ft_rules(k, InterlockedIncrement(&s_count[k]), kind);
    /* surface, depth and texture are resources too */
    if (k == K_SURFACE || k == K_DEPTH || k == K_TEXTURE)
        fail |= ft_rules(K_RESOURCE, InterlockedIncrement(&s_count[K_RESOURCE]), kind);
    return fail;
}

/* ---- the debug layer ----------------------------------------------------- */

static int s_dbg = -1;
static ID3D11InfoQueue *s_iq;

int bw_gpu_debug_level(void)
{
    if (s_dbg < 0) {
        const char *e = getenv("BW_GPU_DEBUG");
        s_dbg = e && *e >= '1' && *e <= '9' ? (*e == '1' ? 1 : 2) : 0;
    }
    return s_dbg;
}

/* Repeats: per message ID, the first few in full, the rest counted. */
#define DBG_IDS 256
#define DBG_SHOW 3
static struct { int id; unsigned long n, told; } s_seen[DBG_IDS];
static int s_nseen;
static unsigned long s_drains, s_total, s_lost;

static unsigned long *seen_count(int id, unsigned long **told)
{
    int i;
    for (i = 0; i < s_nseen; i++)
        if (s_seen[i].id == id) {
            *told = &s_seen[i].told;
            return &s_seen[i].n;
        }
    if (s_nseen == DBG_IDS)
        return NULL;
    s_seen[s_nseen].id = id;
    *told = &s_seen[s_nseen].told;
    return &s_seen[s_nseen++].n;
}

void bw_gpu_debug_attach(struct ID3D11Device *dev)
{
    ID3D11Device *d = (ID3D11Device *)dev;
    if (!d || s_iq)
        return;
    if (FAILED(ID3D11Device_QueryInterface(d, &IID_ID3D11InfoQueue, (void **)&s_iq))) {
        s_iq = NULL;
        fprintf(stderr, "[GPU-DEBUG] the device has no info queue; no debug messages\n");
        return;
    }
    ID3D11InfoQueue_SetMuteDebugOutput(s_iq, FALSE);
    ID3D11InfoQueue_SetMessageCountLimit(s_iq, 4096);
    if (bw_gpu_debug_level() < 2) {
        D3D11_MESSAGE_SEVERITY deny[2] = { D3D11_MESSAGE_SEVERITY_INFO, D3D11_MESSAGE_SEVERITY_MESSAGE };
        D3D11_INFO_QUEUE_FILTER f;
        memset(&f, 0, sizeof f);
        f.DenyList.NumSeverities = 2;
        f.DenyList.pSeverityList = deny;
        ID3D11InfoQueue_AddStorageFilterEntries(s_iq, &f);
    }
    fprintf(stderr, "[GPU-DEBUG] D3D11 debug layer on (BW_GPU_DEBUG=%d): %s copied into this log\n",
            bw_gpu_debug_level(), bw_gpu_debug_level() < 2 ? "corruption, errors and warnings"
                                                         : "every message");
    /* One message of our own through the queue, so a quiet log proves the
     * layer had nothing to say rather than that nothing was read. */
    ID3D11InfoQueue_AddApplicationMessage(s_iq, D3D11_MESSAGE_SEVERITY_WARNING,
                                          "self-test: the debug layer's messages reach this log");
}

static const char *sev_name(D3D11_MESSAGE_SEVERITY s)
{
    switch (s) {
    case D3D11_MESSAGE_SEVERITY_CORRUPTION: return "CORRUPTION";
    case D3D11_MESSAGE_SEVERITY_ERROR:      return "ERROR";
    case D3D11_MESSAGE_SEVERITY_WARNING:    return "WARNING";
    case D3D11_MESSAGE_SEVERITY_INFO:       return "INFO";
    default:                                return "MESSAGE";
    }
}

void bw_gpu_debug_drain(void)
{
    UINT64 n, i;
    if (!s_iq)
        return;
    n = ID3D11InfoQueue_GetNumStoredMessagesAllowedByRetrievalFilter(s_iq);
    {
        /* the queue's own count of messages it had no room for: add its growth */
        static UINT64 last;
        UINT64 d = ID3D11InfoQueue_GetNumMessagesDiscardedByMessageCountLimit(s_iq);
        s_lost += (unsigned long)(d >= last ? d - last : d);
        last = d;
    }
    for (i = 0; i < n; i++) {
        SIZE_T len = 0;
        D3D11_MESSAGE *m;
        unsigned long *cnt, *told = NULL;
        if (FAILED(ID3D11InfoQueue_GetMessage(s_iq, i, NULL, &len)) || !len)
            continue;
        m = (D3D11_MESSAGE *)malloc(len);
        if (!m)
            break;
        if (SUCCEEDED(ID3D11InfoQueue_GetMessage(s_iq, i, m, &len))) {
            s_total++;
            cnt = seen_count((int)m->ID, &told);
            if (cnt)
                (*cnt)++;
            if (!cnt || *cnt <= DBG_SHOW)
                fprintf(stderr, "[GPU-DEBUG] %s #%d: %.*s%s\n", sev_name(m->Severity), (int)m->ID,
                        (int)(m->DescriptionByteLength ? m->DescriptionByteLength - 1 : 0),
                        m->pDescription ? m->pDescription : "",
                        cnt && *cnt == DBG_SHOW ? " (repeats of this one are counted, not shown)" : "");
        }
        free(m);
    }
    ID3D11InfoQueue_ClearStoredMessages(s_iq);
    /* now and then, how often the repeats came */
    if (++s_drains % 900 == 0) {
        char line[512];
        size_t l = 0;
        int k;
        for (k = 0; k < s_nseen && l < sizeof line - 40; k++)
            if (s_seen[k].n > DBG_SHOW && s_seen[k].n != s_seen[k].told) {
                l += (size_t)_snprintf(line + l, sizeof line - l, " #%d x%lu", s_seen[k].id, s_seen[k].n);
                s_seen[k].told = s_seen[k].n;
            }
        line[sizeof line - 1] = 0;
        if (l || s_lost)
            fprintf(stderr, "[GPU-DEBUG] %lu messages so far; repeated:%s%s; %lu lost to the queue limit\n",
                    s_total, l ? line : "", l ? "" : " none new", s_lost);
    }
}
