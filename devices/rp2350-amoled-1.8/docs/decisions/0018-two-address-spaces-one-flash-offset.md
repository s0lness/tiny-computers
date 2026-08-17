# 0018: A flash offset means two different things, and decision 0011 only checked one of them

Date: 2026-08-17
Status: implemented (`firmware/runtime/storage.c`, rule 6 in
`tools/invariants/rules/rp2350-amoled-1.8.ts`), red/green evidence below,
hardware validation owed - this document does not flash a board

## The bug, in one sentence

`storage.c` computed one flash offset and used it for two different address
spaces: a read through the XIP window, which the RP2350 bootrom maps onto
the ACTIVE PARTITION, and a write via `flash_range_erase()`/
`flash_range_program()`, which take an offset from the START OF THE CHIP.
At `16MB - 4KB` the read was so far outside the 7MB partition window that
the board never got past `storage_init()`: this is the white-screen freeze
AGENTS.md has carried as open since 2026-08-15.

## What was measured on the real board today

`picotool partition info` on this device:

```
0(A) 00100000->00800000  "slot_a"    (1MB -> 8MB, 7MB long)
1(A) 00800000->00f00000  "slot_b"    (8MB -> 15MB, 7MB long)
2(A) 00f00000->00f01000  "manifest"  (one 4KB sector)
```

The board boots `slot_a`. Sweeping XIP reads with a watchdog-armed probe
found the window's real shape: **reads succeed up to XIP offset 6.75MB and
stall the bus from 7.00MB up** - exactly `slot_a`'s declared 7MB length, not
the chip's 16MB. A stalled read freezes the core hard enough that it cannot
even service the USB interrupt, which matches AGENTS.md's own observation
that `picotool ... -f` cannot force a reboot once this happens: the request
is served by the hung app's own USB interface.

The old `STORAGE_FLASH_OFFSET`, `16MB - 4KB` (chip-absolute), sits at XIP
offset `15MB` inside `slot_a`'s 7MB window - nowhere close to 6.75MB. That
alone explains the freeze: `storage_init()`'s very first `memcpy()` off
`XIP_BASE + STORAGE_FLASH_OFFSET` reads from a stalled bus before the
watchdog is armed, which is exactly the "stable white screen, before
`watchdog_enable()`" symptom AGENTS.md already narrowed the search to.

**The second bug, independent of the first and worse in a quieter way.**
Setting the offset to something in-window (`7MB - 4KB`, still computed as a
chip-absolute number) and measuring the write showed the read and the write
land one megabyte apart and never meet: the sector at XIP offset `0x6FF000`
stayed all-`0xFF` (erased, never written), while the actual record showed up
one megabyte earlier, at XIP offset `0x5FF000`:

```
de c0 01 00 01 00 00 00 0c 90 48 c9 ff ff ff ff
```

`de c0` little-endian is magic `0xC0DE`, `01` is `kind` (`STORAGE_KIND_DINO_
HISCORE`), `01 00 00 00` is `value = 1`, `0c 90 48 c9` is a CRC that
validates, and the trailing `ff ff ff ff` is the untouched `reserved1`
field. A well-formed, valid record - written at the wrong address, one
partition-base (`slot_a` starts at `0x100000` = 1MB) away from where
anything would ever read it back. Nothing here would ever have thrown an
error: the save would appear to succeed, and the durable value would be
gone the instant the boot's RAM cache aged out, silently, forever.

## Why decision 0011 got half of this right

