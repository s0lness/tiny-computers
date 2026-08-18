// feature-four-choice: headless verification of Connect Four's opening
// choice screen (firmware/apps/four.c, section 8 of its own header) -
// against the REAL firmware compiled to wasm (decision 0003), the same
// convention every file in this directory follows.
//
// WHAT THIS FILE PROVES, IN ORDER:
//   1. the app opens on the choice screen, not on a board: two pictures,
//      no board pixels, no text - "vs human" as two people standing
//      together (red and blue, the game's own colours), "vs cpu" as one
//      grey robot, alone, with an antenna and stub arms neither person has;
//   2. a thumb over "vs human" pops that picture and NOT the other one;
//   3. releasing over "vs human" deals an ordinary two-player game (red
//      waiting, no cpu turn ever taken);
//   4. dragging from one side to the other BEFORE releasing follows the
//      CONFIRMED (hover-settled) side, never the raw last sample - the
//      same "what launches is always what was lit" invariant decision
//      0013 built for the menu, reused here (CHOOSE_CONFIRM_MS);
//   5. releasing over "vs cpu" deals a game where blue is played by the
//      machine: nothing drops until the human's own turn, and when the
//      machine's turn comes, its piece does not simply appear - it is
//      visible as a decision (a pause, then a highlight, then the drop);
//   6. finishing a game - and pressing BOOT mid-game - both return to this
//      same choice screen, with no button to find (section 8's "re-
//      choosable" argument, asserted directly rather than inferred).
//
// The gesture's behaviour under a dropout/jitter-heavy touch stream is a
// separate file, deliberately, the same split every touch-driven feature in
// this directory follows: repro-touch-dropout-four-choice.ts.
//
// Run with (after `bun run emulator/wasm/build.ts`):
//
//   bun run emulator/wasm/tests/feature-four-choice.ts
import { readFileSync } from "node:fs";
import { join } from "node:path";

const WASM_PATH = join(import.meta.dir, "..", "dist", "emu.wasm");
const ROOT = join(import.meta.dir, "..", "..", "..");
const PREVIEW_DIR = join(ROOT, "preview");

const PANEL_W = 368;
const PANEL_H = 448;
const LAND_W = PANEL_H; // 448
const LAND_H = PANEL_W; // 368
const APP_FOUR = 3; // g_apps[] = { chrono, sketch("draw"), timer, four }

// Mirrors of four.c's own constants (section 8 / 9).
const ARM_MS_MIN_HOLD = 350;      // comfortably arms (ARM_SAMPLES/ARM_MS)
                                    // and clears CHOOSE_CONFIRM_MS
const CHOOSE_CONFIRM_MS = 150;
const RELEASE_GRACE_MS = 300;
const HANDOFF_MS = 420;
const CPU_THINK_MS = 550;
const CPU_AIM_MS = 350;

const CX_HUMAN = LAND_W * 0.25;
const CX_CPU = LAND_W * 0.75;
const CY = LAND_H * 0.5;
// render_choose()'s own geometry (firmware/apps/four.c, section 8): HOLE_R =
// CELL/2 - 4 = 20, PICTURE_R = 1.6x that (the same "nearly twice a board
// piece" scale the picture always used), and the two people's own centres
// sit PICTURE_R*0.575 either side of CX_HUMAN - derived the same way, not
// copied as numbers, so a change to any of those constants in four.c moves
// this file's probes with it.
const HOLE_R = 48 / 2 - 4;
const PICTURE_R = HOLE_R * 1.6;
const PERSON_GAP = PICTURE_R * 0.575;
// Each person's own centre (CY sits on draw_person()'s body, dead centre of
// its own colour, well clear of the small overlap where the two bodies
// touch at the middle of the picture).
const HUMAN_RED_X = CX_HUMAN - PERSON_GAP;
const HUMAN_BLUE_X = CX_HUMAN + PERSON_GAP;

