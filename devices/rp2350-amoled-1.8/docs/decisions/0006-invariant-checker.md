# 0006: An invariant checker over the built firmware

Date: 2026-08-13
Status: plan, revised in review on 2026-08-13. The review measured the real
artifact rather than estimating; the numbers below come from
`firmware/build/main.elf` as built at the head of this branch and must be
reproduced by the tool itself in phase 2, not trusted from here.

## Why this exists

The bug that cost a full working day (decision 0005) was not findable by
simulation. No emulator surveyed models flash chip-select arbitration between
two cores, and building one that did would take months (decision 0003's
landscape survey).

But it was findable by a **rule**:

> No code may execute from flash, on either core, ever, because core0 borrows
> the flash chip select to read BOOT and a fetch during that borrow returns
> garbage.

That is checkable mechanically, against the linker map, in seconds, at build
time. We would have had the error on the morning it was introduced instead of
finding it at night.

This is the third leg of the testing strategy, and the three do different
jobs. The emulator covers app logic, which is most of the behaviour and none
of the hard bugs. Differential testing against real hardware covers
conformance, using the device as the oracle. **The checker covers the hazards
that neither can see, by forbidding the states in which they occur.**

## The lesson it encodes

The hazard behind decision 0005 was already known here. It was written down,
correctly, in a comment next to the code that avoided it. Then a refactor
deleted that file and the warning died with it.

A rule that lives only as prose beside the code it constrains will be lost the
moment that code moves. A rule that fails the build will not.

## What the review measured, 2026-08-13

An earlier draft of this plan feared that the call graph would be mostly
unresolvable on our own firmware, because `app_t` is a struct of function
pointers by design. The review measured instead of estimating, with a
40-line breadth-first search over `objdump -d` output, following direct
branches (`bl`, `b`, `b.w`) from `{core1_entry, core1_trampoline,
core1_fault_handler}`:

- The image contains **523 functions**. The core1 roots reach **30** of them.
- On that reached path there are **5 indirect call sites** (`blx r*`), and
  all five are inside the stdio driver dispatch (`__wrap_puts`,
  `weak_raw_vprintf`, `stdio_put_string`).
- The whole image contains **59** indirect call sites. The other 54,
  including all of `app_t`'s dispatch, are on core0 and never poison the
  core1 analysis, because the rules that need reachability are core1 rules.
- The only executable section at a flash VMA is `.flashtext` (0x1ec bytes:
  crt0's reset path, the boot-time vector table, the default unhandled-ISR
  stubs). Everything else that executes has a SRAM VMA, which is
  `copy_to_ram` doing its job, exactly as decision 0004's manual
  verification found.
- **197** symbols carry compiler-clone suffixes (`.constprop`, `.isra`,
  `.part`), including core1's own `i2c1_write_bytes_bounded.constprop.{0,1,2}`.
  Any rule that matches symbols by name must strip these suffixes or it will
  silently miss the functions it was written about.

Two conclusions and one finding follow.

**Conclusion 1: the core1 call graph is nearly closed, so reachability rules
are viable.** The fear about `app_t` was wrong in a useful way: the indirect
dispatch lives on the core the rules do not apply to.

**Conclusion 2: near-closure can be enforced, which converts unsoundness into
a gate.** A static graph misses edges at indirect call sites, so a rule built
on it can miss violations. But with only five such sites on the path, the
checker can require that **every indirect call site reachable from a core1
root is either resolved or explicitly annotated with a reason**, and fail
otherwise. That is rule 0 below. It makes rules 2 and 3 sound in the only way
static analysis can be: not by resolving everything, but by refusing to pass
while anything is unresolved and unexplained.

