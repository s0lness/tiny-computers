/**
 * four-cpu-winrate: measures Connect Four's cpu opponent
 * (firmware/apps/four.c, section 9 of its own header) against a NAIVE
 * PLAYER MODEL - someone who cannot plan ahead - by driving the REAL
 * compiled firmware (decision 0003: not a reimplementation of
 * cpu_choose_col(), the same emu.wasm every test in this repository runs
 * against) through many complete games.
 *
 * WHY THIS EXISTS. Connect Four is solved: a perfect first player always
 * wins. An opponent anywhere near its own ceiling is not a playmate for a
 * two-year-old, it is a wall, and "the cpu feels beatable" is not a claim
 * this project ships on a hunch - every other tuned constant here (touch
 * arming, release grace, the menu's hover confirm) was measured, and this
 * one is too. Two models, because "cannot plan ahead" has more than one
 * honest shape:
 *
 *   - RANDOM: chooses uniformly among the columns that are not full, every
 *     move. The floor - a toddler with no strategy at all.
 *   - NUDGED: as RANDOM, except if an immediate win is sitting on the board
 *     it is taken half the time (an older sibling helping, or a lucky
 *     glance) - a slightly sharper naive player, so the number is not
 *     reported against the single easiest possible opponent.
 *
 * Run with (after `bun run emulator/wasm/build.ts`):
 *
 *   bun tools/four-cpu-winrate.ts [--games N]
 *
 * Defaults to 60 games per model. NOT FAST: every game is driven through
 * the same press/hold/release/think/aim timing the real gesture and the
 * real cpu turn use (RELEASE_GRACE_MS, HANDOFF_MS, CPU_THINK_MS,
 * CPU_AIM_MS, all lifted from four.c, never shortened), because the point
 * is to measure what actually ships, not a faster stand-in for it.
 * Measured throughput is about 0.3 games/s, so 60 games/model is roughly
 * 3-4 minutes per model, 6-8 minutes total; --games 120 (closer to the
 * number this file's own numbers were reported at) takes 12-17 minutes.
 * Re-run this after touching CPU_TAKE_WIN_PCT, CPU_BLOCK_PCT or
 * CPU_NEAR_BIAS_PCT in four.c - the number this prints is what four.c's
 * own header cites, and it drifts out of date the moment those constants
 * change without a re-run.
 *
 * LAST MEASURED 2026-08-15, at CPU_TAKE_WIN_PCT=40, CPU_BLOCK_PCT=10,
 * CPU_NEAR_BIAS_PCT=70, 120 games/model against the real compiled
 * firmware: RANDOM child-win 26.7% (32/120), NUDGED child-win 55.0%
 * (66/120), zero draws either way (a 6x7 board this size rarely fills
 * before somebody completes a four). Read plainly: against a "toddler"
 * with genuinely zero strategy, she wins about a quarter of games; against
 * one who at least notices an obvious win (arguably the more realistic
 * model for a child this device's own cues are built to help), she wins
 * more than half. Neither is a wall (0%), and neither is a joke (100%).
 */
import { readFileSync } from "node:fs";
import { join } from "node:path";

const ROOT = join(import.meta.dir, "..");
const WASM_PATH = join(ROOT, "emulator", "wasm", "dist", "emu.wasm");

const PANEL_W = 368;
const PANEL_H = 448;
const LAND_W = PANEL_H; // 448
const COLS = 7, ROWS = 6;
const BEZEL = 10;
const CELL = 48;
const BOARD_X0 = BEZEL + Math.floor((LAND_W - 2 * BEZEL - COLS * CELL) / 2);
const colX = (c: number) => BOARD_X0 + Math.floor(CELL / 2) + c * CELL;
const THUMB_LY = 200;

// Mirrors of four.c's own constants (section 8/9) - lifted, not re-derived,
// same convention every file under emulator/wasm/tests/ uses.
const RELEASE_GRACE_MS = 300;
const HANDOFF_MS = 420;
const CPU_THINK_MS = 550;
const CPU_AIM_MS = 350;
const CELEBRATE_SKIP_MS = 1200;

const P_RED = 1, P_BLUE = 2;

const args = process.argv.slice(2);
const gamesIdx = args.indexOf("--games");
const GAMES_PER_MODEL = gamesIdx >= 0 ? Number(args[gamesIdx + 1]) : 60;

