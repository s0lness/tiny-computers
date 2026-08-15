// repro-touch-dropout-four-choice: Connect Four's choice screen
// (firmware/apps/four.c, section 8) driven by a touch stream that
// misbehaves the way the real controller does, rather than by a clean
// pointer - the same split feature-four-choice.ts's own header names:
// clean input there is a readable statement of what the screen is supposed
// to do, this file is what proves it survives this panel's actual weather.
//
// TWO HAZARDS, not one, because the choice screen inherits both of the
// ones repro-touch-dropout-four-drop.ts already covers for the board, PLUS
// a third that is specific to having two adjacent targets instead of seven
// equal columns:
//
//   - DROPOUTS (34 episodes/sec, repro-touch-dropout-stroke-start.ts's own
//     back-calculation): a false release must not commit a choice nobody
//     lifted their thumb to make, and must not fail to commit one they did.
//   - STRAYS: a phantom contact with nobody touching the glass must never
//     commit anything.
//   - POSITION JITTER: the reported position of a STILL finger wanders
//     80-250px for up to three reports (measured on the palette,
//     repro-touch-dropout-palette-open.ts's own TOUCHSIM_JITTER_PROFILE,
//     copied verbatim here rather than re-derived - AGENTS.md's own
//     instruction for any new app that hit-tests position). A jitter
//     excursion large enough to cross this screen's own 224px boundary
//     would, without CHOOSE_CONFIRM_MS, flicker the lit side and could
//     commit whichever side the last unlucky sample happened to land on.
//
// Run with (after `bun run emulator/wasm/build.ts`):
//
//   bun run emulator/wasm/tests/repro-touch-dropout-four-choice.ts
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { TouchSim, type TouchReport } from "../../src/touchsim";
import { TOUCHSIM_DEFAULTS } from "../../src/constants";
import { seededRng, seedFromName } from "../../../tools/gate/touch";

const WASM_PATH = join(import.meta.dir, "..", "dist", "emu.wasm");
const PANEL_W = 368;
const PANEL_H = 448;
const LAND_W = PANEL_H; // 448
const APP_FOUR = 3; // g_apps[] = { chrono, sketch("draw"), timer, four }
const RELEASE_GRACE_MS = 300; // four.c's own
const CX_HUMAN = LAND_W * 0.25;
const CX_CPU = LAND_W * 0.75;
const THUMB_LY = 200;

let passCount = 0;
let failCount = 0;
function check(label: string, ok: boolean, detail?: string) {
    const status = ok ? "PASS" : "FAIL";
    if (ok) passCount++; else failCount++;
    console.log(`[${status}] ${label}${detail ? " - " + detail : ""}`);
}

const bytes = readFileSync(WASM_PATH);
const compiled = await WebAssembly.compile(bytes);

