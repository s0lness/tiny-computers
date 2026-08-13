// The shared vocabulary the runner, the ELF/graph machinery, and every
// device-specific invariant file speak. See docs/decisions/0006 for the
// design this implements. Kept dependency-free (no node/bun imports) so it
// can be shared by a future `puck` copy of the machinery unchanged.

export interface Section {
  name: string;
  vma: number;
  lma: number;
  size: number;
  flags: Set<string>;
}

export interface Sym {
  addr: number;
  size: number;
  type: string; // nm's one-letter type code (T/t = text, D/d = data, B/b = bss, ...)
  name: string;
}

export interface Region {
  name: string;
  origin: number;
  length: number;
}

// One direct branch (bl, b, b.w) out of a function, resolved to the target
// symbol objdump itself named at that address - clone suffix intact, since
// resolving it is objdump's job, not ours.
export interface DisasmBranch {
  addr: number;
  mnemonic: "bl" | "b" | "b.w";
  targetName: string;
}

// One `blx r*` (or other register-indirect branch) site: the actual target
// is not statically known, which is exactly what rule 0 exists to gate.
export interface IndirectSite {
  addr: number;
  mnemonic: string;
  operand: string;
}

export interface DisasmFunction {
  name: string;
  addr: number;
  branches: DisasmBranch[];
  indirect: IndirectSite[];
}

export interface Firmware {
  elfPath: string;
  mapPath: string;
  sections: Section[];
  symbols: Sym[];
  regions: Region[];
  functions: Map<string, DisasmFunction>;
}

export interface Violation {
  message: string;
  // The offending symbols, printed by name - decision 0006 is explicit that
  // a failure must never be a bare "invariant violated".
  symbols: string[];
}

export interface Invariant {
  name: string;
  why: string;
  // Repo-relative path to a decision record. The runner checks at startup
  // that this resolves to an existing file.
  see: string;
  check(fw: Firmware): Violation[];
}
