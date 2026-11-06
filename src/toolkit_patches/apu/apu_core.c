/*
 * MCPX APU Core - Standalone extraction from xemu
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2018-2019 Jannik Vogel
 * Copyright (c) 2019-2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "apu_state.h"
#include "apu.h"
#include "apu_xaudio2.h"
#include "apu_wav.h"
#include "fpconv.h"
#include "../kernel/xbox_memory_layout.h"   /* XBOX_WORKER_STACK_TOP */
#include "bw_apu_hot.h"     /* Blood Wake (au-hot): BW_APU_HOTSTAT, a test aid, inert when off */

/* Blood Wake carried patch (TK-14, docs/TOOLKIT_ISSUES.md): the sequencer
 * sample-limit tick (ISTS bit 3) made switchable, and the title's sequencer
 * stops and the ticks raised are counted, so a restart loop shows in the log.
 * BW_APU_SSL_TICK=off (default)|once|periodic (the toolkit's behaviour); the
 * original file is at xboxrecomp/src/apu/apu_core.c (sha256 guarded in
 * CMakeLists.txt). Blood Wake initialises DirectSound without the tick. */
static int s_bw_tick_mode = -1;        /* 0 off, 1 once, 2 periodic */
static unsigned s_bw_ticks, s_bw_seq_stops;

static int bw_ssl_tick_allowed(void)
{
    if (s_bw_tick_mode < 0) {
        const char *e = getenv("BW_APU_SSL_TICK");
        s_bw_tick_mode = e && !strcmp(e, "periodic") ? 2
                       : e && !strcmp(e, "once") ? 1 : 0;
        fprintf(stderr, "[APU] BW sequencer tick: %s\n",
                s_bw_tick_mode == 2 ? "periodic" : s_bw_tick_mode == 1 ? "once" : "off");
    }
    if (s_bw_tick_mode == 0 || (s_bw_tick_mode == 1 && s_bw_ticks))
        return 0;
    s_bw_ticks++;
    if (s_bw_ticks <= 3 || s_bw_ticks % 200 == 0)
        fprintf(stderr, "[APU] BW sequencer tick raised (%u so far)\n", s_bw_ticks);
    return 1;
}

/* ============================================================
 * Globals
 * ============================================================ */

uint8_t *g_apu_ram_ptr = NULL;

MCPXAPUState *g_state = NULL;

/* Forward declarations for software mixer */
static void mixer_init(void);
static void mixer_render(int16_t frame_buf[][2], int num_samples);
static APUMixerVoice g_mixer_voices[APU_MIXER_MAX_VOICES];
static volatile int g_mixer_active_count = 0;
static CRITICAL_SECTION g_mixer_cs;
static bool g_mixer_initialized = false;
struct McpxApuDebug g_dbg;
struct McpxApuDebug g_dbg_cache;
int g_dbg_voice_monitor = -1;
uint64_t g_dbg_muted_voices[4] = { 0 };

/* Global audio mute — disables all AWD/mixer sound playback */
volatile int g_audio_muted = 0;  /* 0 = audio enabled */

/* ============================================================
 * Debug frame markers (minimal stubs)
 * ============================================================ */

void mcpx_debug_begin_frame(void) {}
void mcpx_debug_end_frame(void) {}

/* ============================================================
 * IRQ handling (stubbed - no PCI bus in standalone)
 * ============================================================ */

/* Physical addresses, resolved much as the other bus masters here do it
 * (dma_resolve in nv2a_pb_exec.c, bus_resolve in usb/ohci.c), but in an
 * order of the APU's own.
 *
 * DirectSound builds its voice, SGE and notifier structures in
 * MmAllocateContiguousMemory and hands the APU their physical addresses.
 * Physical P and the contiguous window's 0x80000000 + P are the same bytes on
 * hardware; here the window is separate storage. Reading every address as low
 * RAM meant the voice processor walked zeroes and wrote each "voice done"
 * notification into ordinary RAM, where DirectSound never looked -- so a
 * buffer never reported that it had stopped, and a frontend that waits on
 * IDirectSoundBuffer::GetStatus polled forever.
 *
 * Because the window is separate storage, one physical number below 64 MB can
 * name two different pages: a window page the contiguous allocator handed out
 * and a RAM page of the image or the heap. The cases below decide which one
 * the title meant. */
extern uint32_t g_xbox_image_lo, g_xbox_image_hi;
extern uint32_t xbox_ContiguousAllocatedBytes(void);
extern size_t g_xbox_total_ram;
extern size_t g_xbox_map_size;     /* host mapping; 0 means same as RAM */

/* How far up the host actually mapped guest RAM: the map size when it was
 * set apart from the RAM size (xbox_SetMapSize), else the RAM size. */
static inline uint32_t apu_mapped_ram(void)
{
    return (uint32_t)(g_xbox_map_size ? g_xbox_map_size : g_xbox_total_ram);
}

/* TK-15 (Blood Wake carried patch): resolve the image before the window. */
static int bw_apu_image_first(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("BW_APU_PHYS_ORDER");
        on = !(e && !strcmp(e, "toolkit"));
        fprintf(stderr, "[APU] BW physical order: %s\n",
                on ? "image before window (TK-15)" : "toolkit (window first)");
    }
    return on;
}

uint8_t *mcpx_apu_phys(uint64_t addr)
{
    uint32_t a = (uint32_t)addr & 0x0FFFFFFFu;
    /* Blood Wake carried patch (TK-17): a buffer page or stream segment the
     * title posted carries the copy it meant (apu_vp.c bw_tag_intent):
     * bit 29 the window, bit 28 RAM. Honoured before every rule below. */
    if (a < (64u << 20)) {
        if ((uint32_t)addr & 0x20000000u)
            return g_apu_ram_ptr + 0x80000000u + a;
        if ((uint32_t)addr & 0x10000000u)
            return g_apu_ram_ptr + a;
    }
    /* RAM above the window's 64 MB. On a 128 MB layout (a devkit) most of
     * the heap sits above 0x04000000, and so do the sound buffers a title
     * malloc()s and hands to DirectSound as plain pointers, which
     * MmGetPhysicalAddress turns into physical addresses (the identity for
     * RAM). Folding those into the low 64 MB, as the fallback below does,
     * read code as PCM: full-scale white noise on every effect, and music
     * that lost its upper half each time its ring crossed the boundary.
     * There is no window up here, so it is RAM as far as the host mapped it;
     * only above that does the address wrap. */
    if (a >= (64u << 20)) {
        if (a < apu_mapped_ram())
            return g_apu_ram_ptr + a;
        return g_apu_ram_ptr + (a & 0x03FFFFFFu);
    }
    /* A heap page the title named through MmGetPhysicalAddress is RAM, even
     * where the contiguous allocator has handed out the same number as a
     * window page. A title that loads its effect banks and music stream into
     * the heap puts those physical numbers in the voices' SGE lists; read
     * from the window, effects played whatever the window held there (white
     * noise) and the stream played zeros. Only heap pages are recorded, so
     * the voice-array case below still holds for the image. */
    if (xbox_PhysRamNameMap()[a >> 12])
        return g_apu_ram_ptr + a;
    /* A page the contiguous allocator handed out is the window, even where
     * it overlaps the image's physical range: an image whose BSS runs past
     * 48 MB can have DirectSound's voice array at a window address inside
     * that range. Resolved to RAM, the VP never saw the driver take an idle
     * voice off its list and re-trapped it every frame. (The OHCI resolves
     * the image first: a USB driver can read its descriptors into .data on a
     * page the boot has also allocated as contiguous memory.) */
    /* Blood Wake carried patch (TK-15): the image first, as the OHCI does.
     * DirectSound's keep-alive voice loops over two zero samples in its own
     * section (0x001040A8); the contiguous allocator had handed out that
     * physical page as part of a D3D frame buffer (0x80104000), so the voice
     * played screen pixels: a near-Nyquist square wave at up to -5 dBFS.
     * Blood Wake's image is small (0x00010000-0x001D0000) and nothing the
     * APU legitimately reads is a window page inside it; DirectSound's own
     * structures sit at 0x00C88000 and up. BW_APU_PHYS_ORDER=toolkit puts
     * the window first again. */
    if (bw_apu_image_first() && a >= g_xbox_image_lo && a < g_xbox_image_hi)
        return g_apu_ram_ptr + a;
    if (xbox_ContiguousPageMap()[a >> 12])
        return g_apu_ram_ptr + 0x80000000u + a;
    if (a >= g_xbox_image_lo && a < g_xbox_image_hi)
        return g_apu_ram_ptr + a;
    if (a < xbox_ContiguousAllocatedBytes())
        return g_apu_ram_ptr + 0x80000000u + a;
    return g_apu_ram_ptr + (a & 0x03FFFFFFu);
}

/* Which of the cases above resolved an address, for the RECOMP_APU_TRACE
 * per-voice line and the doorbell line: "named" (a heap page
 * MmGetPhysicalAddress named), "win" (a page the contiguous allocator handed
 * out), "img" (inside the XBE image), "hiwin" (below the arena's high-water
 * mark but not a live page), "ram" (plain RAM: below 64 MB, or above it
 * where the host mapped RAM) or "wrap" (above the mapping, folded into the
 * low 64 MB). A voice reading noise from "wrap" or "hiwin" is the address
 * resolution, not the decoder. */
const char *mcpx_apu_phys_class(uint64_t addr)
{
    uint32_t a = (uint32_t)addr & 0x0FFFFFFFu;
    if (a < (64u << 20)) {                         /* TK-17 posted intent */
        if ((uint32_t)addr & 0x20000000u)
            return "post-win";
        if ((uint32_t)addr & 0x10000000u)
            return "post-ram";
    }
    if (a >= (64u << 20))
        return a < apu_mapped_ram() ? "ram" : "wrap";
    if (xbox_PhysRamNameMap()[a >> 12])
        return "named";
    if (bw_apu_image_first() && a >= g_xbox_image_lo && a < g_xbox_image_hi)
        return "img";
    if (xbox_ContiguousPageMap()[a >> 12])
        return "win";
    if (a >= g_xbox_image_lo && a < g_xbox_image_hi)
        return "img";
    if (a < xbox_ContiguousAllocatedBytes())
        return "hiwin";
    return "ram";
}

/* A diagnostic switch is on when set to anything but empty or a leading
 * '0', as RECOMP_HEADLESS is read: =0 turns it off rather than on. */
int apu_env_on(const char *name)
{
    /* Blood Wake carried patch: cached per name. The toolkit's
     * apu_mmio_hook.c asks for RECOMP_APU_TRACE on every trapped APU
     * register access, and getenv scans the whole environment: 1.8% of the
     * game thread in mission 1, all of it under DSound_HwVoiceSet* (profile
     * with callers, run 20261027-021008-r6-gpu-d7callers). The environment
     * doesn't change while the game runs; callers pass string literals, so
     * the pointer is the key. A race between two first callers stores the
     * same answer twice. */
    enum { SLOTS = 16 };
    static const char *volatile names[SLOTS];
    static volatile int vals[SLOTS];
    int i;
    for (i = 0; i < SLOTS && names[i]; i++)
        if (names[i] == name)
            return vals[i];
    {
        const char *e = getenv(name);
        int on = e && *e && *e != '0';
        if (i < SLOTS) {
            vals[i] = on;
            names[i] = name;
        }
        return on;
    }
}

/* RECOMP_APU_TRACE register lines, read once: every MMIO access tests it. */
static int apu_mmio_trace(void)
{
    static int on = -1;
    if (on < 0)
        on = apu_env_on("RECOMP_APU_TRACE");
    return on;
}

/* The interrupt line, as the frame thread sees it. update_irq used to call
 * pci_irq_assert, which is an empty stub here, so DirectSound's service
 * routine never ran and no voice completion ever reached it. */
static volatile LONG s_irq_line;

/* FETINTSTS is not derived from FECTL here. It is latched once, where the
 * front end traps (fe_method), and an ISTS write clears it like any other
 * status bit.
 *
 * This used to OR FETINTSTS back in whenever FECTL's method mode was not
 * free running (the mask also matches HALTED). The guest's service routine
 * acks ISTS first and resumes the front end afterwards, so its own ack
 * re-latched the bit, and FETRAPREASON is left at REQUESTED after the
 * resume. The next delivery -- a frame later, or the next notification --
 * found FETINTSTS with a stale trap reason and handled FEDECMETH/FEDECPARAM a
 * second time. By then the title's threads, which run alongside the
 * interrupt here, had often released that voice: the routine looked the
 * stale voice up, got a null object and dereferenced it (a read at
 * 0xFFFFFFBE). A guest that halts the front end itself and later acks
 * anything got the same stale replay, plus an unwanted resume. It also made
 * every trap cost two or three ISR calls instead of one. */
static void update_irq(MCPXAPUState *d)
{
    if ((d->regs[NV_PAPU_IEN] & NV_PAPU_ISTS_GINTSTS) &&
        ((d->regs[NV_PAPU_ISTS] & ~NV_PAPU_ISTS_GINTSTS) &
         d->regs[NV_PAPU_IEN])) {
        qatomic_or(&d->regs[NV_PAPU_ISTS], NV_PAPU_ISTS_GINTSTS);
        InterlockedExchange(&s_irq_line, 1);
    } else {
        qatomic_and(&d->regs[NV_PAPU_ISTS], ~NV_PAPU_ISTS_GINTSTS);
        InterlockedExchange(&s_irq_line, 0);
    }
}

/* ---- delivering it ---------------------------------------------------- */

/* HalGetInterruptVector(5) -- the APU's IRQ -- is what DirectSound connects
 * its service routine to (0x002F8E81 in Burnout 3). */
#define APU_VECTOR 5

typedef void (*apu_guest_fn)(void);
extern apu_guest_fn recomp_lookup(uint32_t xbox_va);
extern int  xbox_worker_stack_alloc(void);
extern void xbox_worker_stack_free(int slot);
extern uint32_t xbox_GetConnectedInterrupt(uint32_t vector);
extern uint32_t xbox_AllocThreadTib(void);
extern int xbox_IrqlBlocksInterrupts(void);
extern int xbox_IrqlEnterInterrupt(int level);
extern void xbox_IrqlLeaveInterrupt(int saved);
extern int  xbox_DispatchGateOn(void);
extern int  xbox_DispatchGateTryEnter(void);
extern void xbox_DispatchGateLeave(void);
/* kernel/kernel.h's xbox_Irq* (the device-interrupt post), declared here as
 * the rest are: the APU does not include the kernel header. */
enum { XBOX_IRQ_APU = 1 };
enum { XBOX_IRQ_POSTED = 0, XBOX_IRQ_GATED = 1, XBOX_IRQ_UNGATED = 2 };
extern int  xbox_IrqSafePointsOn(void);
extern void xbox_IrqSetHandler(int line, void (*run)(void));
extern int  xbox_IrqPost(int line);
extern void xbox_IrqNoteUngated(int line);
#if defined(_MSC_VER)
#  define APU_TLS __declspec(thread)
#else
#  define APU_TLS __thread
#endif
extern APU_TLS uint32_t g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi;
extern APU_TLS uint32_t g_fs_base;
static APU_TLS int s_hot_ft;            /* au-hot: this is the frame thread */

