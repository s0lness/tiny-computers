#!/usr/bin/env bun
// The runner: builds the firmware model, checks every invariant's `see`
// resolves to a real file, runs each check, and fails the build on any
// violation - or on any input that is missing or unparsable, which
// decision 0004 says must be treated the same as a real failure, not
// silently skipped.
//
//   bun tools/invariants/runner.ts [path/to/main.elf] [path/to/main.elf.map]
//
// Defaults to this device's own build output. Exit code is 0 only if every
// invariant ran and passed.

import { existsSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { parseRegions, parseSections, parseSymbols } from "./model";
import { parseDisassembly } from "./disasm";
import type { Firmware, Invariant } from "./types";
import { ALL_INVARIANTS } from "./rules/rp2350-amoled-1.8";

// devices/rp2350-amoled-1.8/tools/invariants/runner.ts -> devices/rp2350-amoled-1.8
const DEVICE_ROOT = join(dirname(fileURLToPath(import.meta.url)), "..", "..");

function buildFirmware(elfPath: string, mapPath: string): Firmware {
  if (!existsSync(elfPath)) throw new Error(`no .elf at ${elfPath} - build the firmware first`);
  if (!existsSync(mapPath)) throw new Error(`no .map at ${mapPath} - build the firmware first`);
  return {
    elfPath,
    mapPath,
    sections: parseSections(elfPath),
    symbols: parseSymbols(elfPath),
    regions: parseRegions(mapPath),
    functions: parseDisassembly(elfPath),
  };
}

function checkSeeResolves(invariant: Invariant): void {
  const path = join(DEVICE_ROOT, invariant.see);
  if (!existsSync(path)) {
    throw new Error(
      `invariant "${invariant.name}" has a "see" that does not resolve: ${invariant.see} ` +
        `(looked for ${path})`
    );
  }
}

export function runInvariants(fw: Firmware, invariants: Invariant[]): boolean {
  for (const inv of invariants) checkSeeResolves(inv);

  let ok = true;
  for (const inv of invariants) {
    const violations = inv.check(fw);
    if (violations.length === 0) {
      console.log(`PASS  ${inv.name}`);
      continue;
    }
    ok = false;
    console.log(`FAIL  ${inv.name}`);
    console.log(`  why: ${inv.why}`);
    console.log(`  see: ${inv.see}`);
    for (const v of violations) {
      console.log(`  ${v.message}`);
      for (const s of v.symbols) console.log(`    - ${s}`);
    }
  }
  return ok;
}

if (import.meta.main) {
  const elfPath = process.argv[2] ?? join(DEVICE_ROOT, "firmware", "build", "main.elf");
  const mapPath = process.argv[3] ?? join(DEVICE_ROOT, "firmware", "build", "main.elf.map");

  try {
    const fw = buildFirmware(elfPath, mapPath);
    const ok = runInvariants(fw, ALL_INVARIANTS);
    if (!ok) {
      console.error("\ninvariant checker: FAILED");
      process.exit(1);
    }
    console.log("\ninvariant checker: all invariants passed");
  } catch (err) {
    // Missing or unparsable input is a build failure, not a skip - decision
    // 0004: a checker that is skipped because its input went missing is
    // indistinguishable, from the build's point of view, from one that passed.
    console.error(`invariant checker: ${err instanceof Error ? err.message : String(err)}`);
    process.exit(1);
  }
}
