// The contracts several agents share, compared mechanically.
//
// Decision 0014: this repo is written by agents working in parallel
// worktrees who cannot see each other, and every collision so far was
// caught by a human reading reports. Each collision had the same shape - a
// contract that exists as several hand-maintained copies with no comparator:
//
//   - the app table exists three times (app_roster.inc's g_apps[], build.ts
//     SOURCES, CMakeLists.txt target_sources), and commit 7a15a80 ("unbreak
//     main, which I broke by committing another agent's file") is what a
//     dropped copy looks like;
//   - the emulator ABI exists three times (emu_abi.h, build.ts EMU_EXPORTS,
//     the module's actual export table), and two agents shipped the same
//     sensor call under two names (emu_sensor_vector / emu_sensor_vec3) in
//     the same night;
//   - the "one signal" contracts (sensors.h, tilt.h) hold only until an app
//     grows a private seam, which the level app did (level.h, included by
//     emu_shim.c) the same week decision 0012 paid to remove that pattern;
//   - the tests' APP_* index constants are a fourth copy of the app table,
//     with comments that are already stale ("g_apps[] = { chrono, sketch,
//     timer }" while four exists).
//
// Every check below compares copies of ONE contract against each other, or
// against the firmware itself, and names the copy that disagrees. None of
// them evaluates taste, layout or behaviour - the rest of the gate does
// that. Same posture as the rest of this directory: a rule carries its
// reason, a failure prints it, and an input this file cannot parse is a
// failure, never a skip.

