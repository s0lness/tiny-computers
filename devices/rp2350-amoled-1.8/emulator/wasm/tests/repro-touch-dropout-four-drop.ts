// repro-touch-dropout-four-drop: the Connect Four gesture
// (firmware/apps/four.c) driven by a touch stream that misbehaves the way
// the real controller does, rather than by a clean pointer.
//
// WHY THIS FILE EXISTS SEPARATELY FROM feature-four.ts. This project has
// already shipped one feature that worked in the emulator and did not work
// in the owner's hands, on this exact hazard: the sketchpad's colour
// palette, whose long press was reset dozens of times a second by a
// controller reporting a lost contact that never happened
// (repro-touch-dropout-palette-open.ts, and sketch.c's lastContactMs). The
// palette's own feature test drove clean input and passed all 22 of its
// checks throughout.
//
// Connect Four is exposed to the SAME hazard and its consequence is worse.
// The gesture's verb is RELEASE, and a false release does not blur a stroke
// here: it drops a piece, possibly in a column the thumb was still sliding
// past, and ends that player's turn. Nothing in the rules of Connect Four
// can undo that, and with two people sharing one puck it also hands the puck
// over, so the wrong player is now up as well. The release verdict is
// therefore the thing under test.
//
// THE PROFILE. Identical to repro-touch-dropout-stroke-start.ts's and
// repro-touch-dropout-palette-open.ts's dropoutHeavy: 34 dropout episodes
// per second, back-calculated in the first of those files from a real
// hardware session (798 dropouts over ~23s of finger-down time). At the
// controller's 60Hz report rate that is a per-report loss probability of
// 0.57, so contact is ABSENT on most individual reports and the firmware
// has to bridge continuously.
//
// Run with (after `bun run emulator/wasm/build.ts`):
//
//   bun run emulator/wasm/tests/repro-touch-dropout-four-drop.ts
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { TouchSim, type TouchReport } from "../../src/touchsim";
import { TOUCHSIM_DEFAULTS } from "../../src/constants";
import { seededRng, seedFromName } from "../../../tools/gate/touch";

const WASM_PATH = join(import.meta.dir, "..", "dist", "emu.wasm");
const PANEL_W = 368;
const PANEL_H = 448;
const APP_FOUR = 3; // g_apps[] = { chrono, sketch("draw"), timer, four }

// Mirrors of four.c's own constants.
const COLS = 7;
// The column partition, derived exactly the way four.c derives it: square
// cells, centred in the VISIBLE canvas (gfx.h PANEL_BEZEL_MARGIN_PX - the
// case hides a band along every edge).
//
// This drifted once and the drift is worth recording, because it failed in
// the most misleading way available: when four.c went from a full-width
// 61px pitch to a square 48px one, this file kept the old derivation, so
// every touch it aimed at "column c" landed in a different column and the
// test reported 76% of slides "landing elsewhere" - which reads exactly
// like a gesture bug in the firmware and was a stale constant in the test.
// Deriving it the same way the app does, rather than copying the numbers
// out, is what stops that: a change to CELL or to the bezel moves both.
const BEZEL = 10;
const LAND_W = PANEL_H;
const SAFE_X0 = BEZEL;
const SAFE_W = LAND_W - 2 * BEZEL;
const CELL = 48;
const BOARD_X0 = SAFE_X0 + Math.floor((SAFE_W - COLS * CELL) / 2);
const RELEASE_GRACE_MS = 300;  // four.c's own, see its header section 2
const HANDOFF_MS = 420;        // four.c's own
const colX = (c: number) => BOARD_X0 + Math.floor(CELL / 2) + c * CELL;

let passCount = 0;
let failCount = 0;
function check(label: string, ok: boolean, detail?: string) {
    const status = ok ? "PASS" : "FAIL";
    if (ok) passCount++; else failCount++;
    console.log(`[${status}] ${label}${detail ? " - " + detail : ""}`);
}

const bytes = readFileSync(WASM_PATH);
const compiled = await WebAssembly.compile(bytes);

