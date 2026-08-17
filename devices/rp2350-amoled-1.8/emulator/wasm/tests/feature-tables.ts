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
// convention every test in this directory uses against its app). Updated
// 2026-08-17 for the owner's exact mockup: QROW_H grew from 64 to 80 to
// give the new full-width rule real separation from the equation above it
// (tables.c's QROW_H comment), recovered from the loupe (LOUPE_ZONE_H
// 88->80) and the pad (CELL_H 49->47) - see tables.c's own LAYOUT comment
// for the arithmetic (80+80+188=348=USABLE_H).
const OX = 10, OY = 10;
const QROW_H = 80, LOUPE_ZONE_H = 80;
const NUMPAD_X0 = OX, NUMPAD_Y0 = OY + QROW_H + LOUPE_ZONE_H;
const CELL_W = 95, CELL_H = 47, NUMPAD_COLS = 3;
const CELL_BACK = 9, CELL_ZERO = 10, CELL_CHECK = 11;
const cellCx = (c: number) => NUMPAD_X0 + (c % NUMPAD_COLS) * CELL_W + CELL_W / 2;
const cellCy = (c: number) => NUMPAD_Y0 + Math.floor(c / NUMPAD_COLS) * CELL_H + CELL_H / 2;
const digitCell = (d: number) => (d === 0 ? CELL_ZERO : d - 1);

// The question band and the counters box, lifted from tables.c the same
// way the numpad geometry above is. Updated again for the owner's exact
// mockup (2026-08-17): the blank's own x0 now depends on whether the
// current question's factor is one digit or two (question_slot_x0() in
// tables.c - the fix for the "6 x    1 =" spacing hole), so a test that
// does not yet know the first question's factor checks BOTH possible
// cursor positions rather than assuming one.
const QROW_CY = OY + QROW_H / 2;      // 50
const Q_FACTOR_X0 = 142, Q_GAP = 20, QDIGIT_W = 36, QICON_BOX = 32, Q2W = 76;
const Q_SLOT_X0_NARROW = Q_FACTOR_X0 + QDIGIT_W + Q_GAP + QICON_BOX + Q_GAP; // 250, 1-digit factor
const Q_SLOT_X0_WIDE = Q_FACTOR_X0 + Q2W + Q_GAP + QICON_BOX + Q_GAP;        // 290, 2-digit factor ("10")
const CURSOR_X_CANDIDATES = [Q_SLOT_X0_NARROW + 4, Q_SLOT_X0_WIDE + 4];      // tables.c's cursor_x_for_len(0)
const RULE_Y = OY + QROW_H - 4; // 86, tables.c's RULE_Y

