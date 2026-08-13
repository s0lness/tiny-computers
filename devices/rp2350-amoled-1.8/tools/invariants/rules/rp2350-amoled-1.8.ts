// Device-specific invariants for rp2350-amoled-1.8. Per docs/decisions/0006:
// the ELF/map/graph machinery (../model.ts, ../disasm.ts, ../graph.ts)
// knows nothing about this board; everything here does, so moving the
// machinery to `puck` later only means importing it from a new path.

import { reachableFrom, reachedNamesMatching, stripCloneSuffix } from "../graph";
import type { Firmware, Invariant, Violation } from "../types";

// core1_trampoline is pico-sdk's own multicore.c launch stub; core1_entry
// and core1_fault_handler are ours (sensors.c). All three are seeded
// directly, rather than relying on the graph to connect them, because
// core1_trampoline's body is `pop {r0, r1, pc}` - not a bl/b/b.w/blx - so a
// direct-branch graph could never discover core1_entry as its callee.
const ROOTS = ["core1_entry", "core1_trampoline", "core1_fault_handler"];

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
};

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
    const reach = reachableFrom(fw.functions, ROOTS);
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

// --- rule 1: no executable byte at a flash VMA, outside the boot allowlist -

// The only executable section the fix (docs/decisions/0004/0005,
// copy_to_ram) leaves at a flash VMA: crt0's reset path, the boot-time
// vector table, and the default unhandled-ISR stubs - reachable only at
// cold reset or on an already-dead machine, per decision 0006's review.
const FLASH_REGION_NAME = "FLASH";
const FLASH_CODE_ALLOWLIST = new Set([".flashtext"]);

export const rule1NoCodeInFlash: Invariant = {
  name: "no executable byte at a flash VMA, outside the boot allowlist",
  why:
    "core0 borrows the flash chip select to read BOOT; a fetch during the " +
    "borrow returns garbage and the fetching core stops without faulting. " +
    "The fix is copy_to_ram, whole image, both cores - a placement rule, " +
    "not a reachability one, because the hazard is total: it does not " +
    "care whether the fetching code is ever reached from core1's roots.",
  see: "docs/decisions/0005-rca-core1-dies-on-first-button.md",
  check(fw) {
    const flash = fw.regions.find((r) => r.name === FLASH_REGION_NAME);
    if (!flash) {
      throw new Error(`invariant checker: no "${FLASH_REGION_NAME}" region in the linker map`);
    }
    const bad = fw.sections.filter(
      (s) =>
        s.flags.has("CODE") &&
        s.size > 0 &&
        s.vma >= flash.origin &&
        s.vma < flash.origin + flash.length &&
        !FLASH_CODE_ALLOWLIST.has(s.name)
    );
    if (bad.length === 0) return [];
    return [
      {
        message: "executable section(s) placed at a flash VMA outside the allowlist",
        symbols: bad.map((s) => `${s.name} (vma=0x${s.vma.toString(16)}, size=0x${s.size.toString(16)})`),
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
      const reach = reachableFrom(fw.functions, ROOTS);
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

export const ALL_INVARIANTS: Invariant[] = [
  rule0EscapesAnnotated,
  rule1NoCodeInFlash,
  rule2NoStdioOnCore1,
  rule3NoSdkI2cOnCore1,
  rule4Core1StackRegion,
];
