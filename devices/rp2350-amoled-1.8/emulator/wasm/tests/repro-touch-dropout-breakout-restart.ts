// repro-touch-dropout-breakout-restart: breakout.c's restart-from-game-over
// tap, driven by a touch stream that misbehaves the way the real FT3168
// does (AGENTS.md: ~34 dropout episodes/sec while a finger is down,
// occasional strays with nothing touching), rather than by a clean
// mouse-like tap.
//
// WHY THIS FILE EXISTS SEPARATELY FROM feature-breakout.ts. AGENTS.md's own
// "Regression tests" section is explicit: "a feature driven by touch needs
// BOTH kinds of file" - a feature-* file proves what the gesture is
// SUPPOSED to do, under clean input; this file proves the SAME gesture
// still behaves once the controller is doing what it really does. This
// project has already shipped one feature that passed every clean-input
// check and misfired in the owner's hands on exactly this hazard (the
// sketchpad's palette, see repro-touch-dropout-palette-open.ts). Breakout's
// restart gesture is dino.c's own tap_tick() arming/release-grace idiom,
// reused verbatim (see breakout.c's own header comment, "GAME OVER, AND HOW
// SHE STARTS AGAIN"), so this is the corresponding half of that reuse:
// proof that the SAME constants, applied to a third trigger now (four.c's
// slide, dino.c's jump/restart, and now breakout's restart), still hold
// under the SAME measured profile repro-touch-dropout-dino-jump.ts uses -
// this file mirrors that one closely rather than inventing a new shape for
// an identical state machine.
//
// THE STAKES ARE LOWER THAN DINO'S OWN JUMP TEST, WORTH SAYING PLAINLY. A
// false restart fire only matters while s->lifePhase == LP_GAMEOVER_WAIT -
// breakout.c reads touch nowhere else (see that file's header, "WHY THIS
// EXCEPTION TO READS NO TOUCH IS SAFE TO MAKE NARROWLY"), so a dropout or a
// stray during ordinary play, or during the brief life-lost freeze, cannot
// reach this gesture at all; there is nothing here for it to misfire. What
// this file actually rules out is a false or missing fire ONCE the table is
// already sitting at game over - firing twice from one press would restart
// twice in a row (harmless, but not "exactly once"), and firing on a burst
// too small to be a real press would end the wordless "waiting for a tap"
// picture with nobody there.
//
// THE PROFILE. Identical to repro-touch-dropout-dino-jump.ts's
// dropoutHeavy: 34 dropout episodes/sec, back-calculated from a real
// hardware session. At the controller's 60Hz report rate that is a
// per-report loss probability of 0.57 - contact is ABSENT on most
// individual reports, so the firmware has to bridge continuously.
//
// Run with (after `bun run emulator/wasm/build.ts`):
//   bun run emulator/wasm/tests/repro-touch-dropout-breakout-restart.ts
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { TouchSim, type TouchReport } from "../../src/touchsim";
import { TOUCHSIM_DEFAULTS } from "../../src/constants";
import { seededRng, seedFromName } from "../../../tools/gate/touch";

const WASM_PATH = join(import.meta.dir, "..", "dist", "emu.wasm");
const TOUCH_X = 184, TOUCH_Y = 224; // breakout's restart reads only touchDown - any point does

let passCount = 0;
let failCount = 0;
function check(label: string, ok: boolean, detail?: string) {
    const status = ok ? "PASS" : "FAIL";
    if (ok) passCount++; else failCount++;
    console.log(`[${status}] ${label}${detail ? " - " + detail : ""}`);
}

const bytes = readFileSync(WASM_PATH);
const compiled = await WebAssembly.compile(bytes);
let APP_BREAKOUT = -1;

