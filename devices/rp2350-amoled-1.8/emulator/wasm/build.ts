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
import { existsSync, mkdirSync, renameSync, rmSync, statSync } from "node:fs";
import { join, resolve } from "node:path";

const WASM_DIR = import.meta.dir; // emulator/wasm
const DEVICE_ROOT = resolve(WASM_DIR, "..", ".."); // devices/rp2350-amoled-1.8
const FIRMWARE = join(DEVICE_ROOT, "firmware");
const DIST = join(WASM_DIR, "dist");
const OUT = join(DIST, "emu.wasm");

// The linker writes HERE, and the result is renamed onto OUT only once it has
// succeeded. Two reasons, and the second one cost an afternoon twice.
//
// A failed or crashed link leaves a truncated file behind, which the next
// test run happily loads as if it were a module.
//
// And more importantly, this repo is worked on by more than one agent at a
// time, so a second `zig cc` writing dist/emu.wasm while a test is reading it
// is entirely normal. A torn read compiles (the wasm prefix is valid) and
// then fails at the first call with "call_indirect to a signature that does
// not match", which reads exactly like a firmware bug in whatever app you
// happen to be working on and is nothing of the sort. A rename is atomic, so
// a reader sees either the whole old module or the whole new one, never half
// of one. The pid keeps two concurrent builders off each other's temp file.
const OUT_TMP = `${OUT}.tmp-${process.pid}`;

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
  "emu_sensor_vector",
  "emu_tilt",
  "emu_app_current",
  "emu_app_switch",
  "emu_arena_used",
  "emu_arena_capacity",
  "emu_tune_get",
  "emu_tune_set",
  "emu_tune_reset",
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
  // The orientation signal (firmware/runtime/tilt.h). Compiled in for the
  // same reason sound_synth.c is: the emulator must run the board's own
  // filter, axis mapping and hysteresis, not a browser-side second copy of
  // them that agrees on the day it is written and drifts after.
  join(FIRMWARE, "runtime", "tilt.c"),
  join(FIRMWARE, "runtime", "sound_synth.c"),
  join(FIRMWARE, "apps", "digits.c"),
  join(FIRMWARE, "apps", "chrono.c"),
  join(FIRMWARE, "apps", "sketch.c"),
  join(FIRMWARE, "apps", "menu.c"),
  join(FIRMWARE, "apps", "timer.c"),
  join(FIRMWARE, "apps", "four.c"),
  join(FIRMWARE, "apps", "level.c"),
  join(FIRMWARE, "apps", "clock.c"),
  join(FIRMWARE, "apps", "morpion.c"),
  join(FIRMWARE, "apps", "dino.c"),
  join(FIRMWARE, "apps", "bowling.c"),
  join(FIRMWARE, "apps", "tiltball.c"),
  join(FIRMWARE, "apps", "breakout.c"),
  join(FIRMWARE, "apps", "shapes.c"),
  // An empty translation unit unless the menu-stub define is set, and
  // deliberately absent from firmware/CMakeLists.txt so a stub app cannot
  // reach a uf2. See that file's own header comment; it is how the menu's
  // layout gets captured at six and twelve apps from the real firmware
  // rather than from a script that re-implements it.
  //
  // Nothing in this array's comments may be written in capitals: the gate
  // parses this literal with a regex that treats any capitalised token as a
  // directory constant, and fails the whole run when it does not recognise
  // one (tools/gate/freshness.ts, parseArrayLiteral). Loudly, which is that
  // tool's stated posture, but this is the cheaper end to fix.
  join(FIRMWARE, "apps", "stubapps.c"),
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
  // Sketchpad live tuning (firmware/apps/sketch.c's SKETCH_LIVE_TUNE),
  // always ON for the emulator build, unlike the board's default build
  // (off unless -DSKETCH_LIVE_TUNE=1 is passed to cmake, see
  // firmware/CMakeLists.txt): the emulator IS a development tool, so there
  // is no shipped-firmware state to protect here, and the tunables panel
  // (emu_abi.h's "tunables" section) has nothing to build controls from
  // without this.
  "-DSKETCH_LIVE_TUNE=1",
  // Extra flags from the environment, whitespace separated. This exists for
  // firmware that carries a compile-time LAYOUT VARIANT the owner is meant
  // to judge rather than a knob to be tuned - apps/four.c's
  // FOUR_FULL_HEIGHT, the first user - so that rendering the alternative is
  //
  //   EMU_EXTRA_DEFINES=-DFOUR_FULL_HEIGHT=0 bun run emulator/wasm/build.ts
  //
  // rather than editing a #define, building, capturing, and remembering to
  // put it back. It is deliberately NOT the tunables mechanism (emu_abi.h):
  // that one is for values a running module can change, and a layout that
  // decides the size of every rectangle on screen is not one of those.
  ...(process.env.EMU_EXTRA_DEFINES ?? "").split(/\s+/).filter(Boolean),
  ...EMU_EXPORTS.map((name) => `-Wl,--export=${name}`),
  ...INCLUDES.flatMap((dir) => ["-I", dir]),
  ...SOURCES,
  "-o", OUT_TMP,
];

console.log(`${ZIG} ${args.join(" ")}`);

// RETRIED, because this linker crashes at random on this toolchain. The
// header comment above already records that -Wl,--export-dynamic
// "intermittently segfaults the linker"; dropping it did not remove the
// intermittency, it only made it rarer. What is observed is an exit code
// with NO diagnostic output at all on inputs that link cleanly moments
// later. It is BURSTY rather than evenly random: observed here as roughly
// one failure in three most of the time, and once as twelve consecutive
// failures on unchanged sources that then built first try a minute later
// (with the identical command line run by hand succeeding in between).
// That shape points at contention over zig's shared global cache rather
// than at anything about these inputs, which matters because this repo is
// worked on by more than one agent at a time and a second `zig cc` running
// in another checkout is entirely normal.
//
// A one-shot build therefore reports a phantom failure often enough that
// "the build is broken" stops meaning anything, and the specific way that
// bites is silent: a caller that ignores the exit status runs its tests
// against the PREVIOUS emu.wasm and gets results for code it is not
// looking at. That happened while this retry was being written.
//
// So: retry, and let a genuine error (which does print diagnostics, and
// fails identically every time) survive all the attempts and be reported
// as itself.
const MAX_ATTEMPTS = 20;
let result = Bun.spawnSync([ZIG, ...args], { stdout: "inherit", stderr: "inherit" });
for (let attempt = 2; !result.success && attempt <= MAX_ATTEMPTS; attempt++) {
  console.error(`zig cc exited ${result.exitCode}; retrying (attempt ${attempt}/${MAX_ATTEMPTS})`);
  result = Bun.spawnSync([ZIG, ...args], { stdout: "inherit", stderr: "inherit" });
}

if (!result.success) {
  // Whatever half-written module the crashed linker left behind goes with it,
  // rather than sitting in dist/ as a *.tmp-1234 nobody will ever look at.
  // The previous emu.wasm is untouched and still valid, which is the other
  // half of what the temp file buys.
  rmSync(OUT_TMP, { force: true });
  console.error(`zig cc exited ${result.exitCode} on all ${MAX_ATTEMPTS} attempts - this one is real`);
  process.exit(result.exitCode ?? 1);
}

renameSync(OUT_TMP, OUT); // atomic: see OUT_TMP's own comment
const size = statSync(OUT).size;
console.log(`built ${OUT} (${size} bytes)`);
