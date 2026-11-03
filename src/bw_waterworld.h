#ifndef BW_WATERWORLD_H
#define BW_WATERWORLD_H

/* bw_waterworld.h - the game's WaterWorld and Water object layout, shared by
 * the high frame rate (bw_hifps_hooks.c) and the native ocean
 * (bw_ocean_native.c). One copy (0.2.5 item 71: both files had their own).
 * Seen in the generated code (agents P2a and P2b). */

/* WaterWorld, a global; its first word is the Water object. */
#define WW_VA            0x001D77F8u
#define WW_CLOCK_MS      0xACA4u       /* the water's clock: ms, s, the previous s */
#define WW_CLOCK_S       0xACA8u
#define WW_CLOCK_PREV_S  0xACACu
#define WW_PROF_TIME     0xCCF8u       /* mode 0x1D's time-spent counter (rdtsc profiling) */

/* The Water object ([WW_VA]). */
#define W_UPDATES        0x08u
#define W_IDX_NEW        0x0Cu         /* which of the two height buffers is new */
#define W_IDX_PREV       0x10u
#define W_HEIGHTS        0x2AB8u
#define W_HEIGHTS_SZ     0x1000u
#define W_PATCH          0x4AB8u       /* the Bezier patches, two of W_PATCH_SZ */
#define W_PATCH_SZ       0x10000u
#define W_PFLAG          0x24AB8u      /* their built flags, two of W_PFLAG_SZ */
#define W_PFLAG_SZ       0x400u
#define W_OBJ_SIZE       0x252F0u

#endif
