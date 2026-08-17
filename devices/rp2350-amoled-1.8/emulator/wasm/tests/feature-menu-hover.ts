// feature-menu-hover: the menu's GRID, and the press-drag-release gesture
// over it (firmware/apps/menu.c), against the REAL firmware compiled to
// wasm.
//
// Run with (after `bun run emulator/wasm/build.ts`):
//
//   bun run emulator/wasm/tests/feature-menu-hover.ts
//
// REWRITTEN 2026-08-15, when the menu stopped being a row of full-height
// columns. It is worth saying what the old version of this file asserted,
// because it passed on a layout that was about to break: it checked that a
// thumb lit "the column it is over", derived the column from LAND_W/n, and
// slid horizontally. Every one of those statements stays TRUE at six apps
// and at twelve - a 37px column is still a column, and a thumb still lights
// the one it is over. What the file never asked was whether the thing it was
// lighting was big enough for the finger of the person this device is for,
// so it would have gone on passing all the way down to half a fingertip.
// This version asks that first (section 1), because it is the reason the
// layout changed at all. Rewritten, not loosened: everything the old file
// proved is still proved below, in two dimensions instead of one.
//
// WHAT THIS FILE PROVES:
//   1. EVERY TARGET IS BIGGER THAN THE FINGER IT IS FOR. Each app's cell is
//      probed at its four corners through the real hit test, and no cell is
//      smaller than 112 x 112 - one and a half of a child's ~75px fingertip
//      (AGENTS.md) - at whatever app count this firmware declares. The
//      cells also partition the launch area: no gap between them a touch can
//      fall into and hit nothing.
//   2. a thumb on the glass LIGHTS the cell it is over and no other, and
//      moving it moves the light - ACROSS a row and DOWN a column, since a
//      grid has a second axis the old row of columns did not;
//   3. releasing over a lit cell launches that app - and nothing launches
//      while the thumb is still down;
//   4. releasing in the dead band below the grid CANCELS: no app launched,
//      the menu still there, and the screen byte-identical to how it was
//      found (a cancel must leave no trace, or the menu is a worse place to
//      change your mind than it was before);
//   5. the same gesture under the MEASURED hardware profile - 34 dropout
//      episodes/sec AND the position jitter that puts a still finger's
//      reported centroid 80-250px away for up to three reports - still
//      launches the cell the thumb finished on, and a thumb held still does
//      NOT launch by itself. The jitter half is new here and it is the whole
//      reason menu.c confirms a hover before moving the halo: 80px is most
//      of a cell, and on a grid it can throw the report a row down as easily
//      as a column sideways.
//   6. WHAT LAUNCHES IS WHAT WAS LIT. Under that same profile, the app that
//      launches is the one the framebuffer showed lit at the moment of the
//      lift, on every trial. This is the invariant a child is actually told
//      ("let go when the one you want is lit") and it is checked against
//      pixels, not against menu.c's own idea of its state.
//   7. every tick of all of it obeys decision 0001's 8px rule and the "no
//      pixel changes outside the pushed rectangle" invariant.
//
// The menu is the one screen where a wrong launch is most expensive: it is
// the only way into an app, so a gesture that fires early takes the child
// somewhere she did not ask for, and the only way back is a two-button chord
// she has to be taught.
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { TouchSim, type TouchReport } from "../../src/touchsim";
// TOUCHSIM_HARDWARE_MEASURED, not TOUCHSIM_DEFAULTS: the defaults are what a
// controller is imagined doing (2 dropouts/sec, no position jitter at all),
// and this one is what the FT3168 on this board was measured doing. The
// previous version of this file imported the defaults and raised the dropout
// rate by hand, which left positionJitterEnabled false - so the defect that
// most threatens a grid of 112px cells was the one thing it did not simulate.
import { TOUCHSIM_HARDWARE_MEASURED } from "../../src/constants";