/* Blood Wake carried patch (au-vp, 0.3.0): the frame's hand-off of the APU
 * lock (d->lock), without SwitchToThread.
 *
 * The frame thread holds d->lock across its whole loop and used to drop it
 * after every frame, call SwitchToThread() and take it again, so a thread
 * waiting for it could get in (below, "Let the guest in once per frame").
 * SwitchToThread gives the rest of the caller's time slice to a thread that
 * is ready on its processor, and the caller then waits for that slice. On a
 * quiet PC, where the threads share few unparked cores, the frame thread sat
 * there 5-12 ms behind the kernel timer thread's vblank DPC or the title's
 * thread, "ready, not running" at its MMCSS priority: a whole pass's slack
 * (seen, RC5 = fb0421f, Original with BW_AUDIO_SURROUND=force on WASAPI, the
 * DSP stage idle, BW_APU_HOTSTAT: 11-33 passes a 45 s run over 5 ms, all in
 * the hand-off; 4 short passes in 9 runs, each a 9.9-12.2 ms yield).
 *
 * Now every taker of d->lock other than the frame thread counts itself as
 * waiting (s_bw_lock_waiting, ours, not the critical section's internals)
 * from just before it enters the lock until it has it. The hand-off offers
 * the lock only while that count is not zero, and then waits for the taker
 * without giving its slice away: a short spin, then 0.1 ms steps on a
 * high-resolution timer, 2 ms at most, then takes the lock back (after the
 * taker is done). With nobody waiting it keeps the lock and goes on. In this
 * port the title's own threads never take d->lock (its voice locks are
 * per-voice, apu_vp.c); the takers are the APU's start and shutdown, the test
 * tone and the host mixer's play. Scheduling only; every edition, always on.
 * BW_APU_HANDOFF=yield: the SwitchToThread hand-off, as before (the undo).
 * With BW_APU_HOTSTAT (a test aid) a 5 s line counts the takers' waits and
 * the title's voice-lock waits ([APU-HOT] ... the APU lock). */
static volatile LONG s_bw_lock_waiting;
static volatile LONG64 s_bw_lk_n, s_bw_lk_us, s_bw_lk_max;   /* the takers' waits (HOTSTAT) */
static LONG64 s_bw_ho_offer, s_bw_ho_us, s_bw_ho_max, s_bw_ho_timeout;
static int s_bw_handoff = -1;            /* 1 the counted hand-off, 0 SwitchToThread */
static HANDLE s_bw_ho_timer;

static void bw_apu_lock_other(MCPXAPUState *d)
{
    int64_t t0 = g_bw_hot ? qemu_clock_get_us(QEMU_CLOCK_REALTIME) : 0;
    InterlockedIncrement(&s_bw_lock_waiting);
    qemu_mutex_lock(&d->lock);
    InterlockedDecrement(&s_bw_lock_waiting);
    if (t0) {
        LONG64 w = qemu_clock_get_us(QEMU_CLOCK_REALTIME) - t0, m;
        InterlockedIncrement64(&s_bw_lk_n);
        InterlockedAdd64(&s_bw_lk_us, w);
        while ((m = s_bw_lk_max) < w && InterlockedCompareExchange64(&s_bw_lk_max, w, m) != m)
            ;
    }
}

static int bw_handoff_mode(void)
{
    if (s_bw_handoff < 0) {
        const char *e = getenv("BW_APU_HANDOFF");
        s_bw_handoff = !(e && (e[0] == 'y' || e[0] == '0'));
        if (s_bw_handoff)
            s_bw_ho_timer = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */,
                                                   TIMER_ALL_ACCESS);
        fprintf(stderr, "[APU] BW the frame's hand-off of the APU lock: %s (BW_APU_HANDOFF)\n",
                s_bw_handoff ? (s_bw_ho_timer ? "only to a thread waiting for it, waited for on 0.1 ms"
                                                " high-resolution steps (au-vp; no SwitchToThread)"
                                              : "only to a thread waiting for it, waited for in 1 ms sleeps"
                                                " (au-vp; no SwitchToThread; no high-resolution timer)")
                              : "SwitchToThread every frame (as before)");
    }
    return s_bw_handoff;
}

static void bw_apu_handoff(MCPXAPUState *d)
{
    int64_t t0;
    int i;
    if (!bw_handoff_mode()) {
        qemu_mutex_unlock(&d->lock);
        SwitchToThread();
        qemu_mutex_lock(&d->lock);
        return;
    }
    if (!InterlockedCompareExchange(&s_bw_lock_waiting, 0, 0))
        return;                          /* nobody waits: keep it */
    t0 = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
    qemu_mutex_unlock(&d->lock);
    for (i = 0; i < 512 && InterlockedCompareExchange(&s_bw_lock_waiting, 0, 0); i++)
        YieldProcessor();                /* a taker on another processor gets in at once */
    while (InterlockedCompareExchange(&s_bw_lock_waiting, 0, 0)) {
        if (qemu_clock_get_us(QEMU_CLOCK_REALTIME) - t0 >= 2000) {
            s_bw_ho_timeout++;
            break;
        }
        if (s_bw_ho_timer) {
            LARGE_INTEGER due;
            due.QuadPart = -1000;        /* 0.1 ms */
            if (SetWaitableTimer(s_bw_ho_timer, &due, 0, NULL, NULL, FALSE))
                WaitForSingleObject(s_bw_ho_timer, 5);
        } else {
            Sleep(1);
        }
    }
    qemu_mutex_lock(&d->lock);
    {
        int64_t w = qemu_clock_get_us(QEMU_CLOCK_REALTIME) - t0;
        s_bw_ho_offer++;
        s_bw_ho_us += w;
        if (w > s_bw_ho_max)
            s_bw_ho_max = w;
    }
}

extern volatile LONG64 g_bw_vlock_n, g_bw_vlock_spun, g_bw_vlock_us, g_bw_vlock_max;   /* apu_vp.c (HOTSTAT) */

/* the 5 s line (HOTSTAT only; the frame thread) */
static void bw_handoff_stat(void)
{
    static int64_t next;
    int64_t now;
    if (!g_bw_hot)
        return;
    now = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
    if (!next)
        next = now + 5000000;
    if (now < next)
        return;
    next = now + 5000000;
    fprintf(stderr, "[APU-HOT] 5 s: the APU lock (au-vp): taken by other threads %lld times, their wait avg %.1f max %.1f us;"
                    " the hand-off %s, offered %lld times, %.1f us avg, max %.1f, timeouts %lld; the front end's voice locks"
                    " %lld, %lld had to wait, %.1f us in all, max %.1f\n",
            (long long)s_bw_lk_n, s_bw_lk_n ? (double)s_bw_lk_us / (double)s_bw_lk_n : 0.0, (double)s_bw_lk_max,
            s_bw_handoff == 1 ? "counted" : "SwitchToThread", (long long)s_bw_ho_offer,
            s_bw_ho_offer ? (double)s_bw_ho_us / (double)s_bw_ho_offer : 0.0, (double)s_bw_ho_max,
            (long long)s_bw_ho_timeout, (long long)g_bw_vlock_n, (long long)g_bw_vlock_spun,
            (double)g_bw_vlock_us, (double)g_bw_vlock_max);
    s_bw_lk_n = s_bw_lk_us = s_bw_lk_max = 0;
    s_bw_ho_offer = s_bw_ho_us = s_bw_ho_max = s_bw_ho_timeout = 0;
    g_bw_vlock_n = g_bw_vlock_spun = g_bw_vlock_us = g_bw_vlock_max = 0;
}

/* Call the connected service routine while the line is up, from the frame
 * thread, the way the OHCI model delivers USB interrupts (ohci_call_isr):
 * a worker stack for the call and a TIB of this thread's own. When a guest
 * thread or DPC holds the one-CPU gate, which is when the single-CPU console
 * could not have taken the interrupt beside it, the interrupt is posted and
 * the holder runs it at its next safe point (xbox_IrqPost). The routine
 * acknowledges by writing ISTS, which drops the line through update_irq. */

/* Push the routine's arguments on g_esp and call it at the device IRQL.
 * Returns what it returned, or -1 with nothing connected. */
static int apu_call_isr(void)
{
    uint32_t kint, routine, context;
    apu_guest_fn fn;

    kint = xbox_GetConnectedInterrupt(APU_VECTOR);
    if (!kint)
        return -1;
    routine = *(uint32_t *)(g_apu_ram_ptr + kint + 0);
    context = *(uint32_t *)(g_apu_ram_ptr + kint + 4);
    fn = routine ? recomp_lookup(routine) : NULL;
    if (!fn)
        return -1;
    g_esp -= 4; *(uint32_t *)(g_apu_ram_ptr + g_esp) = context;
    g_esp -= 4; *(uint32_t *)(g_apu_ram_ptr + g_esp) = kint;
    g_esp -= 4; *(uint32_t *)(g_apu_ram_ptr + g_esp) = 0xDEADBEEFu;
    {
        /* au-hot: the guest's routine, timed on the frame thread only */
        int64_t hot0 = g_bw_hot && s_hot_ft ? bw_hot_t() : 0;
        if (hot0)
            bw_hot_stage(BW_ST_ISR, 0);  /* au-vp: the watch */
        int _irql = xbox_IrqlEnterInterrupt(16); fn(); xbox_IrqlLeaveInterrupt(_irql);
        if (hot0) {
            bw_hot_seg(BW_HOT_ISR, bw_hot_t() - hot0);
            bw_hot_stage(BW_ST_IRQ, 0);
        }
    }
    {
        static unsigned n;
        if (n++ < 3) {
            fprintf(stderr, "[APU] interrupt delivered to 0x%08X -> %s\n",
                    routine, (g_eax & 1) ? "claimed" : "declined");
            fflush(stderr);
        }
    }
    return (int)(g_eax & 1u);
}

/* The gate holder's run of a posted APU interrupt (kernel_hal.c has put it
 * on a worker stack). The routine reads the APU registers through the MMIO
 * trap, which takes d->lock as any guest access does; the frame thread is
 * not holding it here (apu_deliver_irq drops it before posting). If the line
 * dropped since the post, the routine reads ISTS and declines, as on
 * hardware. */
static void apu_isr_on_holder(void)
{
    apu_call_isr();
}

/* BW_APU_IRQSTAT (Blood Wake carried patch): what interrupt delivery did. */
static volatile LONG s_bw_irq_held, s_bw_irq_posted, s_bw_irq_isr;

static void apu_deliver_irq(MCPXAPUState *d)
{
    /* Per thread, like the g_fs_base it guards: a new instance's frame
     * thread is a new OS thread and needs its own TIB. g_fs_base itself
     * cannot tell, as every thread starts at the main thread's TIB. */
    static APU_TLS int tib_ready;
    static unsigned held_off;
    uint32_t kint, routine;
    int slot, how;

    if (!InterlockedCompareExchange(&s_irq_line, 0, 0))
        return;
    kint = xbox_GetConnectedInterrupt(APU_VECTOR);
    if (!kint)
        return;
    routine = *(uint32_t *)(g_apu_ram_ptr + kint + 0);
    if (!routine || !recomp_lookup(routine))
        return;
    if (!tib_ready) {
        uint32_t tib = xbox_AllocThreadTib();
        if (!tib)
            return;
        g_fs_base = tib;
        tib_ready = 1;
    }

    qemu_mutex_unlock(&d->lock);
    xbox_IrqSetHandler(XBOX_IRQ_APU, apu_isr_on_holder);
    /* The one-CPU gate (xbox_DispatchGateTryEnter): with it, no thread at
     * DISPATCH_LEVEL, no DPC and no KeSynchronizeExecution routine runs
     * alongside the routine. Held: posted, and this thread goes on with its
     * frame. It must never wait for the gate: a DirectSound DPC holding it
     * can be waiting on this thread to clear a trapped FE method. The post
     * happens without d->lock, since the release of the gate it may take
     * runs other lines' routines here. */
    if (xbox_IrqSafePointsOn()) {
        how = xbox_IrqPost(XBOX_IRQ_APU);
        if (how == XBOX_IRQ_POSTED) {
            InterlockedIncrement(&s_bw_irq_posted);
            qemu_mutex_lock(&d->lock);
            return;
        }
    } else if (!xbox_DispatchGateOn()) {
        /* No gate (the default): a hold-off while a guest thread sits at
         * raised IRQL, which is when the single-CPU console could not have
         * taken the interrupt, then delivered anyway. */
        /* Blood Wake carried patch (TK-19): no hold-off by default. The
         * APU's interrupt is a device IRQL, above DISPATCH, so on the console
         * it preempts a thread sitting at DISPATCH (DirectSound's raised-IRQL
         * sections). Held off for up to 50 frames at a time, DirectSound's
         * trap handling starved once the game ran fast (GPU back end): the
         * main thread spun forever at raised IRQL in sub_000FE041 waiting for
         * a voice's idle report. BW_APU_HOLDOFF=1 restores the toolkit's
         * hold-off. */
        static int no_holdoff = -1;
        if (no_holdoff < 0)
            no_holdoff = !apu_env_on("BW_APU_HOLDOFF");
        if (!no_holdoff && xbox_IrqlBlocksInterrupts() && ++held_off <= 50) {
            InterlockedIncrement(&s_bw_irq_held);
            qemu_mutex_lock(&d->lock);
            return;
        }
        held_off = 0;
        how = XBOX_IRQ_GATED;   /* an absent gate's leave takes nothing */
    } else {
        /* RECOMP_IRQ_SAFE_POINTS=0: the bounded hold-off, then beside the
         * holder. */
        how = xbox_DispatchGateTryEnter() ? XBOX_IRQ_GATED : XBOX_IRQ_UNGATED;
        if (how == XBOX_IRQ_UNGATED && ++held_off <= 50) {
            qemu_mutex_lock(&d->lock);
            return;
        }
        held_off = 0;
    }
    if (how == XBOX_IRQ_UNGATED)
        xbox_IrqNoteUngated(XBOX_IRQ_APU);
    slot = xbox_worker_stack_alloc();
    if (slot >= 0) {
        g_esp = XBOX_WORKER_STACK_TOP(slot);
        g_eax = g_ecx = g_edx = g_ebx = g_esi = g_edi = 0;
        apu_call_isr();
        InterlockedIncrement(&s_bw_irq_isr);
        xbox_worker_stack_free(slot);
    }
    if (how == XBOX_IRQ_GATED)
        xbox_DispatchGateLeave();
    qemu_mutex_lock(&d->lock);
}

/* ============================================================
 * MMIO Read / Write
 * ============================================================ */

uint64_t mcpx_apu_read(void *opaque, hwaddr addr, unsigned int size)
{
    MCPXAPUState *d = (MCPXAPUState *)opaque;
    uint64_t r = 0;

    switch (addr) {
    case NV_PAPU_XGSCNT:
        /* The sequencer's own sample counter, reset by a title write and
         * advanced by the frame thread while the sequencer runs. It used to
         * return the host clock divided by 100, which advances far too fast
         * and in the wrong units for a title comparing it against SSL. */
        r = d->sample_count;
        break;
    default:
        if (addr < 0x20000) {
            r = qatomic_read(&d->regs[addr]);
        }
        break;
    }

    if (apu_mmio_trace())
        fprintf(stderr, "[APU] read  [0x%05llX] size=%u -> 0x%08llX\n",
                (unsigned long long)addr, size, (unsigned long long)r);
    (void)size;
    return r;
}

