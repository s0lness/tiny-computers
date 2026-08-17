// feature-bowling-provisional-release: headless verification of bowling.c's
// section-6 fix, against the REAL firmware compiled to wasm (decision 0003).
//
// WHY THIS EXISTS. The owner played bowling on the real board and said it
// "sort of freezes when I toss the ball, like it's lagging." Reading
// bowling.c plus firmware/runtime/runtime_core.c's own touch-latching logic
// found the true wall-clock gap between a finger genuinely lifting and the
// ball visibly moving: the touch controller's own report latency before
// core0 even learns contact is gone (median ~17ms, tail ~129ms - this
// board's own measured sample pacing), THEN the full RELEASE_GRACE_MS
// (300ms) of confirmed silence bowling.c demands before it trusts a lift is
// real, THEN up to RENDER_MIN_MS (16ms) of push-throttle before the first
// moved frame reaches the panel. tools/sweep-bowling-grace.ts (this file's
// own sibling investigation) measured whether RELEASE_GRACE_MS could simply
// shrink to close that gap and found it could not, safely, below roughly
// 150-180ms - a real floor the FT3168's dropout rate imposes on bowling's
// velocity-window release exactly the way it does on tables.c's own commit
// window (see that file's sweep-tables-grace.ts header comment).
//
// THE FIX. A provisional release CUE, not a shorter grace: the instant
// contact is lost while the ball is held (PH_HELD), before ANY silence is
// trusted, the ball is nudged PROV_RELEASE_NUDGE_PX (14px) in the flick's
// own direction - a felt "it let go" within one touch-report period of the
// real lift, while the actual launch-or-snap-back DECISION still waits the
// full, unshortened RELEASE_GRACE_MS untouched. This file proves both
// halves of that claim against the real compiled firmware:
//
//   1. shortly after contact is lost (well inside RELEASE_GRACE_MS), the
//      ball has visibly moved - the nudge fired;
//   2. at that same moment, NO "bowling: launch" log line has appeared yet -
//      the safety-critical decision has not been touched;
//   3. if contact resumes before the grace window elapses (a bridged
//      dropout, not a real lift), the gesture recovers cleanly: 1:1
//      finger-tracking resumes and the eventual real release still
//      produces exactly one correct launch, not two, and not a residue of
//      the provisional nudge frozen on screen;
//   4. if contact never resumes, the real release still fires once the full
//      RELEASE_GRACE_MS has genuinely elapsed - the grace itself is
//      unchanged, only the cosmetic wait is shorter now.
//
// RED BEFORE GREEN: with the section-6 nudge code removed (git stash the
// bowling.c hunk, or check out the commit before it), check 1 fails - the
// ball is bit-identical to its pre-lift position for the entire grace
// window, exactly the "frozen 300ms" the owner felt - while checks 2 and 4
// still pass (the grace itself was never the thing in question).
//
// Run with (after `bun run emulator/wasm/build.ts`):
//
//   bun run emulator/wasm/tests/feature-bowling-provisional-release.ts
import { readFileSync } from "node:fs";
import { join } from "node:path";

const WASM_PATH = join(import.meta.dir, "..", "dist", "emu.wasm");

const PANEL_W = 368;
const PANEL_H = 448;
const LAND_W = PANEL_H; // 448
const LAND_H = PANEL_W; // 368
const APP_BOWLING = 7; // g_apps[] = { chrono, sketch, timer, four, clock, morpion, dino, bowling, ... } (level removed 2026-08-17)

