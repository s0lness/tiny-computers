/* app_roster.inc: this firmware's app layer, as the runtime sees it.
 *
 * INCLUDED, not compiled: firmware/runtime/runtime_core.c pulls this in at
 * the point where its app table used to live. runtime_core.c is a SYNCED PACK
 * SOURCE now (tools/sync-pack.ts, PACK_PROVENANCE.md) - shared verbatim with
 * every other firmware built on this board - and the list of apps is the one
 * thing that differs between them. So the apps stayed here and the runtime
 * went upstream, which is the whole shape of that convergence: this repo owns
 * its apps, the pack owns the runtime.
 *
 * THIS FILE IS OURS. Edit it freely. Everything under firmware/runtime/,
 * firmware/lib/ and emulator/wasm/shim/ is not: see AGENTS.md.
 *
 * What the runtime asks for, and nothing else:
 *
 *   const app_t *const g_apps[];   the app table (app.h)
 *   const int g_appCount;
 *   const int g_menuAppIndex[];    the menu's roster, indices into g_apps[]
 *   const int g_menuAppCount;
 *   extern const app_t g_menuApp;  the picker, addressed by APP_INDEX_MENU
 *   extern void menu_set_return_app(int index);
 *
 * The text below is the app table's own history, moved here unchanged with
 * it. Read it before adding or removing an app.
 */

/* ---- the app table -------------------------------------------------------
 *
 * One extern per app, exactly as app.h's header comment describes: adding
 * an app is one entry here plus one file. g_menuApp and menu_set_return_app
 * are declared directly here (the same extern-per-app pattern) rather than
 * by including apps/menu.h, which this file may not: menu.h only pulls in
 * app.h itself, but the include is still outside the {app.h, gfx.h,
 * sensors.h} allowance runtime_core.h documents, and there is nothing menu.h
 * would add beyond what a plain extern already gives.
 */
extern const app_t g_chronoApp;
extern const app_t g_sketchApp;
extern const app_t g_timerApp;
extern const app_t g_fourApp;
extern const app_t g_clockApp;
extern const app_t g_morpionApp;
extern const app_t g_dinoApp;
extern const app_t g_bowlingApp;
extern const app_t g_tiltballApp;
extern const app_t g_breakoutApp;
// tables: multiplication-tables practice (firmware/apps/tables.c), the
// twelfth app - decision 0013's own ceiling, reached exactly rather than
// approached (see that file's own header for the numpad/loupe design this
// app needed to fit inside the remaining budget).
extern const app_t g_tablesApp;
extern const app_t g_menuApp;
extern void menu_set_return_app(int index);

/* SCREENSHOT FIXTURE, compiled out by default. See apps/stubapps.c for the
 * whole argument; in one line: the menu's layout has to be judged at six and
 * twelve apps, and this firmware declares eleven real ones - one under
 * decision 0013's own ceiling, since the owner had the bubble level removed
 * (2026-08-17) - so a twelve-app capture needs one stub again (see
 * tools/preview-menu-grid.ts). The fixture also stays wired for whatever
 * comes AFTER twelve (decision 0013's "past that something has to be
 * hidden"), which is why the guard below exists rather than being deleted
 * outright. Declared with a bare extern, the same way g_menuApp above is,
 * because runtime_core.h's contract is that this file includes nothing from
 * apps/.
 *
 * MENU_STUB_APPS is the TOTAL app count wanted (11 real + the rest stubs),
 * so MENU_STUB_APPS=12 appends one. Spelled out one #if per entry rather
 * than generated with a macro: a static initialiser list is exactly the
 * place where clever expansion stops being readable, and a few lines that
 * say what they do cost nothing. */
#if MENU_STUB_APPS
extern const app_t g_stubApps[];
#endif

