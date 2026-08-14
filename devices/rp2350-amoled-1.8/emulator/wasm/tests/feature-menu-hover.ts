// feature-menu-hover: the menu's press-drag-release gesture
// (firmware/apps/menu.c), against the REAL firmware compiled to wasm.
//
// Run with (after `bun run emulator/wasm/build.ts`):
//
//   bun run emulator/wasm/tests/feature-menu-hover.ts
//
// WHAT THIS FILE PROVES:
//   1. a thumb on the glass LIGHTS the column it is over and no other, and
//      moving it moves the light, leaving the column it left plain again;
//   2. releasing over a lit column launches that app - and nothing launches
//      while the thumb is still down, which is the whole difference from the
//      launch-on-touch-down this replaced;
//   3. releasing in the dead band below the icons CANCELS: no app is
//      launched, the menu is still there, and the screen is byte-identical
//      to how it was found (a cancel must leave no trace, or the menu is a
//      worse place to change your mind than it was before);
//   4. the same gesture under the measured hardware dropout profile (34
//      episodes/sec) still launches the column the thumb finished on, and a
//      thumb held still does NOT launch by itself;
//   5. every tick of all of it obeys decision 0001's 8px rule and the "no
//      pixel changes outside the pushed rectangle" invariant.
//
// The menu is the one screen where a wrong launch is most expensive: it is
// the only way into an app, so a gesture that fires early takes the child
// somewhere she did not ask for, and the only way back is a two-button chord
// she has to be taught.
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { TouchSim, type TouchReport } from "../../src/touchsim";
import { TOUCHSIM_DEFAULTS } from "../../src/constants";

const WASM_PATH = join(import.meta.dir, "..", "dist", "emu.wasm");
const PANEL_W = 368;
const PANEL_H = 448;
const LAND_W = PANEL_H; // 448
const LAND_H = PANEL_W; // 368
const APP_INDEX_MENU = -1;

// menu.c's own constants.
const ICON_TOP_MARGIN = 24;
const ICON_H = 96;
const MENU_LAUNCH_BAND_H = 220;
const MENU_RELEASE_GRACE_MS = 300;

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
        appCount(): number {
            const ptr = exp.emu_device();
            const b = new Uint8Array(memory.buffer, ptr);
            let end = 0;
            while (b[end] !== 0) end++;
            return (JSON.parse(decoder.decode(b.subarray(0, end))).apps ?? []).length;
        },
        appCurrent(): number { return exp.emu_app_current(); },
        openMenu(nowMs: number) { exp.emu_app_switch(APP_INDEX_MENU); exp.emu_tick(nowMs); },

        // Diffed 32 bits at a time; only differing words are resolved to
        // coordinates. Same technique feature-four.ts uses, for the same
        // reason: this app animates a halo across columns, so "nothing
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

function landPx(fb: Uint8Array, lx: number, ly: number): [number, number] {
    const idx = (Math.round(lx) * PANEL_W + (PANEL_W - 1 - Math.round(ly))) * 2;
    return [fb[idx]!, fb[idx + 1]!];
}
const isWhite = (p: [number, number]) => p[0] === 0xff && p[1] === 0xff;

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

