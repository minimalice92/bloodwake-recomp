#ifndef BW_GUESTCALL_H
#define BW_GUESTCALL_H

/* bw_guestcall.h - calling the game's own functions from a D-011 wrapper,
 * and keeping the caller's registers around it. One copy for the hook files
 * (0.2.5 item 71: bw_fix_hooks.c's f_* and bw_visextra_hooks.c's v_* were
 * the same code twice).
 *
 * A call pushes the arguments right to left, then a return address nobody
 * reads, sets ecx (thiscall), runs the translated body and puts esp back, so
 * the callee's own ret N adjustment doesn't matter. The registers are saved
 * around calls made after the original body, so what the body returned
 * reaches the game unchanged unless a fix changes it on purpose. */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef void (*recomp_func_t)(void);
extern recomp_func_t recomp_lookup(uint32_t xbox_va);
extern ptrdiff_t g_xbox_mem_offset;
/* The guest registers are thread-local in the runtime (TOOLKIT_ISSUES TK-7). */
extern __declspec(thread) uint32_t g_eax, g_ecx, g_edx, g_esp;
extern __declspec(thread) uint32_t g_ebx, g_esi, g_edi, g_ebp;
extern __declspec(thread) uint32_t g_seh_ebp, g_eflags;

typedef struct {
    uint32_t eax, ecx, edx, ebx, esi, edi, ebp, seh_ebp, eflags;
} bw_gregs_t;

static __inline void bw_gregs_save(bw_gregs_t *r)
{
    r->eax = g_eax; r->ecx = g_ecx; r->edx = g_edx; r->ebx = g_ebx;
    r->esi = g_esi; r->edi = g_edi; r->ebp = g_ebp; r->seh_ebp = g_seh_ebp;
    r->eflags = g_eflags;
}

static __inline void bw_gregs_restore(const bw_gregs_t *r)
{
    g_eax = r->eax; g_ecx = r->ecx; g_edx = r->edx; g_ebx = r->ebx;
    g_esi = r->esi; g_edi = r->edi; g_ebp = r->ebp; g_seh_ebp = r->seh_ebp;
    g_eflags = r->eflags;
}

/* eax after the call, or 0 (with one line, prefixed by tag) when no
 * translated function is at va. */
static __inline uint32_t bw_guest_call(const char *tag, uint32_t va, uint32_t this_ptr,
                                       int nargs, const uint32_t *args)
{
    recomp_func_t fn = recomp_lookup(va);
    uint32_t sp0 = g_esp;
    int i;
    if (!fn) {
        fprintf(stderr, "%s no translated function at 0x%08X\n", tag, va);
        fflush(stderr);
        return 0;
    }
    for (i = nargs - 1; i >= 0; i--) {
        g_esp -= 4;
        *(volatile uint32_t *)((uintptr_t)g_esp + g_xbox_mem_offset) = args[i];
    }
    g_esp -= 4;
    *(volatile uint32_t *)((uintptr_t)g_esp + g_xbox_mem_offset) = 0;   /* return address: never read */
    g_ecx = this_ptr;
    fn();
    g_esp = sp0;
    return g_eax;
}

#endif