// A fresh module per trial - trials must not interact (a game-over table
// already reached in one trial should not leak into the next).
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
    if (APP_BREAKOUT < 0) {
        const ptr = exp.emu_device();
        const b = new Uint8Array(memory.buffer, ptr);
        let end = 0; while (b[end] !== 0) end++;
        const apps: string[] = JSON.parse(decoder.decode(b.subarray(0, end))).apps || [];
        APP_BREAKOUT = apps.indexOf("breakout");
        if (APP_BREAKOUT < 0) throw new Error(`"breakout" not in app table: ${JSON.stringify(apps)}`);
    }
    exp.emu_tick(0);
    exp.emu_app_switch(APP_BREAKOUT);
    exp.emu_tick(10);
    if (exp.emu_app_current() !== APP_BREAKOUT) throw new Error("failed to switch into breakout");
    fwLog.length = 0;
    return {
        feed(report: TouchReport, nowMs: number) {
            exp.emu_touch(report.fingers === 1 ? 1 : 0, report.x, report.y);
            exp.emu_tick(nowMs);
        },
        tilt(x: number, y: number, z: number, nowMs: number) {
            exp.emu_sensor_vector(1, x, y, z);
            exp.emu_tick(nowMs);
        },
        drainLog(): string[] { const out = fwLog.slice(); fwLog.length = 0; return out; },
    };
}
type Dev = Awaited<ReturnType<typeof freshDevice>>;

// Drives the table to a confirmed LP_GAMEOVER_WAIT with NO touch at all -
// tilt pinned hard to one rail, the same "the paddle can no longer catch a
// ball on the other side of the field" setup feature-breakout.ts's own
// lives tests use (landscape gx=-1: panel Y carries landscape gx for a
// landscape app - see feature-breakout.ts's own rotation comment). Reaching
// game over is setup, not the gesture under test, exactly the way
// repro-touch-dropout-dino-jump.ts's own startRunCleanly() treats starting a
// run as setup for testing the JUMP, not the thing itself.
async function reachGameOverCleanly(dev: Dev, t: number): Promise<number> {
    const stepMs = 40, ceilingMs = 60000;
    for (let elapsed = 0; elapsed < ceilingMs; elapsed += stepMs) {
        t += stepMs;
        dev.tilt(0, -1, 0, t);
        if (dev.drainLog().some((l) => l.includes("breakout: game over"))) return t;
    }
    throw new Error(`reachGameOverCleanly: never reached game over within ${ceilingMs}ms of sim time`);
}

const dropoutHeavy = { ...TOUCHSIM_DEFAULTS, dropoutsEnabled: true, dropoutsPerSec: 34, straysEnabled: false };
const strayProfile = { ...TOUCHSIM_DEFAULTS, dropoutsEnabled: false, straysEnabled: true };
const STEP_MS = 1000 / dropoutHeavy.reportRateHz;