// A FRESH module per trial. Trials must not interact: a piece dropped in
// trial 3 changes which row trial 4's piece lands on, a game that ends
// enters a celebration that swallows the next gesture, and a board that
// fills up makes a column refuse a piece entirely - none of which is what
// is being measured here.
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

    // The app now opens on a choice screen (vs human / vs cpu - four.c's
    // header, section 8). This file drives the two-player gesture, so every
    // fresh device is walked through picking "vs human" here, with CLEAN
    // input (not through TouchSim), before any trial's own touch stream
    // begins - the choice screen gets its own dedicated dropout coverage in
    // repro-touch-dropout-four-choice.ts, so this file does not need to
    // retest it under duress.
    let setupT = 10;
    const setupPress = (lx: number, ly: number, down: boolean) => {
        setupT += 15;
        exp.emu_touch(down ? 1 : 0, PANEL_W - 1 - Math.round(ly), Math.round(lx));
        exp.emu_tick(setupT);
    };
    for (let e = 0; e < 350; e += 15) setupPress(LAND_W * 0.25, 200, true);
    for (let e = 0; e < RELEASE_GRACE_MS + 350; e += 15) setupPress(0, 0, false);
    for (let e = 0; e < HANDOFF_MS + 200; e += 15) setupPress(0, 0, false);
    fwLog.length = 0;
    return {
        startMs: setupT,
        // The report is in PANEL coordinates already (TouchSim works in the
        // panel's own space, like the real controller): the LANDSCAPE point a
        // caller wants is converted on the way in, at setPointer.
        feed(report: TouchReport, nowMs: number) {
            exp.emu_touch(report.fingers === 1 ? 1 : 0, report.x, report.y);
            exp.emu_tick(nowMs);
        },
        drainLog(): string[] { const out = fwLog.slice(); fwLog.length = 0; return out; },
    };
}

// gfx.h's mapping is landscape (lx, ly) -> panel (PANEL_W-1-ly, lx), so a
// landscape point a test wants to touch becomes this panel point before it
// ever reaches TouchSim - the same conversion feature-four.ts's touchLand()
// does, kept here rather than shared for the same reason every test in this
// directory is self-contained.
function landToPanel(lx: number, ly: number): [number, number] {
    return [PANEL_W - 1 - Math.round(ly), Math.round(lx)];
}

const dropoutHeavy = { ...TOUCHSIM_DEFAULTS, dropoutsEnabled: true, dropoutsPerSec: 34, straysEnabled: false };
const STEP_MS = 1000 / dropoutHeavy.reportRateHz;
const THUMB_LY = 200;

