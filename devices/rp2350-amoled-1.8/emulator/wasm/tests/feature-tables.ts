// feature-tables: what multiplication-tables practice (firmware/apps/
// tables.c) is supposed to do, asserted against the REAL compiled firmware.
// Clean input, a readable statement of intent - same convention as every
// feature-*.ts in this directory (see feature-tiltball.ts's own header).
// The dropout-driven partner is repro-touch-dropout-tables.ts.
//
//   bun run emulator/wasm/build.ts
//   bun run emulator/wasm/tests/feature-tables.ts
import { readFileSync } from "node:fs";
import { join } from "node:path";

const WASM_PATH = join(import.meta.dir, "..", "dist", "emu.wasm");
const PANEL_W = 368, PANEL_H = 448;
const LAND_W = PANEL_H, LAND_H = PANEL_W; // 448 x 368
const BEZEL = 10;

// tables.c's own layout constants, lifted rather than re-derived (the
// convention every test in this directory uses against its app).
const OX = 10, OY = 10;
const NUMPAD_X0 = OX, NUMPAD_Y0 = OY + 92;
const CELL_W = 99, CELL_H = 64, NUMPAD_COLS = 3;
const CELL_BACK = 9, CELL_ZERO = 10, CELL_CHECK = 11;
const cellCx = (c: number) => NUMPAD_X0 + (c % NUMPAD_COLS) * CELL_W + CELL_W / 2;
const cellCy = (c: number) => NUMPAD_Y0 + Math.floor(c / NUMPAD_COLS) * CELL_H + CELL_H / 2;
const digitCell = (d: number) => (d === 0 ? CELL_ZERO : d - 1);

const ARM_MS = 40, COMMIT_CONFIRM_MS = 72, RELEASE_GRACE_MS = 300;
const HOLD_MS = ARM_MS + COMMIT_CONFIRM_MS + 60;   // comfortably past both
const RELEASE_WAIT_MS = RELEASE_GRACE_MS + 60;

let passCount = 0, failCount = 0;
function check(label: string, ok: boolean, detail?: string) {
  if (ok) passCount++; else failCount++;
  console.log(`[${ok ? "PASS" : "FAIL"}] ${label}${detail ? " - " + detail : ""}`);
}

function landToPanel(lx: number, ly: number): [number, number] {
  return [PANEL_W - 1 - Math.round(ly), Math.round(lx)];
}