void mcpx_apu_write(void *opaque, hwaddr addr, uint64_t val,
                     unsigned int size)
{
    MCPXAPUState *d = (MCPXAPUState *)opaque;

    if (apu_mmio_trace())
        fprintf(stderr, "[APU] write [0x%05llX] size=%u <- 0x%08llX\n",
                (unsigned long long)addr, size, (unsigned long long)val);
    (void)size;
    {   /* D3 diagnostics: list registers, FECTL and ISTS into the event ring
         * (apu_vp.c, bwd3_apu_event), tagged 0x1xxxx with the register. */
        extern void bwd3_apu_event(MCPXAPUState *d, uint32_t what, uint32_t arg);
        if ((addr >= 0x2054 && addr <= 0x2074) || addr == NV_PAPU_FECTL
                || addr == NV_PAPU_ISTS)
            bwd3_apu_event(d, 0x10000u | (uint32_t)addr, (uint32_t)val);
    }

    switch (addr) {
    case NV_PAPU_XGSCNT:
        /* Writing XGSCNT resets the sequencer's sample counter; the value
         * the title writes (0, so far, always) becomes the new start. */
        qatomic_set(&d->regs[addr], (uint32_t)val);
        d->sample_count = val;
        break;
    case NV_PAPU_ISTS:
        qatomic_and(&d->regs[NV_PAPU_ISTS], ~(uint32_t)val);
        update_irq(d);
        qemu_cond_broadcast(&d->cond);
        break;
    case NV_PAPU_FECTL:
    case NV_PAPU_SECTL:
        if (addr == NV_PAPU_SECTL
                && (qatomic_read(&d->regs[addr]) & NV_PAPU_SECTL_XCNTMODE)
                       != NV_PAPU_SECTL_XCNTMODE_OFF
                && ((uint32_t)val & NV_PAPU_SECTL_XCNTMODE)
                       == NV_PAPU_SECTL_XCNTMODE_OFF) {
            /* TK-14 (Blood Wake carried patch): the title stopping a
             * running sequencer. */
            s_bw_seq_stops++;
            if (s_bw_seq_stops <= 3 || s_bw_seq_stops % 200 == 0)
                fprintf(stderr, "[APU] BW sequencer stopped by the title"
                                " (%u so far, %u ticks raised)\n",
                        s_bw_seq_stops, s_bw_ticks);
        }
        qatomic_set(&d->regs[addr], (uint32_t)val);
        /* Starting the APU has to start the frame thread.
         *
         * The thread idles on pause_requested, which init sets and only the
         * test tone ever cleared -- so a title that enabled the APU through
         * these registers got an APU that stayed asleep. Nothing then advanced
         * the front end, and a title waiting on a notify completion (the
         * FEMEMDATA magic write, which is how completion reaches guest memory)
         * waited forever. Wreckless hangs exactly there during DirectSound
         * init, and because it initialises its whole engine behind a
         * successful DirectSound create, that hang is not confined to audio.
         *
         * Resume whenever the write is not switching the block off; the thread
         * re-checks FECTL itself and idles again if it is halted or trapped. */
        {
            uint32_t sectl = qatomic_read(&d->regs[NV_PAPU_SECTL]);
            uint32_t fectl = qatomic_read(&d->regs[NV_PAPU_FECTL]);
            bool running =
                ((sectl & NV_PAPU_SECTL_XCNTMODE) != NV_PAPU_SECTL_XCNTMODE_OFF)
                && ((fectl & NV_PAPU_FECTL_FEMETHMODE)
                    != NV_PAPU_FECTL_FEMETHMODE_HALTED);
            if (running && d->pause_requested) {
                d->pause_requested = false;
                fprintf(stderr, "[APU] started by the title"
                                " (SECTL=%08X FECTL=%08X)\n", sectl, fectl);
            }
        }
        qemu_cond_broadcast(&d->cond);
        break;
    case NV_PAPU_FEMEMDATA:
        /* 'magic write' - value written to FEMEMADDR on notify completion */
        stl_le_phys(address_space_memory, d->regs[NV_PAPU_FEMEMADDR], (uint32_t)val);
        qatomic_set(&d->regs[addr], (uint32_t)val);
        break;
    case NV_PAPU_GPSADDR:
    case NV_PAPU_GPFADDR:
    case NV_PAPU_EPSADDR:
    case NV_PAPU_EPFADDR:
    case NV_PAPU_GPSMAXSGE:
    case NV_PAPU_GPFMAXSGE:
    case NV_PAPU_EPSMAXSGE:
    case NV_PAPU_EPFMAXSGE:
        /* D-012 (Blood Wake carried patch): with the DSPs on, their SGE
         * entries get their copy (window or RAM, TK-17) decided now, right
         * after DirectSound's translations (bw_apu_xdsp.c). */
        qatomic_set(&d->regs[addr], (uint32_t)val);
        {
            extern void bw_xdsp_sge_written(MCPXAPUState *d, uint32_t addr);
            bw_xdsp_sge_written(d, (uint32_t)addr);
        }
        break;
    default:
        if (addr < 0x20000) {
            qatomic_set(&d->regs[addr], (uint32_t)val);
        }
        break;
    }
}

/* ============================================================
 * Test tone state (used by monitor and test tone functions)
 * ============================================================ */

static struct {
    bool active;
    double phase;
    double phase_inc;
    int16_t amplitude;
} g_test_tone = { false, 0.0, 0.0, 0 };

/* ============================================================
 * Monitor - Audio output (XAudio2 primary, waveOut fallback)
 * ============================================================ */

#if defined(_WIN32)
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#endif
/* On Linux, waveOut* are inert stubs from win32_compat.h: the APU's
 * waveOut fallback path stays inactive and never produces audio. */

/* Ring of waveOut buffers for double-buffering */
#define WAVEOUT_NUM_BUFS 4
#define WAVEOUT_BUF_SAMPLES 2048  /* ~42.7ms at 48kHz, matches 8-frame delivery rate */
#define MIXER_FRAME_SAMPLES 256  /* Internal mixing frame size (matches frame_buf) */
/* monitor.queued_bytes_low/high are the pacing marks in bytes of stereo
 * s16 output; pacing compares them in samples (SAMPLE_BYTES each), so a
 * device buffer that is not a multiple of the 256-sample EP period keeps
 * exact marks. */
#define SAMPLE_BYTES (2 * (int)sizeof(int16_t))

typedef struct {
    HWAVEOUT hwo;
    WAVEHDR  hdrs[WAVEOUT_NUM_BUFS];
    int16_t  bufs[WAVEOUT_NUM_BUFS][WAVEOUT_BUF_SAMPLES][2];
    int      next_buf;
    bool     initialized;
    int      frames_written;
} WaveOutState;

static WaveOutState g_waveout = { 0 };

#include "../../bw_edition.h"
#include "../../bw_latlog.h"   /* Blood Wake (lat-m): BW_LATLOG test aid (press to sound) */
#include "bw_apu_out.h"        /* Blood Wake (au-out): MMCSS for the frame thread */

/* Blood Wake carried patch (every edition, call GAIN-1; bw_apu_gain.c): the
 * finished mix gets one fixed gain, set so the loudest peak measured across
 * the game lands just under -1 dBFS: a pure volume change, no limiting and
 * no delay (docs/audio/gain.md). It replaced 0.2.4's +4 dB with a look-ahead
 * limiter (Remastered AUDIO_MAKEUP_GAIN), which ducked big battle explosions
 * by up to 5 dB. BW_AUDIO_GAIN_DB=<dB> sets it in any edition (0 = none). */
#include "bw_apu_gain.h"

/* Blood Wake (Remastered AUDIO_SURROUND): the carried apu_xaudio2.c's source
 * voice channels, and BW_AUDIO_WAV6=<path>, a 6-channel WAV of exactly what
 * a 5.1 device is given (the header is rewritten every second).
 *
 * au-vp (0.3.0, a test aid's fix): the frame thread wrote it itself, under
 * the APU lock (seen with BW_APU_HOTSTAT's watch: 4.8 and 5.2 ms in
 * NtWriteFile < fwrite < mcpx_apu_monitor_frame). Now it copies each period
 * into a ring (8 s) and a writer thread of its own writes the same frames in
 * the same order, the header as before, and flushes each batch.
 * BW_AUDIO_WAV_ASYNC=0: written by the frame thread, as before. */
extern int xa2_output_channels(void);

static FILE *s_w6_f;
static uint32_t s_w6_frames;
#define BW_W6_RING (48000 * 8)
static int16_t *s_w6_buf;
static volatile LONG64 s_w6_w, s_w6_r;

static void apu_wav6_put(const int16_t *s, int n);

static DWORD WINAPI apu_wav6_writer(LPVOID arg)
{
    (void)arg;
    for (;;) {
        for (;;) {
            LONG64 r = s_w6_r, w = InterlockedCompareExchange64(&s_w6_w, 0, 0);
            LONG64 at = r % BW_W6_RING, n = w - r;
            if (n <= 0)
                break;
            if (n > BW_W6_RING - at)
                n = BW_W6_RING - at;
            if (n > 4800)
                n = 4800;
            apu_wav6_put(s_w6_buf + 6 * at, (int)n);
            InterlockedExchange64(&s_w6_r, r + n);
        }
        fflush(s_w6_f);
        Sleep(10);
    }
}

static void apu_wav6_write(const int16_t *s, int n)
{
    static int tried, async;
    if (!tried) {
        const char *p = getenv("BW_AUDIO_WAV6");
        const char *e = getenv("BW_AUDIO_WAV_ASYNC");
        tried = 1;
        if (p && *p)
            s_w6_f = fopen(p, "wb");
        if (s_w6_f) {
            uint8_t h[44] = { 0 };
            fwrite(h, 1, 44, s_w6_f);
            if (!(e && e[0] == '0')) {
                s_w6_buf = (int16_t *)malloc(sizeof(int16_t) * 6 * BW_W6_RING);
                async = s_w6_buf && CreateThread(NULL, 0, apu_wav6_writer, NULL, 0, NULL) != NULL;
            }
            fprintf(stderr, "[AUDIO-WAV6] %s written by %s (BW_AUDIO_WAV_ASYNC)\n", p,
                    async ? "a writer thread of its own, from an 8 s ring (au-vp; the same frames)"
                          : "the APU frame thread (as before)");
        }
    }
    if (!s_w6_f)
        return;
    if (!async) {
        apu_wav6_put(s, n);
        return;
    }
    while (n > 0) {
        LONG64 w = s_w6_w, r = InterlockedCompareExchange64(&s_w6_r, 0, 0);
        LONG64 room = BW_W6_RING - (w - r), at = w % BW_W6_RING, k = n;
        if (room <= 0) {
            Sleep(1);                    /* 8 s of disk stall: wait, as the old path did */
            continue;
        }
        if (k > room) k = room;
        if (k > BW_W6_RING - at) k = BW_W6_RING - at;
        memcpy(s_w6_buf + 6 * at, s, sizeof(int16_t) * 6 * (size_t)k);
        InterlockedExchange64(&s_w6_w, w + k);
        s += 6 * k;
        n -= (int)k;
    }
}

/* the file's part: the frames, and the header each time a second is crossed */
static void apu_wav6_put(const int16_t *s, int n)
{
    FILE *f = s_w6_f;
    uint32_t frames;
    fwrite(s, 12, (size_t)n, f);
    s_w6_frames += (uint32_t)n;
    frames = s_w6_frames;
    if (frames % 48000 < (uint32_t)n) {
        uint8_t h[44];
        uint32_t data = frames * 12, riff = 36 + data, sr = 48000, br = 48000 * 12, fl = 16;
        uint16_t pcm = 1, ch = 6, ba = 12, bps = 16;
        long pos = ftell(f);
        memcpy(h, "RIFF", 4); memcpy(h + 4, &riff, 4); memcpy(h + 8, "WAVEfmt ", 8);
        memcpy(h + 16, &fl, 4); memcpy(h + 20, &pcm, 2); memcpy(h + 22, &ch, 2);
        memcpy(h + 24, &sr, 4); memcpy(h + 28, &br, 4); memcpy(h + 32, &ba, 2);
        memcpy(h + 34, &bps, 2); memcpy(h + 36, "data", 4); memcpy(h + 40, &data, 4);
        fseek(f, 0, SEEK_SET);
        fwrite(h, 1, 44, f);
        fseek(f, pos, SEEK_SET);
        fflush(f);
    }
}

/* Blood Wake carried patch (audio robustness, every edition): the device
 * queue's target adapts to the machine. The pacing keeps the XAudio2 queue
 * between low and high (high = 4 buffers of 512 = 43 ms, low = half): a
 * busy PC that delays the frame thread, or the system audio engine, past
 * that runs the device dry (mission 10 beside parallel compiles: 400
 * starves in 70 s with the fold-down, run 20261027-023423-r6-gpu-D2-load-
 * m10-fold). Each new device underrun now raises high by one buffer (low
 * stays half of it), up to 7 buffers (75 ms); after 20 s without one it
 * comes down a buffer at a time to its floor (4 buffers, the old latency;
 * 2 in Remastered). Underruns in the first 3 s of audio (the start-up fill)
 * don't count. BW_APU_QUEUE=fixed keeps the old fixed marks. */
static int s_bq_mode = -1;                 /* 1 adaptive, 0 fixed */
static int s_bq_base, s_bq_cap, s_bq_step; /* high mark, samples */
static unsigned s_bq_last_u;
static int64_t s_bq_calm_since, s_bq_start;
static int s_bq_logged;

static int bw_queue_adaptive(void)
{
    if (s_bq_mode < 0) {
        const char *e = getenv("BW_APU_QUEUE");
        s_bq_mode = !(e && e[0] == 'f');
    }
    return s_bq_mode;
}

/* Blood Wake (au-out, carried apu_xaudio2.c): the WASAPI output's marks in
 * samples, from its engine period (floor, start, cap); 0 on XAudio2. */
extern int xa2_marks_samples(int *floor, int *start, int *cap);
extern int xa2_queue_restart(void);     /* au-fx: once, after a rebuild with no tail */
extern void *xa2_wait_event(void);
extern int64_t xa2_topup_due(int64_t now_us);
extern int64_t xa2_spread_due(int64_t now_us, int q, int high, int period);   /* au-orig: BW_WASAPI_SPREAD */
extern int xa2_wait(void *ev, int64_t wait_us);
extern int xa2_take_voice_wake(int clear_only);   /* au-lat: BW_APU_VPWAKE */

/* Blood Wake (au-orig, docs/audio/orig.md): the WASAPI output's floor. The
 * output is the port's in every edition since 0.3.0, with its own floor (one
 * engine period). In Remastered and later the launcher's "Low latency" off
 * (BW_FIX=-AUDIO_LOW_LATENCY) keeps the queue at its start mark, as before;
 * Original has no such setting. On XAudio2 AUDIO_LOW_LATENCY is unchanged
 * (bw_queue_init below: 2 buffers in Remastered, 4 in Original). */
static int bw_wa_low_floor(void)
{
    return !bw_edition_at_least(BW_ED_REMASTERED) || bw_fix_enabled(BW_FIX_AUDIO_LOW_LATENCY);
}

static void bw_queue_init(int buf_samples, int count, int *low, int *high)
{
    int l, h, floor_bufs = 4;
    const char *e = getenv("BW_APU_QUEUE_FLOOR");      /* buffers; for tests */
    if (!bw_queue_adaptive())
        return;
    {
        /* au-out: the WASAPI output's marks in samples: the floor follows the
         * device's engine period (bw_wa_low_floor: every edition; without it
         * the floor is the start), steps of one APU period */
        int fl, st, cap;
        if (xa2_marks_samples(&fl, &st, &cap)) {
            s_bq_base = bw_wa_low_floor() ? fl : st;
            /* two steps from the start down to the floor (40 s calm), as
             * lat-a's 384-sample units took; at least an APU period */
            s_bq_step = (st - s_bq_base + 1) / 2;
            if (s_bq_step < MIXER_FRAME_SAMPLES)
                s_bq_step = MIXER_FRAME_SAMPLES;
            s_bq_cap = cap;
            *high = st;
            *low = st / 2;
            fprintf(stderr, "[APU] BW device queue: adaptive, in samples (the engine's period), starts at %d"
                            " samples (%.1f ms), %d..%d (%.1f..%.1f ms), steps of %d (BW_APU_QUEUE=fixed for"
                            " the old marks)\n", st, st / 48.0, s_bq_base, s_bq_cap, s_bq_base / 48.0,
                    s_bq_cap / 48.0, s_bq_step);
            return;
        }
    }
    /* Remastered (BW_FIX AUDIO_LOW_LATENCY): the queue may come down to 2
     * buffers (21 ms). Every boot starts at 4 (43 ms), the old level: a
     * boot starting at 2 starved through its first seconds of loading (40
     * dry spells in 2.4 s, run 20261027-032136-r6-gpu-D2-ed2-bat-remastered),
     * so it steps down once the device has run 20 s without a dry spell. */
    if (e && *e)
        floor_bufs = atoi(e);
    else if (bw_fix_enabled(BW_FIX_AUDIO_LOW_LATENCY))
        floor_bufs = 2;
    if (floor_bufs < 2)
        floor_bufs = 2;
    xa2_pacing_marks(buf_samples, count < floor_bufs ? count : floor_bufs,
                     MIXER_FRAME_SAMPLES, &l, &s_bq_base);
    xa2_pacing_marks(buf_samples, count < 4 ? count : 4, MIXER_FRAME_SAMPLES, &l, &h);
    if (h < s_bq_base)
        h = s_bq_base;
    s_bq_step = buf_samples;
    s_bq_cap = buf_samples * (count - 1);
    if (s_bq_cap < h)
        s_bq_cap = h;
    *high = h;
    *low = h / 2;
    fprintf(stderr, "[APU] BW device queue: adaptive, starts at %d samples (%.1f ms), %d..%d"
                    " (%.1f..%.1f ms) (BW_APU_QUEUE=fixed for the old marks)\n",
            h, h / 48.0, s_bq_base, s_bq_cap, s_bq_base / 48.0, s_bq_cap / 48.0);
}

