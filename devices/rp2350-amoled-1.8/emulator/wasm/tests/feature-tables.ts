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
import { cellTouchCy } from "../../../tools/tables-layout";

const WASM_PATH = join(import.meta.dir, "..", "dist", "emu.wasm");
const PANEL_W = 368, PANEL_H = 448;
const BEZEL = 10;

// tables.c's own layout constants, lifted rather than re-derived (the
// convention every test in this directory uses against its app). This app
// is now genuinely PORTRAIT (PANEL_W x PANEL_H, native, unrotated) - see
// tables.c's own LAYOUT comment for the two-pass orientation correction
// (a relayed "stay in landscape, shuffle the bands" was superseded once
// the owner specified the physical grip precisely and it was checked
// against emu_shim.c's own button descriptor). Touch coordinates below are
// therefore PANEL coordinates directly, no rotation.
const OX = 10, OY = 10;
const USABLE_W = PANEL_W - 2 * OX, USABLE_H = PANEL_H - 2 * OY; // 348, 428
const QROW_H = 64, LOUPE_ZONE_H = 80;
// CELL_W is 100, not "however much width is left over" - a rendered
// preview at CELL_W=140 (nearly the full USABLE_W) showed three isolated
// digit columns rather than a grid a child's eye reads as a keypad (see
// tables.c's own comment on this constant). CELL_H (57) comes out taller
// than any earlier version of this pad managed: portrait's own 428px of
// usable height is markedly more generous than landscape's 348px.
const CELL_W = 100, CELL_H = 57, NUMPAD_COLS = 3;
const NUMPAD_W = CELL_W * NUMPAD_COLS; // 300
const NUMPAD_X0 = OX + (USABLE_W - NUMPAD_W) / 2, NUMPAD_Y0 = OY + QROW_H + LOUPE_ZONE_H; // 34, 154
const CELL_BACK = 9, CELL_ZERO = 10, CELL_CHECK = 11;
const cellCx = (c: number) => NUMPAD_X0 + (c % NUMPAD_COLS) * CELL_W + CELL_W / 2;
const digitCell = (d: number) => (d === 0 ? CELL_ZERO : d - 1);

// The question band, lifted from tables.c the same way the numpad geometry
// above is. The blank's own x0 depends on whether the current question's
// factor is one digit or two (question_slot_x0() in tables.c - the fix for
// the "6 x    1 =" spacing hole), so a test that does not yet know the
// first question's factor checks BOTH possible cursor positions rather
// than assuming one. Smaller than the app's landscape-detour numbers:
// portrait's USABLE_W (348) is narrower than landscape's (428), a real
// width constraint independent of portrait's extra vertical room.
const QROW_CY = OY + QROW_H / 2;      // 42
const Q_FACTOR_X0 = 112, Q_GAP = 14, QDIGIT_W = 34, QICON_BOX = 26, Q2W = 72;
const Q_SLOT_X0_NARROW = Q_FACTOR_X0 + QDIGIT_W + Q_GAP + QICON_BOX + Q_GAP; // 200, 1-digit factor
const Q_SLOT_X0_WIDE = Q_FACTOR_X0 + Q2W + Q_GAP + QICON_BOX + Q_GAP;        // 238, 2-digit factor ("10")
const CURSOR_X_CANDIDATES = [Q_SLOT_X0_NARROW + 4, Q_SLOT_X0_WIDE + 4];      // tables.c's cursor_x_for_len(0)