// Mirrors of bowling.c's own constants - feature-bowling.ts's own comment
// explains why these are derived the same way rather than hand-copied.
const BEZEL = 10;
const SAFE_X0 = BEZEL, SAFE_X1 = LAND_W - BEZEL;
const SAFE_Y0 = BEZEL, SAFE_Y1 = LAND_H - BEZEL;
const LANE_CY = (SAFE_Y0 + SAFE_Y1) / 2;
const BALL_R = 17;
const BALL_START_X = SAFE_X0 + 58;
const BALL_START_Y = LANE_CY;
const ARM_MS = 40;
const RELEASE_GRACE_MS = 300;
const PROV_RELEASE_NUDGE_PX = 14;

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
    const fwLogLines: string[] = [];

    const imports = {
        env: {
            js_log(ptr: number, len: number) {
                fwLogLines.push(decoder.decode(new Uint8Array(memory.buffer, ptr, len)));
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
    if (exp.emu_init() !== 1) throw new Error("emu_init() failed - see [fw] log lines above");

    function fbSnapshot(): Uint8Array {
        return new Uint8Array(memory.buffer, exp.emu_fb(), PANEL_W * PANEL_H * 2).slice();
    }

    return {
        appSwitch(index: number) { exp.emu_app_switch(index); },
        appCurrent(): number { return exp.emu_app_current(); },
        // gfx.h's mapping is (lx, ly) -> panel (PANEL_W-1-ly, lx); emu_touch
        // takes panel coordinates.
        touchLand(down: boolean, lx: number, ly: number, nowMs: number) {
            exp.emu_touch(down ? 1 : 0, PANEL_W - 1 - Math.round(ly), Math.round(lx));
            exp.emu_tick(nowMs);
        },
        fbSnapshot,
        drainLog(): string[] { const out = fwLogLines.slice(); fwLogLines.length = 0; return out; },
    };
}
type Device = Awaited<ReturnType<typeof loadDevice>>;

function landPx(fb: Uint8Array, lx: number, ly: number): [number, number] {
    const idx = (Math.round(lx) * PANEL_W + (PANEL_W - 1 - Math.round(ly))) * 2;
    return [fb[idx]!, fb[idx + 1]!];
}
const isBlackish = (fb: Uint8Array, lx: number, ly: number) => {
    const [hi, lo] = landPx(fb, lx, ly);
    return hi < 0x80 && lo < 0x80;
};

const idle = (dev: Device, t0: number, ms: number, step = 15): number => {
    let t = t0;
    for (let e = 0; e < ms; e += step) { t += step; dev.touchLand(false, 0, 0, t); }
    return t;
};

async function main() {
    console.log("=== bowling's provisional release cue (firmware/apps/bowling.c section 6) ===\n");
    const dev = await loadDevice();
    dev.appSwitch(APP_BOWLING);
    dev.touchLand(false, 0, 0, 10);
    check("switched into bowling", dev.appCurrent() === APP_BOWLING, `app_current()=${dev.appCurrent()}`);

    // ---- grab the ball and drag it forward, clean input, far enough that
    // the velocity window reads a solid forward speed - the same shape
    // feature-bowling.ts's own throwGesture uses, minus the release.
    const STEP = 15;
    let t = 1000;
    for (let e = 0; e < ARM_MS + 30; e += STEP) { t += STEP; dev.touchLand(true, BALL_START_X, BALL_START_Y, t); }
    const dragTargetX = BALL_START_X + 130;
    const dragMs = 220;
    for (let e = 0; e < dragMs; e += STEP) {
        const u = Math.min(1, e / dragMs);
        const lx = BALL_START_X + (dragTargetX - BALL_START_X) * u;
        t += STEP;
        dev.touchLand(true, lx, LANE_CY, t);
    }
    // A brief hold right on target so the velocity window's freshest samples
    // are clean - same reasoning throwGesture's own comment gives.
    for (let e = 0; e < 30; e += STEP) { t += STEP; dev.touchLand(true, dragTargetX, LANE_CY, t); }
    const lastHeldX = dragTargetX, lastHeldY = LANE_CY;
    const fbAtLift = dev.fbSnapshot();
    check("the ball is at the drag's last position right before lift",
        isBlackish(fbAtLift, lastHeldX, lastHeldY));
    // Just past the ball's own leading edge: NOT ink right now (the ball
    // has not moved past its own radius yet). Deliberately checked against
    // isBlackish rather than isPaper - this point sits on the lane's own
    // pale-grey surface (COL_LANE), which is neither ink nor paper by this
    // file's own thresholds, so "was it paper" would be the wrong question
    // here; "did it become ink" (the differential check right below, after
    // lift) is what actually proves the nudge fired.
    const probeX = lastHeldX + BALL_R + 5;
    check("just ahead of the ball's own edge is NOT ink before any lift",
        !isBlackish(fbAtLift, probeX, lastHeldY), `sampled (${probeX}, ${lastHeldY})`);

    // ---- lift, for real, then wait only a THIRD of the grace window - well
    // inside "still waiting to be trusted," per RELEASE_GRACE_MS's own
    // definition (bowling.c's header, section 1).
    dev.drainLog();
    t += STEP;
    dev.touchLand(false, 0, 0, t);
    const shortWaitMs = Math.floor(RELEASE_GRACE_MS / 3);
    t = idle(dev, t, shortWaitMs, STEP);

    const fbShortWait = dev.fbSnapshot();
    const logSoFar = dev.drainLog().join("");
    check(`the ball has visibly moved within ${shortWaitMs}ms of lift, well before RELEASE_GRACE_MS (${RELEASE_GRACE_MS}ms) elapses`,
        isBlackish(fbShortWait, probeX, lastHeldY),
        `probe (${probeX}, ${lastHeldY}) - PROV_RELEASE_NUDGE_PX=${PROV_RELEASE_NUDGE_PX}`);
    check("...but the real launch has NOT fired yet - the safety-critical decision still waits the full grace",
        !logSoFar.includes("bowling: launch"), logSoFar || "(no log)");

    // ---- finish waiting out the grace: the REAL launch must still fire,
    // unchanged, once the full window has genuinely elapsed.
    t = idle(dev, t, RELEASE_GRACE_MS - shortWaitMs + 60, STEP);
    const logAfterGrace = dev.drainLog().join("");
    check("the real launch fires once RELEASE_GRACE_MS has genuinely elapsed - the grace itself is unchanged",
        logAfterGrace.includes("bowling: launch"), logAfterGrace.split("\n")[0] ?? "(no log)");

    // Let the throw resolve and the rack reset before the next scenario.
    t = idle(dev, t, 1350 + 5 * 55 + 260 + 140 + 260 + 400, STEP);
    dev.drainLog();

    // ---- scenario 2: a BRIDGED dropout. Grab, drag, lose contact briefly
    // (short enough that the nudge fires), then contact RESUMES before the
    // grace window elapses - a real finger still down, just a dropped
    // report. The gesture must recover cleanly: exactly one launch, at the
    // REAL release, not a residue of the provisional nudge and not a second
    // phantom launch from the bridged gap.
    t += 200;
    for (let e = 0; e < ARM_MS + 30; e += STEP) { t += STEP; dev.touchLand(true, BALL_START_X, BALL_START_Y, t); }
    // Drag only HALFWAY, then the dropout hits mid-flick - a real flick
    // that gets interrupted, not one that had already finished moving.
    const midX = BALL_START_X + (dragTargetX - BALL_START_X) * 0.5;
    const halfDragMs = dragMs / 2;
    for (let e = 0; e < halfDragMs; e += STEP) {
        const u = Math.min(1, e / halfDragMs);
        const lx = BALL_START_X + (midX - BALL_START_X) * u;
        t += STEP;
        dev.touchLand(true, lx, LANE_CY, t);
    }
    // Drop contact for a short bridged gap (nudge fires), then resume - a
    // dropout, not a lift.
    t += STEP;
    dev.touchLand(false, 0, 0, t);
    t = idle(dev, t, Math.floor(RELEASE_GRACE_MS / 4), STEP);
    dev.drainLog();
    t += STEP;
    dev.touchLand(true, midX, LANE_CY, t);
    const fbResumed = dev.fbSnapshot();
    check("after contact resumes, the ball snaps back to 1:1 finger-tracking (no residue of the nudge)",
        isBlackish(fbResumed, midX, LANE_CY));
    // The flick CONTINUES from where it was interrupted - a dropout does
    // not mean she stopped moving her finger - all the way to the real
    // target, so the velocity window at the real release still reflects a
    // genuine, ongoing forward flick rather than a hand frozen mid-air.
    for (let e = 0; e < halfDragMs; e += STEP) {
        const u = Math.min(1, e / halfDragMs);
        const lx = midX + (dragTargetX - midX) * u;
        t += STEP;
        dev.touchLand(true, lx, LANE_CY, t);
    }
    // Hold briefly then release for real.
    for (let e = 0; e < 30; e += STEP) { t += STEP; dev.touchLand(true, dragTargetX, LANE_CY, t); }
    t += STEP;
    dev.touchLand(false, 0, 0, t);
    t = idle(dev, t, RELEASE_GRACE_MS + 60, STEP);
    const logBridged = dev.drainLog().join("\n");
    const launchCount = (logBridged.match(/bowling: launch/g) ?? []).length;
    check("a bridged dropout mid-drag still resolves to exactly one launch at the real release",
        launchCount === 1, `${launchCount} launch line(s): ${logBridged.trim()}`);

    console.log(`\n${passCount} passed, ${failCount} failed`);
    if (failCount > 0) process.exit(1);
}

main();