// A FRESH module per trial - the choice, once made, deals a board and the
// next trial's own gesture must not inherit that state.
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
    exp.emu_app_switch(APP_FOUR);
    exp.emu_tick(10);
    if (exp.emu_app_current() !== APP_FOUR) throw new Error("failed to switch into four");
    fwLog.length = 0;
    return {
        // The report is in PANEL coordinates already (TouchSim works in the
        // panel's own space, like the real controller).
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
    console.log("=== Connect Four's choice screen, under a dropout/jitter-heavy touch stream ===\n");
    console.log(`    profile: ${dropoutHeavy.dropoutsPerSec} dropout episodes/sec at ${dropoutHeavy.reportRateHz}Hz, RELEASE_GRACE_MS=${RELEASE_GRACE_MS}\n`);

    // ---- scenario A: held still on "vs human" must not commit early -----
    // The verb is RELEASE, exactly as on the board (four.c section 2); a
    // dropout storm is a stream of fake releases, and any one of them
    // committing early would deal a game nobody chose yet.
    const HOLD_TRIALS = 25;
    const HOLD_MS = 3000;
    let heldWithoutCommitting = 0;
    for (let i = 0; i < HOLD_TRIALS; i++) {
        const dev = await freshDevice();
        const sim = new TouchSim(dropoutHeavy, PANEL_W, PANEL_H, seededRng(seedFromName(`four-choice-hold-${i}`)));
        const [px, py] = landToPanel(CX_HUMAN, THUMB_LY);
        sim.setPointer(true, px, py);
        let t = 1000;
        let committed = false;
        for (let held = 0; held < HOLD_MS; held += STEP_MS) {
            t += STEP_MS;
            dev.feed(sim.poll(t), t);
            if (dev.drainLog().some((l) => l.includes("four: choice"))) { committed = true; break; }
        }
        if (!committed) heldWithoutCommitting++;
    }
    const holdRate = (heldWithoutCommitting / HOLD_TRIALS) * 100;
    check("a thumb held on \"vs human\" for 3 seconds does not commit a choice by itself",
        holdRate >= 96, `${heldWithoutCommitting}/${HOLD_TRIALS} trials survived the hold (${holdRate.toFixed(0)}%)`);

    // ---- scenario B: press on one side, slide to the other, release -----
    // End to end, at the rate a child's thumb actually moves: exactly one
    // choice committed, and it is the side the thumb finished on - a false
    // early commit mid-slide would deal the WRONG game, which she cannot
    // read her way out of any faster than the right one.
    console.log("");
    const SLIDE_TRIALS = 25;
    const SLIDE_MS = 500;
    const SETTLE_MS = 320; // long enough to clear CHOOSE_CONFIRM_MS (150ms)
                             // on the side the thumb stops on
    let correct = 0, committedEarly = 0, missed = 0, wrongSide = 0;
    for (let i = 0; i < SLIDE_TRIALS; i++) {
        const dev = await freshDevice();
        const sim = new TouchSim(dropoutHeavy, PANEL_W, PANEL_H, seededRng(seedFromName(`four-choice-slide-${i}`)));
        const toCpu = i % 2 === 0;
        const from = toCpu ? CX_HUMAN : CX_CPU;
        const to = toCpu ? CX_CPU : CX_HUMAN;
        let t = 1000;

        for (let e = 0; e < SLIDE_MS; e += STEP_MS) {
            const u = e / SLIDE_MS;
            const [px, py] = landToPanel(from + (to - from) * u, THUMB_LY);
            sim.setPointer(true, px, py);
            t += STEP_MS;
            dev.feed(sim.poll(t), t);
        }
        const [tx, ty] = landToPanel(to, THUMB_LY);
        sim.setPointer(true, tx, ty);
        for (let e = 0; e < SETTLE_MS; e += STEP_MS) { t += STEP_MS; dev.feed(sim.poll(t), t); }

        const early = dev.drainLog().some((l) => l.includes("four: choice"));
        if (early) committedEarly++;

        sim.setPointer(false, 0, 0);
        const choices: string[] = [];
        for (let e = 0; e < RELEASE_GRACE_MS + 400; e += STEP_MS) {
            t += STEP_MS;
            dev.feed(sim.poll(t), t);
            for (const l of dev.drainLog()) if (l.includes("four: choice")) choices.push(l);
        }

        if (early) continue;
        if (choices.length === 0) { missed++; continue; }
        const wantVsCpu = toCpu ? 1 : 0;
        if (choices.length === 1 && choices[0]!.includes(`vsCpu=${wantVsCpu}`)) correct++;
        else wrongSide++;
    }
    const rate = (correct / SLIDE_TRIALS) * 100;
    console.log(`    ${correct}/${SLIDE_TRIALS} slides committed exactly the side the thumb finished on (${rate.toFixed(0)}%); ` +
        `${committedEarly} committed early, ${missed} never committed, ${wrongSide} committed the other side`);
    check("a slide finished by a genuine lift commits the side the thumb finished on",
        rate >= 90, `${rate.toFixed(0)}% over ${SLIDE_TRIALS} trials`);
    check("no trial committed a choice while the thumb was still sliding", committedEarly === 0, `${committedEarly} early commit(s)`);

    // ---- scenario C: strays must never commit a choice -------------------
    // Same shape as repro-touch-dropout-four-drop.ts's own C1: fewer than
    // ARM_SAMPLES phantom contacts can never arm a gesture, at any spacing
    // - a hard property (four.c's own ARMING comment), so tried at every
    // spacing rather than left to a random process to maybe hit.
    console.log("");
    let burstCommits = 0;
    const burstCases: string[] = [];
    for (const spacingMs of [17, 33, 60, 120, 200, 400]) {
        for (const count of [1, 2, 3]) {
            const devC = await freshDevice();
            let tC = 1000;
            const [sx, sy] = landToPanel(CX_HUMAN, THUMB_LY);
            for (let k = 0; k < count; k++) {
                devC.feed({ fingers: 1, x: sx, y: sy }, tC);
                for (let e = STEP_MS; e < spacingMs; e += STEP_MS) { tC += STEP_MS; devC.feed({ fingers: 0, x: 0, y: 0 }, tC); }
                tC += STEP_MS;
            }
            for (let e = 0; e < RELEASE_GRACE_MS + 600; e += STEP_MS) { tC += STEP_MS; devC.feed({ fingers: 0, x: 0, y: 0 }, tC); }
            if (devC.drainLog().some((l) => l.includes("four: choice"))) {
                burstCommits++;
                burstCases.push(`${count} contact(s) ${spacingMs}ms apart`);
            }
        }
    }
    check("no burst of up to three phantom contacts, at any spacing, ever commits a choice",
        burstCommits === 0, burstCommits === 0 ? "18 burst shapes tried" : burstCases.join(", "));

    const strayProfile = { ...TOUCHSIM_DEFAULTS, dropoutsEnabled: false, straysEnabled: true };
    const devC2 = await freshDevice();
    const simC2 = new TouchSim(strayProfile, PANEL_W, PANEL_H, seededRng(seedFromName("four-choice-strays")));
    simC2.setPointer(false, 0, 0);
    let strayCommits = 0;
    let tC2 = 1000;
    for (let e = 0; e < 120000; e += STEP_MS) {
        tC2 += STEP_MS;
        devC2.feed(simC2.poll(tC2), tC2);
        if (devC2.drainLog().some((l) => l.includes("four: choice"))) strayCommits++;
    }
    console.log(`    ${simC2.simStrays} stray contacts synthesised over 120s at the emulator's modelled rate ` +
        `(${strayProfile.straysPerSec}/s), with nothing touching the glass`);
    check("two minutes of the modelled stray process commits nothing", strayCommits === 0,
        `${strayCommits} choice(s) committed by strays`);

    // ---- scenario D: position jitter near the two targets' shared edge --
    // TOUCHSIM_JITTER_PROFILE, copied from repro-touch-dropout-palette-
    // open.ts rather than re-derived (this device's own convention for any
    // app that hit-tests position, per AGENTS.md): the reported position of
    // a STILL finger wanders 80-250px for up to three reports. A thumb
    // anchored well inside "vs human" (100px from the shared edge at
    // lx=224, so a single 80-250px jitter CAN reach past it) must still
    // never end up confirming "vs cpu" - CHOOSE_CONFIRM_MS is the only
    // thing standing between a jitter excursion and the wrong game being
    // dealt, the same role it plays for the menu's own grid (decision 0013).
    console.log("");
    const jitterProfile = {
        ...TOUCHSIM_DEFAULTS,
        dropoutsEnabled: false, straysEnabled: false,
        positionJitterEnabled: true, positionJitterPerSec: 1.5,
        positionJitterMinPx: 80, positionJitterMaxPx: 250, positionJitterMaxHoldReports: 3,
    };
    const JITTER_TRIALS = 20;
    const JITTER_HOLD_MS = 2000;
    let jitterEpisodes = 0;
    let wrongSideCommits = 0;
    let jitterCorrect = 0;
    for (let i = 0; i < JITTER_TRIALS; i++) {
        const dev = await freshDevice();
        const sim = new TouchSim(jitterProfile, PANEL_W, PANEL_H, seededRng(seedFromName(`four-choice-jitter-${i}`)));
        const [px, py] = landToPanel(CX_HUMAN, THUMB_LY); // anchored on "vs human"
        sim.setPointer(true, px, py);
        let t = 1000;
        for (let held = 0; held < JITTER_HOLD_MS; held += STEP_MS) { t += STEP_MS; dev.feed(sim.poll(t), t); }
        sim.setPointer(false, 0, 0);
        let choice: string | undefined;
        for (let e = 0; e < RELEASE_GRACE_MS + 400; e += STEP_MS) {
            t += STEP_MS;
            dev.feed(sim.poll(t), t);
            for (const l of dev.drainLog()) if (l.includes("four: choice")) choice = l;
        }
        jitterEpisodes += sim.simJitterEpisodes ?? 0;
        if (choice?.includes("vsCpu=1")) wrongSideCommits++;
        else if (choice?.includes("vsCpu=0")) jitterCorrect++;
    }
    console.log(`    ${jitterEpisodes} simulated jitter episodes across ${JITTER_TRIALS} x ${JITTER_HOLD_MS}ms of held contact ` +
        `anchored on "vs human", ${jitterCorrect}/${JITTER_TRIALS} confirmed "vs human", ${wrongSideCommits} confirmed "vs cpu"`);
    check("jitter episodes were actually produced by this profile (a dormant filter proves nothing - decision 0010/0013's own standard)",
        jitterEpisodes > 0, `${jitterEpisodes} episodes`);
    check("a thumb anchored on \"vs human\" never has the jitter profile flip its choice to \"vs cpu\"",
        wrongSideCommits === 0, `${wrongSideCommits}/${JITTER_TRIALS} wrong-side commits`);

    console.log(`\n${passCount} passed, ${failCount} failed`);
    if (failCount > 0) process.exit(1);
}

main();
