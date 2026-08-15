// repro-touch-dropout-clock-set: setting the clock (firmware/apps/clock.c)
// with a touch stream that misbehaves the way the real controller does,
// rather than with a clean pointer.
//
// WHY THIS FILE EXISTS SEPARATELY FROM feature-clock.ts, and why it is not
// optional: this project has already shipped one feature that worked in the
// emulator and did not work in the owner's hands, on exactly this hazard -
// the sketchpad's palette, whose long press was reset dozens of times a
// second by a controller reporting a lost contact that never happened
// (repro-touch-dropout-palette-open.ts). Its own feature test drove clean
// input and passed all 22 of its checks throughout. AGENTS.md's rule since
// then: a feature driven by touch needs BOTH kinds of file.
//
// WHAT IS ACTUALLY AT RISK HERE, stated before it is measured. The clock's
// set gesture (hold BOOT, slide over the pair you want) has two pieces that
// a dropout could plausibly break:
//
//   1. THE FIELD LATCH. Which pair is being set is decided when the finger
//      LANDS, and a dropout followed by a re-acquire looks exactly like a
//      new landing. If the app re-picked the field from a stale or default
//      position, a dropout mid-slide would silently start setting the hours
//      while the owner is setting the minutes.
//   2. THE COMMIT. The gesture ends when BOOT is released, not when the
//      finger lifts. If the app tied the commit to the finger instead, a
//      dropout at the wrong instant would either commit early (a half-dialled
//      time) or never commit at all.
//
// The design's answer to both is that the value is ABSOLUTE - where the
// finger is IS the number, re-derived on every report - so a lost report
// changes nothing rather than losing an increment. This file is what turns
// that argument into a measurement.
//
// THE PROFILE. Identical to the other dropout files: 34 dropout episodes per
// second, back-calculated in repro-touch-dropout-stroke-start.ts from a real
// hardware session (798 dropouts over ~23s of finger-down time). At the
// controller's 60Hz report rate that is a per-report loss probability of
// 0.57, so contact is ABSENT on most individual reports.
//
// Run with (after `bun run emulator/wasm/build.ts`):
//
//   bun run emulator/wasm/tests/repro-touch-dropout-clock-set.ts
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { TouchSim, type TouchReport } from "../../src/touchsim";
import { TOUCHSIM_DEFAULTS } from "../../src/constants";

const WASM_PATH = join(import.meta.dir, "..", "dist", "emu.wasm");
const PANEL_W = 368;
const PANEL_H = 448;
const LAND_H = PANEL_W;
const LAND_W = PANEL_H;
const APP_CLOCK = -3; // APP_INDEX_CLOCK - see runtime_core.h for why the
                      // clock is not in g_apps[]
const BEZEL = 10;     // gfx.h PANEL_BEZEL_MARGIN_PX

let passCount = 0;
let failCount = 0;
function check(label: string, ok: boolean, detail?: string) {
    const status = ok ? "PASS" : "FAIL";
    if (ok) passCount++; else failCount++;
    console.log(`[${status}] ${label}${detail ? " - " + detail : ""}`);
}