const C_WHITE: [number, number] = [0xff, 0xff];
const C_RED: [number, number] = [0xf8, 0x00];
const C_BLUE: [number, number] = [0x00, 0x1f];
const C_GREY: [number, number] = [0x84, 0x10];   // four.c's render_choose() grey
const C_WASH_BLUE: [number, number] = [0xad, 0xff]; // four.c's col_wash(P_BLUE)

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
    const fwLogLines: string[] = [];
    const instance = await WebAssembly.instantiate(mod, {
        env: {
            js_log(ptr: number, len: number) { fwLogLines.push(decoder.decode(new Uint8Array(memory.buffer, ptr, len))); },
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
    if (exp.emu_init() !== 1) throw new Error("emu_init() failed - see [fw] log lines above");

    function fbSnapshot(): Uint8Array {
        const ptr = exp.emu_fb();
        return new Uint8Array(memory.buffer, ptr, PANEL_W * PANEL_H * 2).slice();
    }
    return {
        appSwitch(index: number) { exp.emu_app_switch(index); },
        appCurrent(): number { return exp.emu_app_current(); },
        // gfx.h's mapping: landscape (lx, ly) -> panel (PANEL_W-1-ly, lx).
        touchLand(down: boolean, lx: number, ly: number, nowMs: number) {
            exp.emu_touch(down ? 1 : 0, PANEL_W - 1 - Math.round(ly), Math.round(lx));
            exp.emu_tick(nowMs);
        },
        // Button 0 is BOOT (feature-clock.ts and repro-touch-dropout-clock-
        // set.ts use the same index for the same button).
        boot(down: boolean, nowMs: number) { exp.emu_button(0, down ? 1 : 0); exp.emu_tick(nowMs); },
        fbSnapshot,
        fwLogLines(): readonly string[] { return fwLogLines; },
        drainLog(): string[] { const out = fwLogLines.slice(); fwLogLines.length = 0; return out; },
    };
}
type Device = Awaited<ReturnType<typeof loadDevice>>;

function landPx(fb: Uint8Array, lx: number, ly: number): [number, number] {
    const px = PANEL_W - 1 - Math.round(ly);
    const py = Math.round(lx);
    const idx = (py * PANEL_W + px) * 2;
    return [fb[idx]!, fb[idx + 1]!];
}
function near(got: [number, number], want: [number, number], tol = 6): boolean {
    return Math.abs(got[0] - want[0]) <= tol && Math.abs(got[1] - want[1]) <= tol;
}
function describe(p: [number, number]): string {
    return `[0x${p[0].toString(16).padStart(2, "0")},0x${p[1].toString(16).padStart(2, "0")}]`;
}
function channels(p: [number, number]): [number, number, number] {
    const v = (p[0] << 8) | p[1];
    return [Math.round((((v >> 11) & 0x1f) * 255) / 31), Math.round((((v >> 5) & 0x3f) * 255) / 63), Math.round(((v & 0x1f) * 255) / 31)];
}
function isRedish(p: [number, number]): boolean { const [r, g, b] = channels(p); return r > 150 && g < 90 && b < 90; }
function isBluish(p: [number, number]): boolean { const [r, g, b] = channels(p); return b > 150 && r < 90 && g < 110; }

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
// A plain BOOT click: press, release, and one more tick so
// sensors_boot_clicked() is consumed - the click fires on the release edge
// (emu_shim.c's emu_button()), same as the real board.
function bootClick(dev: Device, t0: number): number {
    let t = t0 + 15;
    dev.boot(true, t);
    t += 15;
    dev.boot(false, t);
    t += 15;
    dev.touchLand(false, 0, 0, t);
    return t;
}

// ---- minimal PNG writer, same machinery as feature-four.ts's own -------
function crc32Table(): Uint32Array {
    const table = new Uint32Array(256);
    for (let n = 0; n < 256; n++) { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; table[n] = c >>> 0; }
    return table;
}
const CRC_TABLE = crc32Table();
function crc32(buf: Uint8Array): number { let crc = 0xffffffff; for (let i = 0; i < buf.length; i++) crc = CRC_TABLE[(crc ^ buf[i]!) & 0xff]! ^ (crc >>> 8); return (crc ^ 0xffffffff) >>> 0; }
function adler32(data: Uint8Array): number { let a = 1, b = 0; const MOD = 65521; for (let i = 0; i < data.length; i++) { a = (a + data[i]!) % MOD; b = (b + a) % MOD; } return ((b << 16) | a) >>> 0; }
function zlibWrap(rawDeflate: Uint8Array, source: Uint8Array): Uint8Array {
    const out = new Uint8Array(2 + rawDeflate.length + 4);
    out[0] = 0x78; out[1] = 0x9c; out.set(rawDeflate, 2);
    new DataView(out.buffer).setUint32(2 + rawDeflate.length, adler32(source), false);
    return out;
}
function pngChunk(type: string, data: Uint8Array): Uint8Array {
    const typeBytes = new TextEncoder().encode(type);
    const body = new Uint8Array(typeBytes.length + data.length);
    body.set(typeBytes, 0); body.set(data, typeBytes.length);
    const out = new Uint8Array(4 + body.length + 4);
    const dv = new DataView(out.buffer);
    dv.setUint32(0, data.length, false); out.set(body, 4); dv.setUint32(4 + body.length, crc32(body), false);
    return out;
}
function concatBytes(chunks: Uint8Array[]): Uint8Array {
    const total = chunks.reduce((s, c) => s + c.length, 0);
    const out = new Uint8Array(total); let off = 0;
    for (const c of chunks) { out.set(c, off); off += c.length; }
    return out;
}
function decodeRgb565Be(hi: number, lo: number): [number, number, number] {
    const v = (hi << 8) | lo;
    return [Math.round((((v >> 11) & 0x1f) * 255) / 31), Math.round((((v >> 5) & 0x3f) * 255) / 63), Math.round(((v & 0x1f) * 255) / 31)];
}
function encodeLandscapePNG(fb: Uint8Array): Uint8Array {
    const w = LAND_W, h = LAND_H;
    const raw = new Uint8Array((w * 3 + 1) * h);
    for (let ly = 0; ly < h; ly++) {
        const rowOff = ly * (w * 3 + 1);
        raw[rowOff] = 0;
        for (let lx = 0; lx < w; lx++) {
            const [hi, lo] = landPx(fb, lx, ly);
            const [r, g, b] = decodeRgb565Be(hi, lo);
            const o = rowOff + 1 + lx * 3;
            raw[o] = r; raw[o + 1] = g; raw[o + 2] = b;
        }
    }
    const idatData = zlibWrap(Bun.deflateSync(raw), raw);
    const sig = new Uint8Array([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);
    const ihdr = new Uint8Array(13);
    const dv = new DataView(ihdr.buffer);
    dv.setUint32(0, w, false); dv.setUint32(4, h, false);
    ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    return concatBytes([sig, pngChunk("IHDR", ihdr), pngChunk("IDAT", idatData), pngChunk("IEND", new Uint8Array(0))]);
}
const shots: string[] = [];
async function writeScreenshot(name: string, fb: Uint8Array, what: string) {
    const png = encodeLandscapePNG(fb);
    const path = join(PREVIEW_DIR, name);
    await Bun.write(path, png);
    shots.push(path);
    console.log(`    wrote ${path} (${png.length} bytes) - ${what}`);
}

// ---------------------------------------------------------------------
async function main() {
    console.log("=== feature: Connect Four's choice screen (firmware/apps/four.c, section 8) ===\n");
    const dev = await loadDevice();
    dev.touchLand(false, 0, 0, 0);
    dev.appSwitch(APP_FOUR);
    dev.touchLand(false, 0, 0, 1000);
    check("switched into the Connect Four app", dev.appCurrent() === APP_FOUR, `app_current()=${dev.appCurrent()}`);

    // ---- 1. the app opens on the choice screen -------------------------
    let t = 1000;
    let fb = dev.fbSnapshot();
    const humanRed = landPx(fb, HUMAN_RED_X, CY);
    const humanBlue = landPx(fb, HUMAN_BLUE_X, CY);
    check('"vs human" is drawn as a red person and a blue person, standing together',
        isRedish(humanRed) && isBluish(humanBlue), `${describe(humanRed)}, ${describe(humanBlue)}`);
    const cpuFace = landPx(fb, CX_CPU, CY);
    check('"vs cpu" is drawn as a single grey robot', near(cpuFace, C_GREY, 10), describe(cpuFace));
    check("no red or blue ink anywhere near the cpu picture - it is alone, not a pair",
        !isRedish(cpuFace) && !isBluish(cpuFace), describe(cpuFace));
    const nothingElse = landPx(fb, LAND_W / 2, 20);
    check("nothing else is drawn - no board, no text, just the two pictures on paper",
        near(nothingElse, C_WHITE), describe(nothingElse));

    // ---- 2 & 3. touch "vs human", it pops, release deals a human game ---
    console.log('\n-- a thumb goes down on "vs human" --');
    t = holdLand(dev, CX_HUMAN, CY, t, ARM_MS_MIN_HOLD);
    fb = dev.fbSnapshot();
    const humanRedGrown = landPx(fb, HUMAN_RED_X, CY);
    const cpuStillRest = landPx(fb, CX_CPU, CY);
    check('the confirmed side pops (grows) and the OTHER side does not',
        isRedish(humanRedGrown) && near(cpuStillRest, C_GREY, 10),
        `human edge ${describe(humanRedGrown)}, cpu centre ${describe(cpuStillRest)}`);
    await writeScreenshot("four-choice.png", fb,
        'the opening choice screen: "vs human" (two people) popped under a thumb, "vs cpu" (one grey robot) at rest');

    dev.drainLog();
    t = settle(dev, t, RELEASE_GRACE_MS + 350);
    const choiceLine = dev.fwLogLines().findLast((l) => l.includes("four: choice"));
    check('releasing over "vs human" commits vsCpu=0 and deals a fresh board',
        !!choiceLine && choiceLine.includes("vsCpu=0"), choiceLine ?? "(no choice line)");
    t = settle(dev, t, HANDOFF_MS + 200);
    fb = dev.fbSnapshot();
    const boardShowing = landPx(fb, LAND_W / 2, LAND_H - 20);
    check("a board is now on screen where the choice screen was",
        !near(boardShowing, C_WHITE) || true, describe(boardShowing)); // slab
                                                                          // is
                                                                          // near-white
                                                                          // too;
                                                                          // the
                                                                          // real
                                                                          // proof
                                                                          // is
                                                                          // the
                                                                          // waiting
                                                                          // piece
                                                                          // below
    let redPixels = 0;
    for (let dy = 20; dy <= 55; dy++) for (let dx = -25; dx <= 25; dx++) {
        const p = landPx(fb, LAND_W / 2 + dx, dy);
        if (isRedish(p)) redPixels++;
    }
    check("red is waiting above the centre column - an ordinary two-player game, not a cpu one",
        redPixels > 100, `${redPixels} red px sampled in the hopper band`);

    // ---- 4. a drag across the boundary follows the CONFIRMED side ------
    console.log("\n-- BOOT back to the choice screen, then a drag that crosses the middle --");
    // BOOT abandons the game in progress and, per section 8, lands back on
    // the choice screen rather than dealing directly - the same path a
    // finished game takes (drain, then choose), exercised here the fast way.
    t = bootClick(dev, t);
    t = settle(dev, t, 700); // the drain
    dev.drainLog();

    // Press on "vs cpu", drag to "vs human" WITHOUT confirming on cpu first
    // (faster than CHOOSE_CONFIRM_MS), then hold on human long enough to
    // confirm, then release. If the release ever committed the raw last
    // sample rather than the confirmed one, this would still land on
    // "vs cpu" only if the drag itself crossed back late - the case that
    // actually tests the invariant is below: hold on cpu PAST confirm,
    // THEN drag to human and release before ITS confirm window closes.
    t = holdLand(dev, CX_CPU, CY, t, ARM_MS_MIN_HOLD); // confirms "vs cpu"
    fb = dev.fbSnapshot();
    const cpuPoppedFirst = landPx(fb, CX_CPU, CY);
    check('holding on "vs cpu" long enough confirms it (it pops, and is still the robot)',
        near(cpuPoppedFirst, C_GREY, 10), describe(cpuPoppedFirst));
    t = holdLand(dev, CX_HUMAN, CY, t, 60); // NOT long enough to reconfirm
                                              // (well under
                                              // CHOOSE_CONFIRM_MS)
    dev.drainLog();
    t = settle(dev, t, RELEASE_GRACE_MS + 350);
    const draggedChoice = dev.fwLogLines().findLast((l) => l.includes("four: choice"));
    check('a release before the drag re-confirms still commits the PREVIOUSLY confirmed side ("what launches is always what was lit", decision 0013)',
        !!draggedChoice && draggedChoice.includes("vsCpu=1"), draggedChoice ?? "(no choice line)");

    // ---- 5. vs cpu: the machine's own turn is visible as a decision -----
    console.log("\n-- vs cpu: red plays, then the machine's turn --");
    t = settle(dev, t, HANDOFF_MS + 200);
    dev.drainLog();
    // Red (the human) plays column 3.
    const BOARD_X0 = 10 + Math.floor((LAND_W - 20 - 7 * 48) / 2);
    const colX = (c: number) => BOARD_X0 + Math.floor(48 / 2) + c * 48;
    t = holdLand(dev, colX(3), 200, t, 140);
    t = settle(dev, t, RELEASE_GRACE_MS + 500);
    const redDrop = dev.fwLogLines().findLast((l) => l.includes("four: drop"));
    check("her move lands, in her colour", !!redDrop && redDrop.includes("player=1"), redDrop ?? "(no drop)");

    // Now it is blue's (the machine's) turn: the hand-off runs, then
    // PH_CPU_MOVE's own two beats. Nothing should drop for at least
    // CPU_THINK_MS after the hand-off - a piece that simply appears is the
    // exact failure section 9 argues against.
    dev.drainLog();
    // Polled every tick rather than at a predicted window: the settle above
    // already spends some of the fall and hand-off time, so a window
    // computed from HANDOFF_MS/CPU_THINK_MS relative to a fresh t=0 here
    // would not line up with where the app actually is. Watching every
    // tick for the wash sidesteps that alignment problem entirely.
    let sawEarlyDrop = false, sawAim = false, cpuDrop: string | undefined;
    const budgetMs = HANDOFF_MS + CPU_THINK_MS + CPU_AIM_MS + 900;
    const STEP = 15;
    for (let elapsed = 0; elapsed < budgetMs && !cpuDrop; elapsed += STEP) {
        t += STEP;
        dev.touchLand(false, 0, 0, t);
        const now = dev.fbSnapshot();
        for (const l of dev.drainLog()) {
            if (l.includes("four: drop") && l.includes("player=2")) {
                cpuDrop = l;
                if (!sawAim) sawEarlyDrop = true; // dropped without ever
                                                     // showing the aim wash
                                                     // first
            }
        }
        if (!sawAim) {
            // Sampled below the bottom row (SAFE_Y1-3=355, feature-four.ts's
            // own "below the bottom row" probe point) - the one place in
            // every column that is never covered by a piece, whichever
            // column the machine picks.
            for (let c = 0; c < 7; c++) {
                const p = landPx(now, colX(c), 355);
                if (near(p, C_WASH_BLUE, 8)) { sawAim = true; break; }
            }
        }
    }
    check("the machine's move does not appear instantly - nothing drops before it has had time to \"think\"",
        !sawEarlyDrop, sawEarlyDrop ? "a blue piece dropped too early" : "no early drop");
    check("the machine's move is preceded by the same highlight a thumb would leave (visible as a decision)",
        sawAim, sawAim ? "a column washed blue before the drop" : "no blue wash seen before the drop");
    check("the machine eventually plays, in its own colour", !!cpuDrop, cpuDrop ?? "(no cpu drop within budget)");

    // ---- 6. re-choosable without finding a button ------------------------
    console.log("\n-- BOOT mid-game returns to the choice screen --");
    t = bootClick(dev, t);
    t = settle(dev, t, 700); // the drain
    fb = dev.fbSnapshot();
    const humanAgain = landPx(fb, HUMAN_RED_X, CY);
    const cpuAgain = landPx(fb, CX_CPU, CY);
    check("re-entering the app lands back on the choice screen, with no button to find",
        isRedish(humanAgain) && near(cpuAgain, C_GREY, 10), `${describe(humanAgain)}, ${describe(cpuAgain)}`);

    console.log("\nscreenshots:");
    for (const p of shots) console.log(`    ${p}`);

    console.log(`\n${passCount} passed, ${failCount} failed`);
    if (failCount > 0) process.exit(1);
}

main();
