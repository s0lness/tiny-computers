// repro-touch-dropout-morpion: the morpion gesture (firmware/apps/
// morpion.c) driven by a touch stream that misbehaves the way the real
// controller does, rather than by a clean pointer.
//
// WHY THIS FILE EXISTS SEPARATELY FROM feature-morpion.ts. This project has
// already shipped one feature that worked in the emulator and did not work
// in the owner's hands, on this exact hazard: the sketchpad's colour
// palette, whose long press was reset dozens of times a second by a
// controller reporting a lost contact that never happened
// (repro-touch-dropout-palette-open.ts, and sketch.c's lastContactMs). The
// palette's own feature test drove clean input and passed all 22 of its
// checks throughout. AGENTS.md's regression-test section now states the
// rule this file obeys: a feature driven by touch needs BOTH kinds of file.
//
// MORPION IS THE WORST CASE ON THIS DEVICE SO FAR. The gesture's verb is
// RELEASE, and a false release does not blur a stroke here: it plants a
// mark in one of nine cells, ends that player's turn and hands the puck
// over. Connect Four has 42 cells and a bad piece is one bad move; noughts
// and crosses has nine, and one unintended mark is very often the whole
// game. The release verdict is therefore the thing under test.
//
// AND THE GAME INVITES THE WORST CASE. The scenario that breaks a
// dropout-blind implementation is a finger held STILL on the glass, which
// in Connect Four is a player choosing a column and here is a player
// looking at a board with a threat on it. Thinking with a thumb down is how
// this game is played.
//
// THE PROFILE. Identical to repro-touch-dropout-stroke-start.ts's and
// repro-touch-dropout-four-drop.ts's dropoutHeavy: 34 dropout episodes per
// second, back-calculated in the first of those files from a real hardware
// session (798 dropouts over ~23s of finger-down time). At the controller's
// 60Hz report rate that is a per-report loss probability of 0.57, so
// contact is ABSENT on most individual reports and the firmware has to
// bridge continuously.
//
// Run with (after `bun run emulator/wasm/build.ts`):
//
//   bun run emulator/wasm/tests/repro-touch-dropout-morpion.ts
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { TouchSim, type TouchReport } from "../../src/touchsim";
import { TOUCHSIM_DEFAULTS } from "../../src/constants";

const WASM_PATH = join(import.meta.dir, "..", "dist", "emu.wasm");
const PANEL_W = 368;
const PANEL_H = 448;
// g_apps[] = { chrono, sketch("draw"), timer, four, morpion }
const APP_MORPION = 4;

// Mirrors of morpion.c's own constants, derived the way the firmware
// derives them (the board is laid out against the VISIBLE canvas - gfx.h's
// PANEL_BEZEL_MARGIN_PX, the band the case hides). This file only ever
// needs a point inside a given cell.
const GRID = 3;
const BEZEL = 10;
const LAND_W = PANEL_H;
const LAND_H = PANEL_W;
const SAFE_X1 = LAND_W - BEZEL;
const SAFE_Y0 = BEZEL, SAFE_Y1 = LAND_H - BEZEL;
const SAFE_H = SAFE_Y1 - SAFE_Y0;
const SLAB_PAD = 8;
const CELL = Math.floor((SAFE_H - 2 * SLAB_PAD) / GRID);
const SLAB_W = GRID * CELL + 2 * SLAB_PAD;
const SLAB_X0 = SAFE_X1 - SLAB_W;
const SLAB_Y0 = SAFE_Y0 + Math.floor((SAFE_H - SLAB_W) / 2);
const BOARD_X0 = SLAB_X0 + SLAB_PAD;
const BOARD_Y0 = SLAB_Y0 + SLAB_PAD;
const cellCx = (cell: number) => BOARD_X0 + Math.floor(CELL / 2) + (cell % GRID) * CELL;
const cellCy = (cell: number) => BOARD_Y0 + Math.floor(CELL / 2) + Math.floor(cell / GRID) * CELL;

const RELEASE_GRACE_MS = 300;
const INK_MS = 340;
const HANDOFF_MS = 420;

let passCount = 0;
let failCount = 0;
function check(label: string, ok: boolean, detail?: string) {
    const status = ok ? "PASS" : "FAIL";
    if (ok) passCount++; else failCount++;
    console.log(`[${status}] ${label}${detail ? " - " + detail : ""}`);
}

const bytes = readFileSync(WASM_PATH);
const compiled = await WebAssembly.compile(bytes);