Decision 0011's flash section ("Yes, we can write a few bytes to flash")
reasoned entirely in chip-absolute terms - "Offset `0x00FFF000`, the last
sector of the 16MB part" - and that reasoning is exactly correct for
`flash_range_erase()`/`flash_range_program()`, which the pico-sdk
documents as taking "the offset into flash, in bytes". It never had reason
to distinguish that from a read, because at the time it was written this
firmware had no partition table at all: `store/`'s two-slot design (the
subject of decision 0002 section 6) was already retired in favour of the
single-binary runtime, and nothing suggested XIP would ever see less than
the whole chip. The partition table (`store/partitions.json`, `slot_a`/
`slot_b`/`manifest`) is older than decision 0011 as a *file* but was never
load-bearing for anything storage.c did until this board was actually
booted from a partitioned image - which is precisely the gap this document
closes: decision 0011 reasoned correctly about the chip's address space,
and the read happens in the partition's.

**The earlier fix (`6ea3a65`, "Tell the SDK this board has 16MB of flash")
was real and necessary, and still not sufficient.** `PICO_FLASH_SIZE_BYTES`
really was wrong (4MB, the Pico 2 board file's default, on a board with a
16MB part), and correcting it was the right call regardless: the SDK uses
that number to size the flash region in the linker script and the QMI's
address window, so a declared 4MB chip would make even an in-window access
look like it ran off the end of what the hardware was told to serve.
Correcting it did not touch the actual freeze, because the freeze was never
about the chip's declared size - the XIP window was never wide enough to
reach `16MB - 4KB` in the first place, 16MB declaration or not. Both facts
are true at once: the board really does have 16MB, and the offset needed
was never a chip-relative one to begin with.

## The fix

Two address spaces, named as what they are, computed independently:

- **Partition-relative** (`STORAGE_SECTOR_OFFSET`, `firmware/runtime/
  storage.c`): `STORAGE_PARTITION_BYTES - FLASH_SECTOR_SIZE`, i.e. the last
  4KB sector of a 7MiB partition. Feeds the XIP read - `XIP_BASE +
  STORAGE_SECTOR_OFFSET` - because the bootrom maps that window onto
  whichever partition is active, never the chip.
- **Chip-absolute** (`s_absoluteSectorOffset`, resolved once by
  `storage_init()`): the active partition's own base plus
  `STORAGE_SECTOR_OFFSET`. Feeds `flash_range_erase()`/
  `flash_range_program()`, which are chip-relative regardless of which
  partition is running.

The active partition's base is **discovered at runtime**, not hardcoded, so
the same image is correct whether it is running from `slot_a` (`0x100000`)
or `slot_b` (`0x800000`) - which of the two it is depends on which one
`store/`'s crash-recovery machinery last chose (decision 0002 section 6),
not on anything storage.c controls.

## The API, and where it is real

`pico/bootrom.h` (this SDK checkout:
`src/rp2_common/pico_bootrom/include/pico/bootrom.h`), guarded by
`#if !PICO_RP2040` (this is an RP2350 board, so the guard passes):

- **`rom_get_boot_info(boot_info_t *info)`** - a thin wrapper around the
  bootrom's `SYS_INFO_BOOT_INFO` query. `info->partition` is "the partition
  that was booted, or -1 if not applicable" (the header's own doc comment,
  line 1102's field). `0` is `slot_a`, `1` is `slot_b`, matching
  `store/partitions.json`'s own `"id"` field for each.
- **`rom_get_partition_table_info(uint32_t *out, uint32_t words, uint32_t
  flags)`** - fills a buffer with partition-table data selected by `flags`.
  Called with `PT_INFO_PARTITION_LOCATION_AND_FLAGS | PT_INFO_
  SINGLE_PARTITION | (partition << 24)` (the flag constants are in
  `src/rp2_common/boot_bootrom_headers/include/boot/bootrom_constants.h`),
  it returns exactly one partition's location, packed as first/last
  4KB-sector numbers (`PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_BITS`/`_LSB`,
  `src/common/boot_picobin_headers/include/boot/picobin.h`). Multiplying
  the first-sector number by `FLASH_SECTOR_SIZE` gives the partition's
  chip-absolute byte offset.

