// feature-four-cpu: headless verification of Connect Four's cpu opponent
// (firmware/apps/four.c, section 9 of its own header) - against the REAL
// firmware compiled to wasm (decision 0003), the same convention every
// file in this directory follows.
//
// WHAT THIS FILE PROVES:
//   1. the machine only ever moves on ITS OWN turn - never on hers, never
//      by itself while she is deciding (the same "nothing plays by itself"
//      property feature-four.ts asserts for two-player play, re-asserted
//      here because a vs-cpu game is the one mode where something is now
//      SUPPOSED to move without a finger touching the glass, so proving it
//      stays scoped to the right turn matters more here, not less);
//   2. every move it makes lands in a column that actually had room - it
//      never plays on top of a full one;
//   3. it can be beaten by a reasonably attentive human (a handful of
//      games are enough to show at least one human win; the actual
//      MEASURED win rate, over thousands of games against a naive player
//      model, is tools/four-cpu-winrate.ts's job, not this file's - a
//      number belongs next to the code that produces it, and that tool
//      re-runs in seconds, this file exists to catch a regression that
//      breaks the shape of the behaviour, not to re-measure the rate).
//
// The machine's move being VISIBLE as a decision (a think beat, then an
// aim beat with the same highlight a thumb would leave) is asserted in
// feature-four-choice.ts, which already drives a vs-cpu game to get there
// - not duplicated here.
//
// Run with (after `bun run emulator/wasm/build.ts`):
//
//   bun run emulator/wasm/tests/feature-four-cpu.ts
import { readFileSync } from "node:fs";
import { join } from "node:path";

const WASM_PATH = join(import.meta.dir, "..", "dist", "emu.wasm");
const PANEL_W = 368;
const PANEL_H = 448;
const LAND_W = PANEL_H; // 448
const LAND_H = PANEL_W; // 368
const APP_FOUR = 3;

const COLS = 7, ROWS = 6;
const BEZEL = 10;
const SAFE_X0 = BEZEL, SAFE_W = LAND_W - 2 * BEZEL;
const CELL = 48;
const BOARD_X0 = SAFE_X0 + Math.floor((SAFE_W - COLS * CELL) / 2);
const colX = (c: number) => BOARD_X0 + Math.floor(CELL / 2) + c * CELL;
const RELEASE_GRACE_MS = 300;
const HANDOFF_MS = 420;
const CPU_THINK_MS = 550;
const CPU_AIM_MS = 350;
const P_RED = 1, P_BLUE = 2;
const CENTRE_COL = 3;
const THUMB_LY = 200;

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
    const fwLog: string[] = [];
    const instance = await WebAssembly.instantiate(mod, {
        env: {
            js_log(ptr: number, len: number) { fwLog.push(decoder.decode(new Uint8Array(memory.buffer, ptr, len))); },
            sinf: (x: number) => Math.sin(x), cosf: (x: number) => Math.cos(x),
            atan2f: (y: number, x: number) => Math.atan2(y, x), sqrtf: (x: number) => Math.sqrt(x),
            fabsf: (x: number) => Math.abs(x), floorf: (x: number) => Math.floor(x),
            fmodf: (x: number, y: number) => x % y, powf: (x: number, y: number) => Math.pow(x, y),
            expf: (x: number) => Math.exp(x),
        },
    });
    memory = instance.exports.memory as WebAssembly.Memory;
    const exp = instance.exports as any;
    if (exp.emu_init() !== 1) throw new Error("emu_init() failed");
    exp.emu_tick(0);
    exp.emu_app_switch(APP_FOUR);
    exp.emu_tick(10);
    fwLog.length = 0;
    return {
        touchLand(down: boolean, lx: number, ly: number, nowMs: number) {
            exp.emu_touch(down ? 1 : 0, PANEL_W - 1 - Math.round(ly), Math.round(lx));
            exp.emu_tick(nowMs);
        },
        fbSnapshot(): Uint8Array {
            const ptr = exp.emu_fb();
            return new Uint8Array(memory.buffer, ptr, PANEL_W * PANEL_H * 2).slice();
        },
        fwLogLines(): readonly string[] { return fwLog; },
        drainLog(): string[] { const out = fwLog.slice(); fwLog.length = 0; return out; },
    };
}
type Device = Awaited<ReturnType<typeof loadDevice>>;