async function loadDevice() {
    const mod = await WebAssembly.compile(readFileSync(WASM_PATH));
    let memory!: WebAssembly.Memory;
    const dec = new TextDecoder();
    const fwLog: string[] = [];
    const instance = await WebAssembly.instantiate(mod, {
        env: {
            js_log(ptr: number, len: number) { fwLog.push(dec.decode(new Uint8Array(memory.buffer, ptr, len)).trimEnd()); },
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
    });
    memory = instance.exports.memory as WebAssembly.Memory;
    const exp = instance.exports as any;
    if (exp.emu_init() !== 1) throw new Error("emu_init() failed");
    exp.emu_tick(0);
    exp.emu_app_switch(APP_CLOCK);
    exp.emu_tick(20);
    exp.emu_tick(40);
    fwLog.length = 0;
    return {
        boot(down: boolean) { exp.emu_button(0, down ? 1 : 0); },
        // The report is already in PANEL coordinates (TouchSim works in the
        // panel's own space, like the real controller).
        feed(report: TouchReport, nowMs: number) {
            exp.emu_touch(report.fingers === 1 ? 1 : 0, report.x, report.y);
            exp.emu_tick(nowMs);
        },
        tick(nowMs: number) { exp.emu_tick(nowMs); },
        drainLog(): string[] { const out = fwLog.slice(); fwLog.length = 0; return out; },
    };
}

// clock.c's value_from_axis(), mirrored, plus the position that produces a
// wanted value - the same helper feature-clock.ts uses, kept here rather
// than shared because every test in this directory is self-contained.
function valueFromAxis(v: number, lo: number, hi: number, range: number): number {
    if (v <= lo) return 0;
    if (v >= hi) return range - 1;
    return Math.floor(((v - lo) * range) / (hi - lo));
}
function axisForValue(want: number, lo: number, hi: number, range: number): number {
    let first = -1, last = -1;
    for (let v = lo; v <= hi; v++) {
        if (valueFromAxis(v, lo, hi, range) === want) { if (first < 0) first = v; last = v; }
    }
    return Math.round((first + last) / 2);
}

// The panel point that sets `field` (0 = hours, 1 = minutes) to `value` in
// the long-ways layout, which is the one the clock comes up in: the pair is
// chosen by landscape x, the value carried by landscape y, and gfx.h maps
// landscape (lx, ly) to panel (PANEL_W-1-ly, lx).
function pointFor(field: number, value: number): [number, number] {
    const ly = axisForValue(value, BEZEL, LAND_H - BEZEL, field === 0 ? 24 : 60);
    const lx = field === 0 ? LAND_W / 4 : (3 * LAND_W) / 4;
    return [PANEL_W - 1 - ly, lx];
}

const dropoutHeavy = { ...TOUCHSIM_DEFAULTS, dropoutsEnabled: true, dropoutsPerSec: 34, straysEnabled: false };
const STEP_MS = 1000 / dropoutHeavy.reportRateHz;

async function main() {
    console.log("=== setting the clock under a dropout-heavy touch stream ===\n");
    console.log(`    profile: ${dropoutHeavy.dropoutsPerSec} dropout episodes/sec at ${dropoutHeavy.reportRateHz}Hz ` +
        `(p=${(dropoutHeavy.dropoutsPerSec / dropoutHeavy.reportRateHz).toFixed(2)} per report)\n`);

    // ---- A. the dialled-in time is the same as with a clean stream -------
    const TRIALS = 25;
    const HOLD_MS = 900;
    const WANT_H = 8, WANT_M = 38;
    let exact = 0;
    let commits = 0;
    let totalDropouts = 0;
    const wrong: string[] = [];

    for (let i = 0; i < TRIALS; i++) {
        const dev = await loadDevice();
        const sim = new TouchSim(dropoutHeavy, PANEL_W, PANEL_H);
        let t = 1000;
        dev.boot(true);

        for (const [field, value] of [[0, WANT_H], [1, WANT_M]] as const) {
            const [px, py] = pointFor(field, value);
            sim.setPointer(true, px, py);
            for (let held = 0; held < HOLD_MS; held += STEP_MS) {
                t += STEP_MS;
                dev.feed(sim.poll(t), t);
            }
            // Lift between the two pairs, the way a hand does.
            sim.setPointer(false, px, py);
            for (let lifted = 0; lifted < 120; lifted += STEP_MS) {
                t += STEP_MS;
                dev.feed(sim.poll(t), t);
            }
        }

        dev.boot(false);
        t += STEP_MS;
        dev.tick(t);
        totalDropouts += sim.simDropouts;

        const lines = dev.drainLog().filter((l) => l.includes("clock: set to"));
        commits += lines.length;
        const want = `clock: set to ${String(WANT_H).padStart(2, "0")}:${WANT_M}`;
        if (lines.length === 1 && lines[0] === want) exact++;
        else if (wrong.length < 3) wrong.push(lines.join(" | ") || "(no commit)");
    }

    console.log(`    ${totalDropouts} dropout episodes simulated across ${TRIALS} settings\n`);
    check("the time that lands is EXACTLY the time the finger was pointing at, every single time",
        exact === TRIALS, `${exact}/${TRIALS} exact${wrong.length ? "; wrong: " + wrong.join(" ; ") : ""}`);
    check("one commit per hold, however many times contact was lost inside it",
        commits === TRIALS, `${commits} commits over ${TRIALS} holds`);

    // ---- B. a dropout cannot hand the gesture to the other pair ----------
    // The field is latched when the finger lands, and a re-acquire after a
    // dropout is another landing. It can only be the SAME pair, because the
    // finger has not moved - which is the property being measured, not
    // assumed: the hours must come out at their seeded value (12, from an
    // unset clock) after a minutes-only gesture, over and over.
    let hoursHeld = 0;
    const strayHours: string[] = [];
    for (let i = 0; i < TRIALS; i++) {
        const dev = await loadDevice();
        const sim = new TouchSim(dropoutHeavy, PANEL_W, PANEL_H);
        let t = 1000;
        dev.boot(true);
        const [px, py] = pointFor(1, 45);
        sim.setPointer(true, px, py);
        for (let held = 0; held < 1200; held += STEP_MS) {
            t += STEP_MS;
            dev.feed(sim.poll(t), t);
        }
        dev.boot(false);
        t += STEP_MS;
        dev.tick(t);
        const line = dev.drainLog().findLast((l) => l.includes("clock: set to")) ?? "(no commit)";
        if (line === "clock: set to 12:45") hoursHeld++;
        else if (strayHours.length < 3) strayHours.push(line);
    }
    check("a minutes gesture never becomes an hours gesture, however often contact is lost mid-slide",
        hoursHeld === TRIALS, `${hoursHeld}/${TRIALS} kept the hours at 12${strayHours.length ? "; got: " + strayHours.join(" ; ") : ""}`);

    // ---- C. the commit does not depend on the finger being down ----------
    // BOOT is what ends the gesture. A dropout at the instant of release
    // must not swallow the commit, and lifting deliberately before releasing
    // BOOT must not either.
    let committedAfterLift = 0;
    for (let i = 0; i < TRIALS; i++) {
        const dev = await loadDevice();
        const sim = new TouchSim(dropoutHeavy, PANEL_W, PANEL_H);
        let t = 1000;
        dev.boot(true);
        const [px, py] = pointFor(1, 20);
        sim.setPointer(true, px, py);
        for (let held = 0; held < 900; held += STEP_MS) { t += STEP_MS; dev.feed(sim.poll(t), t); }
        sim.setPointer(false, px, py);
        for (let lifted = 0; lifted < 600; lifted += STEP_MS) { t += STEP_MS; dev.feed(sim.poll(t), t); }
        dev.boot(false);
        t += STEP_MS;
        dev.tick(t);
        if (dev.drainLog().some((l) => l === "clock: set to 12:20")) committedAfterLift++;
    }
    check("lifting the finger first and releasing BOOT afterwards still commits what was dialled in",
        committedAfterLift === TRIALS, `${committedAfterLift}/${TRIALS}`);

    console.log(`\n${passCount} passed, ${failCount} failed`);
    if (failCount > 0) process.exit(1);
}

main();
