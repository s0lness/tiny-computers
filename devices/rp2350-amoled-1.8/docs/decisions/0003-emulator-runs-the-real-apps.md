# 0003: The emulator runs the real apps, compiled to WebAssembly

Date: 2026-08-13
Status: accepted

## The problem this replaces

The first emulator was a careful line-by-line TypeScript port of
`firmware/main.c`. Its README is honest about the ambition, and it lists,
function by function, what was ported faithfully: the capsule rasteriser with
its MIN composition, the stroke pipeline, the simulated touch controller.

It was correct on the day it was written.

Then the firmware was restructured into a runtime with an app table, and the
port became a description of a program that no longer exists. That is not a
failure of care. It is the predictable behaviour of two implementations of one
thing: they agree exactly once, at the moment the second is written, and drift
from then on with no test that can notice.

What made the cost concrete: the owner found a real bug on the board (an app
launched from the menu stops responding to input) and asked for an emulator to
reproduce it in. A TypeScript port cannot reproduce a bug in C. It can only
reproduce bugs in the port. The one job the emulator was being asked to do was
the one job a reimplementation cannot do.

## Decision

**The emulator compiles the firmware's own C and runs it in the browser.**

`firmware/apps/*.c`, `firmware/runtime/gfx.c` and the portable part of the
runtime are compiled to `wasm32-freestanding` with `zig cc`, and the page
supplies what the board would have supplied: a surface to push pixels at, a
touch controller, two buttons, and a clock.

Not "the same algorithm". The same object code.

### Why this is possible at all

Because `firmware/runtime/app.h` already says an app never touches hardware.
The runtime owns the screen and the input, and an app is a struct of
callbacks over a framebuffer.

That interface was written to keep app code honest, not to enable emulation.
It turning out to be exactly the right emulation boundary is a good sign it
was cut in the right place: the seam that made the code clean is the seam the
hardware was hiding behind all along.

### The split this forces, which is worth having anyway

`runtime.c` divides into:

- **a portable core**: the arena, the app table, the deferred switch, and the
  frame dispatch. No pico-sdk, no hardware. Compiles for both targets.
- **the board's entry point**: startup ordering, core1, the watchdog, the
  PMIC key decoding, the USB link.

The bug the owner found lives in the first half. Being able to run that half
under a debugger, deterministically, one frame at a time, is the point.

### What is real in the emulator

- every app's logic, layout and redraw decisions, because it is the same code
- the framebuffer, the byte-swapped RGB565 format, the landscape rotation
- `gfx_push`'s 8-pixel row-length rounding, including the slide-left at the
  right edge (decision 0001)
- the arena, the app table, and the switch path

The pushed windows are reported back to the page and drawn as an overlay. The
panel defect behind decision 0001 was a defect about window geometry, and it
cost days of bisection precisely because the windows were invisible. They are
now the most visible thing on the screen.

### What is not real, and must never be trusted here

- **Timing.** The browser's clock drives the tick. Nothing reproduces the
  695us I2C touch read, the 12ms full-panel push, or the second core. Any
  question about responsiveness is a question for the board. Always.
- **The touch controller's defects.** The real FT3168 drops contact mid
  stroke and emits strays, which is the only reason the sketchpad has dropout
  bridging, glitch rejection and stroke-start confirmation. A clean mouse drag
  exercises none of that code. The old emulator's `touchsim` is kept for this
  reason and is the part of it most worth keeping.
- **The panel.** No burn-in, no brightness, no tearing.

## Cost

A C-to-wasm toolchain has to exist on the machine. `zig cc` is one 93MB
self-contained binary that cross-compiles C without a sysroot, which is why it
was chosen over emscripten (a much larger install for a much larger feature
set, none of which is wanted here) and over wasi-sdk (needs a WASI shim in the
browser for a program that makes no system calls).

Freestanding means no libc. The module imports about eight math functions and
one logging call from JavaScript, and its "heap" is a static array. For a
program whose only allocation is one 330KB framebuffer, that is less of a
constraint than it sounds, and it removes an entire class of divergence
between the two targets.

## Consequences

- An app is written once. Both targets get it.
- A design question ("does the timer ring read as a dial?") is answered in the
  browser in seconds, with the owner drawing on it directly via `markup`.
- A hardware question ("is the stop responsive?") is still answered only on
  the board, and the emulator must never be allowed to look like it answers
  it.
- Adding an app means adding it to one table, and it appears in both.
