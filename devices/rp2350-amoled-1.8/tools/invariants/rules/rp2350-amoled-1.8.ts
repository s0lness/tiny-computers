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

export const ALL_INVARIANTS: Invariant[] = [
  rule0EscapesAnnotated,
  rule1NoCodeInFlash,
  rule2NoStdioOnCore1,
  rule3NoSdkI2cOnCore1,
  rule4Core1StackRegion,
  rule5FramebufferFitsInHeap,
];
