// repro-sketch-palette-animation-starved: headless reproduction of the
// owner's report on real hardware, 2026-08-14, the day after the
// watchdog-reset fix landed and worked ("ça marche !"). One thing left:
//
//   "L'animation est super super saccadée. Concretement j'ai qu'une frame
//   intermediaire avant que ça fasse les carrés en gros."
//
// One intermediate frame across the whole ~280ms pop-in, not the eight or
// nine PALETTE_RENDER_MIN_MS (33ms, ~30fps) was sized to produce.
//
// THE CAUSE. palette_render_frame() was only ever reached from inside
// sketch_tick's touch-sample drain loop - called once per drained sample,
// throttled (after the watchdog fix) to at most once every 33ms. That
// throttle assumed samples would keep arriving often enough to have
// something to throttle. The palette's own gesture is a finger held
// still, and this controller - the owner's own account - "répète les
// mêmes coordonnées, le dedupe les jette, et il n'arrive presque rien":
// almost nothing reaches the drain loop at all while the position
// genuinely is not changing. Before the watchdog fix, a FLOOD of samples
// during the animation (that fix's own subject) hid this: hundreds of
// calls landing regardless meant a handful landing at useful moments was
// enough to look smooth. Bounding the call count correctly is what made
// the sample dependency visible - a case no earlier test covered, because
// every earlier test (including this one's own sibling,
// repro-sketch-palette-watchdog-reset.ts) drives the palette by FEEDING a
// stream of samples, which is exactly the condition that was masking
// this.
//
// THE FIX, in sketch.c: palette_advance_animation(), called once per
// sketch_tick() AFTER the drain loop - so on every tick, sample or not -
// reads whichever cell the last known touch position falls over and
// advances the animation from wall-clock time (f->nowMs) alone. The
// throttle and per-cell scoping palette_render_frame() already had are
// untouched; only how often it gets a CHANCE to do that check changed.
//
// THIS FILE reproduces the exact scenario no other test does: a genuine
// long press that opens the palette, then ZERO further touch samples -
// not even repeats - for the rest of the animation, only continued
// tick()s (which is what a real device does regardless of input; the
// emulator's own touch queue is purely event-driven, see touchsim's own
// header comment, so simply not calling touch() again is the emulator's
// exact equivalent of "the controller stopped producing anything new").
//
// Run with (after `bun run emulator/wasm/build.ts`):
//
//   bun run emulator/wasm/tests/repro-sketch-palette-animation-starved.ts
import { readFileSync } from "node:fs";
import { join } from "node:path";

const WASM_PATH = join(import.meta.dir, "..", "dist", "emu.wasm");
const APP_DRAW = 1; // g_apps[] = { chrono, sketch("draw"), timer, four }

const CONFIRM_MS = 40;
const LONG_PRESS_MS = 550;
const PALETTE_POP_MS = 240;
const PALETTE_STAGGER_MS = 20;
const PALETTE_RENDER_MIN_MS = 33;
const PALETTE_ANIM_TOTAL_MS = PALETTE_POP_MS + 2 * PALETTE_STAGGER_MS; // 280

let passCount = 0;
let failCount = 0;
function check(label: string, ok: boolean, detail?: string) {
    const status = ok ? "PASS" : "FAIL";
    if (ok) passCount++; else failCount++;
    console.log(`[${status}] ${label}${detail ? " - " + detail : ""}`);
}

async function loadDevice() {
    const bytes = readFileSync(WASM_PATH);
    const mod = await WebAssembly.compile(bytes);
    let memory!: WebAssembly.Memory;
    const decoder = new TextDecoder();
    const fwLog: string[] = [];
    const imports = {
        env: {
            js_log(ptr: number, len: number) { const b = new Uint8Array(memory.buffer, ptr, len); fwLog.push(decoder.decode(b)); },
            sinf: (x: number) => Math.sin(x),
            cosf: (x: number) => Math.cos(x),
            atan2f: (y: number, x: number) => Math.atan2(y, x),
            sqrtf: (x: number) => Math.sqrt(x),
            fabsf: (x: number) => Math.abs(x),
            floorf: (x: number) => Math.floor(x),
            fmodf: (x: number, y: number) => x % y,
            powf: (x: number, y: number) => Math.pow(x, y),
            expf: (x: number) => Math.exp(x),
        },
    };
    const instance = await WebAssembly.instantiate(mod, imports);
    memory = instance.exports.memory as WebAssembly.Memory;
    const exp = instance.exports as any;
    if (exp.emu_init() !== 1) throw new Error("emu_init() failed - see fw log lines above");

    return {
        tick(nowMs: number) { exp.emu_tick(nowMs); },
        touch(down: boolean, x: number, y: number) { exp.emu_touch(down ? 1 : 0, Math.round(x), Math.round(y)); },
        appSwitch(index: number) { exp.emu_app_switch(index); },
        drainLog(): string[] { const out = fwLog.slice(); fwLog.length = 0; return out; },
    };
}

