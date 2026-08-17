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
// SECOND ROUND, 2026-08-14, same day, next hardware session: stroke START
// was fixed (12/12 confirmed, strays=0) but strokes were still fragmenting
// mid-draw ("traits qui s'arrêtent subitement" - lines stopping suddenly,
// dropouts=354 against strokeStarted=strokeEnded=12). Scenarios C and D
// below are the standing check for THAT fix (LIFT_DEBOUNCE_MS raised
// 80ms -> 220ms): C proves a long stroke survives the dropout rate that
// used to fragment it, D proves the other half of the trade - a real lift
// followed by a new touch elsewhere still ends the first stroke and starts
// a clean second one, with no line joining them - was not given away to get
// C.
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
// SEEDED, 2026-08-17, closing the gap this file's own header used to admit
// ("not bit-for-bit deterministic"). Every TouchSim below now takes
// seededRng(seedFromName(...)) (tools/gate/touch.ts, the same mulberry32
// used by the gate's own gesture driver and by repro-touch-dropout-tables.ts)
// as its fourth argument, so this file plays the exact same weather every
// run: a failure replays exactly, by re-running the file, with no seed to
// copy out of a log by hand. This was the missing half of the gate's own
// `test-determinism` rule (tools/gate/contracts.ts's UNSEEDED_BASELINE
// named this file "the documented flake... owed a seeded rng most of all");
// this file's own baseline entry is removed in the same change.
//
// This did NOT remove the statistical-threshold shape of scenarios A/C/E -
// each trial still draws its own weather (a different seed per trial index,
// `stroke-a-${i}` etc.), so the threshold still has to hold against many
// independent draws. What seeding buys is that it is now the SAME many
// draws every run, not a fresh unseeded sample each time.
//
// LIVE-TUNED CONSTANTS. This build is compiled with SKETCH_LIVE_TUNE=1 (see
// emulator/wasm/build.ts), so sketch.c's dropout-tolerance constants are
// runtime variables, read here through emu_tune_get() rather than
// hardcoded in this file - a value changed in sketch.c's own _DEFAULT
// #define (or dialed in via the emulator's tuning panel before a run) is
// picked up automatically, instead of this file silently testing against a
// stale assumption about what LIFT_DEBOUNCE_MS currently is. Scenario E, at
// the end, is the standing check that the live-tune wiring itself
// (devlink's TUNE command's emulator-side twin) actually reaches sketch.c's
// state and not just a decoration.
//
// Run with (after `bun run emulator/wasm/build.ts`):
//
//   bun run emulator/wasm/tests/repro-touch-dropout-stroke-start.ts
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { TouchSim, type TouchReport } from "../../src/touchsim";
import { TOUCHSIM_DEFAULTS } from "../../src/constants";
import { seededRng, seedFromName } from "../../../tools/gate/touch";

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

interface Tunable { id: string; min: number; max: number; default: number }

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

    const readCString = (ptr: number): string => {
        const bytes = new Uint8Array(memory.buffer);
        let end = ptr;
        while (end < bytes.length && bytes[end] !== 0) end++;
        return decoder.decode(bytes.subarray(ptr, end));
    };

    const device = JSON.parse(readCString(exp.emu_device()));
    const tunables: Tunable[] = device.tunables || [];
    const tuneIndex = (id: string): number => {
        const i = tunables.findIndex((t) => t.id === id);
        if (i < 0) throw new Error(`no tunable declared with id "${id}" - device.tunables=${JSON.stringify(tunables)}`);
        return i;
    };

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
        // Raw framebuffer pixel, byte-swap-invariant for the one value this
        // file checks against (0xFFFF, blank paper - see gfx.h: swapping the
        // two bytes of 0xFFFF is still 0xFFFF, so this needs no unswap).
        pixel16(x: number, y: number): number {
            const ptr = exp.emu_fb();
            const view = new DataView(memory.buffer, ptr, PANEL_W * PANEL_H * 2);
            return view.getUint16((y * PANEL_W + x) * 2, true);
        },
        // Drains and clears the captured firmware log lines since the last call.
        drainLog(): string[] {
            const out = fwLog.slice();
            fwLog.length = 0;
            return out;
        },
        tunables,
        tuneGet(id: string): number { return exp.emu_tune_get(tuneIndex(id)); },
        tuneSet(id: string, value: number): void { exp.emu_tune_set(tuneIndex(id), value); },
        // Owner ask 2026-08-14: a per-control way back to default, no
        // reflash. emu_tune_reset is the emulator's twin of the device's
        // devlink TUNE RESET; not yet wired to a page control
        // (emulator/src/ is owned elsewhere right now), so this is exercised
        // directly against the wasm export rather than through the UI.
        tuneReset(id: string): void { exp.emu_tune_reset(tuneIndex(id)); },
    };
}

