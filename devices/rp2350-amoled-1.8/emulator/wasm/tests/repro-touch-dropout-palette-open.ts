// repro-touch-dropout-palette-open: headless reproduction of the owner's
// report on real hardware, 2026-08-14, right after the palette shipped
// with its icons - in TWO rounds, both on the same day, both because the
// emulator's simulated controller was kinder than the real one.
//
// ROUND ONE: "j'ai bien les icones mais la palette marche pas" - the
// palette did not open at all, even though the same gesture opened it
// reliably in the emulator (feature-sketch-palette.ts's own clean-input
// checks, all passing). Reproduced headless by driving TouchSim's dropout
// profile (34 episodes/sec, back-calculated from a hardware session) at a
// STATIONARY real pointer - a long press held still, the palette's entire
// gesture. Opened roughly 1 trial in 4 instead of reliably.
//
// CAUSE ONE: sketch.c's stroke state machine used ONE clock
// (st->lastSampleMs) for two different questions - "how long since the
// finger last MOVED" (the speed/jump-allowance calc, which genuinely needs
// this) and "how long since the finger was last known to be DOWN AT ALL"
// (the genuine-lift check). lastSampleMs only advances on an accepted
// move, so a finger held perfectly still froze it at the last real
// movement, and any dropout landing more than LIFT_DEBOUNCE_MS after that
// - trivial at 34 episodes/sec - was believed a genuine lift despite
// contact having been reported continuously. FIXED by splitting the clock:
// lastContactMs (see its own struct comment) refreshes on every haveTouch
// sample, moved or not, and the genuine-lift check reads that instead.
//
// ROUND TWO, same day: the fix above shipped, and the owner still could
// not open the palette on the real board. This time measured on hardware
// rather than reasoned about (TOUCH_POLL_SELFTEST, finger held
// deliberately still, ~40 seconds): `splits=10`. A split is sketch.c's
// "confirmed jump, close this stroke and open a new one" counter - the
// REPORTED POSITION was jumping hundreds of pixels while the finger sat
// still, and a second reading was landing near the wrong spot often enough
// to get confirmed. touchsim.ts had no way to produce that at all (see its
// own header comment): dropouts stop reports arriving, strays fabricate a
// contact where there is none, but neither ever moves the reported
// position of a contact that IS genuinely down. Added positionJitter* to
// touchsim.ts and calibrated its rate against this exact number - see
// TOUCHSIM_JITTER_PROFILE below.
//
// CAUSE TWO, found by reproducing it here first rather than guessing: the
// first attempted fix compared the raw per-sample position to the hold's
// anchor, filtered through the drawing pipeline's own jump/glitch/split
// classification (trust only a sample already judged "smoothly
// accepted"). That classification's own allowance GROWS with time since
// the last accepted sample, which is backwards for a finger that has been
// sitting still: a held finger produces almost no accepted samples at all
// (the true position never changes), so by the time a jitter sample
// finally arrives, the allowance has often grown large enough to accept it
// outright as "plausible travel" - measured here landing 130px from the
// anchor, accepted, no split even logged. FIXED by measuring the hold
// discriminator sketch.c's own way now: not any single sample, confirmed
// or not, but whether the RAW position has been outside HOLD_STILL_RADIUS_
// PX continuously, with no return, for HOLD_MOVE_GRACE_MS - the same
// "brief blip vs. sustained departure" reasoning LIFT_DEBOUNCE_MS already
// applies to contact loss, applied here to distance. See sketch.c's
// holdOutsideSinceMs struct comment for the full account, including why
// this also makes a hold survive a split for free (a split's own
// stroke_end/stroke_begin never touch it).
//
// A THIRD BUG, found IN THIS FILE while measuring cause two's own fix:
// scenario C originally measured a real, slow, deliberate drag falsely
// opening the palette 10-20% of the time under the noisy profile, and
// this file reported that as a genuine cost of the discriminator. It was
// this file's own mistake, not sketch.c's: the settle tick before each
// trial stamped the touch queue's clock at nowMs=10 while the rest of the
// scenario counted time from nowMs=1000 (and, in scenario C specifically,
// a shared time counter was never reset between freshly-loaded device
// instances, compounding the same gap further on every later trial) - a
// stale holdStartMs on the very first sample that read as "550ms already
// elapsed" regardless of the drag, nothing to do with the fix under test.
// See each scenario's own settle-tick comment below for the one-line fix;
// once corrected, the same measurement came back 0/80 across four
// independent runs. Left in scenario C's own comment as a reminder that a
// discriminator and the harness measuring it both have to be trusted
// before a number gets reported as a property of the code, not the test.
//
// Run with (after `bun run emulator/wasm/build.ts`):
//
//   bun run emulator/wasm/tests/repro-touch-dropout-palette-open.ts
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { TouchSim, type TouchReport } from "../../src/touchsim";
import { TOUCHSIM_DEFAULTS, type TouchSimConfig } from "../../src/constants";

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