const WASM_PATH = join(import.meta.dir, "..", "dist", "emu.wasm");
const PANEL_W = 368;
const PANEL_H = 448;
const LAND_W = PANEL_H; // 448
const LAND_H = PANEL_W; // 368
const APP_INDEX_MENU = -1;

// menu.c's own constants.
const ICON_H = 96;
const MENU_CELL_H = 112;
const MENU_COLS_MAX = 4;
const MENU_ROWS_MAX = Math.floor(LAND_H / MENU_CELL_H); // 3
const MENU_RELEASE_GRACE_MS = 300;
const MENU_HOVER_CONFIRM_MS = 150;

// AGENTS.md's finger table: this is the number the whole layout exists to
// clear, so it is spelled out rather than folded into a threshold.
const CHILD_FINGERTIP_PX = 75;

let passCount = 0;
let failCount = 0;
function check(label: string, ok: boolean, detail?: string) {
    const status = ok ? "PASS" : "FAIL";
    if (ok) passCount++; else failCount++;
    console.log(`[${status}] ${label}${detail ? " - " + detail : ""}`);
}

type Violation = { kind: string; detail: string };
const violations: Violation[] = [];
let ticksChecked = 0;

const compiled = await WebAssembly.compile(readFileSync(WASM_PATH));

async function loadDevice() {
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

    function fbSnapshot(): Uint8Array {
        return new Uint8Array(memory.buffer, exp.emu_fb(), PANEL_W * PANEL_H * 2).slice();
    }
    function pushRects() {
        const n = exp.emu_push_count();
        const out: { x: number; y: number; w: number; h: number }[] = [];
        for (let i = 0; i < n; i++) out.push({ x: exp.emu_push_x(i), y: exp.emu_push_y(i), w: exp.emu_push_w(i), h: exp.emu_push_h(i) });
        return out;
    }

    const dev = {
        // The MENU's own roster (docs/decisions/0019), not g_appCount /
        // emu_device()'s "apps" array: the two used to be the same number,
        // and since the roster cut are not any more (the grid this file
        // tests shows five slots, not every app g_apps[] carries). A slot
        // launches g_apps[menuAppIndex(slot)], not g_apps[slot] - the launch
        // checks below read that back rather than assuming slot === app
        // index the way the pre-0019 version of this file could.
        menuAppCount(): number { return exp.emu_menu_app_count(); },
        menuAppIndex(slot: number): number { return exp.emu_menu_app_index(slot); },
        // The full app table's own names (g_apps[]/g_appCount, via
        // emu_device()'s "apps" array) - used below only to name what a menu
        // slot resolves to, never for n itself (see menuAppCount() above).
        appNames(): string[] {
            const ptr = exp.emu_device();
            const b = new Uint8Array(memory.buffer, ptr);
            let end = 0;
            while (b[end] !== 0) end++;
            return (JSON.parse(decoder.decode(b.subarray(0, end))).apps ?? []) as string[];
        },
        appCurrent(): number { return exp.emu_app_current(); },
        openMenu(nowMs: number) { exp.emu_app_switch(APP_INDEX_MENU); exp.emu_tick(nowMs); },

        // Diffed 32 bits at a time; only differing words are resolved to
        // coordinates. Same technique feature-four.ts uses, for the same
        // reason: this app animates a halo across cells, so "nothing
        // changed" is the exception rather than the rule.
        tickChecked(nowMs: number) {
            ticksChecked++;
            const before = fbSnapshot();
            exp.emu_tick(nowMs);
            const after = fbSnapshot();
            const rects = pushRects();
            for (const r of rects) {
                if (r.w % 8 !== 0) violations.push({ kind: "8px-rule", detail: `t=${nowMs} rect w=${r.w}` });
            }
            const b32 = new Uint32Array(before.buffer, before.byteOffset, before.byteLength >> 2);
            const a32 = new Uint32Array(after.buffer, after.byteOffset, after.byteLength >> 2);
            for (let w = 0; w < b32.length; w++) {
                if (b32[w] === a32[w]) continue;
                for (let half = 0; half < 2; half++) {
                    const idx = w * 4 + half * 2;
                    if (before[idx] === after[idx] && before[idx + 1] === after[idx + 1]) continue;
                    const pixel = idx >> 1;
                    const py = (pixel / PANEL_W) | 0;
                    const px = pixel - py * PANEL_W;
                    if (!rects.some((r) => px >= r.x && px < r.x + r.w && py >= r.y && py < r.y + r.h)) {
                        violations.push({ kind: "outside-push", detail: `t=${nowMs} pixel (${px},${py})` });
                    }
                }
            }
        },
        // Landscape in, panel out: gfx.h's (lx,ly) -> (PANEL_W-1-ly, lx).
        touchLand(down: boolean, lx: number, ly: number, nowMs: number) {
            exp.emu_touch(down ? 1 : 0, PANEL_W - 1 - Math.round(ly), Math.round(lx));
            this.tickChecked(nowMs);
        },
        feedRaw(report: TouchReport, nowMs: number) {
            exp.emu_touch(report.fingers === 1 ? 1 : 0, report.x, report.y);
            exp.emu_tick(nowMs);
        },
        fbSnapshot,
        drainLog(): string[] { const out = fwLog.slice(); fwLog.length = 0; return out; },
        log(): readonly string[] { return fwLog; },
    };
    return dev;
}
type Device = Awaited<ReturnType<typeof loadDevice>>;

