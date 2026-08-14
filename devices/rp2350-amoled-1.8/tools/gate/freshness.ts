// Bug 8: the tests ran against a stale binary.
//
// `emulator/wasm/tests/*.ts` load a prebuilt `emulator/wasm/dist/emu.wasm`
// and never compile anything. Edit a `.c`, run a test, and the test checks
// the PREVIOUS binary against the new expectations - it passes or fails
// about code nobody is looking at. AGENTS.md already warns about this in
// prose ("A one-shot build that is not checked will leave the PREVIOUS
// emu.wasm in place"), which is exactly the shape of failsafe this project
// has been converting into machinery: a hazard written down next to the
// thing that avoids it, and lost the moment someone does not read it.
//
// This is the mechanical version. It compares the module's mtime against
// every source that goes into it, and refuses rather than warns.
//
// WHY MTIME AND NOT A CONTENT HASH. A content hash of the sources would be
// stronger (it survives a `touch`, and it does not fire on a no-op save),
// but it needs somewhere to record the hash the module was built FROM, and
// the only honest place for that is inside the module - which means
// changing the ABI and the build for a check whose whole value is being
// cheap and unconditional. mtime answers the question actually being asked
// ("was this .c saved after that .wasm was linked?") with a stat() per
// file and no new state anywhere. Its false positive (a source touched but
// not changed) costs one rebuild; its false negative needs a clock going
// backwards. The trade is the right way round.
//
// THE SOURCE LIST IS DERIVED, NOT RESTATED. It is read out of
// emulator/wasm/build.ts by parsing the same SOURCES/INCLUDES arrays that
// file already maintains, so an app added to the build cannot be missing
// here. A build.ts this parser does not understand is a hard failure, not
// a skip, per tools/invariants/README.md's own rule about instruments that
// pass because they could not read their input.
import { existsSync, readdirSync, readFileSync, statSync } from "node:fs";
import { dirname, join, relative, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const DEVICE_ROOT = resolve(dirname(fileURLToPath(import.meta.url)), "..", "..");
const WASM_DIR = join(DEVICE_ROOT, "emulator", "wasm");
const BUILD_TS = join(WASM_DIR, "build.ts");
export const WASM_PATH = join(WASM_DIR, "dist", "emu.wasm");

export interface Staleness {
  wasmPath: string;
  wasmMtimeMs: number;
  // Sources strictly newer than the module, newest first.
  newer: { path: string; mtimeMs: number }[];
  // Every source considered, for the "what did you actually look at" line.
  consideredCount: number;
}

// Pulls the join(...) arguments out of one array literal in build.ts. The
// arrays there are written as join(FIRMWARE, "apps", "sketch.c") and
// friends, with the directory constants defined a few lines above; rather
// than evaluating build.ts (which would run a compiler), this resolves the
// handful of constant names it uses.
const DIR_CONSTANTS: Record<string, string> = {
  WASM_DIR,
  FIRMWARE: join(DEVICE_ROOT, "firmware"),
  DIST: join(WASM_DIR, "dist"),
  DEVICE_ROOT,
};

function parseArrayLiteral(src: string, name: string): string[] {
  const start = src.indexOf(`const ${name} = [`);
  if (start < 0) throw new Error(`emulator/wasm/build.ts: no "const ${name} = [" to parse`);
  const open = src.indexOf("[", start);
  let depth = 0;
  let end = -1;
  for (let i = open; i < src.length; i++) {
    if (src[i] === "[") depth++;
    else if (src[i] === "]") {
      depth--;
      if (depth === 0) { end = i; break; }
    }
  }
  if (end < 0) throw new Error(`emulator/wasm/build.ts: "${name}" array is not closed`);
  const body = src.slice(open + 1, end);

  const out: string[] = [];
  // join(CONST, "a", "b") | CONST | "literal"
  const entryRe = /join\(([^)]*)\)|\b([A-Z_][A-Z0-9_]*)\b/g;
  let m: RegExpExecArray | null;
  while ((m = entryRe.exec(body)) !== null) {
    if (m[1] !== undefined) {
      const parts = m[1].split(",").map((p) => p.trim());
      const head = parts.shift()!;
      const base = DIR_CONSTANTS[head];
      if (base === undefined) {
        throw new Error(`emulator/wasm/build.ts: ${name} uses join(${head}, ...) and this parser does not know ${head}`);
      }
      const rest = parts.map((p) => {
        const lit = p.match(/^"([^"]*)"$/);
        if (!lit) throw new Error(`emulator/wasm/build.ts: ${name} has a non-literal path segment: ${p}`);
        return lit[1]!;
      });
      out.push(join(base, ...rest));
    } else {
      const base = DIR_CONSTANTS[m[2]!];
      if (base === undefined) {
        throw new Error(`emulator/wasm/build.ts: ${name} names ${m[2]} and this parser does not know it`);
      }
      out.push(base);
    }
  }
  if (out.length === 0) throw new Error(`emulator/wasm/build.ts: parsed "${name}" as empty, which cannot be right`);
  return out;
}

