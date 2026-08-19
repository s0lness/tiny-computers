// Bug 9: the board ran a different build than the tree.
//
// Nothing in devlink reports a build identity, so a differential harness
// diffed three apps on the board against four in the tree and reported a
// 28 percent divergence that read as a rendering bug. It was a rendering
// bug in nothing: the board was simply older. Every hour spent on that was
// spent because a comparison was believed before it was checked.
//
// The fix is a name for "what is in this tree" that the board can also
// say. This file computes the tree's half. The board's half is devlink's
// FP command (firmware/devlink.c), fed by BUILD_FINGERPRINT from
// firmware/CMakeLists.txt, which runs this same script at configure and
// build time - one definition, computed in one place, so the two halves
// cannot drift into two opinions about what a fingerprint is.
//
//   bun run tools/gate/fingerprint.ts            print it
//   bun run tools/gate/fingerprint.ts --short    print just the 12 hex digits
//   bun run tools/gate/fingerprint.ts --device   ask the board, and compare
//
// Only --device opens the serial port, and nothing else in this tree calls
// it: the gate does not, because a gate that grabs the port is a gate that
// cannot be run while someone is using the puck. Everything programmatic
// goes through assertDeviceMatchesTree() below, which takes the transport
// rather than creating one.
import { createHash } from "node:crypto";
import { readFileSync, readdirSync, statSync } from "node:fs";
import { dirname, join, relative, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const DEVICE_ROOT = resolve(dirname(fileURLToPath(import.meta.url)), "..", "..");

// WHAT GOES INTO THE IMAGE, and nothing else. Not the emulator, not the
// tools, not the docs: a fingerprint that changes when a README changes is
// a fingerprint that always mismatches and therefore always gets ignored.
const INPUT_DIRS = [
  join(DEVICE_ROOT, "firmware"),
  join(DEVICE_ROOT, "firmware", "runtime"),
  join(DEVICE_ROOT, "firmware", "apps"),
];
const INPUT_FILES = [join(DEVICE_ROOT, "firmware", "CMakeLists.txt")];
// .inc is here for firmware/apps/app_roster.inc (the app table) and
// firmware/apps/app_tunables.inc (the live-tune provider list): since
// 2026-08-19 both live outside the synced, pack-owned runtime sources that
// include them. They compile into the image like any other source, so a
// fingerprint that ignored them would report the same hash for two
// firmwares with different apps in them - which is the precise question
// this hash is asked ("is the board running THIS tree").
const EXTENSIONS = [".c", ".h", ".pio", ".inc"];

export interface Fingerprint {
  /** The full digest. */
  full: string;
  /** The first 12 hex digits, which is what devlink prints. */
  short: string;
  /** Every file that went into it, repo-relative, sorted. */
  files: string[];
}

function collect(): string[] {
  const out = new Set<string>(INPUT_FILES);
  for (const dir of INPUT_DIRS) {
    let entries: string[];
    try {
      entries = readdirSync(dir);
    } catch {
      throw new Error(`fingerprint: cannot read ${dir}`);
    }
    for (const name of entries) {
      const full = join(dir, name);
      if (!statSync(full).isFile()) continue;
      if (!EXTENSIONS.some((e) => name.endsWith(e))) continue;
      out.add(full);
    }
  }
  if (out.size === 0) throw new Error("fingerprint: no firmware sources found, which cannot be right");
  return [...out].sort();
}

export function treeFingerprint(): Fingerprint {
  const files = collect();
  const h = createHash("sha256");
  const rel: string[] = [];
  for (const f of files) {
    // Path AND content, so that renaming a file changes the fingerprint,
    // and line endings normalised, so that a checkout with different git
    // autocrlf settings does not read as a different firmware.
    const r = relative(DEVICE_ROOT, f).replace(/\\/g, "/");
    rel.push(r);
    h.update(r);
    h.update("\0");
    h.update(readFileSync(f, "utf8").replace(/\r\n/g, "\n"));
    h.update("\0");
  }
  const full = h.digest("hex");
  return { full, short: full.slice(0, 12), files: rel };
}

/**
 * The check every comparison against a board must pass first.
 *
 * `askDevice` is whatever can send one devlink line and read the reply -
 * the gate does not own a serial port and must not open one, so this takes
 * the transport rather than creating it. A board that answers
 * "ERR unknown FP" is a board built before fingerprinting existed, which
 * is itself the answer: it is not this tree.
 */
export async function assertDeviceMatchesTree(
  askDevice: (line: string) => Promise<string>
): Promise<void> {
  const tree = treeFingerprint();
  const reply = (await askDevice("FP")).trim();
  const m = reply.match(/^FP\s+([0-9a-f]{12})\b/);
  if (!m) {
    throw new Error(
      `the board did not report a build fingerprint (it said "${reply}").\n` +
      `  A board flashed before devlink's FP command exists cannot be compared against this tree,\n` +
      `  because nothing it says can establish that it is running it. Flash, then compare.`
    );
  }
  if (m[1] !== tree.short) {
    throw new Error(
      `the board is running a different build than this tree.\n` +
      `  board ${m[1]}\n` +
      `  tree  ${tree.short}  (${tree.files.length} sources)\n` +
      `  Any comparison from here would be measuring the flash date, not the code.`
    );
  }
}

/**
 * The --device path. Imported lazily so that merely loading this module
 * never pulls in a serial transport, and so the gate's own import of
 * treeFingerprint() cannot accidentally open a port.
 */
interface DevBridgeModule {
  openDirectBridge(): Promise<{
    send(line: string): Promise<void>;
    lines: unknown;
    close(): Promise<void>;
  }>;
  expectLine(lines: unknown, shape: RegExp, timeoutMs: number, what: string): Promise<string>;
}

async function compareAgainstBoard(): Promise<void> {
  // The specifier is held in a variable so the type checker does not pull
  // tools/dev.ts into this program. That file is large, is edited by
  // whoever is at the board that day, and its own type errors are not this
  // tool's to fail on; the two functions used here are pinned by
  // DevBridgeModule above instead.
  const devPath = "../dev";
  const dev = (await import(devPath)) as DevBridgeModule;
  const bridge = await dev.openDirectBridge();
  try {
    await assertDeviceMatchesTree(async (line) => {
      await bridge.send(line);
      return await dev.expectLine(bridge.lines, /^(FP \S+|ERR .*)$/, 3000, "FP reply");
    });
    console.log(`OK the board is running this tree (${treeFingerprint().short})`);
  } finally {
    await bridge.close();
  }
}

if (import.meta.main) {
  if (process.argv.includes("--device")) {
    await compareAgainstBoard().catch((err) => {
      console.error(err instanceof Error ? err.message : String(err));
      process.exit(1);
    });
    process.exit(0);
  }
  const fp = treeFingerprint();
  const headerAt = process.argv.indexOf("--header");
  if (headerAt >= 0) {
    // Written by firmware/CMakeLists.txt before main is linked, so the
    // image can say what it is. Rewritten only when the value actually
    // changes: an unconditional write would touch the file on every build
    // and make ninja recompile devlink.c for nothing.
    const out = process.argv[headerAt + 1];
    if (!out) throw new Error("--header needs a path");
    const body =
      `// Generated by tools/gate/fingerprint.ts. Do not edit, do not commit.\n` +
      `#define BUILD_FINGERPRINT "${fp.short}"\n`;
    let existing = "";
    try { existing = readFileSync(out, "utf8"); } catch { /* first build */ }
    if (existing !== body) {
      const { writeFileSync, mkdirSync } = await import("node:fs");
      mkdirSync(dirname(out), { recursive: true });
      writeFileSync(out, body);
    }
    console.log(`BUILD_FINGERPRINT ${fp.short} (${fp.files.length} firmware sources) -> ${out}`);
  } else if (process.argv.includes("--short")) {
    console.log(fp.short);
  } else {
    console.log(`${fp.short}  (${fp.files.length} firmware sources)`);
    console.log(`full ${fp.full}`);
    if (process.argv.includes("--files")) for (const f of fp.files) console.log(`  ${f}`);
  }
}