/* ---- menu.c's layout, re-derived --------------------------------------
 *
 * This is UPSTREAM of the code under test and therefore proves nothing on
 * its own (docs/decisions/0010): it is a claim about where the cells are,
 * and section 1 below checks that claim against the firmware's own hit test
 * by probing corners rather than trusting it. That is the same trap the
 * previous version of this file fell into from the other side - it modelled
 * a layout menu.c no longer had and kept passing (see repro-switch-input.ts
 * for the write-up).
 */
function menuRows(n: number): number {
    return Math.min(Math.max(Math.ceil(n / MENU_COLS_MAX), 1), MENU_ROWS_MAX);
}
function rowSpan(n: number, r: number): { first: number; cols: number } {
    const rows = menuRows(n);
    const base = Math.floor(n / rows);
    const extra = n % rows;
    return { first: r * base + Math.min(r, extra), cols: base + (r < extra ? 1 : 0) };
}
type Rect = { bx: number; by: number; bw: number; bh: number };
function cellRect(n: number, i: number): Rect {
    const rows = menuRows(n);
    for (let r = 0; r < rows; r++) {
        const { first, cols } = rowSpan(n, r);
        if (i < first + cols) {
            const c = i - first;
            const w = Math.floor(LAND_W / cols);
            const bx = c * w;
            return { bx, by: r * MENU_CELL_H, bw: c === cols - 1 ? LAND_W - bx : w, bh: MENU_CELL_H };
        }
    }
    throw new Error(`no cell for index ${i} at ${n} apps`);
}
const cellCentre = (n: number, i: number): [number, number] => {
    const { bx, by, bw, bh } = cellRect(n, i);
    return [bx + (bw >> 1), by + (bh >> 1)];
};

function landPx(fb: Uint8Array, lx: number, ly: number): [number, number] {
    const idx = (Math.round(lx) * PANEL_W + (PANEL_W - 1 - Math.round(ly))) * 2;
    return [fb[idx]!, fb[idx + 1]!];
}
const isWhite = (p: [number, number]) => p[0] === 0xff && p[1] === 0xff;

// A point inside a cell's halo but outside the icon's own ink, so it is
// white when the cell is not lit and grey when it is. The halo is a disc of
// radius min(bw,bh)/2 - 3 centred on the cell; the icon box is 96 wide, so a
// point on the horizontal midline just past the icon's edge is inside the
// disc and clear of the ink at every cell size this layout produces.
function haloProbe(n: number, i: number): [number, number] {
    const { bx, by, bw, bh } = cellRect(n, i);
    return [bx + (bw >> 1) + ICON_H / 2 - 4, by + (bh >> 1)];
}

