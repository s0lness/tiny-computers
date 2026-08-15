# The invariant checker

Static checks over the *built firmware artifact* (`main.elf` + `main.elf.map`),
run as a post-build step. It catches the shape of decision
[0005](../../docs/decisions/0005-rca-core1-dies-on-first-button.md): a hazard
that was understood, written down next to the code that avoided it, and lost
the moment a refactor moved that code. Full design rationale, the numbers
behind it, and the phased plan: decision
[0006](../../docs/decisions/0006-invariant-checker.md). Rule 1 was inverted
from "no code anywhere in flash" to "no code *core1 can reach* in flash" by
decision [0017](../../docs/decisions/0017-ram-place-only-what-core1-can-reach.md),
once decision 0016's own sixth invariant made the original whole-image
`copy_to_ram` fix too expensive in SRAM to keep shipping.

## Running it

```
bun run tools/invariants/runner.ts [path/to/main.elf] [path/to/main.elf.map]
```

Defaults to this device's own `firmware/build/main.elf` and `.map`. Exits
non-zero if any invariant fails, if an input is missing, or if binutils
output does not parse - all three are build failures, not skips (see "A line
the parser does not understand fails the run" below).

`bun run typecheck` (inside this directory) type-checks the tool itself.

## How it works, in four pieces

1. **A model of the artifact, from binutils text output, not an ELF parser**
   (`model.ts`, `disasm.ts`): `objdump -h` for sections (VMA/LMA/flags),
   `nm -S` for symbols, the `.map` file's Memory Configuration table for
   named regions, `objdump -d` for the disassembly the call graph is built
   from. The toolchain that built the firmware ships these parsers; decision
   0004's manual verification already used the same oracle.
2. **The core1 reachability graph** (`graph.ts`): a breadth-first search over
   direct branches (`bl`, `b`, `b.w`) from an explicit root set. Unsound by
   construction - an indirect call site (`blx r*`) is a dead end the BFS
   cannot follow - which is exactly why every device's rule 0 exists: it
   requires every such site inside the reached set to be resolved or
   annotated, converting that unsoundness into a gated, visible list instead
   of a silent hole.
3. **Invariants, as TypeScript predicates with a mandatory, checkable
   reason** (`types.ts`, `rules/*.ts`): every `Invariant` carries `why`
   (printed on every failure) and `see`, a repo-relative path to a decision
   record the runner verifies resolves to a real file at startup. A
   mandatory reason does not prevent deleting the invariant; it prevents
   deleting it uninformed.
4. **The runner** (`runner.ts`): builds the model, checks every `see`
   resolves, runs every invariant, and prints the rule, its `why`, and the
   offending symbols by name on failure - never a bare "invariant violated".

**A line the parser does not understand fails the run.** Inherited from
decision 0004: a checker that silently skips what it cannot read, and
passes, is an instrument reporting health without having looked. This
applies to two failure modes beyond a rule actually failing: the `.elf`/
`.map` are missing, or binutils output does not match a shape the parser
knows. Both exit non-zero, on the same reasoning - a checker skipped because
its input went missing is indistinguishable, from the build's point of view,
from one that passed.

## Machinery vs. device knowledge

`model.ts`, `disasm.ts`, `graph.ts`, `types.ts` and `runner.ts` know nothing
about this board - no register addresses, no function names, no hazard
reasoning. `rules/rp2350-amoled-1.8.ts` knows everything: the core1 root set,
which SDK functions install handlers, which symbols are forbidden on core1's
path, and why. This split exists so that moving the machinery to `puck` (the
extracted emulator repo, once it exists and has a second firmware to serve)
is mechanical: a new device gets a new `rules/*.ts` file, not a fork of the
parsers.

## What it cannot do

- **It sees the built artifact, not behaviour.** A rule about what the code
  IS, not what it does. It cannot catch timing, races that are legal by
  construction, or anything requiring execution.
- **Its call graph misses indirect edges by construction.** Rule 0 (in every
  device's rules file) turns that from a silent hole into an explicit,
  annotated, gated list - the strongest claim static analysis can honestly
  make here, not a claim that every edge is resolved.
- **Rule 0 tracks `blx r*`, `bx <reg other than lr>`, and a named list of
  handler-installing SDK calls - not every indirect control transfer.**
  Originally scoped to `blx r*` only; decision 0006 found `bx <reg>` sites
  reachable from core1 outside stdio and left the gap open deliberately,
  since nothing depended on it under `copy_to_ram`. Decision
  [0017](../../docs/decisions/0017-ram-place-only-what-core1-can-reach.md)
  made it load-bearing (RAM-placement decisions, not just placement
  verification, now depend on the reachable set being accurate) and closed
  it, as a deliberate, disclosed widening of rule 0's own definition, not a
  drive-by fix.
- **Rule 0/1 still do not model a fourth escape shape: the ARM linker
  veneer** (`ldr.w pc, [pc]` plus a literal pool word, inserted whenever a
  `bl` cannot directly bridge a RAM-resident caller to a flash-resident
  callee or vice versa). Decision 0017 found this hiding four real
  flash-resident, core1-reachable functions behind a RAM-resident veneer
  stub rule 1 happily passed. Closed for the current tree by moving the
  real targets into RAM (removing the distance that required the veneer),
  not by teaching the graph a fourth shape - this pattern recurs throughout
  the whole image for calls that have nothing to do with core1, so gating
  on it generically is out of scope for a mid-fix widening and is left as
  its own future decision. `audit-core1-veneers.ts` is the interim,
  on-demand check: run it after touching core1's path (sensors.c, tilt.c,
  or anything they call into), before trusting rule 1's PASS -
  ```
  bun run tools/invariants/audit-core1-veneers.ts [path/to/main.elf] [path/to/main.elf.map]
  ```
  It re-derives the same reachable set rule 0/1 use, finds every `*_veneer`
  symbol still in it, reads each one's own literal-pool jump target, and
  fails loud (by name, with the target address) if any resolves to a
  flash-resident, non-panic-path function - exactly the class of gap rule 1
  cannot see. Not wired into the build for the same reason the graph does
  not model this shape yet: it would be scope creep on the change that
  found it, not a reviewed decision to make the mandatory checker slower or
  broader.
- **It cannot measure stack headroom**, only that the map's stack region is
  the expected shape (rule 4). Real worst-case static depth needs
  `-fstack-usage` walked over the same core1 graph - decision 0006 phases
  this in after rule 0 is trusted, and it is not implemented here.
- **The checker prevents regressions of known hazards. It does not discover
  new ones.** Every rule here encodes a hazard this project has already
  paid for once (decisions 0004, 0005, and 0010's sixth). It will
  disappoint anyone hoping it finds a *new* class of bug; it is not that
  kind of tool.
- **Rule 5 reads firmware source text, not just the built artifact** -
  `firmware/runtime/gfx.h` and `firmware/lib/AMOLED/AMOLED_1in8.h`, to
  derive the framebuffer's byte count. This is a deliberate, disclosed
  exception to "machinery knows nothing about this board, and only the
  artifact is read": the framebuffer is a runtime `malloc`, not a linker
  allocation, so nothing in the ELF or map names it, and the only place the
  number exists is the source that computes it. See decision
  [0010](../../docs/decisions/0010-every-instrument-read-upstream-of-its-bug.md)'s
  own record of this - every other instrument in this project reads inside
  the toolchain or inside the emulator; this failure lived downstream of
  both, in a runtime allocation, which is exactly why this one rule reaches
  one step further back, to the source that decides the allocation's size.
