// repro-timer-coil: regression coverage for the coil redesign of the
// timer's dial (firmware/apps/timer.c, "CORRECTED 2026-08-14: THE COIL",
// and its same-day follow-up "30 MINUTES A LAP, TWO LAPS, A WIDER BAND").
// Run with:
//
//   bun run emulator/wasm/tests/repro-timer-coil.ts
//
// The coil replaced the old three-tier ring (5s/30s/1m steps at different
// radii) with a flat 5s step everywhere and concentric bands wound inward,
// then was revised the same day to a 30-minute lap / two-lap ceiling / a
// doubled band width, and gained a shake-to-clear reaction. This file
// exercises exactly the properties that are NEW rather than the ones the
// ring already had (those stay covered by repro-ring-shrink-residue.ts,
// itself updated for the coil, and the other repro-timer-*.ts files, which
// this change did not touch the logic of):
//
//   1. a drag that crosses twelve o'clock, forward then back by the same
//      amount, lands on EXACTLY the value it started from - the branch-cut
//      unwrap this file's header calls out as the mechanism, checked
//      directly rather than trusted by inspection, since that branch cut
//      "bit this project once already" (the ring-shrink-residue bug).
//   2. direct pointing at six o'clock from zero sets exactly 15:00 (half of
//      the current 30-minute lap) - the owner's own worked example was
//      5:00 at the original 10-minute lap and no longer applies; see
//      timer.c's header for why that is accepted, not a regression.
//   3. the 60 minute ceiling: dragging well past it holds at exactly 60:00
//      with no overflow, and immediately resumes decreasing on a reversal
//      (no dead zone).
//   4. pause, then edit: touching the coil while paused edits the paused
//      value directly, without a reset to zero, and the edited value is
//      what a subsequent PWR start actually starts from.
//   5. shake to clear: shaking while SETTING or PAUSED wipes to 00:00 (with
//      BOOT still able to recall the wiped value in one press, same as any
//      other clear-to-zero path); shaking while the alarm rings still
//      dismisses it and does not ALSO clear a second time on the same
//      tick; shaking while RUNNING does nothing (a deliberate judgement
//      call - see timer.c's own shake-clear branch for the reasoning).
//
// PLUS the two invariants firmware fuzzing already proved for the WHOLE
// codebase (emulator/docs/findings-app-fuzzing.md, section 2) - checked
// here again specifically over the coil's own drawing, since that is what
// changed most in this redesign and the general fuzzing pass predates it:
//
//   - every pushed window's row length is a multiple of 8 pixels
//     (docs/decisions/0001-push-min-width.md);
//   - no framebuffer pixel changes outside that tick's own pushed
//     rectangles.
//
// Every scenario below runs through tickChecked()/touchChecked(), a thin
// wrapper around the raw device calls that snapshots the framebuffer before
// and the push list + framebuffer after EVERY tick, so these two invariants
// are asserted continuously across all five scenarios' drags, points,
// pauses, resumes and shakes - not just at a few hand-picked moments.
import { readFileSync } from "node:fs";
import { join } from "node:path";

const WASM_PATH = join(import.meta.dir, "..", "dist", "emu.wasm");

const PANEL_W = 368;
const PANEL_H = 448;
const APP_TIMER = 2; // g_apps[] = { chrono, sketch("draw"), timer }
const BTN_BOOT = 0;
const BTN_PWR = 1;

const RING_CX = 224, RING_CY = 184;
const DEG2RAD = Math.PI / 180;

function panelTouchForAngle(deg: number, R = 150): [number, number] {
    const theta = deg * DEG2RAD;
    const lx = RING_CX + R * Math.sin(theta);
    const ly = RING_CY - R * Math.cos(theta);
    const px = PANEL_W - 1 - ly;
    const py = lx;
    return [Math.round(px), Math.round(py)];
}

let passCount = 0;
let failCount = 0;
function check(label: string, ok: boolean, detail?: string) {
    const status = ok ? "PASS" : "FAIL";
    if (ok) passCount++; else failCount++;
    console.log(`[${status}] ${label}${detail ? " - " + detail : ""}`);
}