// WHICH cell the firmware currently has lit, read from the framebuffer -
// never from a variable this test keeps. -1 for none. Returns -2 if more
// than one is lit, which is a failure with its own name rather than a
// silently-picked first match.
function litCell(dev: Device, n: number): number {
    const fb = dev.fbSnapshot();
    let found = -1;
    for (let i = 0; i < n; i++) {
        if (!isWhite(landPx(fb, ...haloProbe(n, i)))) {
            if (found >= 0) return -2;
            found = i;
        }
    }
    return found;
}

function hold(dev: Device, lx: number, ly: number, t0: number, ms: number, step = 15): number {
    let t = t0;
    for (let held = 0; held < ms; held += step) { t += step; dev.touchLand(true, lx, ly, t); }
    return t;
}
function release(dev: Device, t0: number, ms: number, step = 15): number {
    let t = t0;
    for (let waited = 0; waited < ms; waited += step) { t += step; dev.touchLand(false, 0, 0, t); }
    return t;
}
// Long enough to arm (MENU_ARM_SAMPLES over MENU_ARM_MS) and then to confirm
// the hover (MENU_HOVER_CONFIRM_MS), with margin. Every hold below uses it,
// so the numbers are in one place if menu.c's ever move.
const SETTLE_MS = 250;

async function main() {
    console.log("=== feature: the menu's grid, and its press-drag-release gesture ===\n");
    const dev = await loadDevice();
    dev.tickChecked(0);
    dev.openMenu(100);
    dev.tickChecked(200);
    check("the menu is open", dev.appCurrent() === APP_INDEX_MENU, `app_current()=${dev.appCurrent()}`);

    const n = dev.menuAppCount();
    const rows = menuRows(n);
    const gridH = rows * MENU_CELL_H;
    console.log(`    ${n} apps, ${rows} row(s), cancel band ${LAND_H - gridH}px`);
    for (let r = 0; r < rows; r++) {
        const { first, cols } = rowSpan(n, r);
        const { bw, bh } = cellRect(n, first);
        console.log(`      row ${r}: ${cols} cell(s) of ${bw} x ${bh} px` +
            ` = ${(bw / CHILD_FINGERTIP_PX).toFixed(1)} x ${(bh / CHILD_FINGERTIP_PX).toFixed(1)} child fingertips`);
    }

    // THE ROSTER ITSELF (docs/decisions/0019). Everything above and below
    // this file was already true about a grid of N slots before the roster
    // existed; this is the one check that is actually about the cut - that
    // the five slots resolve, in order, to exactly the apps the owner kept,
    // read by NAME off the real compiled g_apps[] table (appNames(), the
    // full roster) through the menu's own mapping (menuAppIndex()), not
    // assumed from either list on its own.
    const EXPECTED_MENU_APPS = ["chrono", "draw", "timer", "four", "TABLES"];
    const names = dev.appNames();
    const menuNames = Array.from({ length: n }, (_, i) => names[dev.menuAppIndex(i)]);
    check("the menu shows exactly the five apps the owner kept, in this order",
        JSON.stringify(menuNames) === JSON.stringify(EXPECTED_MENU_APPS),
        `menu = [${menuNames.join(", ")}]`);

    let t = 200;

    /* ---- 1. every target is bigger than the finger it is for ---------- */
    console.log("\n-- every cell, probed at its four corners through the real hit test --");

    // The claim, from the layout above.
    let smallestW = Infinity, smallestH = Infinity;
    for (let i = 0; i < n; i++) {
        const { bw, bh } = cellRect(n, i);
        smallestW = Math.min(smallestW, bw);
        smallestH = Math.min(smallestH, bh);
    }
    check(`no target is narrower than a child's fingertip (${CHILD_FINGERTIP_PX}px)`,
        smallestW >= CHILD_FINGERTIP_PX && smallestH >= CHILD_FINGERTIP_PX,
        `smallest cell ${smallestW} x ${smallestH} px`);
    check("and no target is under 112px in either direction, which is the size menu.c promises never to go below",
        smallestW >= 112 && smallestH >= 112, `smallest cell ${smallestW} x ${smallestH} px`);

    // The check of the claim: a thumb 2px inside each corner of the cell the
    // layout says belongs to app i must light app i, or the cell is not
    // actually that big. This is what makes the size figure above mean
    // something rather than repeat itself.
    let cornersWrong = 0;
    let cornersProbed = 0;
    for (let i = 0; i < n; i++) {
        const { bx, by, bw, bh } = cellRect(n, i);
        const corners: [number, number][] = [
            [bx + 2, by + 2], [bx + bw - 3, by + 2],
            [bx + 2, by + bh - 3], [bx + bw - 3, by + bh - 3],
        ];
        for (const [lx, ly] of corners) {
            cornersProbed++;
            t = hold(dev, lx, ly, t, SETTLE_MS);
            const lit = litCell(dev, n);
            if (lit !== i) {
                cornersWrong++;
                if (cornersWrong <= 4) console.log(`      corner (${lx},${ly}) of cell ${i} lit ${lit}`);
            }
            // Let go without launching: back into the cancel band, then lift.
            t = hold(dev, 8, LAND_H - 4, t, SETTLE_MS);
            t = release(dev, t, MENU_RELEASE_GRACE_MS + 100);
        }
    }
    check("all four corners of every cell light that cell, so each target really is the size claimed",
        cornersWrong === 0, `${cornersProbed - cornersWrong}/${cornersProbed} corners correct`);
    check("and none of that launched anything - a drag into the band and a lift is a cancel",
        dev.appCurrent() === APP_INDEX_MENU, `app_current()=${dev.appCurrent()}`);

    // No gap between cells: every landscape x, sampled along each row's own
    // midline, belongs to SOME cell. A dead pixel between two targets is the
    // defect the old contiguous-column layout was careful to avoid, and a
    // grid has twice as many boundaries to get wrong.
    const covered = new Set<number>();
    for (let r = 0; r < rows; r++) {
        const { first, cols } = rowSpan(n, r);
        for (let c = 0; c < cols; c++) {
            const { bx, bw } = cellRect(n, first + c);
            for (let x = bx; x < bx + bw; x++) covered.add(r * LAND_W + x);
        }
    }
    check("the cells tile every row edge to edge: no dead pixel between two targets",
        covered.size === rows * LAND_W, `${covered.size} of ${rows * LAND_W} covered`);

    /* ---- 2. the thumb lights its cell, and only its cell -------------- */
    console.log("\n-- a thumb goes down on cell 1 --");
    t = hold(dev, ...cellCentre(n, 1), t, SETTLE_MS);
    check("the cell under the thumb is lit, and no other cell is", litCell(dev, n) === 1,
        `lit=${litCell(dev, n)}`);
    check("nothing has launched while the thumb is still down", dev.appCurrent() === APP_INDEX_MENU,
        `app_current()=${dev.appCurrent()}`);

    // ACROSS a row, which is all the old row-of-columns layout could do.
    const acrossTo = rowSpan(n, 0).cols - 1;
    console.log(`\n-- it slides across row 0, to cell ${acrossTo} --`);
    for (let i = 2; i <= acrossTo; i++) t = hold(dev, ...cellCentre(n, i), t, SETTLE_MS);
    check("the light followed it across, and the cell it left is plain again",
        litCell(dev, n) === acrossTo, `lit=${litCell(dev, n)}`);

    // DOWN a column: the axis a grid has and a row of columns does not, so
    // this is the assertion that would have been meaningless before.
    if (rows > 1) {
        const down = rowSpan(n, 1).first;
        console.log(`\n-- and DOWN into row 1, to cell ${down} --`);
        t = hold(dev, ...cellCentre(n, down), t, SETTLE_MS);
        check("the light followed it down a row too", litCell(dev, n) === down, `lit=${litCell(dev, n)}`);
    } else {
        console.log("\n-- (one row at this app count: the vertical slide has nowhere to go) --");
    }

    /* ---- 3. release launches ------------------------------------------ */
    const target = litCell(dev, n);
    const targetApp = dev.menuAppIndex(target);
    console.log(`\n-- it lifts over cell ${target} (app ${targetApp}) --`);
    dev.drainLog();
    t = release(dev, t, MENU_RELEASE_GRACE_MS + 200);
    const launch = dev.log().find((l) => l.includes("menu: launch"));
    check("releasing over a lit cell launches that app",
        !!launch && launch.includes(`-> app ${targetApp}`) && dev.appCurrent() === targetApp,
        `${launch?.trim() ?? "(no launch line)"}, app_current()=${dev.appCurrent()}`);

    /* ---- 4. cancel ---------------------------------------------------- */
    console.log("\n-- back in the menu: drag down into the dead band below the grid and let go --");
    const dev2 = await loadDevice();
    dev2.tickChecked(0);
    dev2.openMenu(100);
    dev2.tickChecked(200);
    let t2 = 200;
    const before = dev2.fbSnapshot();
    t2 = hold(dev2, ...cellCentre(n, 0), t2, SETTLE_MS);
    const litDuring = litCell(dev2, n) === 0;
    // ...then below the grid, where nothing is lit.
    t2 = hold(dev2, cellCentre(n, 0)[0], LAND_H - 4, t2, SETTLE_MS);
    const unlitInBand = litCell(dev2, n) === -1;
    check("dragging below the grid puts the light out - the screen says a release here does nothing",
        litDuring && unlitInBand, `lit over the icon=${litDuring}, unlit in the band=${unlitInBand}`);

    dev2.drainLog();
    t2 = release(dev2, t2, MENU_RELEASE_GRACE_MS + 200);
    check("releasing there launches nothing and leaves the menu open",
        dev2.appCurrent() === APP_INDEX_MENU && !dev2.log().some((l) => l.includes("menu: launch")),
        `app_current()=${dev2.appCurrent()}`);
    const after = dev2.fbSnapshot();
    let identical = before.length === after.length;
    for (let i = 0; identical && i < before.length; i++) if (before[i] !== after[i]) identical = false;
    check("and it leaves NO trace - the menu is byte-identical to how it was found", identical);

    /* ---- 5 and 6. the measured hardware profile ------------------------ */
    // TOUCHSIM_HARDWARE_MEASURED, not a hand-rolled profile: 34 dropout
    // episodes/sec AND the position jitter that throws a still finger's
    // reported centroid 80-250px away for one to three reports. The previous
    // version of this file used TOUCHSIM_DEFAULTS with the dropout rate
    // raised by hand, which left positionJitterEnabled false - so the defect
    // that most threatens a grid was the one thing it did not simulate.
    console.log("\n-- the same gesture under the MEASURED profile (34 dropouts/sec + 80-250px jitter) --");
    const profile = TOUCHSIM_HARDWARE_MEASURED;
    const STEP = 1000 / profile.reportRateHz;
    const TRIALS = 20;
    let correct = 0, early = 0, missed = 0, wrong = 0, disagreed = 0;
    // THE MEASUREMENT THAT ACTUALLY SEES THE JITTER, and it took a
    // counterfactual to find that the obvious one does not. A slide that ends
    // with the thumb settling before it lifts launches the right app with or
    // without menu.c's hover confirmation, because the truthful reports after
    // the last jitter episode have time to put the halo back. Built with
    // MENU_HOVER_CONFIRM_MS forced to 0, the launch counters below still read
    // 20/20 - decision 0010's own lesson arriving on this file: an
    // instrument that reads at the END of a gesture cannot see a defect that
    // happens in the middle of it.
    //
    // So the halo is sampled at every report, and an EXCURSION is counted
    // whenever it MOVES to a cell the thumb has not been over at any point in
    // this gesture. Events, not samples: one bad episode holds the halo in
    // the wrong place for as long as the dropouts last.
    //
    // THE BAR IS A RATE AND NOT ZERO, and menu.c's own comment says why: the
    // runtime bridges dropped contact by holding the last reported position,
    // so a wrong position outlives the episode that produced it and no finite
    // window makes this impossible. Measured over 240 gestures per setting:
    // 14 per 100 with the confirmation off, 4 per 100 at 100ms, 1 per 240 at
    // the 150ms menu.c ships. At that rate the expected count over the 20
    // gestures below is 0.08, so "at most one" flakes about three runs in a
    // thousand while still failing loudly at 100ms (expected 0.8, over one in
    // five runs) and instantly at 0. Same shape of threshold, and same
    // reasoning, as repro-touch-dropout-four-drop.ts's own hold-rate gate.
    let excursions = 0;
    let haloSamples = 0;
    let gestures = 0;
    for (let trial = 0; trial < TRIALS; trial++) {
        const d = await loadDevice();
        d.tickChecked(0);
        d.openMenu(100);
        d.tickChecked(200);
        const sim = new TouchSim(profile, PANEL_W, PANEL_H);
        const from = trial % n, to = (trial * 5 + 1) % n;
        let tt = 1000;
        const visited = new Set<number>();
        const put = (lx: number, ly: number) => {
            // Where the FINGER is, not where the controller says it is.
            const rows2 = menuRows(n);
            if (ly < rows2 * MENU_CELL_H) {
                const r = Math.floor(ly / MENU_CELL_H);
                const { first, cols } = rowSpan(n, r);
                visited.add(first + Math.min(cols - 1, Math.floor(lx / Math.floor(LAND_W / cols))));
            }
            sim.setPointer(true, PANEL_W - 1 - Math.round(ly), Math.round(lx));
        };
        // Counted as EVENTS, not as samples: one bad episode holds the halo
        // in the wrong place for as long as the dropouts last, and counting
        // every sample of it would make one excursion look like fifteen.
        let prevLit = -1;
        const sampleHalo = (d2: Device) => {
            haloSamples++;
            const lit = litCell(d2, n);
            if (lit !== prevLit && lit >= 0 && !visited.has(lit)) {
                excursions++;
                if (excursions <= 5) console.log(`      trial ${trial}: halo moved to cell ${lit}, which the thumb never touched`);
            }
            prevLit = lit;
        };
        gestures++;
        const [fx, fy] = cellCentre(n, from);
        const [tx, ty] = cellCentre(n, to);
        for (let e = 0; e < 700; e += STEP) {
            const u = e / 700;
            put(fx + (tx - fx) * u, fy + (ty - fy) * u);
            tt += STEP;
            d.feedRaw(sim.poll(tt), tt);
            sampleHalo(d);
        }
        // Arrive and settle, which is what a child does: slide until the one
        // you want is lit, THEN let go.
        put(tx, ty);
        for (let e = 0; e < 400; e += STEP) { tt += STEP; d.feedRaw(sim.poll(tt), tt); sampleHalo(d); }
        if (d.appCurrent() !== APP_INDEX_MENU) { early++; continue; }

        // WHAT THE SCREEN SAID at the instant of the lift, read from pixels -
        // a SLOT (or -1/-2), same numbering as `from`/`to` above. Mapped to
        // an app index below only when it names a real slot, so a launch
        // verdict is never compared against a mapping of "nothing lit" or
        // "two cells lit".
        const litAtLift = litCell(d, n);
        const litAtLiftApp = litAtLift >= 0 ? d.menuAppIndex(litAtLift) : litAtLift;

        sim.setPointer(false, 0, 0);
        for (let e = 0; e < MENU_RELEASE_GRACE_MS + 400; e += STEP) { tt += STEP; d.feedRaw(sim.poll(tt), tt); }
        const got = d.appCurrent();
        const toApp = d.menuAppIndex(to);
        if (got === APP_INDEX_MENU) missed++;
        else if (got === toApp) correct++;
        else wrong++;
        if (got !== litAtLiftApp) {
            disagreed++;
            if (disagreed <= 3) console.log(`      trial ${trial}: lit slot ${litAtLift} (app ${litAtLiftApp}) at the lift, launched app ${got}`);
        }
    }
    console.log(`    ${correct}/${TRIALS} launched the cell the thumb finished on; ${early} launched early, ${missed} never launched, ${wrong} launched the wrong app`);
    console.log(`    ${excursions} halo excursion(s) to a cell the thumb never touched, over ${gestures} gestures (${haloSamples} samples)`);
    check("a slide finished by a genuine lift launches the right app under the measured profile",
        correct >= TRIALS - 1, `${correct}/${TRIALS}`);
    check("and none launched while the thumb was still down", early === 0, `${early} early`);
    check("WHAT LAUNCHED IS WHAT WAS LIT, on every trial: the screen and the verdict never disagreed",
        disagreed === 0, `${TRIALS - disagreed}/${TRIALS} agreed`);
    check("80-250px of jitter almost never moves the halo to a cell the thumb was not on",
        excursions <= 1, `${excursions} excursion(s) over ${gestures} gestures`);

    // A thumb held still is the case a dropout storm most looks like a lift,
    // and the case the jitter profile was calibrated on.
    console.log("");
    let heldQuiet = 0;
    const HOLD_TRIALS = 15;
    for (let trial = 0; trial < HOLD_TRIALS; trial++) {
        const d = await loadDevice();
        d.tickChecked(0);
        d.openMenu(100);
        d.tickChecked(200);
        const sim = new TouchSim(profile, PANEL_W, PANEL_H);
        const [hx, hy] = cellCentre(n, trial % n);
        sim.setPointer(true, PANEL_W - 1 - Math.round(hy), Math.round(hx));
        let tt = 1000;
        let launched = false;
        for (let e = 0; e < 3000; e += STEP) {
            tt += STEP;
            d.feedRaw(sim.poll(tt), tt);
            if (d.appCurrent() !== APP_INDEX_MENU) { launched = true; break; }
        }
        if (!launched) heldQuiet++;
    }
    const holdRate = (heldQuiet / HOLD_TRIALS) * 100;
    console.log(`    ${heldQuiet}/${HOLD_TRIALS} held for 3 seconds without launching (${holdRate.toFixed(0)}%)`);
    // Same gate, same reasoning, as repro-touch-dropout-four-drop.ts: this is
    // a Bernoulli process and no grace makes it impossible, only rare. A
    // working release verdict sits near 100%, one that believes the runtime's
    // own touchReleased sits near 0%.
    check("a thumb held on the glass for 3 seconds does not launch by itself", holdRate >= 93,
        `${heldQuiet}/${HOLD_TRIALS}`);

    /* ---- 7. invariants ------------------------------------------------- */
    console.log("\n=== invariants across every checked tick ===");
    console.log(`    ${ticksChecked} ticks checked`);
    const byKind = new Map<string, number>();
    for (const v of violations) byKind.set(v.kind, (byKind.get(v.kind) ?? 0) + 1);
    check("every pushed window's row length was a multiple of 8 (decision 0001)",
        (byKind.get("8px-rule") ?? 0) === 0, `${byKind.get("8px-rule") ?? 0} violation(s)`);
    check("no framebuffer pixel changed outside that tick's own pushed rectangles",
        (byKind.get("outside-push") ?? 0) === 0, `${byKind.get("outside-push") ?? 0} violation(s)`);
    for (const v of violations.slice(0, 6)) console.log(`    [${v.kind}] ${v.detail}`);

    console.log(`\n${passCount} passed, ${failCount} failed`);
    if (failCount > 0) process.exit(1);
}

main();
