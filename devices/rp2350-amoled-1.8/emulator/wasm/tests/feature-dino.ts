// feature-dino: a statement of what firmware/apps/dino.c is supposed to do,
// driven with CLEAN input - a mouse-like touch stream, no dropouts, no
// strays. That is the argument tools/gate/README.md's `clean-input` rule
// asks every touch-driven feature test to make explicitly: this file
// exists to read as a plain description of the feature, and its own pair,
// repro-touch-dropout-dino-jump.ts, is what proves the same behaviour
// survives this panel's real weather (34 dropout episodes/sec while a
// finger is down, occasional strays with nothing touching at all).
//
// WHAT THIS FILE READS AT, AND WHAT IT IS THEREFORE BLIND TO (decision
// 0010's required disclosure for a new capability). Every assertion below
// reads dino.c's own printf() lines ("dino: jump", "dino: hit at N px",
// "dino: new record N px", "dino: run start", "dino: passed previous
// best") plus the emulator's own framebuffer/app-index exports. That is
// downstream of the app's decision-making and upstream of the panel: it
// proves the STATE MACHINE (one jump per press, no double jump mid-air,
// death freezes input, restart works, the first run always sets a record)
// is correct. It says nothing about whether the jump TIMING feels fair to
// a two-year-old's thumb on the real board (decision 0003's forbidden
// question, unanswerable here by construction) or whether the drawn
// silhouette reads as "a dino" to her eye (decision 0010's contact-sheet
// argument - see tools/preview-dino.ts for the pictures this file cannot
// judge).
//
// Run with (after `bun run emulator/wasm/build.ts`):
//   bun run emulator/wasm/tests/feature-dino.ts
import { readFileSync } from "node:fs";
import { join } from "node:path";

const WASM_PATH = join(import.meta.dir, "..", "dist", "emu.wasm");
const APP_DINO = 6; // g_apps[] = { chrono, sketch, timer, four, clock, morpion, dino, ... } (level removed 2026-08-17)

let passCount = 0;
let failCount = 0;
function check(label: string, ok: boolean, detail?: string) {
    const status = ok ? "PASS" : "FAIL";
    if (ok) passCount++; else failCount++;
    console.log(`[${status}] ${label}${detail ? " - " + detail : ""}`);
}

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
    if (e.emu_init() !== 1) throw new Error("emu_init() failed");
    e.emu_tick(0);
    e.emu_app_switch(APP_DINO);
    e.emu_tick(10);
    if (e.emu_app_current() !== APP_DINO) throw new Error("did not land in dino");
    log.length = 0;
    return {
        touch(down: boolean, nowMs: number) {
            // dino.c reads only touchDown, never touchX/Y (tap anywhere) -
            // see its own header. Panel coordinates are irrelevant, so a
            // fixed centre point is used throughout.
            e.emu_touch(down ? 1 : 0, 184, 224);
            e.emu_tick(nowMs);
        },
        drainLog(): string[] { const out = log.slice(); log.length = 0; return out; },
        fb(): Uint8Array { return new Uint8Array(memory.buffer, e.emu_fb(), 368 * 448 * 2).slice(); },
    };
}

// A "tap": down long enough to clear TAP_ARM_MS/TAP_ARM_SAMPLES (dino.c),
// then up. 15ms steps, matching the controller's real ~60Hz report rate
// (the same interval every other clean-input test in this directory uses).
async function tapAt(dev: Awaited<ReturnType<typeof loadDevice>>, tRef: { t: number }, downMs: number) {
    for (let e = 0; e < downMs; e += 15) { tRef.t += 15; dev.touch(true, tRef.t); }
    tRef.t += 15;
    dev.touch(false, tRef.t);
}
function idle(dev: Awaited<ReturnType<typeof loadDevice>>, tRef: { t: number }, ms: number) {
    for (let e = 0; e < ms; e += 15) { tRef.t += 15; dev.touch(false, tRef.t); }
}

// Idles in small steps up to maxMs, stopping THE INSTANT "dino: hit at" is
// logged - not a blind idle(maxMs), which would keep ticking well past the
// collision and, if it landed early in the window, run the game clock
// straight through DEATH_FREEZE_MS before the caller ever gets control
// back. Returns whatever log lines were produced up to and including the
// hit (or everything, if no hit occurred inside maxMs).
function idleUntilHit(dev: Awaited<ReturnType<typeof loadDevice>>, tRef: { t: number }, maxMs: number): string[] {
    const collected: string[] = [];
    for (let e = 0; e < maxMs; e += 15) {
        tRef.t += 15;
        dev.touch(false, tRef.t);
        const lines = dev.drainLog();
        collected.push(...lines);
        if (lines.some((l) => l.includes("dino: hit at"))) break;
    }
    return collected;
}