const NUMPAD_W = CELL_W * NUMPAD_COLS;
const INFO_X0 = NUMPAD_X0 + NUMPAD_W;   // 295
const BOX_X0 = INFO_X0 + 8, BOX_Y0 = NUMPAD_Y0 + CELL_H;         // 303, 211
const COUNTER_ROW_X0 = BOX_X0 + 5, COUNTER_ROW_W = (428 - NUMPAD_W - 16) - 10; // 308, 117
const COUNTERS_Y0 = BOX_Y0 + 5;          // 222
const BOX_H = CELL_H * 4 - CELL_H - 6;   // 135, tables.c's (NUMPAD_H - CELL_H - 6)
const COUNTER_ROW_H = (BOX_H - 10) / 2;  // 62, tables.c's (BOX_H - 2*BOX_INSET) / 2

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

  // ---- the answer blank carries a blinking cursor while entering --------
  //
  // The owner's drawing labels the blank "blink". At 0 digits typed, in
  // PHASE_ASK, with no touch at all, the cursor should still toggle on and
  // off on the clock alone (BLINK_PERIOD_MS=500 in tables.c) - sampling
  // across a bit more than one full cycle should see both a lit sample
  // (dark ink at the cursor's cell) and an unlit one (background white).
  {
    const dev = await loadDevice();
    await enterTables(dev, APP_TABLES);
    let sawDark = false, sawLight = false;
    for (let i = 0; i < 1300; i += 100) {
      clock += 100;
      dev.tick(clock);
      const fb = dev.fb();
      // The cursor's x depends on this question's factor width (1 or 2
      // digits) - check both candidate positions, see this file's own
      // comment on Q_SLOT_X0_NARROW/WIDE above.
      const dark = CURSOR_X_CANDIDATES.some((cx) => grayAt(fb, cx, QROW_CY) < 180);
      if (dark) sawDark = true; else sawLight = true;
    }
    check("the answer blank's cursor blinks while entering (both a lit and an unlit sample seen)",
      sawDark && sawLight, `sawDark=${sawDark} sawLight=${sawLight}`);
  }

  // ---- the full-width rule under the question band stays UNBROKEN, even
  // after a keystroke redraws the answer box on top of the same row -------
  //
  // The rule and the answer box used to share a row (ANSWER_BOX_H was
  // QROW_H, the full band height): every keystroke's own white/tint fill
  // repainted straight through the rule's y-coordinate wherever the answer
  // box happened to sit, erasing the MIDDLE of the rule without ever
  // redrawing it - a line that ran in from the left, stopped under the
  // blank, and resumed on the right. Found by looking at the rendered PNG,
  // not by inspection: it reads as one interrupted line, not "a blank with
  // a separator below it". ANSWER_BOX_H now stops at the underline's own
  // bottom edge, clear of RULE_Y, so nothing redraw_answer() does can ever
  // touch the rule's row again - checked here both at rest and after an
  // actual keystroke, which is exactly the sequence that broke it before.
  {
    const dev = await loadDevice();
    await enterTables(dev, APP_TABLES);
    function ruleGapAt(fb: Uint8Array): number {
      for (let lx = 20; lx < LAND_W - 20; lx++) {
        if (grayAt(fb, lx, RULE_Y) >= 180) return lx;
      }
      return -1;
    }
    const gapBefore = ruleGapAt(dev.fb());
    check("the full-width rule is unbroken right after entry", gapBefore === -1, `first gap at x=${gapBefore}`);
    typeDigits(dev, [4, 2]);
    const gapAfter = ruleGapAt(dev.fb());
    check("the rule stays unbroken after typing into the answer (the answer box redraw does not erase it)",
      gapAfter === -1, `first gap at x=${gapAfter}`);
  }

  // ---- the wrong counter only counts a FINAL give-up, not every question
  // resolution - a correct answer must not touch it -----------------------
  //
  // The owner's drawing shows two explicit lines, a check and a cross;
  // this app now counts them as "correct" and "wrong" rather than the
  // earlier "attempted" (which incremented on EVERY resolution, right or
  // wrong). Comparing the WRONG row's own pixels before and after a
  // correct answer is a direct check of that semantic: byte-identical
  // means a correct answer never touched it.
  function rectUnchanged(a: Uint8Array, b: Uint8Array, x0: number, y0: number, w: number, h: number): boolean {
    for (let ly = y0; ly < y0 + h; ly++) {
      for (let lx = x0; lx < x0 + w; lx++) {
        if (grayAt(a, lx, ly) !== grayAt(b, lx, ly)) return false;
      }
    }
    return true;
  }
  {
    const probe = await loadDevice();
    await enterTables(probe, APP_TABLES);
    typeDigits(probe, [9, 9]);
    pressCell(probe, CELL_CHECK);
    typeDigits(probe, [9, 9]);
    pressCell(probe, CELL_CHECK);
    const log = probe.drainLog();
    const m = log.join("\n").match(/tables: (\d+) x (\d+) = (\d+), gave up/);
    check("could read the product off the probe for the counter-row test", !!m, log.join(" | "));
    if (m) {
      const product = Number(m[3]);
      const digits = product < 10 ? [product] : [Math.floor(product / 10), product % 10];

      const dev = await loadDevice();
      await enterTables(dev, APP_TABLES);
      const before = dev.fb();
      typeDigits(dev, digits);
      pressCell(dev, CELL_CHECK);
      const after = dev.fb();
      const wrongRowUnchanged = rectUnchanged(before, after, COUNTER_ROW_X0, COUNTERS_Y0, COUNTER_ROW_W, COUNTER_ROW_H);
      check("a correct answer leaves the WRONG counter row's pixels untouched", wrongRowUnchanged);
      const correctRowChanged = !rectUnchanged(before, after, COUNTER_ROW_X0, COUNTERS_Y0 + COUNTER_ROW_H, COUNTER_ROW_W, COUNTER_ROW_H);
      check("a correct answer DOES change the CORRECT counter row's pixels", correctRowChanged);
    }
  }

  // ---- questions never repeat back to back --------------------------------
  //
  // The owner's own instruction: "randomize the numbers" - genuinely
  // unpredictable to a child who has seen the previous few. tables.c's
  // pick_next_fact() now draws uniformly over every fact except the one
  // just asked, which makes an immediate repeat impossible BY CONSTRUCTION,
  // not just unlikely - this drives a real run of questions (always typing
  // "99" twice to force the "gave up" reveal line, which is deterministic
  // regardless of what the actual product is) and checks the (base,factor)
  // pair named in that line never repeats from one question to the next.
  {
    const dev = await loadDevice();
    await enterTables(dev, APP_TABLES);
    const seen: string[] = [];
    const REVEAL_MS = 1600; // tables.c's REVEAL_MS - touch is ignored until this elapses
    for (let i = 0; i < 14; i++) {
      typeDigits(dev, [9, 9]);
      pressCell(dev, CELL_CHECK);
      typeDigits(dev, [9, 9]);
      pressCell(dev, CELL_CHECK);
      const log = dev.drainLog();
      const m = log.join("\n").match(/tables: (\d+) x (\d+) = \d+, gave up/);
      if (m) seen.push(`${m[1]}x${m[2]}`);
      step(dev, REVEAL_MS + 100); // let PHASE_WRONG_REVEAL finish before the next question's input
    }
    check("drove enough questions to check for a repeat", seen.length >= 10, `${seen.length} questions read`);
    let repeat = -1;
    for (let i = 1; i < seen.length; i++) if (seen[i] === seen[i - 1]) { repeat = i; break; }
    check("no two consecutive questions are the same fact", repeat === -1,
      repeat >= 0 ? `repeat at index ${repeat}: ${seen.join(", ")}` : seen.join(", "));
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