function settle(dev: Device, t0: number, durationMs: number, stepMs = 15): number {
    let t = t0;
    for (let waited = 0; waited < durationMs; waited += stepMs) { t += stepMs; dev.touchLand(false, 0, 0, t); }
    return t;
}
function holdLand(dev: Device, lx: number, ly: number, t0: number, durationMs: number, stepMs = 15): number {
    let t = t0;
    for (let held = 0; held < durationMs; held += stepMs) { t += stepMs; dev.touchLand(true, lx, ly, t); }
    return t;
}
// Picks "vs cpu" with clean input and settles through the opening hand-off.
function chooseVsCpu(dev: Device, t0: number): number {
    let t = holdLand(dev, LAND_W * 0.75, 200, t0, 350);
    t = settle(dev, t, RELEASE_GRACE_MS + 350);
    t = settle(dev, t, HANDOFF_MS + 200);
    return t;
}
// A whole human move: press, hold, release, settle through the fall and
// (if it is her turn again next) the hand-off.
function playMove(dev: Device, col: number, t0: number): number {
    let t = holdLand(dev, colX(col), THUMB_LY, t0, 140);
    return settle(dev, t, RELEASE_GRACE_MS + 900);
}
// Settle through the machine's own whole turn (hand-off already spent by
// the human's own playMove settle above; this spends think+aim+fall).
function settleCpuTurn(dev: Device, t0: number): number {
    return settle(dev, t0, CPU_THINK_MS + CPU_AIM_MS + 900);
}

type Mirror = { cells: number[] };
function newMirror(): Mirror { return { cells: new Array(ROWS * COLS).fill(0) }; }
function mirrorApply(m: Mirror, col: number, row: number, player: number) { m.cells[row * COLS + col] = player; }
function mirrorLanding(m: Mirror, col: number): number {
    for (let r = ROWS - 1; r >= 0; r--) if (m.cells[r * COLS + col] === 0) return r;
    return -1;
}
function mirrorLine(m: Mirror, col: number, row: number, p: number): number {
    const DC = [1, 0, 1, 1], DR = [0, 1, 1, -1];
    let best = 1;
    for (let i = 0; i < 4; i++) {
        let n = 1;
        for (const sign of [1, -1]) {
            let c = col + DC[i]! * sign, r = row + DR[i]! * sign;
            while (c >= 0 && c < COLS && r >= 0 && r < ROWS && m.cells[r * COLS + c] === p) { n++; c += DC[i]! * sign; r += DR[i]! * sign; }
        }
        if (n > best) best = n;
    }
    return best;
}
// A competent human move: take a win, else block, else build, preferring
// the middle - the same shape feature-four.ts's own chooseMove() uses.
function chooseMove(m: Mirror, p: number): number {
    const other = p === P_RED ? P_BLUE : P_RED;
    for (let c = 0; c < COLS; c++) { const r = mirrorLanding(m, c); if (r >= 0 && mirrorLine(m, c, r, p) >= 4) return c; }
    for (let c = 0; c < COLS; c++) { const r = mirrorLanding(m, c); if (r >= 0 && mirrorLine(m, c, r, other) >= 4) return c; }
    let best = -1, bestScore = -1;
    for (let c = 0; c < COLS; c++) {
        const r = mirrorLanding(m, c);
        if (r < 0) continue;
        const score = mirrorLine(m, c, r, p) * 10 + (3 - Math.abs(c - CENTRE_COL));
        if (score > bestScore) { bestScore = score; best = c; }
    }
    return best;
}

const CELEBRATE_SKIP_MS = 1200; // four.c's own