/* Blood Wake (au-lat, docs/audio/lat.md): the boot's margin only while the
 * game can stall. AO-5 starts the WASAPI output's queue at 1536 samples
 * (32 ms) and steps it down to the floor (one engine period) only after 40 s
 * with no short pass, against a loading stall (55 ms seen once, during a
 * mission's load, docs/audio/out.md 5.3); FX-2 does the same after a rebuild
 * with no tail. So a mission's first 40 s played with 11-22 ms more queue
 * than its steady state. With the margin on cue (the WASAPI output's sample
 * marks, every edition since 0.3.0 with that output, lane au-orig;
 * BW_WASAPI_CALM=0 keeps AO-5's timed steps):
 *  - the margin (the start mark) is held from the boot, whenever the game's
 *    steps stop for STOP (100 ms: a load, a menu, a pause, a mission's end),
 *    after a rebuild with no tail (FX-2), and when the frame thread is late:
 *    more than GAP (15 ms) between two of its looks at the queue, about a
 *    pass and a half (it looks at least once a pass, 10 ms here);
 *  - it goes once the game has run its steps for CALM (3 s) with none of
 *    those, the queue falling to its level in one go, as AO-5's steps did;
 *  - the level under it is the old adaptive queue's: the floor, a step up on
 *    each short pass, a step down after 20 s with none.
 * The game's steps come from Overlays_Update's wrapper (bw_hifps_hooks.c,
 * host-side; bw_apu_out.c). BW_WASAPI_CALM=<s>[,<gap ms>[,<stop ms>]] sets
 * the times. Transitions are logged; BW_XA2_STATS adds a line every 5 s. */
extern long long bw_audio_since_step_us(void);
static int s_cm_mode = -1;                 /* 1 on, 0 off */
static int s_cm_guard = 1, s_cm_level, s_cm_logged;
static int64_t s_cm_calm_us = 3000000, s_cm_gap_us = 15000, s_cm_stop_us = 100000;
static int64_t s_cm_last_us, s_cm_smooth_since;
static int64_t s_cm_st_t0, s_cm_st_guard_us, s_cm_st_gap_max;
static unsigned s_cm_st_rough, s_cm_st_on, s_cm_st_off;
static int s_cm_stats = -1;

static int bw_queue_calm(void)
{
    if (s_cm_mode < 0) {
        const char *e = getenv("BW_WASAPI_CALM");
        s_cm_mode = !(e && *e == '0');      /* every edition (au-orig) */
        if (s_cm_mode && e && *e) {
            double c = 0, g = 0, s = 0;
            int n = sscanf(e, "%lf,%lf,%lf", &c, &g, &s);
            if (n >= 1 && c > 0 && c < 120)
                s_cm_calm_us = (int64_t)(c * 1e6);
            if (n >= 2 && g >= 1 && g < 1000)
                s_cm_gap_us = (int64_t)(g * 1e3);
            if (n >= 3 && s >= 10 && s < 10000)
                s_cm_stop_us = (int64_t)(s * 1e3);
        }
        s_cm_stats = getenv("BW_XA2_STATS") && *getenv("BW_XA2_STATS") != '0';
    }
    return s_cm_mode;
}

static void bw_cm_set(MCPXAPUState *d, int h, int high, const char *what, unsigned u)
{
    if (h == high)
        return;
    d->monitor.queued_bytes_high = h * SAMPLE_BYTES;
    d->monitor.queued_bytes_low = h / 2 * SAMPLE_BYTES;
    if (s_cm_logged++ < 400)
        fprintf(stderr, "[APU] BW device queue %s to %d samples (%.1f ms; %s; underruns %u)\n",
                h > high ? "raised" : "lowered", h, h / 48.0, what, u);
}

/* The queue's marks with the margin on cue (above). Called where
 * bw_queue_adapt's timed steps would run, at each of the frame thread's looks
 * at the queue (each period it may make, and each wake while it waits). */
static void bw_queue_adapt_calm(MCPXAPUState *d, int64_t now_us, unsigned u, int high, int st)
{
    int64_t gap = s_cm_last_us ? now_us - s_cm_last_us : 0;
    long long since = bw_audio_since_step_us();
    int running = since >= 0 && since < s_cm_stop_us;
    int rough = gap > s_cm_gap_us;
    const char *why = NULL;
    int want;

    if (s_cm_guard && s_cm_last_us)
        s_cm_st_guard_us += gap;           /* the share of time it's held, for the 5 s line */
    s_cm_last_us = now_us;
    if (!s_bq_start) {
        s_bq_start = now_us;
        s_bq_calm_since = now_us;
        s_bq_last_u = u;
        s_cm_level = s_bq_base;
        s_cm_guard = 1;
        s_cm_smooth_since = s_cm_st_t0 = now_us;
        fprintf(stderr, "[APU] BW device queue: the margin (%d samples, %.1f ms) until the game runs its steps"
                        " smoothly for %.1f s (no frame-thread gap over %.0f ms, no stop over %.0f ms); then"
                        " %d (BW_WASAPI_CALM=0 for AO-5's timed steps)\n", st, st / 48.0, s_cm_calm_us / 1e6,
                s_cm_gap_us / 1e3, s_cm_stop_us / 1e3, s_cm_level);
    }
    if (s_cm_level < s_bq_base)
        s_cm_level = s_bq_base;             /* a new device's floor or cap */
    if (s_cm_level > s_bq_cap)
        s_cm_level = s_bq_cap;
    if (rough) {
        s_cm_st_rough++;
        if (gap > s_cm_st_gap_max)
            s_cm_st_gap_max = gap;
    }
    if (xa2_queue_restart()) {              /* FX-2 (BW_WASAPI_REBUILD_START=0: never set) */
        s_cm_smooth_since = now_us;
        if (!s_cm_guard) {
            s_cm_guard = 1;
            s_cm_st_on++;
        }
        why = "a new stream with no tail (FX-2)";
    }
    if (u != s_bq_last_u) {
        s_bq_last_u = u;
        s_bq_calm_since = now_us;
        s_cm_smooth_since = now_us;
        if (now_us - s_bq_start >= 3000000 && s_cm_level < s_bq_cap) {
            s_cm_level = s_cm_level + s_bq_step > s_bq_cap ? s_bq_cap : s_cm_level + s_bq_step;
            if (!why)
                why = "a short pass";
        }
    } else if (now_us - s_bq_calm_since > 20000000 && s_cm_level > s_bq_base) {
        s_bq_calm_since = now_us;
        s_cm_level = s_cm_level - s_bq_step < s_bq_base ? s_bq_base : s_cm_level - s_bq_step;
        if (!why)
            why = "20 s with no short pass";
    }
    if (!running || rough) {
        s_cm_smooth_since = now_us;
        if (!s_cm_guard) {
            s_cm_guard = 1;
            s_cm_st_on++;
            why = rough ? "the frame thread was late" : "the game's steps stopped";
        }
    } else if (s_cm_guard && now_us - s_cm_smooth_since >= s_cm_calm_us) {
        s_cm_guard = 0;
        s_cm_st_off++;
        why = "the game ran its steps smoothly";
    }
    want = s_cm_guard ? (s_cm_level > st ? s_cm_level : st) : s_cm_level;
    if (want != high) {
        char what[200];
        int k = snprintf(what, sizeof what, "margin %s: %s, %.1f s into the boot", s_cm_guard ? "on" : "off",
                         why ? why : "the level's own step", (now_us - s_bq_start) / 1e6);
        if (rough && k > 0 && k < (int)sizeof what)
            snprintf(what + k, sizeof what - (size_t)k, ", gap %.1f ms", gap / 1e3);
        bw_cm_set(d, want, high, what, u);
    }
    if (s_cm_stats > 0 && now_us - s_cm_st_t0 >= 5000000) {
        fprintf(stderr, "[APU-MARGIN] %.1f s: margin %s, held %.0f%% of the last %.1f s; on %u, off %u;"
                        " frame-thread gaps over %.0f ms %u (max %.1f ms); level %d\n",
                (now_us - s_bq_start) / 1e6, s_cm_guard ? "on" : "off",
                100.0 * s_cm_st_guard_us / (double)(now_us - s_cm_st_t0), (now_us - s_cm_st_t0) / 1e6,
                s_cm_st_on, s_cm_st_off, s_cm_gap_us / 1e3, s_cm_st_rough, s_cm_st_gap_max / 1e3, s_cm_level);
        s_cm_st_t0 = now_us;
        s_cm_st_guard_us = 0;
        s_cm_st_gap_max = 0;
        s_cm_st_rough = s_cm_st_on = s_cm_st_off = 0;
    }
}

static void bw_queue_adapt(MCPXAPUState *d, int64_t now_us)
{
    unsigned u = xa2_underruns();
    int high = d->monitor.queued_bytes_high / SAMPLE_BYTES, h = high;

    if (s_bq_mode <= 0 || !s_bq_step)
        return;
    {
        /* au-out: a rebuild on another device may bring another engine
         * period, and with it another floor */
        int fl, st, cap;
        if (xa2_marks_samples(&fl, &st, &cap)) {
            int base = bw_wa_low_floor() ? fl : st;
            if (base != s_bq_base || cap != s_bq_cap) {
                s_bq_base = base;
                s_bq_cap = cap;
                h = high < base ? base : high > cap ? cap : high;
                if (h != high) {
                    d->monitor.queued_bytes_high = h * SAMPLE_BYTES;
                    d->monitor.queued_bytes_low = h / 2 * SAMPLE_BYTES;
                    fprintf(stderr, "[APU] BW device queue %d..%d samples for the new device: at %d (%.1f ms)\n",
                            base, cap, h, h / 48.0);
                    high = h;
                }
            }
            /* au-lat: the margin on cue, where there is one to take away */
            if (s_bq_base < st && bw_queue_calm()) {
                bw_queue_adapt_calm(d, now_us, u, high, st);
                return;
            }
        }
    }
    {
        /* au-fx (docs/audio/fx.md): a WASAPI stream put in place with no
         * tail to carry (its device was lost) starts the queue again at the
         * boot's start mark (AO-5's 32 ms) and steps down to the floor as at
         * boot. Lane V's RC1 soak saw the first pass after such a rebuild
         * short when the queue sat at its floor (inferred cause: no margin).
         * BW_WASAPI_REBUILD_START=0 keeps the queue where it was. */
        int fl, st, cap;
        if (xa2_queue_restart() && s_bq_start && xa2_marks_samples(&fl, &st, &cap) && st > high) {
            d->monitor.queued_bytes_high = st * SAMPLE_BYTES;
            d->monitor.queued_bytes_low = st / 2 * SAMPLE_BYTES;
            s_bq_calm_since = now_us;
            s_bq_last_u = u;
            fprintf(stderr, "[APU] BW device queue back to %d samples (%.1f ms) for the new stream, from %d;"
                            " down to the floor as at boot (BW_WASAPI_REBUILD_START=0 keeps it)\n",
                    st, st / 48.0, high);
            return;
        }
    }
    if (!s_bq_start) {
        s_bq_start = now_us;
        s_bq_calm_since = now_us;
        s_bq_last_u = u;
        return;
    }
    if (u != s_bq_last_u) {
        s_bq_last_u = u;
        s_bq_calm_since = now_us;
        if (now_us - s_bq_start < 3000000)
            return;
        if (high < s_bq_cap)
            h = high + s_bq_step > s_bq_cap ? s_bq_cap : high + s_bq_step;
    } else if (now_us - s_bq_calm_since > 20000000 && high > s_bq_base) {
        s_bq_calm_since = now_us;
        h = high - s_bq_step < s_bq_base ? s_bq_base : high - s_bq_step;
    }
    if (h != high) {
        d->monitor.queued_bytes_high = h * SAMPLE_BYTES;
        d->monitor.queued_bytes_low = h / 2 * SAMPLE_BYTES;
        if (s_bq_logged++ < 40)
            fprintf(stderr, "[APU] BW device queue %s to %d samples (%.1f ms; underruns %u)\n",
                    h > high ? "raised" : "lowered", h, h / 48.0, u);
    }
}

void mcpx_apu_monitor_init(MCPXAPUState *d, Error **errp)
{
    (void)errp;
    d->monitor.stream = NULL;
    d->monitor.queued_bytes_low = 1024;
    d->monitor.queued_bytes_high = 3072;

    /* RECOMP_AUDIO_WAV=<path>: dump exactly what the device is given,
     * capped at RECOMP_AUDIO_WAV_SECS seconds when that is set. */
    {
        const char *wav = getenv("RECOMP_AUDIO_WAV");
        const char *secs = getenv("RECOMP_AUDIO_WAV_SECS");
        if (wav && *wav)
            if (apu_wav_open(wav, secs ? atof(secs) : 0.0))
                bw_wav_started();   /* au-vp: written by a thread of its own (BW_AUDIO_WAV_ASYNC) */
    }

    /* Blood Wake carried patch (audio robustness, every edition): an
     * adaptive device queue (bw_queue_adapt) needs room to grow, so the
     * device gets 8 buffers unless RECOMP_AUDIO_BUF_COUNT says otherwise;
     * the pacing starts at the same 4-buffer level as before. */
    if (bw_queue_adaptive() && !getenv("RECOMP_AUDIO_BUF_COUNT"))
        _putenv_s("RECOMP_AUDIO_BUF_COUNT", "8");

    /* Try XAudio2 first (lower latency) */
    if (xa2_init()) {
        /* Pacing marks in samples (xa2_pacing_marks): high is all the
         * device buffers, capped so a period submitted below it never
         * fills a buffer the voice has no slot for; low is half of it. */
        int low, high;
        xa2_pacing_marks(xa2_get_buffer_size(), xa2_get_buffer_count(),
                         MIXER_FRAME_SAMPLES, &low, &high);
        bw_queue_init(xa2_get_buffer_size(), xa2_get_buffer_count(), &low, &high);
        d->monitor.queued_bytes_low  = low * SAMPLE_BYTES;
        d->monitor.queued_bytes_high = high * SAMPLE_BYTES;
        fprintf(stderr, "[APU] Using %s audio backend (pacing: %d..%d samples queued)\n",
                XA2_BACKEND_NAME, low, high);
        if (high < 2 * MIXER_FRAME_SAMPLES && !xa2_wait_event())   /* au-out: event-driven tops up per pass */
            fprintf(stderr, "[APU] device buffers hold under two EP periods;"
                            " expect underruns\n");
        return;
    }
    fprintf(stderr, "[APU] %s unavailable, falling back to waveOut\n", XA2_BACKEND_NAME);

    WAVEFORMATEX wfx = { 0 };
    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = 2;
    wfx.nSamplesPerSec  = 48000;
    wfx.wBitsPerSample  = 16;
    wfx.nBlockAlign     = wfx.nChannels * wfx.wBitsPerSample / 8;
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

    MMRESULT mr = waveOutOpen(&g_waveout.hwo, WAVE_MAPPER, &wfx,
                               0, 0, CALLBACK_NULL);
    if (mr != MMSYSERR_NOERROR) {
        fprintf(stderr, "[APU] waveOutOpen failed (error %u)\n", mr);
        g_waveout.initialized = false;
        return;
    }

    /* Prepare all headers */
    for (int i = 0; i < WAVEOUT_NUM_BUFS; i++) {
        memset(&g_waveout.hdrs[i], 0, sizeof(WAVEHDR));
        g_waveout.hdrs[i].lpData = (LPSTR)g_waveout.bufs[i];
        g_waveout.hdrs[i].dwBufferLength = WAVEOUT_BUF_SAMPLES * 2 * sizeof(int16_t);
        waveOutPrepareHeader(g_waveout.hwo, &g_waveout.hdrs[i], sizeof(WAVEHDR));
    }

    g_waveout.next_buf = 0;
    g_waveout.initialized = true;
    g_waveout.frames_written = 0;

    fprintf(stderr, "[APU] waveOut audio output initialized (48kHz stereo 16-bit, %d buffers)\n",
            WAVEOUT_NUM_BUFS);
}