type Device = Awaited<ReturnType<typeof loadDevice>>;

async function main() {
    console.log("=== reproduction + regression: stroke survival under a dropout-heavy touch stream ===\n");

    const dev = await loadDevice();
    dev.tick(0);
    dev.appSwitch(APP_DRAW);
    // 1000, not 10: this settle tick's own nowMs becomes the touch queue's
    // "last known time" baseline, and every touch() call issued before the
    // NEXT tick() is stamped against it (see emu_touch/emu_tick's own
    // pairing) - so it has to be consistent with the time base the rest of
    // this scenario actually uses (t/tD/tE below all start at 1000), or
    // the very first confirmed stroke's own pendStartMs comes out nearly a
    // second stale. Harmless for everything this file originally checked
    // (stroke-start rate, split/glitch counts, LIFT_DEBOUNCE_MS - all
    // re-anchor themselves from later, correctly-timed samples), but found
    // 2026-08-14 once sketch.c's palette feature added holdStartMs, which
    // is deliberately set ONCE at confirmation and never refreshed: a
    // stale pendStartMs there reads as "550ms have already passed" on the
    // very next sample, and this file's scenario D confirms a real stroke
    // moving nowhere near the palette's own hold gesture, catching it
    // immediately as a spurious "long press" that rolled the stroke back
    // before it could ever end normally.
    dev.tick(1000);
    check("switched into sketch", dev.appCurrent() === APP_DRAW, `app_current()=${dev.appCurrent()}`);
    dev.drainLog();

    check("device declares the live tunables this file depends on",
        ["lift", "confirm", "pendgrace", "minjump", "maxjump", "maxspeed"].every((id) => dev.tunables.some((t) => t.id === id)),
        `tunables=${JSON.stringify(dev.tunables)}`);

    const liftMs = dev.tuneGet("lift");
    console.log(`    live LIFT_DEBOUNCE_MS = ${liftMs}ms (read from the running module, not hardcoded here)\n`);

    // A settle wait longer than the current lift debounce, used between
    // scenarios (and between trials within a scenario) so one phase's
    // in-flight stroke state cannot bleed into the next. Padded well past
    // the debounce itself, not just past it, so this stays correct even if
    // the constant is later retuned smaller or larger.
    const SETTLE_MARGIN_MS = 150;
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
    const STEP_MS = 1000 / dropoutHeavy.reportRateHz;
    // Enough steps to clear liftMs + SETTLE_MARGIN_MS of simulated "no
    // contact" time, rounded up. Computed from the LIVE value (not a
    // hardcoded step count) specifically because scenario A's own inter-
    // trial gap used to be a fixed 8 steps (~133ms) - correct against the
    // old 80ms debounce, silently too short once LIFT_DEBOUNCE_MS was
    // raised to 220ms, which bled trials into each other and read as a
    // regression in the fix when it was actually a stale assumption in
    // this file. Not making that mistake twice.
    const settleSteps = Math.ceil((liftMs + SETTLE_MARGIN_MS) / STEP_MS);

    // Advances the sim/device by `steps` reports with no real contact, so
    // wall-clock-based logic (lift debounce, stray decay) gets fresh
    // "nothing here" samples the same way core1's idle heartbeat does on
    // real hardware (sensors.c's TOUCH_IDLE_HEARTBEAT_MS) - see that file's
    // comment for why a real device never has this problem and an emulator
    // fed by explicit emu_touch() calls does.
    function idleSettle(sim: TouchSim, t: number, steps: number): number {
        sim.setPointer(false, 0, 0);
        for (let s = 0; s < steps; s++) {
            t += STEP_MS;
            const report = sim.poll(t);
            dev.touch(report.fingers === 1, report.x, report.y);
            dev.tick(t);
        }
        return t;
    }

    // ---- scenario A: draw N short strokes through a controller far worse
    // than TOUCHSIM_DEFAULTS, and count how many actually start. ----------
    const TRIALS = 40; // large enough that the threshold below has real margin against
                        // trial-to-trial variance (measured floor over 3000 seeded trials
                        // via the same rng below: 92.4%) without making the test slow.
    let started = 0;
    let t = 1000;

    for (let i = 0; i < TRIALS; i++) {
        // A fresh, seeded TouchSim per trial (name includes the trial index,
        // matching repro-touch-dropout-tables.ts's own convention), not one
        // instance reused across all 40: that is what makes each trial's own
        // weather independently reproducible by name.
        const sim = new TouchSim(dropoutHeavy, PANEL_W, PANEL_H, seededRng(seedFromName(`stroke-a-${i}`)));
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

        // Lift, and let the live lift-debounce plus margin pass before the
        // next trial, so trials do not bleed into each other as one long
        // bridged stroke (see settleSteps' own comment above).
        t = idleSettle(sim, t, settleSteps);

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
    // A clean settle first: scenario A's last trial already waited past the
    // debounce (idleSettle above), but do it again explicitly here so
    // scenario B never depends on scenario A's internal pacing to already
    // have left the stroke machine idle - a fresh TouchSim instance below
    // does not know or care about the old one's state, so this file should
    // not either. Seeded like every settle gap in this file, though the
    // weather does not matter here (no pointer down, nothing to drop).
    t = idleSettle(new TouchSim(dropoutHeavy, PANEL_W, PANEL_H, seededRng(seedFromName("stroke-settle-after-a"))), t, settleSteps);
    dev.drainLog();

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
    // FOUND 2026-08-17, while chasing the flake this whole file's seeding
    // was meant to fix: this scenario is not actually noise-limited, it has
    // a REAL failure mode. Two strays landing on ADJACENT report ticks (both
    // pass the same per-report Bernoulli draw back to back - rare, but the
    // 20-second window here is ~1200 ticks, so it happens) are, to
    // sketch.c, indistinguishable from a finger that touched down and
    // immediately moved: the stroke-start rule's "moved" path (see
    // sketch.c's CONFIRM_MS comment, top of file) confirms on the SECOND
    // report with no minimum elapsed time at all - that check exists only
    // on the "persisted" path (CONFIRM_MS=40ms). A brute-force sweep of 60
    // independently seeded 20-second windows at this exact rate
    // (straysPerSec=0.2, realistic, not stress) found 3/60 (5%) producing a
    // confirmed "stroke start" from strays alone, and the two REAL
    // unseeded-run failures this investigation started from (2/40 runs of
    // the full file, both this same check) both showed exactly this: one
    // stray-only "stroke start" line and a changed framebuffer hash, zero
    // real touch the whole scenario. Seeds 12289, 12309 and 12344 (this
    // scenario's own 20s window, TOUCHSIM_DEFAULTS.straysPerSec=0.2,
    // dropoutsEnabled=false) reproduce it on demand.
    //
    // THIS IS A SKETCH.C FINDING, NOT A TEST BUG - the assertion below is
    // exactly what this scenario has always claimed ("a stray alone must
    // not leave a mark") and sketch.c genuinely breaks it a small fraction
    // of the time. sketch.c is another agent's file in this worktree as of
    // 2026-08-17 (see this repo's AGENTS.md / the shared-worktree note), so
    // it is not touched here - reported to the owner instead. The seed
    // below is the repo's ordinary seedFromName("stroke-b-strays"), chosen
    // for its name alone before it was run even once, not searched for a
    // pass: it happens to land clean (simStrays fire, none adjacent enough
    // to confirm), so this scenario's own assertion stays exactly as strict
    // as it always was and will catch a regression that makes the real rate
    // worse. It will NOT go red the day someone fixes the underlying bug
    // either, which is the trade of picking a seed at all - the seeds two
    // sentences up are what to replay to prove a fix actually closed this.
    const strayGen = new TouchSim(strayOnly, PANEL_W, PANEL_H, seededRng(seedFromName("stroke-b-strays")));
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
        console.log("    (no simulated stray actually fired this run against this seed - so this phase did not " +
            "exercise anything; framebuffer-unchanged still holds trivially)");
    }
    check("a stray alone (no real touch) does not leave a mark on the canvas",
        postStrayHash === preStrayHash,
        `pre=${preStrayHash} post=${postStrayHash}, strokeStarts=${strayStrokeStarts}, simStrays=${strayGen.simStrays}`);

    // ---- scenario C: a long, continuous stroke must survive the dropout
    // rate that used to fragment it into many short ones. -------------------
    // This is the standing check for the SECOND fix (LIFT_DEBOUNCE_MS
    // 80ms -> 220ms): CONFIRM_MS alone (scenario A) fixed getting a stroke
    // STARTED, not keeping it going - see this file's header comment for
    // the hardware numbers (dropouts=354, strokeStarted=strokeEnded=12) that
    // showed strokes were still fragmenting mid-draw even after A's fix.
    t = idleSettle(new TouchSim(dropoutHeavy, PANEL_W, PANEL_H, seededRng(seedFromName("stroke-settle-after-b"))), t, settleSteps);
    dev.drainLog();

    const SURVIVAL_TRIALS = 25;
    const STROKE_MS = 4000; // several seconds of continuous contact - long
                             // enough that fragmentation under the OLD
                             // 80ms debounce would show up almost certainly
                             // (see the LIFT_DEBOUNCE_MS comment in
                             // sketch.c for the per-episode math), short
                             // enough that many trials stay fast.
    const strokeSteps = Math.round(STROKE_MS / STEP_MS);

    let survived = 0;
    let totalStarts = 0;
    for (let i = 0; i < SURVIVAL_TRIALS; i++) {
        const survivalSim = new TouchSim(dropoutHeavy, PANEL_W, PANEL_H, seededRng(seedFromName(`stroke-c-${i}`)));
        const x0 = 30 + (i * 11) % 300;
        const y0 = 30 + (i * 23) % 380;
        survivalSim.setPointer(true, x0, y0);

        let starts = 0;
        for (let s = 0; s < strokeSteps; s++) {
            t += STEP_MS;
            // A slow, continuous drift, not a jump - this is a stroke a
            // real hand could draw, not a test artifact that would trip
            // the glitch/split logic on its own.
            const dx = x0 + Math.sin(s * 0.05) * 60 + s * 0.15;
            const dy = y0 + Math.cos(s * 0.05) * 60;
            survivalSim.setPointer(true, dx, dy);
            const report = survivalSim.poll(t);
            dev.touch(report.fingers === 1, report.x, report.y);
            dev.tick(t);
            const log = dev.drainLog();
            starts += log.filter((l) => l.includes("stroke start")).length;
        }

        totalStarts += starts;
        if (starts === 1) survived++;

        // Deliberate lift between trials, well past the debounce, so the
        // next trial starts from a genuinely idle stroke machine.
        t = idleSettle(survivalSim, t, settleSteps);
        dev.drainLog();
    }

    const survivalRate = (survived / SURVIVAL_TRIALS) * 100;
    console.log(`    ${survived}/${SURVIVAL_TRIALS} continuous ${STROKE_MS}ms strokes stayed ONE stroke ` +
        `(${survivalRate.toFixed(0)}%), ${totalStarts} total "stroke start" events across all trials ` +
        `(${SURVIVAL_TRIALS} expected if none fragmented)`);
    // Threshold set the same way scenario A's was: run, not reasoned about.
    // At LIFT_DEBOUNCE_MS=220ms and this file's dropout rate/duration, this
    // scenario measured a 80-100% survival floor across 10 repeated runs;
    // at the OLD 80ms debounce (checked by hand against `git show
    // HEAD~1:firmware/apps/sketch.c`) it measured 0-8% - every stroke this
    // long fragmented into several. 60% is comfortably below the new
    // floor and comfortably above the old one, so this gate has real margin
    // in both directions and would have caught the regression it exists for.
    check("a long continuous stroke survives a dropout-heavy stream as ONE stroke",
        survivalRate >= 60, `${survivalRate.toFixed(0)}% over ${SURVIVAL_TRIALS} trials`);

    // ---- scenario D: the other half of the trade - a real lift, held well
    // past the debounce, followed by a new touch elsewhere, must still end
    // the first stroke and start a clean second one with NO line joining
    // them. This is the failure mode a too-generous debounce creates, and
    // per the task brief it is more visible and more annoying than a broken
    // line, so it gets its own check rather than being assumed safe from
    // scenario C alone. -------------------------------------------------
    console.log("");
    const devD = await loadDevice(); // fresh instance: a clean white canvas,
                                      // so "no pixel changed between the two
                                      // strokes" cannot be confused with ink
                                      // left over from scenarios A-C.
    devD.tick(0);
    devD.appSwitch(APP_DRAW);
    devD.tick(1000); // 1000, matching tD's own start below - see the main dev's own settle-tick comment above
    devD.drainLog();

    const liftMsD = devD.tuneGet("lift");
    let tD = 1000;
    const STEP_MS_D = 1000 / TOUCHSIM_DEFAULTS.reportRateHz; // clean signal, no TouchSim noise:
                                                              // this checks the debounce/split
                                                              // logic itself, not dropout
                                                              // tolerance (already scenario C's job).

    // Stroke 1: a short drag near the top-left.
    const p1 = [
        { x: 40, y: 60 }, { x: 55, y: 70 }, { x: 70, y: 82 }, { x: 84, y: 95 },
    ];
    devD.touch(true, p1[0].x, p1[0].y);
    tD += STEP_MS_D;
    devD.tick(tD);
    for (let i = 1; i < p1.length; i++) {
        tD += STEP_MS_D;
        devD.touch(true, p1[i].x, p1[i].y);
        devD.tick(tD);
    }
    devD.drainLog();

    // Genuine lift, held for the live debounce plus a healthy margin -
    // longer than scenario C's settle, specifically to make this an
    // unambiguous "the finger left" rather than something that could be
    // argued as still within dropout-bridging territory.
    const LIFT_HOLD_MS = liftMsD + 400;
    let sawStrokeEnd = false;
    for (let held = 0; held < LIFT_HOLD_MS; held += STEP_MS_D) {
        tD += STEP_MS_D;
        // Pushed every iteration, not once before the loop: sketch_tick()
        // only re-evaluates its wall-clock lift check when a DRAINED sample
        // says haveTouch==false (see sketch.c's fingerDown branch). A
        // single zero sample gets drained once and the queue then sits
        // empty, so the debounce would never be re-checked again - the same
        // "idle heartbeat" gap idleSettle() above exists to close, needed
        // here too since this loop does not go through that helper.
        devD.touch(false, 0, 0);
        devD.tick(tD);
        if (devD.drainLog().some((l) => l.includes("stroke end"))) sawStrokeEnd = true;
    }
    check("a lift held well past the debounce is believed and ends the stroke",
        sawStrokeEnd, `waited ${LIFT_HOLD_MS.toFixed(0)}ms against a ${liftMsD}ms debounce`);

    const preSecondStrokeHash = devD.fbHash();

    // Stroke 2: far enough from stroke 1 (well beyond a pen radius, see
    // PEN_SIZE/sketch.c) that a straight connecting line would be
    // unmistakable in the pixels between them.
    const p2 = [
        { x: 300, y: 360 }, { x: 285, y: 350 }, { x: 270, y: 338 }, { x: 256, y: 325 },
    ];
    devD.touch(true, p2[0].x, p2[0].y);
    tD += STEP_MS_D;
    devD.tick(tD);
    for (let i = 1; i < p2.length; i++) {
        tD += STEP_MS_D;
        devD.touch(true, p2[i].x, p2[i].y);
        devD.tick(tD);
    }
    const strokeStartsAfterLift = devD.drainLog().filter((l) => l.includes("stroke start")).length;

    check("the new touch elsewhere starts its own second stroke",
        strokeStartsAfterLift === 1, `${strokeStartsAfterLift} "stroke start" line(s) after the lift`);

    // Sample points along the straight segment between the end of stroke 1
    // and the start of stroke 2: if the two were ever bridged (drawn as one
    // continuous stroke instead of ending and restarting), draw_capsule
    // would have put ink along exactly this line. None of these points is
    // near either actual stroke's own path (both stay within about 30px of
    // their own start), so any ink here can only have come from a bridge.
    const lastP1 = p1[p1.length - 1];
    const firstP2 = p2[0];
    let bridgePixelsFound = 0;
    const SAMPLES = 9;
    for (let i = 1; i < SAMPLES; i++) {
        const t2 = i / SAMPLES;
        const sx = Math.round(lastP1.x + (firstP2.x - lastP1.x) * t2);
        const sy = Math.round(lastP1.y + (firstP2.y - lastP1.y) * t2);
        if (devD.pixel16(sx, sy) !== 0xffff) bridgePixelsFound++;
    }
    check("no connecting line was drawn between the two strokes",
        bridgePixelsFound === 0,
        `${bridgePixelsFound}/${SAMPLES - 1} sampled points along the (${lastP1.x},${lastP1.y})-(${firstP2.x},${firstP2.y}) ` +
        `segment had ink, framebuffer changed=${devD.fbHash() !== preSecondStrokeHash}`);

    // ---- scenario E: the live-tune wiring itself actually reaches
    // sketch.c's state, not just a decorated no-op. Sets "lift" far below
    // its default (so almost no dropout survives the shorter window) and
    // confirms the fragmentation rate gets measurably worse, then restores
    // the default so this file leaves no side effect for anything that
    // might run after it in the same process. -------------------------
    console.log("");
    const devE = await loadDevice();
    devE.tick(0);
    devE.appSwitch(APP_DRAW);
    devE.tick(1000); // 1000, matching tE's own start below - see the main dev's own settle-tick comment above
    devE.drainLog();

    const defaultLift = devE.tuneGet("lift");
    const readBackDefault = devE.tunables.find((t) => t.id === "lift")!.default;
    check("emu_tune_get reads sketch.c's actual current value, not a stale copy",
        Math.abs(defaultLift - readBackDefault) < 0.01,
        `emu_tune_get=${defaultLift} declared default=${readBackDefault}`);

    devE.tuneSet("lift", 20); // the tunable's own declared minimum (see
                              // sketch.c's g_sketchTunables) - short enough
                              // that this controller's own dropout episodes
                              // (a few tens of ms, see this file's header
                              // comment) reliably exceed it.
    const appliedLift = devE.tuneGet("lift");
    check("emu_tune_set actually changes what emu_tune_get reads back",
        appliedLift < defaultLift, `set 20, read back ${appliedLift} (default was ${defaultLift})`);

    let brokenTrials = 0;
    const E_TRIALS = 15;
    let tE = 1000;
    for (let i = 0; i < E_TRIALS; i++) {
        const tinyLiftSim = new TouchSim(dropoutHeavy, PANEL_W, PANEL_H, seededRng(seedFromName(`stroke-e-${i}`)));
        const x0 = 50 + (i * 17) % 260;
        const y0 = 50 + (i * 31) % 340;
        tinyLiftSim.setPointer(true, x0, y0);
        let starts = 0;
        for (let s = 0; s < strokeSteps; s++) {
            tE += STEP_MS;
            const dx = x0 + Math.sin(s * 0.05) * 60 + s * 0.15;
            const dy = y0 + Math.cos(s * 0.05) * 60;
            tinyLiftSim.setPointer(true, dx, dy);
            const report = tinyLiftSim.poll(tE);
            devE.touch(report.fingers === 1, report.x, report.y);
            devE.tick(tE);
            starts += devE.drainLog().filter((l) => l.includes("stroke start")).length;
        }
        if (starts !== 1) brokenTrials++;
        tinyLiftSim.setPointer(false, 0, 0);
        for (let s = 0; s < 5; s++) { // appliedLift itself is tiny; a short settle is enough here
            tE += STEP_MS;
            const report = tinyLiftSim.poll(tE);
            devE.touch(report.fingers === 1, report.x, report.y);
            devE.tick(tE);
        }
        devE.drainLog();
    }
    console.log(`    with lift forced to its minimum (${appliedLift}ms): ${brokenTrials}/${E_TRIALS} ` +
        `strokes fragmented (compare scenario C's ${SURVIVAL_TRIALS - survived}/${SURVIVAL_TRIALS} at the default ${liftMs}ms)`);
    check("forcing the live-tuned lift knob down measurably breaks stroke survival (the knob is wired to real state)",
        brokenTrials >= E_TRIALS * 0.5, `${brokenTrials}/${E_TRIALS} fragmented`);

    // ---- scenario F: emu_tune_reset actually restores the declared
    // default, not just whatever this file happens to pass it. Also
    // doubles as this file's cleanup (leave no side effect on "lift" for
    // anything that might run after it in the same process), same as
    // scenario E's old tuneSet(defaultLift) line, now proven rather than
    // just asserted by construction. -----------------------------------
    devE.tuneReset("lift");
    const afterReset = devE.tuneGet("lift");
    check("emu_tune_reset restores the tunable's declared default",
        Math.abs(afterReset - defaultLift) < 0.01,
        `forced to ${appliedLift}, reset to ${afterReset}, declared default was ${defaultLift}`);

    console.log(`\n${passCount} passed, ${failCount} failed`);
    if (failCount > 0) process.exit(1);
}

main();
