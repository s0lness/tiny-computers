/*
 * menu: the in-app app-launcher, shown full-screen over the whole landscape
 * canvas. Not a slot-switching bootloader menu (that design is gone, see
 * docs/decisions/0002) and not an overlay saved and restored over whatever
 * app was running (that was the old firmware/menu.c's job when switching
 * meant surviving a reboot with nothing else able to redraw the sketchpad's
 * canvas; here the runtime already owns clearing to white and pushing on
 * every switch, menu included, so there is nothing left to save).
 *
 * Declared as g_menuApp (see app.h) in menu.c. Deliberately NOT in
 * runtime.c's g_apps[] table: the menu is the shell a child uses to pick an
 * app, not itself one of the apps being picked.
 */
#ifndef APPS_MENU_H
#define APPS_MENU_H

#include "../runtime/app.h"

// Tells the menu which app to treat as "where we came from": its tile
// starts selected when the menu opens, exactly like the old menu.c defaulted
// its cursor to the currently running slot. Called by the runtime in the
// same frame it decides to switch into the menu, before that switch is
// applied, so by the time menu_enter() runs (arena freshly reset) this value
// is already set and safe to read.
//
// The runtime also keeps its own copy of this same value, for a reason
// menu.c does not need to know about: closing the menu (the same
// long-double-press gesture, fired again while already inside it) is a
// runtime-level decision, not something menu.c requests, so the runtime
// needs this value for its own use too, not just to hand to menu_enter().
void menu_set_return_app(int index);

extern const app_t g_menuApp;

#endif // APPS_MENU_H