**Finding: rule 2 fails on today's firmware.** `hard_assertion_failure` calls
`panic`, `panic` calls `__wrap_puts`, and both are reachable from
`core1_entry` (via the SDK's sleep/alarm/mutex internals, which assert). So
"nothing on the core1 path takes the stdio lock" is violated right now, on
the panic path. This is arguably a real hazard rather than noise: a core1
panic would take the stdio mutex that core0 also takes, so a dying core1 can
wedge core0 too. Whether the answer is a core1-specific panic hook
(`PICO_PANIC_FUNCTION`) or an annotated exception "a panicking core1 is
already lost", it must be decided explicitly in phase 3, not swallowed by a
silent allowlist. A checker whose first real finding is suppressed to make
the build green would be decision 0004's fifth lying instrument.

## Where it lives

**In this repository, under `tools/`, until it has a second consumer.** An
earlier draft placed it in `puck`, the extracted emulator repo, because the
machinery is generic. The machinery is generic, but the extraction has not
happened yet, the acceptance test below needs this repo's git history and
toolchain, and a tool gated on a migration that has not occurred protects
nobody in the meantime. The generic/device split survives as code layout:
the ELF/map/graph machinery knows nothing about this board, the invariant
files know everything, and moving the machinery to `puck` later is mechanical.
This is a judgment call, reversed from the earlier draft; if the extraction
lands first, land the checker there instead.

## The pieces

### 1. A model of the built firmware, from binutils output, not an ELF parser

Do not write or import an ELF parser. The toolchain that built the firmware
ships the parsers, and their text output is the same oracle decision 0004's
manual verification already used:

- `objdump -h`: sections, VMA, LMA, and the executable flag. `copy_to_ram`
  builds have a flash LMA and a RAM VMA, and confusing the two is exactly
  the mistake this tool must not make.
- `arm-none-eabi-nm -S`: symbols with addresses and sizes.
- the `.map` file's Memory Configuration table: named regions with origin and
  length (FLASH, RAM, SCRATCH_X, ...), so regions are read rather than
  hardcoded and the tool works for a layout it has never seen.
- `objdump -d`: the disassembly the call graph is built from.

One rule about parsing, inherited from decision 0004: **a line the parser
does not understand fails the run.** A checker that skips what it cannot
read, and passes, is an instrument reporting health without having looked.

### 2. The core1 reachability helper, and rule 0

Build the graph by following direct branches from an explicit root set.
Handle tail calls (`b`, `b.w` to another function), strip clone suffixes,
match symbols by address rather than name where possible.

**Rule 0, which makes the others trustworthy: every `blx r*` site reachable
from a core1 root must be annotated in the invariant file with a reason, or
the run fails.** Today that is five sites, all in stdio. Anyone who adds an
indirect call to core1's path must explain where it can go; the explanation
is then visible in the next failure instead of being an invisible hole in
the analysis.

**The root set is the honest open problem, and it gets its own investigation
task in phase 2.** `core1_entry`, the SDK's `core1_trampoline`, and the fault
handlers `core1_install_fault_handlers()` installs are known. What is not
established: whether any IRQ fires on core1. `sleep_ms` is on core1's path
(the FT3168 reset), and the SDK's sleep machinery involves the alarm pool,
whose `alarm_pool_irq_handler` dispatches through two more `blx r*` sites.
Whether that handler runs on core1 or only on core0 (with core1 merely
waiting on WFE) depends on which core owns the default alarm pool's IRQ, and
this review did not establish it. Until it is established, the checker
should treat calls from core1's path into handler-installing SDK functions
(`irq_set_exclusive_handler`, `exception_set_exclusive_handler`, alarm pool
creation) as taint: each such call site must have its handler listed as a
root or be annotated with why it is not one.

### 3. Invariants, as TypeScript predicates with a mandatory, checkable reason

Not a declarative config file. A useful rule needs logic, and a list of
patterns becomes unreadable faster than code does.

```ts
export const noCodeInFlash: Invariant = {
  name: "no executable byte at a flash VMA, outside the boot allowlist",
  why: "core0 borrows the flash chip select to read BOOT; a fetch during " +
       "the borrow returns garbage and the fetching core stops without " +
       "faulting. The fix is copy_to_ram, whole image, both cores.",
  see: "docs/decisions/0005-rca-core1-dies-on-first-button.md",
  check(fw) { /* ... */ },
};
```

**What actually makes `why` load-bearing, since a mandatory string is
satisfied by typing "because":**

- `why` is printed in every failure, so its consumer is the person whose
  build just broke. A junk reason produces a confused colleague and a
  question aimed at its author; the field polices itself through use, not
  through the type checker.
- `see` is a repo-relative path to a decision record, and the runner checks
  at startup that it resolves to an existing file. The durable reasoning
  then lives in the one documentation system this project has shown survives
  refactors, and the invariant carries a pointer that cannot silently rot.
- Be honest about the limit: a mandatory reason does not prevent deleting
  the invariant. It prevents deleting it uninformed. The person removing the
  rule reads, in the diff they are writing, exactly what they are choosing
  to re-expose. That is the achievable guarantee, and it is the one that was
  missing when the original hazard comment died with its file.

### 4. A runner that fails the build, and cannot be skipped silently

Runs after every firmware build, wired in as a post-build step, and in CI.
On violation it exits non-zero and prints the rule, the `why`, and **the
offending symbols by name**, not a bare "invariant violated".

Two failure modes of the runner itself are treated as build failures, for
decision 0004 reasons: inputs missing (no `.elf`, no `.map`) and inputs
unparsable. A checker that is skipped because its input went missing is
indistinguishable, from the build's point of view, from a checker that
passed.

## The first invariants to ship

Chosen because each one has already cost us something.

**1. No executable byte at a flash VMA, outside an explicit allowlist**
(decision 0005). This replaces the earlier draft's "no symbol reachable from
`core1_entry` in FLASH", deliberately, for three reasons. First, the shipped
fix's actual invariant (0004: "no code executes from flash, on either core,
ever") is a whole-image property, and encoding the fix rather than the
symptom needs no call graph at all: placement is total, reachability is an
approximation. Second, sound and cheap beats precise and unsound: the
reachability version inherits every unresolved edge as a possible miss,
the placement version cannot miss. Third, it still catches every named
regression: turning `copy_to_ram` off puts hundreds of functions at flash
VMAs; marking a function `__in_flash` gives it a flash VMA; a linker script
surprise shows up as a new flash-VMA executable section. The allowlist is
today exactly `.flashtext` and the symbols in it (crt0 reset path, boot
vector table, default ISR stubs, all reachable only at cold reset or on an
already-dead machine), and the allowlist lives in the invariant file with
its own reason.

**2. Nothing on the core1 path takes the stdio lock** (`sensors.h`'s
ownership rule, enforced today by convention and a comment). Reachability
from the core1 roots must not include `printf`, `puts`, `stdio_put_string`,
or the stdio mutex functions. **Known to fail on current firmware via the
panic path**; see the finding above. Phase 3 resolves that finding first.

**3. No SDK i2c symbol on the core1 path.** The SDK's read path has an
unbounded wait; we carry local bounded versions, and nothing stops someone
calling `i2c_read_blocking` directly again. Today's reached set contains no
SDK i2c symbols, so this rule passes and would catch the regression the day
it is introduced. Direct calls are what the graph sees, and a direct call is
how this regression would arrive.

**4. Core1's stack, split into what the map can actually answer.** The map
cannot measure headroom; the earlier draft overclaimed. What it can check
cheaply: core1's stack region (`.stack1_dummy`, 2KB, SCRATCH_X at
0x20080000) exists, has the expected size, and shares its memory region with
nothing else. Worst-case static stack depth is the real replacement for the
runtime canary that lied (2048 of 2048, decision 0004), and it is buildable
from `-fstack-usage` `.su` files walked over the same core1 graph, but it is
the most expensive invariant and lands last, after the graph is trusted and
rule 0 has closed the indirect sites it would otherwise silently undercount.

