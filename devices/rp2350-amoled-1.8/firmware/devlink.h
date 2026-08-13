/*
 * devlink: a tiny line-oriented command protocol over the same USB CDC port
 * the runtime already prints debug output to, so an agent can see and drive
 * the device without a human touching it.
 *
 * Integration contract:
 *   - Call devlink_init() once, after the framebuffer and every hook
 *     function below exist (typically right after gfx_init() succeeds,
 *     before the main loop starts). See runtime.c's "devlink wiring"
 *     section for the actual hook implementations.
 *   - Call devlink_poll() once per main-loop iteration. It never blocks
 *     waiting for input: it drains whatever bytes are already queued on
 *     stdin and returns. It only spends non-trivial time when a full SHOT
 *     command has been received, while it writes the screenshot out.
 *
 * See tools/README-devlink.md for the wire protocol.
 */
#ifndef DEVLINK_H
#define DEVLINK_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint16_t *fb;   // framebuffer, RGB565, byte-swapped (panel wants the
                     // opposite byte order to a CPU uint16_t; see gfx.h's
                     // px_swap for why). devlink reads this, never writes it.
    int w, h;        // framebuffer dimensions in pixels

    // Touch injection and erase. Owned by the caller (runtime.c); devlink
    // calls them synchronously from inside devlink_poll() and never on its
    // own, same as before. What runs underneath changed with the
    // single-binary runtime: these used to call straight into the
    // sketchpad's own stroke functions, and now they feed
    // sensors_inject_touch()/sensors_inject_erase() instead, so every app
    // sees an injected touch, not just the one app that used to own this
    // hook. devlink itself does not know or care about that change.
    void (*inject_down)(int x, int y);
    void (*inject_move)(int x, int y);
    void (*inject_up)(void);
    void (*erase)(void);

    // App navigation: added so an agent that can only screenshot whichever
    // app happens to be running is not stuck verifying one screen forever.
    int (*app_current)(void);           // the running app's index. Opaque as
                                         // far as devlink is concerned (it
                                         // only ever prints it back).
    const char *(*app_name)(int index); // name for an index, or NULL if
                                         // there isn't one at that index.
    bool (*app_switch)(int index);      // requests a switch to index;
                                         // returns false, and changes
                                         // nothing, if index is out of range.
} devlink_hooks_t;

// Copies *hooks by value. Call once, after hooks->fb has been allocated and
// every function pointer above points at a working function.
void devlink_init(const devlink_hooks_t *hooks);

// Call once per main-loop iteration. Non-blocking: reads whatever is already
// buffered on stdin with getchar_timeout_us(0) and returns as soon as
// nothing more is waiting. The one exception is emitting a SHOT reply, which
// is bounded (at most w*h RLE pairs, base64-encoded) and does not wait on
// anything external, so it cannot stall the caller's main loop indefinitely.
void devlink_poll(void);

#endif // DEVLINK_H