void mcpx_apu_monitor_finalize(MCPXAPUState *d)
{
    (void)d;
    bw_wav_close();                     /* au-vp: the writer's ring first */
    if (xa2_is_active()) {
        xa2_shutdown();
        return;
    }
    if (!g_waveout.initialized) return;

    waveOutReset(g_waveout.hwo);
    for (int i = 0; i < WAVEOUT_NUM_BUFS; i++) {
        waveOutUnprepareHeader(g_waveout.hwo, &g_waveout.hdrs[i], sizeof(WAVEHDR));
    }
    waveOutClose(g_waveout.hwo);
    g_waveout.initialized = false;
    fprintf(stderr, "[APU] waveOut audio output shut down (%d frames written)\n",
            g_waveout.frames_written);
}

/* ============================================================
 * Host playback accounting (RECOMP_AUDIO_HOST, or RECOMP_APU_TRACE)
 *
 * The WAV tap records what the APU hands the device backend, back to back,
 * so it cannot show a period that reached the device late: the device
 * plays silence meanwhile and the WAV has no hole. A clean WAV therefore
 * says nothing about whether the output was continuous; that rests on what
 * the device consumed, which this measures. At each submit the device's
 * consumed-sample count (xa2_samples_played) is set against the wall clock
 * since a baseline. The difference, the deficit, is silence the device
 * inserted, plus about one device buffer of jitter (the device takes a
 * buffer at a time, and Wine mixes in its own periods). Each time the
 * deficit passes the part already accounted for by more than that jitter,
 * the excess is starvation, logged as
 *   [AUDIO-HOST] starve wav=<sample index> ms=<length> ...
 * with the sample index into the WAV, so a check can splice that much
 * silence in and analyse what the speaker actually played. starved_ms on
 * the once-a-second line is their total, and pending_ms what is above the
 * accounted part but not yet reported (negative: inside the jitter band);
 * the inserted silence is their sum, to within one buffer. Also tracked:
 * submits the backend refused (in the WAV, never played), the longest
 * wall-clock gap between submits, and each spell with FECTL trapped
 * (frames produced: none) with its wall-clock length.
 * ============================================================ */

/* For this long after a baseline the accounted part follows the deficit
 * up, so it starts at the top of the jitter band: the device's first pull,
 * and XAudio2 staging a whole buffer before it queues one, both read as
 * deficit. A loss in it is still reported when it comes as one rise of
 * more than the jitter. */
#define STARVE_SETTLE_US 250000
/* The device's clock and the wall clock drift apart. While the device has
 * not run dry the accounted part follows the deficit up whatever the rate
 * (a device clock several percent slow, as SDL's dummy driver runs, is
 * not loss). Once it has, a loss is pending and the accounted part only
 * creeps up at this rate, so drift on a real device (tens of ppm) is not
 * added to it, and silence inserted at under 0.2 ms a second is missed.
 * The limit: on a clock more than this far off (the dummy driver) each dry
 * spell can add up to about a buffer of drift to its report, and a single
 * loss under one buffer is absorbed by the creep and never reported. */
#define STARVE_DRIFT_PPM 200

void apu_starve_rebase(ApuStarve *s, int64_t now_us, uint64_t played)
{
    s->t0_us = now_us;
    s->played0 = played;
    s->deficit = 0;
    s->ref = 0;
    s->ref_us = now_us;
    s->dry = 0;
}

int64_t apu_starve_step(ApuStarve *s, int64_t now_us, uint64_t played,
                        int jitter, int dry)
{
    int64_t deficit = (now_us - s->t0_us) * 48 / 1000
                    - (int64_t)(played - s->played0);
    double creep = (double)(now_us - s->ref_us) * 48e-3 * STARVE_DRIFT_PPM * 1e-6;

    s->deficit = deficit;
    s->ref_us = now_us;
    s->dry |= dry;
    /* Once the device has run dry, only a reported loss moves the accounted
     * part to the deficit, so the reports add up to the inserted silence
     * however it came. Moving it to every new high (as this did) left a run
     * of losses each under the jitter -- twenty 5 ms gaps -- reporting
     * nothing, while one 100 ms gap reported 100 ms. */
    if (s->dry && deficit > s->ref + jitter) {
        int64_t lost = deficit - (int64_t)s->ref;
        s->ref = (double)deficit;
        s->dry = 0;
        return lost;
    }
    if (deficit > s->ref) {
        if (!s->dry || now_us - s->t0_us < STARVE_SETTLE_US)
            s->ref = (double)deficit;
        else
            s->ref = fmin((double)deficit, s->ref + creep);
    } else if (deficit < s->ref - jitter) {
        /* A device clock running fast pulls the band down: follow it to
         * the top of the band. */
        s->ref = (double)(deficit + jitter);
    }
    return 0;
}

static struct {
    int      on;                /* -1 until the environment is read */
    ApuStarve acct;             /* t0_us 0: a new baseline at the next submit */
    uint64_t submitted;         /* samples handed to the backend (= WAV sample index) */
    unsigned refused;           /* submits the backend refused (in the WAV, not played) */
    unsigned underruns_seen;    /* xa2_underruns() at the last submit */
    double   starved_ms;        /* total, all baselines */
    unsigned starves;
    int64_t  last_submit_us, gap_max_us;    /* gap_max: this report interval */
    int      q_min;                         /* this report interval */
    int64_t  trap_start_us, trap_max_us;    /* trap_max: this report interval */
    unsigned trap_spells, trap_long;        /* long: > 20 ms */
    int64_t  next_report_us;
    int64_t  first_submit_us;               /* for the rate lines */
} g_hs = { -1 };

static int hs_on(void)
{
    if (g_hs.on < 0)
        g_hs.on = apu_env_on("RECOMP_APU_TRACE") ||
                  apu_env_on("RECOMP_AUDIO_HOST");
    return g_hs.on;
}

/* Called with the frame thread's view of FECTL each loop iteration. */
static void hs_trap(bool trapped)
{
    int64_t now;
    if (!hs_on())
        return;
    now = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
    if (trapped) {
        if (!g_hs.trap_start_us)
            g_hs.trap_start_us = now;
        return;
    }
    if (g_hs.trap_start_us) {
        int64_t dur = now - g_hs.trap_start_us;
        g_hs.trap_spells++;
        if (dur > 20000)
            g_hs.trap_long++;
        if (dur > g_hs.trap_max_us)
            g_hs.trap_max_us = dur;
        if (dur > 20000)
            fprintf(stderr, "[AUDIO-HOST] trap spell %.1f ms ending at wav=%llu\n",
                    dur / 1000.0, (unsigned long long)g_hs.submitted);
        g_hs.trap_start_us = 0;
    }
}

/* The frame thread is pausing on purpose: the device drains, which is not
 * starvation. Start a new baseline after it. */
static void hs_rebase(void)
{
    g_hs.acct.t0_us = 0;
}

/* The queue is read before the submit. Both backends count an underrun
 * only when a poll sees the queue empty, and a dry spell that begins
 * partway through a period (a front-end trap, a stall) gets no other poll
 * before this one: read after the submit, the queue holds the new period
 * and the spell is never counted, so its loss would be folded into the
 * reference. It also keeps queued= and q_min meaning what was left. */
static int hs_poll(void)
{
    return hs_on() && xa2_is_active() ? xa2_queued_samples() : 0;
}

/* After handing n samples (stereo frames) to the device; ok is what the
 * backend returned and q the queue hs_poll read just before. A refused
 * submit is in the WAV and counts in the sample index all the same, so the
 * index stays the WAV's; what was lost ends at the index on the refused
 * line (on XAudio2 the whole device buffer the period completed, on SDL2
 * the period). */
static void hs_submit(int n, int ok, int q)
{
    int64_t now, gap, lost;
    uint64_t played;
    unsigned u;

    if (!hs_on() || !xa2_is_active())
        return;
    now = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
    played = xa2_samples_played();

    /* samples= on the last-submit line is what was submitted since the
     * first, so samples / (t_last - t_first) is the rate the device was fed
     * at, pauses included. */
    if (!g_hs.first_submit_us) {
        g_hs.first_submit_us = now;
        fprintf(stderr, "[APU] first submit t=%.6f\n", now / 1e6);
    }
    if (g_hs.acct.t0_us == 0) {
        apu_starve_rebase(&g_hs.acct, now, played);
        g_hs.underruns_seen = xa2_underruns();
        g_hs.last_submit_us = now;
        g_hs.q_min = q;
        g_hs.next_report_us = now + 1000000;
    }

    gap = now - g_hs.last_submit_us;
    if (gap > g_hs.gap_max_us)
        g_hs.gap_max_us = gap;
    g_hs.last_submit_us = now;
    if (q < g_hs.q_min)
        g_hs.q_min = q;

    u = xa2_underruns();
    lost = apu_starve_step(&g_hs.acct, now, played, xa2_get_buffer_size(),
                           u != g_hs.underruns_seen);
    g_hs.underruns_seen = u;
    if (lost) {
        g_hs.starves++;
        g_hs.starved_ms += lost / 48.0;
        fprintf(stderr, "[AUDIO-HOST] starve wav=%llu ms=%.1f queued=%d gap_ms=%.1f trapped=%d\n",
                (unsigned long long)g_hs.submitted, lost / 48.0, q, gap / 1000.0,
                g_hs.trap_start_us != 0);
    }
    g_hs.submitted += (uint64_t)n;
    if (!ok && g_hs.refused++ < 100)
        fprintf(stderr, "[AUDIO-HOST] refused wav=%llu queued=%d\n",
                (unsigned long long)g_hs.submitted, q);

    if (now >= g_hs.next_report_us) {
        fprintf(stderr, "[AUDIO-HOST] wav=%llu played=%llu starves=%u starved_ms=%.1f pending_ms=%.1f"
                " deficit=%lld refused=%u gap_max_ms=%.1f q_min=%d trap_spells=%u trap_long=%u"
                " trap_max_ms=%.1f underruns=%u\n",
                (unsigned long long)g_hs.submitted, (unsigned long long)played,
                g_hs.starves, g_hs.starved_ms,
                (g_hs.acct.deficit - g_hs.acct.ref) / 48.0, (long long)g_hs.acct.deficit,
                g_hs.refused, g_hs.gap_max_us / 1000.0, g_hs.q_min,
                g_hs.trap_spells, g_hs.trap_long, g_hs.trap_max_us / 1000.0,
                xa2_underruns());
        fprintf(stderr, "[APU] last submit t=%.6f samples=%llu\n", now / 1e6,
                (unsigned long long)(g_hs.submitted - (uint64_t)n));
        g_hs.gap_max_us = 0;
        g_hs.trap_max_us = 0;
        g_hs.q_min = q;
        g_hs.next_report_us = now + 1000000;
    }
}

/* Saturating add of a 16-bit stereo block into another. */
static void mix_into(int16_t dst[][2], const int16_t src[][2], int n)
{
    for (int i = 0; i < n; i++) {
        for (int c = 0; c < 2; c++) {
            int32_t v = (int32_t)dst[i][c] + src[i][c];
            if (v > 32767)  v = 32767;
            if (v < -32768) v = -32768;
            dst[i][c] = (int16_t)v;
        }
    }
}

/* Hand one EP period (MIXER_FRAME_SAMPLES samples) to the waveOut arm. The
 * device buffers are larger than a period, so periods accumulate in the
 * current buffer and it is written when full: every sample rendered is
 * written once, none is rendered twice. */
static void waveout_submit(MCPXAPUState *d, const int16_t period[][2])
{
    static int fill;                     /* samples already in next_buf */
    int idx = g_waveout.next_buf;
    WAVEHDR *hdr = &g_waveout.hdrs[idx];

    if (fill == 0) {
        /* Wait if this buffer is still playing (with timeout) */
        int wait_loops = 0;
        while (!(hdr->dwFlags & WHDR_DONE) && (hdr->dwFlags & WHDR_INQUEUE)) {
            qemu_mutex_unlock(&d->lock);
            Sleep(1);
            qemu_mutex_lock(&d->lock);
            if (++wait_loops > 50) break;
        }
    }

    memcpy(g_waveout.bufs[idx][fill], period,
           MIXER_FRAME_SAMPLES * 2 * sizeof(int16_t));
    fill += MIXER_FRAME_SAMPLES;
    if (fill < WAVEOUT_BUF_SAMPLES)
        return;

    hdr->dwFlags &= ~WHDR_DONE;
    waveOutWrite(g_waveout.hwo, hdr, sizeof(WAVEHDR));
    g_waveout.next_buf = (idx + 1) % WAVEOUT_NUM_BUFS;
    g_waveout.frames_written++;
    fill = 0;
}

/* Called once per 32-sample frame; acts on the last frame of each EP period
 * (8 frames, 256 samples, 5.33 ms).
 *
 * frame_buf holds the period the DSP stage just wrote (mcpx_apu_dsp_frame,
 * or the VP monitor point, which accumulates). The test tone and the
 * software mixer are rendered into a scratch period and added to it, and
 * exactly that one period is handed to the device -- once.
 *
 * This used to clear frame_buf before rendering the tone and the mixer into
 * it, so whatever the VP and DSP produced never reached the device, and it
 * rendered a whole device buffer (1024 samples) per 256-sample period. */
