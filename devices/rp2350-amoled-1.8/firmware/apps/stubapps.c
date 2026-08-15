/*
 * stubapps: seven do-nothing apps, so the menu's geometry can be SEEN at
 * app counts this device does not have yet.
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
 * comes out of the real menu_enter().
 *
 * COMPILED OUT BY DEFAULT AND ABSENT FROM THE BOARD BUILD. Everything below
 * is inside `#if MENU_STUB_APPS`, so with the define unset this is an empty
 * translation unit; firmware/CMakeLists.txt does not list this file at all,
 * so a stub app cannot reach a uf2 even by accident. The emulator build
 * lists it unconditionally and it costs nothing there either. To use it:
 *
 *   EMU_EXTRA_DEFINES=-DMENU_STUB_APPS=12 bun run emulator/wasm/build.ts
 *
 * MENU_STUB_APPS is the TOTAL app count wanted, not the number of stubs:
 * runtime_core.c appends (MENU_STUB_APPS - 5) of these to the five real
 * apps. That spelling was chosen because the number the reader cares about
 * is "twelve apps", and a build flag that says 7 while the screenshot is
 * called twelve is one subtraction away from a mislabelled capture, which
 * is a mistake tools/preview-menu-icons.ts has already made once (see its
 * comment on the file called "menu-icon-timer" that was a picture of
 * Connect Four).
 *
 * A stub draws NOTHING. The runtime clears the framebuffer to white and
 * pushes the whole panel on switch-in (app.h's enter() contract), so a stub
 * that never draws leaves a blank white screen, which is the honest picture
 * of an app that does not exist. Its ICON is not blank though: menu.c
 * (under the same #if) lends any app past the fifth one of the four real
 * icons that actually draw something (chrono/sketch/timer/four - the level
 * has no icon of its own either, see menu.c), cycling, so a twelve-app
 * capture shows the layout under real ink instead of a grid of holes. Ink
 * is what the layout has to be judged against.
 */
#if MENU_STUB_APPS

#include <stdbool.h>

#include "app.h"

static void stub_enter(void) {}
static void stub_tick(const app_frame_t *f) { (void)f; }

// Named "stub N" rather than borrowed from the apps that are coming
// (tic-tac-toe, breakout, ...): a capture that says "breakout" would be a
// picture of an app nobody has written, and this project has already paid
// for one instrument that labelled something it had not measured.
#define STUB_APP(label)                 \
    { .name = label,                    \
      .enter = stub_enter,              \
      .tick = stub_tick,                \
      .leave = NULL,                    \
      .landscape = true,                \
      .wantsShake = false }

const app_t g_stubApps[7] = {
    STUB_APP("stub1"), STUB_APP("stub2"), STUB_APP("stub3"), STUB_APP("stub4"),
    STUB_APP("stub5"), STUB_APP("stub6"), STUB_APP("stub7"),
};

#endif // MENU_STUB_APPS
