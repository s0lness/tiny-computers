// build.ts: compiles the real firmware (runtime_core.c, gfx.c, sound_synth.c,
// every app under firmware/apps/) plus this directory's emu_shim.c to a
// single wasm32-freestanding module, per docs/decisions/0003-emulator-runs-
// the-real-apps.md. TypeScript, run with `bun run emulator/wasm/build.ts`, not
// .js/.mjs - see the project owner's standing rule (AGENTS.md / CLAUDE.md):
// build scripts are source too.
//
// Toolchain notes that shaped the flags below, found by actually invoking
// zig 0.16.0 rather than assumed from its docs:
//
//   - `-Wl,--allow-undefined` (the flag this file's task brief expected to
//     need) is rejected outright by zig cc: "unsupported linker arg". The
//     flag that actually leaves undefined externs as wasm imports for this
//     target is `-Wl,--import-symbols` (a zig/lld wasm-specific option, only
//     reachable through -Wl, from the `cc` frontend). Verified by inspecting
//     the built module with `WebAssembly.Module.imports()`: sinf/floorf
//     etc. come through as env.* imports exactly as emu_abi.h documents,
//     with no other flag needed.
//   - `-Wl,--export-dynamic` was tried first for "export every emu_*
//     symbol" (matching this task's original invocation) but is flaky on
//     this zig build for wasm32-freestanding: it intermittently segfaults
//     the linker and, even when it does not crash, does not reliably export
//     C functions with default visibility. Dropped in favour of one
//     `-Wl,--export=<name>` per emu_* symbol below, which is deterministic
//     and was verified the same way (Module.exports()).
//   - zig's freestanding wasm32 target ships NO <stdlib.h>, <math.h> or
//     <stdio.h> at all (a real "file not found", not a link error) - it
//     takes the C standard's freestanding/hosted split literally, unlike a
//     hosted target that would bundle a minimal libc for these. gfx.c and
//     several apps `#include` all three as real, unmodified firmware code,
//     so shim/ carries stand-ins for those three IN ADDITION TO the
//     DEV_Config.h/AMOLED_1in8.h vendor-header shims this task named up
//     front. See each shim header's own comment for exactly what it
//     provides and why.
//
// None of the above changes what the JS host has to implement: js_log plus
// the eight math functions, exactly emu_abi.h's documented list. Everything
// this file works around (malloc, printf, the three extra math functions)
// is compiled INTO the module by emu_shim.c or shim/math.h, never imported.
import { existsSync, mkdirSync, statSync } from "node:fs";
import { join, resolve } from "node:path";

const WASM_DIR = import.meta.dir; // emulator/wasm
const DEVICE_ROOT = resolve(WASM_DIR, "..", ".."); // devices/rp2350-amoled-1.8
const FIRMWARE = join(DEVICE_ROOT, "firmware");
const DIST = join(WASM_DIR, "dist");
const OUT = join(DIST, "emu.wasm");

const ZIG = process.env.ZIG_EXE ?? "C:\\Users\\sylve\\tools\\zig\\zig.exe";

// Every symbol emu_abi.h declares. Exported explicitly (see the header
// comment above on why --export-dynamic was dropped) rather than derived by
// parsing emu_abi.h, so a symbol added there and forgotten here fails loudly
// (an undefined-export link error) instead of silently missing from the
// module the JS side expects.
const EMU_EXPORTS = [
  "emu_device",
  "emu_init",
  "emu_tick",
  "emu_fb",
  "emu_push_count",
  "emu_push_x",
  "emu_push_y",
  "emu_push_w",
  "emu_push_h",
  "emu_touch",
  "emu_button",
  "emu_button_verdict",
  "emu_sensor_event",
  "emu_app_current",
  "emu_app_switch",
  "emu_sound_sample_rate",
  "emu_sound_play_seq",
  "emu_sound_stop_seq",
  "emu_sound_buffer",
  "emu_sound_frames",
];

const SOURCES = [
  join(WASM_DIR, "emu_shim.c"),
  join(FIRMWARE, "runtime", "runtime_core.c"),
  join(FIRMWARE, "runtime", "gfx.c"),
  join(FIRMWARE, "runtime", "sound_synth.c"),
  join(FIRMWARE, "apps", "digits.c"),
  join(FIRMWARE, "apps", "chrono.c"),
  join(FIRMWARE, "apps", "sketch.c"),
  join(FIRMWARE, "apps", "menu.c"),
  join(FIRMWARE, "apps", "timer.c"),
  join(FIRMWARE, "apps", "shapes.c"),
];

// shim/ goes FIRST: it is what makes the real firmware sources compile
// unmodified (DEV_Config.h and AMOLED_1in8.h stand in for the vendor
// headers; stdlib.h/math.h/stdio.h stand in for libc headers this target
// does not ship - see this file's header comment). runtime/ and apps/ give
// the bare-filename #includes those directories' own files already use on
// the board (app.h, gfx.h, sensors.h, runtime_core.h, digits.h, menu.h).
const INCLUDES = [
  join(WASM_DIR, "shim"),
  join(FIRMWARE, "runtime"),
  join(FIRMWARE, "apps"),
  WASM_DIR, // emu_abi.h, included bare from emu_shim.c
];

if (!existsSync(ZIG)) {
  console.error(`zig not found at ${ZIG} (set ZIG_EXE to override)`);
  process.exit(1);
}
for (const src of SOURCES) {
  if (!existsSync(src)) {
    console.error(`source not found: ${src}`);
    process.exit(1);
  }
}

mkdirSync(DIST, { recursive: true });

const args = [
  "cc",
  "-target", "wasm32-freestanding",
  "-O2",
  "-nostdlib",
  "-Wl,--no-entry",
  "-Wl,--import-symbols", // undefined externs (js_log, the math imports)
                           // become real wasm imports instead of a hard
                           // link error - see this file's header comment.
  ...EMU_EXPORTS.map((name) => `-Wl,--export=${name}`),
  ...INCLUDES.flatMap((dir) => ["-I", dir]),
  ...SOURCES,
  "-o", OUT,
];

console.log(`${ZIG} ${args.join(" ")}`);
const result = Bun.spawnSync([ZIG, ...args], { stdout: "inherit", stderr: "inherit" });

if (!result.success) {
  console.error(`zig cc exited ${result.exitCode}`);
  process.exit(result.exitCode ?? 1);
}

const size = statSync(OUT).size;
console.log(`built ${OUT} (${size} bytes)`);