void mcpx_apu_monitor_frame(MCPXAPUState *d)
{
    int16_t period[MIXER_FRAME_SAMPLES][2];
    int16_t extra[MIXER_FRAME_SAMPLES][2];

    if ((d->ep_frame_div + 1) % 8) {
        return;
    }

    memcpy(period, d->monitor.frame_buf, sizeof(period));
    /* The VP monitor point accumulates into frame_buf, so the next period
     * has to start from silence. */
    memset(d->monitor.frame_buf, 0, sizeof(d->monitor.frame_buf));

    if (g_audio_muted) {
        memset(period, 0, sizeof(period));
    } else {
        memset(extra, 0, sizeof(extra));
        if (g_test_tone.active) {
            for (int i = 0; i < MIXER_FRAME_SAMPLES; i++) {
                int16_t s = (int16_t)(sin(g_test_tone.phase) * g_test_tone.amplitude);
                extra[i][0] = s;
                extra[i][1] = s;
                g_test_tone.phase += g_test_tone.phase_inc;
                if (g_test_tone.phase >= 2.0 * M_PI)
                    g_test_tone.phase -= 2.0 * M_PI;
            }
        }
        mixer_render(extra, MIXER_FRAME_SAMPLES);
        mix_into(period, (const int16_t (*)[2])extra, MIXER_FRAME_SAMPLES);
        /* Blood Wake (GAIN-1): the clean-level gain, and BW_AUDIO_PEAKLOG and
         * BW_AUDIO_WAV_PRE (test aids) before it */
        bw_gain_stage2(&period[0][0], MIXER_FRAME_SAMPLES, !(xa2_is_active() && d->pace_stalled));
    }

    /* A period dropped while pacing is stalled never reaches the device,
     * so it stays out of the dump too. */
    if (!(xa2_is_active() && d->pace_stalled))
        bw_wav_write((const int16_t *)period, MIXER_FRAME_SAMPLES);   /* au-vp: off the frame thread */
    if (g_bw_lat_on) {
        /* Blood Wake carried patch (lat-m, test aid): each period's hand-over
         * against the WAV's sample index, with the device queue then (samples
         * still to play before it) and the samples the device has played, so
         * a sound found in the WAV gets its hand-over time and an estimated
         * start of playback (the device's own delay after that is unknown). */
        static int64_t s_lat_wav_n;
        int in_wav = !(xa2_is_active() && d->pace_stalled);
        bw_lat_ev(BW_LAT_AUDIO, bw_lat_now(), s_lat_wav_n, xa2_is_active() ? xa2_queued_samples() : -1,
                  xa2_is_active() ? (int64_t)xa2_samples_played() : -1, in_wav, MIXER_FRAME_SAMPLES,
                  d->ep_frame_div, 0.0, 0.0);   /* f: lat-a, the frame counter (V/W/Y events) */
        if (in_wav)
            s_lat_wav_n += MIXER_FRAME_SAMPLES;
    }

    if (xa2_is_active()) {
        if (!d->pace_stalled) {
            int q = hs_poll();
            const int16_t *out = (const int16_t *)period;
            int16_t p6[MIXER_FRAME_SAMPLES][6];
            if (xa2_output_channels() == 6) {
                /* Blood Wake (Remastered AUDIO_SURROUND): the device gets the
                 * 5.1 period the fold-down made beside frame_buf; the stereo
                 * period above stays the WAV dump's (and the meters'). */
                extern int16_t g_bw_frame6[256][6];
                memcpy(p6, g_bw_frame6, sizeof p6);
                memset(g_bw_frame6, 0, sizeof p6);
                if (g_audio_muted)
                    memset(p6, 0, sizeof p6);
                else
                    bw_gain_stage6(&p6[0][0], MIXER_FRAME_SAMPLES);
                apu_wav6_write(&p6[0][0], MIXER_FRAME_SAMPLES);
                out = &p6[0][0];
            }
            {
                int64_t hot0 = g_bw_hot ? bw_hot_t() : 0;   /* au-hot: the device write */
                if (hot0) {
                    bw_hot_begin(BW_HOT_SUB, hot0);
                    bw_hot_stage(BW_ST_WRITE, 0);   /* au-vp: the watch */
                }
                hs_submit(MIXER_FRAME_SAMPLES,
                          xa2_submit_samples(out, MIXER_FRAME_SAMPLES), q);
                if (hot0) {
                    bw_hot_end(BW_HOT_SUB, bw_hot_t());
                    bw_hot_submit();
                    bw_hot_stage(BW_ST_MON, 0);
                }
            }
        } else {
            /* Blood Wake carried patch (0.2.5 item 105): a stalled queue
             * submits nothing, so a lost device is looked after from here
             * (xa2_poll_stalled in the carried apu_xaudio2.c) */
            extern void xa2_poll_stalled(int stalled_ms);
            xa2_poll_stalled((int)((qemu_clock_get_us(QEMU_CLOCK_REALTIME) - d->pace_drain_us) / 1000));
        }
        return;
    }
    if (g_waveout.initialized)
        waveout_submit(d, (const int16_t (*)[2])period);
}

/* ============================================================
 * Throttle (timing control for frame pacing)
 * ============================================================ */

/* How long the device queue may sit at high without falling before the
 * device counts as stopped. A consuming device takes a buffer (10.7 ms at
 * the default 512 samples) well inside this; the largest buffers the hosts
 * accept (4096 samples, 85 ms) still fit. */
#define PACE_STALL_US 200000

/* BW_APU_PACESTAT counters (see bw_pacestat_tick). */
typedef struct {
    int on;
    int64_t t0;
    long periods, restarts, waits, timeouts;
    int64_t late_us, late_max, yield_us, yield_max, work_max;
    int q_min;
    int64_t q_sum;
    long q_n;
} BwPaceStat;
static BwPaceStat s_ps = { -1 };

/* One pacing decision, taken at an EP-period boundary. Returns 0 to render
 * the next period now, or how many microseconds to wait before asking again.
 *
 * Without a queue to read (waveOut, or no device), the host clock paces one
 * period per EP_FRAME_US, as before. With XAudio2 the device queue paces:
 *
 *   queued >= high   wait one period (the device has enough; the clock is
 *                    not advanced, so a device that pauses briefly stops
 *                    rendering at high and nothing is dropped)
 *   queued <= low    render ahead of the clock, but no further ahead of it
 *                    than the queue can hold (high samples), so a device
 *                    that stops consuming cannot run the APU ahead of guest
 *                    time. (Bounding it by one device buffer instead holds
 *                    the queue under any low mark larger than a buffer: the
 *                    clock then admits one period per period and the queue
 *                    never climbs.)
 *   otherwise        one period per EP_FRAME_US of host clock, counted
 *                    from now rather than from a schedule a render-ahead
 *                    pushed forward
 *
 * A device that stops consuming for good (an XAudio2 critical error or a
 * removed device, a suspended PipeWire sink, a paused SDL device) would hold
 * the queue at high forever, and with it the APU clock: no frames, no voice
 * notifications, and every guest wait on them hangs. When the queue has not
 * fallen for PACE_STALL_US the host clock paces instead and the periods are
 * dropped rather than queued (pace_stalled, read by monitor_frame); the
 * first fall of the queue hands pacing back to it.
 *
 * The period's credit is charged by mcpx_apu_pace_rendered, once the period
 * has been rendered, not here: a pass that renders nothing (the front end
 * trapped) asks again a millisecond later, and charging each of those asks
 * let a trap that began on a period boundary eat the queue's lead.
 *
 * A clock that has fallen more than a period behind (after a stall, or the
 * first call) restarts from now instead of rendering the backlog in a burst. */
int64_t mcpx_apu_pace_step(MCPXAPUState *d, int64_t now_us)
{
    int64_t ahead_us = 0;
    int mid_band = 0;

    if (d->ep_frame_div % 8)
        return 0;

    if (xa2_is_active()) {
        int q    = xa2_queued_samples();
        bw_queue_adapt(d, now_us);
        int low  = d->monitor.queued_bytes_low / SAMPLE_BYTES;
        int high = d->monitor.queued_bytes_high / SAMPLE_BYTES;

        if (q < high || q < d->pace_q_seen || d->pace_drain_us == 0) {
            d->pace_drain_us = now_us;
            if (d->pace_stalled) {
                d->pace_stalled = false;
                fprintf(stderr, "[APU] audio device consuming again: the"
                                " queue paces the APU\n");
            }
        }
        d->pace_q_seen = q;
        if (!d->pace_stalled && q >= high &&
            now_us - d->pace_drain_us >= PACE_STALL_US) {
            static int shown;
            d->pace_stalled = true;
            if (!shown++)
                fprintf(stderr, "[APU] audio device stopped consuming (%d"
                                " samples queued for %d ms): the host clock"
                                " paces the APU and periods are dropped\n",
                        q, PACE_STALL_US / 1000);
        }

        if (d->pace_stalled)
            ;                              /* host clock, as without a queue */
        else if (q >= high) {
            /* au-lat (BW_APU_VPWAKE, off by default): a voice that just
             * started gets its period now rather than at the next pass */
            if (!xa2_take_voice_wake(0))
                return EP_FRAME_US;
            return 0;
        }
        else if (xa2_wait_event()) {
            /* Blood Wake (au-out): an event-driven WASAPI stream. Its engine
             * reads one period per pass and the throttle wakes on each pass,
             * so the passes are the clock: below high, top the stream up now,
             * right after the pass, which leaves the most time before the
             * next one (or, with BW_WASAPI_LEAD, that long before the next
             * pass). Never ahead of the device's own consumption. */
            int64_t due = xa2_topup_due(now_us);
            /* au-orig (call ORIG-4, BW_WASAPI_SPREAD): with no lead, the
             * period that completes the pass's need waits for its steady
             * place in the pass (the carried apu_xaudio2.c) */
            if (due <= now_us)
                due = xa2_spread_due(now_us, q, high, MIXER_FRAME_SAMPLES);
            if (due > now_us && !xa2_take_voice_wake(0))
                return due - now_us;
            xa2_take_voice_wake(1);        /* au-lat: this period picks it up */
            return 0;
        }
        else if (q <= low)
            ahead_us = (int64_t)high * EP_FRAME_US / MIXER_FRAME_SAMPLES;
        else
            mid_band = 1;
    } else {
        d->pace_stalled = false;
    }

    if (d->next_frame_time_us == 0 ||
        now_us - d->next_frame_time_us > EP_FRAME_US) {
        if (d->next_frame_time_us && s_ps.on > 0)
            s_ps.restarts++;
        d->next_frame_time_us = now_us;
    }

    /* In the band the lead a render-ahead built is already in the queue;
     * keeping it in the schedule as well would idle until the queue had
     * drained it. One period per period from here holds the level. */
    if (mid_band && d->next_frame_time_us - now_us > EP_FRAME_US)
        d->next_frame_time_us = now_us + EP_FRAME_US;

    if (d->next_frame_time_us - now_us > ahead_us)
        return d->next_frame_time_us - now_us - ahead_us;
    if (s_ps.on > 0 && xa2_is_active()) {
        int q = d->pace_q_seen;
        if (q < s_ps.q_min)
            s_ps.q_min = q;
        s_ps.q_sum += q;
        s_ps.q_n++;
    }
    return 0;
}

/* A period that pace_step admitted has been rendered: advance the clock. */
void mcpx_apu_pace_rendered(MCPXAPUState *d)
{
    d->next_frame_time_us += EP_FRAME_US;
}

/* Blood Wake carried patch (audio robustness): BW_APU_PACESTAT=1 prints,
 * every 5 s of wall clock, how the frame thread kept time: periods rendered
 * against the wall clock's count, clock restarts (pace_step finding the
 * thread more than a period behind), the timed waits and how late they woke
 * (from the time asked for, so the ms rounding of the wait counts too), the
 * per-frame lock hand-off (unlock, yield, relock), the longest pass of work,
 * and the device queue at each period it admitted. */

static int bw_pacestat_on(void)
{
    if (s_ps.on < 0) {
        s_ps.on = apu_env_on("BW_APU_PACESTAT");
        s_ps.q_min = 1 << 30;
    }
    return s_ps.on;
}

static void bw_pacestat_tick(int64_t now_us)
{
    if (!s_ps.t0)
        s_ps.t0 = now_us;
    if (now_us - s_ps.t0 < 5000000)
        return;
    fprintf(stderr, "[APU-PACE] %.1f s: periods %ld (wall %.0f), restarts %ld; waits %ld"
                    " (timed out %ld), late avg %.2f max %.2f ms; lock hand-off %.1f ms"
                    " (max %.2f); longest pass %.2f ms; queue min %d avg %.0f samples;"
                    " underruns %u\n",
            (now_us - s_ps.t0) / 1e6, s_ps.periods, (now_us - s_ps.t0) / (double)EP_FRAME_US,
            s_ps.restarts, s_ps.waits, s_ps.timeouts,
            s_ps.timeouts ? s_ps.late_us / 1000.0 / s_ps.timeouts : 0.0, s_ps.late_max / 1000.0,
            s_ps.yield_us / 1000.0, s_ps.yield_max / 1000.0, s_ps.work_max / 1000.0,
            s_ps.q_n ? s_ps.q_min : -1, s_ps.q_n ? (double)s_ps.q_sum / s_ps.q_n : -1.0,
            xa2_underruns());
    memset(&s_ps, 0, sizeof s_ps);
    s_ps.on = 1;
    s_ps.t0 = now_us;
    s_ps.q_min = 1 << 30;
}

static void throttle(MCPXAPUState *d)
{
    int64_t start_us = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
    int64_t hot_mark = g_bw_hot ? bw_hot_t() : 0;   /* au-hot: throttle's own work */

    while (!d->pause_requested) {
        int64_t now_us = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
        int64_t wait_us = mcpx_apu_pace_step(d, now_us);
        void *ev;
        if (wait_us <= 0)
            break;
        if ((ev = xa2_wait_event()) != NULL && !d->pace_stalled) {
            /* Blood Wake (au-out): an event-driven WASAPI stream: wait for
             * the engine's next pass (it sets the event), at most as long as
             * the timer would have. A pause request waits that long at most
             * (it comes at shutdown); the timed-out loop re-checks as before. */
            int got;
            int64_t hot0 = g_bw_hot ? bw_hot_t() : 0, hot1 = 0;   /* au-hot */
            if (hot0) {
                bw_hot_seg(BW_HOT_PACE, hot0 - hot_mark);
                bw_hot_stage(BW_ST_WAIT, wait_us);   /* au-vp: the watch */
            }
            qemu_mutex_unlock(&d->lock);
            got = xa2_wait(ev, wait_us);
            if (hot0) {
                hot1 = bw_hot_t();
                bw_hot_stage(BW_ST_RELOCK, 0);
            }
            qemu_mutex_lock(&d->lock);
            if (hot0) {
                hot_mark = bw_hot_t();
                bw_hot_wake(hot1, hot_mark, hot1 - hot0, got);
                bw_hot_stage(BW_ST_PACE, 0);
            }
            if (bw_pacestat_on()) {
                s_ps.waits++;
                if (!got)
                    s_ps.timeouts++;
            }
            continue;
        }
        if (g_bw_hot)
            bw_hot_stage(BW_ST_TIMEDWAIT, (wait_us + 999) / 1000 * 1000);   /* au-vp: the watch */
        if (bw_pacestat_on()) {
            BOOL ok = SleepConditionVariableCS(&d->cond.cv, &d->lock.cs,
                                               (DWORD)((wait_us + 999) / 1000));
            s_ps.waits++;
            if (!ok) {
                int64_t late = qemu_clock_get_us(QEMU_CLOCK_REALTIME) - (now_us + wait_us);
                s_ps.timeouts++;
                s_ps.late_us += late;
                if (late > s_ps.late_max)
                    s_ps.late_max = late;
            }
        } else {
            qemu_cond_timedwait(&d->cond, &d->lock,
                                (int)((wait_us + 999) / 1000));
        }
        if (g_bw_hot) {
            hot_mark = bw_hot_t();      /* au-hot: a timed wait isn't work */
            bw_hot_stage(BW_ST_PACE, 0);
        }
    }

    if (g_bw_hot)
        bw_hot_seg(BW_HOT_PACE, bw_hot_t() - hot_mark);
    d->sleep_acc_us += (int)(qemu_clock_get_us(QEMU_CLOCK_REALTIME) - start_us);
}

/* ============================================================
 * se_frame - Process one audio frame (VP -> GP -> EP pipeline)
 * ============================================================ */

static void se_frame(MCPXAPUState *d)
{
    mcpx_apu_update_dsp_preference(d);
    mcpx_debug_begin_frame();
    g_dbg.gp_realtime = d->gp.realtime;
    g_dbg.ep_realtime = d->ep.realtime;

    int64_t now_ms = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    int64_t elapsed_ms = now_ms - d->frame_count_time_ms;
    if (elapsed_ms >= 1000) {
        g_dbg.utilization = 1.0f - d->sleep_acc_us / (elapsed_ms * 1000.0f);
        g_dbg.frames_processed = (int)(d->frame_count * 1000.0 / elapsed_ms + 0.5);
        d->frame_count_time_ms = now_ms;
        d->frame_count = 0;
        d->sleep_acc_us = 0;
    }
    d->frame_count++;
    apu_irq_stats.se_frames++;

    /* Buffer for all mixbins for this frame */
    float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME];
    memset(mixbins, 0, sizeof(mixbins));

    if (g_bw_hot) {
        /* au-hot (BW_APU_HOTSTAT, a test aid): the same calls, timed */
        int64_t t0 = bw_hot_t(), t1, t2;
        bw_hot_stage(BW_ST_VP, 0);       /* au-vp: the watch */
        if (g_bw_vp_ftz) {
            unsigned m = bw_hot_ftz_enter();   /* BW_APU_VP_FTZ: an experiment only */
            mcpx_apu_vp_frame(d, mixbins);
            bw_hot_ftz_leave(m);
        } else {
            mcpx_apu_vp_frame(d, mixbins);
        }
        t1 = bw_hot_t();
        bw_hot_seg(BW_HOT_VP, t1 - t0);
        bw_hot_stage(BW_ST_DSP, 0);
        mcpx_apu_dsp_frame(d, mixbins);
        t2 = bw_hot_t();
        bw_hot_seg(BW_HOT_DSPF, t2 - t1);
        bw_hot_stage(BW_ST_MON, 0);
        bw_hot_begin(BW_HOT_MON, t2);
        mcpx_apu_monitor_frame(d);
        bw_hot_end(BW_HOT_MON, bw_hot_t());
        bw_hot_stage(BW_ST_LOOP, 0);
    } else {
    mcpx_apu_vp_frame(d, mixbins);
    mcpx_apu_dsp_frame(d, mixbins);
    mcpx_apu_monitor_frame(d);
    }

    d->ep_frame_div++;

    mcpx_debug_end_frame();
}

