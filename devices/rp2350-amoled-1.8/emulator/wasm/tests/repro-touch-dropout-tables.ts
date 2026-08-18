// repro-touch-dropout-tables: the multiplication-tables numpad/loupe
// gesture (firmware/apps/tables.c) driven by a touch stream that
// misbehaves the way the real FT3168 does, rather than by a clean pointer.
//
// WHY THIS FILE EXISTS SEPARATELY FROM feature-tables.ts. AGENTS.md's own
// rule: a feature driven by touch needs BOTH kinds of file, because a
// feature proven only against clean input is a feature proven against a
// controller this board does not have (the sketchpad's palette shipped
// this way once and did not work in the owner's hands). This is the
// numpad's own instance of that rule, and the numpad is the device's own
// worst case for it (AGENTS.md's own numpad warning): twelve small
// targets in a tight grid, the exact shape the FT3168's measured 80-250px
// centroid jitter and 34/sec dropouts hit hardest.
//
// THE COMMIT_CONFIRM_MS=72 THIS FILE'S SCENARIO E FORMALISES is the same
// measurement tools/sweep-tables-commit.ts ran ad hoc while choosing that
// constant (see tables.c's own header comment for the full swept table).
// This is that finding turned into a standing check, the same way decision
// 0013's own 150ms table became feature-menu-hover.ts's excursion counter.
//
// Run with (after `bun run emulator/wasm/build.ts`):
//
//   bun run emulator/wasm/tests/repro-touch-dropout-tables.ts
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { TouchSim, type TouchReport } from "../../src/touchsim";
import { TOUCHSIM_DEFAULTS } from "../../src/constants";
import { seededRng, seedFromName } from "../../../tools/gate/touch";
import {
  PANEL_W, PANEL_H,
  NUMPAD_X0, NUMPAD_Y0, NUMPAD_W, NUMPAD_H, NUMPAD_COLS, NUMPAD_ROWS, CELL_W, CELL_H,
  CELL_ZERO, cellCx, cellTouchCy, cellFromTouch, cellValueName, panelPoint,
} from "../../../tools/tables-layout";

const WASM_PATH = join(import.meta.dir, "..", "dist", "emu.wasm");

// tables.c's own layout, read from tools/tables-layout.ts (this directory's
// standing convention: lift the app's own constants rather than re-derive
// them) - see that file's own header for why this is now ONE shared copy
// rather than one hand-copied into this file, feature-tables.ts,
// preview-tables.ts and sweep-tables-grace.ts separately. This file's own
// stale copy (CELL_W=99, CELL_H=64, NUMPAD_Y0=OY+92 - the pre-2026-08-17
// layout) went unnoticed for a full redesign cycle: it did not fail, it
// just hit-tested the wrong rectangle and stopped proving anything -
// exactly the failure mode a shared import is meant to close off.

const RELEASE_GRACE_MS = 300;

let passCount = 0, failCount = 0;
function check(label: string, ok: boolean, detail?: string) {
  const status = ok ? "PASS" : "FAIL";
  if (ok) passCount++; else failCount++;
  console.log(`[${status}] ${label}${detail ? " - " + detail : ""}`);
}

const bytes = readFileSync(WASM_PATH);
const compiled = await WebAssembly.compile(bytes);

async function freshDevice() {
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
  const jsonBytes = new Uint8Array(memory.buffer, exp.emu_device());
  let end = 0; while (jsonBytes[end] !== 0) end++;
  const apps: string[] = JSON.parse(decoder.decode(jsonBytes.subarray(0, end))).apps || [];
  const APP_TABLES = apps.map((a) => a.toLowerCase()).indexOf("tables");
  if (APP_TABLES < 0) throw new Error("no tables app in this emu.wasm");
  exp.emu_tick(0);
  exp.emu_app_switch(APP_TABLES);
  exp.emu_tick(10);
  fwLog.length = 0;
  return {
    feed(r: TouchReport, nowMs: number) { exp.emu_touch(r.fingers === 1 ? 1 : 0, r.x, r.y); exp.emu_tick(nowMs); },
    drainLog(): string[] { const o = fwLog.slice(); fwLog.length = 0; return o; },
  };
}

