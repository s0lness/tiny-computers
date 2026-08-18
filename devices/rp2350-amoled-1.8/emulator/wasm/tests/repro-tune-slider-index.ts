// repro-tune-slider-index: every slider in the tunables panel must write to
// the knob whose name is printed above it.
//
// THE BUG. tunables.ts hides the sketchpad's six settled constants from the
// panel ("you can remove these from the emulator btw") but leaves them
// REGISTERED, because a test proves the tuning path is not a decorated no-op by
// setting one of them. The filter that hid them also renumbered what was left:
// the row's position in the filtered array was handed straight to
// emu_tune_set, which addresses the FULL registry. The six hidden knobs sit
// first, so every visible slider wrote six slots up - "pulsecycle" set "lift".
//
// WHY IT SURVIVED A DAY OF BEING LOOKED AT. The read-back used the same wrong
// index, so the number under the slider followed the thumb perfectly. The panel
// looked alive. Nothing it could touch was on screen. The owner moved the
// sliders repeatedly, on the emulator and reaching for the board, and said so
// three times before this was found: "quand je change les sliders, rien ne
// change ni sur l'émulateur ni sur l'appareil. Ça m'énerve."
//
// WHAT THIS ASSERTS, and the only thing worth asserting: setting a row moves
// exactly ONE knob, the one named on the row. Not "a set is accepted" (the
// broken version accepted every set), not "the value reads back" (it did).
//
// Run with (after `bun run emulator/wasm/build.ts`):
//
//   bun run emulator/wasm/tests/repro-tune-slider-index.ts

import { readFileSync } from "node:fs";
import { join } from "node:path";
import { HIDDEN_TUNABLES } from "../../src/tunables";

const WASM = join(import.meta.dir, "..", "dist", "emu.wasm");

const mod = await WebAssembly.compile(readFileSync(WASM));
const inst = await WebAssembly.instantiate(mod, {
  env: {
    js_log() {},
    sinf: Math.sin, cosf: Math.cos, atan2f: Math.atan2, sqrtf: Math.sqrt,
    fabsf: Math.abs, floorf: Math.floor, fmodf: (a: number, b: number) => a % b,
    powf: Math.pow, expf: Math.exp,
  },
});
const e = inst.exports as any;
const memory = inst.exports.memory as WebAssembly.Memory;
if (e.emu_init() !== 1) throw new Error("emu_init failed");

const u8 = new Uint8Array(memory.buffer);
let end = e.emu_device();
while (u8[end]) end++;
const all = JSON.parse(new TextDecoder().decode(u8.subarray(e.emu_device(), end)))
  .tunables as { id: string; min: number; max: number }[];

// The panel's own row list, built the same way buildTuneControls() builds it:
// filtered for display, each row still carrying its position in the FULL
// registry. If that mapping is ever collapsed back to the filtered position,
// this file is what says so.
const visible = all
  .map((t, registryIndex) => ({ ...t, registryIndex }))
  .filter((t) => !HIDDEN_TUNABLES.has(t.id));

if (visible.length === 0) throw new Error("no visible tunables - the panel would be empty");

let failures = 0;
for (const row of visible) {
  const before = all.map((_, i) => e.emu_tune_get(i) as number);
  // 0.37 of the way along: inside every range, and not a value any knob is
  // likely to already hold, so "it did not move" cannot pass by coincidence.
  e.emu_tune_set(row.registryIndex, row.min + (row.max - row.min) * 0.37);
  const after = all.map((_, i) => e.emu_tune_get(i) as number);
  const moved = all
    .map((t, i) => ({ id: t.id, i }))
    .filter(({ i }) => Math.abs(after[i]! - before[i]!) > 1e-6);

  const ok = moved.length === 1 && moved[0]!.id === row.id;
  if (!ok) failures++;
  console.log(`${ok ? "ok  " : "FAIL"} slider "${row.id}" moved ${moved.map((m) => m.id).join(", ") || "nothing"}`);
  e.emu_tune_reset?.(row.registryIndex);
}

if (failures > 0) {
  console.error(`\n${failures}/${visible.length} sliders write to the wrong knob`);
  process.exit(1);
}
console.log(`\nall ${visible.length} visible sliders address their own knob`);