// THE COUNTER PILLS (tables.c's THE COUNTER PILLS), replacing the earlier
// boxed, stacked counters: two coloured pills side by side along the very
// bottom, spanning the FULL usable width - NOT tied to NUMPAD_W, since a
// pill is a readout rather than a key and is free to be wider than the
// pad's own key-driven width. PILL_ICON_CX_OFF/PILL_NUM_CX_OFF are
// tables.c's own, used below to locate the icon/digit ink inside each
// pill without guessing.
const COUNTERS_Y0 = NUMPAD_Y0 + CELL_H * 4;               // 382 - the pad's own bottom edge
const COUNTERS_H = USABLE_H - QROW_H - LOUPE_ZONE_H - CELL_H * 4; // 56
const PILL_GAP = 20, PILL_H = 44;
const PILL_W = (USABLE_W - PILL_GAP) / 2;                 // 164
const PILL_Y0 = COUNTERS_Y0 + (COUNTERS_H - PILL_H) / 2;  // 388
const PILL_WRONG_X0 = OX;                                 // 10
const PILL_RIGHT_X0 = OX + PILL_W + PILL_GAP;             // 194
const PILL_ICON_CX_OFF = 42, PILL_NUM_CX_OFF = 100;
// Sampled well clear of the icon/digit ink (both sit at the pill's own
// vertical centre) - near the pill's own top edge, still inside the
// rounded cap's coloured span at the pill's horizontal centre (see
// tables.c's gfx_fill_pill comment: every row's span includes the centre
// column whenever its half-width is above 0, which it is this close to
// the pill's own vertical centre column).
const pillSampleX = (x0: number) => x0 + PILL_W / 2;
const PILL_SAMPLE_Y = PILL_Y0 + 4;

// THE LOUPE (tables.c's THE LOUPE) - needed for the flicker-fix check
// below (loupe_update()'s own comment has the full bug history).
const LOUPE_R = 34, LOUPE_PAD = 6;
const LOUPE_BOX_W = LOUPE_R * 2 + 2 * LOUPE_PAD, LOUPE_BOX_H = LOUPE_BOX_W; // 80
const LOUPE_CY = OY + QROW_H + LOUPE_ZONE_H / 2; // 114

const ARM_MS = 40, COMMIT_CONFIRM_MS = 72, RELEASE_GRACE_MS = 300;
const HOLD_MS = ARM_MS + COMMIT_CONFIRM_MS + 60;   // comfortably past both
const RELEASE_WAIT_MS = RELEASE_GRACE_MS + 60;