async function main() {
    console.log("=== Connect Four's press-slide-release, under a dropout-heavy touch stream ===\n");
    console.log(`    profile: ${dropoutHeavy.dropoutsPerSec} dropout episodes/sec at ${dropoutHeavy.reportRateHz}Hz ` +
        `(p=${(dropoutHeavy.dropoutsPerSec / dropoutHeavy.reportRateHz).toFixed(2)} per report), ` +
        `RELEASE_GRACE_MS=${RELEASE_GRACE_MS}\n`);

    // ---- scenario A: A THUMB HELD STILL MUST NOT DROP THE PIECE. ---------
    // This is the palette bug's own shape - a finger that is DOWN but not
    // MOVING, which is what a controller's dropouts make indistinguishable
    // from a lift unless the firmware keeps a separate contact clock. Here
    // the consequence of getting it wrong is not a split stroke, it is a
    // piece dropped in whatever column the thumb happened to be resting on
    // while its owner was still thinking.
    const HOLD_TRIALS = 25;
    const HOLD_MS = 3000;
    let heldWithoutDropping = 0;
    let totalDropouts = 0;
    for (let i = 0; i < HOLD_TRIALS; i++) {
        const dev = await freshDevice();
        const sim = new TouchSim(dropoutHeavy, PANEL_W, PANEL_H, seededRng(seedFromName(`four-hold-${i}`)));
        const col = i % COLS;
        const [pxx, pyy] = landToPanel(colX(col), THUMB_LY);
        sim.setPointer(true, pxx, pyy);
        let t = dev.startMs;
        let dropped = false;
        for (let held = 0; held < HOLD_MS; held += STEP_MS) {
            t += STEP_MS;
            dev.feed(sim.poll(t), t);
            if (dev.drainLog().some((l) => l.includes("four: drop"))) { dropped = true; break; }
        }
        totalDropouts += sim.simDropouts;
        if (!dropped) heldWithoutDropping++;
    }
    const holdRate = (heldWithoutDropping / HOLD_TRIALS) * 100;
    console.log(`    ${totalDropouts} simulated dropout episodes across ${HOLD_TRIALS} x ${HOLD_MS}ms of held contact`);
    // A RATE, with the threshold taken from four.c's own arithmetic table,
    // not "all of them". This scenario is a Bernoulli process: three seconds
    // of contact contains about 44 dropout gaps and each one outlasts
    // RELEASE_GRACE_MS with probability ~3e-5, so about 0.14% of trials are
    // expected to lose one however correct the firmware is. Demanding 100%
    // was what this asked for at first, and it duly failed 24/25 on a run
    // that had nothing wrong with it - which is worse than a weaker gate,
    // because a test that cries wolf gets re-run until it agrees.
    //
    // 96% still separates the two cases by a mile: a working release verdict
    // sits at ~99.9%, and one that believes the runtime's own touchReleased
    // sits at ~0% (every trial loses its piece within the first few hundred
    // milliseconds). Any value in between is the interesting news this test
    // exists to deliver.
    check("a thumb held on the glass for 3 seconds does not drop a piece by itself",
        holdRate >= 96, `${heldWithoutDropping}/${HOLD_TRIALS} trials survived the hold (${holdRate.toFixed(0)}%)`);

    // ---- scenario B: press, slide across the board, release. ------------
    // The whole gesture, end to end, at the rate a child's thumb actually
    // moves. What is asserted is not just "a piece fell" but "exactly one
    // piece fell, in the column the thumb finished on, and none fell while
    // the thumb was still sliding" - a false release mid-slide is the failure
    // this app cannot afford, and it would pass a looser check that only
    // looked at the end state.
    console.log("");
    const SLIDE_TRIALS = 25;
    const SLIDE_MS = 900;
    const SETTLE_MS = 320;   // the thumb stops on its column before letting go
    let correct = 0, droppedEarly = 0, missed = 0, wrongColumn = 0;
    for (let i = 0; i < SLIDE_TRIALS; i++) {
        const dev = await freshDevice();
        const sim = new TouchSim(dropoutHeavy, PANEL_W, PANEL_H, seededRng(seedFromName(`four-slide-${i}`)));
        const from = i % COLS;
        const to = (i * 3 + 4) % COLS;
        let t = dev.startMs;

        // press, slide, then hold still on the target
        for (let e = 0; e < SLIDE_MS; e += STEP_MS) {
            const u = e / SLIDE_MS;
            const [pxx, pyy] = landToPanel(colX(from) + (colX(to) - colX(from)) * u, THUMB_LY);
            sim.setPointer(true, pxx, pyy);
            t += STEP_MS;
            dev.feed(sim.poll(t), t);
        }
        const [tx, ty] = landToPanel(colX(to), THUMB_LY);
        sim.setPointer(true, tx, ty);
        for (let e = 0; e < SETTLE_MS; e += STEP_MS) {
            t += STEP_MS;
            dev.feed(sim.poll(t), t);
        }

        const beforeRelease = dev.drainLog();
        const early = beforeRelease.some((l) => l.includes("four: drop"));
        if (early) droppedEarly++;

        // the genuine lift
        sim.setPointer(false, 0, 0);
        const drops: string[] = [];
        for (let e = 0; e < RELEASE_GRACE_MS + 400; e += STEP_MS) {
            t += STEP_MS;
            dev.feed(sim.poll(t), t);
            for (const l of dev.drainLog()) if (l.includes("four: drop")) drops.push(l);
        }

        if (early) continue;
        if (drops.length === 0) { missed++; continue; }
        if (drops.length === 1 && drops[0]!.includes(`col=${to} `)) correct++;
        else wrongColumn++;
    }
    const rate = (correct / SLIDE_TRIALS) * 100;
    console.log(`    ${correct}/${SLIDE_TRIALS} slides dropped exactly one piece in the right column (${rate.toFixed(0)}%); ` +
        `${droppedEarly} dropped early, ${missed} never dropped, ${wrongColumn} landed elsewhere`);
    check("a slide finished by a genuine lift drops exactly one piece, in the column the thumb finished on",
        rate >= 90, `${rate.toFixed(0)}% over ${SLIDE_TRIALS} trials`);
    // FOUND AND FIXED 2026-08-17, the exact lesson scenario A's own comment
    // already tells: "demanding 100% ... duly failed on a run that had
    // nothing wrong with it." This check asked for it anyway - the one
    // remaining `=== 0` in a file whose every other check is a rate, for
    // the SAME RELEASE_GRACE_MS Bernoulli process four.c's own header calls
    // impossible to make zero. A slide-then-settle trial holds contact for
    // ~1220ms (SLIDE_MS+SETTLE_MS), about 40% of scenario A's 3000ms hold,
    // so by that same table an early drop is rarer per trial than a missed
    // hold but not absent: measured directly (150 independently seeded runs
    // of this exact scenario, same rng this file now uses), 146 saw zero
    // early drops, 4 saw exactly one, none saw two or more. <=1 covers every
    // seed measured, including the two real unseeded flakes that started
    // this investigation (both exactly one), while a state machine that
    // actually released mid-slide as a matter of course would blow past it
    // immediately (that failure mode is every trial, not one in dozens).
    check("no trial dropped a piece while the thumb was still sliding", droppedEarly <= 1, `${droppedEarly} early drop(s)`);

    // ---- scenario D: DRUMMING ON THE GLASS WHILE THE PUCK CHANGES HANDS
    // must not place a second piece.
    //
    // This one is here because of the two-player change. With a machine
    // playing the other side there was always a move in between, and the
    // hand that had just dropped a piece had something to watch. Two people
    // passing one puck do not: the player who just released is still holding
    // it, their thumb is still on it, and the next press is the OTHER
    // player's move. four.c answers that with the hand-off (a beat in which
    // no gesture may arm - see its section 6), and this is that beat under a
    // touch stream that is dropping contact the whole time, since a dropout
    // storm is precisely a stream of fake releases and a fake release is
    // what would place the piece.
    //
    // The window drummed on is deliberately shorter than the fall plus the
    // hand-off (~350ms of fall, before bounces, plus HANDOFF_MS) so that
    // every press AND every release inside it lands while the app is not
    // accepting gestures at all. What it must not do is wedge: the last
    // check makes a real gesture afterwards and requires it to work.
    console.log("");
    const devD = await freshDevice();
    const simD = new TouchSim(dropoutHeavy, PANEL_W, PANEL_H, seededRng(seedFromName("four-drum")));
    let tD = devD.startMs;
    const [dx, dy] = landToPanel(colX(2), THUMB_LY);
    simD.setPointer(true, dx, dy);
    for (let e = 0; e < 500; e += STEP_MS) { tD += STEP_MS; devD.feed(simD.poll(tD), tD); }
    simD.setPointer(false, 0, 0);
    let firstDrop: string | undefined;
    for (let e = 0; e < RELEASE_GRACE_MS + 200; e += STEP_MS) {
        tD += STEP_MS;
        devD.feed(simD.poll(tD), tD);
        for (const l of devD.drainLog()) if (l.includes("four: drop")) firstDrop = l;
    }
    check("the first player's piece went in", !!firstDrop && firstDrop.includes("player=1"), firstDrop ?? "(no drop)");

    // Drum: 8 presses of ~40ms with ~45ms gaps, all inside the fall and the
    // hand-off that follows it.
    const drumDrops: string[] = [];
    for (let beat = 0; beat < 8; beat++) {
        simD.setPointer(true, dx, dy);
        for (let e = 0; e < 40; e += STEP_MS) { tD += STEP_MS; devD.feed(simD.poll(tD), tD); }
        simD.setPointer(false, 0, 0);
        for (let e = 0; e < 45; e += STEP_MS) { tD += STEP_MS; devD.feed(simD.poll(tD), tD); }
        for (const l of devD.drainLog()) if (l.includes("four: drop")) drumDrops.push(l);
    }
    check("drumming on the glass while the piece falls and the puck changes hands places nothing",
        drumDrops.length === 0, drumDrops.length ? drumDrops.map((l) => l.trim()).join(" | ") : "0 extra pieces");

    // ...and the app is not wedged by it: a real gesture afterwards plays,
    // for the OTHER player.
    for (let e = 0; e < 1200; e += STEP_MS) { tD += STEP_MS; devD.feed(simD.poll(tD), tD); }
    devD.drainLog();
    simD.setPointer(true, ...landToPanel(colX(6), THUMB_LY) as [number, number]);
    for (let e = 0; e < 400; e += STEP_MS) { tD += STEP_MS; devD.feed(simD.poll(tD), tD); }
    simD.setPointer(false, 0, 0);
    let secondDrop: string | undefined;
    for (let e = 0; e < RELEASE_GRACE_MS + 400; e += STEP_MS) {
        tD += STEP_MS;
        devD.feed(simD.poll(tD), tD);
        for (const l of devD.drainLog()) if (l.includes("four: drop")) secondDrop = l;
    }
    check("and the beat does not wedge it - the next real gesture plays, for the OTHER player",
        !!secondDrop && secondDrop.includes("player=2") && secondDrop.includes("col=6"),
        secondDrop ?? "(no second drop)");

    // ---- scenario C: strays must never drop anything. -------------------
    // The mirror image of a dropout: phantom contacts reported while nothing
    // is touching the glass. A stray that dropped a piece onto the board
    // while the puck sat in a bag would be indistinguishable, to her, from
    // the toy playing by itself.
    //
    // C1 IS THE ONE THAT MATTERS, because it is not statistical. four.c
    // arms a gesture only on ARM_SAMPLES contacts sustained at ARM_RATE_HZ,
    // which makes "fewer than ARM_SAMPLES phantom contacts can never arm,
    // at any spacing" a hard property rather than a probability - so it is
    // asserted directly, by injecting bursts at every spacing from one
    // report apart to nearly half a second apart, instead of by running a
    // random stray process and hoping it does not produce a bad pair.
    //
    // AN EARLIER VERSION OF FOUR.C FAILED THIS, and the failure is the
    // reason the arming rule looks the way it does: with ARM_SAMPLES=2 and
    // no rate condition, any two strays inside RELEASE_GRACE_MS armed a
    // gesture and the release verdict then dropped a piece. See four.c's
    // own ARMING comment.
    console.log("");
    let burstDrops = 0;
    const burstCases: string[] = [];
    for (const spacingMs of [17, 33, 60, 120, 200, 400]) {
        for (const count of [1, 2, 3]) {
            const devC = await freshDevice();
            let tC = devC.startMs;
            const [sx, sy] = landToPanel(colX(3), THUMB_LY);
            for (let k = 0; k < count; k++) {
                // one phantom contact...
                devC.feed({ fingers: 1, x: sx, y: sy }, tC);
                // ...then nothing at all until the next one is due
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
            if (devC.drainLog().some((l) => l.includes("four: drop"))) {
                burstDrops++;
                burstCases.push(`${count} contact(s) ${spacingMs}ms apart`);
            }
        }
    }
    check("no burst of up to three phantom contacts, at any spacing, ever drops a piece",
        burstDrops === 0, burstDrops === 0 ? "18 burst shapes tried" : burstCases.join(", "));

    // C2: the emulator's own modelled stray process, run long. This is the
    // rate emulator/src/constants.ts calibrates as what this controller
    // actually does, so it is the rate a claim about the shipped device can
    // honestly be made at.
    const strayProfile = { ...TOUCHSIM_DEFAULTS, dropoutsEnabled: false, straysEnabled: true };
    const devC2 = await freshDevice();
    const simC2 = new TouchSim(strayProfile, PANEL_W, PANEL_H, seededRng(seedFromName("four-strays")));
    simC2.setPointer(false, 0, 0);
    let strayDrops = 0;
    let tC2 = devC2.startMs;
    for (let e = 0; e < 120000; e += STEP_MS) {
        tC2 += STEP_MS;
        devC2.feed(simC2.poll(tC2), tC2);
        if (devC2.drainLog().some((l) => l.includes("four: drop"))) strayDrops++;
    }
    console.log(`    ${simC2.simStrays} stray contacts synthesised over 120s at the emulator's modelled rate ` +
        `(${strayProfile.straysPerSec}/s), with nothing touching the glass`);
    check("two minutes of the modelled stray process drops nothing", strayDrops === 0,
        `${strayDrops} piece(s) dropped by strays`);

    console.log(`\n${passCount} passed, ${failCount} failed`);
    if (failCount > 0) process.exit(1);
}

main();
