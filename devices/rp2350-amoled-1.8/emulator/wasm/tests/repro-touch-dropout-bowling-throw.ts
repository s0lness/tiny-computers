// repro-touch-dropout-bowling-throw: the bowling flick (firmware/apps/
// bowling.c) driven by a touch stream that misbehaves the way the real
// FT3168 does, rather than by a clean pointer.
//
// WHY THIS FILE EXISTS SEPARATELY FROM feature-bowling.ts. The sketchpad's
// colour palette passed all 22 of its clean-input checks and did not work
// in the owner's hands (repro-touch-dropout-palette-open.ts); Connect
// Four's own release verdict got the same separate treatment for the same
// reason (repro-touch-dropout-four-drop.ts). The bowling flick's verb is
// also RELEASE, and it is worse exposed than either: it does not just read
// a position at release, it reads a WINDOW of recent positions and derives
// a VELOCITY from them, which is exactly the measurement this controller
// is worst at (repeats coordinates ~60x per new position, drops contact
// ~34 times a second, and occasionally reports a position 80-250px away
// that HOLDS for a report or three - see bowling.c's own header comment,
// section 1, for the full argument and for what the app does about each).
//
// THE PROFILE. Identical to repro-touch-dropout-four-drop.ts's
// dropoutHeavy: 34 dropout episodes per second, the measured hardware
// figure (TOUCH_POLL_SELFTEST, 798 dropouts over ~23s of contact). At the
// controller's 60Hz report rate that is a per-report loss probability of
// 0.57, so contact is ABSENT on most individual reports.
//
// Run with (after `bun run emulator/wasm/build.ts`):
//
//   bun run emulator/wasm/tests/repro-touch-dropout-bowling-throw.ts
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { TouchSim, type TouchReport } from "../../src/touchsim";
import { TOUCHSIM_DEFAULTS } from "../../src/constants";
import { seededRng, seedFromName } from "../../../tools/gate/touch";

const WASM_PATH = join(import.meta.dir, "..", "dist", "emu.wasm");
const PANEL_W = 368;
const PANEL_H = 448;
const APP_BOWLING = 7; // g_apps[] = { chrono, sketch, timer, four, clock, morpion, dino, bowling, ... } (level removed 2026-08-17)

// Mirrors of bowling.c's own constants, derived the same way rather than
// hand-copied - see feature-bowling.ts's own comment on why (a change to
// the bezel or the layout has to move every dependent number here too).
const LAND_W = PANEL_H;
const BEZEL = 10;
const SAFE_X0 = BEZEL, SAFE_X1 = LAND_W - BEZEL;
const SAFE_Y0 = BEZEL, SAFE_Y1 = PANEL_W - BEZEL;
const LANE_CY = (SAFE_Y0 + SAFE_Y1) / 2;
const BALL_START_X = SAFE_X0 + 58;
const BALL_START_Y = LANE_CY;
const ARM_MS = 40;
const RELEASE_GRACE_MS = 300;
const MIN_LAUNCH_SPEED = 0.16;
const MAX_LAUNCH_SPEED = 1.55;

let passCount = 0;
let failCount = 0;
function check(label: string, ok: boolean, detail?: string) {
    const status = ok ? "PASS" : "FAIL";
    if (ok) passCount++; else failCount++;
    console.log(`[${status}] ${label}${detail ? " - " + detail : ""}`);
}

const bytes = readFileSync(WASM_PATH);
const compiled = await WebAssembly.compile(bytes);

