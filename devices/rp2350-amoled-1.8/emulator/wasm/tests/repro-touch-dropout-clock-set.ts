// repro-touch-dropout-clock-set: setting the clock (firmware/apps/clock.c)
// with a touch stream that misbehaves the way the real controller does,
// rather than with a clean pointer.
//
// THIS FILE PINS THE NEW GESTURE, NOT THE OLD ONE. clock.c used to be set by
// holding BOOT and sliding a finger; this file used to pin that gesture's own
// dropout exposure (the field latch, and the commit not depending on the
// finger being down). The owner replaced the whole gesture 2026-08-18:
// double-press PWR to open, TAP one of four chevron zones to step a field,
// double-press PWR again to commit (see clock.c's own "THE GESTURE" header
// section for the full design and why). That is a different mechanism with a
// different dropout exposure, so this file was rewritten rather than
// deleted, per this project's standing rule that a feature driven by touch
// needs both a clean-input feature test AND a dropout-profile one
// (AGENTS.md's "Regression tests" section).
//
// WHAT IS ACTUALLY AT RISK HERE, stated before it is measured, and it is a
// GENUINELY DIFFERENT SHAPE OF RISK than the old gesture had:
//
//   1. THE OLD GESTURE was continuous and ABSOLUTE - "where the finger is
//      IS the number" - so a dropout changed nothing: a lost report just
//      meant the next good report re-asserted the same (or a very close)
//      position. Loss was harmless by construction.
//   2. THE NEW GESTURE is discrete and RELATIVE - a chevron tap steps a
//      field by exactly one, and it fires on setting_tick()'s own
//      f->touchPressed edge (runtime_core.c: down && !wasDown, resolved
//      fresh from whatever the touch queue drained THIS tick) with NO
//      arming delay and NO debounce of its own - unlike dino.c's jump or
//      four.c's drop, which both require several consistent samples
//      (TAP_ARM_SAMPLES et al) before accepting a tap as real. So the
//      question this file exists to answer is the mirror image of the old
//      one: does a controller that reports a lost-then-regained contact
//      make ONE physical press-and-release look like SEVERAL
//      touchPressed edges, over-stepping the field the owner only touched
//      once?
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
import { seededRng, seedFromName } from "../../../tools/gate/touch";

const WASM_PATH = join(import.meta.dir, "..", "dist", "emu.wasm");
const PANEL_W = 368;
const PANEL_H = 448;
const LAND_H = PANEL_W;
const LAND_W = PANEL_H;
// g_apps[] = { chrono, sketch("draw"), timer, four, clock, morpion, ... }
const APP_CLOCK = 4;
const BTN_PWR = 1;
const DOUBLE_PRESS_WINDOW_MS = 500; // clock.c's own constant, mirrored

let passCount = 0;
let failCount = 0;
function check(label: string, ok: boolean, detail?: string) {
    const status = ok ? "PASS" : "FAIL";
    if (ok) passCount++; else failCount++;
    console.log(`[${status}] ${label}${detail ? " - " + detail : ""}`);
}

const bytes = readFileSync(WASM_PATH);
const compiled = await WebAssembly.compile(bytes);