Rules 2 and 3 are both "reachable-from-core1 must not intersect set Y":
one helper, two rule instances.

## What it cannot do

State this in the tool's own README, not only here.

- It sees the built artifact, not behaviour. A rule about what the code IS,
  not what it does.
- Its call graph misses indirect edges by construction; rule 0 turns that
  from a silent hole into an explicit, annotated, gated list, which is the
  strongest claim static analysis can honestly make here.
- It cannot catch timing, races that are legal by construction, or anything
  requiring execution. The bug in decision 0005 is catchable as a rule only
  because the hazard was already understood. **The checker prevents
  regressions of known hazards. It does not discover new ones.**

That last line matters. Sold as a bug finder it will disappoint. Sold as the
thing that stops a known hazard coming back after a refactor, it is exactly
what this project needed and did not have.

## Prior art, answered

Searched 2026-08-13. Nothing found does the actual job (assertions over an
ELF and map that fail a firmware build), so the tool is worth writing, but
two findings shrink it and one was rejected with reasons:

- **puncover** (HBehrens/puncover, Memfault-maintained) is the closest
  neighbour: ELF plus objdump-derived call graph plus `.su` stack analysis,
  for ARM firmware. It is a report viewer, not a failing check, it is
  Python, and its own issue tracker confirms it does not chase function
  pointers (issue #105), the same limitation rule 0 addresses. Verdict:
  steal its approach (parse the toolchain's own output rather than
  binaries), not the tool.
- **GCC's `-fcallgraph-info=su`** (GCC 10+; the installed arm toolchain is
  14.2) emits per-TU call graphs with indirect call sites marked, and pairs
  with `-fstack-usage` for invariant 4. Candidate to replace or cross-check
  the disassembly-derived graph; evaluate in phase 2, do not commit to it
  here, since it sees compilation units where the checker needs the linked
  image.
