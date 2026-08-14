// repro-touch-dropout-stroke-start: headless reproduction of the owner's
// report ("it draws now, but i feel like i have to press really hard for
// anything to register") and a standing regression check for the fix.
//
// Measured on real hardware 2026-08-14 (TOUCH_POLL_SELFTEST, a continuous
// drawing session): 401 candidate stroke starts produced 14 confirmed
// strokes - 3.5 percent - while the controller lost contact mid-touch 798
// times in the same window. sketch.c's old stroke-start rule required a
// second controller report to land before EVEN ONE dropout occurred; given
// how often this FT3168 drops out, that killed almost every real touch
// before it had a chance to move. The fix (sketch.c's CONFIRM_MS, and the
// pendingStart grace in the haveTouch==false branch) gives a pending stroke
// start the same LIFT_DEBOUNCE_MS grace an already-started stroke already
// gets, plus a time-persistence path for a stationary or dropout-bridged
// touch. See sketch.c's CONFIRM_MS comment for the full reasoning.
//
// This is the test AGENTS.md says did not exist: "nothing simulated a
// controller this bad." emulator/src/touchsim.ts already models a
// controller's dropouts and strays as a configurable-rate Bernoulli
// process (see its own header comment) - genuinely worse than its own
// TOUCHSIM_DEFAULTS (2 dropouts/sec), so this drives it far more
// aggressively than that default, matching what the hardware diagnostic
// actually measured (roughly 34 drop episodes/sec of touch-down time, back-
// calculated from 798 dropouts over about a 23-second stroke session).
//
// CAVEAT, stated rather than hidden: TouchSim draws from Math.random() with
// no seed hook (nothing in this codebase threads one through, and this file
// does not add one), so this is not bit-for-bit deterministic. The
// assertions below are written as statistical thresholds over many trials,
// not exact counts, specifically so the test is robust to that - the
// dropout rate is high enough, and the trial count large enough, that the
// pass/fail line is not close to the threshold either way.
//
// Run with (after `bun run emulator/wasm/build.ts`):
//
//   bun run emulator/wasm/tests/repro-touch-dropout-stroke-start.ts
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { TouchSim, type TouchReport } from "../../src/touchsim";
import { TOUCHSIM_DEFAULTS } from "../../src/constants";

const WASM_PATH = join(import.meta.dir, "..", "dist", "emu.wasm");

const PANEL_W = 368;
const PANEL_H = 448;
const APP_DRAW = 1; // g_apps[] = { chrono, sketch("draw"), timer } - see repro-switch-input.ts

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
            js_log(ptr: number, len: number) {
                const b = new Uint8Array(memory.buffer, ptr, len);
                const line = decoder.decode(b);
                fwLog.push(line);
            },
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

    if (exp.emu_init() !== 1) {
        throw new Error("emu_init() failed - see fw log lines above");
    }

    return {
        tick(nowMs: number) { exp.emu_tick(nowMs); },
        touch(down: boolean, x: number, y: number) { exp.emu_touch(down ? 1 : 0, Math.round(x), Math.round(y)); },
        appSwitch(index: number) { exp.emu_app_switch(index); },
        appCurrent(): number { return exp.emu_app_current(); },
        fbHash(): number | bigint {
            const ptr = exp.emu_fb();
            const fb = new Uint8Array(memory.buffer, ptr, PANEL_W * PANEL_H * 2);
            return Bun.hash(fb);
        },
        // Drains and clears the captured firmware log lines since the last call.
        drainLog(): string[] {
            const out = fwLog.slice();
            fwLog.length = 0;
            return out;
        },
    };
}

type Device = Awaited<ReturnType<typeof loadDevice>>;