// Plays ONE game to completion (a competent human, chooseMove(), against
// the machine), reports every drop and the final outcome through the
// callbacks, then skips any celebration, waits out the drain, and returns
// with the app back at the choice screen - ready for the next game.
async function playGame(
    dev: Device, t0: number,
    onDrop: (col: number, row: number, player: number, expectedTurn: number, mirror: Mirror) => void,
    onOutcome: (kind: "win" | "draw", winner?: number) => void,
): Promise<number> {
    const mirror = newMirror();
    let t = t0;
    let over = false;
    for (let move = 0; move < 60 && !over; move++) {
        const placed = mirror.cells.filter((v) => v !== 0).length;
        const turn = placed % 2 === 0 ? P_RED : P_BLUE;
        dev.drainLog();
        if (turn === P_RED) {
            const col = chooseMove(mirror, P_RED);
            if (col < 0) break; // board full with no legal move: PH_DRAWPAUSE
                                  // will have already fired by now
            t = playMove(dev, col, t);
        } else {
            t = settleCpuTurn(dev, t);
        }
        for (const l of dev.drainLog()) {
            const m = l.match(/four: drop col=(\d+) row=(\d+) player=(\d+)/);
            if (m) {
                const mc = Number(m[1]), mr = Number(m[2]), mp = Number(m[3]);
                onDrop(mc, mr, mp, turn, mirror);
                mirrorApply(mirror, mc, mr, mp);
            }
            if (l.includes("four: win")) {
                const w = l.match(/player=(\d+)/);
                onOutcome("win", w ? Number(w[1]) : undefined);
                over = true;
            } else if (l.includes("four: draw")) {
                onOutcome("draw");
                over = true;
            }
        }
    }
    // Skip a win celebration if one is running (harmless otherwise - too
    // short a touch to arm the choice screen's own gesture, and PH_DRAIN
    // does not read touch specially either), then wait out the drain.
    t = settle(dev, t, CELEBRATE_SKIP_MS + 100);
    t = holdLand(dev, colX(3), THUMB_LY, t, 60);
    t = settle(dev, t, 1400);
    return t;
}

async function main() {
    console.log("=== feature: Connect Four's cpu opponent (firmware/apps/four.c, section 9) ===\n");

    // ---- 1 & 2: the machine only moves on its own turn, legally ---------
    console.log('-- vs cpu: does the machine ever move outside its own turn, or illegally? --');
    const dev = await loadDevice();
    let t = chooseVsCpu(dev, 1000);
    let illegalMoves = 0, offTurn = 0, gamesChecked = 0;
    for (let g = 0; g < 3; g++) {
        t = await playGame(dev, t,
            (col, row, player, expectedTurn, mirror) => {
                if (player !== expectedTurn) { offTurn++; return; }
                const wantRow = mirrorLanding(mirror, col);
                if (wantRow !== row || wantRow < 0) illegalMoves++;
            },
            () => { gamesChecked++; });
        t = chooseVsCpu(dev, t); // re-pick "vs cpu" for the next game -
                                   // section 8's "re-choosable" path, taken
                                   // here rather than worked around
    }
    check("the machine never drops a piece attributed to the wrong colour for its turn",
        offTurn === 0, `${offTurn} off-turn drop(s)`);
    check("every drop - human's and the machine's - lands in the row that column actually had open",
        illegalMoves === 0, `${illegalMoves} illegal drop(s)`);
    check("games actually completed while this was checked (a silent hang would pass 0/0 otherwise)",
        gamesChecked >= 3, `${gamesChecked} game(s) played to completion`);

    // ---- 3: it can be beaten ---------------------------------------------
    // chooseMove() here is a deliberately STRONG opponent (always takes a
    // win, always blocks, always prefers the centre) - much stronger than
    // the two-year-old this cpu is tuned against, on purpose: it is the
    // sanity floor, not the target. If even this cannot win once in ten
    // games, CPU_TAKE_WIN_PCT/CPU_BLOCK_PCT are set far too high and this
    // file should say so without waiting on the slower tool.
    //
    // This file does NOT assert a win rate against a REALISTIC (naive,
    // cannot-plan-ahead) opponent, or that the cpu wins its own share -
    // that number, measured over thousands of games against the naive
    // player model the task calls for, is tools/four-cpu-winrate.ts's job,
    // and it is the number that actually belongs in this app's report.
    console.log("\n-- can a human beat it? (ten quick games, a deliberately strong human) --");
    const dev2 = await loadDevice();
    let t2 = chooseVsCpu(dev2, 1000);
    let humanWins = 0, cpuWins = 0, draws = 0;
    for (let g = 0; g < 10; g++) {
        t2 = await playGame(dev2, t2,
            () => {},
            (kind, winner) => {
                if (kind === "draw") draws++;
                else if (winner === P_RED) humanWins++;
                else if (winner === P_BLUE) cpuWins++;
            });
        t2 = chooseVsCpu(dev2, t2);
    }
    console.log(`    10 games: human won ${humanWins}, cpu won ${cpuWins}, draws ${draws}`);
    check("a strong human wins at least one game in ten - the cpu is beatable, not a wall",
        humanWins >= 1, `${humanWins}/10 human wins`);

    console.log(`\n${passCount} passed, ${failCount} failed`);
    if (failCount > 0) process.exit(1);
}

main();
