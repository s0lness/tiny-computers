# 0017: RAM-place only what core1 can reach, not the whole image

Date: 2026-08-15
Status: implemented, all six invariants pass, hardware validation owed (the
board is deliberately not flashed by this change - see "What this cannot
prove without the board" below)

## The problem, again

Decision 0016 measured it that morning: `copy_to_ram` (decisions 0004/0005's
fix for the day core1 died on the first button press) copies the WHOLE
image to SRAM, and an 11-app build needs more SRAM for its own `.text` than
that leaves for the one allocation this firmware cannot survive failing -
`gfx_init()`'s 329,728-byte framebuffer malloc. Short by 46,380 bytes. The
board cannot be flashed until this is solved.

## The fix

Stop copying everything. Keep in RAM only what core1 can execute; let
everything core1 never touches - every app, the menu, gfx, storage, sound,
devlink, `runtime_core.c`'s own dispatch - run from flash by XIP, same as a
normal embedded image.

This is sound rather than hopeful because it does not weaken the hazard
this project already paid a day to learn (decision 0004): core0 borrows the
flash chip select to read BOOT, unconditionally, on a 50ms timer, regardless
of what core1 is doing, and a core1 instruction fetch during that borrow
returns garbage. Core0 itself is safe by construction (it is the one doing
the borrow, inside `__no_inline_not_in_flash_func`, interrupts off). Core1
is the only core actually at risk, and this change makes ALL of core1's
reachable code RAM-resident, exactly as total as `copy_to_ram` was for it -
narrower only in that it stops paying for code core1 was never going to
execute.

## What actually changed

**`firmware/CMakeLists.txt`**: `pico_set_binary_type(main copy_to_ram)` is
gone. The board now links as pico-sdk's ordinary "default" binary type
(flash XIP, with `__not_in_flash_func`/`__time_critical_func`-annotated code
still placed in RAM by the linker script's own `.time_critical*` handling -
that mechanism is what this change leans on).

**`firmware/runtime/sensors.c` and `tilt.c`**: every function actually on
core1's path - `core1_entry` itself, the bounded i2c1 helpers, the touch/
IMU/PMIC/RTC pollers, the fault handler chain, `tilt_submit_device_g` and
its two small callees - is annotated `__not_in_flash_func` (a hand-rolled
equivalent in tilt.c, which stays free of any pico-sdk include on principle;
see that file's own comment).

**`firmware/linker_overrides/default_text_excludes.incl` and
`default_rodata_excludes.incl`**: pico-sdk's own default linker script
already excludes libgcc/libc/libm wholesale from flash placement (why
software float/division helpers and newlib's mem-family functions were
already fine, with no work here). Nothing excluded pico-sdk's OWN C sources
(pico_time, pico_multicore, hardware_irq, pico_flash, ...) or this repo's
vendor code (`firmware/lib/Touch`), because those are not archive members
`EXCLUDE_FILE` can reach by the patterns that already existed. Rather than
patching the pico-sdk checkout (which this repo does not do, except one
functional vendor bug fix under `firmware/lib`), these two files use
`pico_add_linker_script_override_path()` - a documented pico-sdk extension
point - to REPLACE the two files, adding an explicit, individually-justified
list of every file that defines a core1-reachable function. See that file's
own long header comment for the complete list and the reasoning behind each
entry, including two real toolchain surprises found only by building and
reading the `.map` file rather than assuming: this toolchain's actual C
library archive is `libg.a`, not `libc.a` (the SDK's own exclude pattern
silently misses it), and `firmware/lib/Touch/FT3168.c` links as an archive
(`libTouch.a`) rather than a plain object, so a same-named exclude pattern
by source path matched nothing at all.

## Why the inverted rule 1 is sound, and how I convinced myself

**It reuses rule 0's reachability graph, not a second traversal** (the
brief for this change said to; it also happens to be the only way this is
trustworthy). `tools/invariants/rules/rp2350-amoled-1.8.ts`'s
`rule1NoCodeInFlash` calls the exact same `reachableFrom(fw.functions,
allRoots(fw))` rule 0 already computes and gates, then checks every reached
function's own address against the FLASH region instead of checking section
placement wholesale. Nothing core1 cannot reach is ever inspected, and
nothing at a flash VMA escapes unless it is core1-unreachable or explicitly
excepted (the panic path - see below).

**Building the root set correctly is what this soundness actually rests
on, and it took real work to get right.** Reusing rule 0's graph is only as
sound as what feeds it. Widening the RAM-placement question from "is
`copy_to_ram` on" (a single boolean, unconditionally true or false) to "is
every core1-reachable FUNCTION individually RAM-placed" turned three
pre-existing, previously-inert gaps in this project's own reachability
model into load-bearing ones - each found by building the real 11-app
image and reading its actual disassembly and map, not by reasoning about
what the toolchain probably does:

1. **`bx <reg>` other than `bx lr` was not tracked as an indirect site at
   all** - only `blx r*` was. Decision 0006 found nine such sites in the
   whole image and left the gap open deliberately, since under
   `copy_to_ram` nothing depended on it. One of the nine is a tail call
   inside `flash_safe_execute_core_init()` (pico_flash), reachable from
   `core1_entry`. `tools/invariants/disasm.ts` now tracks `bx <reg>`
   identically to `blx r*` (excluding `bx lr`, an ordinary return); this
   surfaced the site, which is now a resolved extra root
   (`default_core_init_deinit`, added deterministically: nothing in this
   firmware overrides the weak `get_flash_safety_helper()` or reassigns
   `default_flash_safety_helper`'s fields, so the target is not a guess).
2. **`core1_trampoline`'s landing point, `core1_wrapper` (pico_multicore),
   was never in the reachable set at all.** The jump from the trampoline
   into it is `pop {r0, r1, pc}`, not a `bl`/`b`/`b.w`/`blx`/`bx` - the
   exact same reason `core1_trampoline` itself had to be a seeded root
   rather than a discovered edge, one level deeper than anyone had reason
   to look before this change needed placement, not just reachability, to
   be right. `core1_wrapper` is now a resolved extra root too, and its own
   `(*entry)()` tail call (another `bx r*`) is annotated: this firmware
   only ever calls `multicore_launch_core1(core1_entry)`, so the target is
   deterministic.
3. **pico-sdk's per-core initializer array** (`runtime_run_per_core_
   initializers()`, called from `core1_wrapper`) walks a linker-collected
   table via `blx r3` in a loop - a THIRD escape shape, and this one's
   target set is link-time data, not a fixed pair of names. Rather than
   hardcode a list a future SDK bump could silently invalidate,
   `perCoreInitializerRoots()` reads the actual symbol table between the
   two boundary symbols (`__pre_init_first_per_core_initializer` and
   `__init_array_start`) and derives the six current entries automatically
   - a build with a different SDK version or a different per-core
   initializer set changes what the checker expects, with no edit needed
   here.

Rules 0, 2 and 3 all moved to this same widened root set (`allRoots()`),
not only rule 1: they were previously checking a narrower, slightly wrong
picture of core1's real call graph, which could only ever have
under-reported a stdio-lock or SDK-i2c violation reachable solely through
one of these three newly-resolved edges.

**A fourth escape shape was found only by testing the placement claim
against `nm`, not by trusting a green build - and this is the part of this
change I am least comfortable calling fully closed.** An ARM linker veneer
(`ldr.w pc, [pc]` plus a literal pool word, inserted whenever a `bl`'s
direct-branch encoding cannot bridge a RAM-resident caller to a
flash-resident callee or vice versa) is not a `bl`/`b`/`b.w`/`blx`/`bx <reg>`
either, and the reachability graph does not follow it at all. Four real
functions (`panic`, `FT3168_Reset`, `__wrap_atan2f`, `strlen`, `_exit`) were
still at flash VMAs, still genuinely reachable from core1, and rule 1
printed PASS regardless, because the graph only ever saw the tiny
RAM-resident veneer stub, never the real function past it. Found by hand:
after every green run, I re-derived the full reachable set with a scratch
script and checked every `*_veneer` symbol it still contained against `nm`
for where it actually jumped. Fixed by moving the real targets into RAM too
(closing the distance that required a veneer in the first place, which
removes the blind spot rather than papering over it), not by widening the
graph to model this fourth shape - see
`firmware/linker_overrides/default_text_excludes.incl`'s own "THE HONEST
LIMIT" paragraph for the full account, including why I did not also teach
rule 0 to recognise it (this pattern recurs throughout the image for
ordinary far calls unrelated to core1 at all - `__memcpy_veneer`,
`____wrap_atan2f_veneer` - so gating on it generically is a materially
bigger change than this task's brief, and decision 0006's own precedent for
the `bx <reg>` gap argues against widening a rule's detection silently,
mid-fix, without treating it as its own decision). **What is true today,
verified**: every `*_veneer` symbol left in the reached set after the fix
resolves to a real, confirmed-RAM function - checked once, by hand, after
the last rebuild. **What is not true**: the checker itself cannot
re-verify this claim on a future rebuild the way it can everything else it
checks. A future change that reintroduces distance between a core1-reached
function and something it calls (annotating LESS, not more - the direction
this class of regression would come from) could reopen a veneer silently.
This is the one place in this change where I am reporting "I checked, and
it currently holds" rather than "the tool proves it holds forever," and it
is the most likely way this specific fix could regress without a build
failure - see the report's own "single most likely way this kills core1"
answer.

**The panic path is exempted from placement, extending decision 0007's
reasoning to a new hazard.** Decision 0007 already ruled that a panicking
core1 is "already lost" for the stdio-lock hazard (rule 2): core0's own
liveness guard (`sensors_restart_core1()`) recovers a dead core1 for any
reason, including a panic, without depending on how or whether its panic
path completes. The same argument extends to the flash-fetch hazard this
rule enforces: if a fetch inside `panic()` itself gets corrupted by a
chip-select borrow, the observable result is still "core1 stops advancing",
exactly the signature the liveness guard already watches for and already
recovers from. `PANIC_PATH_ALREADY_LOST` in `rules/rp2350-amoled-1.8.ts`
encodes this as its own named exception (not reused from rule 2's, since
the two rules' reasoning, while parallel, is not the same claim). In
practice, after fixing the veneer gap above, the whole panic chain ended
up RAM-resident anyway - the exemption is a documented backstop, not
load-bearing for placement today.

## Measured, not asserted: RAM before and after

Both builds measured with the same address-scoped arithmetic rule 5 already
uses (sum every `ALLOC` section whose VMA falls inside the linker map's
`RAM` region, excluding the crt0 `.heap` placeholder - see decision 0016
for why that exclusion is correct).

**Before** (this device root's own prior state, matching decision 0016's
own measurement of the 11-app `copy_to_ram` build):

| section | bytes |
|---|---|
| `.ram_vector_table` | 272 |
| `.text` (the whole image) | 145,964 |
| `.data` | 14,976 |
| `.bss` | 79,728 |
| **used** | **240,940** |
| RAM region | 524,288 |
| **free** | **283,348** |
| framebuffer needed | 329,728 |
| **margin** | **-46,380 (fails)** |

**After** (this change, final state, both `.text` and `.rodata` overrides
applied):

| section | bytes |
|---|---|
| `.ram_vector_table` | 272 |
| `.data` (core1's RAM-pinned code + ordinary initialised data) | 43,504 |
| `.bss` | 79,728 |
| **used** | **123,504** |
| RAM region | 524,288 |
| **free** | **400,784** |
| framebuffer needed | 329,728 |
| **margin** | **71,056 (passes)** |

Net SRAM recovered by RAM-pinning only core1's reachable code instead of
the whole image: **117,436 bytes** (240,940 - 123,504). The margin (71,056
bytes) is over ten times the last known-good `copy_to_ram` build's own
margin (6,888 bytes, decision 0016) and comfortably clears the 16,384-byte
WARN line rule 5 itself defines.

## What ended up pinned to RAM, and why

The full, current, verified list (51 functions, checked address-by-address
against the FLASH region after the fix - see
`tools/invariants/rules/rp2350-amoled-1.8.ts`'s rule 1 for the mechanical
version of this check):

**This repo's own code** (`__not_in_flash_func`/`TILT_NOT_IN_FLASH` in
`sensors.c`/`tilt.c`): `core1_entry`, `core1_fault_handler`,
`core1_fault_handler_c`, `core1_install_fault_handlers`, the bounded i2c1
helpers (`i2c1_write_bytes_bounded`, `i2c1_write_to`, `i2c1_write_reg_to`,
`i2c1_read_bytes_bounded`, `i2c1_read_reg_n_to`), `touch_read_fingers_to`,
`touch_read_xy_to`, `touch_set_active_to`, `touch_q_push`,
`touch_recover_core1`, `touch_diag_poll_core1`, `imu_poll_core1`,
`pmic_poll_core1`, `pmic_write_selftest_core1`, `pmic_poweroff_poll_core1`,
`bcd_to_bin`, `bin_to_bcd`, `clock_publish`, `rtc_set_poll_core1`,
`device_to_panel`, `lp_alpha`, `tilt_submit_device_g` - every one of these
runs on core1, in its normal loop or in an interrupt/fault path core1 can
enter, so all of it must be RAM-resident by the same total argument
`copy_to_ram` used, scoped to the one core that needs it.

**pico-sdk, RAM-pinned via the linker-script override** (not annotated at
the source, since this repo does not patch pico-sdk): the bounded-lock and
IRQ primitives `multicore_lockout_victim_init()` needs
(`hw_claim_lock`/`unlock`, `next_striped_spin_lock_num`,
`spin_lock_claim_unused`, `spinlock_set_extexclall`, `lock_init`,
`mutex_init`, `irq_set_exclusive_handler`, `irq_set_enabled`,
`exception_set_exclusive_handler`); the pico_flash entry point core1 calls
directly plus its two escape targets (`flash_safe_execute_core_init`,
`get_flash_safety_helper`, `default_core_init_deinit`,
`multicore_lockout_victim_init`); the multicore boot path
(`core1_trampoline`, `core1_wrapper`, `runtime_run_per_core_initializers`
and the six per-core initializer entries: `first_per_core_initializer`,
`runtime_init_per_core_bootrom_reset`, `runtime_init_per_core_enable_
coprocessors`, `spinlock_set_extexclall`, `runtime_init_per_core_irq_
priorities`, `runtime_init_per_core_tls_setup`); the boot-ROM lookup
`runtime_init_per_core_bootrom_reset` uses (`rom_func_lookup` - its own
code is RAM-pinned; its indirect jump target, the RP2350's on-chip boot
ROM, is a different physical memory the QSPI borrow cannot touch at all,
so the escape itself needs no placement, only an annotation - see rule 0's
`ESCAPE_ANNOTATIONS`); timing primitives (`time_us_64`, `timer_time_us_64`,
`sleep_ms`, `best_effort_wfe_or_timeout`, `busy_wait_until`,
`alarm_pool_add_alarm_at_force_in_context`, `alarm_pool_cancel_alarm`);
`mutex_try_enter`/`mutex_enter_block_until`/`mutex_try_enter_block_until`/
`mutex_exit` (already `__time_critical_func` in pico-sdk's own
`pico_sync/mutex.c`, found already RAM with no work here); the panic chain
(`panic`, `hard_assertion_failure`, `weak_raw_vprintf`, `stdio_put_string`,
`__wrap_puts`, `_exit`); `__wrap_atan2f` (`pico_float`'s hand-written VFP
assembly - `tilt_submit_device_g`'s one real trig call; `sqrtf`/`fabsf`
both compile to a single inline FPU instruction each and need no entry at
all, confirmed unchanged from decision 0004's own finding); libgcc's
64-bit division helpers (`__aeabi_uldivmod`, `__udivmoddi4`,
`__aeabi_idiv0` - free, already excluded by pico-sdk's own default rule);
`strlen` (this toolchain's `libg.a`, a real toolchain-naming surprise - see
`default_text_excludes.incl`'s own account).

**Vendor code** (`firmware/lib/Touch`, RAM-pinned via the linker-script
override, whole-archive since it has exactly one member): `FT3168_Reset`,
called directly by `touch_recover_core1()`.

**Everything else - every app, `runtime_core.c`, `gfx.c`, `menu.c`,
`storage.c`, `sound.c`, `devlink.c`, and the bulk of pico-sdk (USB, PIO,
the display driver stack, clocks, stdio init) - is core0-only and now runs
from flash by XIP**, which is what recovers the SRAM decision 0016 needed.

## What this cannot prove without the board

This task's own constraints forbid flashing or opening a serial port - the
board is deliberately on older firmware after this morning's brick
recovery, and the owner will flash it, watched, once this is reviewed. So,
honestly:

- **That the veneer-closure claim above (every `*_veneer` symbol resolves
  to a confirmed-RAM function) is not itself hiding a fifth escape shape
  neither this project nor I have seen yet.** The static analysis here is
  thorough but is still static analysis of one compiler's output on one
  toolchain version; a different optimisation level, a different GCC
  point release, or a future source change could produce a control-flow
  shape none of rule 0's three modeled escapes (`blx r*`, `bx <reg>`,
  handler-installer calls) or my own by-hand veneer audit anticipated.
- **Timing.** Code that used to execute from RAM via `copy_to_ram` now
  executes some of its OWN startup/recovery path (core1's boot sequence,
  `touch_recover_core1()`'s `FT3168_Reset()`... no, both of those are now
  RAM too - the honest timing question is narrower than it first looks,
  because everything core1 executes routinely IS still RAM-resident,
  exactly as before. The place timing could differ is core0: every app,
  `gfx_push()`, `menu.c`'s dispatch and every other core0-only function now
  executes from flash by XIP instead of RAM, which is measurably slower
  per instruction on real silicon (XIP cache misses cost real cycles this
  device's 16ms frame budget has no slack for, decision 0003's own point
  that the emulator cannot answer this). `tools/gate` cannot measure this
  either - it is pixel/rectangle bookkeeping, not a clock. **What I would
  want measured on the board**: the same per-app frame-time instrumentation
  this project has used before (the profiler line, or a dedicated timing
  probe) on the hottest-drawing apps (`sketch.c`'s stroke rendering,
  `menu.c`'s grid redraw, `four.c`'s full-panel column pushes) to confirm
  the frame budget still holds now that their code fetches over XIP
  instead of from SRAM. I have no reason to expect this fails - XIP on
  RP2350 has a cache, and this device's own 12ms full-panel push (AGENTS.md)
  was always measured on hardware running SOME mix of flash and RAM even
  before this change (the vendor driver stack, `hardware_pio`/`hardware_dma`
  etc were never inside `copy_to_ram`'s reach in the sense that mattered
  for THIS specific hazard - wait, they were, `copy_to_ram` copied
  everything - so this is a genuine, not-yet-measured behavioural change,
  and it is the one this report cannot close out from a desk.
- **The acceptance test decisions 0004/0005 themselves defined**: ten real
  PWR presses over several minutes, each starting and stopping the
  stopwatch, survived without a core1 death. Nothing about this change
  alters the reasoning that made the original fix work (core1 never
  fetches from flash, full stop, still true here, just scoped to core1's
  own reachable code instead of the whole image) - but the whole POINT of
  decision 0004 was that reasoning alone had already failed once, and only
  hardware settled it that time too.

## The single most likely way this kills core1 in the field despite passing every check

**A future edit adds one call from an already core1-reachable function
into a new pico-sdk or vendor function, at -O3, that the compiler places
far enough from its RAM-resident caller that the linker inserts a veneer -
and nobody notices, because rule 1 passes.** This is not hypothetical: it
is exactly the shape of gap this document spent its longest section
closing for the CURRENT tree, four times, by hand. The checker's own
"what it cannot do" now has a fourth entry it did not have before this
change (the `ldr.w pc, [pc]` veneer shape, alongside `copy_to_ram`-era
decision 0006's disclosed-but-then-inert `bx <reg>` gap, which THIS change
made load-bearing and closed). A veneer only appears when the RAM/flash
distance is large enough to need one, so the failure mode is specifically
triggered by adding a new, not-yet-RAM-pinned dependency to core1's path -
the same "one innocent call silently reopens the hole" pattern decision
0004 identified as how this bug returned the first time, one layer further
down the toolchain than source-level review can see. The mitigation that
exists is procedural, not mechanical: rule 1 passing is necessary but, as
of this change, provably not sufficient - anyone touching core1's path
(sensors.c, tilt.c, or anything they call into) should re-run this
document's own by-hand check (list every `reach.reached` name, `nm` every
`*_veneer` symbol still present, confirm each resolves to a RAM address)
until rule 0 is taught to model this fourth escape shape, which is the
correct permanent fix and is explicitly left as future work here rather
than rushed into this change.