// Every .c named by build.ts, plus every header in the directories it puts
// on the include path. Headers are swept by directory rather than followed
// through #include, deliberately: a coarse over-approximation costs a
// rebuild that was not strictly needed, and following includes properly
// means writing a preprocessor.
export function moduleSources(): string[] {
  if (!existsSync(BUILD_TS)) throw new Error(`no build.ts at ${BUILD_TS}`);
  const src = readFileSync(BUILD_TS, "utf8");
  const sources = parseArrayLiteral(src, "SOURCES");
  const includes = parseArrayLiteral(src, "INCLUDES");

  const files = new Set<string>([BUILD_TS, ...sources]);
  for (const dir of includes) {
    if (!existsSync(dir)) throw new Error(`emulator/wasm/build.ts puts ${dir} on the include path and it does not exist`);
    for (const name of readdirSync(dir)) {
      if (name.endsWith(".h") || name.endsWith(".c")) files.add(join(dir, name));
    }
  }
  for (const f of files) {
    if (!existsSync(f)) throw new Error(`emulator/wasm/build.ts names a source that does not exist: ${f}`);
  }
  return [...files].sort();
}

export function checkFreshness(): Staleness {
  if (!existsSync(WASM_PATH)) {
    throw new Error(
      `no module at ${WASM_PATH}\n` +
      `  fix: bun run emulator/wasm/build.ts`
    );
  }
  const wasmMtimeMs = statSync(WASM_PATH).mtimeMs;
  const sources = moduleSources();
  const newer = sources
    .map((p) => ({ path: p, mtimeMs: statSync(p).mtimeMs }))
    .filter((s) => s.mtimeMs > wasmMtimeMs)
    .sort((a, b) => b.mtimeMs - a.mtimeMs);
  return { wasmPath: WASM_PATH, wasmMtimeMs, newer, consideredCount: sources.length };
}

export function stalenessMessage(s: Staleness): string {
  const when = (ms: number) => new Date(ms).toISOString().replace("T", " ").slice(0, 19);
  const lines = [
    `emu.wasm is older than ${s.newer.length} of the ${s.consideredCount} sources it is built from.`,
    `  ${relative(DEVICE_ROOT, s.wasmPath).replace(/\\/g, "/")}  linked ${when(s.wasmMtimeMs)}`,
  ];
  for (const n of s.newer.slice(0, 8)) {
    lines.push(`  ${relative(DEVICE_ROOT, n.path).replace(/\\/g, "/")}  saved ${when(n.mtimeMs)}`);
  }
  if (s.newer.length > 8) lines.push(`  ... and ${s.newer.length - 8} more`);
  lines.push(``);
  lines.push(`Whatever you are about to conclude would be about the PREVIOUS build.`);
  lines.push(`  fix: bun run emulator/wasm/build.ts`);
  return lines.join("\n");
}

// What a test file calls at startup. Throws (rather than returning a
// boolean nobody checks) so that forgetting to handle it still stops the
// run.
export function assertFresh(): void {
  const s = checkFreshness();
  if (s.newer.length > 0) throw new Error(stalenessMessage(s));
}

if (import.meta.main) {
  try {
    const s = checkFreshness();
    if (s.newer.length > 0) {
      console.error(stalenessMessage(s));
      process.exit(1);
    }
    console.log(`emu.wasm is newer than all ${s.consideredCount} of its sources`);
  } catch (err) {
    console.error(err instanceof Error ? err.message : String(err));
    process.exit(1);
  }
}