async function main() {
    console.log("=== breakout's restart-from-game-over tap, under a dropout-heavy touch stream ===\n");
    console.log(`    profile: ${dropoutHeavy.dropoutsPerSec} dropout episodes/sec at ${dropoutHeavy.reportRateHz}Hz ` +
        `(p=${(dropoutHeavy.dropoutsPerSec / dropoutHeavy.reportRateHz).toFixed(2)} per report)\n`);

    // ---- A. A THUMB HELD STILL ON THE GLASS, ONCE AT GAME OVER, MUST
    // RESTART EXACTLY ONCE, NOT REPEATEDLY. The palette bug's own shape: a
    // finger that is DOWN but not moving, which is what a controller's
    // dropouts make indistinguishable from a rapid sequence of
    // lift-and-retouch unless the firmware bridges short gaps rather than
    // believing every one of them. --------------------------------------
    const HOLD_TRIALS = 15;
    const HOLD_MS = 3000;
    let exactlyOneRestart = 0;
    let totalDropouts = 0;
    const restartCounts: number[] = [];
    for (let i = 0; i < HOLD_TRIALS; i++) {
        const dev = await freshDevice();
        let t = await reachGameOverCleanly(dev, 1000);
        const sim = new TouchSim(dropoutHeavy, 368, 448, seededRng(seedFromName(`breakout-restart-hold-${i}`)));
        sim.setPointer(true, TOUCH_X, TOUCH_Y);
        let restarts = 0;
        for (let held = 0; held < HOLD_MS; held += STEP_MS) {
            t += STEP_MS;
            dev.feed(sim.poll(t), t);
            restarts += dev.drainLog().filter((l) => l.includes("breakout: restart")).length;
        }
        totalDropouts += sim.simDropouts;
        restartCounts.push(restarts);
        if (restarts === 1) exactlyOneRestart++;
    }
    const holdRate = (exactlyOneRestart / HOLD_TRIALS) * 100;
    console.log(`    ${totalDropouts} simulated dropout episodes across ${HOLD_TRIALS} x ${HOLD_MS}ms of held contact`);
    console.log(`    restart counts observed: ${restartCounts.join(",")}`);
    check("a thumb held on the glass at game over restarts exactly once, not on every dropout-then-redown",
        holdRate === 100, `${exactlyOneRestart}/${HOLD_TRIALS} trials restarted exactly once (${holdRate.toFixed(0)}%)`);

    // ---- B. NO BURST OF FEWER THAN TAP_ARM_SAMPLES PHANTOM CONTACTS CAN
    // EVER RESTART, AT ANY SPACING - the hard, non-statistical guarantee
    // apps/four.c's own test makes for its arming logic, applied here to a
    // third reuse of the same three numbers. ------------------------------
    console.log("");
    let burstRestarts = 0;
    const burstCases: string[] = [];
    for (const spacingMs of [17, 33, 60, 120, 200, 400]) {
        for (const count of [1, 2, 3]) {
            const dev = await freshDevice();
            let t = await reachGameOverCleanly(dev, 1000);
            for (let k = 0; k < count; k++) {
                dev.feed({ fingers: 1, x: TOUCH_X, y: TOUCH_Y }, t);
                for (let e = STEP_MS; e < spacingMs; e += STEP_MS) {
                    t += STEP_MS;
                    dev.feed({ fingers: 0, x: 0, y: 0 }, t);
                }
                t += STEP_MS;
            }
            for (let e = 0; e < 700; e += STEP_MS) {
                t += STEP_MS;
                dev.feed({ fingers: 0, x: 0, y: 0 }, t);
            }
            if (dev.drainLog().some((l) => l.includes("breakout: restart"))) {
                burstRestarts++;
                burstCases.push(`${count} contact(s) ${spacingMs}ms apart`);
            }
        }
    }
    check("no burst of up to three phantom contacts, at any spacing, ever fires a restart",
        burstRestarts === 0, burstRestarts === 0 ? "18 burst shapes tried" : burstCases.join(", "));

    // ---- C. the emulator's own modelled stray process, run long, with
    // nothing touching the glass, once the table is sitting at game over -
    // exactly repro-touch-dropout-dino-jump.ts's own scenario C, applied to
    // this gesture. A stray restart here would mean the toy appears to
    // start playing itself while sitting untouched at the one screen this
    // app ever reads touch on. --------------------------------------------
    console.log("");
    const dev = await freshDevice();
    let t = await reachGameOverCleanly(dev, 1000);
    const sim = new TouchSim(strayProfile, 368, 448, seededRng(seedFromName("breakout-restart-strays")));
    sim.setPointer(false, 0, 0);
    let strayFires = 0;
    for (let e = 0; e < 120000; e += STEP_MS) {
        t += STEP_MS;
        dev.feed(sim.poll(t), t);
        if (dev.drainLog().some((l) => l.includes("breakout: restart"))) strayFires++;
    }
    console.log(`    ${sim.simStrays} stray contacts synthesised over 120s at the emulator's modelled rate ` +
        `(${strayProfile.straysPerSec}/s), with nothing touching the glass`);
    check("two minutes of the modelled stray process, sitting at game over, fires no restart", strayFires === 0,
        `${strayFires} unwanted fire(s)`);

    console.log(`\n${passCount} passed, ${failCount} failed`);
    if (failCount > 0) process.exit(1);
}

main();