// The calibrated profile: 34 dropout episodes/sec (unchanged from round
// one, same hardware session) PLUS position jitter tuned so that a
// STATIONARY hold produces splits at close to the hardware's own measured
// rate - 10 in 40 seconds, ~0.25/sec. positionJitterPerSec=1.5 was found by
// actually running the calibration (not guessed): swept 0.3 through 2.0
// against a stationary pointer, 3 repeats each, and 1.5 landed closest
// (measured avg 9.7 splits/40s across repeats) - see this file's own git
// history for the sweep if the panel's own behaviour is ever re-measured
// and this needs re-tuning.
const TOUCHSIM_JITTER_PROFILE: TouchSimConfig = {
    ...TOUCHSIM_DEFAULTS,
    dropoutsEnabled: true, dropoutsPerSec: 34,
    straysEnabled: false,
    positionJitterEnabled: true, positionJitterPerSec: 1.5,
    positionJitterMinPx: 80, positionJitterMaxPx: 250, positionJitterMaxHoldReports: 3,
};

async function main() {
    console.log("=== reproduction + regression: palette long-press under a dropout+jitter touch stream ===\n");

    const dev = await loadDevice();
    dev.tick(0);
    dev.appSwitch(APP_DRAW);
    // 1000, not 10: this settle tick's own nowMs is the touch queue's "last
    // known time" baseline for any touch() issued before the next tick(),
    // and it has to match the time base the trials below actually use (t
    // starts at 1000) or the very first confirmed hold's own holdStartMs
    // comes out nearly a second stale - which reads as "550ms already
    // elapsed" on the very next sample and opens the palette immediately,
    // for the wrong reason. Found via repro-touch-dropout-stroke-start.ts's
    // own scenario D breaking the same way once holdStartMs existed to
    // notice it - see that file's own settle-tick comment for the fuller
    // account; this file inherited the same pre-existing 10-vs-1000
    // mismatch from the same test-writing convention.
    dev.tick(1000);
    check("switched into sketch", dev.appCurrent() === APP_DRAW, `app_current()=${dev.appCurrent()}`);
    dev.drainLog();

    const STEP_MS = 1000 / TOUCHSIM_JITTER_PROFILE.reportRateHz;

    // ---- scenario A: a stationary long press, under the calibrated
    // dropout+jitter profile, must open the palette - and the profile
    // itself must actually be producing splits at a rate comparable to
    // the hardware measurement, not just dropouts the earlier fix already
    // handled. A generous per-trial window (40s, matching the hardware
    // session's own length) gives a broken candidacy mechanism many
    // chances to get lucky, which is exactly why the threshold below is
    // set from what was actually measured broken vs. fixed, not guessed. -
    const TRIALS = 30;
    const HOLD_WINDOW_MS = 40000; // matches the hardware session's own length
    let opened = 0;
    let totalSplits = 0;
    let t = 1000;
    for (let i = 0; i < TRIALS; i++) {
        const x0 = 60 + (i * 17) % 250;
        const y0 = 60 + (i * 31) % 330;
        const sim = new TouchSim(TOUCHSIM_JITTER_PROFILE, PANEL_W, PANEL_H);
        sim.setPointer(true, x0, y0); // held perfectly still: the palette's own gesture

        let didOpen = false;
        for (let held = 0; held < HOLD_WINDOW_MS; held += STEP_MS) {
            t += STEP_MS;
            const report: TouchReport = sim.poll(t);
            dev.touch(report.fingers === 1, report.x, report.y);
            dev.tick(t);
            for (const l of dev.drainLog()) {
                if (l.includes("stroke split")) totalSplits++;
                if (l.includes("palette: open")) didOpen = true;
            }
            if (didOpen) break;
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

    // The profile check first, per decision 0008's own instruction ("write
    // tests that put realistic input in one end and assert the discard
    // rates that come out"): if this ever stops producing splits at
    // roughly the hardware's own rate, the scenario below is not testing
    // what it claims to, regardless of whether it happens to pass.
    // Trials that opened quickly break out of their own loop early (see
    // above), so this total is not "splits per 30*40s" - it is whatever
    // the profile produced before each trial succeeded or timed out,
    // which is why this is a sanity floor (did the profile fire AT ALL,
    // repeatedly) rather than a tight rate match.
    console.log(`    ${totalSplits} total "stroke split" events observed across ${TRIALS} trials (profile: ` +
        `dropouts=${TOUCHSIM_JITTER_PROFILE.dropoutsPerSec}/s, jitter episodes=${TOUCHSIM_JITTER_PROFILE.positionJitterPerSec}/s, ` +
        `magnitude ${TOUCHSIM_JITTER_PROFILE.positionJitterMinPx}-${TOUCHSIM_JITTER_PROFILE.positionJitterMaxPx}px)`);
    check("the jitter profile is actually producing splits, not sitting dormant (decision 0008)",
        totalSplits > 0, `${totalSplits} splits observed`);

    const rate = (opened / TRIALS) * 100;
    console.log(`    ${opened}/${TRIALS} stationary long-presses opened the palette (${rate.toFixed(0)}%)`);
    // Measured by actually running this scenario repeatedly (not guessed):
    // pre-round-two-fix (raw position filtered through the drawing
    // pipeline's own accept test), this profile opened the palette in
    // roughly 1 trial in 4 within a 2.5s window and got measurably worse
    // once the window grew to 40s and jitter joined dropouts (25/30, then
    // as low as 28/30 in some 30-trial runs once the accept-test bug was
    // found and fixed at the wrong layer). Post-fix (holdOutsideSinceMs,
    // the sustained-excursion debounce), repeated 50-trial runs scored
    // 50/50 four times running; smaller 30-trial runs scored 28-30/30,
    // reflecting genuine statistical variance from TouchSim's unseeded RNG
    // (occasionally a real, legitimately-classified sustained excursion or
    // a real extended dropout lands inside one trial's 40s window - not a
    // misclassification, just unlucky timing) rather than a systematic
    // failure. 80% sits with real margin below every floor this fix has
    // shown across many repeated runs, while still being far above what
    // either pre-fix version scored.
    check("a stationary long press reliably opens the palette under a dropout+jitter stream",
        rate >= 80, `${rate.toFixed(0)}% over ${TRIALS} trials`);

    // ---- scenario B: the fix must not break genuine lift detection. A
    // touch that is held only briefly (under LONG_PRESS_MS, so the palette
    // never opens) and then genuinely, totally released must still end the
    // stroke within a bounded time - lastContactMs has to keep tracking a
    // REAL departure just as faithfully as it now tracks a stationary
    // hold's continued presence, or this fix would have traded one bug for
    // its opposite (a stroke that never believes it was ever lifted). -----
    //
    // THIS SCENARIO USED TO FLAKE, roughly 1 run in 20-25 by the owner's own
    // count, and was living in the tree mislabelled a "flake" rather than
    // investigated. Reproduced deterministically with a seeded RNG (Mulberry32)
    // driving the exact same profile: across 3000 seeded trials, every single
    // failure - 42 of them, a 1.4% rate at the original single 300ms hold
    // below - showed NO "stroke start" line at all during the hold, and NONE
    // (0/3000) showed a stroke that started and then failed to end. So this
    // was never a lift-detection bug: st->lastContactMs/LIFT_DEBOUNCE_MS
    // ended every stroke that actually started, every time. The bug was in
    // this scenario's own premise, waiting to see "stroke end" without first
    // confirming a stroke had begun. At this file's own 34-dropout/sec
    // profile (roughly 57% of individual reports lost, back-calculated from
    // the hardware session TOUCHSIM_JITTER_PROFILE was itself calibrated
    // against), a single fixed-length 300ms hold occasionally never
    // accumulates CONFIRM_MS=40ms of persistence before PENDING_GRACE_MS's
    // own 80ms dropout-run give-up fires and resets the candidate - not a
    // production defect, just bad luck landing inside one fixed window.
    //
    // THE FIX: hold for real, the way a finger actually would, until the
    // firmware itself reports "stroke start" - not for a fixed guess at how
    // long that takes. Measured (same 3000-trial run): confirm time is
    // p50=83ms, p95=217ms, p99=317ms, max=383ms. CONFIRM_WAIT_CAP_MS=400 sits
    // just past that measured max and comfortably below LONG_PRESS_MS=550
    // (150ms of margin - opening the palette by accident here would change
    // what this scenario is even testing). A single 400ms attempt alone still
    // failed 15/6000 seeded trials (0.25%, the tail this scenario's own
    // fixed-window predecessor was actually seeing); MAX_CONFIRM_ATTEMPTS=3
    // retries (release, let PENDING_GRACE_MS/LIFT_DEBOUNCE_MS both settle,
    // touch down again) compounds that tail to 0/6000 across the same seeded
    // sweep - independent per-attempt draws, so three failed attempts in a
    // row is the 0.25% rate cubed, not added.
    console.log("");
    const devB = await loadDevice();
    devB.tick(0);
    devB.appSwitch(APP_DRAW);
    devB.tick(1000); // 1000, matching tB's own start below - see dev's own settle-tick comment above
    devB.drainLog();

    const briefHoldSim = new TouchSim(TOUCHSIM_JITTER_PROFILE, PANEL_W, PANEL_H);
    let tB = 1000;
    const CONFIRM_WAIT_CAP_MS = 400; // measured max confirm time was 383ms - see this scenario's own header comment
    const MAX_CONFIRM_ATTEMPTS = 3;
    const SETTLE_MS = 300; // > PENDING_GRACE_MS(80) and > LIFT_DEBOUNCE_MS_DEFAULT(220): guarantees
                            // a clean idle state before retrying, whichever of the two was still armed
    let openedDuringBriefHold = false;
    let confirmedStroke = false;
    for (let attempt = 0; attempt < MAX_CONFIRM_ATTEMPTS && !confirmedStroke; attempt++) {
        briefHoldSim.setPointer(true, 150, 150);
        for (let held = 0; held < CONFIRM_WAIT_CAP_MS; held += STEP_MS) {
            tB += STEP_MS;
            const report = briefHoldSim.poll(tB);
            devB.touch(report.fingers === 1, report.x, report.y);
            devB.tick(tB);
            for (const l of devB.drainLog()) {
                if (l.includes("palette: open")) openedDuringBriefHold = true;
                if (l.includes("stroke start")) confirmedStroke = true;
            }
            if (confirmedStroke) break;
        }
        if (!confirmedStroke) {
            // Never confirmed inside this attempt's cap: release and let the
            // candidate/dropout-grace state settle cleanly before trying
            // again, rather than touching straight back down on top of
            // whatever state this attempt left behind.
            briefHoldSim.setPointer(false, 0, 0);
            for (let s = 0; s < SETTLE_MS; s += STEP_MS) {
                tB += STEP_MS;
                const report = briefHoldSim.poll(tB);
                devB.touch(report.fingers === 1, report.x, report.y);
                devB.tick(tB);
                devB.drainLog();
            }
        }
    }
    check("a brief hold (under LONG_PRESS_MS) does not open the palette", !openedDuringBriefHold);
    check("the brief hold confirms into a real stroke within a bounded time (separate from lift detection)",
        confirmedStroke, `${MAX_CONFIRM_ATTEMPTS} attempts of ${CONFIRM_WAIT_CAP_MS}ms each`);

    // Genuine, total release - no more simulated dropouts/strays/jitter,
    // just real contact gone - held for LIFT_DEBOUNCE_MS_DEFAULT plus a
    // healthy margin, pushed every step (see idleSettle's own reasoning in
    // repro-touch-dropout-stroke-start.ts: the drain loop only re-checks
    // the debounce when a fresh sample says haveTouch==false).
    briefHoldSim.setPointer(false, 0, 0);
    let sawStrokeEnd = false;
    const LIFT_WAIT_MS = LIFT_DEBOUNCE_MS_DEFAULT + 400;
    for (let waited = 0; waited < LIFT_WAIT_MS; waited += STEP_MS) {
        tB += STEP_MS;
        const report = briefHoldSim.poll(tB); // dropouts/jitter disabled has no effect on a
                                               // real=false pointer; this is just a clean,
                                               // consistent "nothing here".
        devB.touch(report.fingers === 1, report.x, report.y);
        devB.tick(tB);
        if (devB.drainLog().some((l) => l.includes("stroke end"))) { sawStrokeEnd = true; break; }
    }
    check("a genuine total release still ends the stroke within a bounded time (lift detection still works)",
        sawStrokeEnd, `waited up to ${LIFT_WAIT_MS}ms`);

    // ---- scenario C: a moving finger, under the same dropout+jitter
    // profile, must NOT open the palette - the discriminator has to still
    // reject a real stroke, not just tolerate noise. This is the OTHER half
    // of the trade stated in sketch.c's HOLD_MOVE_GRACE_MS comment.
    //
    // AN EARLIER VERSION OF THIS SCENARIO measured this false-opening
    // 10-20% of the time and this file asserted that as a genuine,
    // statistical cost of the fix. It was wrong, and the mistake is worth
    // recording rather than quietly fixing: each trial's own settle tick
    // (`devC.tick(10)`) was stamping the touch queue's clock at nowMs=10
    // while the trial's own drag logic counted time from nowMs=1000 (tC
    // was also, separately, never reset between trials, compounding the
    // same gap further on every iteration after the first) - a stale
    // pendStartMs/holdStartMs on the very first sample, unrelated to
    // anything sketch.c does, that read as "550ms already elapsed" and
    // opened the palette immediately regardless of the drag. Once the
    // settle tick and the per-trial reset were both fixed (see below), the
    // SAME scenario, same drag speed, same full dropout+jitter severity,
    // measured 0/20 across four independent 20-trial runs (80 trials, 0
    // false opens) - the discriminator was never actually fooled by real
    // movement; the test harness was fooling itself. Gated at <=15% here
    // rather than requiring literal zero, so this stays a meaningful check
    // rather than a brittle exact-match one, while still failing loudly if
    // this regresses anywhere near the old, wrong number. -----------------
    console.log("");
    const DRAG_TRIALS = 20;
    let draggedFalseOpens = 0;
    for (let i = 0; i < DRAG_TRIALS; i++) {
        const devC = await loadDevice();
        devC.tick(0);
        devC.appSwitch(APP_DRAW);
        devC.tick(1000); // 1000, matching tC's own reset below - see dev's own settle-tick comment above.
                          // Reset per trial, not accumulated across a fresh devC each iteration, for the
                          // same reason: a growing tC against each trial's own freshly-1000-based devC
                          // would reintroduce the identical stale-holdStartMs gap this comment is about.
        devC.drainLog();
        let tC = 1000;

        const dragSim = new TouchSim(TOUCHSIM_JITTER_PROFILE, PANEL_W, PANEL_H);
        let openedDuringDrag = false;
        const DRAG_MS = LONG_PRESS_MS + 300; // well past LONG_PRESS_MS: a broken
                                              // discriminator would have opened by now
        const dragStartX = 80 + (i * 23) % 200, dragStartY = 150 + (i * 19) % 200;
        dragSim.setPointer(true, dragStartX, dragStartY);
        for (let held = 0; held < DRAG_MS; held += STEP_MS) {
            tC += STEP_MS;
            // A slow, steady drag - about 0.1px/ms, comfortably past
            // HOLD_STILL_RADIUS_PX (12px) within a couple hundred ms, the
            // same order of magnitude as an unhurried child's line.
            dragSim.setPointer(true, dragStartX + held * 0.1, dragStartY);
            const report = dragSim.poll(tC);
            devC.touch(report.fingers === 1, report.x, report.y);
            devC.tick(tC);
            if (devC.drainLog().some((l) => l.includes("palette: open"))) openedDuringDrag = true;
        }
        if (openedDuringDrag) draggedFalseOpens++;
    }
    const dragFalseRate = (draggedFalseOpens / DRAG_TRIALS) * 100;
    console.log(`    ${draggedFalseOpens}/${DRAG_TRIALS} slow real drags falsely opened the palette (${dragFalseRate.toFixed(0)}%)`);
    check("a slow, real, continuous drag does not open the palette, even under the noisy profile",
        dragFalseRate <= 15, `${dragFalseRate.toFixed(0)}% false-opened over ${DRAG_TRIALS} trials (measured: 0/80 across four repeated runs)`);

    console.log(`\n${passCount} passed, ${failCount} failed`);
    if (failCount > 0) process.exit(1);
}

main();
