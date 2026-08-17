/**
 * Renders breakout (firmware/apps/breakout.c) from the REAL compiled
 * firmware, headlessly, for the owner to judge by eye - same technique as
 * tools/preview-tiltball.ts and every file under emulator/wasm/tests/: load
 * emu.wasm, drive it, read the framebuffer back. Nothing here reimplements
 * the app; the wall, the ball and the paddle in these PNGs are the same C
 * that runs on the board.
 *
 *   bun run emulator/wasm/build.ts
 *   bun tools/preview-breakout.ts
 *
 * Breakout is in the default app table unconditionally (runtime_core.c) -
 * no build flag needed.
 *
 * Writes preview/breakout-<shot>.png, LANDSCAPE (448x368, the way this app
 * is held), the rotation applied at encode time only.
 *
 * THE SHOTS, and why these six:
 *
 *   entry        the picture at switch-in: full eighteen-brick wall on its
 *                three nested arcs, ball mid-field, paddle centred (no
 *                tilt has been sent), and three solid lives-dots in the
 *                top-left corner - see breakout.c's own header comment,
 *                "THREE LIVES, LOSING THE BALL, AND GAME OVER".
 *   midgame      some time in, untouched: a few bricks gone, the ball and
 *                paddle wherever the physics alone put them, and by then
 *                often already down a life or two - see "THIS TOOL NO
 *                LONGER TRIES..." below.
 *   life-lost    caught mid-ripple, shortly after the ball has crossed the
 *                floor and a life has just gone: the ball is gone, a black
 *                ring is blooming near the paddle, and the lives row is
 *                down to two dots.
 *   gameover     all three lives spent, the freeze long past: no ball, no
 *                ripple, an empty lives row, the wall exactly as it was
 *                left - a still tableau waiting for a tap, the same idiom
 *                dino.c's own idle "waiting for a tap" screen uses.
 *   restarted    a tap fired from gameover: lives are back to three, a
 *                fresh ball sits at its deterministic start position, and
 *                the wall's regrow wave (reused from the ordinary mid-game
 *                clear celebration) is under way.
 *   regrown      that same regrow wave finished: the wall complete again.
 *
 * ENTRY AND MIDGAME are driven with no touch and no tilt (the paddle sits at
 * its default centred rest position) - the "nobody is holding it" case
 * breakout.c's own header comment still argues is worth watching for a
 * short while. LIFE-LOST, GAMEOVER, RESTARTED and REGROWN pin the paddle
 * hard to one rail (a sustained extreme tilt) so a miss - and then a game
 * over - is reached quickly and reliably, exactly the setup
 * emulator/wasm/tests/feature-breakout.ts's own lives tests use.
 *
 * THIS TOOL NO LONGER TRIES TO CAPTURE A REGROW REACHED THROUGH A NATURAL,
 * UNATTENDED FULL CLEAR - the pre-lives version of this file did, because
 * an untouched table played (and eventually cleared its own wall) forever.
 * It no longer can: even an idealised controller that tracks the ball's own
 * x every tick (tried as a probe while writing feature-breakout.ts, not
 * kept here) spends all three lives in roughly ten to fifteen seconds of
 * simulated play, against a paddle hit zone that is real but much narrower
 * than its drawn lens shape - see breakout.c's header comment's closing
 * "WHETHER THREE LIVES IS THE RIGHT NUMBER" paragraph. So REGROWN here is
 * reached the way GAME OVER's own restart tap reaches it in play too: the
 * regrow wave is the same event either way (breakout.c's own header,
 * "GAME OVER, AND HOW SHE STARTS AGAIN"), so a picture of it is a picture
 * of it regardless of which door it walked in through.
 *
 * Deliberately does NOT open a serial port or touch a physical device.
 */
import { readFileSync, mkdirSync } from "node:fs";
import { join } from "node:path";

const ROOT = join(import.meta.dir, "..");
const WASM_PATH = join(ROOT, "emulator", "wasm", "dist", "emu.wasm");
const PREVIEW_DIR = join(ROOT, "preview");

const PANEL_W = 368;
const PANEL_H = 448;
const LAND_W = PANEL_H; // 448
const LAND_H = PANEL_W; // 368
const FRAME_MS = 20;

