// repro-touch-dropout-dino-jump: dino.c's tap-to-jump gesture, driven by a
// touch stream that misbehaves the way the real FT3168 does (AGENTS.md:
// ~34 dropout episodes/sec while a finger is down, occasional strays with
// nothing touching), rather than by a clean mouse-like tap.
//
// WHY THIS FILE EXISTS SEPARATELY FROM feature-dino.ts. This project has
// already shipped one feature that passed every clean-input check and
// misfired in the owner's hands on exactly this hazard (the sketchpad's
// palette, see repro-touch-dropout-palette-open.ts). dino.c's own arming
// state machine (tap_tick()) is apps/four.c's ARM_SAMPLES/ARM_MS/
// ARM_RATE_HZ/RELEASE_GRACE_MS idiom, reused verbatim rather than
// re-derived - this file is the corresponding half of that reuse: proof
// that the SAME constants, applied to a jump trigger instead of a slide
// gesture, still hold under the SAME measured profile
// repro-touch-dropout-four-drop.ts uses.
//
// The stakes here are a genuine step up from Connect Four's own dropout
// risk. A false jump-fire while she is holding the glass mid-jump does
// nothing (dino.c already ignores a tap while airborne); a false jump-fire
// while she is simply RESTING a thumb on the glass, thinking, could launch
// her into an obstacle she was not ready to jump over - a stray that
// "plays the game for her" is exactly the toy-appears-broken failure this
// project's other dropout repros exist to rule out.
//
// THE PROFILE. Identical to repro-touch-dropout-four-drop.ts's
// dropoutHeavy: 34 dropout episodes/sec, back-calculated from a real
// hardware session. At the controller's 60Hz report rate that is a
// per-report loss probability of 0.57 - contact is ABSENT on most
// individual reports, so the firmware has to bridge continuously.
//
// Run with (after `bun run emulator/wasm/build.ts`):
//   bun run emulator/wasm/tests/repro-touch-dropout-dino-jump.ts
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { TouchSim, type TouchReport } from "../../src/touchsim";
import { TOUCHSIM_DEFAULTS } from "../../src/constants";
import { seededRng, seedFromName } from "../../../tools/gate/touch";

const WASM_PATH = join(import.meta.dir, "..", "dist", "emu.wasm");
const APP_DINO = 7; // g_apps[] = { chrono, sketch, timer, four, level, clock, morpion, dino, ... }
const TOUCH_X = 184, TOUCH_Y = 224; // dino.c reads only touchDown - any point does

let passCount = 0;
let failCount = 0;
function check(label: string, ok: boolean, detail?: string) {
    const status = ok ? "PASS" : "FAIL";
    if (ok) passCount++; else failCount++;
    console.log(`[${status}] ${label}${detail ? " - " + detail : ""}`);
}

const bytes = readFileSync(WASM_PATH);
const compiled = await WebAssembly.compile(bytes);

// A fresh module per trial - trials must not interact (a run already ended
// in one trial should not leak its "dead, waiting" state into the next).
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
    exp.emu_app_switch(APP_DINO);
    exp.emu_tick(10);
    if (exp.emu_app_current() !== APP_DINO) throw new Error("failed to switch into dino");
    fwLog.length = 0;
    return {
        feed(report: TouchReport, nowMs: number) {
            exp.emu_touch(report.fingers === 1 ? 1 : 0, report.x, report.y);
            exp.emu_tick(nowMs);
        },
        drainLog(): string[] { const out = fwLog.slice(); fwLog.length = 0; return out; },
    };
}

// Every scenario below is about a JUMP misfiring - dino.c starts in the
// idle "waiting for a tap" state, where the very first successful arm
// starts a RUN ("dino: run start"), not a jump (see dino.c's RS_DEAD_WAIT
// branch). Testing "does a touch profile fire an unwanted jump" therefore
// needs the device already alive and grounded first, with a genuine
// release behind it (TAP_RELEASE_GRACE_MS, dino.c) so the profile under
// test starts from a clean arming state - exactly the setup
// repro-touch-dropout-four-drop.ts does not need (Connect Four is "live"
// the instant it is entered) and this file does. Uses clean, direct
// touches (not TouchSim) deliberately: starting the run is setup, not the
// thing being measured.
async function startRunCleanly(dev: Awaited<ReturnType<typeof freshDevice>>, t: number): Promise<number> {
    for (let e = 0; e < 80; e += 15) { t += 15; dev.feed({ fingers: 1, x: TOUCH_X, y: TOUCH_Y }, t); }
    t += 15; dev.feed({ fingers: 0, x: 0, y: 0 }, t);
    const started = dev.drainLog();
    if (!started.some((l) => l.includes("dino: run start"))) {
        throw new Error(`startRunCleanly: expected "dino: run start", got: ${started.join(" | ") || "(nothing)"}`);
    }
    // Past TAP_RELEASE_GRACE_MS (300) of silence, so the profile under test
    // begins with tap_tick() fully reset (everContacted=false, armed=false).
    for (let e = 0; e < 400; e += 15) { t += 15; dev.feed({ fingers: 0, x: 0, y: 0 }, t); }
    dev.drainLog();
    return t;
}

const dropoutHeavy = { ...TOUCHSIM_DEFAULTS, dropoutsEnabled: true, dropoutsPerSec: 34, straysEnabled: false };
const strayProfile = { ...TOUCHSIM_DEFAULTS, dropoutsEnabled: false, straysEnabled: true };
const STEP_MS = 1000 / dropoutHeavy.reportRateHz;