// Appended, not inserted: index 0 is what boots (app.h) and every emulator
// test in emulator/wasm/tests/ addresses apps by their index in this array
// (APP_DRAW = 1 and so on), so a new app goes on the end. The clock
// (firmware/apps/clock.c), morpion (firmware/apps/morpion.c) and the four
// games merged 2026-08-15 (dino, bowling, tiltball, breakout) all read
// app_frame_t.tilt or plain touch like every other app, so there is no
// reason for any of them to sit outside this table behind a flag or a
// private index - see runtime_core.h's git history for APP_INDEX_CLOCK, the
// clock's own now-removed equivalent (it existed only because appending to
// this table used to move every menu column; decision 0013's grid replaced
// that layout, so the reason is gone). The bubble level (firmware/apps/
// level.c) used to sit here too, unconditionally like the rest, until the
// owner had it removed outright on 2026-08-17 - not gated, gone. Eleven
// real apps, with tables.c appended here, sits one under decision 0013's
// own twelve-app ceiling - the grid absorbs up to twelve at 112x112 cells,
// the smallest the grid ever gets, without narrowing any target further -
// see tools/gate/run.ts's app-count-ceiling rule, which would fail loudly
// on a thirteenth.
const app_t *const g_apps[] = {
    &g_chronoApp, &g_sketchApp, &g_timerApp, &g_fourApp, &g_clockApp, &g_morpionApp,
    &g_dinoApp, &g_bowlingApp, &g_tiltballApp, &g_breakoutApp, &g_tablesApp,
#if MENU_STUB_APPS > 11
    &g_stubApps[0],
#endif
};
const int g_appCount = sizeof(g_apps) / sizeof(g_apps[0]);

/* ---- the menu's roster, separate from the table above ---------------------
 *
 * app.h's own comment on g_menuAppIndex/g_menuAppCount has the contract; this
 * is where it is decided. Five of the eleven apps above, the owner's cut on
 * 2026-08-17: chrono, sketch ("draw"), timer, four, tables. The other six -
 * clock, morpion, dino, bowling, tiltball, breakout - stay exactly where they
 * were in g_apps[] above, fully built, fully compiled into both targets, and
 * fully reachable by every test under emulator/wasm/tests/ that already
 * switches to one of them by its g_apps[] index (APP_CLOCK = 4 and so on) -
 * this table only decides what menu.c draws and what a touch on the grid can
 * launch, nothing about what the firmware carries. See docs/decisions/0019.
 *
 * Indices into g_apps[] above, spelled out one per line rather than computed,
 * the same "a few lines that say what they do cost nothing" call g_apps[]
 * itself makes for MENU_STUB_APPS.
 */
#if MENU_STUB_APPS
// SCREENSHOT-FIXTURE BUILD (firmware/apps/stubapps.c). This branch is never
// compiled into a real build - CMakeLists.txt never lists stubapps.c and no
// build here defines MENU_STUB_APPS on its own; it exists only for
// tools/preview-menu-grid.ts's own instructions for judging decision 0013's
// (and, since 2026-08-17, decision 0020's) grid arithmetic at an app count
// this device does not have real apps for. The array is the WHOLE padded
// g_apps[] table, real apps and the stub alike - unchanged shape from
// before decision 0020.
//
// g_menuAppCount, below, is what actually picks how many of those entries
// the menu shows, and it is MENU_STUB_APPS itself when that is SMALLER than
// the array (MENU_STUB_APPS=4, 6 or 7: a hypothetical few-app menu, built
// from the first few real apps in table order - their identity does not
// matter for judging layout geometry, only their count does) and the whole
// array when MENU_STUB_APPS is 11 or 12 (the padding case this fixture was
// originally built for). One mechanism now covers every count
// tools/preview-menu-grid.ts's own regression sweep (4, 5, 6, 7, 11, 12)
// needs, rather than a padding-only fixture that could reach 11 or 12 and
// nothing smaller.
const int g_menuAppIndex[] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10,
#if MENU_STUB_APPS > 11
    11,
#endif
};
#define MENU_STUB_ARR_LEN ((int)(sizeof(g_menuAppIndex) / sizeof(g_menuAppIndex[0])))
const int g_menuAppCount = (MENU_STUB_APPS < MENU_STUB_ARR_LEN) ? MENU_STUB_APPS : MENU_STUB_ARR_LEN;
#else
const int g_menuAppIndex[] = {
    0,  // chrono
    1,  // sketch ("draw")
    2,  // timer
    // The clock joined on 2026-08-18, once its own gesture was one a child
    // could find (double-press to open, either button to confirm) and its
    // face followed the edge that is up. Placed FOURTH, which at six apps is
    // the bottom-left cell: the owner asked for it "en bas a gauche, avant le
    // puissance 4 et les tables", so the top row stays the three tools he had
    // learned to reach for and the new one starts the second row rather than
    // displacing anything.
    4,  // clock
    3,  // four
    10, // tables
};
const int g_menuAppCount = (int)(sizeof(g_menuAppIndex) / sizeof(g_menuAppIndex[0]));
#endif