/* ============================================================
 * APU frame thread (background processing)
 * ============================================================ */

ApuIrqStats apu_irq_stats;

/* Every 5 s under RECOMP_APU_TRACE: front-end traps, the running count of
 * VP/DSP frames (se_frame), the frames that found FECTL trapped, and the
 * device underruns. Trapped frames climbing while se_frames stands still is
 * the service routine not clearing the trap; underruns with se_frames
 * advancing is the host, not the title. */
static int s_irq_trace = -1;
static uint64_t s_irq_next_ns;

extern int xbox_IrqlRaisedCount(void);

/* BW_APU_IRQSTAT=1: every 2 s, the front end's trap state, the interrupt
 * registers and line, and what delivery did since the last line. Reads
 * registers only, so it does not slow the title the way RECOMP_APU_TRACE's
 * per-access lines do (which hid a timing race, 2026-10-26). */
static void bw_irq_stat(MCPXAPUState *d)
{
    static int on = -1;
    static uint64_t next;
    static unsigned last_traps, last_ftf, last_se;
    uint64_t now;
    if (on < 0)
        on = apu_env_on("BW_APU_IRQSTAT");
    if (!on)
        return;
    now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (now < next)
        return;
    next = now + 2000000000ull;
    fprintf(stderr, "[APU-IRQSTAT] FECTL %08X ISTS %08X IEN %08X line %ld FEDEC %08X/%08X"
                    " | traps +%u fe-trapped-frames +%u se_frames +%u | delivered %ld"
                    " held %ld posted %ld | irql-raised %d\n",
            qatomic_read(&d->regs[NV_PAPU_FECTL]), qatomic_read(&d->regs[NV_PAPU_ISTS]),
            qatomic_read(&d->regs[NV_PAPU_IEN]), (long)InterlockedCompareExchange(&s_irq_line, 0, 0),
            qatomic_read(&d->regs[NV_PAPU_FEDECMETH]), qatomic_read(&d->regs[NV_PAPU_FEDECPARAM]),
            apu_irq_stats.traps - last_traps, apu_irq_stats.fe_trapped_frames - last_ftf,
            apu_irq_stats.se_frames - last_se,
            (long)InterlockedExchange(&s_bw_irq_isr, 0), (long)InterlockedExchange(&s_bw_irq_held, 0),
            (long)InterlockedExchange(&s_bw_irq_posted, 0), xbox_IrqlRaisedCount());
    last_traps = apu_irq_stats.traps;
    last_ftf = apu_irq_stats.fe_trapped_frames;
    last_se = apu_irq_stats.se_frames;
}

static void apu_irq_stats_line(void)
{
    uint64_t now;

    if (s_irq_trace < 0)
        s_irq_trace = apu_env_on("RECOMP_APU_TRACE");
    if (!s_irq_trace)
        return;
    now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (now < s_irq_next_ns)
        return;
    if (s_irq_next_ns)
        fprintf(stderr, "[APU-IRQ] traps %u se_frames %u fe-trapped-frames %u underruns %u\n",
                apu_irq_stats.traps, apu_irq_stats.se_frames,
                apu_irq_stats.fe_trapped_frames, xa2_underruns());
    s_irq_next_ns = now + 5000000000ull;
}

/* Blood Wake carried patch (D-012): the frame thread is the audio's real-time
 * thread, and with the DSPs on (bw_apu_xdsp.c) it is busy for about half of
 * each period. At normal priority a burst of other work on the machine (a
 * parallel compile in the measurements) delayed it past the device queue:
 * 19 device starves (262 ms) in a 100 s battle against 0 with the
 * fold-down. BW_APU_THREAD_PRIORITY = normal | above | highest (default) |
 * critical. */
static void bw_apu_thread_priority(void)
{
    const char *e = getenv("BW_APU_THREAD_PRIORITY");
    int p = THREAD_PRIORITY_HIGHEST;
    const char *name = "highest";
    if (e && *e) {
        if (e[0] == 'n') {
            p = THREAD_PRIORITY_NORMAL;
            name = "normal";
        } else if (e[0] == 'a') {
            p = THREAD_PRIORITY_ABOVE_NORMAL;
            name = "above normal";
        } else if (e[0] == 'c' || e[0] == 't') {
            p = THREAD_PRIORITY_TIME_CRITICAL;
            name = "time critical";
        }
    }
    if (p != THREAD_PRIORITY_NORMAL && !SetThreadPriority(GetCurrentThread(), p))
        name = "normal (SetThreadPriority failed)";
    fprintf(stderr, "[APU] BW frame thread priority: %s (BW_APU_THREAD_PRIORITY)\n", name);
    bw_audio_thread_mmcss("APU frame");  /* Blood Wake (au-out): Remastered and later, MMCSS */
}

static void *mcpx_apu_frame_thread(void *arg)
{
    MCPXAPUState *d = MCPX_APU_DEVICE(arg);
    bw_apu_thread_priority();
    bw_hot_init();                      /* au-hot: BW_APU_HOTSTAT (a test aid; off: nothing) */
    bw_hot_set_lock(&d->lock.cs);       /* au-vp: the watch names the lock's owner */
    s_hot_ft = 1;
    qemu_mutex_lock(&d->lock);

    while (!qatomic_read(&d->exiting)) {
        if (d->pause_requested && !g_test_tone.active && !g_mixer_active_count) {
            d->is_idle = true;
            if (xa2_is_active())
                xa2_expect_drain();     /* a pause, not an underrun */
            hs_rebase();
            qemu_cond_signal(&d->idle_cond);
            if (g_bw_hot)
                bw_hot_stage(BW_ST_IDLE, 0);   /* au-vp: the watch */
            qemu_cond_wait(&d->cond, &d->lock);
            if (g_bw_hot)
                bw_hot_stage(BW_ST_LOOP, 0);
            d->is_idle = false;
            continue;
        }

        /* Always run the audio output loop — the software mixer and test tone
         * need continuous frame delivery regardless of APU register state.
         * The VP/DSP pipeline (se_frame) only runs when registers allow it. */
        throttle(d);
        int64_t bw_t_work = bw_pacestat_on() ? qemu_clock_get_us(QEMU_CLOCK_REALTIME) : 0;
        int64_t hot_f0 = g_bw_hot ? bw_hot_t() : 0;    /* au-hot: this frame's work */
        int div0 = d->ep_frame_div;
        if (hot_f0) {
            bw_hot_frame_begin(hot_f0);
            bw_hot_stage(BW_ST_ACK, 0);  /* au-vp: the watch */
        }

        /* The doorbell ack stands in for the GP DSP, which on hardware runs
         * whatever the front end is doing. Tying it to se_frame stopped it
         * whenever FECTL was trapped or halted, and DirectSound then waits
         * forever to post its next command: Burnout 3 stalls in
         * sub_002F805E polling the same doorbell it was acked on at init. */
        mcpx_apu_dsp_ack_poll(d);
        if (hot_f0)
            bw_hot_seg(BW_HOT_ACK, bw_hot_t() - hot_f0);

        int xcntmode = GET_MASK(qatomic_read(&d->regs[NV_PAPU_SECTL]),
                                NV_PAPU_SECTL_XCNTMODE);
        uint32_t fectl = qatomic_read(&d->regs[NV_PAPU_FECTL]);
        bool apu_active = (xcntmode != NV_PAPU_SECTL_XCNTMODE_OFF) &&
                          !(fectl & NV_PAPU_FECTL_FEMETHMODE_TRAPPED) &&
                          !(fectl & NV_PAPU_FECTL_FEMETHMODE_HALTED);

        /* A bit test, so HALTED (0x80, a subset of TRAPPED's 0xE0) counts
         * too: xemu's frame thread idles in both modes. */
        bool fe_trapped = (xcntmode != NV_PAPU_SECTL_XCNTMODE_OFF) &&
                          (fectl & NV_PAPU_FECTL_FEMETHMODE_TRAPPED) &&
                          !g_test_tone.active;

        hs_trap(fe_trapped);

        if (apu_active && !g_test_tone.active) {
            /* Full pipeline: VP voices → DSP → monitor → waveOut */
            se_frame(d);
        } else if (fe_trapped) {
            /* The title has the APU on, but the front end is trapped (a voice
             * went idle under FETFORCE1) and stays so until DirectSound's
             * service routine clears FECTL, or the title halted it. No frame
             * is produced meanwhile, as in xemu, which waits and re-checks:
             * the device queue holds what is already paced into it. The
             * software mixer pauses with it; the test tone keeps the branch
             * below. This used to fall through to
             * the lightweight branch, which advances the frame counter and
             * hands the device a period with this frame's 32 samples silent:
             * one hole per trapped frame, four or five in a row when the
             * routine cleared the trap from a DPC (32-160 samples of digital
             * zero in streamed music every few seconds). The interrupt is
             * delivered below; the wait is after it. */
            apu_irq_stats.fe_trapped_frames++;
        } else {
            /* Lightweight: just monitor frame (test tone + software mixer) */
            if (hot_f0)
                bw_hot_begin(BW_HOT_MON, bw_hot_t());
            mcpx_apu_monitor_frame(d);
            if (hot_f0)
                bw_hot_end(BW_HOT_MON, bw_hot_t());
            d->ep_frame_div++;
        }

        if (div0 % 8 == 0 && d->ep_frame_div != div0) {
            mcpx_apu_pace_rendered(d);
            s_ps.periods++;
        }

        /* The SSL tick: the sequencer's periodic sample-limit interrupt.
         *
         * DirectSound (Black, XDK 5849) resets XGSCNT to 0, programs SSL
         * (0x800 = 2048 samples) and enables ISTS bit 3 in IEN. Its service
         * routine -- dispatched on that bit, kicking the GP doorbell and
         * reposting front-end commands -- is what lets the rest of
         * DirectSound initialisation finish. Without the tick the title
         * re-runs the sequencer setup forever and never gets past audio. */
        {
            uint32_t sectl = qatomic_read(&d->regs[NV_PAPU_SECTL]);
            uint32_t ssl = qatomic_read(&d->regs[NV_PAPU_SSL]);
            d->sample_count += NUM_SAMPLES_PER_FRAME;
            if (ssl
                    && d->sample_count >= ssl
                    && (sectl & NV_PAPU_SECTL_XCNTMODE)
                           != NV_PAPU_SECTL_XCNTMODE_OFF) {
                d->sample_count -= ssl;
                /* Blood Wake carried patch (TK-14): Blood Wake's DirectSound
                 * answers bit 3 with a full hardware restart (0x000FC6CC ->
                 * 0x00102C98(0,0) + 0x00102630(1)), which stops the sequencer
                 * and zeroes XGSCNT, so a tick every SSL samples restarts the
                 * APU ~23 times a second and each restart leaves 256-768
                 * samples of silence in the output. BW_APU_SSL_TICK picks the
                 * tick: off (default), once (first crossing only), periodic
                 * (the toolkit's). */
                if (bw_ssl_tick_allowed()) {
                    qatomic_or(&d->regs[NV_PAPU_ISTS], NV_PAPU_ISTS_SSLINTSTS);
                    d->set_irq = true;
                }
            }
        }

        /* What xemu's frame thread does after each frame: turn a pending
         * notification (set by the voice processor or a trapped method) into
         * the interrupt line. Nothing here ever did, so the line never rose
         * even before there was anything to deliver it to. */
        if (d->set_irq) {
            update_irq(d);
            d->set_irq = false;
        }
        if (hot_f0) {
            int64_t h = bw_hot_t();      /* au-hot: the delivery, the guest's routine in it */
            bw_hot_stage(BW_ST_IRQ, 0);  /* au-vp: the watch */
            apu_deliver_irq(d);
            bw_hot_seg(BW_HOT_IRQ, bw_hot_t() - h);
            bw_hot_stage(BW_ST_LOOP, 0);
        } else {
        apu_deliver_irq(d);
        }
        apu_irq_stats_line();
        bw_irq_stat(d);

        /* Let the guest in once per frame.
         *
         * The thread holds d->lock for its whole loop and only drops it inside
         * throttle()'s wait. Once voices really play, processing can run
         * behind real time, throttle never waits, and the lock is never
         * released -- while every VOICE_ON/OFF/RELEASE the title writes needs
         * it (voice_lock). A critical section is not fair, so the title's
         * thread starved there indefinitely: Burnout 3 froze on its vehicle
         * select, blocked in voice_lock at raised IRQL, which in turn held
         * off every USB interrupt. */
        if (bw_t_work) {
            int64_t t = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
            if (t - bw_t_work > s_ps.work_max)
                s_ps.work_max = t - bw_t_work;
            bw_t_work = t;
        }
        {
            int64_t hot_y = hot_f0 ? bw_hot_t() : 0;   /* au-hot: the hand-off */
            if (hot_f0)
                bw_hot_stage(BW_ST_YIELD, 0);   /* au-vp: the watch */
        bw_apu_handoff(d);              /* au-vp: only to a waiting thread, no SwitchToThread */
            if (hot_f0)
                bw_hot_stage(BW_ST_YRELOCK, 0);
            if (hot_f0) {
                int64_t t = bw_hot_t();
                bw_hot_seg(BW_HOT_YLD, t - hot_y);
                bw_hot_frame(hot_f0, t);
                bw_hot_stage(BW_ST_LOOP, 0);
            }
        }
        bw_handoff_stat();              /* au-vp: HOTSTAT's 5 s line of the lock */
        if (bw_t_work) {
            int64_t t = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
            s_ps.yield_us += t - bw_t_work;
            if (t - bw_t_work > s_ps.yield_max)
                s_ps.yield_max = t - bw_t_work;
            bw_pacestat_tick(t);
        }
        if (fe_trapped &&
            (qatomic_read(&d->regs[NV_PAPU_FECTL]) &
             NV_PAPU_FECTL_FEMETHMODE_TRAPPED)) {
            /* Still trapped after the delivery (held off at raised IRQL, or
             * cleared from a DPC): give the title a millisecond with the lock
             * dropped rather than spinning on it. */
            if (g_bw_hot)
                bw_hot_stage(BW_ST_TRAPWAIT, 1000);   /* au-vp: the watch */
            qemu_cond_timedwait(&d->cond, &d->lock, 1);
            if (g_bw_hot)
                bw_hot_stage(BW_ST_LOOP, 0);
        }
    }

    qemu_mutex_unlock(&d->lock);
    return NULL;
}

/* ============================================================
 * Wait for idle / resume helpers
 * ============================================================ */

static void mcpx_apu_wait_for_idle(MCPXAPUState *d)
{
    d->pause_requested = true;
    qemu_cond_signal(&d->cond);
    while (!d->is_idle) {
        qemu_cond_wait(&d->idle_cond, &d->lock);
    }
}

static void mcpx_apu_resume(MCPXAPUState *d)
{
    d->pause_requested = false;
    qemu_cond_signal(&d->cond);
}

/* ============================================================
 * Reset
 * ============================================================ */

static void mcpx_apu_reset_locked(MCPXAPUState *d)
{
    memset(d->regs, 0, sizeof(d->regs));
    mcpx_apu_vp_reset(d);

    if (d->gp.dsp) {
        memset((void *)d->gp.dsp->core.pram_opcache, 0,
               sizeof(d->gp.dsp->core.pram_opcache));
    }
    if (d->ep.dsp) {
        memset((void *)d->ep.dsp->core.pram_opcache, 0,
               sizeof(d->ep.dsp->core.pram_opcache));
    }
    d->set_irq = false;
}