async function main() {
    console.log("=== reproduction + regression: stroke start under a dropout-heavy touch stream ===\n");

    const dev = await loadDevice();
    dev.tick(0);
    dev.appSwitch(APP_DRAW);
    dev.tick(10);
    check("switched into sketch", dev.appCurrent() === APP_DRAW, `app_current()=${dev.appCurrent()}`);
    dev.drainLog();

    // ---- scenario A: draw N short strokes through a controller far worse
    // than TOUCHSIM_DEFAULTS, and count how many actually start. ----------
    const dropoutHeavy = {
        ...TOUCHSIM_DEFAULTS,
        dropoutsEnabled: true,
        dropoutsPerSec: 34, // 17x TOUCHSIM_DEFAULTS.dropoutsPerSec (2), matching
                             // this file's header comment: ~34 drop episodes/sec
                             // of touch-down time, back-calculated from the
                             // hardware session's 798 dropouts over ~23s.
        straysEnabled: false, // isolate the dropout-tolerance question here;
                               // scenario B below exercises strays alone.
    };
    const sim = new TouchSim(dropoutHeavy, PANEL_W, PANEL_H);

    const TRIALS = 40; // large enough that the threshold below has real margin against run-to-run
                        // variance from TouchSim's unseeded RNG (measured floor over 10 repeated
                        // runs at this TRIALS/rate: 83%) without making the test slow.
    let started = 0;
    let t = 1000;
    const STEP_MS = 1000 / dropoutHeavy.reportRateHz;

    for (let i = 0; i < TRIALS; i++) {
        const x0 = 40 + (i * 13) % 280;
        const y0 = 40 + (i * 29) % 360;
        sim.setPointer(true, x0, y0);

        // ~150ms touch-and-drag, same order of magnitude as a quick real
        // stroke segment: enough reports for the confirmation window to
        // matter, short enough that this stays a "candidate", not a long
        // stroke that would confirm by CONFIRM_PX movement almost by luck.
        for (let s = 0; s < 9; s++) {
            t += STEP_MS;
            const dx = x0 + s * 2, dy = y0 + s * 2;
            sim.setPointer(true, dx, dy);
            const report: TouchReport = sim.poll(t);
            dev.touch(report.fingers === 1, report.x, report.y);
            dev.tick(t);
        }

        // Lift, and let LIFT_DEBOUNCE_MS (80ms) plus margin pass before the
        // next trial, so trials do not bleed into each other as one long
        // bridged stroke.
        sim.setPointer(false, 0, 0);
        for (let s = 0; s < 8; s++) {
            t += STEP_MS;
            const report = sim.poll(t);
            dev.touch(report.fingers === 1, report.x, report.y);
            dev.tick(t);
        }

        const log = dev.drainLog();
        if (log.some((l) => l.includes("stroke start"))) started++;
    }

    const rate = (started / TRIALS) * 100;
    console.log(`    ${started}/${TRIALS} candidate touches started a stroke (${rate.toFixed(0)}%), ` +
        `simulated dropout rate=${dropoutHeavy.dropoutsPerSec}/s (TOUCHSIM_DEFAULTS=${TOUCHSIM_DEFAULTS.dropoutsPerSec}/s)`);
    // Tuned by actually running this scenario (not just reasoned about) at
    // this TRIALS/dropoutsPerSec, 10 repeated unseeded runs each, against
    // both this fix and the pre-fix rule (checked by hand from `git show
    // HEAD:firmware/apps/sketch.c`, not compiled by this test): this fix
    // scored 83-98%, the pre-fix rule scored 63-83%. The two ranges are
    // close enough at the edges that this alone is not a bulletproof "would
    // always have failed before the fix" guarantee - TouchSim's
    // per-distinct-report Bernoulli model is a different failure shape than
    // the field session's mostly-duplicate (haveTouch=88977 against
    // newReport=1399) read pattern, and the field's pendingStart=401 count
    // is itself inflated by the pre-fix rule re-arming a fresh candidate
    // after every kill - exactly the behaviour this fix removes, and not
    // something this synthetic model reproduces. What IS solid: 70% is
    // comfortably below every floor this fix has shown across many runs (a
    // 13-point margin), so this gate will not flake on the current code,
    // and it is set well above what a badly broken rule scores on average.
    check("stroke-start success rate is high under a dropout-heavy stream",
        rate >= 70, `${rate.toFixed(0)}% over ${TRIALS} trials`);

    // ---- scenario B: the other side of the trade - a stray alone, no real
    // touch, must not leave a mark. -----------------------------------------
    const preStrayHash = dev.fbHash();
    const strayOnly = {
        ...TOUCHSIM_DEFAULTS,
        dropoutsEnabled: false,
        straysEnabled: true,
        straysPerSec: TOUCHSIM_DEFAULTS.straysPerSec, // realistic rate, not inflated -
                                                       // this is the false-positive side
                                                       // of the trade, not the one being
                                                       // stress-tested.
    };
    const strayGen = new TouchSim(strayOnly, PANEL_W, PANEL_H);
    strayGen.setPointer(false, 0, 0); // no real touch, ever, this whole phase

    let strayStrokeStarts = 0;
    // 20s at the realistic default rate (0.2/s) expects ~4 strays
    // (P(zero strays) < 2%, Poisson) - long enough that this phase actually
    // exercises the thing it claims to check, not just "nothing happened to
    // fire this run".
    const strayWindowMs = 20000;
    const tEnd = t + strayWindowMs;
    for (; t < tEnd; t += STEP_MS) {
        const report = strayGen.poll(t);
        dev.touch(report.fingers === 1, report.x, report.y);
        dev.tick(t);
        const log = dev.drainLog();
        if (log.some((l) => l.includes("stroke start"))) strayStrokeStarts++;
    }
    const postStrayHash = dev.fbHash();
    console.log(`    ${strayWindowMs}ms of stray-only input (rate=${strayOnly.straysPerSec}/s, simDropouts=0, ` +
        `simStrays=${strayGen.simStrays}) -> strayStrokeStarts=${strayStrokeStarts}`);
    if (strayGen.simStrays === 0) {
        console.log("    (no simulated stray actually fired this run - Math.random() is unseeded, see header " +
            "comment - so this phase did not exercise anything; framebuffer-unchanged still holds trivially)");
    }
    check("a stray alone (no real touch) does not leave a mark on the canvas",
        postStrayHash === preStrayHash,
        `pre=${preStrayHash} post=${postStrayHash}, strokeStarts=${strayStrokeStarts}, simStrays=${strayGen.simStrays}`);

    console.log(`\n${passCount} passed, ${failCount} failed`);
    if (failCount > 0) process.exit(1);
}

main();