// ---------------------------------------------------------------------
// Invariant tracking, shared across every scenario below - see this file's
// header. Accumulates violations rather than failing immediately, so one
// bad tick does not hide the rest of a scenario's behaviour, and the final
// report names exactly which ticks (if any) misbehaved.
// ---------------------------------------------------------------------
type Violation = { kind: string; detail: string };
const violations: Violation[] = [];

async function loadDevice() {
    const bytes = readFileSync(WASM_PATH);
    const mod = await WebAssembly.compile(bytes);

    let memory!: WebAssembly.Memory;
    const decoder = new TextDecoder();
    const fwLogLines: string[] = [];

    const imports = {
        env: {
            js_log(ptr: number, len: number) {
                const b = new Uint8Array(memory.buffer, ptr, len);
                const line = decoder.decode(b);
                fwLogLines.push(line);
                console.log(`    [fw] ${line}`);
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
        throw new Error("emu_init() failed - see [fw] log lines above");
    }

    function fbSnapshot(): Uint8Array {
        const ptr = exp.emu_fb();
        return new Uint8Array(memory.buffer, ptr, PANEL_W * PANEL_H * 2).slice();
    }

    function pushRects(): { x: number; y: number; w: number; h: number }[] {
        const n = exp.emu_push_count();
        const out: { x: number; y: number; w: number; h: number }[] = [];
        for (let i = 0; i < n; i++) {
            out.push({
                x: exp.emu_push_x(i),
                y: exp.emu_push_y(i),
                w: exp.emu_push_w(i),
                h: exp.emu_push_h(i),
            });
        }
        return out;
    }

    return {
        // Raw building blocks - used only by tickChecked()/the setup calls
        // before a scenario's own checked ticks begin (app switch, the
        // very first frame), never mixed with tickChecked() mid-scenario,
        // so the invariant scan always sees a clean before/after pair.
        touchRaw(down: boolean, x: number, y: number) { exp.emu_touch(down ? 1 : 0, x, y); },
        buttonRaw(index: number, down: boolean) { exp.emu_button(index, down ? 1 : 0); },
        buttonVerdictRaw(index: number, isLong: boolean) { exp.emu_button_verdict(index, isLong ? 1 : 0); },
        appSwitch(index: number) { exp.emu_app_switch(index); },
        appCurrent(): number { return exp.emu_app_current(); },

        // Checked tick: snapshots before, ticks, then diffs the framebuffer
        // against the union of this tick's own pushed rectangles and checks
        // every rectangle's row length - see this file's header.
        tickChecked(nowMs: number) {
            const before = fbSnapshot();
            exp.emu_tick(nowMs);
            const after = fbSnapshot();
            const rects = pushRects();

            for (const r of rects) {
                if (r.w % 8 !== 0) {
                    violations.push({
                        kind: "8px-rule",
                        detail: `t=${nowMs} pushed rect (${r.x},${r.y},${r.w},${r.h}) has width ${r.w}, not a multiple of 8`,
                    });
                }
            }

            // Only bother diffing if something actually changed - the
            // common case for most ticks (no-op incremental updates).
            let anyDiff = false;
            for (let i = 0; i < before.length; i++) {
                if (before[i] !== after[i]) { anyDiff = true; break; }
            }
            if (!anyDiff) return;

            for (let py = 0; py < PANEL_H; py++) {
                for (let px = 0; px < PANEL_W; px++) {
                    const idx = (py * PANEL_W + px) * 2;
                    if (before[idx] === after[idx] && before[idx + 1] === after[idx + 1]) continue;
                    const inside = rects.some((r) => px >= r.x && px < r.x + r.w && py >= r.y && py < r.y + r.h);
                    if (!inside) {
                        violations.push({
                            kind: "outside-push",
                            detail: `t=${nowMs} pixel (${px},${py}) changed but is outside every pushed rect (${JSON.stringify(rects)})`,
                        });
                    }
                }
            }
        },

        // A single touch sample, through a checked tick (16ms of "elapsed
        // time" from the caller's own clock, same cadence every other repro
        // test's drag helpers use).
        touchChecked(down: boolean, x: number, y: number, nowMs: number) {
            exp.emu_touch(down ? 1 : 0, x, y);
            this.tickChecked(nowMs);
        },

        // A shake event (emu_abi.h's "sensor event", index is irrelevant -
        // emu_device() declares exactly one sensor, "shake" - see
        // emu_shim.c's emu_sensor_event()), through a checked tick.
        shakeChecked(nowMs: number) {
            exp.emu_sensor_event(0);
            this.tickChecked(nowMs);
        },

        fbHash(): number | bigint {
            const ptr = exp.emu_fb();
            const fb = new Uint8Array(memory.buffer, ptr, PANEL_W * PANEL_H * 2);
            return Bun.hash(fb);
        },
        fwLogLines(): readonly string[] { return fwLogLines; },
    };
}

type Device = Awaited<ReturnType<typeof loadDevice>>;

// Drags smoothly from an UNWRAPPED angle fromDeg to toDeg (may run past 360
// or below 0 - several laps' worth of physical rotation, same convention
// repro-ring-shrink-residue.ts uses) over `steps` samples, through checked
// ticks throughout, then lifts. A single call is one continuous touch
// context (one point_touch() on its first sample, drag_touch() on every
// sample after) - see dragThrough() below for chaining several legs
// WITHOUT lifting in between, which matters whenever the test cares about
// the exact landed tick rather than just "roughly there or clamped".
function dragTo(dev: Device, fromDeg: number, toDeg: number, t0: number, steps: number): number {
    let t = t0;
    for (let i = 1; i <= steps; i++) {
        const deg = fromDeg + (toDeg - fromDeg) * (i / steps);
        const mod = ((deg % 360) + 360) % 360;
        const [px, py] = panelTouchForAngle(mod);
        dev.touchChecked(true, px, py, (t += 16));
    }
    dev.touchChecked(false, 0, 0, (t += 50));
    return t;
}

// Like dragTo(), but chains several UNWRAPPED-angle legs (waypoints[0] ->
// waypoints[1] -> waypoints[2] -> ...), each with its OWN sample count
// (stepsPerLeg[i], matching waypoints.length-1), inside ONE continuous
// touch context: only the very first sample of the very first leg is a
// point_touch() (touchPressed), every sample after - including the first
// sample of every later leg - is a drag_touch() continuation. This is what
// an actual finger tracing several waypoints without lifting does, and it
// matters here specifically: calling dragTo() several times in a row
// instead would lift and re-press between legs, and a fresh point_touch()
// at the start of a new leg SNAPS to that leg's own first sampled angle
// rather than smoothly continuing the drag from wherever the previous leg
// left off - off by up to half a tick per lift, which compounds across
// legs into an exact-value test failing over an irrelevant reason.
//
// Per-leg step counts (not one shared count) matter for a second, subtler
// reason found by this file's own "bigger round trip" test below: a leg
// whose per-step angular delta is SMALLER than drag_touch()'s own
// DRAG_COMMIT_HYSTERESIS_TICKS dead band does not commit every sample, so
// the DISPLAYED tick can lag the true accumulator by close to a full tick
// at the end of that leg even though the accumulator itself is exact -
// hysteresis is deliberately path-dependent (see drag_touch()'s own
// comment), so a leg used only to "arrive at" some starting angle should
// use FEW, LARGE steps (each comfortably past the hysteresis threshold, so
// it commits immediately and exactly) rather than many small ones, which
// is what fine anchor legs below use a one-sample leg for.
function dragThrough(dev: Device, waypoints: number[], stepsPerLeg: number[], t0: number): number {
    let t = t0;
    for (let leg = 0; leg < waypoints.length - 1; leg++) {
        const fromDeg = waypoints[leg];
        const toDeg = waypoints[leg + 1];
        const steps = stepsPerLeg[leg];
        for (let i = 1; i <= steps; i++) {
            const deg = fromDeg + (toDeg - fromDeg) * (i / steps);
            const mod = ((deg % 360) + 360) % 360;
            const [px, py] = panelTouchForAngle(mod);
            dev.touchChecked(true, px, py, (t += 16));
        }
    }
    dev.touchChecked(false, 0, 0, (t += 50));
    return t;
}

// A single tap-and-release at one angle: exactly the "point directly" fast
// gesture, no drag involved.
function tapAt(dev: Device, deg: number, t0: number): number {
    let t = t0;
    const [px, py] = panelTouchForAngle(deg);
    dev.touchChecked(true, px, py, (t += 16));
    dev.touchChecked(false, 0, 0, (t += 50));
    return t;
}

function pwrShortClickChecked(dev: Device, tPressMs: number): number {
    dev.buttonRaw(BTN_PWR, true);
    dev.tickChecked(tPressMs);
    dev.buttonRaw(BTN_PWR, false);
    dev.buttonVerdictRaw(BTN_PWR, false);
    dev.tickChecked(tPressMs + 50);
    return tPressMs + 50;
}

function lastStartLine(dev: Device): string | undefined {
    return [...dev.fwLogLines()].reverse().find((l) => l.startsWith("timer: start, "));
}

// ---------------------------------------------------------------------
// 1. Branch-cut round trip: forward across twelve o'clock, then back by
//    the same amount, must land on exactly the value before the crossing
//    ever happened.
// ---------------------------------------------------------------------
async function testBranchCutRoundTrip() {
    console.log("\n=== 1. drag across twelve o'clock, forward then back, lands on the right total ===\n");

    // Baseline: point once near (but not past) twelve o'clock, no crossing
    // at all, then read back the committed value via a PWR start.
    const baseline = await loadDevice();
    baseline.appSwitch(APP_TIMER);
    baseline.tickChecked(1000);
    let tb = tapAt(baseline, 350, 1000);
    tb = pwrShortClickChecked(baseline, tb + 50);
    const baselineLine = lastStartLine(baseline);
    check("baseline (no crossing) started from a real value", !!baselineLine, baselineLine);

    // Same starting point, but this time drag forward past twelve o'clock
    // and then back past it by the same angular amount, before ever
    // pressing PWR.
    const roundTrip = await loadDevice();
    roundTrip.appSwitch(APP_TIMER);
    roundTrip.tickChecked(1000);
    let t = 1000;
    // Press at 350 (point), then drag forward to 370 (=10, past 12
    // o'clock) - a single small step, well under the 180deg unwrap
    // threshold - then drag back to 350 again.
    {
        const [px0, py0] = panelTouchForAngle(350);
        roundTrip.touchChecked(true, px0, py0, (t += 16));
        const [px1, py1] = panelTouchForAngle(((370 % 360) + 360) % 360);
        roundTrip.touchChecked(true, px1, py1, (t += 16));
        const [px2, py2] = panelTouchForAngle(350);
        roundTrip.touchChecked(true, px2, py2, (t += 16));
        roundTrip.touchChecked(false, 0, 0, (t += 50));
    }
    t = pwrShortClickChecked(roundTrip, t + 50);
    const roundTripLine = lastStartLine(roundTrip);
    check("round-trip (forward past 12, then back) started from a real value", !!roundTripLine, roundTripLine);

    check(
        "forward-then-back by the same amount lands EXACTLY where it started (no lap gained or lost)",
        !!baselineLine && !!roundTripLine && baselineLine === roundTripLine,
        `baseline=${JSON.stringify(baselineLine)} roundTrip=${JSON.stringify(roundTripLine)}`,
    );

    // A second, larger round trip: one and a half laps forward, then the
    // same total angle back, via many small realistic samples (not one big
    // jump) - the shape of drag an actual finger produces. Kept under two
    // laps (LAPS_MAX) so the forward leg never hits the ceiling and clips
    // the comparison - the ceiling itself is tested separately, in
    // testCeiling() below.
    const bigBaseline = await loadDevice();
    bigBaseline.appSwitch(APP_TIMER);
    bigBaseline.tickChecked(1000);
    let tbb = tapAt(bigBaseline, 40, 1000);
    tbb = pwrShortClickChecked(bigBaseline, tbb + 50);
    const bigBaselineLine = lastStartLine(bigBaseline);

    const bigRoundTrip = await loadDevice();
    bigRoundTrip.appSwitch(APP_TIMER);
    bigRoundTrip.tickChecked(1000);
    // ONE continuous touch context (dragThrough(), not three separate
    // dragTo() calls) - see dragThrough()'s own comment for why a lift
    // between legs would introduce spurious point_touch() snaps. The first
    // leg is a single sample landing exactly on 40deg (the point_touch()
    // anchor, matching the baseline's own tapAt(40) exactly), not a fine
    // multi-sample approach to 40 - see dragThrough()'s own comment on why
    // a small-delta leg would leave the hysteresis-gated display lagging.
    let tr = dragThrough(bigRoundTrip, [40, 40, 40 + 540, 40], [1, 120, 120], 1000);
    tr = pwrShortClickChecked(bigRoundTrip, tr + 50);
    const bigRoundTripLine = lastStartLine(bigRoundTrip);

    check(
        "a bigger (1.5-lap) forward-then-back round trip (many small samples) also lands exactly where it started",
        !!bigBaselineLine && !!bigRoundTripLine && bigBaselineLine === bigRoundTripLine,
        `baseline=${JSON.stringify(bigBaselineLine)} roundTrip=${JSON.stringify(bigRoundTripLine)}`,
    );
}

// ---------------------------------------------------------------------
// 2. Direct pointing at six o'clock from zero sets exactly 15:00 - the
//    owner's own worked example was 5:00 at the original 10-minute lap
//    length; at the current 30-minute lap, six o'clock (half a turn) is
//    half of 30:00, i.e. 15:00 - see timer.c's header, "First, six o'clock
//    is now 15:00, not 5:00", for why that changed and is accepted.
// ---------------------------------------------------------------------
async function testPointSixOClock() {
    console.log("\n=== 2. pointing directly at six o'clock from zero sets exactly 15:00 ===\n");

    const dev = await loadDevice();
    dev.appSwitch(APP_TIMER);
    dev.tickChecked(1000);

    let t = tapAt(dev, 180, 1000); // six o'clock: half a turn from 12
    t = pwrShortClickChecked(dev, t + 50);
    const line = lastStartLine(dev);
    check(
        "a single tap at six o'clock from zero starts at exactly 15:00",
        line === "timer: start, 15:00\r\n",
        line,
    );
}

// ---------------------------------------------------------------------
// 3. The 60 minute ceiling: dragging well past it holds at exactly 60:00,
//    and reversing immediately resumes decreasing (no dead zone).
// ---------------------------------------------------------------------
async function testCeiling() {
    console.log("\n=== 3. the 60 minute ceiling ===\n");

    const dev = await loadDevice();
    dev.appSwitch(APP_TIMER);
    dev.tickChecked(1000);

    // Two laps is the max (2 * 30:00 = 60:00); drag a good deal further
    // than that (8 laps' worth) to prove it clamps rather than wrapping or
    // overflowing.
    let t = dragTo(dev, 0, 8 * 360, 1000, 240);
    t = pwrShortClickChecked(dev, t + 50);
    let line = lastStartLine(dev);
    check("dragging well past two laps clamps at exactly 60:00", line === "timer: start, 60:00\r\n", line);

    // Pause it, touch to edit again (still at the ceiling), drag forward
    // EVEN FURTHER, confirm it is still exactly 60:00 - the ceiling has to
    // hold from a paused-edit re-entry too, not just from a fresh SETTING.
    t = pwrShortClickChecked(dev, t + 500); // RUNNING -> PAUSED
    t = dragTo(dev, 8 * 360, 12 * 360, t + 100, 120);
    t = pwrShortClickChecked(dev, t + 50);
    line = lastStartLine(dev);
    check("still exactly 60:00 after more forward dragging from an already-maxed paused edit", line === "timer: start, 60:00\r\n", line);

    // Now reverse a small amount from the ceiling and confirm it actually
    // decreases right away - no dead zone before the drag "catches up".
    const dev2 = await loadDevice();
    dev2.appSwitch(APP_TIMER);
    dev2.tickChecked(1000);
    let t2 = dragTo(dev2, 0, 8 * 360, 1000, 240); // pinned at the ceiling
    // Reverse by 30 degrees only (10 ticks = 50s) - small enough that a
    // dead zone (the old single-ring drag's own risk, guarded against by
    // clamping the ACCUMULATOR itself, not just the displayed value - see
    // drag_touch()'s own comment) would leave this still reading 60:00.
    t2 = dragTo(dev2, 8 * 360, 8 * 360 - 30, t2 + 100, 10);
    t2 = pwrShortClickChecked(dev2, t2 + 50);
    const line2 = lastStartLine(dev2);
    check(
        "reversing 30deg off the ceiling immediately reads under 60:00 (no dead zone)",
        line2 !== undefined && line2 !== "timer: start, 60:00\r\n",
        line2,
    );
}

// ---------------------------------------------------------------------
// 4. Pause, then edit: touching the coil while paused edits the paused
//    value directly, and a subsequent start uses the edited value.
// ---------------------------------------------------------------------
async function testPauseThenEdit() {
    console.log("\n=== 4. pause, then edit (no reset to zero) ===\n");

    const dev = await loadDevice();
    dev.appSwitch(APP_TIMER);
    dev.tickChecked(1000);

    let t = tapAt(dev, 30, 1000); // sub_lap_ticks_for_angle(30) = 30 ticks = 2:30
    t = pwrShortClickChecked(dev, t + 50);
    const startLine = lastStartLine(dev);
    check("started at 2:30 before pausing", startLine === "timer: start, 02:30\r\n", startLine);

    // Let a little real time pass while RUNNING, then pause.
    for (let i = 0; i < 20; i++) { t += 50; dev.tickChecked(t); }
    t = pwrShortClickChecked(dev, t + 16); // RUNNING -> PAUSED

    const pauseLine = [...dev.fwLogLines()].reverse().find((l) => l.startsWith("timer: pause at "));
    check("paused with time still on the clock", !!pauseLine, pauseLine);

    // Touch the coil while paused: must edit directly, not clear to zero.
    const beforeEditLogCount = dev.fwLogLines().length;
    t = tapAt(dev, 60, t + 100);
    const editedLines = dev.fwLogLines().slice(beforeEditLogCount);
    const editLine = editedLines.find((l) => l.startsWith("timer: paused value now editable, "));
    check("touching while paused produced an 'editable' transition, not a reset", !!editLine, JSON.stringify(editedLines));
    check(
        "the edited value is NOT 00:00 - pausing then touching must not clear the dial",
        !!editLine && !editLine.includes("00:00"),
        editLine,
    );

    // Confirm the edit is real by starting from it and reading the value
    // back: sub_lap_ticks_for_angle(60) = 60 ticks = 5:00, and the lap the
    // paused value carried (0, since 2:30 never left lap 0) must be
    // preserved, matching point_touch()'s own contract.
    t = pwrShortClickChecked(dev, t + 50);
    const restartLine = lastStartLine(dev);
    check(
        "starting after the pause-edit runs from the EDITED value (05:00), not the original 02:30",
        restartLine === "timer: start, 05:00\r\n",
        restartLine,
    );

    // Sanity check the other half of the same feature still works: pausing
    // and pressing PWR again (no touch at all) must resume normally,
    // unaffected by the new touch-to-edit reaction.
    const dev2 = await loadDevice();
    dev2.appSwitch(APP_TIMER);
    dev2.tickChecked(1000);
    let t2 = tapAt(dev2, 90, 1000);
    t2 = pwrShortClickChecked(dev2, t2 + 50); // start
    for (let i = 0; i < 10; i++) { t2 += 50; dev2.tickChecked(t2); }
    t2 = pwrShortClickChecked(dev2, t2 + 16); // pause
    const hashPaused = dev2.fbHash();
    t2 = pwrShortClickChecked(dev2, t2 + 50); // resume, no touch involved
    const resumeLine = [...dev2.fwLogLines()].reverse().find((l) => l.startsWith("timer: resume at "));
    check("PWR alone (no touch) still resumes normally after a pause", !!resumeLine, resumeLine);
    t2 += 2000;
    dev2.tickChecked(t2);
    const hashRunningAgain = dev2.fbHash();
    check("and it is genuinely counting down again (framebuffer moved)", hashRunningAgain !== hashPaused);
}

// ---------------------------------------------------------------------
// 5. Shake to clear. Owner, direct quote: "also i should be able to shake
//    to clear while i'm still in edit mode there." Four sub-scenarios:
//    SETTING clears, PAUSED clears, RUNNING is unaffected (this file's own
//    judgement call, not a spec'd rule), and the alarm's own pre-existing
//    shake dismissal still works and does not double-fire into a second
//    clear on the same tick.
// ---------------------------------------------------------------------
async function testShakeToClear() {
    console.log("\n=== 5. shake to clear ===\n");

    // ---- SETTING: shake wipes to 00:00, BOOT still recalls in one press. ---
    {
        const dev = await loadDevice();
        dev.appSwitch(APP_TIMER);
        dev.tickChecked(1000);

        let t = tapAt(dev, 30, 1000); // 30 ticks = 2:30, well short of starting it
        const beforeShakeCount = dev.fwLogLines().length;
        t += 100;
        dev.shakeChecked(t);
        const shakeLines = dev.fwLogLines().slice(beforeShakeCount);
        const clearLine = shakeLines.find((l) => l.startsWith("timer: shake cleared to 00:00"));
        check(
            "shaking while SETTING (not yet started) clears to 00:00",
            clearLine === "timer: shake cleared to 00:00 (BOOT recalls 02:30)\r\n",
            JSON.stringify(shakeLines),
        );

        // BOOT afterwards recalls the shaken-away value in one press, same
        // "again is one press" contract every other clear-to-zero path has.
        t += 16;
        dev.buttonRaw(BTN_BOOT, true);
        dev.buttonRaw(BTN_BOOT, false);
        t += 16;
        dev.tickChecked(t);
        const recallLine = dev.fwLogLines().find((l) => l.startsWith("timer: BOOT recalled "));
        check("BOOT afterwards recalls the shaken-away 02:30 in one press", recallLine === "timer: BOOT recalled 02:30\r\n", recallLine);
    }

    // ---- PAUSED: shake wipes to 00:00 even WITHOUT touching first - see
    // timer.c's own comment on why PAUSED counts as "editable" for this. ---
    {
        const dev = await loadDevice();
        dev.appSwitch(APP_TIMER);
        dev.tickChecked(1000);

        let t = tapAt(dev, 60, 1000); // 60 ticks = 5:00
        t = pwrShortClickChecked(dev, t + 50); // start
        for (let i = 0; i < 10; i++) { t += 50; dev.tickChecked(t); }
        t = pwrShortClickChecked(dev, t + 16); // pause

        const beforeShakeCount = dev.fwLogLines().length;
        t += 100;
        dev.shakeChecked(t); // no touch at all before this - straight from PAUSED
        const shakeLines = dev.fwLogLines().slice(beforeShakeCount);
        const clearLine = shakeLines.find((l) => l.startsWith("timer: shake cleared to 00:00"));
        // Clears back to the ORIGINALLY SET value (05:00), not the live
        // paused remainder (a couple of seconds under 5:00 by now) - shake-
        // clear reads s->setTicks, which RUNNING/PAUSED never modify, the
        // same field BOOT's own reset reads - see timer.c's shake-clear
        // branch and handle_alarm()'s identical pattern.
        check(
            "shaking a PAUSED countdown with no prior touch also clears to 00:00, recalling the originally SET 05:00",
            clearLine === "timer: shake cleared to 00:00 (BOOT recalls 05:00)\r\n",
            JSON.stringify(shakeLines),
        );

        // And it is genuinely stopped, not still frozen-but-secretly-primed:
        // the screen must be static from here (SETTING at 00:00), same
        // check the alarm-dismissal tests use.
        const hashRightAfter = dev.fbHash();
        t += 2000;
        dev.tickChecked(t);
        const hashLater = dev.fbHash();
        check("after the shake-clear the screen is static (clean SETTING at 00:00)", hashRightAfter === hashLater);
    }

    // ---- RUNNING: shake does NOT clear - a deliberate judgement call. ------
    {
        const dev = await loadDevice();
        dev.appSwitch(APP_TIMER);
        dev.tickChecked(1000);

        let t = tapAt(dev, 60, 1000); // 5:00
        t = pwrShortClickChecked(dev, t + 50); // start
        for (let i = 0; i < 10; i++) { t += 50; dev.tickChecked(t); }

        const beforeShakeCount = dev.fwLogLines().length;
        const hashBeforeShake = dev.fbHash();
        t += 100;
        dev.shakeChecked(t);
        const shakeLines = dev.fwLogLines().slice(beforeShakeCount);
        check(
            "shaking a RUNNING countdown produces no 'shake cleared' line - it is not read here",
            !shakeLines.some((l) => l.startsWith("timer: shake cleared")),
            JSON.stringify(shakeLines),
        );

        // Confirm it is still genuinely running afterwards (not silently
        // frozen by some other path the shake might have tripped): let more
        // time pass and check the framebuffer actually moved.
        t += 2000;
        dev.tickChecked(t);
        const hashLater = dev.fbHash();
        check(
            "and the countdown is still genuinely running after the shake (framebuffer kept moving)",
            hashLater !== hashBeforeShake,
        );
    }

    // ---- ALARM: shake still dismisses, and does not ALSO fire a second
    // clear on the same tick (the alarm's own unconditional early return in
    // timer_tick() is what guarantees this - see that function's header). ---
    {
        const dev = await loadDevice();
        dev.appSwitch(APP_TIMER);
        dev.tickChecked(1000);

        let t = tapAt(dev, 1, 1000); // smallest tick, 1 = 5s - fastest route to ALARM
        t = pwrShortClickChecked(dev, t + 50);
        const startLine = lastStartLine(dev);
        check("started at 00:05 for a fast route to the alarm", startLine === "timer: start, 00:05\r\n", startLine);

        let ringingLine: string | undefined;
        for (let i = 0; i < 200 && !ringingLine; i++) {
            t += 50;
            dev.tickChecked(t);
            ringingLine = dev.fwLogLines().find((l) => l === "timer: ringing\r\n");
        }
        check("the alarm started ringing", !!ringingLine);
        for (let i = 0; i < 10; i++) { t += 50; dev.tickChecked(t); } // a couple of flash cycles

        const beforeShakeCount = dev.fwLogLines().length;
        t += 16;
        dev.shakeChecked(t);
        const shakeLines = dev.fwLogLines().slice(beforeShakeCount);
        const dismissLines = shakeLines.filter((l) => l.startsWith("timer: alarm dismissed"));
        const clearLines = shakeLines.filter((l) => l.startsWith("timer: shake cleared"));
        check(
            "the shake produced EXACTLY ONE alarm dismissal",
            dismissLines.length === 1,
            JSON.stringify(shakeLines),
        );
        check(
            "and NOT a second, separate shake-clear on the same tick (one gesture, one effect)",
            clearLines.length === 0,
            JSON.stringify(shakeLines),
        );
        check(
            "the dismissal still recalls the value that just alarmed (00:05)",
            dismissLines[0]?.includes("BOOT recalls 00:05") ?? false,
            dismissLines[0],
        );
    }
}

async function main() {
    console.log("=== timer coil redesign: branch cut, point gesture, ceiling, pause-then-edit, shake ===");

    await testBranchCutRoundTrip();
    await testPointSixOClock();
    await testCeiling();
    await testPauseThenEdit();
    await testShakeToClear();

    console.log("\n=== invariants over all of the above: 8px rule + no pixels outside pushed rects ===");
    const byKind = new Map<string, number>();
    for (const v of violations) byKind.set(v.kind, (byKind.get(v.kind) ?? 0) + 1);
    check(
        "every pushed window's row length was a multiple of 8 (decision 0001)",
        (byKind.get("8px-rule") ?? 0) === 0,
        `${byKind.get("8px-rule") ?? 0} violation(s)`,
    );
    check(
        "no framebuffer pixel ever changed outside that tick's own pushed rectangles",
        (byKind.get("outside-push") ?? 0) === 0,
        `${byKind.get("outside-push") ?? 0} violation(s)`,
    );
    if (violations.length > 0) {
        console.log("first few violations:");
        for (const v of violations.slice(0, 10)) console.log(`    [${v.kind}] ${v.detail}`);
    }

    console.log(`\n${passCount} passed, ${failCount} failed`);
    if (failCount > 0) process.exit(1);
}

main();