const dropoutHeavy = { ...TOUCHSIM_DEFAULTS, dropoutsEnabled: true, dropoutsPerSec: 34, straysEnabled: false };
const STEP_MS = 1000 / dropoutHeavy.reportRateHz;

async function main() {
  console.log("=== tables.c's numpad/loupe gesture, under a dropout-heavy touch stream ===\n");
  console.log(`    profile: ${dropoutHeavy.dropoutsPerSec} dropout episodes/sec at ${dropoutHeavy.reportRateHz}Hz, RELEASE_GRACE_MS=${RELEASE_GRACE_MS}\n`);

  // ---- A: a thumb held still on a digit for 3s never commits it early ---
  // Nothing commits until a genuine release, so a held contact - however
  // battered by dropouts - must not log a digit while it is still down.
  const HOLD_TRIALS = 20, HOLD_MS = 3000;
  let committedEarly = 0, totalDropouts = 0;
  for (let i = 0; i < HOLD_TRIALS; i++) {
    const dev = await freshDevice();
    const sim = new TouchSim(dropoutHeavy, PANEL_W, PANEL_H, seededRng(seedFromName(`tables-hold-${i}`)));
    const cell = i % 9;
    const [px, py] = panelPoint(cellCx(cell), cellTouchCy(cell));
    sim.setPointer(true, px, py);
    let t = 1000;
    for (let held = 0; held < HOLD_MS; held += STEP_MS) {
      t += STEP_MS;
      dev.feed(sim.poll(t), t);
      if (dev.drainLog().some((l) => l.includes("tables: digit"))) { committedEarly++; break; }
    }
    totalDropouts += sim.simDropouts;
  }
  console.log(`    ${totalDropouts} simulated dropout episodes across ${HOLD_TRIALS} x ${HOLD_MS}ms of held contact`);
  check("a thumb held on a digit for 3 seconds never commits it while still down",
    committedEarly === 0, `${committedEarly}/${HOLD_TRIALS} committed early`);

  // ---- B: press, drag across the pad, release lands the digit under the
  // finger's FINAL resting cell. --------------------------------------
  console.log("");
  const DRAG_TRIALS = 25, DRAG_MS = 900, SETTLE_MS = 320;
  let correct = 0, early = 0, missed = 0, wrongCell = 0;
  for (let i = 0; i < DRAG_TRIALS; i++) {
    const dev = await freshDevice();
    const sim = new TouchSim(dropoutHeavy, PANEL_W, PANEL_H, seededRng(seedFromName(`tables-drag-${i}`)));
    const from = i % 9;
    const to = (i * 5 + 4) % 9;
    let t = 1000;
    for (let e = 0; e < DRAG_MS; e += STEP_MS) {
      const u = e / DRAG_MS;
      const [px, py] = panelPoint(
        cellCx(from) + (cellCx(to) - cellCx(from)) * u,
        cellTouchCy(from) + (cellTouchCy(to) - cellTouchCy(from)) * u);
      sim.setPointer(true, px, py);
      t += STEP_MS;
      dev.feed(sim.poll(t), t);
    }
    const [tx, ty] = panelPoint(cellCx(to), cellTouchCy(to));
    sim.setPointer(true, tx, ty);
    for (let e = 0; e < SETTLE_MS; e += STEP_MS) { t += STEP_MS; dev.feed(sim.poll(t), t); }

    if (dev.drainLog().some((l) => l.includes("tables: digit"))) early++;

    sim.setPointer(false, 0, 0);
    const digits: number[] = [];
    for (let e = 0; e < RELEASE_GRACE_MS + 400; e += STEP_MS) {
      t += STEP_MS;
      dev.feed(sim.poll(t), t);
      for (const l of dev.drainLog()) {
        const m = l.match(/tables: digit (\d+)/);
        if (m) digits.push(Number(m[1]));
      }
    }
    const expected = to === CELL_ZERO ? 0 : to + 1;
    if (digits.length === 0) { missed++; continue; }
    if (digits.length === 1 && digits[0] === expected) correct++;
    else wrongCell++;
  }
  const rate = (correct / DRAG_TRIALS) * 100;
  console.log(`    ${correct}/${DRAG_TRIALS} drags committed exactly the digit under the finger's final cell (${rate.toFixed(0)}%); ` +
    `${early} early, ${missed} missed, ${wrongCell} wrong`);
  check("a drag finished by a genuine lift commits the digit the thumb finished on",
    rate >= 90, `${rate.toFixed(0)}% over ${DRAG_TRIALS} trials`);
  check("no trial committed a digit while the thumb was still moving", early === 0, `${early} early commit(s)`);

  // ---- C: bursts of phantom contacts, at every spacing, never arm a
  // gesture (ARM_SAMPLES/ARM_RATE_HZ's own hard property, not a rate) ------
  console.log("");
  let burstCommits = 0;
  const burstCases: string[] = [];
  for (const spacingMs of [17, 33, 60, 120, 200, 400]) {
    for (const count of [1, 2, 3]) {
      const dev = await freshDevice();
      let t = 1000;
      const [sx, sy] = panelPoint(cellCx(4), cellTouchCy(4));
      for (let k = 0; k < count; k++) {
        dev.feed({ fingers: 1, x: sx, y: sy }, t);
        for (let e = STEP_MS; e < spacingMs; e += STEP_MS) { t += STEP_MS; dev.feed({ fingers: 0, x: 0, y: 0 }, t); }
        t += STEP_MS;
      }
      for (let e = 0; e < RELEASE_GRACE_MS + 600; e += STEP_MS) { t += STEP_MS; dev.feed({ fingers: 0, x: 0, y: 0 }, t); }
      if (dev.drainLog().some((l) => l.includes("tables: digit"))) {
        burstCommits++;
        burstCases.push(`${count} contact(s) ${spacingMs}ms apart`);
      }
    }
  }
  check("no burst of up to three phantom contacts, at any spacing, ever commits a digit",
    burstCommits === 0, burstCommits === 0 ? "18 burst shapes tried" : burstCases.join(", "));

  // ---- D: the emulator's own modelled stray process, run long. ----------
  const strayProfile = { ...TOUCHSIM_DEFAULTS, dropoutsEnabled: false, straysEnabled: true };
  const devD = await freshDevice();
  const simD = new TouchSim(strayProfile, PANEL_W, PANEL_H, seededRng(seedFromName("tables-strays")));
  simD.setPointer(false, 0, 0);
  let strayCommits = 0, tD = 1000;
  for (let e = 0; e < 120000; e += STEP_MS) {
    tD += STEP_MS;
    devD.feed(simD.poll(tD), tD);
    if (devD.drainLog().some((l) => l.includes("tables: digit"))) strayCommits++;
  }
  console.log(`    ${simD.simStrays} stray contacts synthesised over 120s at the emulator's modelled rate, nothing touching the glass`);
  check("two minutes of the modelled stray process commits nothing", strayCommits === 0, `${strayCommits} commit(s)`);

  // ---- E: a still thumb, with the reported position wandering, still
  // commits the digit it was actually on almost every time, and never
  // commits a digit the controller never reported - the property this
  // app's COMMIT_CONFIRM_MS=72 was chosen against
  // (tools/sweep-tables-commit.ts, tables.c's own header comment). --------
  console.log("");
  const jitterProfile = {
    ...TOUCHSIM_DEFAULTS,
    dropoutsEnabled: true, dropoutsPerSec: 34, straysEnabled: false,
    positionJitterEnabled: true, positionJitterPerSec: 1.5,
    positionJitterMinPx: 80, positionJitterMaxPx: 250, positionJitterMaxHoldReports: 3,
  };
  // Which numpad cell a PANEL-space report names, mirroring tables.c's own
  // numpad_hit(): clamped into the grid if inside its bounding box,
  // otherwise -1 (the cancel region - the loupe zone above, the counter
  // pills below, or off-panel). Portrait, native, unrotated: a panel report
  // (panelX, panelY) IS (lx, ly) directly - no rotation to undo, unlike
  // this app's landscape detour, which this function used to map through.
  // Was a private copy of the grid arithmetic, which never subtracted the
  // thumb bias and so named a row lower than the firmware hit-tested - see
  // cellFromTouch()'s own comment in tools/tables-layout.ts. The shared mirror
  // owns this now, bias and asymmetric edges included.
  const cellFromReport = (panelX: number, panelY: number) => cellFromTouch(panelX, panelY);

  const JIT_TRIALS = 60;
  let jitCorrect = 0, jitElsewhere = 0, jitNone = 0, jitEpisodes = 0, jitInvented = 0;
  const inventedCases: string[] = [];
  for (let i = 0; i < JIT_TRIALS; i++) {
    const dev = await freshDevice();
    const sim = new TouchSim(jitterProfile, PANEL_W, PANEL_H, seededRng(seedFromName(`tables-jitter-${i}`)));
    const cell = i % 9;
    const [px, py] = panelPoint(cellCx(cell), cellTouchCy(cell));
    sim.setPointer(true, px, py);
    let t = 1000;
    const reported = new Set<number>();
    for (let e = 0; e < 700; e += STEP_MS) {
      t += STEP_MS;
      const rep = sim.poll(t);
      if (rep.fingers === 1) reported.add(cellFromReport(rep.x, rep.y));
      dev.feed(rep, t);
    }
    sim.setPointer(false, 0, 0);
    let got = -2;
    for (let e = 0; e < RELEASE_GRACE_MS + 400; e += STEP_MS) {
      t += STEP_MS;
      const rep = sim.poll(t);
      if (rep.fingers === 1) reported.add(cellFromReport(rep.x, rep.y));
      dev.feed(rep, t);
      for (const l of dev.drainLog()) {
        const m = l.match(/tables: digit (\d+)/);
        if (m) got = Number(m[1]) === 0 ? CELL_ZERO : Number(m[1]) - 1;
      }
    }
    jitEpisodes += sim.simJitterEpisodes;
    if (got === -2) { jitNone++; continue; }
    if (!reported.has(got)) {
      jitInvented++;
      inventedCases.push(`trial ${i}: committed ${cellValueName(got)}, controller only ever said ${[...reported].map(cellValueName).join("/")}`);
    }
    if (got === cell) jitCorrect++; else jitElsewhere++;
  }
  console.log(`    ${jitEpisodes} jitter episodes synthesised (80-250px, up to 3 reports each) across ${JIT_TRIALS} trials`);
  console.log(`    ${jitCorrect} landed on the finger's own cell, ${jitElsewhere} elsewhere, ${jitNone} nothing committed`);
  check("the jitter profile is actually firing, not sitting dormant (decision 0008)", jitEpisodes > 0, `${jitEpisodes} episodes`);
  check("a commit is always a cell the controller actually reported - nothing invented",
    jitInvented === 0, jitInvented === 0 ? `${JIT_TRIALS} trials` : inventedCases.join("; "));
  check("a still thumb, with the reported position wandering, still commits its own digit almost every time",
    jitCorrect >= Math.ceil(JIT_TRIALS * 0.9), `${jitCorrect}/${JIT_TRIALS}`);

  console.log(`\n${passCount} passed, ${failCount} failed`);
  if (failCount > 0) process.exit(1);
}

main();