import { existsSync, readFileSync, readdirSync } from "node:fs";
import { basename, dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { compiledModule } from "./device";
import type { Violation } from "./rules";

const DEVICE_ROOT = resolve(dirname(fileURLToPath(import.meta.url)), "..", "..");
const FIRMWARE = join(DEVICE_ROOT, "firmware");
const APPS_DIR = join(FIRMWARE, "apps");
// The app table used to live in runtime_core.c. It moved to
// firmware/apps/app_roster.inc on 2026-08-19, when the runtime became a
// SYNCED COPY of the puck device pack (tools/sync-pack.ts): a shared runtime
// cannot carry one consumer's app list, and this parser has to follow the
// contract, not the old address. runtime_core.c is not read here at all any
// more - if a future edit puts a g_apps[] back into it, this file will not
// see it, and the app-wiring rule below is the thing that would go quiet.
const APP_ROSTER = join(APPS_DIR, "app_roster.inc");
const CMAKELISTS = join(FIRMWARE, "CMakeLists.txt");
const BUILD_TS = join(DEVICE_ROOT, "emulator", "wasm", "build.ts");
const EMU_ABI_H = join(DEVICE_ROOT, "emulator", "wasm", "emu_abi.h");
const EMU_SHIM_C = join(DEVICE_ROOT, "emulator", "wasm", "emu_shim.c");
const TESTS_DIR = join(DEVICE_ROOT, "emulator", "wasm", "tests");

const SEE = "docs/decisions/0014-six-agents-one-contract.md";

/** C sources with comments and string/char literals blanked, so a token in
 * a comment ("core1 owns i2c1") can never accuse the code beside it. Line
 * structure is preserved (newlines survive), so #if parsing stays honest. */
export function stripC(src: string): string {
  let out = "";
  let i = 0;
  const n = src.length;
  while (i < n) {
    const c = src[i]!;
    const d = i + 1 < n ? src[i + 1]! : "";
    if (c === "/" && d === "/") {
      while (i < n && src[i] !== "\n") i++;
    } else if (c === "/" && d === "*") {
      i += 2;
      while (i < n && !(src[i] === "*" && src[i + 1] === "/")) {
        if (src[i] === "\n") out += "\n";
        i++;
      }
      i += 2;
    } else if (c === '"' || c === "'") {
      const q = c;
      i++;
      while (i < n && src[i] !== q) {
        if (src[i] === "\\") i++;
        i++;
      }
      i++;
      out += q === '"' ? '""' : "''";
    } else {
      out += c;
      i++;
    }
  }
  return out;
}

/* ===========================================================================
 * 1. The app table, in its three copies.
 * ======================================================================= */

export interface TableEntry {
  token: string; // "chrono" for &g_chronoApp; "stub[3]" for &g_stubApps[3]
  gatedBy: string | null; // the innermost #if condition, or null
}

export interface AppWiring {
  /** app token -> defining file basename, from `const app_t g_<t>App =` in apps/*.c */
  defined: Map<string, string>;
  table: TableEntry[];
  /** basenames of apps/*.c named by build.ts SOURCES */
  wasmAppSources: string[];
  /** basenames of apps/*.c named by CMakeLists target_sources(main ...) */
  boardAppSources: string[];
}

/** Apps defined but legitimately absent from g_apps[], with the reason. */
const TABLE_EXEMPT: Record<string, string> = {
  menu: "the menu is APP_INDEX_MENU (-1), dispatched by app_for_index(), deliberately not a g_apps[] entry",
};

/** apps/*.c legitimately in one build and not the other, with the reason. */
const BUILD_EXEMPT: Record<string, { where: "wasm-only" | "board-only"; why: string }> = {
  "stubapps.c": {
    where: "wasm-only",
    why: "screenshot fixture; deliberately absent from CMakeLists so a stub app cannot reach a uf2 (stubapps.c's own header)",
  },
};

export function parseAppWiring(): AppWiring {
  // Definitions, from every .c under firmware/apps/.
  const defined = new Map<string, string>();
  for (const name of readdirSync(APPS_DIR)) {
    if (!name.endsWith(".c")) continue;
    const src = stripC(readFileSync(join(APPS_DIR, name), "utf8"));
    for (const m of src.matchAll(/\bconst\s+app_t\s+g_(\w+)App\s*=/g)) {
      defined.set(m[1]!, name);
    }
  }

  // The table, from firmware/apps/app_roster.inc, with #if nesting tracked so a gated
  // entry is known to be gated.
  const core = stripC(readFileSync(APP_ROSTER, "utf8"));
  const tableStart = core.search(/const\s+app_t\s*\*\s*const\s+g_apps\s*\[\s*\]\s*=/);
  if (tableStart < 0) throw new Error("firmware/apps/app_roster.inc: no `const app_t *const g_apps[] =` to parse");
  const open = core.indexOf("{", tableStart);
  const close = core.indexOf("}", open);
  if (open < 0 || close < 0) throw new Error("firmware/apps/app_roster.inc: g_apps[] initialiser is not brace-delimited the way this parser expects");
  const body = core.slice(open + 1, close);

  const table: TableEntry[] = [];
  const condStack: string[] = [];
  // \r?\n, because a CRLF checkout otherwise leaves a \r on every line and
  // a $-anchored match quietly sees nothing - which for THIS parser means
  // every gated entry reads as ungated, silently.
  for (const line of body.split(/\r?\n/)) {
    const pp = line.match(/^\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)$/);
    if (pp) {
      const kind = pp[1]!;
      if (kind === "if" || kind === "ifdef" || kind === "ifndef") condStack.push(`${kind}${pp[2] ?? ""}`.trim());
      else if (kind === "elif" || kind === "else") {
        if (condStack.length > 0) condStack[condStack.length - 1] = `${kind}${pp[2] ?? ""}`.trim();
      } else if (kind === "endif") condStack.pop();
      continue;
    }
    const gatedBy = condStack.length > 0 ? condStack[condStack.length - 1]! : null;
    for (const m of line.matchAll(/&g_(\w+)App\b/g)) table.push({ token: m[1]!, gatedBy });
    for (const m of line.matchAll(/&g_stubApps\s*\[\s*(\d+)\s*\]/g)) table.push({ token: `stub[${m[1]}]`, gatedBy });
  }
  if (table.length === 0) throw new Error("firmware/apps/app_roster.inc: parsed g_apps[] as empty, which cannot be right");

  // build.ts SOURCES.
  const buildSrc = readFileSync(BUILD_TS, "utf8");
  const srcStart = buildSrc.indexOf("const SOURCES = [");
  if (srcStart < 0) throw new Error("emulator/wasm/build.ts: no `const SOURCES = [` to parse");
  const srcEnd = buildSrc.indexOf("];", srcStart);
  const srcBody = buildSrc.slice(srcStart, srcEnd);
  const wasmAppSources = [...srcBody.matchAll(/join\(FIRMWARE,\s*"apps",\s*"([^"]+)"\)/g)].map((m) => m[1]!);
  if (wasmAppSources.length === 0) throw new Error("emulator/wasm/build.ts: SOURCES names no apps/*.c, which cannot be right");

  // CMakeLists target_sources.
  const cmakeRaw = readFileSync(CMAKELISTS, "utf8")
    .split("\n")
    .map((l) => l.replace(/#.*$/, "")) // cmake comments hold parens; strip first
    .join("\n");
  const tsStart = cmakeRaw.search(/target_sources\s*\(\s*main\s+PRIVATE/);
  if (tsStart < 0) throw new Error("firmware/CMakeLists.txt: no `target_sources(main PRIVATE` to parse");
  let depth = 0;
  let tsEnd = -1;
  for (let i = cmakeRaw.indexOf("(", tsStart); i < cmakeRaw.length; i++) {
    if (cmakeRaw[i] === "(") depth++;
    else if (cmakeRaw[i] === ")") {
      depth--;
      if (depth === 0) { tsEnd = i; break; }
    }
  }
  if (tsEnd < 0) throw new Error("firmware/CMakeLists.txt: target_sources(main ...) is not closed");
  const tsBody = cmakeRaw.slice(tsStart, tsEnd);
  const boardAppSources = [...tsBody.matchAll(/\bapps\/([\w.-]+\.c)\b/g)].map((m) => m[1]!);
  if (boardAppSources.length === 0) throw new Error("firmware/CMakeLists.txt: target_sources names no apps/*.c, which cannot be right");

  return { defined, table, wasmAppSources, boardAppSources };
}

export function diffAppWiring(w: AppWiring): Violation[] {
  const out: Violation[] = [];
  const v = (detail: string) =>
    out.push({
      rule: "app-wiring",
      why: "the app table exists as three hand-maintained copies (g_apps[], build.ts SOURCES, CMakeLists); a copy dropped in a merge ships a build where the app silently does not exist on one target",
      see: SEE,
      detail,
    });

  const tableTokens = new Set(w.table.map((t) => t.token));

  for (const [token, file] of w.defined) {
    if (tableTokens.has(token)) continue;
    if (token in TABLE_EXEMPT) continue;
    v(`firmware/apps/${file} defines g_${token}App and g_apps[] (firmware/apps/app_roster.inc) never references it: the app compiles and is unreachable. If a merge dropped the table entry, this is it.`);
  }

  for (const t of w.table) {
    if (t.token.startsWith("stub[")) continue; // stubapps.c, handled by BUILD_EXEMPT
    const file = w.defined.get(t.token);
    if (file === undefined) {
      v(`g_apps[] references &g_${t.token}App and no file under firmware/apps/ defines it: the wasm link will fail, or worse, only the board link will.`);
      continue;
    }
    if (!w.wasmAppSources.includes(file)) {
      v(`g_apps[] runs ${file} but emulator/wasm/build.ts SOURCES does not compile it: the emulator, the tests and this gate would all be blind to it.`);
    }
    if (!w.boardAppSources.includes(file)) {
      v(`g_apps[] runs ${file} but firmware/CMakeLists.txt does not compile it: the gate would pass and the BOARD build would fail to link, which nobody sees until flash time.`);
    }
  }

  for (const file of w.wasmAppSources) {
    if (w.boardAppSources.includes(file)) continue;
    const ex = BUILD_EXEMPT[file];
    if (ex && ex.where === "wasm-only") continue;
    v(`emulator/wasm/build.ts compiles apps/${file} and firmware/CMakeLists.txt does not: the two targets are drifting apart. Add it to CMakeLists or argue it in BUILD_EXEMPT (tools/gate/contracts.ts).`);
  }
  for (const file of w.boardAppSources) {
    if (w.wasmAppSources.includes(file)) continue;
    const ex = BUILD_EXEMPT[file];
    if (ex && ex.where === "board-only") continue;
    v(`firmware/CMakeLists.txt compiles apps/${file} and emulator/wasm/build.ts does not: an app the board runs that no test or gate rule has ever seen.`);
  }

  // The exemptions rot like any baseline: an entry that no longer applies
  // fails the run (known.ts's rule, same reason).
  for (const [token, why] of Object.entries(TABLE_EXEMPT)) {
    if (!w.defined.has(token)) {
      v(`TABLE_EXEMPT names g_${token}App ("${why}") and nothing defines it any more: delete the entry`);
    }
  }
  for (const [file, ex] of Object.entries(BUILD_EXEMPT)) {
    const inWasm = w.wasmAppSources.includes(file);
    const inBoard = w.boardAppSources.includes(file);
    const stillApplies = ex.where === "wasm-only" ? inWasm && !inBoard : inBoard && !inWasm;
    if (!stillApplies) {
      v(`BUILD_EXEMPT names apps/${file} as ${ex.where} and that is no longer true: delete the entry`);
    }
  }

  return out;
}

/* ===========================================================================
 * 2. The emulator ABI, in its three copies.
 * ======================================================================= */

export interface AbiSurface {
  /** emu_* functions declared in emu_abi.h */
  header: string[];
  /** EMU_EXPORTS in build.ts */
  exportsList: string[];
  /** emu_* function exports of the built module */
  module: string[];
}

export function parseAbiSurface(): AbiSurface {
  const header = [
    ...new Set(
      [...stripC(readFileSync(EMU_ABI_H, "utf8")).matchAll(/\b(?:void|int|float|uint32_t)\s+\**\s*(emu_\w+)\s*\(/g)].map(
        (m) => m[1]!
      )
    ),
  ];
  if (header.length === 0) throw new Error("emu_abi.h: parsed zero emu_* declarations, which cannot be right");

  const buildSrc = readFileSync(BUILD_TS, "utf8");
  const start = buildSrc.indexOf("const EMU_EXPORTS = [");
  if (start < 0) throw new Error("emulator/wasm/build.ts: no `const EMU_EXPORTS = [` to parse");
  const end = buildSrc.indexOf("];", start);
  const exportsList = [...buildSrc.slice(start, end).matchAll(/"(emu_\w+)"/g)].map((m) => m[1]!);
  if (exportsList.length === 0) throw new Error("emulator/wasm/build.ts: EMU_EXPORTS parsed empty, which cannot be right");

  const module = WebAssembly.Module.exports(compiledModule())
    .filter((e) => e.kind === "function" && e.name.startsWith("emu_"))
    .map((e) => e.name);

  return { header, exportsList, module };
}

export function diffAbiSurface(s: AbiSurface): Violation[] {
  const out: Violation[] = [];
  const v = (detail: string) =>
    out.push({
      rule: "abi-parity",
      why: "the emulator ABI exists as three copies (emu_abi.h, build.ts EMU_EXPORTS, the module's export table); the night two agents shipped emu_sensor_vector and emu_sensor_vec3 for the same physical sensor, no instrument compared any of them",
      see: SEE,
      detail,
    });

  const header = new Set(s.header);
  const exportsList = new Set(s.exportsList);
  const module = new Set(s.module);

  for (const name of s.header) {
    if (!exportsList.has(name)) {
      v(`${name} is declared in emu_abi.h and missing from build.ts EMU_EXPORTS: the JS side finds undefined at call time, in whichever test happens to call it first.`);
    }
  }
  for (const name of s.exportsList) {
    if (!header.has(name)) {
      v(`${name} is exported by build.ts and not declared in emu_abi.h: an ABI function that exists only in the build script is invisible to the next agent reading the contract, which is how the same function ships twice under two names.`);
    }
    if (!module.has(name)) {
      v(`${name} is in EMU_EXPORTS and absent from the built module's export table: the module was built by a different EMU_EXPORTS than this tree's.`);
    }
  }
  for (const name of s.module) {
    if (!exportsList.has(name)) {
      v(`the built module exports ${name}, which build.ts EMU_EXPORTS does not name: the module and the tree disagree about the ABI.`);
    }
  }
  return out;
}

/* ===========================================================================
 * 3. The shim stays below the app layer.
 * ======================================================================= */

export function checkShimPurity(shimSource: string, appsHeaderNames: string[]): Violation[] {
  const out: Violation[] = [];
  const v = (detail: string) =>
    out.push({
      rule: "shim-purity",
      why: "emu_shim.c is the emulator's stand-in for the BOARD (sensors, panel, buttons); the moment it reaches into an app it becomes a private seam for that app, which is the written-twice pattern decisions 0008 and 0012 each cost a rework to remove - and which the level branch reintroduced (level.h) the same week",
      see: SEE,
      detail,
    });

  const stripped = stripC(shimSource);
  // Includes are inside string literals which stripC blanks, so read them
  // from the raw source instead.
  for (const m of shimSource.matchAll(/^\s*#\s*include\s+"([^"]+)"/gm)) {
    const inc = basename(m[1]!);
    if (appsHeaderNames.includes(inc)) {
      v(`emu_shim.c includes "${m[1]}", a header under firmware/apps/: the shim implements runtime seams (sensors.h, gfx, sound), never an app's own. Publish the signal through the runtime instead.`);
    }
  }
  for (const m of stripped.matchAll(/\bg_(\w+)App\b/g)) {
    v(`emu_shim.c references g_${m[1]}App directly: apps are reached through the app table and the runtime, never from the shim.`);
  }
  return out;
}

/* ===========================================================================
 * 4. Apps read signals, never chips. (Decision 0012's owed rule, the source
 *    half; the linked-image reachability half still needs the ARM build.)
 * ======================================================================= */

const APP_FORBIDDEN: { re: RegExp; why: string }[] = [
  { re: /\b(QMI8658_\w+)\b/, why: "the IMU is core1's chip; apps read app_frame_t.tilt / shaken (tilt.h, decision 0012)" },
  { re: /\b(FT3168_\w+)\b/, why: "the touch controller is core1's chip; apps read the frame struct or sensors_touch_next()" },
  { re: /\b(AXP2101\w*|pmic_\w+)\b/, why: "the PMIC is core1's chip; apps read frame.key" },
  { re: /\b(i2c\d?_(?:read|write)\w*|i2c_(?:read|write)_blocking\w*)\b/, why: "no app touches i2c: core1 owns the bus (sensors.h's ownership rule)" },
  { re: /\b(flash_range_erase|flash_range_program)\b/, why: "no app writes flash: a sector erase stalls the loop against core1's 1500ms limit and a power cut mid-erase corrupts silently (decision 0010's storage section); a runtime storage service is the only allowed path" },
  { re: /\b(watchdog_\w+)\b/, why: "the watchdog is the runtime's; an app feeding it can mask its own hang" },
  { re: /\b(multicore_\w+)\b/, why: "core1 is launched once, by sensors_start(); apps never touch core plumbing" },
  { re: /\b(tilt_submit_device_g)\b/, why: "only the sensor side (core1 / emu_shim) feeds the tilt signal; an app feeding it is a second producer of the one signal" },
];

export function checkAppHygiene(fileName: string, srcStripped: string): Violation[] {
  const out: Violation[] = [];
  for (const f of APP_FORBIDDEN) {
    const m = srcStripped.match(f.re);
    if (m) {
      out.push({
        rule: "app-hygiene",
        why: "apps read signals, never chips: every hardware rule in this firmware (bus ownership, flash/chip-select, watchdog) is enforced one layer below the apps, and an app that reaches past that layer reopens a hazard that already cost a debugging day",
        see: SEE,
        detail: `firmware/apps/${fileName} references ${m[1]}: ${f.why}`,
      });
    }
  }
  return out;
}

/* ===========================================================================
 * 5. Tests address apps by constants that must match the firmware.
 * ======================================================================= */

export function checkTestAppConstants(fileName: string, src: string, appNames: string[]): Violation[] {
  const out: Violation[] = [];
  const why =
    "test APP_* constants are a fourth hand-maintained copy of the app table; one insertion reorders every index and every test silently drives the wrong app";
  const norm = (s: string) => s.toLowerCase().replace(/[^a-z0-9]/g, "");
  for (const m of src.matchAll(/\bconst\s+APP_([A-Z0-9_]+)\s*=\s*(\d+)\b/g)) {
    const name = m[1]!;
    if (name === "INDEX_MENU") continue;
    const idx = Number(m[2]!);
    const declaredAt = appNames.findIndex((a) => norm(a) === norm(name));
    if (declaredAt >= 0) {
      // The constant names a real app: its value must be that app's index.
      if (declaredAt !== idx) {
        out.push({
          rule: "test-app-index",
          why,
          see: SEE,
          detail: `emulator/wasm/tests/${fileName}: APP_${name} = ${idx}, but "${appNames[declaredAt]}" is g_apps[${declaredAt}] (apps: ${appNames.join(", ")}). Fix the constant, or the table order changed under every test.`,
        });
      }
    } else if (idx < appNames.length) {
      // The constant points into the table under a name the firmware does
      // not declare: it is driving g_apps[idx], whatever that now is.
      out.push({
        rule: "test-app-index",
        why,
        see: SEE,
        detail: `emulator/wasm/tests/${fileName}: APP_${name} = ${idx}, and g_apps[${idx}] is "${appNames[idx]}", not anything called ${name.toLowerCase()}. The test drives whatever app now sits at that index.`,
      });
    }
    // else: a numeric APP_* constant that names no app and is not a valid
    // index (APP_ARENA_BYTES = 65536, or a test for an app gated out of
    // this build, which skips itself). Not an index; not this rule's.
  }
  return out;
}

/* ===========================================================================
 * 6. Tests that simulate the controller must be deterministic.
 * ======================================================================= */

/**
 * Files allowed to construct TouchSim without a seeded rng, and why. Same
 * anti-rot contract as cleaninput.ts: an entry that no longer applies
 * fails the run. NO NEW ENTRIES: a new test seeds (TouchDriver, or
 * seededRng from tools/gate/touch.ts as TouchSim's fourth argument).
 */
export const UNSEEDED_BASELINE: Record<string, string> = {
  "repro-timer-coil.ts":
    "predates the rule and documents the unseeded draw in its own comment (line ~731). Owed a seeded rng.",
  "repro-touch-dropout-palette-open.ts": "predates the rule. Owed a seeded rng.",
};

/** Counts `new TouchSim(...)` constructions with fewer than 4 arguments
 * (the 4th is the rng; absent, TouchSim falls back to Math.random).
 * Splits at depth-1 commas and drops a trailing empty segment, so a
 * multi-line call with a trailing comma (repro-timer-coil.ts's shape)
 * counts its real arguments, not its commas. */
export function countUnseededTouchSim(src: string): number {
  let count = 0;
  let at = 0;
  for (;;) {
    const hit = src.indexOf("new TouchSim(", at);
    if (hit < 0) break;
    let i = hit + "new TouchSim".length;
    let depth = 0;
    const segments: string[] = [""];
    for (; i < src.length; i++) {
      const c = src[i]!;
      if (c === "(" || c === "[" || c === "{") {
        depth++;
        if (depth > 1) segments[segments.length - 1] += c;
      } else if (c === ")" || c === "]" || c === "}") {
        depth--;
        if (depth === 0) break;
        segments[segments.length - 1] += c;
      } else if (c === "," && depth === 1) {
        segments.push("");
      } else if (depth >= 1) {
        segments[segments.length - 1] += c;
      }
    }
    const args = segments.map((s) => s.trim()).filter((s) => s.length > 0);
    if (args.length > 0 && args.length < 4) count++;
    at = i + 1;
  }
  return count;
}

export function checkTouchSimSeeding(
  files: { name: string; src: string }[],
  baseline: Record<string, string>
): Violation[] {
  const out: Violation[] = [];
  const why =
    "TouchSim defaults its rng to Math.random, so an unseeded test is a different test every run; repro-touch-dropout-stroke-start.ts is the documented flake this rule exists to stop multiplying";
  const unseededByFile = new Map<string, number>();
  for (const f of files) unseededByFile.set(f.name, countUnseededTouchSim(f.src));

  for (const f of files) {
    const n = unseededByFile.get(f.name)!;
    if (n > 0 && !(f.name in baseline)) {
      out.push({
        rule: "test-determinism",
        why,
        see: SEE,
        detail: `emulator/wasm/tests/${f.name} constructs TouchSim ${n} time(s) without an rng: pass seededRng(...) (tools/gate/touch.ts) as the fourth argument, or drive through TouchDriver.`,
      });
    }
  }
  for (const [name, reason] of Object.entries(baseline)) {
    const f = files.find((x) => x.name === name);
    if (!f) {
      out.push({
        rule: "test-determinism",
        why: "an exemption for a file that is gone is an exemption nobody re-argued",
        see: SEE,
        detail: `UNSEEDED_BASELINE names ${name}, which is not in emulator/wasm/tests/ any more: delete the entry`,
      });
    } else if (unseededByFile.get(name)! === 0) {
      out.push({
        rule: "test-determinism",
        why: "an exemption that is no longer needed reads as permission, and the next file copies it",
        see: SEE,
        detail: `UNSEEDED_BASELINE exempts ${name} ("${reason}") and it no longer constructs an unseeded TouchSim: delete the entry`,
      });
    }
  }
  return out;
}

/* ===========================================================================
 * 7. The menu grid's ceiling (decision 0013).
 * ======================================================================= */

export function checkAppCount(declared: number, max: number): Violation[] {
  if (declared <= max) return [];
  return [{
    rule: "app-count-ceiling",
    why: `the menu grid holds ${max} apps at 112px cells (decision 0013); past that menu.c clamps rows and narrows cells, so nothing visibly breaks while every target slides under a child's fingertip`,
    see: "docs/decisions/0013-the-menu-is-a-grid-and-nothing-is-hidden.md",
    detail: `the firmware declares ${declared} apps and the grid's ceiling is ${max}: a second menu design is owed before this app, not after`,
  }];
}

/* ===========================================================================
 * The collector run.ts calls.
 * ======================================================================= */

export interface ContractsReport {
  violations: Violation[];
  /** Table entries behind preprocessor conditions: present in the source,
   * absent from this run's module, therefore NOT gated by this run. */
  gated: TableEntry[];
  summary: string[];
}

export function runContracts(appNames: string[], appCountMax: number): ContractsReport {
  for (const p of [APP_ROSTER, CMAKELISTS, BUILD_TS, EMU_ABI_H, EMU_SHIM_C]) {
    if (!existsSync(p)) throw new Error(`contracts: required input missing: ${p}`);
  }

  const violations: Violation[] = [];
  const summary: string[] = [];

  const wiring = parseAppWiring();
  violations.push(...diffAppWiring(wiring));
  const gated = wiring.table.filter((t) => t.gatedBy !== null);
  summary.push(
    `  app table: ${wiring.table.length} entries in g_apps[], ${wiring.defined.size} app_t definitions, ` +
      `${wiring.wasmAppSources.length} apps/*.c in build.ts, ${wiring.boardAppSources.length} in CMakeLists`
  );

  const abi = parseAbiSurface();
  violations.push(...diffAbiSurface(abi));
  summary.push(
    `  emulator ABI: ${abi.header.length} declared, ${abi.exportsList.length} exported by build.ts, ${abi.module.length} in the module`
  );

  const shimSrc = readFileSync(EMU_SHIM_C, "utf8");
  const appsHeaders = readdirSync(APPS_DIR).filter((n) => n.endsWith(".h"));
  violations.push(...checkShimPurity(shimSrc, appsHeaders));

  let hygieneFiles = 0;
  for (const name of readdirSync(APPS_DIR)) {
    if (!name.endsWith(".c")) continue;
    hygieneFiles++;
    violations.push(...checkAppHygiene(name, stripC(readFileSync(join(APPS_DIR, name), "utf8"))));
  }
  summary.push(`  shim purity + app hygiene: emu_shim.c, ${hygieneFiles} app sources`);

  const testFiles = readdirSync(TESTS_DIR)
    .filter((n) => n.endsWith(".ts"))
    .map((name) => ({ name, src: readFileSync(join(TESTS_DIR, name), "utf8") }));
  if (testFiles.length === 0) throw new Error(`contracts: no test files under ${TESTS_DIR}`);
  for (const f of testFiles) violations.push(...checkTestAppConstants(f.name, f.src, appNames));
  violations.push(...checkTouchSimSeeding(testFiles, UNSEEDED_BASELINE));
  summary.push(`  ${testFiles.length} test files: APP_* constants checked against the firmware, TouchSim seeding checked`);

  violations.push(...checkAppCount(appNames.length, appCountMax));

  return { violations, gated, summary };
}