/* ============================================================
 * Public API: Init / Shutdown
 * ============================================================ */

/* The diagnostics' state is per instance: a new instance after a shutdown
 * reads the switches again and counts from zero, with no baseline or trap
 * spell left over from the last one. */
static void apu_diag_reset(void)
{
    memset(&g_hs, 0, sizeof(g_hs));
    g_hs.on = -1;
    memset(&apu_irq_stats, 0, sizeof(apu_irq_stats));
    s_irq_trace = -1;
    s_irq_next_ns = 0;
}

MCPXAPUState *mcpx_apu_init_standalone(uint8_t *ram_ptr)
{
    MCPXAPUState *d = (MCPXAPUState *)calloc(1, sizeof(MCPXAPUState));
    if (!d) {
        fprintf(stderr, "[APU] Failed to allocate MCPXAPUState\n");
        return NULL;
    }

    g_apu_ram_ptr = ram_ptr;
    g_state = d;
    d->ram_ptr = ram_ptr;
    /* A line left up by a previous instance would call the guest ISR
     * against this one's empty ISTS. */
    InterlockedExchange(&s_irq_line, 0);
    apu_diag_reset();

    /* RECOMP_APU_SOLO=<voice>: mute every other voice in the mix, for
     * telling which voice a sound (or a noise) comes from. */
    {
        const char *solo = getenv("RECOMP_APU_SOLO");
        if (solo && *solo) {
            g_dbg_voice_monitor = atoi(solo);
            fprintf(stderr, "[APU] solo: only voice %d is mixed\n",
                    g_dbg_voice_monitor);
        }
    }

    d->set_irq = false;
    d->exiting = false;
    d->is_idle = false;
    d->pause_requested = true;

    qemu_mutex_init(&d->lock);
    bw_apu_lock_other(d);               /* au-vp: a taker other than the frame thread */
    qemu_cond_init(&d->cond);
    qemu_cond_init(&d->idle_cond);

    /* Init VP (voice processor) */
    mcpx_apu_vp_init(d);

    /* Init DSP (GP/EP - stubbed) */
    mcpx_apu_dsp_init(d);

    /* Init software mixer for DirectSound bridge */
    mixer_init();

    /* Init monitor (waveOut output) */
    Error *local_err = NULL;
    mcpx_apu_monitor_init(d, &local_err);
    if (local_err) {
        warn_reportf_err(local_err, "mcpx_apu_monitor_init failed: ");
    }

    /* Start background frame thread */
    qemu_thread_create(&d->apu_thread, "mcpx.apu_thread",
                       mcpx_apu_frame_thread, d, QEMU_THREAD_JOINABLE);
    mcpx_apu_wait_for_idle(d);
    qemu_mutex_unlock(&d->lock);

    fprintf(stderr, "[APU] MCPX APU initialized (standalone)\n");
    fprintf(stderr, "[APU]   RAM pointer: %p\n", (void *)ram_ptr);
    fprintf(stderr, "[APU]   MMIO base: 0xFE800000 (512KB)\n");
    fprintf(stderr, "[APU]   VP: %d max voices, %d samples/frame\n",
            MCPX_HW_MAX_VOICES, NUM_SAMPLES_PER_FRAME);
    return d;
}

void mcpx_apu_shutdown(MCPXAPUState *d)
{
    if (!d) return;

    fprintf(stderr, "[APU] Shutting down MCPX APU...\n");

    bw_apu_lock_other(d);               /* au-vp: a taker other than the frame thread */
    mcpx_apu_wait_for_idle(d);
    qatomic_set(&d->exiting, true);
    qemu_cond_signal(&d->cond);
    qemu_mutex_unlock(&d->lock);

    /* Everything below is freed only once the frame thread has returned.
     * g_state is cleared before the teardown, so a mixer start that begins
     * after this point finds no instance; a play already in flight is not
     * serialised against shutdown. */
    qemu_thread_join(&d->apu_thread);
    g_state = NULL;
    mcpx_apu_vp_finalize(d);
    mcpx_apu_dsp_finalize(d);
    mcpx_apu_monitor_finalize(d);

    qemu_cond_destroy(&d->idle_cond);
    qemu_cond_destroy(&d->cond);
    qemu_mutex_destroy(&d->lock);
    free(d);
    fprintf(stderr, "[APU] Shutdown complete\n");
}

/* ============================================================
 * VP MMIO handlers (sub-region at +0x20000)
 *
 * These are called when the game writes to the VP PIO registers
 * to configure voices, SSL, etc.
 * ============================================================ */

uint64_t mcpx_apu_vp_read(void *opaque, hwaddr addr, unsigned int size);
void mcpx_apu_vp_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size);

/* Dispatch a VP-region access (offset 0x20000-0x2FFFF from APU base) */
void mcpx_apu_dispatch_mmio(MCPXAPUState *d, hwaddr addr, uint64_t val,
                             unsigned int size, bool is_write)
{
    if (addr >= 0x20000 && addr < 0x30000) {
        /* VP region */
        hwaddr vp_addr = addr - 0x20000;
        if (is_write) {
            mcpx_apu_vp_write(d, vp_addr, val, size);
        }
        /* VP reads handled by caller if needed */
    } else if (addr < 0x20000) {
        /* Main APU registers */
        if (is_write) {
            mcpx_apu_write(d, addr, val, size);
        }
    } else if (addr == 0x30000 + NV_PAPU_GPRST) {
        if (is_write)
            mcpx_apu_dsp_gprst_write((uint32_t)val);
    }
    /* The rest of the GP (0x30000) and EP (0x50000) regions is ignored */
}

/* ============================================================
 * Public MMIO API (called from VEH or MMIO hook)
 * addr is offset from APU base (0xFE800000)
 * ============================================================ */

uint64_t mcpx_apu_mmio_read(MCPXAPUState *d, uint64_t addr, unsigned int size)
{
    if (!d) return 0;
    if (addr >= 0x20000 && addr < 0x30000) {
        return mcpx_apu_vp_read(d, addr - 0x20000, size);
    } else if (addr < 0x20000) {
        return mcpx_apu_read(d, (hwaddr)addr, size);
    }
    return 0;
}

void mcpx_apu_mmio_write(MCPXAPUState *d, uint64_t addr, uint64_t val, unsigned int size)
{
    if (!d) return;
    mcpx_apu_dispatch_mmio(d, (hwaddr)addr, val, size, true);
}

/* ============================================================
 * APU Test Tone - Direct waveOut sine generator
 *
 * Bypasses the VP pipeline entirely and writes a 440Hz sine wave
 * directly to the monitor frame_buf. This verifies that waveOut
 * output works correctly.
 * ============================================================ */

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void mcpx_apu_play_test_tone(MCPXAPUState *d)
{
    if (!d) {
        fprintf(stderr, "[APU-TEST] No APU state\n");
        return;
    }

    if (g_test_tone.active) {
        /* Toggle off */
        g_test_tone.active = false;
        fprintf(stderr, "[APU-TEST] Test tone OFF\n");
        return;
    }

    /* 440Hz at 48kHz sample rate */
    g_test_tone.phase = 0.0;
    g_test_tone.phase_inc = 2.0 * M_PI * 440.0 / 48000.0;
    g_test_tone.amplitude = 6000;  /* ~18% of full scale */
    g_test_tone.active = true;

    /* Make sure waveOut is running - enable SECTL and resume APU thread */
    bw_apu_lock_other(d);               /* au-vp: a taker other than the frame thread */
    d->regs[NV_PAPU_SECTL] = NV_PAPU_SECTL_XCNTMODE & ~NV_PAPU_SECTL_XCNTMODE_OFF;
    d->regs[NV_PAPU_FECTL] = NV_PAPU_FECTL_FEMETHMODE_FREE_RUNNING;
    /* Initialize empty voice lists so VP frame doesn't crash */
    d->regs[NV_PAPU_TVL2D] = 0xFFFF;
    d->regs[NV_PAPU_TVL3D] = 0xFFFF;
    d->regs[NV_PAPU_TVLMP] = 0xFFFF;
    mcpx_apu_resume(d);
    qemu_mutex_unlock(&d->lock);

    fprintf(stderr, "[APU-TEST] Test tone ON - 440Hz sine, amplitude=%d\n",
            g_test_tone.amplitude);
}

/* ============================================================
 * Software mixer - mixes DirectSound buffers to waveOut
 *
 * This bypasses the VP hardware voice pipeline entirely.
 * DirectSound buffers register PCM data here, and the APU
 * frame thread mixes them into the monitor frame_buf.
 * ============================================================ */

static void mixer_init(void)
{
    if (g_mixer_initialized) return;
    InitializeCriticalSection(&g_mixer_cs);
    memset(g_mixer_voices, 0, sizeof(g_mixer_voices));
    g_mixer_initialized = true;
}

int apu_mixer_alloc_voice(void)
{
    if (!g_mixer_initialized) mixer_init();
    EnterCriticalSection(&g_mixer_cs);
    for (int i = 0; i < APU_MIXER_MAX_VOICES; i++) {
        if (!g_mixer_voices[i].active && !g_mixer_voices[i].pcm_data) {
            g_mixer_voices[i].volume = 1.0f;
            g_mixer_voices[i].sample_rate = 44100;
            g_mixer_voices[i].num_channels = 2;
            LeaveCriticalSection(&g_mixer_cs);
            return i;
        }
    }
    LeaveCriticalSection(&g_mixer_cs);
    return -1;
}

void apu_mixer_free_voice(int slot)
{
    if (slot < 0 || slot >= APU_MIXER_MAX_VOICES) return;
    EnterCriticalSection(&g_mixer_cs);
    apu_mixer_stop(slot);
    g_mixer_voices[slot].pcm_data = NULL;
    g_mixer_voices[slot].pcm_bytes = 0;
    g_mixer_voices[slot].play_offset = 0;
    LeaveCriticalSection(&g_mixer_cs);
}

APUMixerVoice *apu_mixer_get_voice(int slot)
{
    if (slot < 0 || slot >= APU_MIXER_MAX_VOICES) return NULL;
    return &g_mixer_voices[slot];
}

int apu_mixer_set_position(int slot, uint32_t byte_offset)
{
    if (slot < 0 || slot >= APU_MIXER_MAX_VOICES) return 0;
    EnterCriticalSection(&g_mixer_cs);
    APUMixerVoice *v = &g_mixer_voices[slot];
    uint32_t frame_bytes = v->num_channels * sizeof(int16_t);
    int valid = (v->num_channels == 1 || v->num_channels == 2) &&
                byte_offset < v->pcm_bytes &&
                byte_offset / frame_bytes < v->pcm_bytes / frame_bytes;
    if (valid) v->play_offset = ((uint64_t)(byte_offset / frame_bytes)) << 16;
    LeaveCriticalSection(&g_mixer_cs);
    return valid;
}

void apu_mixer_get_state(int slot, uint32_t *byte_offset, int *active, int *looping)
{
    if (byte_offset) *byte_offset = 0;
    if (active) *active = 0;
    if (looping) *looping = 0;
    if (slot < 0 || slot >= APU_MIXER_MAX_VOICES) return;
    EnterCriticalSection(&g_mixer_cs);
    APUMixerVoice *v = &g_mixer_voices[slot];
    if (byte_offset) *byte_offset = (uint32_t)(v->play_offset >> 16) *
                                    v->num_channels * sizeof(int16_t);
    if (active) *active = v->active;
    if (looping) *looping = v->active && v->looping;
    LeaveCriticalSection(&g_mixer_cs);
}

void apu_mixer_play(int slot, int looping)
{
    if (g_audio_muted) return;
    if (slot < 0 || slot >= APU_MIXER_MAX_VOICES) return;
    EnterCriticalSection(&g_mixer_cs);
    APUMixerVoice *v = &g_mixer_voices[slot];
    if (!v->pcm_data || (v->num_channels != 1 && v->num_channels != 2) ||
        v->pcm_bytes < v->num_channels * sizeof(int16_t)) {
        LeaveCriticalSection(&g_mixer_cs);
        return;
    }
    v->looping = looping;
    if (!v->active) InterlockedIncrement((volatile LONG *)&g_mixer_active_count);
    v->active = 1;

    static int play_log_count = 0;
    if (play_log_count < 20) {
        fprintf(stderr, "[APU-MIX] Play voice %d: %u bytes, %u ch, %u Hz, vol=%.2f, loop=%d\n",
                slot, v->pcm_bytes, v->num_channels, v->sample_rate, v->volume, looping);
        play_log_count++;
    }
    LeaveCriticalSection(&g_mixer_cs);

    /* The frame thread takes the APU lock before the mixer lock. */
    extern MCPXAPUState *g_state;
    if (g_state) {
        bw_apu_lock_other(g_state);     /* au-vp: a taker other than the frame thread */
        g_state->pause_requested = false;
        qemu_cond_signal(&g_state->cond);
        qemu_mutex_unlock(&g_state->lock);
    }
}

void apu_mixer_stop(int slot)
{
    if (slot < 0 || slot >= APU_MIXER_MAX_VOICES) return;
    EnterCriticalSection(&g_mixer_cs);
    if (g_mixer_voices[slot].active) {
        g_mixer_voices[slot].active = 0;
        InterlockedDecrement((volatile LONG *)&g_mixer_active_count);
    }
    LeaveCriticalSection(&g_mixer_cs);
}

/* Mix all active voices into frame_buf. Called from mcpx_apu_monitor_frame.
 * Keep 16 fractional bits in a wide offset so buffers can exceed 65536 frames. */
static void mixer_render(int16_t frame_buf[][2], int num_samples)
{
    if (!g_mixer_initialized) return;
    EnterCriticalSection(&g_mixer_cs);

    for (int v = 0; v < APU_MIXER_MAX_VOICES; v++) {
        APUMixerVoice *voice = &g_mixer_voices[v];
        if (!voice->active || !voice->pcm_data || voice->pcm_bytes == 0)
            continue;

        uint32_t total_frames = voice->pcm_bytes / sizeof(int16_t);
        if (voice->num_channels == 2) total_frames /= 2;
        if (total_frames == 0) continue;

        /* Fixed-point 16.16 increment per output sample */
        uint64_t inc = ((uint64_t)voice->sample_rate << 16) / 48000;
        uint64_t pos = voice->play_offset;
        uint64_t end = (uint64_t)total_frames << 16;
        float vol = voice->volume;

        for (int i = 0; i < num_samples; i++) {
            uint32_t src_frame = pos >> 16;

            if (src_frame >= total_frames) {
                if (voice->looping) {
                    pos %= end;
                    src_frame = (uint32_t)(pos >> 16);
                } else {
                    pos = 0;
                    voice->active = 0;
                    InterlockedDecrement((volatile LONG *)&g_mixer_active_count);
                    break;
                }
            }

            int32_t left, right;
            if (voice->num_channels >= 2) {
                left  = (int32_t)(voice->pcm_data[src_frame * 2] * vol);
                right = (int32_t)(voice->pcm_data[src_frame * 2 + 1] * vol);
            } else {
                left = right = (int32_t)(voice->pcm_data[src_frame] * vol);
            }

            /* Accumulate (mix) into frame_buf with clamping */
            int32_t mixed_l = frame_buf[i][0] + left;
            int32_t mixed_r = frame_buf[i][1] + right;
            if (mixed_l > 32767) mixed_l = 32767;
            if (mixed_l < -32768) mixed_l = -32768;
            if (mixed_r > 32767) mixed_r = 32767;
            if (mixed_r < -32768) mixed_r = -32768;
            frame_buf[i][0] = (int16_t)mixed_l;
            frame_buf[i][1] = (int16_t)mixed_r;

            pos += inc;
        }

        voice->play_offset = pos;
        uint32_t end_frame = pos >> 16;
        if (end_frame >= total_frames) {
            if (voice->looping) {
                voice->play_offset = pos % end;
            } else if (voice->active) {
                voice->play_offset = 0;
                voice->active = 0;
                InterlockedDecrement((volatile LONG *)&g_mixer_active_count);
            }
        }
    }
    LeaveCriticalSection(&g_mixer_cs);
}
