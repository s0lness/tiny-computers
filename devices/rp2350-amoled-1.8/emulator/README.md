# Emulator

Local browser emulator for the Waveshare RP2350-Touch-AMOLED-1.8. It runs the
firmware's real portable C code, compiled to `wasm32-freestanding`, rather
than maintaining a TypeScript copy of the apps. The browser supplies the
framebuffer surface, input and clock in place of the board hardware.

Read [`../AGENTS.md`](../AGENTS.md) before changing it. The architectural
contract and its limits are recorded in
[`../docs/decisions/0003-emulator-runs-the-real-apps.md`](../docs/decisions/0003-emulator-runs-the-real-apps.md).

## Run it

From the device directory:

```powershell
cd emulator
bun install --frozen-lockfile
bun run wasm/build.ts
bun run server.ts
```

Open <http://127.0.0.1:5330>. The server binds to localhost explicitly. Use
the page's quit button if it was launched without a visible console.

The Wasm build requires Zig. It defaults to
`C:\Users\sylve\tools\zig\zig.exe`; set `ZIG_EXE` to use another path. The
output is `wasm/dist/emu.wasm`, which is ignored by git. The server watches
that file and reloads connected pages after a rebuilt module is stable and
has valid Wasm magic bytes.

## What runs where

```text
wasm/build.ts        compiles the firmware sources with Zig
wasm/emu_shim.c      host implementations required by the portable runtime
wasm/emu_abi.h       contract between the firmware and browser
src/wasm.ts          loads and validates the module and device descriptor
src/main.ts          browser loop, controls, replay and live reload
src/panel.ts         presents RGB565 framebuffer pushes on canvas
src/device.ts        puck and button interaction
src/recorder.ts      bounded trace capture
src/replay.ts        deterministic reset-and-step replay
src/freeze.ts        screenshot, state bundle and annotations
src/audio.ts         plays samples synthesised by the firmware
server.ts            localhost server and artifact write routes
scripts/verify.ts    headless server and graceful-failure checks
```

The Wasm module contains `runtime_core.c`, `gfx.c`, `sound_synth.c` and every
file under `firmware/apps/`. Board-only code such as `runtime.c`, `sensors.c`,
`bootbtn.c` and `devlink.c` is not compiled into it.

## Verification

```powershell
bun run typecheck
bun run wasm/build.ts
bun run wasm/tests/repro-arena-not-zeroed.ts
bun run wasm/tests/repro-poweroff-gesture.ts
bun run wasm/tests/repro-ring-shrink-residue.ts
bun run wasm/tests/repro-switch-input.ts
bun run wasm/tests/repro-timer-boot-pwr-running-and-alarm.ts
bun run wasm/tests/repro-timer-swallows-pwr-short-with-boot.ts
bun run verify
```

`bun run verify` starts its own server on a random local port. With a built
module it checks that the device boots. Without one it checks that the page
shows an actionable error instead of going blank. It also verifies that the
freeze and trace write routes reject requests missing the custom anti-CSRF
header. Headless verification requires Chrome at its standard Windows path.

`bun run build.ts` creates a static review copy under `dist/`. It includes the
Wasm module when one has already been built. Static hosting cannot provide
quit, freeze, trace or live reload because those features require `server.ts`.

## Deliberate limits

The emulator is useful for app logic, layout, raster output, app switching,
repeatable input traces and regression tests. It does not model core1, I2C,
PIO, DMA, the physical touch controller, panel transfer timing, speaker
timbre, burn-in or power behaviour. Responsiveness and hardware-path claims
must still be measured on the board.

Freeze bundles are written under `freezes/` and traces under `traces/`. Both
directories are ignored by git. All write routes require
`x-rp2350-emulator: 1`; keep that guard and the matching client header on any
new route that changes local state.
