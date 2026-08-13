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

// Historically told the menu which app to treat as "where we came from", so
// its tile could start selected when the menu opened. The menu is touch-only
// now (see menu.c's header comment): there is no selection cursor left to
// default, so menu.c's own implementation of this ignores the value. The
// function still exists and is still called unconditionally by
// runtime_core.c on every chord that opens the menu (see its "BOOT+PWR
// long-press chord" comment) - that call site is the runtime's, not this
// app's, to change, so the entry point stays even though menu.c no longer
// has anything to do with what it is handed.
void menu_set_return_app(int index);

extern const app_t g_menuApp;

#endif // APPS_MENU_H
