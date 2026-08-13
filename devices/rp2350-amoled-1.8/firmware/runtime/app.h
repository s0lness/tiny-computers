/*
 * app: what an app is, and what the runtime promises it.
 *
 * Every app compiles into one binary and switching between them is a
 * function call, not a reboot. The numbers that decided this:
 *
 *   function call       under one frame     repaint only
 *   slot switch, warm   ~20ms (untested)    reboot, skipping panel init
 *   slot switch, cold   182ms               reboot plus panel init
 *
 * and 150ms of that 182 is the sleep-out the SH8601 physically requires
 * after reset. A reboot cannot beat a function call, so switching apps no
 * longer reboots. The partition machinery in store/ stays, repurposed as a
 * golden fallback image for crash recovery, which is what it is actually
 * worth.
 *
 * An app is a struct of callbacks. It does not own the screen, the input,
 * or the main loop; the runtime does, and hands it a turn.
 */
#ifndef APP_H
#define APP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- the arena ---------------------------------------------------------
 *
 * App state lives here, not in file-scope statics, and this is not a style
 * preference. With every app in one image, static buffers coexist at link
 * time: ten apps each keeping "a bit" of state will eat the SRAM budget
 * silently, and the linker does not warn at 99KB, it fails at 521. An arena
 * makes the cost of an app's state one number, visible in one place, and
 * reclaims it on switch.
 *
 * An app that cannot fit in the arena is a design problem, not a budget
 * problem. The sketchpad, the largest, needs a few hundred bytes: its
 * document is the framebuffer, which the runtime owns.
 */
#define APP_ARENA_BYTES 65536

// Allocates from the arena, zeroed. Only legal inside enter(); the arena is
// reset on every switch, so a pointer taken in enter() is valid until
// leave() and not one instruction longer.
//
// It never returns NULL. Running out of arena is a build-time bug (an app
// asked for more than the arena holds), not a runtime condition to handle,
// and handing NULL back to callers that reasonably do not check it would
// turn a precise failure into a null-dereference somewhere else entirely.
// So the runtime traps instead: it fills the screen with a distinctive
// colour and stays there, keeping the watchdog fed so the trap remains on
// screen rather than dissolving into a silent reboot loop.
void *app_alloc(size_t bytes);

// Zeroed convenience form, which is what an app's state struct always wants.
#define APP_STATE(type) ((type *)app_alloc(sizeof(type)))

/* ---- input the runtime hands to the app --------------------------------
 *
 * Apps read signals, never chips (see sensors.h for why that rule is
 * absolute). The runtime drains the sensor queues, consumes what belongs to
 * it, and leaves the rest in the frame struct below.
 *
 * The runtime consumes, and the app never sees:
 *   - the BOOT+PWR long-press chord that opens the menu;
 *   - shake, unless the app set wants_shake.
 *
 * Everything else is the app's.
 */
typedef struct {
    // Touch, already clamped to the panel and resolved into a simple
    // down/moved/up shape. Apps that need the raw sample stream (only the
    // sketchpad does, for its stroke reconstruction) read sensors.h
    // directly instead; this is the convenient form for everything else.
    bool     touchDown;      // a finger is on the glass right now
    bool     touchPressed;   // it went down this frame
    bool     touchReleased;  // it came up this frame
    int      touchX, touchY; // panel coordinates, valid while touchDown

    // Power key bits the runtime did not consume (KEY_SHORT, KEY_PRESS and
    // KEY_RELEASE from sensors.h always pass through unchanged; a KEY_LONG
    // that opened or closed the menu is missing by the time an app sees
    // this - see sensors.h's PWR key section for why KEY_RELEASE exists at
    // all, and runtime_core.c's frame.key comment for the pass-through
    // list). Zero on most frames.
    uint8_t  key;

    // True on the frame BOOT was released, which is the conventional moment
    // for a click. BOOT is read by borrowing the flash chip select, which
    // costs a few microseconds with interrupts off, so the runtime samples
    // it at 20Hz (every 50ms) and never faster.
    bool     bootClicked;

    // Bumped when an accepted shake happened, and only delivered to apps
    // that asked for it.
    bool     shaken;

    uint32_t nowMs;
    uint32_t dtMs;           // since the previous tick, clamped
} app_frame_t;

/* ---- the app ----------------------------------------------------------- */

typedef struct {
    // Shown in the menu, and printed on switch. Kept short: the menu shows
    // pictures rather than words, because reading English should not be the
    // entry fee for a child using this.
    const char *name;

    // Called once on switch-in. The arena has just been reset and the
    // framebuffer has just been cleared to white; the app allocates its
    // state and draws its first frame. It does NOT need to push: the
    // runtime pushes the whole panel after enter() returns, once.
    void (*enter)(void);

    // Called every iteration of the main loop. The app draws what changed
    // and pushes only that, via gfx_push / gfx_push_land.
    void (*tick)(const app_frame_t *f);

    // Optional. Called before the arena is reset. Only needed by an app
    // with something to persist; nothing has yet.
    void (*leave)(void);

    // Landscape apps are drawn through gfx's rotation and are held sideways
    // with the buttons along the top edge. Portrait apps (the sketchpad)
    // are not. The runtime uses this only to orient the menu overlay it
    // draws on top of them.
    bool landscape;

    // Opt in to shake. Off by default, deliberately: see sensors.h.
    bool wantsShake;
} app_t;

/* ---- the app table -----------------------------------------------------
 *
 * Defined in runtime.c. Index 0 is what boots. Adding an app is one entry
 * here plus one file, and a rebuild; there is no install step and no
 * scripting layer, because an interpreter between a sensor and a pixel is
 * exactly the latency this design exists to remove. Recompiling costs the
 * owner a minute and the child nothing.
 */
extern const app_t *const g_apps[];
extern const int g_appCount;

// Switches to an app by index. Safe to call from inside tick(): the switch
// is deferred to the end of the current frame, so an app is never torn down
// underneath its own stack frame.
void app_switch_to(int index);

// The index of the app currently running.
int app_current(void);

#endif // APP_H
