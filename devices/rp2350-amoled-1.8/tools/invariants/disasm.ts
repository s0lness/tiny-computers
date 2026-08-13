// Parses `objdump -d` into per-function branch/indirect-call lists. This is
// the input the core1 reachability BFS (graph.ts) walks. Not a disassembler
// in its own right: it trusts objdump's own symbol resolution (the name in
// `<...>` after a branch target) rather than re-deriving it from addresses.
//
// Same rule as model.ts: a line whose *shape* is not one of the four this
// function knows about fails the run. Lines whose shape is understood but
// whose mnemonic is not one we act on (the overwhelming majority - loads,
// arithmetic, compares) are fine to pass through unrecorded; that is a
// choice about what matters to the graph, not a line being skipped blind.

import { OBJDUMP } from "./toolchain";
import { runTool } from "./run-tool";
import type { DisasmBranch, DisasmFunction, IndirectSite } from "./types";

const FILE_FORMAT_LINE = /file format /;
const SECTION_HEADER = /^Disassembly of section .+:$/;
const FUNCTION_HEADER = /^([0-9a-f]+) <(.+)>:$/;
const INSTRUCTION_ADDR = /^([0-9a-f]+):$/;

// objdump falls back to two other shapes inside sections that carry the
// CODE flag but hold real data, not instructions - this build's .data does
// (see the ELF/model.ts side: .data is CONTENTS,ALLOC,LOAD,CODE), because
// g_apps (the app function-pointer table) lives there. Neither shape can
// contain a bl/b/b.w/blx: both are recognised and skipped deliberately,
// not silently, which is the distinction decision 0004 cares about.
//
//   20011b28:	20011b70 20011b5c 20011b9c              p.. \.. ...
// one to four 32-bit words (not the 16-bit halfword groups a real Thumb
// instruction's byte column uses) followed by objdump's own ASCII gutter.
const RAW_DATA_ROW = /^([0-9a-f]{8})( [0-9a-f]{8}){0,3} {2,}/;
// objdump elides a run of identical-to-the-previous-line data rows as a
// bare "...", which by construction cannot differ from what was already
// shown, so it cannot hide a branch either.
const ELISION_LINE = "\t...";

const DIRECT_BRANCH_MNEMONICS = new Set(["bl", "b", "b.w"]);
// The bracket objdump appends to a resolved operand, e.g.
// "2000e2b8 <exception_set_exclusive_handler>" or "20001234 <foo+0x1c>".
const TARGET_NAME = /<([^+>]+)(?:\+0x[0-9a-f]+)?>\s*$/;
// `blx`/`bx` (excluding plain `bx lr`, an ordinary return) taking a register
// operand is a call through a value the graph cannot see - rule 0's gate.
const REGISTER_OPERAND = /^(r\d|r1[0-5]|sp|lr|pc|fp|ip|sl|sb)$/;

export function parseDisassembly(elfPath: string): Map<string, DisasmFunction> {
  const text = runTool(OBJDUMP, ["-d", elfPath]);
  const functions = new Map<string, DisasmFunction>();
  let current: DisasmFunction | null = null;

  const finish = () => {
    if (current) functions.set(current.name, current);
    current = null;
  };

  for (const rawLine of text.split(/\r?\n/)) {
    const line = rawLine.replace(/\r$/, "");
    if (line.trim() === "") continue;
    if (FILE_FORMAT_LINE.test(line)) continue;
    if (SECTION_HEADER.test(line)) continue;

    const fh = FUNCTION_HEADER.exec(line);
    if (fh) {
      finish();
      current = { name: fh[2]!, addr: parseInt(fh[1]!, 16), branches: [], indirect: [] };
      continue;
    }

    if (line === ELISION_LINE) continue;

    // Instruction line: "ADDR:\tHEXBYTES\tMNEMONIC[\tOPERAND[\t@ COMMENT]]"
    const cols = line.split("\t");
    const addrMatch = INSTRUCTION_ADDR.exec(cols[0] ?? "");

    if (addrMatch && cols.length === 2 && RAW_DATA_ROW.test(cols[1]!)) continue;

    if (!addrMatch || cols.length < 3) {
      throw new Error(`objdump -d ${elfPath}: unparsable line: ${JSON.stringify(line)}`);
    }
    if (!current) {
      throw new Error(
        `objdump -d ${elfPath}: instruction line before any function header: ${JSON.stringify(line)}`
      );
    }
    const addr = parseInt(addrMatch[1]!, 16);
    const mnemonic = cols[2]!.trim();
    const operand = (cols[3] ?? "").trim();

    if (DIRECT_BRANCH_MNEMONICS.has(mnemonic)) {
      const tm = TARGET_NAME.exec(operand);
      if (!tm) {
        throw new Error(
          `objdump -d ${elfPath}: ${mnemonic} at ${addrMatch[1]} has no resolvable target: ` +
            JSON.stringify(line)
        );
      }
      const targetName = tm[1]!;
      // A branch back into the function currently being scanned is a local
      // jump (loop, if/else, tail-merge), not an edge to a different
      // function - the whole reason this is compared by name rather than
      // by walking address ranges.
      if (targetName !== current.name) {
        current.branches.push({ addr, mnemonic: mnemonic as DisasmBranch["mnemonic"], targetName });
      }
      continue;
    }

    // Rule 0, verbatim from decision 0006, is scoped to "every blx r* site".
    // `bx <reg other than lr>` is the same hazard in spirit (a branch
    // through a value the graph cannot see) and this build does contain a
    // handful of them, outside stdio - see the invariant checker's own
    // README for why that gap is left open rather than silently widened.
    if (mnemonic === "blx") {
      if (!REGISTER_OPERAND.test(operand)) {
        throw new Error(
          `objdump -d ${elfPath}: ${mnemonic} at ${addrMatch[1]} has an unexpected operand: ` +
            JSON.stringify(operand)
        );
      }
      current.indirect.push({ addr, mnemonic, operand } satisfies IndirectSite);
    }
  }
  finish();

  if (functions.size === 0) {
    throw new Error(`objdump -d ${elfPath}: found zero functions`);
  }
  return functions;
}
