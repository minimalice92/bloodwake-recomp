/* bw_audio_hooks.c - D-011 wrappers for the audio path (agent D2).
 *
 * Every hook here is a wrapper sub_X that calls the game's own generated body
 * sub_X_gen (tools/run_pipeline.py hands this file to the generator with
 * src/recomp_manual.c). Unless its own switch or Remastered change says
 * otherwise it returns what the body returned, so Original is unchanged.
 *
 * Claims are labelled as in the docs: seen, inferred, unknown.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "bw_edition.h"

/* The guest registers are thread-local in the runtime (TOOLKIT_ISSUES TK-7). */
extern __declspec(thread) uint32_t g_eax;

/* ── The speaker configuration ─────────────────────────────────────────────
 *
 * XAPI_XGetAudioFlags (0x000D9240): ExQueryNonVolatileSetting(XC_AUDIO)
 * adjusted by the AV pack (seen, names batch). Two readers (seen):
 * DSound_SettingsCtor (0xFAE52), when the title's override global [0x116710]
 * isn't a valid config (Blood Wake never writes it), and the game's
 * Sound_MixToCenter (0x18610), which routes to the centre only when bit 1
 * (surround) is set. The port's kernel answers 0: stereo.
 *
 * Values (XC_AUDIO_FLAGS): low word 0 stereo, 1 mono, 2 surround; 0x10000
 * Dolby Digital (AC3) out, 0x20000 DTS. DirectSound treats "surround, or AC3
 * without headphones" as the speaker mode that sends rear sources to the rear
 * bins instead of folding them onto the front HRTF (DSound_ApuApplySpeakerConfig
 * 0xFCC80, seen).
 *
 * BW_AUDIO_SPEAKERS = stereo | mono | surround | ac3 | 0x<hex> forces the
 * answer in any edition (a test switch). Remastered (BW_FIX AUDIO_SURROUND)
 * answers surround when the host's audio device has 6 or more channels
 * (bw_audio_host_channels, the carried XAudio2 output). */
extern void sub_000D9240_gen(void);
/* -1 = not decided yet (XAudio2 init hasn't run), 0 or 1; why = what decided it */
extern int bw_audio_surround_wanted(const char **why);   /* toolkit_patches/apu */

static int s_spk_mode = -2;                     /* -1: the game's own answer */
static uint32_t s_spk_flags;

/* Item 67 (MR2 #4): the answer is cached only once the audio output has
 * decided on surround. main.c brings XAudio2 up before the game's first
 * instruction, so a call before that is not expected (seen: mission 1 asks
 * long after init, run 20261103-015058-r6-gpu-c2-67-force); if one came, it
 * would get the game's own answer and the next call would decide. The log
 * names the switch that set the answer. */
uint32_t bw_audio_speaker_flags(int *forced)
{
    if (s_spk_mode == -2) {
        const char *e = getenv("BW_AUDIO_SPEAKERS");
        const char *why = "BW_AUDIO_SPEAKERS";
        int sur;
        if (e && *e) {
            s_spk_mode = 1;
            if (!strcmp(e, "stereo"))
                s_spk_flags = 0;
            else if (!strcmp(e, "mono"))
                s_spk_flags = 1;
            else if (!strcmp(e, "surround"))
                s_spk_flags = 2;
            else if (!strcmp(e, "ac3"))
                s_spk_flags = 0x10000;
            else
                s_spk_flags = (uint32_t)strtoul(e, NULL, 0);
        } else if ((sur = bw_audio_surround_wanted(&why)) < 0) {
            static int told;
            if (!told++)
                fprintf(stderr, "[AUDIO] the game asked for the speaker configuration before the "
                        "audio output was set up: its own answer for now, decided on a later call\n");
            if (forced)
                *forced = 0;
            return 0;
        } else if (sur) {
            s_spk_mode = 1;
            s_spk_flags = 2;
        } else {
            s_spk_mode = -1;
        }
        if (s_spk_mode > 0)
            fprintf(stderr, "[AUDIO] speaker configuration the game sees: 0x%X (%s)\n",
                    s_spk_flags, why);
    }
    if (forced)
        *forced = s_spk_mode > 0;
    return s_spk_flags;
}

void sub_000D9240(void)
{
    int forced;
    uint32_t flags;
    sub_000D9240_gen();
    flags = bw_audio_speaker_flags(&forced);
    if (forced)
        g_eax = flags;
}
