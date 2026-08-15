# 0016: A sixth invariant for the one allocation nothing upstream can see

Date: 2026-08-15
Status: implemented (`tools/invariants/rules/rp2350-amoled-1.8.ts`'s rule 5),
red/green evidence below

## The failure, measured this morning

The board firmware builds `copy_to_ram` (decisions 0004/0005): the whole
image, `.text` included, lives at a RAM VMA, so the linked image occupies
real SRAM before a single app runs. `gfx_init()` (`firmware/runtime/gfx.c`)
then does:

```c
gfx_fb = (uint16_t *)malloc((size_t)PANEL_W * PANEL_H * 2);
```

368x448x2 = 329,728 bytes, and `runtime.c` hangs forever in `for (;;) {}` if
that malloc returns NULL, on purpose - a NULL framebuffer is worse than a
dead board. An 11-app build linked cleanly, passed all five existing
invariants, passed a clean gate, passed 32/32 emulator tests, and then hung
the board into a USB device that could not report its own vendor ID.
Recovery took an hour: unplugging, 12-second power holds, BOOTSEL attempts.

**Nothing in the toolchain could see it coming**, because the framebuffer is
a runtime `malloc`, not a linker allocation. The link succeeds either way;
the device dies or doesn't depending on a number no section, symbol or
region names.

This is decision 0010's law, applied to memory rather than to pixels or
touch: every instrument here reads at one point on the chain from source to
a child's hand, and validates only what is upstream of that point. The five
existing invariants read the linked artifact. The gate reads the emulator's
framebuffer and, since its own fix, the panel buffer downstream of that. The
regression tests read pushed windows. **All of them read strictly before the
one call that actually decides whether this build boots**, because that call
happens after every one of their reading points, at a moment none of them
model: `malloc()`, at runtime, on real silicon.

## What the sixth invariant checks

`rule5FramebufferFitsInHeap` (`tools/invariants/rules/rp2350-amoled-1.8.ts`),
wired into the same post-build step as the other five
(`firmware/CMakeLists.txt`). It stands in for the allocator at build time:
given the linked image's own footprint, would `gfx_init()`'s malloc have
succeeded.

**1. SRAM actually consumed**, derived, not restated. Every `ALLOC` section
with a nonzero size and a VMA inside the `RAM` region (from the `.map`'s
Memory Configuration table, the same table rule 1 and rule 4 already read)
is summed and named in the failure text. On the current tree that is
`.ram_vector_table`, `.text` (copy_to_ram's own doing), `.data` and `.bss` -
whatever the linker actually placed there, not a hardcoded list of names.
One deliberate exclusion, justified in the code: pico-sdk's crt0 `.heap`
placeholder section. The linker script sets `__end__` (where `_sbrk()`
starts allocating) to `.heap`'s own *start* address, before its declared
bytes are placed, so `.heap`'s size is the first slice of free heap, not
bytes taken away from it - counting it as "used" would double-subtract space
the allocator actually hands out. Confirmed against
`pico-sdk/src/rp2_common/pico_standard_link/script_include/section_heap.incl`
and `section_end.incl`, not assumed.

**2. The framebuffer requirement, from source, not restated.** `PANEL_W` and
`PANEL_H` are read out of `firmware/runtime/gfx.h` (resolving gfx.h's own
one-level alias to `AMOLED_1IN8_WIDTH`/`HEIGHT` in
`firmware/lib/AMOLED/AMOLED_1in8.h`), and the bytes-per-pixel figure is read
from `gfx_fb`'s own `extern uint16_t *gfx_fb;` declaration rather than
written as a bare `2` - a table maps known element types to their width and
throws on anything it does not recognise, so a future switch away from
RGB565 fails loudly instead of quietly undercounting. This mirrors
`tools/gate`'s own `arena-headroom` rule, which already reads its capacity
from the firmware (`emu_arena_capacity()`) rather than restating
`APP_ARENA_BYTES` in TypeScript - a hardcoded 329,728 is a check that lies
the day the panel or the pixel format changes. The difference is *how* it
reads: the gate calls into compiled code at runtime; this rule, which never
executes anything, reads the two headers as text, with the same discipline
model.ts applies to binutils output ("a line the parser does not understand
fails the run") aimed at `#define` lines instead.

**3. The stacks, found rather than guessed - and found to not currently
matter, which is itself the finding.** The brief this rule was built against
assumed core0's and core1's stacks compete with the framebuffer for the same
SRAM and should be subtracted. Investigating the map instead of assuming
found something more specific: `__HeapLimit` and `__StackLimit` are both
defined as `ORIGIN(RAM) + LENGTH(RAM)` (pico-sdk's linker script), and
`_sbrk()` (`pico_clib_interface/newlib_interface.c`) refuses to grow the
heap past `&__StackLimit`, returning malloc's NULL rather than overrunning
it. Core1's stack (`.stack1_dummy`, rule 4's own section) and core0's
(`.stack_dummy`) both live in `SCRATCH_X`/`SCRATCH_Y` - separate 4KB banks
immediately *after* `RAM` ends, not inside it. So on this linker layout the
two stacks physically share the chip with the framebuffer's heap but cannot
compete with it for bytes: malloc cannot reach past the RAM region's own
end regardless of what sits beyond it. The rule does not hardcode this
conclusion, though: it sums every `ALLOC` section *by address* inside the
RAM region, so if a future SDK change ever puts a stack back inside `RAM`
(the older, single-region layout the linker script's own comment says this
one replaced), that stack's bytes are counted as "used" the same run it
happens, with no edit to this file. The check still reports both stacks'
location and size on every run, inside or outside, so the reasoning stays
auditable rather than assumed.

**4. Fails the build with the arithmetic in the message**, always: bytes
used (and which sections), bytes free, bytes needed, the shortfall in one
subtraction anyone can redo by hand.

**5. Warns, without failing, under a measured threshold.** The reference
"last working" build (758e739) passed with a margin this rule computes at
6,888 bytes free after the framebuffer - not the 180 bytes the brief's own
morning measurement (`arm-none-eabi-size`'s text+bss total) reported; see
"What the brief got wrong" below for why the two numbers differ and why the
one this rule computes is the correct one. Either way, "passed" is not "had
room to spare": **16,384 bytes** is the warn line, chosen from measurement
rather than a round guess - the six-game merge (758e739 to 4132437) added
about 53KB of linked `.text`+`.bss` across seven new apps, roughly 7.6KB per
app. 16KB is a little over twice that average, so a single ordinarily-sized
future app cannot flip a WARN straight into the exact brick this rule exists
to prevent without a build printing the word "WARN" first. The PASS line
prints the current margin on every run, always - not only once it turns
red - because 180 bytes (or 6,888) was itself the evidence a build had
already been quietly PASSING with the wrong grade for its entire life.

## Red, then green - both real builds

Per this project's own rule (decision 0006's acceptance test: "a checker
never shown to fail on a real bad build is not known to work"), both
reference commits were built fresh with the installed toolchain, not
inferred from the numbers already sitting in a report.

**758e739 ("last working"), a pre-existing worktree build reused as-is**
(`arm-none-eabi-size`: text 112,752 / bss 81,628, matching the morning's own
measurement exactly) - **rule 5 PASSES**, with a printed margin:

```
PASS  the panel framebuffer's malloc fits in the SRAM the linked image leaves
  margin: 6888B free after the 329728B framebuffer (used 187672B of 524288B RAM, framebuffer 368x448x2)
  WARN margin is only 6888B, under the 16384B warn line - this build happened to work, it did not pass with room to spare
```

**4132437 ("11 apps"), rebuilt from scratch in a temporary worktree**
(`cmake -S firmware -B firmware/build`, default Release) - **rule 5 FAILS**,
all five existing invariants still PASS:

```
FAIL  the panel framebuffer's malloc fits in the SRAM the linked image leaves
  framebuffer does not fit: used 240940B of 524288B RAM, leaving 283348B free; the framebuffer needs 368x448x2 = 329728B; short by 46380B
    - .ram_vector_table (272B @ 0x20000000)
    - .text (145964B @ 0x20000110)
    - .data (14976B @ 0x20023b40)
    - .bss (79728B @ 0x200275c0)
    - .stack1_dummy: 2048B at 0x20080000, outside the heap-bound RAM region - cannot compete with the framebuffer's malloc
    - .stack_dummy: 2048B at 0x20081000, outside the heap-bound RAM region - cannot compete with the framebuffer's malloc
```

A third build, the same commit rebuilt `MinSizeRel` (`-Os`), was also
produced (in the same scratch worktree) to check the brief's claim that
`-Os` alone does not save this build: it does not - rule 5 still fails,
short by 1,452 bytes, checked directly against `check()` since this
particular build hit a pre-existing, unrelated gap in the disassembly
parser (an objdump output shape rule 0's graph does not recognise at `-Os`,
newly surfaced by this exercise and left as found - out of this change's
scope, and now known rather than silently possible).

The current working tree, rebuilt in place while writing this (a0829c4 plus
one uncommitted, in-flight `menu.c` change from another agent sharing this
worktree) **also failed rule 5**, short by 46,380 bytes - the live checkout
was itself sitting on the exact cliff this document exists to describe,
discovered by the instrument that now exists to catch it rather than by a
second bricked board.

## What the brief got wrong

The morning's own numbers (180 bytes of margin on 758e739) were computed
from `arm-none-eabi-size`'s classic text/data/bss buckets, which classify
sections by ELF flags, not by address - so `.flashtext`, `.rodata`,
`.ARM.exidx`, `.binary_info` and `.flash_end` (a few hundred bytes, resident
at a **flash** VMA, executing at cold reset only, per rule 1's own
allowlist) get folded into the same "text" total as the SRAM-resident
`.text`. This rule sums by VMA against the region table instead, which is
strictly more correct and produces a real, still-thin-but-larger margin:
6,888 bytes, not 180. The qualitative verdict is identical either way (a
thin pass, a large failure, a `-Os` rebuild that still fails) - the brief's
instinct that "180 bytes of margin" was alarming was right, the literal
number was an artifact of a coarser tool being pointed at a question it was
never built to answer precisely.

The brief also asked this rule to "account for the stacks... find them and
subtract them." Investigating found the opposite of what that phrasing
assumed: on this linker layout the two stacks cannot be subtracted from the
framebuffer's budget, because they were never inside it - `_sbrk()`'s own
bound already excludes `SCRATCH_X`/`SCRATCH_Y`. Had this rule subtracted
2,048+2,048 bytes anyway, `758e739` would fail it, contradicting the one
build known to have actually worked. The honest version of "accounting for
the stacks" turned out to be verifying they do not currently cost anything
and building the arithmetic so it would notice if that ever became false,
not applying a fixed penalty.

## What else shares this blind spot

Searched: `firmware/runtime`, `firmware/apps` and `firmware/lib` contain
exactly one call to `malloc`/`calloc`/`realloc` in the whole image - the
framebuffer. Nothing else currently lives in this blind spot. The app arena
(`APP_ARENA_BYTES`, `firmware/runtime/app.h`) looks like a second candidate
but is not one: it is a fixed-size `static uint8_t g_arena[...]` in `.bss`,
a linker allocation like any other, already summed by this very rule and
separately budgeted per-app by the gate's own `arena-headroom` rule at a
different layer (how much of the arena one app's `enter()` uses, not
whether the arena itself fits). The nearest cousin, not a twin, is flash
storage (decision 0011): a different physical memory, a different failure
mode (a torn write, not an unfulfillable allocation), already designed
around before the first flash-writing app shipped, with its own
invariant-checker rule named as future work there ("no flash-write symbol
reachable from app code"). If a second `malloc()` call is ever added to this
firmware, this rule's arithmetic does not automatically extend to cover it
- it computes one allocation's fit, named in its own `why`, not a general
heap-accounting engine. That would be the seventh invariant, written the
same way this one was: after something is understood, per decision 0006's
own honesty about what this tool is for.

## The number that mattered most

180 bytes - or, correctly computed, 6,888 - was never a passing grade. It
was the entire margin this project had, for its entire life up to last
night, between "boots" and "the owner's hour of power-cycling," and no
build before this one printed it. The instrument that would have said so
did not exist because nothing upstream of the allocator had ever needed to
model the allocator. Decision 0010 named the law in general; this is what
it costs to actually close one instance of it: not a new class of test, one
piece of arithmetic, read from the same map two other rules already parse,
plus two headers nothing here had read before.
