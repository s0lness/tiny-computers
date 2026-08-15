// audit-core1-veneers: the by-hand check docs/decisions/0017 asks a future
// editor of core1's path (sensors.c, tilt.c, or anything they call into) to
// re-run, now automated instead of prose.
//
// WHY THIS EXISTS, AND WHY IT IS NOT WIRED INTO THE BUILD AS A SEVENTH
// INVARIANT: rule 1 (rules/rp2350-amoled-1.8.ts, "no executable byte at a
// flash VMA that core1 can reach") reuses rule 0's reachability graph,
// which follows `bl`/`b`/`b.w` directly and treats `blx r*`/`bx <reg>` and
// handler-installer calls as gated escape sites. It does NOT follow a
// fourth shape: an ARM linker veneer (`ldr.w pc, [pc]` plus a literal pool
// word), inserted whenever a `bl`'s direct-branch encoding cannot bridge a
// RAM-resident caller to a flash-resident callee or vice versa. Building
// this device's real 11-app image surfaced four real functions (`panic`,
// `FT3168_Reset`, `__wrap_atan2f`, `strlen`, `_exit`) still at flash VMAs,
// still genuinely reachable from core1, that rule 1 passed anyway, because
// it only ever saw the tiny RAM-resident veneer stub, never the real
// function past it - see decision 0017's own long account of finding and
// closing this the first time.
//
// Teaching rule 0 to model this shape generically is the correct permanent
// fix, and is NOT done here: this veneer pattern recurs throughout the
// whole image for ordinary far calls that have nothing to do with core1 at
// all (`__memcpy_veneer`, `____wrap_atan2f_veneer` on code paths this
// device's own core0 uses), so gating on it in the mandatory, build-failing
// checker is materially bigger than a mid-fix widening and deserves its own
// decision, not a silent addition here. This script is the interim
// mitigation decision 0017 names: run it by hand after touching core1's
// path, before trusting rule 1's PASS.
//
// Usage: bun run tools/invariants/audit-core1-veneers.ts [elf] [map]
import { parseRegions, parseSections, parseSymbols } from "./model";
import { parseDisassembly } from "./disasm";
import { reachableFrom } from "./graph";
import { OBJDUMP } from "./toolchain";
import { runTool } from "./run-tool";
import { allRoots, PANIC_PATH_ALREADY_LOST } from "./rules/rp2350-amoled-1.8";

const elfPath = process.argv[2] ?? "firmware/build/main.elf";
const mapPath = process.argv[3] ?? "firmware/build/main.elf.map";

const sections = parseSections(elfPath);
const symbols = parseSymbols(elfPath);
const regions = parseRegions(mapPath);
const functions = parseDisassembly(elfPath);

const flash = regions.find((r) => r.name === "FLASH");
const ram = regions.find((r) => r.name === "RAM");
if (!flash || !ram) {
  throw new Error(`audit-core1-veneers: expected FLASH and RAM regions in ${mapPath}`);
}
const inFlash = (addr: number) => addr >= flash.origin && addr < flash.origin + flash.length;
const inRam = (addr: number) => addr >= ram.origin && addr < ram.origin + ram.length;

const roots = allRoots({ elfPath, mapPath, sections, symbols, regions, functions });
const reach = reachableFrom(functions, roots);

const veneerNames = [...reach.reached].filter((n) => n.includes("_veneer"));

if (veneerNames.length === 0) {
  console.log(`No *_veneer symbols reached from core1's roots (${roots.length} roots). Nothing to audit.`);
  process.exit(0);
}

// A veneer's own disassembly is exactly two lines: the `ldr.w pc, [pc]`
// instruction, and the literal pool word right after it holding the real
// target address (with the Thumb bit set, per ARM calling convention - the
// real function's own address is one less).
const disasmText = runTool(OBJDUMP, ["-d", elfPath]);
const lines = disasmText.split(/\r?\n/);

interface VeneerResult {
  name: string;
  veneerAddr: number;
  targetAddr: number;
  where: "RAM" | "FLASH" | "OTHER";
}

const results: VeneerResult[] = [];
for (const name of veneerNames) {
  const fn = functions.get(name);
  if (!fn) throw new Error(`audit-core1-veneers: reached veneer "${name}" has no disassembled body`);
  const headerRe = new RegExp(`^${fn.addr.toString(16)} <${name.replace(/[.*+?^${}()|[\]\\]/g, "\\$&")}>:$`);
  const idx = lines.findIndex((l) => headerRe.test(l));
  if (idx === -1) {
    throw new Error(`audit-core1-veneers: could not find "${name}"'s own disassembly block`);
  }
  // Expect exactly: "ADDR: BYTES  ldr.w  pc, [pc]  @ (...)" then
  // "ADDR+4: WORDHEX  .word  0xWORDHEX".
  const wordLine = lines[idx + 2] ?? "";
  const m = /^\s*[0-9a-f]+:\s*([0-9a-f]{8})\s+\.word\s+0x[0-9a-f]+/.exec(wordLine);
  if (!m) {
    throw new Error(
      `audit-core1-veneers: "${name}" did not have the expected veneer shape ` +
        `(ldr.w pc,[pc] + literal word) - got: ${JSON.stringify(lines.slice(idx, idx + 3))}`
    );
  }
  const rawWord = parseInt(m[1]!, 16);
  const targetAddr = rawWord & ~1; // clear the Thumb bit
  const where = inRam(targetAddr) ? "RAM" : inFlash(targetAddr) ? "FLASH" : "OTHER";
  results.push({ name, veneerAddr: fn.addr, targetAddr, where });
}

results.sort((a, b) => a.veneerAddr - b.veneerAddr);
console.log(`${results.length} veneer(s) reached from core1's roots:\n`);
let bad = 0;
for (const r of results) {
  const exempt = PANIC_PATH_ALREADY_LOST.has(r.name);
  const tag = r.where === "RAM" ? "OK  " : exempt ? "WARN" : "FAIL";
  if (r.where !== "RAM" && !exempt) bad++;
  console.log(
    `  ${tag}  ${r.name} @ 0x${r.veneerAddr.toString(16)} -> 0x${r.targetAddr.toString(16)} (${r.where})`
  );
}
console.log("");
if (bad > 0) {
  console.log(
    `${bad} veneer(s) bridge to a FLASH-resident target that is not panic-path-exempt - ` +
      `this is the exact class of gap decision 0017 documents. Find the real function name ` +
      `at the printed target address and add its defining file to firmware/linker_overrides/ ` +
      `default_text_excludes.incl (and default_rodata_excludes.incl if it has separately- ` +
      `declared const data).`
  );
  process.exit(1);
}
console.log("Every veneer reached from core1 resolves to RAM (or is panic-path-exempt). Clean.");
