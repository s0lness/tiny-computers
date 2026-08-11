/*
 * menu: the in-app app-launcher, shared by every app (see AGENTS.md's build
 * notes for the "why in-app, not the bootloader" reasoning - the short
 * version is that opening and closing it must be instant, which a
 * bootloader-hosted menu cannot be, since a reboot is the only way in or
 * out of one).
 *
 * menu_run() is modal and blocking: it takes over the screen, drives its own
 * loop, and only returns once the user has picked a different app (in which
 * case it has already rebooted into it and this function never returns at
 * all - see below) or cancelled (in which case it returns -1).
 *
 * Input reaches the menu through a callback rather than menu.c reading the
 * PMIC directly, for two independent reasons, both of which matter and
 * neither of which is optional:
 *
 *   1. The menu runs on core0. In the sketchpad, core1 is the sole owner of
 *      i2c1 from the moment it launches (see firmware/main.c's core1_entry()
 *      banner comment); core0 touching i2c1 - even transitively, even once -
 *      is exactly the class of bug that produces a corrupted transaction on
 *      a shared bus. menu.c is one implementation compiled into both apps,
 *      so it is written to never touch i2c1 in EITHER app, even though
 *      chrono alone (single-core) could technically get away with it.
 *
 *   2. In the sketchpad this callback only ever reads a volatile byte
 *      (g_keyEvent): core1's pmic_poll_core1() keeps servicing the PMIC on
 *      its own regardless of what core0 is doing, menu_run() included, so
 *      draining g_keyEvent is all core0 ever needs to do. chrono has no
 *      second core, so nothing services the PMIC while chrono's own main
 *      loop is blocked inside menu_run() unless the callback does it: its
 *      implementation of this same callback calls chrono's buttons_poll()
 *      (the function that actually touches i2c1) before reading g_keyEvent.
 *      menu.c neither knows nor cares which of the two is happening behind
 *      the callback; that is the point of it being a callback.
 *
 * BOOT input is not routed through a callback: bootbtn.c never touches i2c1
 * (it borrows the flash chip select instead), so it is safe for menu.c to
 * poll directly in either app, exactly like chrono's own main loop already
 * does for its own purposes.
 */
#ifndef MENU_H
#define MENU_H

#include <stdint.h>

// Supplies the next PWR-button event since the last call, or 0 if nothing
// new has latched. Same bit encoding as the AXP2101's register 0x49 that
// every PMIC-reading path in this codebase already uses: 0x02 press edge,
// 0x04 long press, 0x08 short-press verdict (latches on release). The
// implementation is expected to read-and-clear whatever event source it
// wraps, the same one-shot contract every other g_keyEvent consumer in this
// codebase already follows.
typedef uint8_t (*menu_pwr_poll_fn)(void);

// Runs the modal menu over whatever is currently in `fb`. Blocking: returns
// only once the user has cancelled. Returns -1 on cancel.
//
// A pick that launches a DIFFERENT slot does not return an ordinary value at
// all: menu_run() calls appswitch_go_to() itself and that reboots, so
// control never comes back here. The non-negative return exists only for
// the fallback where that reboot request could not even be issued (e.g. the
// current slot could not be determined); callers are not expected to act on
// it beyond logging, since there is nothing else productive left to do.
//
// The framebuffer is otherwise left exactly as it was: menu_run() saves the
// small rectangle it draws into before touching it and restores those exact
// pixels before returning on cancel. That is not a courtesy, it is load
// bearing for the sketchpad: its canvas has no representation other than
// the live framebuffer (strokes are rasterised once and forgotten, see
// firmware/main.c), so there is nothing else that could put a cancelled
// drawing back. Callers that CAN cheaply repaint their own UI from other
// state (chrono, from its digit values) are still free to do so afterwards;
// it will simply be redundant over the region the menu already restored.
int menu_run(uint16_t *fb, menu_pwr_poll_fn poll_pwr);

#endif // MENU_H
