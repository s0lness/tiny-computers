// Device-specific invariants for rp2350-amoled-1.8. Per docs/decisions/0006:
// the ELF/map/graph machinery (../model.ts, ../disasm.ts, ../graph.ts)
// knows nothing about this board; everything here does, so moving the
// machinery to `puck` later only means importing it from a new path.

import { existsSync, readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { reachableFrom, reachedNamesMatching, stripCloneSuffix } from "../graph";
import type { Firmware, Invariant, Region, Section, Violation } from "../types";

// rules/rp2350-amoled-1.8.ts -> tools/invariants/rules -> tools/invariants
// -> tools -> the device root. Only rule 5 below needs this: it is the one
// invariant that reads firmware *source* (gfx.h, AMOLED_1in8.h) rather than
// only the built .elf/.map, because the number it guards (the framebuffer's
// byte count) is a runtime malloc, not a linker allocation - nothing in the
// artifact names it (see rule 5's own header comment).
const DEVICE_ROOT = join(dirname(fileURLToPath(import.meta.url)), "..", "..", "..");

// core1_trampoline is pico-sdk's own multicore.c launch stub; core1_entry
// and core1_fault_handler are ours (sensors.c). All three are seeded
// directly, rather than relying on the graph to connect them, because
// core1_trampoline's body is `pop {r0, r1, pc}` - not a bl/b/b.w/blx - so a
// direct-branch graph could never discover core1_entry as its callee.
const ROOTS = ["core1_entry", "core1_trampoline", "core1_fault_handler"];

// --- roots the graph cannot discover on its own, resolved by inspection ---
//
// Added for rule 1's inversion (docs/decisions/0004/0005: RAM-place only
// what core1 can reach, instead of copy_to_ram's whole image). That change
// is only as sound as this root set: a function core1 genuinely executes but
// this graph never reaches is a function rule 1 will silently leave at a
// flash VMA. Building this list is what surfaced two gaps neither rule 0 nor
// the original (placement-only) rule 1 had any reason to care about before:
//
// 1. `bx <reg>` other than `bx lr` was not tracked as an indirect site at
//    all (only `blx r*` was - decision 0006 found nine `bx <reg>` sites in
//    the whole image and left the gap open deliberately, since under
//    copy_to_ram nothing depended on it). disasm.ts now tracks both
//    uniformly; this file resolves the two that land on core1's own path.
// 2. core1_trampoline's landing point is core1_wrapper (pico_multicore), not
//    core1_entry directly - and the jump into it is `pop {..., pc}`, which
//    is not a bl/b/b.w/blx either, the exact same reason core1_trampoline
//    itself had to be a seeded ROOT rather than a discovered edge.
//
// "core1_wrapper" (pico_multicore's core1_trampoline pops PC here, not a
// branch instruction the parser models - see the comment above). It then
// calls runtime_run_per_core_initializers() by a plain bl (found fine) and
// tail-calls `(*entry)()` via `bx r3` - see ESCAPE_ANNOTATIONS below for why
// that always resolves to core1_entry.
//
// "default_core_init_deinit" - sensors.c's core1_entry() calls pico-sdk's
// flash_safe_execute_core_init() directly (storage.c's flash_safe_execute()
// needs this so it can park core1 during a dino high-score save, decision
// 0011); that function tail-calls `helper->core_init_deinit(true)` through
// flash_safety_helper_t, a struct of function pointers - a `bx r*`, invisible
// to the graph. Resolved by inspection: get_flash_safety_helper() is
// `__attribute__((weak))` but nothing in this firmware (grepped) overrides
// it or reassigns default_flash_safety_helper's fields after its own static
// initializer, so the call is deterministic. See ESCAPE_ANNOTATIONS below.
const EXTRA_ROOTS_RESOLVED = ["core1_wrapper", "default_core_init_deinit"];

// pico-sdk's per-core initializer array: core1_wrapper() (a resolved root
// above) calls runtime_run_per_core_initializers(), which walks
// `[__pre_init_first_per_core_initializer, __init_array_start)` and calls
// every entry through `blx r3` in a loop - another indirect site the graph
// cannot follow, and unlike the two above the SET of targets is link-time
// data, not a fixed pair of function names, so it is read from the symbol
// table rather than hardcoded. Each `__pre_init_<name>` entry's address
// holds a pointer to <name> itself (pico-sdk's PICO_RUNTIME_INIT_FUNC
// mechanism), so stripping the prefix recovers the real target - verified
// against this build: the six entries found were first_per_core_initializer,
// runtime_init_per_core_bootrom_reset, runtime_init_per_core_enable_
// coprocessors, spinlock_set_extexclall, runtime_init_per_core_irq_
// priorities and runtime_init_per_core_tls_setup, none of which call
// anything the graph cannot already follow directly. A build that adds or
// removes a per-core initializer changes this set automatically, with no
// edit needed here - the alternative, a hardcoded name list, is exactly the
// kind of thing a future SDK bump silently invalidates.
const PER_CORE_INIT_ARRAY_START_SYM = "__pre_init_first_per_core_initializer";
const PER_CORE_INIT_ARRAY_END_SYM = "__init_array_start";
const PRE_INIT_PREFIX = "__pre_init_";

function perCoreInitializerRoots(fw: Firmware): string[] {
  const start = fw.symbols.find((s) => s.name === PER_CORE_INIT_ARRAY_START_SYM);
  const end = fw.symbols.find((s) => s.name === PER_CORE_INIT_ARRAY_END_SYM);
  if (!start || !end) {
    throw new Error(
      `invariant checker: expected pico-sdk's per-core initializer array boundary symbols ` +
        `"${PER_CORE_INIT_ARRAY_START_SYM}"/"${PER_CORE_INIT_ARRAY_END_SYM}" in the image - ` +
        `core1_wrapper() walks this array via an indirect blx the reachability graph cannot ` +
        `follow; an SDK change that renames or removes these would silently blind rule 0/1/2/3 ` +
        `to whatever the array now holds, so this fails loud instead of guessing`
    );
  }
  if (!(end.addr > start.addr)) {
    throw new Error(
      `invariant checker: "${PER_CORE_INIT_ARRAY_END_SYM}" (0x${end.addr.toString(16)}) is not ` +
        `after "${PER_CORE_INIT_ARRAY_START_SYM}" (0x${start.addr.toString(16)}) - the per-core ` +
        `initializer array's bounds look wrong, refusing to guess`
    );
  }
  const roots: string[] = [];
  for (const s of fw.symbols) {
    if (s.addr < start.addr || s.addr >= end.addr) continue;
    if (!s.name.startsWith(PRE_INIT_PREFIX)) continue;
    roots.push(s.name.slice(PRE_INIT_PREFIX.length));
  }
  if (roots.length === 0) {
    throw new Error(
      `invariant checker: found zero "${PRE_INIT_PREFIX}*" symbols between the per-core ` +
        `initializer array bounds (0x${start.addr.toString(16)}..0x${end.addr.toString(16)}) - ` +
        `expected at least one (first_per_core_initializer itself)`
    );
  }
  return roots;
}

// The full root set every rule below should use: the three hardware/ISR
// entry points, the two resolved-by-inspection tail-call targets, and the
// per-core initializer array's actual contents. Rules 0, 2 and 3 move to
// this too (not just rule 1): they were previously checking a narrower,
// slightly wrong picture of core1's real call graph, which could only ever
// under-report a stdio or SDK-i2c violation reachable solely through one of
// these newly-resolved edges.
// Exported (not just used internally by rules 0/1/2/3) so
// tools/invariants/audit-core1-veneers.ts - the by-hand check decision
// 0017 asks a future editor of core1's path to re-run, now a real script
// instead of prose - can compute the identical reachable set rather than
// a second, potentially-drifting copy of this logic.
export function allRoots(fw: Firmware): string[] {
  return [...ROOTS, ...EXTRA_ROOTS_RESOLVED, ...perCoreInitializerRoots(fw)];
}

// --- rule 0: every escape from the direct-branch graph is annotated -------
//
// Two shapes of "the static graph cannot see where this actually goes":
// a `blx r*` (the register's value is not known until runtime), and a call
// into an SDK function that installs a handler invoked later through a
// mechanism this graph does not model at all (NVIC vectoring, not a bl/b).
// docs/decisions/0006's root-set investigation asked specifically whether
// core1 ever installs an alarm/IRQ handler; HANDLER_INSTALLERS is how that
// question stays answered instead of re-opening silently on some future
// refactor.
const HANDLER_INSTALLERS = new Set([
  "irq_set_exclusive_handler",
  "exception_set_exclusive_handler",
  "alarm_pool_create",
  "alarm_pool_init_default",
]);

interface EscapeSite {
  func: string;
  detail: string;
}

function findEscapeSites(fw: Firmware, reachedRaw: Set<string>, indirectSites: { func: string; mnemonic: string; operand: string; addr: number }[]): EscapeSite[] {
  const sites: EscapeSite[] = [];
  for (const s of indirectSites) {
    sites.push({ func: s.func, detail: `${s.mnemonic} ${s.operand} @ 0x${s.addr.toString(16)}` });
  }
  for (const name of reachedRaw) {
    const fn = fw.functions.get(name);
    if (!fn) continue;
    for (const b of fn.branches) {
      if (HANDLER_INSTALLERS.has(stripCloneSuffix(b.targetName))) {
        sites.push({ func: fn.name, detail: `bl ${b.targetName} @ 0x${b.addr.toString(16)}` });
      }
    }
  }
  return sites;
}

// Keyed by the CONTAINING function's canonical name, not by address: an
// address-keyed annotation would break on every rebuild that shifts a
// single byte, which is not what "explain it once" is supposed to cost.
//
// Measured 2026-08-14 (reproducing decision 0006's review numbers exactly:
// 523 functions in the image, 30 reached from these roots, 5 indirect
// sites, all five inside the three stdio names below): the root-set
// question - does any IRQ fire on core1 - is answered here, not left open.
// `sleep_ms`/`sleep_us` on core1's path (touch_recover_core1 ->
// FT3168_Reset) do reach `alarm_pool_add_alarm_at_force_in_context`,
// `alarm_pool_cancel_alarm` and `best_effort_wfe_or_timeout`, but NOT
// `alarm_pool_create` or `alarm_pool_init_default`: the default alarm pool
// is built by `runtime_init_default_alarm_pool` (nm shows it as a
// `__pre_init_*` symbol - pico-sdk's PICO_PREINIT/init-array mechanism),
// which runs during core0's crt0 startup, strictly before `main()` and
// therefore strictly before `sensors_start()` can call
// `multicore_launch_core1()`. core1 never runs crt0's init-array (it is
// launched straight into `core1_entry`), so it can only ever find the pool
// already built and take the WFE/poll path - it cannot race core0 for
// pool creation because it launches after the race is already decided.
// No IRQ handler is installed or re-installed from core1's path; the only
// installer call left is `core1_install_fault_handlers`'s five calls to
// `exception_set_exclusive_handler`, which install `core1_fault_handler` -
// already a root above, so they add no reachable code the graph does not
// already have.
const ESCAPE_ANNOTATIONS: Record<string, string> = {
  __wrap_puts:
    "stdio's low-level putc dispatch. Reached only via panic() on the " +
    "assertion path (hard_assertion_failure -> panic -> __wrap_puts); see " +
    "rule 2's exception and docs/decisions/0007.",
  weak_raw_vprintf:
    "panic()'s own printf-with-a-format-string path, same reachability as " +
    "__wrap_puts above; see docs/decisions/0007.",
  stdio_put_string:
    "the function that actually takes print_mutex, called by both names " +
    "above; see docs/decisions/0007.",
  // core1_install_fault_handlers() is a small static function called once
  // from core1_entry() and the compiler inlines it there - confirmed from
  // the disassembly, where the five exception_set_exclusive_handler calls
  // show up directly inside core1_entry, not in a separate symbol. The
  // annotation is keyed on where the calls actually end up post-inlining,
  // which is why this says core1_entry rather than the C source function
  // name: an address-keyed annotation would break on every rebuild, but so
  // would a source-function-keyed one the moment inlining changes - both
  // need a human to notice and re-key, which is the point of this being a
  // gate rather than silent.
  core1_entry:
    "installs core1_fault_handler for all five fault vectors " +
    "(HardFault/MemManage/BusFault/UsageFault/SecureFault) via " +
    "core1_install_fault_handlers(), inlined here by the compiler; " +
    "core1_fault_handler is already a graph root above, so this adds no " +
    "reachable code the BFS does not already have.",
  // The four entries below exist because EXTRA_ROOTS_RESOLVED/
  // perCoreInitializerRoots() (above) had to resolve escape sites BY HAND to
  // build a root set the graph could not discover on its own - each one is
  // the site itself, not a name the escape resolves to (matching every
  // other key in this object).
  core1_wrapper:
    "tail call (`bx r3`) is `(*entry)()`: pico_multicore's core1_wrapper " +
    "invokes whatever function pointer multicore_launch_core1() was given. " +
    "This firmware only ever calls multicore_launch_core1(core1_entry) " +
    "(sensors.c's sensors_start()/sensors_restart_core1(), both grepped, " +
    "no other call site exists), so the target is deterministic and is " +
    "core1_entry, already a graph root. See EXTRA_ROOTS_RESOLVED's comment.",
  flash_safe_execute_core_init:
    "tail call (`bx r3`) is `helper->core_init_deinit(true)`, through " +
    "pico-sdk's flash_safety_helper_t vtable (pico_flash/flash.c). Nothing " +
    "in this firmware overrides the weak get_flash_safety_helper() or " +
    "reassigns default_flash_safety_helper's fields after its own static " +
    "initializer (grepped), so the call always lands on " +
    "default_core_init_deinit, added as an extra root rather than left " +
    "unresolved. See EXTRA_ROOTS_RESOLVED's comment.",
  runtime_run_per_core_initializers:
    "the `blx r3` is pico-sdk's per-core initializer array walk " +
    "(PICO_RUNTIME_INIT_FUNC), called from core1_wrapper on every core1 " +
    "(re)launch. Every entry between __pre_init_first_per_core_initializer " +
    "and __init_array_start is discovered from the symbol table and added " +
    "as a root by perCoreInitializerRoots() - see that function's comment.",
  multicore_lockout_victim_init:
    "installs multicore_lockout_handler (pico_multicore's flash-lockout " +
    "IRQ victim) via irq_set_exclusive_handler(), required so storage.c's " +
    "flash_safe_execute() can later park this core during a dino " +
    "high-score save (decision 0011). multicore_lockout_handler is " +
    "installed, not called, so it is not itself in this graph's reached " +
    "set; it does not need this rule's RAM-placement check either, because " +
    "pico-sdk already marks it `__not_in_flash_func` for exactly this " +
    "reason (src/rp2_common/pico_multicore/multicore.c: \"note this method " +
    "is in RAM because lockout is used when writing to flash\").",
  // runtime_init_per_core_bootrom_reset is one of the per-core initializer
  // array's own entries (perCoreInitializerRoots() above), so it runs on
  // core1 on every (re)launch. Its `bx r3` tail-calls whatever
  // rom_func_lookup() returns for the two-byte bootrom function code 0x5253
  // ("SR" - a per-core bootrom reset call, per pico-sdk's own naming). That
  // target is inside the RP2350's on-chip boot ROM: a different physical
  // memory from the external QSPI flash the BOOT-read hazard borrows, so a
  // fetch there is unaffected by the borrow regardless of when it happens -
  // this is why the escape itself is safe to leave unresolved, NOT a claim
  // that runtime_init_per_core_bootrom_reset's own code (which IS an
  // ordinary flash-VMA risk like any other function here) can skip rule 1;
  // it cannot, and does not - see firmware/linker_overrides/
  // default_text_excludes.incl.
  runtime_init_per_core_bootrom_reset:
    "tail call (`bx r3`) into rom_func_lookup()'s result: the RP2350's " +
    "on-chip boot ROM, a physically different memory from the external " +
    "QSPI flash the BOOT-read hazard borrows (docs/decisions/0004/0005), " +
    "so a fetch there cannot be corrupted by that borrow no matter when it " +
    "happens. This function's OWN code is still an ordinary flash-VMA risk " +
    "like any other core1-reachable function and IS RAM-placed (firmware/ " +
    "linker_overrides/default_text_excludes.incl) - only the escape site " +
    "itself, which this graph cannot follow into the boot ROM's own " +
    "unmapped-in-this-image code, is what this annotation excuses.",
  // rom_func_lookup's own two `bx r3` sites are the SAME boot-ROM jump,
  // parameterised by whichever caller's function code was looked up (this
  // firmware only reaches it from runtime_init_per_core_bootrom_reset
  // above, but the function itself is generic pico-sdk code with no way to
  // name a single caller).
  rom_func_lookup:
    "both `bx r3` sites jump into the RP2350's on-chip boot ROM - a " +
    "different physical memory from the external QSPI flash the BOOT-read " +
    "hazard borrows (docs/decisions/0004/0005), so neither fetch can be " +
    "corrupted by that borrow. Not part of this image, so this graph " +
    "cannot and need not model it further.",
};

// Two ARM linker-generated veneers (`ldr.w pc, [pc]` plus a literal pool
// word - a trampoline for a `bl` whose direct-branch encoding cannot reach
// its target), both inserted at flash-resident stdio_put_string's own two
// calls into pico-sdk's already-`__time_critical_func`-annotated
// mutex_try_enter_block_until()/mutex_exit() (pico_sync/mutex.c's
// print_mutex acquire/release, per decision 0007's own account of
// stdio_put_string's locking). stdio_put_string is itself reachable from
// core1 only via the panic path and is already exempted below
// (PANIC_PATH_ALREADY_LOST); the veneer is a distinct symbol the graph
// discovers as stdio_put_string's OWN resolved branch target (objdump
// itself names the veneer, not the real function, as the `bl`'s
// destination), so it needs its own line here rather than inheriting
// stdio_put_string's exemption automatically. The veneer's few bytes exist
// only to bridge a flash-resident caller to an already-RAM callee; they
// carry no logic of their own and are exactly as "already lost" as the
// panic call that reaches them.
const PANIC_PATH_VENEERS = [
  "__mutex_try_enter_block_until_veneer",
  "__mutex_exit_veneer",
];

export const rule0EscapesAnnotated: Invariant = {
  name: "every indirect or handler-installing call site reachable from core1 is annotated",
  why:
    "A static direct-branch graph misses two kinds of edge: a `blx r*` " +
    "(the target is a runtime value) and a call into an SDK function that " +
    "installs a handler invoked later through NVIC vectoring, which no " +
    "bl/b/b.w models. Rule 0 is what makes rules 2 and 3 trustworthy " +
    "despite that: not by resolving every edge, but by refusing to pass " +
    "while any such site is unexplained. See docs/decisions/0006, " +
    "'The core1 reachability helper, and rule 0'.",
  see: "docs/decisions/0006-invariant-checker.md",
  check(fw) {
    const reach = reachableFrom(fw.functions, allRoots(fw));
    const sites = findEscapeSites(fw, reach.reached, reach.indirectSites);
    const unannotated = sites.filter((s) => !(stripCloneSuffix(s.func) in ESCAPE_ANNOTATIONS));
    if (unannotated.length === 0) return [];
    return [
      {
        message: `${unannotated.length} escape site(s) reachable from core1 have no annotation in ESCAPE_ANNOTATIONS`,
        symbols: unannotated.map((s) => `${s.func}: ${s.detail}`),
      },
    ];
  },
};

// --- rule 1: no executable byte at a flash VMA that core1 can reach -------
//
// INVERTED from the original placement-only rule (docs/decisions/0004/0005:
// copy_to_ram, whole image, both cores) once the sixth invariant (decision
// 0016, the framebuffer malloc) made that whole-image cost unaffordable: an
// 11-app build needs SRAM copy_to_ram cannot give back. The hazard itself
// has NOT changed - core0's bootbtn.c still borrows the flash chip select on
// a timer with interrupts off, unconditionally, regardless of what core1 is
// doing - so the fix has to keep being total FOR CORE1, while letting core0
// (which is the one doing the borrowing, and is `__no_inline_not_in_flash_
// func` + interrupts-off for the borrow itself, decision 0004/0005) run
// everything else from flash by XIP.
//
// This reuses rule 0's reachability graph rather than a second traversal,
// per the task this rule was rewritten under: the same allRoots(fw) call,
// the same reachableFrom(). Rule 0 is what makes this sound rather than
// hopeful - it already refuses to pass while any core1-reachable indirect
// or handler-installing call site is unexplained, so by the time this rule
// runs, `reach.reached` is not merely "everything the BFS could follow" but
// "everything the BFS could follow, PLUS everything an unresolved edge on
// that path was manually traced to" (EXTRA_ROOTS_RESOLVED,
// perCoreInitializerRoots() above).
const FLASH_REGION_NAME = "FLASH";

// Reachable from core1 only via the panic path
// (hard_assertion_failure -> panic -> ...), and decision 0007 already ruled
// on the parallel question for rule 2 (the stdio-lock hazard): a panicking
// core1 is not made safe by anything panic() itself does, because it is
// already lost by the time it gets there, and core0's own liveness guard
// (sensors_restart_core1(), decision 0004/0005) recovers a dead core1 for
// ANY reason - a wedged i2c1 wait, a genuine LOCKUP, or a panic - without
// depending on how, or whether, core1's panic path completes. That argument
// carries over to THIS hazard unchanged: if a fetch inside panic() itself
// gets corrupted by a chip-select borrow, the observable result is still
// "core1 stops advancing", which is exactly the signature the liveness
// guard already watches for and already recovers from. RAM-pinning newlib's
// printf/panic formatting machinery for a core that is, by the time it gets
// there, already being torn down and restarted regardless, buys nothing.
// Kept as its own named set (not merely reusing rule 2's `exceptions`
// object) because the two rules' reasoning, while parallel, is not the same
// claim - rule 2 is about a mutex, this is about an instruction fetch - and
// a future change to one must not silently change the other.
export const PANIC_PATH_ALREADY_LOST = new Set([
  "panic",
  "hard_assertion_failure",
  "__wrap_puts",
  "weak_raw_vprintf",
  "stdio_put_string",
  // The two mutex veneers stdio_put_string's own print_mutex acquire/
  // release resolve to - see PANIC_PATH_VENEERS' own comment above for why
  // these need a separate entry rather than inheriting stdio_put_string's.
  ...PANIC_PATH_VENEERS,
]);

export const rule1NoCodeInFlash: Invariant = {
  name: "no executable byte at a flash VMA that core1 can reach",
  why:
    "core0 borrows the flash chip select to read BOOT, unconditionally, on " +
    "a timer, regardless of what core1 is doing; a fetch during the borrow " +
    "returns garbage and the fetching core stops without faulting. The " +
    "original fix (copy_to_ram, decisions 0004/0005) made this safe by " +
    "moving the WHOLE image to RAM; decision 0016 made that unaffordable " +
    "for an 11-app build. This is the narrower, equally-total form: every " +
    "function core1 can reach - by direct branch, or by an escape site " +
    "rule 0 has resolved and required to be a root - must not be placed at " +
    "a flash VMA. Code core1 never reaches (every app, the menu, gfx, " +
    "storage, sound, devlink - all core0-only) is free to run from flash.",
  see: "docs/decisions/0005-rca-core1-dies-on-first-button.md",
  check(fw) {
    const flash = fw.regions.find((r) => r.name === FLASH_REGION_NAME);
    if (!flash) {
      throw new Error(`invariant checker: no "${FLASH_REGION_NAME}" region in the linker map`);
    }
    const reach = reachableFrom(fw.functions, allRoots(fw));
    const bad: { name: string; addr: number }[] = [];
    for (const name of reach.reached) {
      if (PANIC_PATH_ALREADY_LOST.has(stripCloneSuffix(name))) continue;
      const fn = fw.functions.get(name);
      if (!fn) {
        // Every reached name comes from a ROOT (checked to exist) or a
        // resolved branch target objdump itself named - disasm.ts builds
        // `functions` from every function header in the whole image, so a
        // miss here means the model and the graph have gone out of sync,
        // not that this symbol is somehow benign. Fail loud, per this
        // tool's own rule about lines/data it does not understand.
        throw new Error(
          `invariant checker: core1-reachable symbol "${name}" has no disassembled function body - cannot check its placement`
        );
      }
      if (fn.addr >= flash.origin && fn.addr < flash.origin + flash.length) {
        bad.push({ name, addr: fn.addr });
      }
    }
    if (bad.length === 0) return [];
    bad.sort((a, b) => a.addr - b.addr);
    return [
      {
        message: `${bad.length} function(s) reachable from core1 are placed at a flash VMA`,
        symbols: bad.map((b) => `${b.name} (vma=0x${b.addr.toString(16)})`),
      },
    ];
  },
};

// --- rules 2 and 3: "reachable from core1 must not intersect set Y" -------

function reachableExcludes(opts: {
  name: string;
  why: string;
  see: string;
  forbidden: string[];
  // Canonical names allowed through despite matching `forbidden`, each with
  // its own reason - printed alongside a hit so an exception is visible,
  // never a silent pass.
  exceptions?: Record<string, string>;
}): Invariant {
  return {
    name: opts.name,
    why: opts.why,
    see: opts.see,
    check(fw): Violation[] {
      const reach = reachableFrom(fw.functions, allRoots(fw));
      const hits = reachedNamesMatching(reach, opts.forbidden);
      const real = hits.filter((h) => !(stripCloneSuffix(h) in (opts.exceptions ?? {})));
      const excepted = hits.filter((h) => stripCloneSuffix(h) in (opts.exceptions ?? {}));
      const violations: Violation[] = [];
      if (real.length > 0) {
        violations.push({
          message: `forbidden symbol(s) reachable from core1: ${opts.name}`,
          symbols: real,
        });
      }
      if (excepted.length > 0 && process.env.INVARIANTS_VERBOSE) {
        for (const e of excepted) {
          console.log(`  [exception] ${e}: ${opts.exceptions![stripCloneSuffix(e)]}`);
        }
      }
      return violations;
    },
  };
}

export const rule2NoStdioOnCore1: Invariant = reachableExcludes({
  name: "nothing on the core1 path takes the stdio lock",
  why:
    "sensors.h's ownership-rule banner: stdio takes a lock that core0 " +
    "also takes, so a core1 printf/puts turns a diagnostic into a " +
    "deadlock. Core1 publishes counters instead (sensors_stats_t). Both " +
    "the source name and its --wrap'd compiled form are listed - a real " +
    "`printf()` call on core1 was mutation-tested against an earlier " +
    "version of this list that named only `printf`/`puts` and passed " +
    "silently, because pico-sdk's PICO_STDIO_ENABLE_CRLF_SUPPORT wraps " +
    "both symbols (__wrap_printf/__wrap_vprintf, __wrap_puts) and the " +
    "unwrapped names never appear in the linked image at all.",
  see: "docs/decisions/0006-invariant-checker.md",
  forbidden: [
    "printf",
    "puts",
    "__wrap_printf",
    "__wrap_vprintf",
    "__wrap_puts",
    "weak_raw_vprintf",
    "stdio_put_string",
  ],
  exceptions: {
    __wrap_puts: "reachable only via panic(); see docs/decisions/0007.",
    weak_raw_vprintf: "reachable only via panic(); see docs/decisions/0007.",
    stdio_put_string: "reachable only via panic(); see docs/decisions/0007.",
  },
});

export const rule3NoSdkI2cOnCore1: Invariant = reachableExcludes({
  name: "no SDK i2c symbol on the core1 path",
  why:
    "the SDK's i2c read path has an unbounded wait (decision 0004); we " +
    "carry local bounded versions (i2c1_write_bytes_bounded and friends, " +
    "sensors.c) and nothing else stops a future direct call to the SDK's " +
    "own blocking functions from reintroducing that hang.",
  see: "docs/decisions/0004-the-day-the-instruments-lied.md",
  forbidden: [
    "i2c_write_blocking",
    "i2c_write_blocking_until",
    "i2c_write_blocking_internal",
    "i2c_write_timeout_per_char_us",
    "i2c_write_burst_blocking",
    "i2c_read_blocking",
    "i2c_read_blocking_until",
    "i2c_read_blocking_internal",
    "i2c_read_timeout_per_char_us",
    "i2c_read_burst_blocking",
  ],
});

// --- rule 4: core1's stack region, the part the map can actually answer ---
//
// The map cannot measure headroom (that needs -fstack-usage, decision
// 0006 phase 5, "after rule 0 is green" - not attempted here). What it can
// check cheaply: the region exists, is the expected size, and nothing else
// shares it - the replacement for the runtime canary that lied (2048 of
// 2048 free, decision 0004).
const CORE1_STACK_REGION = "SCRATCH_X";
const CORE1_STACK_SECTION = ".stack1_dummy";
const CORE1_STACK_EXPECTED_BYTES = 2048;

export const rule4Core1StackRegion: Invariant = {
  name: "core1's stack region is exactly its own dummy section, at the expected size",
  why:
    "a core1 stack canary once reported 2048 of 2048 bytes free - a core " +
    "that calls functions and touches no stack, which is impossible " +
    "(decision 0004). The map-level version of that check cannot lie the " +
    "same way: it reads the linked layout, not a runtime counter.",
  see: "docs/decisions/0004-the-day-the-instruments-lied.md",
  check(fw) {
    const region = fw.regions.find((r) => r.name === CORE1_STACK_REGION);
    if (!region) {
      throw new Error(`invariant checker: no "${CORE1_STACK_REGION}" region in the linker map`);
    }
    const occupants = fw.sections.filter(
      (s) => s.size > 0 && s.vma >= region.origin && s.vma < region.origin + region.length
    );
    const violations: Violation[] = [];
    if (occupants.length !== 1 || occupants[0]!.name !== CORE1_STACK_SECTION) {
      violations.push({
        message: `${CORE1_STACK_REGION} should hold exactly ${CORE1_STACK_SECTION} and nothing else`,
        symbols: occupants.map((s) => `${s.name} (size=0x${s.size.toString(16)})`),
      });
    } else if (occupants[0]!.size !== CORE1_STACK_EXPECTED_BYTES) {
      violations.push({
        message: `${CORE1_STACK_SECTION} is ${occupants[0]!.size} bytes, expected ${CORE1_STACK_EXPECTED_BYTES}`,
        symbols: [CORE1_STACK_SECTION],
      });
    }
    return violations;
  },
};

// --- rule 5: the framebuffer must fit in whatever SRAM the linked image leaves
//
// Measured 2026-08-15: an 11-app build linked cleanly, passed rules 0-4, and
// bricked the board. `gfx_init()` (firmware/runtime/gfx.c) does
// `gfx_fb = malloc(PANEL_W * PANEL_H * 2)` and `runtime.c` hangs forever if
// that malloc returns NULL - deliberately, because a NULL framebuffer is
// worse than a dead board. Nothing above sees this: the framebuffer is a
// runtime allocation, not a linker one, so no section, symbol or region
// names it. This rule is the whole-image arithmetic that stands in for the
// allocator: how much SRAM does the linked image leave, and does that cover
// the one allocation this firmware cannot survive failing.
//
// Where every number in it comes from, so a failure explains its own
// arithmetic instead of asserting a number nobody can check:
//
// - "how much SRAM is left" is answered from the same three pieces rule 1
//   and rule 4 already parse from the map and the section table - no new
//   input.
// - "how big is the framebuffer" is answered by reading
//   firmware/runtime/gfx.h and firmware/lib/AMOLED/AMOLED_1in8.h as text,
//   the same discipline model.ts applies to binutils output ("a line the
//   parser does not understand fails the run"), just aimed at two headers
//   instead of objdump. This is new: every other invariant here reads only
//   the built artifact. It has to, because the artifact has nothing to read
//   - the same fact that let the bug through in the first place. Precedent:
//   tools/gate's arena-headroom rule already reads its capacity from the
//   firmware (emu_arena_capacity()) rather than restating APP_ARENA_BYTES in
//   TypeScript, for the identical reason - a hardcoded 329728 is a check
//   that lies the day PANEL_W, PANEL_H or the pixel format changes.

const GFX_H = "firmware/runtime/gfx.h";
const AMOLED_1IN8_H = "firmware/lib/AMOLED/AMOLED_1in8.h";

function readDeviceSource(relPath: string): string {
  const abs = join(DEVICE_ROOT, relPath);
  if (!existsSync(abs)) {
    throw new Error(`invariant checker: no such firmware source file: ${relPath} (looked for ${abs})`);
  }
  return readFileSync(abs, "utf8");
}

// Not a preprocessor: it resolves exactly the two shapes rule 5 needs
// (`#define NAME VALUE` and `#define NAME OTHER_NAME`) and throws on
// anything else, same as every parser in this tool.
function findDefine(text: string, name: string, path: string): string {
  const re = new RegExp(`^\\s*#define\\s+${name}\\s+(\\S+)`, "m");
  const m = re.exec(text);
  if (!m) throw new Error(`invariant checker: no "#define ${name}" found in ${path}`);
  return m[1]!;
}

// gfx_fb's element type decides bytes-per-pixel; read from its own extern
// declaration rather than assumed, so a future switch away from RGB565
// (a wider pixel format, say) fails loudly here instead of quietly
// undercounting the allocation.
const BYTES_PER_PIXEL_BY_TYPE: Record<string, number> = { uint16_t: 2 };
const GFX_FB_DECL = /extern\s+(\w+)\s*\*\s*gfx_fb\s*;/;

interface FramebufferRequirement {
  width: number;
  height: number;
  bytesPerPixel: number;
  bytes: number;
}

function framebufferBytesFromSource(): FramebufferRequirement {
  const gfxH = readDeviceSource(GFX_H);
  const panelH = readDeviceSource(AMOLED_1IN8_H);

  // PANEL_W/PANEL_H are themselves aliases (gfx.h: "#define PANEL_W
  // AMOLED_1IN8_WIDTH"), one indirection resolved against the panel driver's
  // own header rather than assumed to be numeric in gfx.h.
  const widthAlias = findDefine(gfxH, "PANEL_W", GFX_H);
  const heightAlias = findDefine(gfxH, "PANEL_H", GFX_H);
  const widthTok = findDefine(panelH, widthAlias, AMOLED_1IN8_H);
  const heightTok = findDefine(panelH, heightAlias, AMOLED_1IN8_H);
  const width = Number(widthTok);
  const height = Number(heightTok);
  if (!Number.isInteger(width) || width <= 0) {
    throw new Error(
      `invariant checker: ${AMOLED_1IN8_H}'s "${widthAlias}" did not parse to a positive integer: ${JSON.stringify(widthTok)}`
    );
  }
  if (!Number.isInteger(height) || height <= 0) {
    throw new Error(
      `invariant checker: ${AMOLED_1IN8_H}'s "${heightAlias}" did not parse to a positive integer: ${JSON.stringify(heightTok)}`
    );
  }

  const declMatch = GFX_FB_DECL.exec(gfxH);
  if (!declMatch) {
    throw new Error(`invariant checker: ${GFX_H}: no "extern TYPE *gfx_fb;" declaration found`);
  }
  const fbType = declMatch[1]!;
  const bytesPerPixel = BYTES_PER_PIXEL_BY_TYPE[fbType];
  if (bytesPerPixel === undefined) {
    throw new Error(
      `invariant checker: ${GFX_H}: gfx_fb's element type "${fbType}" has no known byte width - ` +
        `update BYTES_PER_PIXEL_BY_TYPE`
    );
  }

  return { width, height, bytesPerPixel, bytes: width * height * bytesPerPixel };
}

function regionOrThrow(fw: Firmware, name: string): Region {
  const r = fw.regions.find((x) => x.name === name);
  if (!r) throw new Error(`invariant checker: no "${name}" region in the linker map`);
  return r;
}

function withinRegion(s: Section, r: Region): boolean {
  return s.vma >= r.origin && s.vma < r.origin + r.length;
}

// malloc's actual ceiling. Confirmed, not assumed, from two independent
// sources: the map defines both `__HeapLimit` and `__StackLimit` as
// `ORIGIN(RAM) + LENGTH(RAM)` (pico-sdk's section_heap.incl /
// section_end.incl), and pico-sdk's own `_sbrk()`
// (src/rp2_common/pico_clib_interface/newlib_interface.c) refuses to grow
// the heap past `&__StackLimit`, returning `(void*)-1` (malloc's NULL)
// rather than overrunning it. So "how much SRAM is left for the
// framebuffer" is answered entirely inside the RAM region; nothing outside
// it can extend that ceiling.
const HEAP_BOUND_REGION = "RAM";

// pico-sdk's crt0 heap spacer (script_include/section_heap.incl): the
// linker sets `__end__` (the symbol `_sbrk()` starts allocating from) to
// THIS SECTION'S OWN START ADDRESS, before its declared bytes are placed.
// So `.heap`'s size is the first slice of the free heap, not bytes taken
// away from it - counting it as "used" would double-subtract space `_sbrk`
// actually hands out. Verified against the .map on this tree: `__end__`
// lands exactly at `.heap`'s start address, matching its own comment
// ("historically sbrk was growing past __HeapLimit... we now set
// __HeapLimit explicitly to where the end of the heap is").
const HEAP_PLACEHOLDER_SECTION = ".heap";

// The two stacks that share this chip's SRAM with everything else: core1's
// (rule 4's own CORE1_STACK_SECTION, reused rather than redeclared) and
// core0's, pico-sdk's ordinary crt0 stack. Found, not assumed: both are
// named, sized (0x800 each) sections in the map, exactly like every other
// section this rule sums.
const CORE0_STACK_SECTION = ".stack_dummy";

// Below this many free bytes, the build is treated the way decision 0006
// treats an unannotated escape site: not a failure, but not silent either.
// Chosen from measurement, not a round guess: the six-game merge
// (758e739 -> 4132437) added about 53KB of linked .text+.bss across seven
// new apps, ~7.6KB/app average. 16384 (16KB) is a little over twice that
// average - enough that a single ordinarily-sized new app cannot flip a
// WARN straight to a brick without a build ever printing the word "WARN"
// first.
const MARGIN_WARN_BYTES = 16384;

interface HeapArithmetic {
  ramRegion: Region;
  usedBytes: number;
  countedSections: string[];
  freeBytes: number;
  fb: FramebufferRequirement;
  marginBytes: number; // freeBytes - fb.bytes; negative means it does not fit
  core1Stack: Section;
  core0Stack: Section;
}

function computeHeapArithmetic(fw: Firmware): HeapArithmetic {
  const ramRegion = regionOrThrow(fw, HEAP_BOUND_REGION);

  const countedSections: string[] = [];
  let usedBytes = 0;
  for (const s of fw.sections) {
    if (s.size <= 0) continue;
    if (!s.flags.has("ALLOC")) continue;
    if (s.name === HEAP_PLACEHOLDER_SECTION) continue;
    if (!withinRegion(s, ramRegion)) continue;
    usedBytes += s.size;
    countedSections.push(`${s.name} (${s.size}B @ 0x${s.vma.toString(16)})`);
  }
  const freeBytes = ramRegion.length - usedBytes;

  const core1Stack = fw.sections.find((s) => s.name === CORE1_STACK_SECTION && s.size > 0);
  if (!core1Stack) throw new Error(`invariant checker: no "${CORE1_STACK_SECTION}" section in the image`);
  const core0Stack = fw.sections.find((s) => s.name === CORE0_STACK_SECTION && s.size > 0);
  if (!core0Stack) throw new Error(`invariant checker: no "${CORE0_STACK_SECTION}" section in the image`);
  // Either stack landing inside the heap-bound RAM region is not a
  // hypothetical this rule silently trusts away: withinRegion() above
  // already sums every ALLOC section it finds there, by address, not by an
  // allowlist of names - so a future layout that puts a stack back inside
  // RAM is counted as "used" automatically, the same run it happens, with
  // no edit to this file required.

  const fb = framebufferBytesFromSource();
  const marginBytes = freeBytes - fb.bytes;

  return { ramRegion, usedBytes, countedSections, freeBytes, fb, marginBytes, core1Stack, core0Stack };
}

function stackLocationNote(s: Section, ramRegion: Region): string {
  const where = withinRegion(s, ramRegion)
    ? "inside the heap-bound RAM region - its bytes are already included above"
    : "outside the heap-bound RAM region - cannot compete with the framebuffer's malloc " +
      "(pico-sdk's _sbrk refuses to grow the heap past the RAM region's own end)";
  return `${s.name}: ${s.size}B at 0x${s.vma.toString(16)}, ${where}`;
}

export const rule5FramebufferFitsInHeap: Invariant = {
  name: "the panel framebuffer's malloc fits in the SRAM the linked image leaves",
  why:
    "gfx_init() (firmware/runtime/gfx.c) mallocs PANEL_W*PANEL_H*2 bytes for " +
    "the framebuffer, and runtime.c hangs forever on purpose if that malloc " +
    "returns NULL rather than run with a NULL framebuffer. An 11-app build " +
    "measured 2026-08-15 linked cleanly, passed every invariant above, and " +
    "bricked the board: the allocation is a runtime malloc, not a linker " +
    "allocation, so nothing in the artifact names it and no toolchain " +
    "instrument sees it coming. This rule is the arithmetic that stands in " +
    "for the allocator at build time.",
  see: "docs/decisions/0006-invariant-checker.md",
  check(fw) {
    const a = computeHeapArithmetic(fw);
    if (a.marginBytes >= 0) return [];
    return [
      {
        message:
          `framebuffer does not fit: used ${a.usedBytes}B of ${a.ramRegion.length}B RAM, ` +
          `leaving ${a.freeBytes}B free; the framebuffer needs ${a.fb.width}x${a.fb.height}x` +
          `${a.fb.bytesPerPixel} = ${a.fb.bytes}B; short by ${-a.marginBytes}B`,
        symbols: [...a.countedSections, stackLocationNote(a.core1Stack, a.ramRegion), stackLocationNote(a.core0Stack, a.ramRegion)],
      },
    ];
  },
  note(fw) {
    const a = computeHeapArithmetic(fw);
    const lines = [
      `margin: ${a.marginBytes}B free after the ${a.fb.bytes}B framebuffer ` +
        `(used ${a.usedBytes}B of ${a.ramRegion.length}B RAM, framebuffer ${a.fb.width}x${a.fb.height}x${a.fb.bytesPerPixel})`,
    ];
    if (a.marginBytes >= 0 && a.marginBytes < MARGIN_WARN_BYTES) {
      lines.push(
        `WARN margin is only ${a.marginBytes}B, under the ${MARGIN_WARN_BYTES}B warn line - ` +
          `this build happened to work, it did not pass with room to spare`
      );
    }
    return lines;
  },
};

// --- rule 6: storage.c's sector fits inside the smallest partition this
//     firmware could ever be booted from -----------------------------------
//
// docs/decisions/0018: the RP2350 bootrom maps the XIP window onto the
// ACTIVE PARTITION, not the whole chip, so storage.c's one durable sector
// is placed at a PARTITION-RELATIVE offset (STORAGE_SECTOR_OFFSET =
// STORAGE_PARTITION_BYTES - FLASH_SECTOR_SIZE) rather than a chip-absolute
// one. STORAGE_PARTITION_BYTES is therefore a claim about how big the
// partition storage.c actually runs inside is, and the only thing that can
// falsify that claim is store/partitions.json - the file the board's own
// partition table is embedded from (decision 0011,
// store/bootloader/CMakeLists.txt's pico_embed_pt_in_binary()). This rule
// reads that file as data, the same discipline rule 5 already applies to
// gfx.h/AMOLED_1in8.h, and fails if STORAGE_PARTITION_BYTES exceeds the
// smallest partition sharing this firmware's own boot family
// ("rp2350-arm-s" - slot_a and slot_b; the manifest partition is family
// "data" and this image never boots from it, so it is excluded rather than
// making this check impossible to pass by construction).

const STORAGE_C = "firmware/runtime/storage.c";
const PARTITIONS_JSON = "store/partitions.json";
const FIRMWARE_PARTITION_FAMILY = "rp2350-arm-s";

interface PartitionEntry {
  name: string;
  start: number;
  size: number;
  families: string[];
}

// Not a full C preprocessor: strips a trailing numeric-literal suffix
// (u/U/l/L, possibly both) off a single-token #define and parses what is
// left as a decimal or hex integer - the one extra step findDefine's own
// caller (rule 5) never needed, because PANEL_W/PANEL_H carry no suffix.
function parseNumericDefine(text: string, name: string, path: string): number {
  const tok = findDefine(text, name, path);
  const stripped = tok.replace(/[uUlL]+$/, "");
  const value = Number(stripped);
  if (!Number.isInteger(value) || value <= 0) {
    throw new Error(
      `invariant checker: ${path}'s "#define ${name}" did not parse to a positive integer: ${JSON.stringify(tok)}`
    );
  }
  return value;
}

function readPartitionTable(): PartitionEntry[] {
  const abs = join(DEVICE_ROOT, PARTITIONS_JSON);
  if (!existsSync(abs)) {
    throw new Error(`invariant checker: no such file: ${PARTITIONS_JSON} (looked for ${abs})`);
  }
  let parsed: unknown;
  try {
    parsed = JSON.parse(readFileSync(abs, "utf8"));
  } catch (e) {
    throw new Error(`invariant checker: ${PARTITIONS_JSON} did not parse as JSON: ${(e as Error).message}`);
  }
  const partitions = (parsed as { partitions?: unknown }).partitions;
  if (!Array.isArray(partitions)) {
    throw new Error(`invariant checker: ${PARTITIONS_JSON} has no "partitions" array`);
  }
  return partitions.map((p): PartitionEntry => {
    const name = (p as { name?: unknown }).name;
    const start = (p as { start?: unknown }).start;
    const size = (p as { size?: unknown }).size;
    const families = (p as { families?: unknown }).families;
    if (
      typeof name !== "string" ||
      typeof start !== "number" ||
      typeof size !== "number" ||
      !Array.isArray(families) ||
      !families.every((f) => typeof f === "string")
    ) {
      throw new Error(`invariant checker: ${PARTITIONS_JSON} has a malformed partition entry: ${JSON.stringify(p)}`);
    }
    return { name, start, size, families: families as string[] };
  });
}

export const rule6StorageFitsPartition: Invariant = {
  name: "storage.c's durable sector fits inside the smallest partition this firmware could boot from",
  why:
    "storage.c's STORAGE_SECTOR_OFFSET is PARTITION-RELATIVE (docs/decisions/0018: the RP2350 " +
    "bootrom maps the XIP window onto the active partition, not the chip), so it is only ever " +
    "valid if it stays inside every partition this same image could actually be booted from. " +
    "store/partitions.json is the file that claim is checked against, not a number restated by " +
    "hand in two places that could drift apart silently.",
  see: "docs/decisions/0018-two-address-spaces-one-flash-offset.md",
  check(_fw) {
    const storageSrc = readDeviceSource(STORAGE_C);
    const partitionBytes = parseNumericDefine(storageSrc, "STORAGE_PARTITION_BYTES", STORAGE_C);

    const bootable = readPartitionTable().filter((p) => p.families.includes(FIRMWARE_PARTITION_FAMILY));
    if (bootable.length === 0) {
      throw new Error(
        `invariant checker: ${PARTITIONS_JSON} has no partition of family "${FIRMWARE_PARTITION_FAMILY}" - ` +
          `cannot check storage.c's sector against it`
      );
    }
    const smallest = bootable.reduce((a, b) => (a.size < b.size ? a : b));
    if (partitionBytes <= smallest.size) return [];
    return [
      {
        message:
          `storage.c's STORAGE_PARTITION_BYTES (${partitionBytes}) exceeds "${smallest.name}"'s ` +
          `own size (${smallest.size}) in ${PARTITIONS_JSON} - a board that booted from ` +
          `"${smallest.name}" would compute a storage sector that falls outside its own partition`,
        symbols: [`STORAGE_PARTITION_BYTES=${partitionBytes}`, `${smallest.name}.size=${smallest.size}`],
      },
    ];
  },
};

export const ALL_INVARIANTS: Invariant[] = [
  rule0EscapesAnnotated,
  rule1NoCodeInFlash,
  rule2NoStdioOnCore1,
  rule3NoSdkI2cOnCore1,
  rule4Core1StackRegion,
  rule5FramebufferFitsInHeap,
  rule6StorageFitsPartition,
];