- **GNU ld's `ASSERT()`** could encode invariant 1 inside the linker script
  with zero new tooling. Rejected: pico-sdk owns the linker script
  (`pico_set_binary_type` selects it), so using ASSERT means forking and
  tracking the SDK's script forever, and it cannot print offending symbols
  or carry a `why`.
- **Heavy CFG recovery** (Ghidra headless, angr, radare2) resolves more
  indirection than a direct-branch BFS. Rejected on the measurement above:
  five indirect sites on the path do not justify a JVM or an analysis
  framework, and rule 0 handles them at annotation cost instead.

## Implementation phases

Reordered so the invariant that catches 0005 ships first, since it needs no
call graph.

1. **The runner, the invariant type (`why` plus resolving `see`), the
   section/region model from `objdump -h`, `nm`, and the map, and invariant 1.**
   Wire into the firmware build as a post-build step that fails it.
2. **The acceptance test, immediately, before any more machinery:**
   - Checker FAILS on commit `e11fafc` (the instrumented pre-fix build) and
     PASSES on `4869d00` (the `copy_to_ram` fix). Both are buildable with
     the installed toolchain in a temporary worktree; both were built on
     this machine on 2026-08-13.
   - History only exercises invariant 1, so history is not sufficient. Each
     rule gets a known-bad build by mutation of current source: remove
     `copy_to_ram` (rule 1 fails), add a flash-resident function (rule 1
     fails), add a `printf` to `sensors.c`'s core1 loop (rule 2 fails), call
     `i2c_read_blocking` from core1 (rule 3 fails). A checker never shown to
     fail on a real bad build is not known to work.
3. **The core1 reachability helper**: direct-branch BFS over `objdump -d`,
   clone-suffix handling, rule 0's indirect-site gate, and the root-set
   investigation (does any IRQ fire on core1; settle the alarm pool
   question). Reproduce this review's numbers as its first output.
4. **Rules 2 and 3**, beginning with an explicit decision on the panic
   path finding (core1 panic hook versus annotated exception), recorded in
   the invariant file's `why`.
5. **Invariant 4**: the cheap map-level stack checks first; static
   worst-case depth via `-fstack-usage` after, and only after rule 0 is
   green.
6. Extraction to `puck`, when `puck` exists and has a second firmware to
   serve.

## Questions raised in review, resolved

- *Is the call graph worth building?* Yes, but only because measurement
  showed core1's graph is nearly closed (30 functions, 5 indirect sites,
  all in stdio). Had the answer been "60 percent unresolved", rules 2 and 3
  would have been dropped rather than built unsound. The graph is also no
  longer the expensive part: the placement rule that catches 0005 does not
  use it.
- *Is the reachability form of rule 1 right?* No. The whole-image placement
  rule is sound without a graph, encodes the fix that actually shipped, and
  catches the same regressions plus core0-side ones. Replaced.
- *Four scripts instead of a tool?* The measured sizes say this is roughly
  one 50-line placement check, one ~150-line graph helper shared by two
  rules, and a runner. Build it as one small tool because the rules share
  the model, the `why`/`see` mechanism, and the fail-the-build wiring, but
  hold that line: this is a few hundred lines plus invariant files, not a
  framework, and any growth beyond "parse toolchain output, assert, print
  names" should be challenged against this paragraph.
