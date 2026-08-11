/*
 * devlink: a tiny line-oriented command protocol over the same USB CDC port
 * main.c already prints debug output to, so an agent can see and drive the
 * device without a human touching it.
 *
 * Integration contract:
 *   - Call devlink_init() once, after the framebuffer and the four hook
 *     functions below exist (typically right after the framebuffer is
 *     allocated, before the main loop starts).
 *   - Call devlink_poll() once per main-loop iteration. It never blocks
 *     waiting for input: it drains whatever bytes are already queued on
 *     stdin and returns. It only spends non-trivial time when a full SHOT
 *     command has been received, while it writes the screenshot out.
 *
 * See tools/README-devlink.md for the wire protocol.
 */
#ifndef DEVLINK_H
#define DEVLINK_H

#include <stdint.h>

typedef struct {
    uint16_t *fb;   // framebuffer, RGB565, byte-swapped (panel wants the
                     // opposite byte order to a CPU uint16_t; see main.c's
                     // px_swap for why). devlink reads this, never writes it.
    int w, h;        // framebuffer dimensions in pixels

    // All four are owned by the caller (main.c). devlink calls them
    // synchronously from inside devlink_poll() and never on its own; there
    // is no timer or ISR involved anywhere in this module.
    void (*inject_down)(int x, int y);
    void (*inject_move)(int x, int y);
    void (*inject_up)(void);
    void (*erase)(void);
} devlink_hooks_t;

// Copies *hooks by value. Call once, after hooks->fb has been allocated and
// hooks->inject_down/move/up and hooks->erase point at working functions.
void devlink_init(const devlink_hooks_t *hooks);

// Call once per main-loop iteration. Non-blocking: reads whatever is already
// buffered on stdin with getchar_timeout_us(0) and returns as soon as
// nothing more is waiting. The one exception is emitting a SHOT reply, which
// is bounded (at most w*h RLE pairs, base64-encoded) and does not wait on
// anything external, so it cannot stall the caller's main loop indefinitely.
void devlink_poll(void);

#endif // DEVLINK_H