function parseFrameLine(line: string): { kind: string; whitenPx: number; coverageEvals: number } | null {
    const m = line.match(/palette: frame kind=(\w+) whitenPx=(\d+) coverageEvals=(\d+)/);
    if (!m) return null;
    return { kind: m[1]!, whitenPx: Number(m[2]), coverageEvals: Number(m[3]) };
}

async function main() {
    console.log("=== reproduction + regression: the pop-in animation must not starve without touch samples ===\n");

    const dev = await loadDevice();
    dev.tick(0);
    dev.appSwitch(APP_DRAW);
    dev.tick(1000);
    dev.drainLog();

    // ---- open the palette the ordinary way: a genuine long press, real
    // touch samples, a realistic ~15ms report cadence. -------------------
    let t = 1000;
    const holdX = 183, holdY = 223; // the centre cell
    const HOLD_STEP_MS = 15;
    let opened = false;
    for (let held = 0; held < CONFIRM_MS + LONG_PRESS_MS + 100 && !opened; held += HOLD_STEP_MS) {
        t += HOLD_STEP_MS;
        dev.touch(true, holdX, holdY);
        dev.tick(t);
        if (dev.drainLog().some((l) => l.includes("palette: open"))) opened = true;
    }
    check("the palette opened via a genuine long press", opened);

    // ---- THE scenario: from here, no more touch() calls AT ALL - not one
    // more sample, not even a repeat of the same position - only tick()s,
    // spanning the whole pop-in animation and a margin past it. A real
    // device's own main loop keeps calling tick() regardless of whether
    // core1 queued anything; this is that, with the touch queue left
    // completely empty (see this file's own header comment on why that is
    // the emulator's honest equivalent of "the controller produced
    // nothing new"). ------------------------------------------------------
    const TICK_STEP_MS = 10; // a plausible main-loop cadence, unrelated to any report rate
    const WINDOW_MS = PALETTE_ANIM_TOTAL_MS + 80;
    const frames: { kind: string; whitenPx: number; coverageEvals: number }[] = [];
    for (let elapsed = 0; elapsed < WINDOW_MS; elapsed += TICK_STEP_MS) {
        t += TICK_STEP_MS;
        dev.tick(t); // no dev.touch() call - the queue stays empty
        for (const l of dev.drainLog()) {
            const f = parseFrameLine(l);
            if (f) frames.push(f);
        }
    }

    const ticksInWindow = Math.ceil(WINDOW_MS / TICK_STEP_MS);
    console.log(`    ${ticksInWindow} tick()s over ${WINDOW_MS}ms, ZERO touch samples after the one that opened it ` +
        `-> ${frames.length} "palette: frame" lines`);

    // THE REGRESSION THIS CATCHES: the report was "one intermediate
    // frame". A minimum comfortably above that, and comfortably below
    // what the throttle alone would allow if every tick rendered (which
    // would itself be the watchdog bug's own shape again), pins the
    // animation to actually animating without pinning it to an exact
    // frame count that a legitimate future retune of PALETTE_POP_MS or
    // PALETTE_RENDER_MIN_MS would then break for no real reason.
    const expectedFrames = Math.ceil(PALETTE_ANIM_TOTAL_MS / PALETTE_RENDER_MIN_MS); // ~9
    const minFrames = 5; // well above the reported "1", well below expectedFrames
    const maxFrames = ticksInWindow; // sanity: can never exceed one render per tick
    check(`the animation actually advances across multiple frames with zero touch samples (>=${minFrames}, ~${expectedFrames} expected)`,
        frames.length >= minFrames, `${frames.length} frames`);
    check(`frame count still stays within what the throttle allows (<=${maxFrames})`,
        frames.length <= maxFrames, `${frames.length} frames over ${ticksInWindow} ticks`);

    // ---- and once settled, continuing to tick with still no touch
    // samples must produce ZERO further frames - "the right number of
    // renders per second is zero, not thirty" once nothing is changing,
    // the same invariant the watchdog fix's own settled-path scoping
    // established, now checked specifically under a starved trace rather
    // than a fed one. ------------------------------------------------------
    const IDLE_TICKS = 50;
    let idleFrames = 0;
    for (let i = 0; i < IDLE_TICKS; i++) {
        t += TICK_STEP_MS;
        dev.tick(t);
        for (const l of dev.drainLog()) if (parseFrameLine(l)) idleFrames++;
    }
    console.log(`    ${IDLE_TICKS} more idle tick()s, settled, still zero touch samples -> ${idleFrames} more frames`);
    check("once settled, idling with no touch samples produces zero further frames", idleFrames === 0, `${idleFrames} frames`);

    console.log(`\n${passCount} passed, ${failCount} failed`);
    if (failCount > 0) process.exit(1);
}

main();