**This is not a new or invented API - it is the exact shape pico-sdk's own
`pico_cyw43_driver` uses at runtime**
(`src/rp2_common/pico_cyw43_driver/cyw43_driver.c`, `cyw43_driver_init()`)
to locate its firmware partition: the same two calls, the same flag
combination, the same shift-and-mask to recover a byte offset from a
sector-number field. `active_partition_base()` in `storage.c` is that
pattern, scoped to "the partition I am running from" instead of "the
partition holding a named blob".

**Linking needs no change.** `hardware_flash`'s own `CMakeLists.txt`
(`pico_mirrored_target_link_libraries(hardware_flash INTERFACE
pico_bootrom)`) already pulls in `pico_bootrom`, and `pico_bootrom` itself
links `boot_picobin_headers` as an interface dependency
(`src/rp2_common/pico_bootrom/CMakeLists.txt`), so `#include "boot/
picobin.h"` resolves without a new `target_link_libraries` entry in
`firmware/CMakeLists.txt`. `hardware_flash` was already linked before this
change (decision 0011, for the erase/program calls themselves).

## Failure is refusal, not a guess

If `rom_get_boot_info()` fails, or reports `partition < 0` (a RAM or debug
boot - "not applicable"), or the partition-table query does not return the
expected three words, `storage_init()` sets `s_nextFreeSlot = -2` and
returns without scanning anything. `storage_save_u32()` already treats `-2`
as "`storage_init()` never ran" and refuses outright. The two states are
now indistinguishable on purpose: from a caller's point of view, "storage
never initialised" and "storage could not learn where it is" are the same
fact - nothing here falls back to assuming `slot_a`.

## What happens to the stored value on a slot switch

Because the sector is partition-relative, `slot_a` and `slot_b` each keep
their **own** copy, at different chip-absolute addresses. Ordinary app
switching never reboots - decision 0002 made it a function call precisely
so this class of question would not come up during normal play. It only
matters for `store/`'s crash-recovery reboot path: a board that boots into
`slot_b` for the first time finds its own sector genuinely erased and
starts a fresh dino high score, even if `slot_a`'s sector, a few megabytes
away and no longer reachable through this partition's XIP window, still
holds an old one. Nothing in `storage.c` carries a value across that
boundary. If that is ever undesirable, it is `store/`'s problem to solve
(it already owns the decision of which slot boots next), not something this
file should paper over by guessing at a cross-partition read.

## The invariant: rule 6