async function loadDevice() {
    let memory!: WebAssembly.Memory;
    const dec = new TextDecoder();
    const inst = await WebAssembly.instantiate(await WebAssembly.compile(readFileSync(WASM_PATH)), {
        env: {
            js_log(_p: number, _l: number) { /* quiet */ },
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

    const jsonBytes = new Uint8Array(memory.buffer, e.emu_device());
    let end = 0; while (jsonBytes[end] !== 0) end++;
    const apps: string[] = JSON.parse(dec.decode(jsonBytes.subarray(0, end))).apps || [];
    const APP_BREAKOUT = apps.indexOf("breakout");
    if (APP_BREAKOUT < 0) {
        throw new Error(
            "this emu.wasm has no breakout app in its table - rebuild it: " +
            "bun run emulator/wasm/build.ts",
        );
    }

    e.emu_tick(0);
    e.emu_app_switch(APP_BREAKOUT);
    e.emu_tick(FRAME_MS);
    if (e.emu_app_current() !== APP_BREAKOUT) throw new Error("did not land in breakout");
    let t = FRAME_MS;
    // While set, applied on every tick step() takes - the "sustained tilt"
    // shots (life-lost, gameover) pin this for many frames in a row, the
    // same way feature-breakout.ts's own lives tests re-send tilt every
    // frame (tilt.c's filter needs a continuous signal, not one sample).
    let heldTilt: [number, number, number] | null = null;
    return {
        now(): number { return t; },
        // v is PANEL-space g; emu_sensor_vector() wants DEVICE axes (the
        // same as a real IMU sample), so this undoes
        // firmware/runtime/tilt.c's device_to_panel() on the way in
        // ((dx,dy,dz) -> (dy,dx,-dz), commit b8c06d2, currently its own
        // inverse) - the same fix feature-breakout.ts's own tilt() applies,
        // for the same reason.
        setTilt(v: [number, number, number] | null) { heldTilt = v; },
        touch(down: boolean, x: number, y: number) { e.emu_touch(down ? 1 : 0, x, y); },
        step(ms: number) {
            const end2 = t + ms;
            while (t < end2) {
                if (heldTilt) e.emu_sensor_vector(1, heldTilt[1], heldTilt[0], -heldTilt[2]);
                t += FRAME_MS;
                e.emu_tick(t);
            }
        },
        tick(ms: number) { t += ms; e.emu_tick(t); }, // one raw step, touch() bracketed around it
        fb(): Uint8Array { return new Uint8Array(memory.buffer, e.emu_fb(), PANEL_W * PANEL_H * 2).slice(); },
    };
}

// ---- wall-ink measure, to find the clear/regrow moments -------------------
// Was a saturated-colour test back when bricks were a palette; now that
// bricks are black rings (breakout.c's own "ONE INK" section), the same
// "neither pure white nor pure black" bucket instead catches the
// anti-aliased grey fringe every curve leaves - the wall's rings dominate it
// (far more edge per unit area than the ball's one filled disc or the
// paddle's own band), which is what still makes it a usable "how much wall
// is left" proxy for finding the shots below. See feature-breakout.ts's
// countWallInk for the same measure used as a test assertion.
function pixelKind(r: number, g: number, b: number): "white" | "black" | "grey" {
    if (r >= 30 && g >= 60 && b >= 30) return "white";
    if (r <= 1 && g <= 1 && b <= 1) return "black";
    return "grey";
}
function wallInk(fb: Uint8Array): number {
    let n = 0;
    for (let i = 0; i < PANEL_W * PANEL_H; i++) {
        const v = (fb[i * 2]! << 8) | fb[i * 2 + 1]!;
        const r = (v >> 11) & 0x1f, g = (v >> 5) & 0x3f, b = v & 0x1f;
        if (pixelKind(r, g, b) === "grey") n++;
    }
    return n;
}

// ---- lives row, read directly - same constants and technique as
// feature-breakout.ts's own livesShown(), lifted from breakout.c's #define
// block (LIFE_DOT_X0/Y/GAP): landscape (lx,ly) -> panel (PANEL_W-1-ly, lx).
const PLAY_L = 26, PLAY_T = 26;
const LIFE_DOT_X0 = PLAY_L + 12, LIFE_DOT_Y = PLAY_T + 12, LIFE_DOT_GAP = 18;
const START_LIVES = 3;
function landPixel565(fb: Uint8Array, lx: number, ly: number): { r: number; g: number; b: number } {
    const px = PANEL_W - 1 - ly, py = lx;
    const i = (py * PANEL_W + px) * 2;
    const v = (fb[i]! << 8) | fb[i + 1]!;
    return { r: (v >> 11) & 0x1f, g: (v >> 5) & 0x3f, b: v & 0x1f };
}
function isBlack565(p: { r: number; g: number; b: number }) { return p.r <= 1 && p.g <= 1 && p.b <= 1; }
function livesShown(fb: Uint8Array): number {
    let n = 0;
    for (let i = 0; i < START_LIVES; i++) {
        if (isBlack565(landPixel565(fb, Math.round(LIFE_DOT_X0 + i * LIFE_DOT_GAP), Math.round(LIFE_DOT_Y)))) n++;
    }
    return n;
}

// ---- PNG (landscape, 24-bit RGB), same machinery as preview-tiltball.ts --
function crc32Table(): Uint32Array {
    const t = new Uint32Array(256);
    for (let n = 0; n < 256; n++) { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; t[n] = c >>> 0; }
    return t;
}
const CRC = crc32Table();
function crc32(b: Uint8Array): number {
    let c = 0xffffffff;
    for (let i = 0; i < b.length; i++) c = CRC[(c ^ b[i]!) & 0xff]! ^ (c >>> 8);
    return (c ^ 0xffffffff) >>> 0;
}
function adler32(d: Uint8Array): number {
    let a = 1, b = 0;
    for (let i = 0; i < d.length; i++) { a = (a + d[i]!) % 65521; b = (b + a) % 65521; }
    return ((b << 16) | a) >>> 0;
}
function chunk(type: string, data: Uint8Array): Uint8Array {
    const tb = new TextEncoder().encode(type);
    const body = new Uint8Array(tb.length + data.length);
    body.set(tb, 0); body.set(data, tb.length);
    const out = new Uint8Array(4 + body.length + 4);
    const dv = new DataView(out.buffer);
    dv.setUint32(0, data.length, false);
    out.set(body, 4);
    dv.setUint32(4 + body.length, crc32(body), false);
    return out;
}
function landPx(fb: Uint8Array, lx: number, ly: number): [number, number, number] {
    const idx = (lx * PANEL_W + (PANEL_W - 1 - ly)) * 2;
    const v = (fb[idx]! << 8) | fb[idx + 1]!;
    return [
        Math.round((((v >> 11) & 0x1f) * 255) / 31),
        Math.round((((v >> 5) & 0x3f) * 255) / 63),
        Math.round(((v & 0x1f) * 255) / 31),
    ];
}
async function write(shot: string, fb: Uint8Array) {
    const raw = new Uint8Array((LAND_W * 3 + 1) * LAND_H);
    for (let ly = 0; ly < LAND_H; ly++) {
        const off = ly * (LAND_W * 3 + 1);
        raw[off] = 0;
        for (let lx = 0; lx < LAND_W; lx++) {
            const [r, g, b] = landPx(fb, lx, ly);
            const o = off + 1 + lx * 3;
            raw[o] = r; raw[o + 1] = g; raw[o + 2] = b;
        }
    }
    const z = Bun.deflateSync(raw);
    const idat = new Uint8Array(2 + z.length + 4);
    idat[0] = 0x78; idat[1] = 0x9c;
    idat.set(z, 2);
    new DataView(idat.buffer).setUint32(2 + z.length, adler32(raw), false);
    const ihdr = new Uint8Array(13);
    const dv = new DataView(ihdr.buffer);
    dv.setUint32(0, LAND_W, false); dv.setUint32(4, LAND_H, false);
    ihdr[8] = 8; ihdr[9] = 2;
    const sig = new Uint8Array([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);
    const parts = [sig, chunk("IHDR", ihdr), chunk("IDAT", idat), chunk("IEND", new Uint8Array(0))];
    const total = parts.reduce((s, p) => s + p.length, 0);
    const out = new Uint8Array(total);
    let o = 0;
    for (const p of parts) { out.set(p, o); o += p.length; }
    mkdirSync(PREVIEW_DIR, { recursive: true });
    const path = join(PREVIEW_DIR, `breakout-${shot}.png`);
    await Bun.write(path, out);
    console.log(`wrote ${path}`);
}

async function main() {
    const dev = await loadDevice();
    await write("entry", dev.fb());
    console.log(`  entry: lives=${livesShown(dev.fb())}`);

    const initialInk = wallInk(dev.fb());

    // midgame: step until a few bricks are visibly gone, capped so this
    // never hangs if the constants ever change enough to make that slow.
    let midgameShot: Uint8Array | null = null;
    for (let i = 0; i < 3000 && !midgameShot; i++) {
        dev.step(200);
        const ink = wallInk(dev.fb());
        if (ink < initialInk * 0.85) midgameShot = dev.fb();
    }
    await write("midgame", midgameShot ?? dev.fb());
    console.log(`  midgame: lives=${livesShown(dev.fb())}`);

    // ---- life-lost and gameover: a fresh device, paddle pinned hard to
    // one rail (landscape gx=-1, fed as the panel vector's Y component -
    // see feature-breakout.ts's own rotation comment for why) so the ball
    // finds the gap quickly and reliably rather than however long an
    // untouched centred paddle happens to take. -----------------------
    const dev2 = await loadDevice();
    dev2.setTilt([0, -1, 0]);
    let lastLives = livesShown(dev2.fb());
    let lostAtT: number | null = null;
    for (let i = 0; i < 6000 && lostAtT === null; i++) {
        dev2.step(40);
        const n = livesShown(dev2.fb());
        if (n < lastLives) lostAtT = dev2.now();
        lastLives = n;
    }
    if (lostAtT === null) throw new Error("never lost a life within the time budget - is the paddle pin still working?");
    // A couple hundred ms into the ripple/freeze - visible, not yet faded.
    dev2.step(200);
    await write("life-lost", dev2.fb());
    console.log(`  life-lost: lives=${livesShown(dev2.fb())} at t=${lostAtT}`);

    let overAtLives = -1;
    for (let i = 0; i < 12000 && overAtLives !== 0; i++) {
        dev2.step(40);
        overAtLives = livesShown(dev2.fb());
    }
    if (overAtLives !== 0) throw new Error("never reached 0 lives within the time budget");
    dev2.step(1200); // well past LIFE_LOST_FREEZE_MS (550) - a genuinely still table
    await write("gameover", dev2.fb());
    console.log(`  gameover: lives=${livesShown(dev2.fb())}`);

    // ---- restarted and regrown: a tap from the same gameover table.
    // TAP_ARM_SAMPLES/TAP_ARM_MS (breakout.c) need a few consecutive
    // contact frames inside a short window; six 20ms frames comfortably
    // covers it, the same margin feature-breakout.ts's own restart test
    // uses. --------------------------------------------------------------
    for (let i = 0; i < 6; i++) { dev2.touch(true, 184, 224); dev2.tick(FRAME_MS); }
    dev2.touch(false, 184, 224);
    dev2.tick(FRAME_MS);
    // CELEB_PAUSE_MS (400ms) of stillness precedes the wave itself
    // (breakout.c: "the wall goes empty for a beat... so nothing is
    // happening in two places on screen at once"), so a shot taken right
    // after the tap fires would show lives and the ball back but an empty
    // wall - correct, but not the WAVE. Stepping partway into
    // CELEB_TOTAL_MS (under 1600ms total) instead catches some rings back,
    // some still growing, some not yet started.
    dev2.step(900);
    await write("restarted", dev2.fb());
    console.log(`  restarted: lives=${livesShown(dev2.fb())}`);

    dev2.step(2000); // CELEB_TOTAL_MS is under 2s from the tap - generous margin
    await write("regrown", dev2.fb());
    console.log(`  regrown: lives=${livesShown(dev2.fb())}, wall ink=${wallInk(dev2.fb())} (entry was ${initialInk})`);
}

main();