async function loadDevice() {
  let memory!: WebAssembly.Memory;
  const dec = new TextDecoder();
  const fwLog: string[] = [];
  const inst = await WebAssembly.instantiate(await WebAssembly.compile(readFileSync(WASM_PATH)), {
    env: {
      js_log(ptr: number, len: number) { fwLog.push(dec.decode(new Uint8Array(memory.buffer, ptr, len))); },
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
  return {
    exports: e,
    tick(nowMs: number) { e.emu_tick(nowMs); },
    touch(down: boolean, panelX: number, panelY: number) { e.emu_touch(down ? 1 : 0, panelX, panelY); },
    appSwitch(i: number) { e.emu_app_switch(i); },
    appCurrent(): number { return e.emu_app_current(); },
    fb(): Uint8Array { return new Uint8Array(memory.buffer, e.emu_fb(), PANEL_W * PANEL_H * 2).slice(); },
    drainLog(): string[] { const o = fwLog.slice(); fwLog.length = 0; return o; },
    device(): any {
      const ptr = e.emu_device();
      const bytes = new Uint8Array(memory.buffer, ptr);
      let end = 0; while (bytes[end] !== 0) end++;
      return JSON.parse(dec.decode(bytes.subarray(0, end)));
    },
  };
}
type Device = Awaited<ReturnType<typeof loadDevice>>;

function grayAt(fb: Uint8Array, lx: number, ly: number): number {
  if (lx < 0 || ly < 0 || lx >= LAND_W || ly >= LAND_H) return 252;
  const px = PANEL_W - 1 - ly, py = lx;
  const i = (py * PANEL_W + px) * 2;
  const v = (fb[i]! << 8) | fb[i + 1]!;
  return ((v >> 5) & 0x3f) << 2;
}

const STEP_MS = 16; // matches app.h's dtMs convention used elsewhere in this suite
let clock = 0;

async function enterTables(dev: Device, appIndex: number) {
  dev.tick(0);
  dev.appSwitch(appIndex);
  clock = 16;
  dev.tick(clock);
  dev.drainLog();
}

function step(dev: Device, ms: number) {
  const end = clock + ms;
  while (clock < end) { clock += STEP_MS; dev.tick(clock); }
}

// Press a numpad cell, hold long enough to arm and confirm, release and
// wait long enough for RELEASE_GRACE to believe the lift - the "clean tap"
// idiom every other feature-*.ts in this directory uses for a press-drag-
// release app (see feature-tiltball.ts's own hold()/tap() equivalents).
function pressCell(dev: Device, cell: number) {
  pressCellFor(dev, cell, HOLD_MS);
}

// Same idiom as pressCell, but with the hold duration named explicitly -
// what the "first hover is immediate" test below uses to drive a hold
// shorter than the old ARM_MS+COMMIT_CONFIRM_MS=112ms floor.
function pressCellFor(dev: Device, cell: number, holdMs: number) {
  const [px, py] = landToPanel(cellCx(cell), cellCy(cell));
  let t = clock;
  const end = t + holdMs;
  while (t < end) { t += STEP_MS; dev.touch(true, px, py); dev.tick(t); }
  dev.touch(false, 0, 0);
  const end2 = t + RELEASE_WAIT_MS;
  while (t < end2) { t += STEP_MS; dev.tick(t); }
  clock = t;
}

function typeDigits(dev: Device, digits: number[]) {
  for (const d of digits) pressCell(dev, digitCell(d));
}

async function main() {
  console.log("=== feature: multiplication-tables practice ===\n");

  let APP_TABLES = -1;
  {
    const dev = await loadDevice();
    const d = dev.device();
    APP_TABLES = (d.apps || []).map((a: string) => a.toLowerCase()).indexOf("tables");
    check("the app table carries 'tables'", APP_TABLES >= 0, `index ${APP_TABLES} of ${JSON.stringify(d.apps)}`);
    if (APP_TABLES < 0) { console.log(`\n${passCount} passed, ${failCount} failed`); process.exit(1); }
  }

  // ---- entering draws a question and nothing is outside the bezel -------
  {
    const dev = await loadDevice();
    await enterTables(dev, APP_TABLES);
    const fb = dev.fb();
    let worst = { x: 0, y: 0 };
    for (let ly = 0; ly < LAND_H; ly++) {
      for (let lx = 0; lx < LAND_W; lx++) {
        if (grayAt(fb, lx, ly) >= 248) continue;
        const nearEdge = lx < BEZEL || ly < BEZEL || lx >= LAND_W - BEZEL || ly >= LAND_H - BEZEL;
        if (nearEdge) worst = { x: lx, y: ly };
      }
    }
    check("nothing is drawn inside the bezel band on entry", worst.x === 0 && worst.y === 0,
      `worst offender (0,0)=none else (${worst.x},${worst.y})`);
  }

  // ---- a digit tap appends to the answer, backspace removes it ----------
  {
    const dev = await loadDevice();
    await enterTables(dev, APP_TABLES);
    pressCell(dev, digitCell(4));
    let log = dev.drainLog();
    check("a digit tap logs the digit it appended", log.some((l) => l.includes("tables: digit 4")), log.join(" | "));
    pressCell(dev, CELL_BACK);
    log = dev.drainLog();
    check("backspace logs", log.some((l) => l.includes("tables: backspace")), log.join(" | "));
  }

  // ---- a short tap, shorter than the old 112ms arm+confirm floor, still
  // lights the key and commits its digit ----------------------------------
  //
  // Before the loupe showed its first cell of a gesture immediately on
  // arming, tables_tick() required BOTH the arm window (ARM_SAMPLES=4,
  // ARM_MS=40, ~40-67ms of contact) AND then COMMIT_CONFIRM_MS=72ms more of
  // that same cell holding steady before hoverCell ever left -1 - a ~112ms
  // floor before anything lit up, matching the owner's own complaint after
  // testing this app ("i have to press for a fairly long time for a touch
  // to register"). A tap held only long enough to arm (about 90ms here,
  // comfortably under that 112ms floor) used to release with hoverCell
  // still -1, i.e. cancelled: nothing lit, nothing typed. The fix shows the
  // very first armed cell with no confirm delay (hoverCell starts at -1,
  // and nothing commits on a hover in the first place - only a release
  // does), so this same short tap should now light the key and type it.
  {
    const dev = await loadDevice();
    await enterTables(dev, APP_TABLES);
    const SHORT_HOLD_MS = 90; // < ARM_MS + COMMIT_CONFIRM_MS (112)
    check("the short tap is actually below the old arm+confirm floor",
      SHORT_HOLD_MS < ARM_MS + COMMIT_CONFIRM_MS, `${SHORT_HOLD_MS}ms vs ${ARM_MS + COMMIT_CONFIRM_MS}ms`);
    pressCellFor(dev, digitCell(7), SHORT_HOLD_MS);
    const log = dev.drainLog();
    check("a short tap (shorter than the old 112ms floor) still lights the key and logs its digit",
      log.some((l) => l.includes("tables: digit 7")), log.join(" | ") || "(nothing logged)");
  }

  // ---- releasing outside the numpad commits nothing ----------------------
  {
    const dev = await loadDevice();
    await enterTables(dev, APP_TABLES);
    // Somewhere in the info column, well clear of the pad.
    const [px, py] = landToPanel(370, 40);
    let t = clock;
    const end = t + HOLD_MS;
    while (t < end) { t += STEP_MS; dev.touch(true, px, py); dev.tick(t); }
    dev.touch(false, 0, 0);
    const end2 = t + RELEASE_WAIT_MS;
    let sawDigit = false;
    while (t < end2) {
      t += STEP_MS; dev.tick(t);
      if (dev.drainLog().some((l) => l.includes("tables: digit"))) sawDigit = true;
    }
    clock = t;
    check("releasing outside the numpad commits no digit (cancel, not a random key)", !sawDigit);
  }

  // ---- answering correctly: attempted and correct both count it ---------
  {
    const dev = await loadDevice();
    await enterTables(dev, APP_TABLES);
    // Reading the question off the framebuffer would be fragile; instead
    // this reads tables.c's own printed resolution line for base/factor/
    // product, which it already logs on giving up. The RNG is seeded from
    // nowMs (tables.c's own comment) and enterTables() always ticks to the
    // same first clock value, so a FRESH device's first question is the
    // same one this probe device just saw - deliberately typed wrong twice
    // on the probe (never affecting a fresh device's own weights) to reveal
    // the product via the "gave up" log line, then a second, fresh device
    // types the real answer from a clean slate and is checked for the
    // "correct" outcome.
    let base = 0, factor = 0, product = 0;
    const probe = await loadDevice();
    await enterTables(probe, APP_TABLES);
    typeDigits(probe, [9, 9]);
    pressCell(probe, CELL_CHECK);
    let log = probe.drainLog();
    check("a wrong first attempt logs a retry", log.some((l) => l.includes("tables: wrong, retry")), log.join(" | "));
    typeDigits(probe, [9, 9]);
    pressCell(probe, CELL_CHECK);
    log = probe.drainLog();
    const revealLine = log.find((l) => l.includes("gave up after 2 tries"));
    check("a second wrong attempt reveals the answer and gives up", !!revealLine, log.join(" | "));
    const m = revealLine?.match(/tables: (\d+) x (\d+) = (\d+), gave up/);
    check("the reveal line names base, factor and product", !!m, revealLine ?? "(none)");
    if (m) { base = Number(m[1]); factor = Number(m[2]); product = Number(m[3]); }

    // Now, from a FRESH device (deterministic same first question, since
    // the RNG is seeded identically), type the CORRECT answer straight
    // away and confirm it resolves as correct.
    const dev2 = await loadDevice();
    await enterTables(dev2, APP_TABLES);
    const digits = product < 10 ? [product] : [Math.floor(product / 10), product % 10];
    typeDigits(dev2, digits);
    pressCell(dev2, CELL_CHECK);
    const log2 = dev2.drainLog();
    check(`a correct first-try answer (${base} x ${factor} = ${product}) logs correct`,
      log2.some((l) => l.includes("correct")), log2.join(" | "));
  }

  // ---- every pushed window's row length is a multiple of 8 --------------
  {
    const dev = await loadDevice();
    await enterTables(dev, APP_TABLES);
    let bad = 0, total = 0;
    const e = dev.exports;
    const n = e.emu_push_count();
    for (let i = 0; i < n; i++) {
      total++;
      const w = e.emu_push_w(i);
      if (w % 8 !== 0) bad++;
    }
    // Also exercise a drag across the numpad, which is where a landscape
    // cell push (row length = cell HEIGHT, per gfx.h's swap) could go wrong.
    pressCell(dev, digitCell(7));
    const n2 = e.emu_push_count();
    for (let i = 0; i < n2; i++) {
      total++;
      const w = e.emu_push_w(i);
      if (w % 8 !== 0) bad++;
    }
    check("every pushed window's row length is a multiple of 8 (decision 0001)", bad === 0, `${bad} bad of ${total}`);
  }

  console.log(`\n${passCount} passed, ${failCount} failed`);
  if (failCount > 0) process.exit(1);
}

main();
