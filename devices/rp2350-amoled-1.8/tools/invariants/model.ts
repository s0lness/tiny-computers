// Builds the section/symbol/region model from binutils text output. Not an
// ELF parser (docs/decisions/0006): `objdump -h` for sections, `nm -S` for
// symbols, the linker map's "Memory Configuration" table for named regions.
//
// Every parser here follows the rule inherited from decision 0004: a line
// it does not understand fails the run, rather than being silently skipped.

import { existsSync, readFileSync } from "node:fs";
import { NM, OBJDUMP } from "./toolchain";
import { runTool } from "./run-tool";
import type { Region, Section, Sym } from "./types";

// --- objdump -h -----------------------------------------------------------
//
// Two lines per section:
//   Idx Name          Size      VMA       LMA       File off  Algn
//     0 .text         0000f900  10000000  10000000  00001000  2**3
//                     CONTENTS, ALLOC, LOAD, READONLY, CODE
const SECTION_LINE =
  /^\s*\d+\s+(\S+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+[0-9a-f]+\s+\S+\s*$/;
const FLAGS_LINE = /^\s+([A-Z][A-Z0-9_, ]*)\s*$/;

export function parseSections(elfPath: string): Section[] {
  const text = runTool(OBJDUMP, ["-h", elfPath]);
  const lines = text.split(/\r?\n/);
  const sections: Section[] = [];

  let i = 0;
  // Skip up to and including the column header ("Idx Name ... Algn").
  while (i < lines.length && !/^Idx\s+Name\s+Size/.test(lines[i]!)) i++;
  if (i === lines.length) {
    throw new Error(`objdump -h ${elfPath}: could not find the section table header`);
  }
  i++;

  while (i < lines.length) {
    const line = lines[i]!;
    if (line.trim() === "") {
      i++;
      continue;
    }
    const m = SECTION_LINE.exec(line);
    if (!m) {
      throw new Error(`objdump -h ${elfPath}: unparsable section line: ${JSON.stringify(line)}`);
    }
    const flagsLine = lines[i + 1];
    if (flagsLine === undefined) {
      throw new Error(`objdump -h ${elfPath}: section ${m[1]} has no flags line`);
    }
    const fm = FLAGS_LINE.exec(flagsLine);
    if (!fm) {
      throw new Error(
        `objdump -h ${elfPath}: unparsable flags line for ${m[1]}: ${JSON.stringify(flagsLine)}`
      );
    }
    sections.push({
      name: m[1]!,
      size: parseInt(m[2]!, 16),
      vma: parseInt(m[3]!, 16),
      lma: parseInt(m[4]!, 16),
      flags: new Set(fm[1]!.split(",").map((s) => s.trim())),
    });
    i += 2;
  }

  if (sections.length === 0) {
    throw new Error(`objdump -h ${elfPath}: found zero sections`);
  }
  return sections;
}

// --- nm -S ------------------------------------------------------------
//
//   200228b0 00000001 B __lock___malloc_recursive_mutex
//
// nm omits the size column entirely for zero-size symbols (linker-generated
// boundary markers like __data_end__), rather than printing 00000000 - both
// shapes are legitimate, not two different things to guess between.
const SYMBOL_LINE_SIZED = /^([0-9a-f]{8})\s+([0-9a-f]{8})\s+(\S)\s+(.+)$/;
const SYMBOL_LINE_UNSIZED = /^([0-9a-f]{8})\s+(\S)\s+(.+)$/;

export function parseSymbols(elfPath: string): Sym[] {
  const text = runTool(NM, ["-S", elfPath]);
  const symbols: Sym[] = [];
  for (const line of text.split(/\r?\n/)) {
    if (line.trim() === "") continue;
    const sized = SYMBOL_LINE_SIZED.exec(line);
    if (sized) {
      symbols.push({
        addr: parseInt(sized[1]!, 16),
        size: parseInt(sized[2]!, 16),
        type: sized[3]!,
        name: sized[4]!,
      });
      continue;
    }
    const unsized = SYMBOL_LINE_UNSIZED.exec(line);
    if (!unsized) {
      throw new Error(`nm -S ${elfPath}: unparsable symbol line: ${JSON.stringify(line)}`);
    }
    symbols.push({
      addr: parseInt(unsized[1]!, 16),
      size: 0,
      type: unsized[2]!,
      name: unsized[3]!,
    });
  }
  if (symbols.length === 0) {
    throw new Error(`nm -S ${elfPath}: found zero symbols`);
  }
  return symbols;
}

// --- the .map file's Memory Configuration table ----------------------------
//
//   Memory Configuration
//
//   Name             Origin             Length             Attributes
//   FLASH            0x10000000         0x00400000         xr
//   ...
//   *default*        0x00000000         0xffffffff
const REGION_LINE = /^(\S+)\s+(0x[0-9a-f]+)\s+(0x[0-9a-f]+)(?:\s+\S*)?\s*$/;

export function parseRegions(mapPath: string): Region[] {
  if (!existsSync(mapPath)) {
    throw new Error(`map file not found: ${mapPath}`);
  }
  const lines = readFileSync(mapPath, "utf8").split(/\r?\n/);
  let i = lines.findIndex((l) => l.trim() === "Memory Configuration");
  if (i === -1) {
    throw new Error(`${mapPath}: no "Memory Configuration" table`);
  }
  i++;
  while (i < lines.length && lines[i]!.trim() === "") i++;
  if (!/^Name\s+Origin\s+Length/.test(lines[i] ?? "")) {
    throw new Error(`${mapPath}: expected the region table header after "Memory Configuration"`);
  }
  i++;

  const regions: Region[] = [];
  while (i < lines.length && lines[i]!.trim() !== "") {
    const line = lines[i]!;
    const m = REGION_LINE.exec(line);
    if (!m) {
      throw new Error(`${mapPath}: unparsable memory region line: ${JSON.stringify(line)}`);
    }
    if (m[1] !== "*default*") {
      regions.push({ name: m[1]!, origin: parseInt(m[2]!, 16), length: parseInt(m[3]!, 16) });
    }
    i++;
  }
  if (regions.length === 0) {
    throw new Error(`${mapPath}: found zero memory regions`);
  }
  return regions;
}