`tools/invariants/rules/rp2350-amoled-1.8.ts`'s `rule6StorageFitsPartition`
reads `STORAGE_PARTITION_BYTES` out of `storage.c` as text (the same
discipline rule 5 already applies to `gfx.h`/`AMOLED_1in8.h` - "a line the
parser does not understand fails the run") and checks it against
`store/partitions.json`, filtered to partitions of family `rp2350-arm-s`
(the family `store/bootloader/CMakeLists.txt` actually embeds this image's
own table under - `slot_a` and `slot_b`; the `manifest` partition is family
`data` and this image never boots from it, so including it would make the
check impossible to pass by construction rather than meaningful). It fails
if `STORAGE_PARTITION_BYTES` exceeds the smallest such partition's own
`size`.

Both runs below are against the pre-existing `firmware/build/main.elf`/
`.map`, unmodified - `rule6StorageFitsPartition` reads only `storage.c` and
`store/partitions.json` as text, not the built artifact, so it needs no
rebuild between red and green; the other five invariants ran unchanged
alongside it on the same firmware, PASS both times, printed here in full
because the runner is one command over all six (now seven) rules, not a
per-rule switch.

### Red

`STORAGE_PARTITION_BYTES` set to `20000000u` (deliberately past both 7MiB
slots), `bun run tools/invariants/runner.ts`:

```
PASS  every indirect or handler-installing call site reachable from core1 is annotated
PASS  no executable byte at a flash VMA that core1 can reach
PASS  nothing on the core1 path takes the stdio lock
PASS  no SDK i2c symbol on the core1 path
PASS  core1's stack region is exactly its own dummy section, at the expected size
PASS  the panel framebuffer's malloc fits in the SRAM the linked image leaves
  margin: 71052B free after the 329728B framebuffer (used 123508B of 524288B RAM, framebuffer 368x448x2)
FAIL  storage.c's durable sector fits inside the smallest partition this firmware could boot from
  why: storage.c's STORAGE_SECTOR_OFFSET is PARTITION-RELATIVE (docs/decisions/0018: the RP2350 bootrom maps the XIP window onto the active partition, not the chip), so it is only ever valid if it stays inside every partition this same image could actually be booted from. store/partitions.json is the file that claim is checked against, not a number restated by hand in two places that could drift apart silently.
  see: docs/decisions/0018-two-address-spaces-one-flash-offset.md
  storage.c's STORAGE_PARTITION_BYTES (20000000) exceeds "slot_b"'s own size (7340032) in store/partitions.json - a board that booted from "slot_b" would compute a storage sector that falls outside its own partition
    - STORAGE_PARTITION_BYTES=20000000
    - slot_b.size=7340032

invariant checker: FAILED
```

`slot_b`, not `slot_a`, because `slot_a` and `slot_b` are the same size and
the smallest-of finds the last tied entry, not the first - both are
correctly over the line either way, so which one gets named is not
load-bearing for the check, only for which name shows up in the message.

### Green

`STORAGE_PARTITION_BYTES` restored to `7340032u`, same command:

```
PASS  every indirect or handler-installing call site reachable from core1 is annotated
PASS  no executable byte at a flash VMA that core1 can reach
PASS  nothing on the core1 path takes the stdio lock
PASS  no SDK i2c symbol on the core1 path
PASS  core1's stack region is exactly its own dummy section, at the expected size
PASS  the panel framebuffer's malloc fits in the SRAM the linked image leaves
  margin: 71052B free after the 329728B framebuffer (used 123508B of 524288B RAM, framebuffer 368x448x2)
PASS  storage.c's durable sector fits inside the smallest partition this firmware could boot from

invariant checker: all invariants passed
```

## What could not be established

**The runtime partition lookup is unverified on silicon.** This task's own
constraints forbid flashing a board or opening `COM4` - the owner will
verify on hardware himself. Everything about `active_partition_base()` is
read from the bootrom header's own documentation and from pico-sdk's real,
shipped `pico_cyw43_driver` caller of the identical two functions, not
observed on this puck. Specifically unverified:

1. That `rom_get_boot_info()` actually reports `partition == 0` when this
   image boots from `slot_a`, on this specific bootrom revision.
2. That `rom_get_partition_table_info()`'s sector-number encoding
   (`PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_BITS`) decodes to `0x100000`
   for `slot_a` and `0x800000` for `slot_b`, matching `store/
   partitions.json`, when read back on real hardware rather than reasoned
   about from the header and the JSON side by side.
3. That the corrected read address (`XIP_BASE + STORAGE_SECTOR_OFFSET`,
   partition-relative, `0x6FF000` into whichever partition is active) is
   genuinely inside the working window this document's own probe measured
   ending at 7.00MB - it should be, by 4KB of margin, but "should be" is
   exactly the standard decision 0010 exists to reject; only a board
   answers this for certain.
4. Everything decision 0011 already flagged as owed to hardware and still
   unflashed (whether a battery is fitted, `flash_safe_execute`'s lockout
   against the real `sensors.c` core1, real `tSE`/`tPP` on this part) is
   unchanged by this document and still open.

**What this document is confident about, and why:** the two hexdump/cliff
measurements above were taken on the real board today, not reasoned about,
and they are sufficient on their own to explain both the white-screen
freeze and the "durable value never read back" bug without needing the
partition-lookup API to be verified first - that part of this document is
a diagnosis, not a hypothesis. The API is the fix for a diagnosed problem;
it is the fix's own correctness, not the diagnosis, that is still owed to
silicon.
