/*
 * bw_input_ext.h - what the Blood Wake carried patches of the toolkit's input
 * files add (src/toolkit_patches/input/xinput_device.c and keyboard.c). Kept
 * in its own header because the toolkit's headers stay untouched (D-010).
 *
 * Every function here is safe to call from any thread.
 */
#ifndef BW_INPUT_EXT_H
#define BW_INPUT_EXT_H

#ifdef __cplusplus
extern "C" {
#endif

/* ---- xinput_device.c --------------------------------------------------- */

/* How many controllers Windows reports connected now (0-4), counting the
 * BW_TEST_XINPUT stand-ins when that test switch is set. */
int  xbox_InputConnectedCount(void);

/* Turn every controller's rumble off at once (all four XInput slots). `why`
 * goes into one log line ("window closed", "boot start", ...). */
void xbox_InputRumbleAllOff(const char *why);

/* The game's window lost (0) or regained (1) the focus. Losing it stops the
 * rumble and holds any new rumble back; regaining it puts back what the game
 * last asked for. Starts as focused. */
void xbox_InputSetFocus(int focused);

/* Radial stick deadzones for real pads, in percent (1-95); 0 = none (what the
 * port has always done: the game gets the stick exactly as Windows reads it). */
void xbox_InputSetDeadzones(int left_pct, int right_pct);

/* ---- keyboard.c -------------------------------------------------------- */

struct InputBindings;

/* ---- usb_gamepad.c (carried) ------------------------------------------- */

/* For `ms` from now, pad `pad`'s reports also carry these digital bits,
 * analog buttons (bit n = report byte 4 + n: A, B, X, Y, Black, White, the
 * triggers, fully pressed) and left-stick deflection (0 = the pad's own).
 * The menus' mouse on screens that read a player's own pad (boat select,
 * controller popups; src/bw_menu_hooks.c). */
void bw_pad_pulse(int pad, int lx, int ly, unsigned digital, unsigned analog, unsigned ms);

/* ---- keyboard.c (carried) ---------------------------------------------- */

/* Use these bindings for the keyboard instead of the built-in map (NULL goes
 * back to the built-in map). ramp_ms (0-2000) is how long a held steering key
 * (the keys bound to left_stick_left / left_stick_right) takes to reach its
 * full turn, 0 = at once; strength_pct (10-100) is how far those keys turn,
 * 100 = full. 0 and 100 are exactly the old behaviour. */
void xbox_InputKeyboardConfigure(const struct InputBindings *b, int ramp_ms,
                                 int strength_pct);

#ifdef __cplusplus
}
#endif

#endif /* BW_INPUT_EXT_H */