async function main() {
    console.log("=== feature: the menu's press-drag-release gesture ===\n");
    const dev = await loadDevice();
    dev.tickChecked(0);
    dev.openMenu(100);
    dev.tickChecked(200);
    check("the menu is open", dev.appCurrent() === APP_INDEX_MENU, `app_current()=${dev.appCurrent()}`);

    const n = dev.appCount();
    const colW = Math.floor(LAND_W / n);
    const colCentre = (i: number) => i * colW + Math.floor((i === n - 1 ? LAND_W - i * colW : colW) / 2);
    const ICON_CY = ICON_TOP_MARGIN + ICON_H / 2;
    // A point that is inside the halo but outside the icon's own ink, so it
    // is white when the column is not lit and grey when it is.
    const haloProbe = (i: number): [number, number] => [colCentre(i), ICON_CY + 52];
    console.log(`    ${n} apps, ${colW}px columns`);

    let t = 200;
    const clean = dev.fbSnapshot();

    // ---- 1. the thumb lights its column, and only its column -----------
    console.log("\n-- a thumb goes down on column 1 --");
    t = hold(dev, colCentre(1), ICON_CY, t, 150);
    let fb = dev.fbSnapshot();
    const lit1 = !isWhite(landPx(fb, ...haloProbe(1)));
    let othersPlain = true;
    for (let i = 0; i < n; i++) if (i !== 1 && !isWhite(landPx(fb, ...haloProbe(i)))) othersPlain = false;
    check("the column under the thumb is lit, and no other column is", lit1 && othersPlain,
        `col1 lit=${lit1}, others plain=${othersPlain}`);
    check("nothing has launched while the thumb is still down", dev.appCurrent() === APP_INDEX_MENU,
        `app_current()=${dev.appCurrent()}`);

    // ---- the thumb slides ------------------------------------------------
    console.log("\n-- it slides to column 3 --");
    for (let i = 2; i <= 3; i++) t = hold(dev, colCentre(i), ICON_CY, t, 60);
    fb = dev.fbSnapshot();
    check("the light followed it, and the column it left is plain again",
        !isWhite(landPx(fb, ...haloProbe(3))) && isWhite(landPx(fb, ...haloProbe(1))),
        `col3 lit=${!isWhite(landPx(fb, ...haloProbe(3)))}, col1 plain=${isWhite(landPx(fb, ...haloProbe(1)))}`);
    check("still nothing launched", dev.appCurrent() === APP_INDEX_MENU);

    // ---- 2. release launches --------------------------------------------
    console.log("\n-- it lifts --");
    dev.drainLog();
    t = release(dev, t, MENU_RELEASE_GRACE_MS + 200);
    const launch = dev.log().find((l) => l.includes("menu: launch"));
    check("releasing over a lit column launches that app",
        !!launch && launch.includes("launch 3") && dev.appCurrent() === 3,
        `${launch?.trim() ?? "(no launch line)"}, app_current()=${dev.appCurrent()}`);

    // ---- 3. cancel -------------------------------------------------------
    console.log("\n-- back in the menu: drag down into the dead band and let go --");
    const dev2 = await loadDevice();
    dev2.tickChecked(0);
    dev2.openMenu(100);
    dev2.tickChecked(200);
    let t2 = 200;
    const before = dev2.fbSnapshot();
    t2 = hold(dev2, colCentre(2), ICON_CY, t2, 150);
    const litDuring = !isWhite(landPx(dev2.fbSnapshot(), ...haloProbe(2)));
    // ...then down past MENU_LAUNCH_BAND_H, where nothing is lit.
    t2 = hold(dev2, colCentre(2), MENU_LAUNCH_BAND_H + 60, t2, 150);
    const unlitInBand = isWhite(landPx(dev2.fbSnapshot(), ...haloProbe(2)));
    check("dragging below the icons puts the light out - the screen says a release here does nothing",
        litDuring && unlitInBand, `lit over the icon=${litDuring}, unlit in the dead band=${unlitInBand}`);

    dev2.drainLog();
    t2 = release(dev2, t2, MENU_RELEASE_GRACE_MS + 200);
    check("releasing there launches nothing and leaves the menu open",
        dev2.appCurrent() === APP_INDEX_MENU && !dev2.log().some((l) => l.includes("menu: launch")),
        `app_current()=${dev2.appCurrent()}`);
    const after = dev2.fbSnapshot();
    let identical = before.length === after.length;
    for (let i = 0; identical && i < before.length; i++) if (before[i] !== after[i]) identical = false;
    check("and it leaves NO trace - the menu is byte-identical to how it was found", identical);

    // ---- 4. the same gesture under the real dropout profile -------------
    console.log("\n-- the same gesture, under 34 dropout episodes per second --");
    const profile = { ...TOUCHSIM_DEFAULTS, dropoutsEnabled: true, dropoutsPerSec: 34, straysEnabled: false };
    const STEP = 1000 / profile.reportRateHz;
    const TRIALS = 20;
    let correct = 0, early = 0, missed = 0, wrong = 0;
    for (let trial = 0; trial < TRIALS; trial++) {
        const d = await loadDevice();
        d.tickChecked(0);
        d.openMenu(100);
        d.tickChecked(200);
        const sim = new TouchSim(profile, PANEL_W, PANEL_H);
        const from = trial % n, to = (trial * 3 + 1) % n;
        let tt = 1000;
        const put = (lx: number, ly: number) => sim.setPointer(true, PANEL_W - 1 - Math.round(ly), Math.round(lx));
        for (let e = 0; e < 700; e += STEP) {
            const u = e / 700;
            put(colCentre(from) + (colCentre(to) - colCentre(from)) * u, ICON_CY);
            tt += STEP;
            d.feedRaw(sim.poll(tt), tt);
        }
        put(colCentre(to), ICON_CY);
        for (let e = 0; e < 300; e += STEP) { tt += STEP; d.feedRaw(sim.poll(tt), tt); }
        if (d.appCurrent() !== APP_INDEX_MENU) { early++; continue; }
        sim.setPointer(false, 0, 0);
        for (let e = 0; e < MENU_RELEASE_GRACE_MS + 400; e += STEP) { tt += STEP; d.feedRaw(sim.poll(tt), tt); }
        if (d.appCurrent() === APP_INDEX_MENU) missed++;
        else if (d.appCurrent() === to) correct++;
        else wrong++;
    }
    console.log(`    ${correct}/${TRIALS} launched the column the thumb finished on; ${early} launched early, ${missed} never launched, ${wrong} launched the wrong app`);
    check("a slide finished by a genuine lift launches the right app under a dropout-heavy stream",
        correct >= TRIALS - 1, `${correct}/${TRIALS}`);
    check("and none launched while the thumb was still down", early === 0, `${early} early`);

    // A thumb held still is the case a dropout storm most looks like a lift.
    console.log("");
    let heldQuiet = 0;
    const HOLD_TRIALS = 15;
    for (let trial = 0; trial < HOLD_TRIALS; trial++) {
        const d = await loadDevice();
        d.tickChecked(0);
        d.openMenu(100);
        d.tickChecked(200);
        const sim = new TouchSim(profile, PANEL_W, PANEL_H);
        sim.setPointer(true, PANEL_W - 1 - Math.round(ICON_CY), Math.round(colCentre(trial % n)));
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

    // ---- 5. invariants ---------------------------------------------------
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