// A fresh module per trial - trials must not interact.
async function freshDevice() {
    let memory!: WebAssembly.Memory;
    const decoder = new TextDecoder();
    const fwLog: string[] = [];
    const instance = await WebAssembly.instantiate(compiled, {
        env: {
            js_log(ptr: number, len: number) { fwLog.push(decoder.decode(new Uint8Array(memory.buffer, ptr, len)).trimEnd()); },
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
    const dev = {
        // Raw button edge/verdict, for scenario B's interleaved sequence.
        button(down: boolean) { exp.emu_button(BTN_PWR, down ? 1 : 0); },
        buttonVerdict(isLong: boolean) { exp.emu_button_verdict(BTN_PWR, isLong ? 1 : 0); },
        // A clean PWR short-press-and-release, with the PMIC's own verdict -
        // the double-press channel is BUTTON events, never touch, so setup
        // through it is never the thing under test in this file.
        shortPress(t: number): number {
            dev.button(true); t += 20; exp.emu_tick(t);
            dev.button(false); dev.buttonVerdict(false); t += 20; exp.emu_tick(t);
            return t;
        },
        doublePress(t: number): number {
            let tt = dev.shortPress(t);
            tt += 100;
            tt = dev.shortPress(tt);
            return tt;
        },
        // The report is already in PANEL coordinates (TouchSim works in the
        // panel's own space, like the real controller).
        feed(report: TouchReport, nowMs: number) {
            exp.emu_touch(report.fingers === 1 ? 1 : 0, report.x, report.y);
            exp.emu_tick(nowMs);
        },
        tick(nowMs: number) { exp.emu_tick(nowMs); },
        drainLog(): string[] { const out = fwLog.slice(); fwLog.length = 0; return out; },
    };
    return dev;
}

type Device = Awaited<ReturnType<typeof freshDevice>>;

// The long-ways HOURS-down chevron zone, panel coordinates - clock.c's own
// chevron_zone(): lx (=touchY) < LAND_W/2 picks hours, ly (=PANEL_W-1-touchX)
// >= LAND_H/2 picks "down" (decrement). Centre of that quarter, well inside
// it, the same "aim for the middle of the zone, not its edge" convention
// every other test in this directory uses.
function hoursDownPoint(): [number, number] {
    const lx = LAND_W * 0.25;
    const ly = LAND_H * 0.75;
    return [PANEL_W - 1 - ly, lx];
}

const dropoutHeavy = { ...TOUCHSIM_DEFAULTS, dropoutsEnabled: true, dropoutsPerSec: 34, straysEnabled: false };
const STEP_MS = 1000 / dropoutHeavy.reportRateHz;

async function main() {
    console.log("=== setting the clock's new tap gesture under a dropout-heavy touch stream ===\n");
    console.log(`    profile: ${dropoutHeavy.dropoutsPerSec} dropout episodes/sec at ${dropoutHeavy.reportRateHz}Hz ` +
        `(p=${(dropoutHeavy.dropoutsPerSec / dropoutHeavy.reportRateHz).toFixed(2)} per report)\n`);

    // ---- A. ONE PHYSICAL TAP-AND-RELEASE ON A CHEVRON ZONE MUST STEP THE
    // FIELD EXACTLY ONCE, NOT REPEATEDLY. Not a multi-second hold - holding
    // is not how this gesture is used at all, it is TAPPED, repeatedly, to
    // step through a range (feature-clock.ts's own hours/minutes sections
    // tap dozens of times to dial in a value). The real hazard is narrower
    // and sharper: a single, ordinary human tap (a couple hundred ms of
    // contact) landing during a dropout run, so the controller reports the
    // finger present, then absent, then present again, all within ONE
    // physical touch - and clock.c's chevron tap had no arming delay of its
    // own to absorb that (unlike dino.c's jump or four.c's drop). Measured
    // via the FINAL COMMITTED HOUR (seeded at 12, one decrement should land
    // on 11), not a per-tap log line - clock.c deliberately does not log
    // every tap (a line per tap the owner is deliberately dialling in
    // quickly would drown the log the same way a line per frame would), so
    // the committed value is the only honest place to read this back from,
    // exactly the way feature-clock.ts already reads every other outcome.
    const TAP_TRIALS = 40;
    const TAP_HOLD_MS = 220; // an ordinary human tap's own contact time
    let exactlyOne = 0;
    let totalDropouts = 0;
    const results: number[] = [];
    const [hx, hy] = hoursDownPoint();

    for (let i = 0; i < TAP_TRIALS; i++) {
        const dev = await freshDevice();
        let t = 1000;
        t = dev.doublePress(t); // open, seeded 12:00
        dev.drainLog();

        const sim = new TouchSim(dropoutHeavy, PANEL_W, PANEL_H, seededRng(seedFromName(`clock-tap-${i}`)));
        sim.setPointer(true, hx, hy);
        for (let held = 0; held < TAP_HOLD_MS; held += STEP_MS) {
            t += STEP_MS;
            dev.feed(sim.poll(t), t);
        }
        sim.setPointer(false, hx, hy);
        for (let lifted = 0; lifted < 200; lifted += STEP_MS) { t += STEP_MS; dev.feed(sim.poll(t), t); }
        totalDropouts += sim.simDropouts;

        t += 400; dev.tick(t); // clear TAP_COOLDOWN_MS before the commit double-press
        t = dev.doublePress(t); // commit
        const setLine = dev.drainLog().findLast((l) => l.includes("clock: set to"));
        const m = setLine?.match(/set to (\d+):/);
        const committedHour = m ? Number(m[1]) : -1;
        results.push(committedHour);
        if (committedHour === 11) exactlyOne++;
    }
    const rate = (exactlyOne / TAP_TRIALS) * 100;
    console.log(`    ${totalDropouts} dropout episodes simulated across ${TAP_TRIALS} x ${TAP_HOLD_MS}ms taps`);
    console.log(`    committed hours observed: ${results.join(",")}`);
    check("one physical tap-and-release on a chevron zone steps the hour exactly once (12 -> 11), even under a dropping controller",
        rate === 100, `${exactlyOne}/${TAP_TRIALS} trials stepped exactly once (${rate.toFixed(0)}%)`);

    // ---- A2. THE FIX DOES NOT COST LEGITIMATE FAST TAPPING. A child
    // stepping through a range one tap at a time must still get one step
    // per tap once each tap clears TAP_COOLDOWN_MS of the last - clean
    // input, no dropout, three quick taps 300ms apart (just past the
    // 250ms cooldown) should land on exactly 12-3=9. -----------------------
    {
        const dev = await freshDevice();
        let t = 1000;
        t = dev.doublePress(t);
        for (let i = 0; i < 3; i++) {
            dev.feed({ fingers: 1, x: hx, y: hy }, (t += 20));
            dev.feed({ fingers: 0, x: 0, y: 0 }, (t += 20));
            t += 300;
            dev.tick(t);
        }
        t = dev.doublePress(t);
        const setLine = dev.drainLog().findLast((l) => l.includes("clock: set to"));
        check("three clean, deliberately-spaced taps still register as three separate steps (12 -> 9)",
            setLine === "clock: set to 09:00", setLine ?? "(no set line)");
    }

    // ---- B. the double-press channel (PWR, a button) is immune to a touch
    // stream misbehaving AT THE SAME TIME - the two channels are read by
    // completely different code (sensors_key_take() vs sensors_touch_next(),
    // sensors.h), so a finger dropping in and out while PWR is pressed twice
    // must not stop set mode from opening or prevent the commit. ------------
    console.log("");
    let combinedOk = 0;
    const COMBINED_TRIALS = 15;
    for (let i = 0; i < COMBINED_TRIALS; i++) {
        const dev = await freshDevice();
        let t = 1000;
        const sim = new TouchSim(dropoutHeavy, PANEL_W, PANEL_H, seededRng(seedFromName(`clock-combined-${i}`)));
        sim.setPointer(true, hx, hy); // a finger resting/wandering on the glass throughout
        // Interleave the double-press with the dropout-heavy touch feed.
        const pressAt = (target: number) => {
            while (t < target) { t += STEP_MS; dev.feed(sim.poll(t), t); }
        };
        dev.drainLog();
        const tOpen = t;
        // Two short presses, 100ms apart (well inside DOUBLE_PRESS_WINDOW_MS),
        // interleaved tick-by-tick with the dropping touch feed above.
        dev.button(true); pressAt(tOpen + 20);
        dev.button(false); dev.buttonVerdict(false); pressAt(tOpen + 40);
        pressAt(tOpen + 140);
        dev.button(true); pressAt(tOpen + 160);
        dev.button(false); dev.buttonVerdict(false); pressAt(tOpen + 180);
        pressAt(tOpen + 400);
        const opened = dev.drainLog().some((l) => l.includes("clock: set mode opened"));
        if (opened) combinedOk++;
    }
    check("set mode opens on the double-press even with a dropping finger resting on the glass throughout",
        combinedOk === COMBINED_TRIALS, `${combinedOk}/${COMBINED_TRIALS}`);

    console.log(`\n${passCount} passed, ${failCount} failed`);
    if (failCount > 0) process.exit(1);
}

main();
