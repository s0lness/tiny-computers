// repro-touch-dropout-palette-open: headless reproduction of the owner's
// report on real hardware, 2026-08-14, right after the palette shipped
// with its icons: "j'ai bien les icones mais la palette marche pas" - the
// palette does not open at all, even though the same gesture opened it
// reliably in the emulator (feature-sketch-palette.ts's own 22 checks, all
// clean input, all passing).
//
// REPRODUCED HEADLESS, no hardware in the loop, by driving the SAME
// TouchSim dropout profile repro-touch-dropout-stroke-start.ts already
// uses (34 dropout episodes/sec, back-calculated from that file's own
// hardware session) against a STATIONARY real pointer - a long press, held
// still, which is the palette's entire gesture (HOLD_STILL_RADIUS_PX). A
// scratch version of this scenario, run before the cause was known, opened
// the palette in roughly 1 trial in 4 (5/20 one run, similar across
// several) instead of reliably.
//
// THE CAUSE: sketch.c's stroke state machine used ONE clock
// (st->lastSampleMs) for two different questions - "how long since the
// finger last MOVED" (the speed/jump-allowance calc, which genuinely needs
// this) and "how long since the finger was last known to be DOWN AT ALL"
// (the genuine-lift check: `nowMs - lastSampleMs >= LIFT_DEBOUNCE_MS`).
// lastSampleMs only advances on newReport (an accepted move). A finger
// held perfectly still - by construction, since HOLD_STILL_RADIUS_PX keeps
// the palette's own hold gesture inside a 12px circle - stops producing
// newReport almost immediately, so lastSampleMs freezes at the moment of
// the last real movement. Any dropout landing more than LIFT_DEBOUNCE_MS
// (220ms) after that - trivial at 34 episodes/sec - was then believed as a
// genuine lift EVEN THOUGH CONTACT HAD BEEN REPORTED CONTINUOUSLY the
// whole time. The stroke ended, holdCandidate was cleared, and the whole
// confirm+hold sequence had to restart from zero - repeatedly, since the
// very next confirmed touch is immediately vulnerable to the same thing
// again. Fixed by splitting the two clocks the same way the not-yet-
// confirmed candidate already splits pendStartMs (when armed) from
// pendLastTouchMs (last contact seen) - see sketch.c's own lastContactMs
// struct comment for the full account. lastSampleMs is untouched and keeps
// its original, narrower job.
//
// WHY THE EMULATOR NEVER CAUGHT THIS: every prior test drove the palette
// with clean, dropout-free input (feature-sketch-palette.ts) or drove
// dropouts against ORDINARY DRAWING, which moves continuously and so kept
// newReport - and therefore lastSampleMs - fresh throughout
// (repro-touch-dropout-stroke-start.ts's scenarios A/C, both still 80-100%
// survival, both genuinely unaffected by this bug for exactly that
// reason). The palette's hold gesture was the first thing in this codebase
// that needs a finger to stay both DOWN and STILL for half a second, which
// is exactly the condition this bug needed to show itself.
//
// Run with (after `bun run emulator/wasm/build.ts`):
//
//   bun run emulator/wasm/tests/repro-touch-dropout-palette-open.ts
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { TouchSim, type TouchReport } from "../../src/touchsim";
import { TOUCHSIM_DEFAULTS } from "../../src/constants";

const WASM_PATH = join(import.meta.dir, "..", "dist", "emu.wasm");
const PANEL_W = 368;
const PANEL_H = 448;
const APP_DRAW = 1; // g_apps[] = { chrono, sketch("draw"), timer }

// Mirrors of sketch.c's own #defines - not live-tuned (unlike lift/confirm/
// pendgrace/minjump/maxjump/maxspeed, see SKETCH_LIVE_TUNE's own list),
// so lifted directly as constants here, same convention feature-sketch-
// palette.ts already uses for these same values.
const CONFIRM_MS = 40; // CONFIRM_MS_DEFAULT
const LONG_PRESS_MS = 550;
const LIFT_DEBOUNCE_MS_DEFAULT = 220;

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
        appCurrent(): number { return exp.emu_app_current(); },
        drainLog(): string[] { const out = fwLog.slice(); fwLog.length = 0; return out; },
    };
}

type Device = Awaited<ReturnType<typeof loadDevice>>;