async function main() {
    console.log("=== dino's tap-to-jump, under a dropout-heavy touch stream ===\n");
    console.log(`    profile: ${dropoutHeavy.dropoutsPerSec} dropout episodes/sec at ${dropoutHeavy.reportRateHz}Hz ` +
        `(p=${(dropoutHeavy.dropoutsPerSec / dropoutHeavy.reportRateHz).toFixed(2)} per report)\n`);

    // ---- A. A THUMB HELD STILL ON THE GLASS MUST JUMP EXACTLY ONCE, NOT
    // REPEATEDLY. This is the palette bug's own shape: a finger that is
    // DOWN but not moving, which is what a controller's dropouts make
    // indistinguishable from a rapid sequence of lift-and-retouch unless
    // the firmware bridges short gaps rather than believing every one of
    // them. --------------------------------------------------------------
    const HOLD_TRIALS = 25;
    const HOLD_MS = 3000;
    let exactlyOneJump = 0;
    let totalDropouts = 0;
    const jumpCounts: number[] = [];
    for (let i = 0; i < HOLD_TRIALS; i++) {
        const dev = await freshDevice();
        let t = await startRunCleanly(dev, 1000);
        const sim = new TouchSim(dropoutHeavy, 368, 448, seededRng(seedFromName(`dino-hold-${i}`)));
        sim.setPointer(true, TOUCH_X, TOUCH_Y);
        let jumps = 0;
        for (let held = 0; held < HOLD_MS; held += STEP_MS) {
            t += STEP_MS;
            dev.feed(sim.poll(t), t);
            jumps += dev.drainLog().filter((l) => l.includes("dino: jump")).length;
        }
        totalDropouts += sim.simDropouts;
        jumpCounts.push(jumps);
        if (jumps === 1) exactlyOneJump++;
    }
    const holdRate = (exactlyOneJump / HOLD_TRIALS) * 100;
    console.log(`    ${totalDropouts} simulated dropout episodes across ${HOLD_TRIALS} x ${HOLD_MS}ms of held contact`);
    console.log(`    jump counts observed: ${jumpCounts.join(",")}`);
    check("a thumb held on the glass for 3 seconds jumps exactly once, not on every dropout-then-redown",
        holdRate === 100, `${exactlyOneJump}/${HOLD_TRIALS} trials jumped exactly once (${holdRate.toFixed(0)}%)`);

    // ---- B. NO BURST OF FEWER THAN TAP_ARM_SAMPLES PHANTOM CONTACTS CAN
    // EVER JUMP, AT ANY SPACING - the hard, non-statistical guarantee
    // apps/four.c's own test makes for its arming logic (see that file's
    // ARMING comment on why "two samples inside the release grace" was
    // the wrong first answer). dino.c's TAP_ARM_SAMPLES/TAP_ARM_MS/
    // TAP_ARM_RATE_HZ are the same three numbers, so this is the same
    // proof applied to a jump trigger. --------------------------------
    console.log("");
    let burstJumps = 0;
    const burstCases: string[] = [];
    for (const spacingMs of [17, 33, 60, 120, 200, 400]) {
        for (const count of [1, 2, 3]) {
            const dev = await freshDevice();
            let t = await startRunCleanly(dev, 1000);
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
            if (dev.drainLog().some((l) => l.includes("dino: jump"))) {
                burstJumps++;
                burstCases.push(`${count} contact(s) ${spacingMs}ms apart`);
            }
        }
    }
    check("no burst of up to three phantom contacts, at any spacing, ever fires a jump",
        burstJumps === 0, burstJumps === 0 ? "18 burst shapes tried" : burstCases.join(", "));

    // ---- C. the emulator's own modelled stray process, run long, with
    // nothing touching the glass. The rate emulator/src/constants.ts
    // calibrates as what this controller actually does, so it is the rate
    // a claim about the shipped device can honestly be made at. A stray
    // jump here would mean the toy appears to play itself while sitting
    // untouched - exactly the failure four.c's own C2 scenario rules out
    // for a dropped piece, applied here to an unwanted jump.
    //
    // ALSO checked for "dino: run start", not just "dino: jump": left alone
    // for two minutes she is guaranteed to have died on the first obstacle
    // within a few seconds (nobody is jumping for her), which lands the
    // game back in the idle "waiting for a tap" state for nearly the whole
    // window - and from there, a stray-triggered arm reads as an unwanted
    // RESTART, not a jump. Checking only "dino: jump" would make this test
    // read as covering the full two minutes while genuinely covering only
    // the first few seconds; the toy playing itself is the same failure
    // whether the stray starts a run or fires a jump inside one.
    console.log("");
    const dev = await freshDevice();
    const sim = new TouchSim(strayProfile, 368, 448, seededRng(seedFromName("dino-strays")));
    sim.setPointer(false, 0, 0);
    let strayFires = 0;
    let t = 1000;
    for (let e = 0; e < 120000; e += STEP_MS) {
        t += STEP_MS;
        dev.feed(sim.poll(t), t);
        if (dev.drainLog().some((l) => l.includes("dino: jump") || l.includes("dino: run start"))) strayFires++;
    }
    console.log(`    ${sim.simStrays} stray contacts synthesised over 120s at the emulator's modelled rate ` +
        `(${strayProfile.straysPerSec}/s), with nothing touching the glass`);
    check("two minutes of the modelled stray process fires no jump and starts no run", strayFires === 0,
        `${strayFires} unwanted fire(s)`);

    console.log(`\n${passCount} passed, ${failCount} failed`);
    if (failCount > 0) process.exit(1);
}

main();