const whiteCount = (fb: Uint8Array) => {
    let n = 0;
    for (let i = 0; i < fb.length; i += 2) if (fb[i] === 0xff && fb[i + 1] === 0xff) n++;
    return n;
};

async function main() {
    console.log("=== dino: clean-input statement of intent ===\n");

    // ---- 1. entering the app draws SOMETHING (the idle standing pose),
    // not a blank white screen. ------------------------------------------
    {
        const dev = await loadDevice();
        const panelPixels = 368 * 448;
        const white = whiteCount(dev.fb());
        check("the idle screen is not blank white (a standing dino is drawn)",
            white < panelPixels, `${panelPixels - white} non-white pixel(s)`);
    }

    // ---- 2. a tap while idle starts a run, and the FIRST EVER run always
    // sets a record (there is nothing yet to fall short of). -------------
    {
        const dev = await loadDevice();
        const t = { t: 1000 };
        await tapAt(dev, t, 80);
        const afterTap = dev.drainLog();
        check("a tap while idle starts a run", afterTap.some((l) => l.includes("dino: run start")), afterTap.join(" | "));
        check("the very first run has no previous best", afterTap.some((l) => l.includes("no previous best")));

        // Never jump: she is guaranteed to meet the first obstacle within
        // MAX_GAP_MS of the run starting (dino.c's spawn timer), and with
        // liftPx pinned at 0 the collision AABBs must overlap the instant
        // their x-ranges do.
        const afterWait = idleUntilHit(dev, t, 6000);
        check("never jumping meets the first obstacle and ends the run",
            afterWait.some((l) => l.includes("dino: hit at")), afterWait.join(" | "));
        check("the run that just ended sets a new record (nothing to beat before it)",
            afterWait.some((l) => l.includes("dino: new record")), afterWait.join(" | "));
    }

    // ---- 3. one press, one jump - a tap held well past the jump's own
    // airtime does not fire a second jump while airborne. -----------------
    {
        const dev = await loadDevice();
        const t = { t: 1000 };
        await tapAt(dev, t, 80);
        dev.drainLog();
        idle(dev, t, 120); // arm, then release before the jump lands

        // Second, independent tap while airborne (well inside the jump's
        // own airtime): must NOT jump again.
        await tapAt(dev, t, 80);
        const midAir = dev.drainLog();
        check("a second tap while still airborne does not fire a second jump",
            !midAir.some((l) => l.includes("dino: jump")), midAir.join(" | ") || "(no jump logged, correct)");

        // Once safely grounded again (jump airtime plus margin), a fresh
        // tap DOES jump again.
        idle(dev, t, 700);
        await tapAt(dev, t, 80);
        const grounded = dev.drainLog();
        check("a fresh tap once grounded again jumps a second time",
            grounded.some((l) => l.includes("dino: jump")), grounded.join(" | "));
    }

    // ---- 4. death freezes input: a tap during DEATH_FREEZE_MS neither
    // jumps nor restarts. --------------------------------------------------
    {
        const dev = await loadDevice();
        const t = { t: 1000 };
        await tapAt(dev, t, 80);
        dev.drainLog();
        // never jump again -> she dies on the first obstacle. Stops the
        // instant the hit is logged - see idleUntilHit()'s own comment on
        // why a blind idle(6000) here would already have run the clock
        // straight through the freeze window this test exists to probe.
        const died = idleUntilHit(dev, t, 6000);
        check("(setup) she died as expected", died.some((l) => l.includes("dino: hit at")), died.join(" | "));

        // A tap DURING the freeze beat (well under DEATH_FREEZE_MS=550, and
        // starting after TAP_RELEASE_GRACE_MS of silence so it is read as a
        // fresh press rather than a continuation of whatever contact caused
        // the collision).
        idle(dev, t, 320);
        await tapAt(dev, t, 60);
        const duringFreeze = dev.drainLog();
        check("a tap during the death freeze neither jumps nor restarts",
            !duringFreeze.some((l) => l.includes("dino: jump") || l.includes("dino: run start")),
            duringFreeze.join(" | ") || "(nothing logged, correct)");

        // Past the freeze, with a genuine lift already behind it, a tap
        // restarts.
        idle(dev, t, 600);
        await tapAt(dev, t, 80);
        const restarted = dev.drainLog();
        check("a tap after the freeze restarts the run",
            restarted.some((l) => l.includes("dino: run start")), restarted.join(" | "));
    }

    console.log(`\n${passCount} passed, ${failCount} failed`);
    if (failCount > 0) process.exit(1);
}

main();