async function loadDevice() {
    let memory!: WebAssembly.Memory;
    const dec = new TextDecoder();
    const log: string[] = [];
    const inst = await WebAssembly.instantiate(await WebAssembly.compile(readFileSync(WASM_PATH)), {
        env: {
            js_log(p: number, l: number) { log.push(dec.decode(new Uint8Array(memory.buffer, p, l))); },
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
    memory = inst.exports.memory as WebAssembly.Memory;
    const e = inst.exports as any;
    if (e.emu_init() !== 1) throw new Error("emu_init() failed - see [fw] log lines above");

    // Resolved by name from emu_device(), not hardcoded (house rule: two
    // preview tools once rendered the wrong app for hours because they
    // hardcoded an index).
    const jsonBytes = new Uint8Array(memory.buffer, e.emu_device());
    let end = 0; while (jsonBytes[end] !== 0) end++;
    const apps: string[] = JSON.parse(dec.decode(jsonBytes.subarray(0, end))).apps || [];
    const APP_FOUR = apps.indexOf("four");
    if (APP_FOUR < 0) throw new Error("this emu.wasm has no four app in its table - rebuild it: bun run emulator/wasm/build.ts");

    e.emu_tick(0);
    e.emu_app_switch(APP_FOUR);
    e.emu_tick(10);
    log.length = 0;
    return {
        touch(down: boolean, lx: number, ly: number, nowMs: number) {
            e.emu_touch(down ? 1 : 0, PANEL_W - 1 - Math.round(ly), Math.round(lx));
            e.emu_tick(nowMs);
        },
        drainLog(): string[] { const out = log.slice(); log.length = 0; return out; },
    };
}
type Device = Awaited<ReturnType<typeof loadDevice>>;

function settle(dev: Device, t0: number, ms: number, stepMs = 15): number {
    let t = t0;
    for (let e = 0; e < ms; e += stepMs) { t += stepMs; dev.touch(false, 0, 0, t); }
    return t;
}
function holdLand(dev: Device, lx: number, ly: number, t0: number, ms: number, stepMs = 15): number {
    let t = t0;
    for (let e = 0; e < ms; e += stepMs) { t += stepMs; dev.touch(true, lx, ly, t); }
    return t;
}
function chooseVsCpu(dev: Device, t0: number): number {
    let t = holdLand(dev, LAND_W * 0.75, THUMB_LY, t0, 350);
    t = settle(dev, t, RELEASE_GRACE_MS + 350);
    return settle(dev, t, HANDOFF_MS + 200);
}
function playMove(dev: Device, col: number, t0: number): number {
    let t = holdLand(dev, colX(col), THUMB_LY, t0, 140);
    return settle(dev, t, RELEASE_GRACE_MS + 900);
}
function settleCpuTurn(dev: Device, t0: number): number {
    return settle(dev, t0, CPU_THINK_MS + CPU_AIM_MS + 900);
}

type Mirror = { cells: number[]; heights: number[] };
function newMirror(): Mirror { return { cells: new Array(ROWS * COLS).fill(0), heights: new Array(COLS).fill(0) }; }
function mirrorLanding(m: Mirror, col: number): number { return m.heights[col]! >= ROWS ? -1 : ROWS - 1 - m.heights[col]!; }
function mirrorApply(m: Mirror, col: number, row: number, player: number) { m.cells[row * COLS + col] = player; m.heights[col]!++; }
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
function legalCols(m: Mirror): number[] {
    const out: number[] = [];
    for (let c = 0; c < COLS; c++) if (mirrorLanding(m, c) >= 0) out.push(c);
    return out;
}

// A tiny xorshift32 PRNG, seeded per process - this file's OWN randomness
// for the naive player model, unrelated to the firmware's (four.c's cpu is
// seeded from the wall clock each game it plays, per its own reset_game()).
let rngState = (Date.now() ^ 0x9e3779b9) >>> 0;
function rngNext(): number {
    let x = rngState;
    x ^= x << 13; x ^= x >>> 17; x ^= x << 5;
    rngState = x >>> 0;
    return rngState;
}
function rngPick<T>(items: T[]): T { return items[rngNext() % items.length]!; }
function rngChance(pct: number): boolean { return (rngNext() % 100) < pct; }

// Model RANDOM: uniformly among the columns that are not full.
function moveRandom(m: Mirror): number {
    const legal = legalCols(m);
    return legal.length ? rngPick(legal) : -1;
}
// Model NUDGED: takes an obvious win half the time, else RANDOM.
function moveNudged(m: Mirror): number {
    const legal = legalCols(m);
    if (legal.length === 0) return -1;
    if (rngChance(50)) {
        for (const c of legal) {
            const r = mirrorLanding(m, c);
            if (mirrorLine(m, c, r, P_RED) >= 4) return c;
        }
    }
    return rngPick(legal);
}

type Outcome = "human" | "cpu" | "draw";

async function playGame(dev: Device, t0: number, humanMove: (m: Mirror) => number): Promise<{ t: number; outcome: Outcome; moves: number }> {
    const mirror = newMirror();
    let t = t0;
    let outcome: Outcome | null = null;
    let moves = 0;
    for (; moves < 50 && !outcome; moves++) {
        const placed = mirror.cells.filter((v) => v !== 0).length;
        const turn = placed % 2 === 0 ? P_RED : P_BLUE;
        dev.drainLog();
        if (turn === P_RED) {
            const col = humanMove(mirror);
            if (col < 0) break;
            t = playMove(dev, col, t);
        } else {
            t = settleCpuTurn(dev, t);
        }
        for (const l of dev.drainLog()) {
            const m = l.match(/four: drop col=(\d+) row=(\d+) player=(\d+)/);
            if (m) mirrorApply(mirror, Number(m[1]), Number(m[2]), Number(m[3]));
            if (l.includes("four: win player=1")) outcome = "human";
            else if (l.includes("four: win player=2")) outcome = "cpu";
            else if (l.includes("four: draw")) outcome = "draw";
        }
    }
    // Skip any celebration, wait out the drain, land back at the choice
    // screen ready for the next game (section 8's own path, not worked
    // around).
    t = settle(dev, t, CELEBRATE_SKIP_MS + 100);
    t = holdLand(dev, colX(3), THUMB_LY, t, 60);
    t = settle(dev, t, 1400);
    return { t, outcome: outcome ?? "draw", moves };
}

async function runModel(name: string, humanMove: (m: Mirror) => number, n: number) {
    const dev = await loadDevice();
    let t = chooseVsCpu(dev, 1000);
    let humanWins = 0, cpuWins = 0, draws = 0, totalMoves = 0;
    const start = performance.now();
    for (let g = 0; g < n; g++) {
        const res = await playGame(dev, t, humanMove);
        t = res.t;
        totalMoves += res.moves;
        if (res.outcome === "human") humanWins++;
        else if (res.outcome === "cpu") cpuWins++;
        else draws++;
        t = chooseVsCpu(dev, t);
    }
    const elapsedS = (performance.now() - start) / 1000;
    const childWinPct = (humanWins / n) * 100;
    const cpuWinPct = (cpuWins / n) * 100;
    const drawPct = (draws / n) * 100;
    console.log(`\n=== ${name}, ${n} games (${elapsedS.toFixed(1)}s, ${(n / elapsedS).toFixed(1)} games/s) ===`);
    console.log(`    child (naive) wins: ${humanWins}/${n} (${childWinPct.toFixed(1)}%)`);
    console.log(`    cpu wins:           ${cpuWins}/${n} (${cpuWinPct.toFixed(1)}%)`);
    console.log(`    draws:               ${draws}/${n} (${drawPct.toFixed(1)}%)`);
    console.log(`    average game length: ${(totalMoves / n).toFixed(1)} moves`);
    return { childWinPct, cpuWinPct, drawPct };
}

async function main() {
    console.log(`=== Connect Four cpu opponent: measured win rate against a naive player model ===`);
    console.log(`(firmware/apps/four.c, section 9 - ${GAMES_PER_MODEL} games per model)`);

    const random = await runModel("RANDOM (uniform among legal columns, no strategy at all)", moveRandom, GAMES_PER_MODEL);
    const nudged = await runModel("NUDGED (as RANDOM, but takes an obvious win half the time)", moveNudged, GAMES_PER_MODEL);

    console.log(`\n=== summary ===`);
    console.log(`    RANDOM child-side win rate: ${random.childWinPct.toFixed(1)}%`);
    console.log(`    NUDGED child-side win rate: ${nudged.childWinPct.toFixed(1)}%`);
    console.log(`\nCopy these two numbers into four.c's header (section 9) if they moved since it was last written.`);
}

main();