let passCount = 0, failCount = 0;
function check(label: string, ok: boolean, detail?: string) {
  if (ok) passCount++; else failCount++;
  console.log(`[${ok ? "PASS" : "FAIL"}] ${label}${detail ? " - " + detail : ""}`);
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

// Portrait, native, unrotated: (x, y) IS the framebuffer's own index, no
// landscape->panel remapping needed any more (this app's landscape detour
// used to route this through PANEL_W-1-ly/lx - see git history).
function grayAt(fb: Uint8Array, x: number, y: number): number {
  if (x < 0 || y < 0 || x >= PANEL_W || y >= PANEL_H) return 252;
  const i = (y * PANEL_W + x) * 2;
  const v = (fb[i]! << 8) | fb[i + 1]!;
  return ((v >> 5) & 0x3f) << 2;
}

// The real RGB behind a panel pixel, same reconstruction preview-tables.ts's
// own landPx() uses - needed for the counter pills, which (per tables.c's
// THE COUNTER PILLS) are a real RGB565 colour, not black ink on a grey
// scale, so grayAt() alone cannot tell orange from green.
function rgbAt(fb: Uint8Array, x: number, y: number): [number, number, number] {
  const i = (y * PANEL_W + x) * 2;
  const v = (fb[i]! << 8) | fb[i + 1]!;
  return [((v >> 11) & 0x1f) * 255 / 31, ((v >> 5) & 0x3f) * 255 / 63, (v & 0x1f) * 255 / 31];
}

// Byte-identical over a rect, at the grayscale resolution grayAt reads -
// used both to prove a redraw left a region untouched and (the wrong-count
// worked example below) to prove two DIFFERENT sequences that should land
// on the same displayed value actually render pixel-for-pixel the same,
// without needing to OCR a rendered digit.
function rectUnchanged(a: Uint8Array, b: Uint8Array, x0: number, y0: number, w: number, h: number): boolean {
  for (let ly = y0; ly < y0 + h; ly++) {
    for (let lx = x0; lx < x0 + w; lx++) {
      if (grayAt(a, lx, ly) !== grayAt(b, lx, ly)) return false;
    }
  }
  return true;
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
  const px = Math.round(cellCx(cell)), py = Math.round(cellTouchCy(cell)); // panel coords directly - portrait, no rotation
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
    for (let y = 0; y < PANEL_H; y++) {
      for (let x = 0; x < PANEL_W; x++) {
        if (grayAt(fb, x, y) >= 248) continue;
        const nearEdge = x < BEZEL || y < BEZEL || x >= PANEL_W - BEZEL || y >= PANEL_H - BEZEL;
        if (nearEdge) worst = { x, y };
      }
    }
    check("nothing is drawn inside the bezel band on entry", worst.x === 0 && worst.y === 0,
      `worst offender (0,0)=none else (${worst.x},${worst.y})`);
  }

  // ---- a digit tap appends, and the whole bottom row is one zero --------
  //
  // Backspace and the check key are both gone (the answer judges itself the
  // moment its second digit lands), so the bottom row is a single
  // triple-width zero. This used to assert that the bottom-left cell logged
  // a backspace; it now asserts that the SAME two positions, either end of
  // that row, both land on the zero. Rewritten rather than deleted: the row
  // still has to do something, and what it does changed.
  {
    const dev = await loadDevice();
    await enterTables(dev, APP_TABLES);
    pressCell(dev, digitCell(4));
    let log = dev.drainLog();
    check("a digit tap logs the digit it appended", log.some((l) => l.includes("tables: digit 4")), log.join(" | "));

    for (const [name, cell] of [["left", CELL_BACK], ["right", CELL_CHECK]] as const) {
      const d2 = await loadDevice();
      await enterTables(d2, APP_TABLES);
      pressCell(d2, cell);
      const l2 = d2.drainLog();
      check(`the bottom row's ${name} end is the zero key`,
        l2.some((l) => l.includes("tables: digit 0")), l2.join(" | "));
    }
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
    // Somewhere in the question band, well clear of the pad (NUMPAD_Y0=154).
    const px = 300, py = 30;
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
    // INVERTED, on purpose. This used to require that a caret blink here.
    // The owner asked for it gone ("le curseur de saisie de texte est
    // pixellise, je pense qu'on devrait l'enlever"), and with the answer
    // judging itself it had a job lasting one keystroke. So the assertion
    // now pins the ABSENCE: nothing may ever darken those pixels while she
    // is entering, across more than two full blink periods. Kept rather
    // than deleted because "no caret" is a real, checkable promise, and a
    // caret creeping back in is exactly the kind of thing nobody notices.
    let sawDark = false;
    for (let i = 0; i < 1300; i += 100) {
      clock += 100;
      dev.tick(clock);
      const fb = dev.fb();
      if (CURSOR_X_CANDIDATES.some((cx) => grayAt(fb, cx, QROW_CY) < 180)) sawDark = true;
    }
    check("no caret ever blinks in the answer blank", !sawDark, `sawDark=${sawDark}`);
  }

  // ---- after ONE digit, the caret sits clearly right of the digit's own
  // ink, not inside it -------------------------------------------------------
  //
  // The owner's bug report: "the blinking cursor doesn't move when I type
  // some numbers". It DID move (cursor_x_for_len(1) is a real x, distinct
  // from cursor_x_for_len(0)) - it just landed inside the single typed
  // digit's own centred ink, so nothing looked different. Candidate x's
  // mirror CURSOR_X_CANDIDATES above (narrow/wide factor), computed the
  // same way cursor_x_for_len(1) now is: question_slot_cx() + QDIGIT_W/2 + 6.
  {
    const CURSOR_X_LEN1_CANDIDATES = [
      Q_SLOT_X0_NARROW + Q2W / 2 + QDIGIT_W / 2 + 6,
      Q_SLOT_X0_WIDE + Q2W / 2 + QDIGIT_W / 2 + 6,
    ];
    const dev = await loadDevice();
    await enterTables(dev, APP_TABLES);
    pressCell(dev, digitCell(4));
    let sawCursor = false;
    for (let i = 0; i < 40; i++) {
      clock += 16;
      dev.tick(clock);
      const fb = dev.fb();
      for (const cx of CURSOR_X_LEN1_CANDIDATES) {
        for (let ly = QROW_CY - 17; ly < QROW_CY + 17; ly++) {
          if (grayAt(fb, cx, ly) < 180) sawCursor = true;
        }
      }
    }
    check("after one digit, nothing is drawn right of the digit's own ink", !sawCursor,
      `checked x in [${CURSOR_X_LEN1_CANDIDATES.join(",")}]`);
  }

  // ---- a resolved answer's highlight is actually centred on the digit ----
  //
  // The owner's bug report: "the highlighting of a correct answer reveals
  // it's not centred properly". Measured before this fix: the tint wash
  // spanned the full ANSWER_BOX_W (92px, x0..x0+91, centre x0+45.5) while
  // the digit centred on the narrower Q_SLOT_W (76px) reference (centre
  // x0+37.5) - an 8px bias. Sampling a wash-only row (above the digit's own
  // ink) and the digit's own ink row and comparing their midpoints is a
  // direct measurement of that bias, not just "some tint appeared".
  {
    const dev = await loadDevice();
    await enterTables(dev, APP_TABLES);
    typeDigits(dev, [6]); // the deterministic first question is 6 x 1 = 6
    pressCell(dev, CELL_CHECK);
    const fb = dev.fb();
    function span(pred: (x: number) => boolean, xMin: number, xMax: number): [number, number] {
      let first = -1, last = -1;
      for (let lx = xMin; lx < xMax; lx++) { if (pred(lx)) { if (first < 0) first = lx; last = lx; } }
      return [first, last];
    }
    const inkRow = QROW_CY;
    const washRow = QROW_CY - 18; // above the digit's own ink, still inside the tinted box
    const [inkL, inkR] = span((lx) => grayAt(fb, lx, inkRow) < 20, 200, 400);
    const [washL, washR] = span((lx) => grayAt(fb, lx, washRow) < 248, 200, 400);
    check("the resolved digit's own ink was found", inkL >= 0, `span [${inkL},${inkR}]`);
    check("the tint wash was found", washL >= 0, `span [${washL},${washR}]`);
    if (inkL >= 0 && washL >= 0) {
      const inkMid = (inkL + inkR) / 2, washMid = (washL + washR) / 2;
      const offset = Math.abs(inkMid - washMid);
      check("the tint wash is centred on the digit it highlights (within 2px)", offset <= 2,
        `ink centre ${inkMid}, wash centre ${washMid}, offset ${offset}px`);
    }
  }

  // ---- worked example: the wrong counter counts every wrong SUBMISSION,
  // not just the final give-up -----------------------------------------------
  //
  // The owner's bug report: "the count of wrong is not correct (doesn't add
  // up)". Worked example, before this fix: type wrong (retry state) -
  // wrongCount stays 0, no redraw. Type wrong again (gives up) - wrongCount
  // becomes 1. Two actual wrong submissions, counter reads 1 - an asymmetry
  // against correctCount, which already increments on every successful
  // submission regardless of which attempt it lands on. After this fix:
  // wrongCount becomes 1 on the FIRST wrong submission already (checked by
  // comparing the counter row's own pixels right after entry against right
  // after that first wrong submission - they must differ, which they did
  // not before), then 2 on the second (same comparison against the
  // first-wrong state).
  {
    const dev = await loadDevice();
    await enterTables(dev, APP_TABLES);
    const atEntry = dev.fb(); // wrong counter reads 0
    typeDigits(dev, [9, 9]);
    pressCell(dev, CELL_CHECK); // FIRST wrong attempt -> PHASE_WRONG_RETRY, not a give-up
    const afterFirstWrong = dev.fb();
    const changedAfterFirst = !rectUnchanged(atEntry, afterFirstWrong, PILL_WRONG_X0, PILL_Y0, PILL_W, PILL_H);
    check("the wrong counter changes on the FIRST wrong attempt already, not only on the final give-up",
      changedAfterFirst);

    typeDigits(dev, [9, 9]);
    pressCell(dev, CELL_CHECK); // SECOND wrong attempt -> gives up
    const afterSecondWrong = dev.fb();
    const changedAfterSecond = !rectUnchanged(afterFirstWrong, afterSecondWrong, PILL_WRONG_X0, PILL_Y0, PILL_W, PILL_H);
    check("the wrong counter changes AGAIN on the second (give-up) attempt - two wrong submissions, two increments",
      changedAfterSecond);
  }

  // ---- the two counter pills carry their own colour, and it survives a
  // keystroke redrawing the answer box on the same frame -------------------
  //
  // The owner's own instruction after seeing this built: "plutot que rose
  // fais la case X de la couleur orange que tu utilisais" - the wrong pill
  // is orange (sketch.c's own palette orange, px_swap(0xFC60), tables.c's
  // pill_color_wrong()), the right pill green (sketch.c's palette green,
  // px_swap(0x07E0), pill_color_right()). Sampled well clear of the black
  // icon/digit ink drawn on top (see PILL_SAMPLE_Y's own comment) - a
  // wide RGB tolerance because the panel's RGB565 quantisation and the
  // emulator's own px_swap round-trip do not reproduce the source byte
  // exactly, only the same HUE. Checked both at rest and after a keystroke
  // elsewhere on screen redraws the answer box, which is the sequence an
  // earlier layout's full-width rule broke under (see git history) - a
  // regression this test would also catch if a future redraw ever bled
  // into the pills' own band.
  {
    const dev = await loadDevice();
    await enterTables(dev, APP_TABLES);
    function isOrange(rgb: [number, number, number]): boolean {
      const [r, g, b] = rgb;
      return r > 200 && g > 90 && g < 190 && b < 60;
    }
    function isGreen(rgb: [number, number, number]): boolean {
      const [r, g, b] = rgb;
      return g > 200 && r < 60 && b < 60;
    }
    const beforeWrong = rgbAt(dev.fb(), pillSampleX(PILL_WRONG_X0), PILL_SAMPLE_Y);
    const beforeRight = rgbAt(dev.fb(), pillSampleX(PILL_RIGHT_X0), PILL_SAMPLE_Y);
    check("the wrong pill is orange right after entry", isOrange(beforeWrong), `rgb(${beforeWrong.map((v) => v.toFixed(0))})`);
    check("the right pill is green right after entry", isGreen(beforeRight), `rgb(${beforeRight.map((v) => v.toFixed(0))})`);
    typeDigits(dev, [4, 2]);
    const afterWrong = rgbAt(dev.fb(), pillSampleX(PILL_WRONG_X0), PILL_SAMPLE_Y);
    const afterRight = rgbAt(dev.fb(), pillSampleX(PILL_RIGHT_X0), PILL_SAMPLE_Y);
    check("the wrong pill is still orange after a keystroke redraws the answer box", isOrange(afterWrong), `rgb(${afterWrong.map((v) => v.toFixed(0))})`);
    check("the right pill is still green after a keystroke redraws the answer box", isGreen(afterRight), `rgb(${afterRight.map((v) => v.toFixed(0))})`);
  }

  // ---- a CORRECT answer never touches the wrong counter -------------------
  //
  // The owner's drawing shows two explicit lines, a check and a cross; this
  // app counts them as "correct" and "wrong". A correct submission must
  // never increment or redraw the wrong row, whichever attempt it lands
  // on - comparing the WRONG row's own pixels before and after a correct
  // answer is a direct check of that: byte-identical means untouched.
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
      const wrongRowUnchanged = rectUnchanged(before, after, PILL_WRONG_X0, PILL_Y0, PILL_W, PILL_H);
      check("a correct answer leaves the WRONG counter pill's pixels untouched", wrongRowUnchanged);
      const correctRowChanged = !rectUnchanged(before, after, PILL_RIGHT_X0, PILL_Y0, PILL_W, PILL_H);
      check("a correct answer DOES change the CORRECT counter pill's pixels", correctRowChanged);
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
    // Also exercise a drag across the numpad, which is where a numpad cell
    // push (row length = cell WIDTH, straight from gfx_push - portrait,
    // native, no rotation to swap it) could go wrong.
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