// A FRESH module per trial - trials must not interact (a pin toppled in
// trial 3 changes what trial 4's throw can knock down, a rack mid-reset
// changes what a grab does), exactly four.c's own dropout repro reasoning.
async function freshDevice() {
    let memory!: WebAssembly.Memory;
    const decoder = new TextDecoder();
    const fwLog: string[] = [];
    const instance = await WebAssembly.instantiate(compiled, {
        env: {
            js_log(ptr: number, len: number) { fwLog.push(decoder.decode(new Uint8Array(memory.buffer, ptr, len))); },
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
    exp.emu_app_switch(APP_BOWLING);
    exp.emu_tick(10);
    if (exp.emu_app_current() !== APP_BOWLING) throw new Error("failed to switch into bowling");
    fwLog.length = 0;
    return {
        // The report is in PANEL coordinates already (TouchSim works in the
        // panel's own space, like the real controller): the LANDSCAPE point
        // a caller wants is converted on the way in, at setPointer.
        feed(report: TouchReport, nowMs: number) {
            exp.emu_touch(report.fingers === 1 ? 1 : 0, report.x, report.y);
            exp.emu_tick(nowMs);
        },
        drainLog(): string[] { const out = fwLog.slice(); fwLog.length = 0; return out; },
    };
}

// gfx.h's mapping is landscape (lx, ly) -> panel (PANEL_W-1-ly, lx).
function landToPanel(lx: number, ly: number): [number, number] {
    return [PANEL_W - 1 - Math.round(ly), Math.round(lx)];
}

const dropoutHeavy = { ...TOUCHSIM_DEFAULTS, dropoutsEnabled: true, dropoutsPerSec: 34, straysEnabled: false };
const STEP_MS = 1000 / dropoutHeavy.reportRateHz;

async function main() {
    console.log("=== bowling's grab-drag-release, under a dropout-heavy touch stream ===\n");
    console.log(`    profile: ${dropoutHeavy.dropoutsPerSec} dropout episodes/sec at ${dropoutHeavy.reportRateHz}Hz ` +
        `(p=${(dropoutHeavy.dropoutsPerSec / dropoutHeavy.reportRateHz).toFixed(2)} per report), ` +
        `RELEASE_GRACE_MS=${RELEASE_GRACE_MS}\n`);

    // ---- scenario A: a thumb held STILL on the ball for 3 seconds must
    // not launch it by itself. This is the palette bug's own shape - a
    // finger that is DOWN but not MOVING, which dropouts make
    // indistinguishable from a lift unless the firmware keeps its own
    // contact clock (bowling.c's lastContactMs, copied from four.c's).
    const HOLD_TRIALS = 25;
    const HOLD_MS = 3000;
    let heldWithoutLaunching = 0;
    let totalDropouts = 0;
    for (let i = 0; i < HOLD_TRIALS; i++) {
        const dev = await freshDevice();
        const sim = new TouchSim(dropoutHeavy, PANEL_W, PANEL_H, seededRng(seedFromName(`bowling-hold-${i}`)));
        const [px, py] = landToPanel(BALL_START_X, BALL_START_Y);
        sim.setPointer(true, px, py);
        let t = 1000;
        let launched = false;
        for (let held = 0; held < HOLD_MS; held += STEP_MS) {
            t += STEP_MS;
            dev.feed(sim.poll(t), t);
            if (dev.drainLog().some((l) => l.includes("bowling: launch"))) { launched = true; break; }
        }
        totalDropouts += sim.simDropouts;
        if (!launched) heldWithoutLaunching++;
    }
    const holdRate = (heldWithoutLaunching / HOLD_TRIALS) * 100;
    console.log(`    ${totalDropouts} simulated dropout episodes across ${HOLD_TRIALS} x ${HOLD_MS}ms of held contact`);
    // A RATE, not "all of them" - the same Bernoulli-process argument
    // four.c's own hold scenario makes (its comment derives ~0.14% expected
    // false losses per trial from this exact profile over a 3s hold; 96%
    // leaves the same wide margin against a broken implementation, which
    // would sit near 0%).
    check("a thumb held on the ball for 3 seconds does not launch it by itself",
        holdRate >= 96, `${heldWithoutLaunching}/${HOLD_TRIALS} trials survived the hold (${holdRate.toFixed(0)}%)`);

    // ---- scenario B: the whole gesture, end to end, at realistic weather:
    // grab, drag forward down the lane, hold an instant on target, release.
    // What matters is not just "did it launch" but "did it launch FORWARD,
    // at a launch speed inside the app's own declared safety bounds" -
    // bowling.c's header comment argues at length that a corrupted last
    // sample must never produce anything outside [MIN_LAUNCH_SPEED,
    // MAX_LAUNCH_SPEED], and this is where that claim gets checked against
    // the actual dropout-heavy stream rather than just asserted in prose.
    console.log("");
    const THROW_TRIALS = 30;
    const SETTLE_MS = ARM_MS + 200; // time to arm before the drag starts
    const DRAG_MS = 260;
    const HOLD_ON_TARGET_MS = 60;
    let firedForward = 0, firedBackwardOrNone = 0, neverFired = 0;
    let speedOutOfBounds = 0;
    for (let i = 0; i < THROW_TRIALS; i++) {
        const dev = await freshDevice();
        const sim = new TouchSim(dropoutHeavy, PANEL_W, PANEL_H, seededRng(seedFromName(`bowling-throw-${i}`)));
        let t = 1000;

        const [sx, sy] = landToPanel(BALL_START_X, BALL_START_Y);
        sim.setPointer(true, sx, sy);
        for (let e = 0; e < SETTLE_MS; e += STEP_MS) { t += STEP_MS; dev.feed(sim.poll(t), t); }

        // A slight lateral wobble per trial, so this exercises more than
        // one exact straight line - still clearly forward (+X), same
        // spirit as four.c's per-trial column variation.
        const targetX = BALL_START_X + 140;
        const targetY = LANE_CY + ((i % 5) - 2) * 12;
        for (let e = 0; e < DRAG_MS; e += STEP_MS) {
            const u = e / DRAG_MS;
            const lx = BALL_START_X + (targetX - BALL_START_X) * u;
            const ly = BALL_START_Y + (targetY - BALL_START_Y) * u;
            const [px, py] = landToPanel(lx, ly);
            sim.setPointer(true, px, py);
            t += STEP_MS;
            dev.feed(sim.poll(t), t);
        }
        const [tx, ty] = landToPanel(targetX, targetY);
        sim.setPointer(true, tx, ty);
        for (let e = 0; e < HOLD_ON_TARGET_MS; e += STEP_MS) { t += STEP_MS; dev.feed(sim.poll(t), t); }

        dev.drainLog();
        sim.setPointer(false, 0, 0);
        let launchLine: string | undefined;
        for (let e = 0; e < RELEASE_GRACE_MS + 300; e += STEP_MS) {
            t += STEP_MS;
            dev.feed(sim.poll(t), t);
            for (const l of dev.drainLog()) if (l.includes("bowling: launch")) launchLine = l;
        }

        if (!launchLine) { neverFired++; continue; }
        // bowling.c's own printf: vx has no sign character (the app only
        // ever prints a launch with vx > 0), vy carries an explicit sign
        // character because %d on a truncated float between -1 and 0 drops
        // the sign entirely - see that printf's own comment.
        const m = launchLine.match(/vx=(\d+)\.(\d+)\s+vy=(-?)(\d+)\.(\d+)/);
        if (!m) { firedBackwardOrNone++; continue; }
        const vx = Number(m[1]) + Number(m[2]) / 1000;
        const vySign = m[3] === "-" ? -1 : 1;
        const vy = vySign * (Number(m[4]) + Number(m[5]) / 1000);
        if (vx <= 0) { firedBackwardOrNone++; continue; }
        firedForward++;
        const speed = Math.sqrt(vx * vx + vy * vy);
        // A little float slack over the declared bounds for the printf's
        // own truncation to three decimal places, not a loosened claim.
        if (speed < MIN_LAUNCH_SPEED - 0.01 || speed > MAX_LAUNCH_SPEED + 0.01) {
            speedOutOfBounds++;
            console.log(`    trial ${i}: speed ${speed.toFixed(3)} outside [${MIN_LAUNCH_SPEED}, ${MAX_LAUNCH_SPEED}] - ${launchLine.trim()}`);
        }
    }
    const forwardRate = (firedForward / THROW_TRIALS) * 100;
    console.log(`    ${firedForward}/${THROW_TRIALS} grab-drag-release gestures launched forward (${forwardRate.toFixed(0)}%); ` +
        `${neverFired} never fired, ${firedBackwardOrNone} fired but not forward`);
    check("a real forward drag-and-release launches the ball under dropout-heavy weather, most of the time",
        forwardRate >= 80, `${forwardRate.toFixed(0)}% over ${THROW_TRIALS} trials`);
    check("every launch that DID fire had a speed inside the app's own declared [MIN,MAX] bounds - no absurd result",
        speedOutOfBounds === 0, `${speedOutOfBounds}/${firedForward} out of bounds`);

    // ---- scenario C: bursts of phantom contacts near the ball, at every
    // spacing, must never arm a launch - the same hard (non-statistical)
    // property four.c's own ARM_SAMPLES test asserts, copied here because
    // bowling.c copies the identical arming rule.
    console.log("");
    let burstLaunches = 0;
    const burstCases: string[] = [];
    for (const spacingMs of [17, 33, 60, 120, 200, 400]) {
        for (const count of [1, 2, 3]) {
            const devC = await freshDevice();
            let tC = 1000;
            const [sx, sy] = landToPanel(BALL_START_X, BALL_START_Y);
            for (let k = 0; k < count; k++) {
                devC.feed({ fingers: 1, x: sx, y: sy }, tC);
                for (let e = STEP_MS; e < spacingMs; e += STEP_MS) {
                    tC += STEP_MS;
                    devC.feed({ fingers: 0, x: 0, y: 0 }, tC);
                }
                tC += STEP_MS;
            }
            for (let e = 0; e < RELEASE_GRACE_MS + 600; e += STEP_MS) {
                tC += STEP_MS;
                devC.feed({ fingers: 0, x: 0, y: 0 }, tC);
            }
            if (devC.drainLog().some((l) => l.includes("bowling: launch"))) {
                burstLaunches++;
                burstCases.push(`${count} contact(s) ${spacingMs}ms apart`);
            }
        }
    }
    check("no burst of up to three phantom contacts, at any spacing, ever launches the ball",
        burstLaunches === 0, burstLaunches === 0 ? "18 burst shapes tried" : burstCases.join(", "));

    // ---- scenario D: the emulator's own modelled stray process, run long,
    // with nothing touching the glass at all - the rate the hardware
    // profile actually calibrates, so it is the rate a claim about the
    // shipped device can honestly be made at.
    const strayProfile = { ...TOUCHSIM_DEFAULTS, dropoutsEnabled: false, straysEnabled: true };
    const devD = await freshDevice();
    const simD = new TouchSim(strayProfile, PANEL_W, PANEL_H, seededRng(seedFromName("bowling-strays")));
    simD.setPointer(false, 0, 0);
    let strayLaunches = 0;
    let tD = 1000;
    for (let e = 0; e < 120000; e += STEP_MS) {
        tD += STEP_MS;
        devD.feed(simD.poll(tD), tD);
        if (devD.drainLog().some((l) => l.includes("bowling: launch"))) strayLaunches++;
    }
    console.log(`    ${simD.simStrays} stray contacts synthesised over 120s at the emulator's modelled rate ` +
        `(${strayProfile.straysPerSec}/s), with nothing touching the glass`);
    check("two minutes of the modelled stray process launches nothing", strayLaunches === 0,
        `${strayLaunches} launch(es) from strays`);

    console.log(`\n${passCount} passed, ${failCount} failed`);
    if (failCount > 0) process.exit(1);
}

main();