async function main() {
    console.log("=== reproduction + regression: palette long-press under a dropout-heavy touch stream ===\n");

    const dev = await loadDevice();
    dev.tick(0);
    dev.appSwitch(APP_DRAW);
    dev.tick(10);
    check("switched into sketch", dev.appCurrent() === APP_DRAW, `app_current()=${dev.appCurrent()}`);
    dev.drainLog();

    // Same profile as repro-touch-dropout-stroke-start.ts's own dropoutHeavy
    // - see that file's header comment for the hardware numbers this rate
    // is back-calculated from (798 dropouts over ~23s of touch-down time).
    const dropoutHeavy = { ...TOUCHSIM_DEFAULTS, dropoutsEnabled: true, dropoutsPerSec: 34, straysEnabled: false };
    const STEP_MS = 1000 / dropoutHeavy.reportRateHz;

    // ---- scenario A: a stationary long press, under the dropout-heavy
    // profile, must open the palette. Generous window (well past CONFIRM_MS
    // + LONG_PRESS_MS): the bug this reproduces did not stop the palette
    // from EVER opening, it kept resetting the hold candidacy before it
    // could accumulate LONG_PRESS_MS of believed-continuous contact, so a
    // long enough window gives it several chances to eventually get lucky
    // even when broken - which is exactly why the threshold below is set
    // from what was actually measured broken vs fixed, not guessed. -------
    const TRIALS = 30;
    const HOLD_WINDOW_MS = 2500; // several LONG_PRESS_MS-lengths of headroom
    let opened = 0;
    let t = 1000;
    for (let i = 0; i < TRIALS; i++) {
        const x0 = 60 + (i * 17) % 250;
        const y0 = 60 + (i * 31) % 330;
        const sim = new TouchSim(dropoutHeavy, PANEL_W, PANEL_H);
        sim.setPointer(true, x0, y0); // held perfectly still: the palette's own gesture

        let didOpen = false;
        for (let held = 0; held < HOLD_WINDOW_MS; held += STEP_MS) {
            t += STEP_MS;
            const report: TouchReport = sim.poll(t);
            dev.touch(report.fingers === 1, report.x, report.y);
            dev.tick(t);
            if (dev.drainLog().some((l) => l.includes("palette: open"))) { didOpen = true; break; }
        }
        if (didOpen) opened++;

        // Release and settle well past any debounce before the next trial,
        // regardless of whether the palette ended up open (open_palette()
        // clears fingerDown itself; this just guarantees a clean idle
        // state either way).
        sim.setPointer(false, 0, 0);
        for (let s = 0; s < 40; s++) {
            t += STEP_MS;
            const report = sim.poll(t);
            dev.touch(report.fingers === 1, report.x, report.y);
            dev.tick(t);
        }
        dev.drainLog();
    }

    const rate = (opened / TRIALS) * 100;
    console.log(`    ${opened}/${TRIALS} stationary long-presses opened the palette (${rate.toFixed(0)}%), ` +
        `dropout rate=${dropoutHeavy.dropoutsPerSec}/s`);
    // Measured by actually running this scenario against the pre-fix code
    // (lastSampleMs used for the genuine-lift check) and the post-fix code
    // (lastContactMs split off): pre-fix scored 20-30% across several
    // 20-30 trial runs (the fix's own diagnosis run: 5/20 = 25%), post-fix
    // scored 100% across the same repeated runs. 90% sits with real margin
    // below the fixed floor and well above the broken ceiling, so this gate
    // has room in both directions and would have caught the regression it
    // exists for.
    check("a stationary long press reliably opens the palette under a dropout-heavy stream",
        rate >= 90, `${rate.toFixed(0)}% over ${TRIALS} trials`);

    // ---- scenario B: the fix must not break genuine lift detection. A
    // touch that is held only briefly (under LONG_PRESS_MS, so the palette
    // never opens) and then genuinely, totally released must still end the
    // stroke within a bounded time - lastContactMs has to keep tracking a
    // REAL departure just as faithfully as it now tracks a stationary
    // hold's continued presence, or this fix would have traded one bug for
    // its opposite (a stroke that never believes it was ever lifted). -----
    console.log("");
    const devB = await loadDevice();
    devB.tick(0);
    devB.appSwitch(APP_DRAW);
    devB.tick(10);
    devB.drainLog();

    const briefHoldSim = new TouchSim(dropoutHeavy, PANEL_W, PANEL_H);
    let tB = 1000;
    const BRIEF_HOLD_MS = 300; // comfortably under CONFIRM_MS + LONG_PRESS_MS (590ms):
                               // confirms as a stroke, never becomes a long press.
    briefHoldSim.setPointer(true, 150, 150);
    let openedDuringBriefHold = false;
    for (let held = 0; held < BRIEF_HOLD_MS; held += STEP_MS) {
        tB += STEP_MS;
        const report = briefHoldSim.poll(tB);
        devB.touch(report.fingers === 1, report.x, report.y);
        devB.tick(tB);
        if (devB.drainLog().some((l) => l.includes("palette: open"))) openedDuringBriefHold = true;
    }
    check("a brief hold (under LONG_PRESS_MS) does not open the palette", !openedDuringBriefHold);

    // Genuine, total release - no more simulated dropouts/strays, just
    // real contact gone - held for LIFT_DEBOUNCE_MS_DEFAULT plus a healthy
    // margin, pushed every step (see idleSettle's own reasoning in
    // repro-touch-dropout-stroke-start.ts: the drain loop only re-checks
    // the debounce when a fresh sample says haveTouch==false).
    briefHoldSim.setPointer(false, 0, 0);
    let sawStrokeEnd = false;
    const LIFT_WAIT_MS = LIFT_DEBOUNCE_MS_DEFAULT + 400;
    for (let waited = 0; waited < LIFT_WAIT_MS; waited += STEP_MS) {
        tB += STEP_MS;
        const report = briefHoldSim.poll(tB); // dropouts disabled has no effect on a
                                               // real=false pointer; this is just a
                                               // clean, consistent "nothing here".
        devB.touch(report.fingers === 1, report.x, report.y);
        devB.tick(tB);
        if (devB.drainLog().some((l) => l.includes("stroke end"))) { sawStrokeEnd = true; break; }
    }
    check("a genuine total release still ends the stroke within a bounded time (lift detection still works)",
        sawStrokeEnd, `waited up to ${LIFT_WAIT_MS}ms`);

    console.log(`\n${passCount} passed, ${failCount} failed`);
    if (failCount > 0) process.exit(1);
}

main();