// A FRESH module per trial. Trials must not interact: a mark placed in
// trial 3 makes trial 4's cell refuse a release entirely, a game that ends
// enters a celebration that swallows the next gesture, and a board that
// fills deals a new one mid-trial - none of which is what is being measured
// here.
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
    exp.emu_app_switch(APP_MORPION);
    exp.emu_tick(10);
    if (exp.emu_app_current() !== APP_MORPION) throw new Error("failed to switch into morpion");
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

// gfx.h's mapping is landscape (lx, ly) -> panel (PANEL_W-1-ly, lx), so a
// landscape point a test wants to touch becomes this panel point before it
// ever reaches TouchSim - kept here rather than shared for the same reason
// every test in this directory is self-contained.
function landToPanel(lx: number, ly: number): [number, number] {
    return [PANEL_W - 1 - Math.round(ly), Math.round(lx)];
}

const dropoutHeavy = { ...TOUCHSIM_DEFAULTS, dropoutsEnabled: true, dropoutsPerSec: 34, straysEnabled: false };
const STEP_MS = 1000 / dropoutHeavy.reportRateHz;

async function main() {
    console.log("=== morpion's press-drag-release, under a dropout-heavy touch stream ===\n");
    console.log(`    profile: ${dropoutHeavy.dropoutsPerSec} dropout episodes/sec at ${dropoutHeavy.reportRateHz}Hz ` +
        `(p=${(dropoutHeavy.dropoutsPerSec / dropoutHeavy.reportRateHz).toFixed(2)} per report), ` +
        `RELEASE_GRACE_MS=${RELEASE_GRACE_MS}\n`);

    // ---- scenario A: A THUMB HELD STILL MUST NOT PLACE A MARK. ----------
    // This is the palette bug's own shape - a finger that is DOWN but not
    // MOVING, which is what a controller's dropouts make indistinguishable
    // from a lift unless the firmware keeps a separate contact clock. Here
    // the consequence of getting it wrong is a mark in whatever cell the
    // thumb happened to be resting on while its owner was still thinking,
    // and thinking with a thumb down is the normal way to play this game.
    const HOLD_TRIALS = 25;
    const HOLD_MS = 3000;
    let heldWithoutPlacing = 0;
    let totalDropouts = 0;
    for (let i = 0; i < HOLD_TRIALS; i++) {
        const dev = await freshDevice();
        const sim = new TouchSim(dropoutHeavy, PANEL_W, PANEL_H);
        const cell = i % (GRID * GRID);
        const [pxx, pyy] = landToPanel(cellCx(cell), cellCy(cell));
        sim.setPointer(true, pxx, pyy);
        let t = 1000;
        let placed = false;
        for (let held = 0; held < HOLD_MS; held += STEP_MS) {
            t += STEP_MS;
            dev.feed(sim.poll(t), t);
            if (dev.drainLog().some((l) => l.includes("morpion: mark"))) { placed = true; break; }
        }
        totalDropouts += sim.simDropouts;
        if (!placed) heldWithoutPlacing++;
    }
    const holdRate = (heldWithoutPlacing / HOLD_TRIALS) * 100;
    console.log(`    ${totalDropouts} simulated dropout episodes across ${HOLD_TRIALS} x ${HOLD_MS}ms of held contact`);
    // A RATE, with the threshold taken from morpion.c's own arithmetic
    // table, not "all of them". This scenario is a Bernoulli process: three
    // seconds of contact contains about 44 dropout gaps and each one
    // outlasts RELEASE_GRACE_MS with probability ~3e-5, so about 0.14% of
    // trials are expected to lose one however correct the firmware is.
    // Demanding 100% is what four.c's equivalent test did at first, and it
    // duly failed on a run that had nothing wrong with it - which is worse
    // than a weaker gate, because a test that cries wolf gets re-run until
    // it agrees.
    //
    // 96% still separates the two cases by a mile: a working release
    // verdict sits at ~99.9%, and one that believes the runtime's own
    // touchReleased sits at ~0% (every trial loses a mark within the first
    // few hundred milliseconds). Any value in between is the interesting
    // news this test exists to deliver.
    check("a thumb held on the glass for 3 seconds does not place a mark by itself",
        holdRate >= 96, `${heldWithoutPlacing}/${HOLD_TRIALS} trials survived the hold (${holdRate.toFixed(0)}%)`);

    // ---- scenario B: press, drag across the board, release. -------------
    // The whole gesture, end to end, at the rate a child's thumb actually
    // moves - and DIAGONALLY, because unlike Connect Four this app has two
    // axes to get wrong. What is asserted is not just "a mark appeared" but
    // "exactly one mark appeared, in the cell the thumb finished on, and
    // none appeared while the thumb was still moving". A false release
    // mid-drag is the failure this app cannot afford, and it would pass a
    // looser check that only looked at the end state.
    console.log("");
    const DRAG_TRIALS = 25;
    const DRAG_MS = 900;
    const SETTLE_MS = 320;   // the thumb stops on its cell before letting go
    let correct = 0, placedEarly = 0, missed = 0, wrongCell = 0;
    for (let i = 0; i < DRAG_TRIALS; i++) {
        const dev = await freshDevice();
        const sim = new TouchSim(dropoutHeavy, PANEL_W, PANEL_H);
        const from = i % (GRID * GRID);
        const to = (i * 5 + 4) % (GRID * GRID);
        let t = 1000;

        for (let e = 0; e < DRAG_MS; e += STEP_MS) {
            const u = e / DRAG_MS;
            const [pxx, pyy] = landToPanel(
                cellCx(from) + (cellCx(to) - cellCx(from)) * u,
                cellCy(from) + (cellCy(to) - cellCy(from)) * u);
            sim.setPointer(true, pxx, pyy);
            t += STEP_MS;
            dev.feed(sim.poll(t), t);
        }
        const [tx, ty] = landToPanel(cellCx(to), cellCy(to));
        sim.setPointer(true, tx, ty);
        for (let e = 0; e < SETTLE_MS; e += STEP_MS) {
            t += STEP_MS;
            dev.feed(sim.poll(t), t);
        }

        const early = dev.drainLog().some((l) => l.includes("morpion: mark"));
        if (early) placedEarly++;

        sim.setPointer(false, 0, 0);
        const marks: string[] = [];
        for (let e = 0; e < RELEASE_GRACE_MS + 400; e += STEP_MS) {
            t += STEP_MS;
            dev.feed(sim.poll(t), t);
            for (const l of dev.drainLog()) if (l.includes("morpion: mark")) marks.push(l);
        }

        if (early) continue;
        if (marks.length === 0) { missed++; continue; }
        if (marks.length === 1 && marks[0]!.includes(`cell=${to} `)) correct++;
        else wrongCell++;
    }
    const rate = (correct / DRAG_TRIALS) * 100;
    console.log(`    ${correct}/${DRAG_TRIALS} drags placed exactly one mark in the right cell (${rate.toFixed(0)}%); ` +
        `${placedEarly} placed early, ${missed} never placed, ${wrongCell} landed elsewhere`);
    check("a diagonal drag finished by a genuine lift places exactly one mark, in the cell the thumb finished on",
        rate >= 90, `${rate.toFixed(0)}% over ${DRAG_TRIALS} trials`);
    check("no trial placed a mark while the thumb was still moving", placedEarly === 0, `${placedEarly} early mark(s)`);

    // ---- scenario C: DRUMMING ON THE GLASS WHILE THE PUCK CHANGES HANDS
    // must not play the other player's move.
    //
    // Two people passing one puck do not have a machine's turn in between:
    // the player who just released is still holding it, their thumb is
    // still on it, and the next press is the OTHER player's move.
    // morpion.c answers that with the ink stroke plus the hand-off (a beat
    // in which no gesture may arm - see its section 3), and this is that
    // beat under a touch stream dropping contact the whole time, since a
    // dropout storm is precisely a stream of fake releases and a fake
    // release is what would place the mark.
    //
    // The window drummed on is deliberately shorter than INK_MS plus
    // HANDOFF_MS so that every press AND every release inside it lands
    // while the app is not accepting gestures at all. What it must not do
    // is wedge: the last check makes a real gesture afterwards and requires
    // it to work, for the other player.
    console.log("");
    const devD = await freshDevice();
    const simD = new TouchSim(dropoutHeavy, PANEL_W, PANEL_H);
    let tD = 1000;
    const [dx, dy] = landToPanel(cellCx(0), cellCy(0));
    simD.setPointer(true, dx, dy);
    for (let e = 0; e < 500; e += STEP_MS) { tD += STEP_MS; devD.feed(simD.poll(tD), tD); }
    simD.setPointer(false, 0, 0);
    let firstMark: string | undefined;
    for (let e = 0; e < RELEASE_GRACE_MS + 200; e += STEP_MS) {
        tD += STEP_MS;
        devD.feed(simD.poll(tD), tD);
        for (const l of devD.drainLog()) if (l.includes("morpion: mark")) firstMark = l;
    }
    check("the first player's mark went in", !!firstMark && firstMark.includes("player=1"), firstMark ?? "(no mark)");

    // Drum: 7 presses of ~40ms with ~45ms gaps, all inside the ink stroke
    // and the hand-off that follows it.
    const drumMarks: string[] = [];
    for (let beat = 0; beat < 7; beat++) {
        simD.setPointer(true, dx, dy);
        for (let e = 0; e < 40; e += STEP_MS) { tD += STEP_MS; devD.feed(simD.poll(tD), tD); }
        simD.setPointer(false, 0, 0);
        for (let e = 0; e < 45; e += STEP_MS) { tD += STEP_MS; devD.feed(simD.poll(tD), tD); }
        for (const l of devD.drainLog()) if (l.includes("morpion: mark")) drumMarks.push(l);
    }
    check("drumming on the glass while the mark is drawn and the puck changes hands plays nothing",
        drumMarks.length === 0, drumMarks.length ? drumMarks.map((l) => l.trim()).join(" | ") : "0 extra marks");

    for (let e = 0; e < INK_MS + HANDOFF_MS + 400; e += STEP_MS) { tD += STEP_MS; devD.feed(simD.poll(tD), tD); }
    devD.drainLog();
    simD.setPointer(true, ...landToPanel(cellCx(8), cellCy(8)) as [number, number]);
    for (let e = 0; e < 400; e += STEP_MS) { tD += STEP_MS; devD.feed(simD.poll(tD), tD); }
    simD.setPointer(false, 0, 0);
    let secondMark: string | undefined;
    for (let e = 0; e < RELEASE_GRACE_MS + 400; e += STEP_MS) {
        tD += STEP_MS;
        devD.feed(simD.poll(tD), tD);
        for (const l of devD.drainLog()) if (l.includes("morpion: mark")) secondMark = l;
    }
    check("and the beat does not wedge it - the next real gesture plays, for the OTHER player",
        !!secondMark && secondMark.includes("player=2") && secondMark.includes("cell=8"),
        secondMark ?? "(no second mark)");

    // ---- scenario D: strays must never place anything. ------------------
    // The mirror image of a dropout: phantom contacts reported while
    // nothing is touching the glass. A mark appearing on the board while
    // the puck sat in a bag would be indistinguishable, to her, from the
    // toy playing by itself.
    //
    // D1 IS THE ONE THAT MATTERS, because it is not statistical. morpion.c
    // arms a gesture only on ARM_SAMPLES contacts sustained at ARM_RATE_HZ,
    // which makes "fewer than ARM_SAMPLES phantom contacts can never arm,
    // at any spacing" a hard property rather than a probability - so it is
    // asserted directly, by injecting bursts at every spacing from one
    // report apart to nearly half a second apart, instead of by running a
    // random stray process and hoping it does not produce a bad pair.
    //
    // AN EARLIER VERSION OF FOUR.C FAILED THE SAME TEST, and that failure
    // is why the arming rule this file exercises looks the way it does:
    // with ARM_SAMPLES=2 and no rate condition, any two strays inside
    // RELEASE_GRACE_MS armed a gesture and the release verdict then played
    // a move. See morpion.c's own ARMING comment.
    console.log("");
    let burstMarks = 0;
    const burstCases: string[] = [];
    for (const spacingMs of [17, 33, 60, 120, 200, 400]) {
        for (const count of [1, 2, 3]) {
            const devC = await freshDevice();
            let tC = 1000;
            const [sx, sy] = landToPanel(cellCx(4), cellCy(4));
            for (let k = 0; k < count; k++) {
                devC.feed({ fingers: 1, x: sx, y: sy }, tC);          // one phantom contact...
                for (let e = STEP_MS; e < spacingMs; e += STEP_MS) {   // ...then nothing at all
                    tC += STEP_MS;
                    devC.feed({ fingers: 0, x: 0, y: 0 }, tC);
                }
                tC += STEP_MS;
            }
            for (let e = 0; e < RELEASE_GRACE_MS + 600; e += STEP_MS) {
                tC += STEP_MS;
                devC.feed({ fingers: 0, x: 0, y: 0 }, tC);
            }
            if (devC.drainLog().some((l) => l.includes("morpion: mark"))) {
                burstMarks++;
                burstCases.push(`${count} contact(s) ${spacingMs}ms apart`);
            }
        }
    }
    check("no burst of up to three phantom contacts, at any spacing, ever places a mark",
        burstMarks === 0, burstMarks === 0 ? "18 burst shapes tried" : burstCases.join(", "));

    // D2: the emulator's own modelled stray process, run long. This is the
    // rate emulator/src/constants.ts calibrates as what this controller
    // actually does, so it is the rate a claim about the shipped device can
    // honestly be made at.
    const strayProfile = { ...TOUCHSIM_DEFAULTS, dropoutsEnabled: false, straysEnabled: true };
    const devC2 = await freshDevice();
    const simC2 = new TouchSim(strayProfile, PANEL_W, PANEL_H);
    simC2.setPointer(false, 0, 0);
    let strayMarks = 0;
    let tC2 = 1000;
    for (let e = 0; e < 120000; e += STEP_MS) {
        tC2 += STEP_MS;
        devC2.feed(simC2.poll(tC2), tC2);
        if (devC2.drainLog().some((l) => l.includes("morpion: mark"))) strayMarks++;
    }
    console.log(`    ${simC2.simStrays} stray contacts synthesised over 120s at the emulator's modelled rate ` +
        `(${strayProfile.straysPerSec}/s), with nothing touching the glass`);
    check("two minutes of the modelled stray process places nothing", strayMarks === 0,
        `${strayMarks} mark(s) placed by strays`);

    // ---- scenario E: THE POSITION OF A STILL FINGER WANDERS. ------------
    // The failure mode touchsim.ts's positionJitter* models, added after a
    // real session measured the reported centroid jumping hundreds of
    // pixels while the owner's finger sat still. Connect Four could largely
    // shrug at this - a jump along one axis only changes which of seven
    // columns is lit - but morpion reads BOTH axes, so a wander of a
    // hundred pixels can land in a different cell, and if it lands there at
    // the moment of release it plays a move in a cell nobody was on.
    //
    // WHAT IS AND IS NOT ASSERTED, and the first draft of this got it
    // wrong in a way worth recording. It asserted that a wrong mark always
    // lands in a NEIGHBOURING cell, which sounded like "nothing amplifies a
    // small lie" and is actually false by construction: the modelled
    // displacement is 80 to 250px and a cell is 110px, so a single episode
    // can put the reported position two cells away and duly did, 1 trial in
    // 30. The firmware was right and the assertion was wrong.
    //
    // Nothing can make this impossible in the first place. If the
    // controller says the finger is somewhere else at the moment of
    // release, the app's only honest reading is that the finger is
    // somewhere else, and inventing a filter to override the controller
    // would trade a rare wrong cell for a permanently sluggish highlight -
    // decision 0008's own argument about filters that are never exercised.
    //
    // So what is asserted is the property that IS the app's to keep: THE
    // MARK ALWAYS LANDS IN A CELL THE CONTROLLER ACTUALLY REPORTED. The
    // cells the simulated controller named during the gesture are collected
    // here, and the cell the firmware played has to be one of them. That is
    // non-statistical, it is what "the app does not invent a position"
    // means, and it holds however badly the hardware lies. The correct-cell
    // rate is reported alongside it as news rather than as a gate.
    console.log("");
    //
    // THE RATE IS THE CALIBRATED ONE AND HAS TO BE WRITTEN OUT IN FULL.
    // TOUCHSIM_DEFAULTS has positionJitterEnabled false AND
    // positionJitterPerSec ZERO, so turning only the flag on produces a
    // profile that never fires a single episode and a scenario that passes
    // by doing nothing - which is exactly the "a filter that never fires in
    // testing" trap decision 0008 is about. The numbers below are
    // repro-touch-dropout-palette-open.ts's TOUCHSIM_JITTER_PROFILE, where
    // 1.5 episodes/sec was found by sweeping 0.3 to 2.0 against the
    // hardware's own measured 10 splits in 40 seconds. This test reports
    // the episode count it actually synthesised so a future zero is
    // visible rather than silent.
    const jitterProfile = {
        ...TOUCHSIM_DEFAULTS,
        dropoutsEnabled: true, dropoutsPerSec: 34, straysEnabled: false,
        positionJitterEnabled: true, positionJitterPerSec: 1.5,
        positionJitterMinPx: 80, positionJitterMaxPx: 250, positionJitterMaxHoldReports: 3,
    };
    // Which cell a PANEL-space report names, by morpion.c's own arithmetic
    // (gfx.h's mapping inverted, then clamped on both axes because
    // cell_from_land() clamps rather than rejecting - there is no point on
    // the canvas that belongs to no cell).
    function cellFromReport(panelX: number, panelY: number): number {
        const lx = panelY, ly = PANEL_W - 1 - panelX;
        let c = Math.floor((lx - BOARD_X0) / CELL);
        let r = Math.floor((ly - BOARD_Y0) / CELL);
        if (c < 0) c = 0; if (c > GRID - 1) c = GRID - 1;
        if (r < 0) r = 0; if (r > GRID - 1) r = GRID - 1;
        return r * GRID + c;
    }

    // 60 rather than the 25-30 the other scenarios use: this one is
    // reporting a RATE that moved from about 90% to about 99% when
    // morpion.c's SETTLE_MS went in, and 30 trials cannot tell those apart
    // (the pre-fix implementation was measured at 25/30 and 29/30 on
    // consecutive runs, straddling anything a 30-trial gate could sit at).
    const JIT_TRIALS = 60;
    let jitCorrect = 0, jitElsewhere = 0, jitNone = 0, jitEpisodes = 0, jitInvented = 0;
    const inventedCases: string[] = [];
    for (let i = 0; i < JIT_TRIALS; i++) {
        const dev = await freshDevice();
        const sim = new TouchSim(jitterProfile, PANEL_W, PANEL_H);
        const cell = i % (GRID * GRID);
        const [pxx, pyy] = landToPanel(cellCx(cell), cellCy(cell));
        sim.setPointer(true, pxx, pyy);
        let t = 1000;
        const reported = new Set<number>();
        for (let e = 0; e < 700; e += STEP_MS) {
            t += STEP_MS;
            const rep = sim.poll(t);
            if (rep.fingers === 1) reported.add(cellFromReport(rep.x, rep.y));
            dev.feed(rep, t);
        }
        sim.setPointer(false, 0, 0);
        let got = -1;
        for (let e = 0; e < RELEASE_GRACE_MS + 400; e += STEP_MS) {
            t += STEP_MS;
            const rep = sim.poll(t);
            if (rep.fingers === 1) reported.add(cellFromReport(rep.x, rep.y));
            dev.feed(rep, t);
            for (const l of dev.drainLog()) {
                const m = l.match(/morpion: mark cell=(\d+)/);
                if (m) got = Number(m[1]);
            }
        }
        jitEpisodes += sim.simJitterEpisodes;
        if (got < 0) { jitNone++; continue; }
        if (!reported.has(got)) {
            jitInvented++;
            inventedCases.push(`trial ${i}: played ${got}, controller only ever said ${[...reported].join("/")}`);
        }
        if (got === cell) jitCorrect++; else jitElsewhere++;
    }
    console.log(`    ${jitEpisodes} jitter episodes synthesised (80-250px, up to 3 reports each) across ${JIT_TRIALS} trials`);
    console.log(`    with the controller's position wandering: ${jitCorrect} landed on the finger's own cell, ` +
        `${jitElsewhere} elsewhere, ${jitNone} nothing placed, over ${JIT_TRIALS} trials`);
    check("the jitter profile is actually firing, not sitting dormant (decision 0008)",
        jitEpisodes > 0, `${jitEpisodes} episodes`);
    check("THE MARK ALWAYS LANDS IN A CELL THE CONTROLLER ACTUALLY REPORTED - the app never invents a position",
        jitInvented === 0, jitInvented === 0 ? `${JIT_TRIALS} trials` : inventedCases.join("; "));
    // 95%, and the gate is placed the way four.c places its own: between
    // the two implementations rather than at the top. WITHOUT morpion.c's
    // SETTLE_MS this scenario measured 83% and 97% on consecutive runs
    // (mean about 90%, i.e. roughly one wrong mark per nine-move game);
    // WITH it, 100%, 100% and 97%. A run that comes back near 90% means
    // that separation has been lost, which is the news this exists to
    // deliver - not "the jitter is gone", which no filter can achieve.
    check("a still thumb, with the reported position wandering, still lands on its own cell almost every time",
        jitCorrect >= Math.ceil(JIT_TRIALS * 0.95), `${jitCorrect}/${JIT_TRIALS}`);
    check("and it always lands somewhere - a wandering position never swallows the move entirely",
        jitNone === 0, `${jitNone} trial(s) placed nothing`);

    console.log(`\n${passCount} passed, ${failCount} failed`);
    if (failCount > 0) process.exit(1);
}

main();
