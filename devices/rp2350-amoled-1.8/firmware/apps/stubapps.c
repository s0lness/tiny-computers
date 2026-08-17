/*
 * stubapps: up to one do-nothing app, so the menu's geometry can be SEEN
 * at app counts this device does not have yet.
 *
 * WHY THIS EXISTS. The menu was rewritten from a row of columns to a grid
 * on 2026-08-15 because the sixth app takes a column under a child's
 * fingertip (docs/decisions/0010 and 0013). The whole argument is about
 * what the screen looks like at six and at twelve, and the owner's list
 * gets there within days - but on the night it was written the firmware
 * declared five apps (chrono, sketch, timer, four, level), so there was
 * nothing to look at past that. Rendering the geometry from a script that
 * re-implements menu.c's layout would be exactly the instrument decision
 * 0010 warns about: it would read one link upstream of the code being
 * judged, and it would agree with itself.
 *
 * So the firmware itself is built with more apps in it, and the capture
 * comes out of the real menu_enter(). The firmware grew to twelve real apps
 * (chrono, sketch, timer, four, level, clock, morpion, dino, bowling,
 * tiltball, breakout, tables) - decision 0013's own twelve-app ceiling,
 * reached exactly by tables.c - then dropped back to eleven when the owner
 * had the bubble level removed outright (2026-08-17; level.c is gone, not
 * merely gated). So a twelve-app capture needs a stub again: this file is
 * back to doing the job it was built for, seeing the ceiling itself, not
 * some future THIRTEENTH app past it (that is still what it stays wired
 * for beyond twelve, per decision 0013's "past that something has to be
 * hidden", but is no longer the only reason it earns its keep).
 *
 * COMPILED OUT BY DEFAULT AND ABSENT FROM THE BOARD BUILD. Everything below
 * is inside `#if MENU_STUB_APPS`, so with the define unset this is an empty
 * translation unit; firmware/CMakeLists.txt does not list this file at all,
 * so a stub app cannot reach a uf2 even by accident. The emulator build
 * lists it unconditionally and it costs nothing there either. To use it:
 *
 *   EMU_EXTRA_DEFINES=-DMENU_STUB_APPS=12 bun run emulator/wasm/build.ts
 *
 * MENU_STUB_APPS is the TOTAL app count wanted, not the number of stubs.
 * runtime_core.c reads it two ways (decision 0020, 2026-08-17): g_apps[]
 * itself only ever appends (MENU_STUB_APPS - 11) of these, and since
 * g_stubApps below carries exactly one entry, only MENU_STUB_APPS=12
 * actually GROWS the table - anything higher still gets just the one stub
 * appended, short of what it asked for, a pre-existing limit of this
 * single-stub design. But the menu's own roster (g_menuAppIndex/
 * g_menuAppCount, app.h) reads MENU_STUB_APPS as a CAP on how many of
 * g_apps[]'s eleven-or-twelve entries the menu shows, not just a floor to
 * pad up to - so MENU_STUB_APPS=4, 6 or 7 also works, showing the menu at
 * that many apps using the first few real ones in table order (their
 * identity does not matter for judging layout geometry, only their count
 * does). That spelling was chosen because the number the reader cares
 * about is "how many apps is this a picture of", and a build flag that
 * says 1 while the screenshot is called twelve is one subtraction away
 * from a mislabelled capture, which is a mistake tools/preview-menu-icons.ts
 * has already made once (see its comment on the file called
 * "menu-icon-timer" that was a picture of Connect Four).
 *
 * A stub draws NOTHING. The runtime clears the framebuffer to white and
 * pushes the whole panel on switch-in (app.h's enter() contract), so a stub
 * that never draws leaves a blank white screen, which is the honest picture
 * of an app that does not exist. Its ICON is not blank though: menu.c
 * (under the same #if) lends any app past the eleventh one of the eleven
 * real apps' own icons, cycling, so a twelve-app capture shows the layout
 * under real ink instead of a grid of holes. Ink is what the layout has to
 * be judged against.
 */
#if MENU_STUB_APPS

#include <stdbool.h>

#include "app.h"

static void stub_enter(void) {}
static void stub_tick(const app_frame_t *f) { (void)f; }

// Named "stub N" rather than borrowed from any real app: a capture that
// says the name of an app already in g_apps[] would be a picture of the
// wrong thing, and this project has already paid for one instrument that
// labelled something it had not measured.
#define STUB_APP(label)                 \
    { .name = label,                    \
      .enter = stub_enter,              \
      .tick = stub_tick,                \
      .leave = NULL,                    \
      .landscape = true,                \
      .wantsShake = false }

const app_t g_stubApps[1] = {
    STUB_APP("stub1"),
};

#endif // MENU_STUB_APPS
