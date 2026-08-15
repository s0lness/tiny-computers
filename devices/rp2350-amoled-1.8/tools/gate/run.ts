#!/usr/bin/env bun
// The gate. One command, every cross-cutting rule, every app, non-zero
// exit on any violation.
//
//   bun run tools/gate/run.ts             the gate
//   bun run tools/gate/run.ts --measure   what every app costs, no verdict
//
// It sits beside tools/invariants rather than inside it because the two
// look at different things: the invariant checker reads the linked ARM
// image and asks what the code IS; this drives the compiled-to-wasm
// firmware and asks what it DOES. Same posture though - a rule carries the
// reason it exists, a failure prints that reason, and an input it cannot
// read is a failure and not a skip (tools/invariants/README.md).
//
// WHAT IT DOES NOT DO IS THE POINT OF ITS LAST SECTION. See the "cannot"
// list printed at the end of every run, and README.md.

import { existsSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { loadDevice } from "./device";
import { chordStimulus, exerciseApp, menuArena, probeClockDriven, probeSettleCadence } from "./exercise";
import { checkFreshness, stalenessMessage } from "./freshness";
import { partition } from "./known";
import { lintCleanInput } from "./cleaninput";
import { runContracts } from "./contracts";
import { treeFingerprint } from "./fingerprint";
import type { Violation } from "./rules";
import { checkArenaHeadroom } from "./rules";
import { selftest } from "./selftest";
import { APP_COUNT_MAX, ARENA_FAIL_FRACTION, LIMITS, PROBE_HOLDS, PROBE_HOLD_MS, PROBE_WINDOW_MS, PWR_LONG_PRESS_MS, PANEL_AREA } from "./rules/rp2350-amoled-1.8";

const DEVICE_ROOT = resolve(dirname(fileURLToPath(import.meta.url)), "..", "..");

function head(title: string): void {
  console.log(`\n=== ${title} ===`);
}

function reportViolations(violations: Violation[]): void {
  // Grouped by rule, because four instances of one broken push are one
  // piece of news, not four.
  const byRule = new Map<string, Violation[]>();
  for (const v of violations) {
    const list = byRule.get(v.rule) ?? [];
    list.push(v);
    byRule.set(v.rule, list);
  }
  for (const [rule, list] of byRule) {
    console.log(`\nFAIL  ${rule}  (${list.length})`);
    console.log(`  why: ${list[0]!.why}`);
    console.log(`  see: ${list[0]!.see}`);
    for (const v of list.slice(0, 12)) console.log(`  - ${v.detail}`);
    if (list.length > 12) console.log(`  ... and ${list.length - 12} more`);
  }
}

async function main(): Promise<void> {
  const measureOnly = process.argv.includes("--measure");
  const started = Date.now();
  const violations: Violation[] = [];

  /* ---- 0a. can these rules fail at all? ----------------------------- */
  // Decision 0004's own lesson, applied to this tool: a checker whose
  // rules all pass and a checker whose rules never fire look identical
  // from outside. Microseconds, and not optional.
  const self = selftest();
  const selfFailed = self.filter((r) => !r.ok);
  if (selfFailed.length > 0) {
    console.error("gate: the gate's own rules are broken, so nothing below would have meant anything:");
    for (const r of selfFailed) console.error(`  FAIL  ${r.name}${r.detail ? ` - ${r.detail}` : ""}`);
    process.exit(1);
  }

  /* ---- 0b. the module is not stale (bug 8) -------------------------- */
  head("the module under test");
  console.log(`${self.length} self-tests: every rule was shown to reject something`);
  const fresh = checkFreshness();
  if (fresh.newer.length > 0) {
    console.error(stalenessMessage(fresh));
    process.exit(1);
  }
  console.log(`emu.wasm is newer than all ${fresh.consideredCount} of its sources`);
  console.log(`tree fingerprint ${treeFingerprint().short}  (bun run tools/gate/fingerprint.ts for what this is for)`);

  /* ---- which apps exist, asked of the firmware, not of a list ------- */
  const probe = await loadDevice({ skipFreshness: true });
  const appNames = probe.descriptor.apps ?? [];
  if (appNames.length === 0) throw new Error("emu_device() declared no apps: nothing to gate");
  console.log(`${appNames.length} apps declared by emu_device(): ${appNames.join(", ")}`);

  /* ---- 0c. the contracts several agents share (decision 0014) ------- */
  // Before any app is driven: these compare the copies of contracts that
  // parallel worktrees maintain by hand (the app table's three copies, the
  // ABI's three, the shim's layer, the tests' index constants), and a
  // disagreement there makes everything the stimuli would find ambiguous.
  head("the contracts several agents share");
  const contracts = runContracts(appNames, APP_COUNT_MAX);
  for (const line of contracts.summary) console.log(line);
  if (contracts.gated.length > 0) {
    // Loud, known.ts-style, not a failure: a gated entry is a deliberate
    // build variant, and the honest statement is that THIS run did not
    // exercise it.
    console.log(`  NOT GATED THIS RUN: ${contracts.gated.length} g_apps[] entr${contracts.gated.length === 1 ? "y" : "ies"} behind preprocessor conditions:`);
    for (const g of contracts.gated) {
      console.log(`    ${g.token}  (#${g.gatedBy}) - absent from this module, so no rule here has seen it`);
    }
  }
  violations.push(...contracts.violations);

  /* ---- 1. every app, every tick (bugs 1, 2, 3, 4) ------------------- */
  head("every app, every tick");
  const rows: string[] = [];
  for (let i = 0; i < appNames.length; i++) {
    // The menu is reachable from any app and is not in the apps array, so
    // it gets exercised as the tail of the first app's own run rather than
    // as an app in its own right.
    const extra = i === 0 ? [chordStimulus(PWR_LONG_PRESS_MS)] : [];
    const r = await exerciseApp(i, appNames[i]!, LIMITS, PWR_LONG_PRESS_MS, extra);
    violations.push(...r.violations);
    violations.push(...checkArenaHeadroom(r.appName, r.arenaBytes, r.arenaCapacity, ARENA_FAIL_FRACTION));
    rows.push(
      `  ${r.appName.padEnd(8)} ${String(r.ticks).padStart(6)} ticks   ` +
      `peak ${String(r.maxPushedPixels).padStart(7)} px/tick (${(r.maxPushedPixels / PANEL_AREA).toFixed(2)} panels)   ` +
      `peak ${String(r.maxPushes).padStart(2)} pushes   ` +
      `arena ${String(r.arenaBytes).padStart(5)}B (${((100 * r.arenaBytes) / r.arenaCapacity).toFixed(0)}%)   during "${r.peakContext}"`
    );
  }
  {
    // The menu allocates like an app and is not a g_apps[] entry, so it
    // gets its own arena line and the same headroom rule.
    const m = await menuArena();
    violations.push(...checkArenaHeadroom("menu", m.arenaBytes, m.arenaCapacity, ARENA_FAIL_FRACTION));
    rows.push(
      `  ${"menu".padEnd(8)} ${"".padStart(6)}                                              ` +
      `arena ${String(m.arenaBytes).padStart(5)}B (${((100 * m.arenaBytes) / m.arenaCapacity).toFixed(0)}%)`
    );
  }
  for (const row of rows) console.log(row);

  /* ---- 2. the two counterfactuals (bugs 5, 6) ----------------------- */
  head("animations: clock-driven, and settling to one picture");
  for (let i = 0; i < appNames.length; i++) {
    const name = appNames[i]!;
    let found = 0;
    for (const hold of PROBE_HOLDS) {
      found += (await probeClockDriven(i, name, LIMITS, hold, PROBE_HOLD_MS, PROBE_WINDOW_MS))
        .map((v) => (violations.push(v), 1)).length;
      found += (await probeSettleCadence(i, name, LIMITS, hold, PROBE_HOLD_MS, PROBE_WINDOW_MS))
        .map((v) => (violations.push(v), 1)).length;
    }
    console.log(`  ${name.padEnd(8)} ${PROBE_HOLDS.length} hold positions x 2 counterfactuals: ${found === 0 ? "clean" : `${found} violation(s)`}`);
  }

  /* ---- 3. gesture tests drive the measured controller (bug 7) ------- */
  head("gesture tests, and what they drive");
  const lint = lintCleanInput();
  console.log(`  ${lint.checked} test files, ${lint.throughTouchSim} drive TouchSim, ${lint.baselined} argued exceptions`);
  violations.push(...lint.violations);

  /* ---- verdict ------------------------------------------------------ */
  const elapsed = ((Date.now() - started) / 1000).toFixed(1);
  const part = partition(violations);

  if (part.known.length > 0) {
    head("known and NOT fixed: the board shows these today");
    for (const { finding, count } of part.known) {
      console.log(`\n  ${finding.rule}  (${count} occurrence${count === 1 ? "" : "s"}, found ${finding.found})`);
      console.log(`    ${finding.what}`);
      console.log(`    still open: ${finding.why}`);
      console.log(`    decides: ${finding.decides}`);
    }
  }
  for (const s of part.stale) {
    part.fresh.push({
      rule: s.rule,
      why: "an exemption that no longer reproduces is how a checker quietly stops covering something",
      see: "tools/gate/known.ts",
      detail: `KNOWN entry ${s.rule} /${s.match.source}/ matched nothing this run: it is fixed, delete the entry`,
    });
  }

  if (measureOnly) {
    console.log(`\nmeasure only: ${part.fresh.length} new violation(s) found and NOT enforced (${elapsed}s)`);
    return;
  }
  if (part.fresh.length > 0) {
    reportViolations(part.fresh);
    console.error(`\ngate: FAILED with ${part.fresh.length} new violation(s) in ${elapsed}s`);
    process.exit(1);
  }

  console.log(`\ngate: no new violations in ${elapsed}s` +
    (part.known.length > 0 ? ` (${part.known.length} known finding(s) above, still open)` : ""));
  console.log(
    `\nWhat this run did NOT check, and cannot:\n` +
    `  - time. There is no watchdog here and no second core, so a budget in\n` +
    `    pixels is a proxy for a cost in microseconds, never a measurement of\n` +
    `    it (docs/decisions/0003). "Is it responsive" is a question for the board.\n` +
    `  - the bezel as a physical object. The band is a number from a photograph;\n` +
    `    only a person holding the device can say whether it is the right number.\n` +
    `  - anything below the sensor seam: the FT3168's registers, the two-ring\n` +
    `    timestamp merge, the PMIC key decoding (docs/decisions/0008).\n` +
    `  - the panel itself: no burn-in, no brightness, no tearing.\n` +
    `  - whether the picture is any good.`
  );
}

if (import.meta.main) {
  if (!existsSync(join(DEVICE_ROOT, "emulator", "wasm", "build.ts"))) {
    console.error(`gate: this does not look like the device root (${DEVICE_ROOT})`);
    process.exit(1);
  }
  main().catch((err) => {
    // Same rule the invariant checker uses: an input this cannot read is a
    // failure, not a skip. A gate that passes because it could not look is
    // the instrument this project has spent two days removing.
    console.error(`gate: ${err instanceof Error ? (err.stack ?? err.message) : String(err)}`);
    process.exit(1);
  });
}
