# app store

Local app-store tooling for the RP2350-Touch-AMOLED-1.8, replacing the retired
6-slot on-device launcher (`store/launcher/`, `store/partition-table.uf2`,
now deleted) with the owner's actual requirement: **the device always shows
a single app**, or one of two, switched by holding the PWR button. No
wireless on this board (RP2350A has no radio), so everything here is USB +
`picotool`.

## Why the old design is gone

`store/launcher/main.c` was a menu screen: boot into it, tap a row, reboot
into the tapped app. That's a a second thing on screen before the real app,
which is exactly what "the device should always have a single app" rules
out. The 6-slot `partitions.json` sized each app at 1MB, which is also far
too small for a real app (the sketchpad's own `firmware/build/main.uf2` is
several times that once `strokes.h` and driver code are linked in).

Both are replaced by: two ~7MB app slots, no on-device menu, a shared
`appswitch.c` helper (`firmware/appswitch.h/.c`) that every app links,
watching PWR for a long press and requesting a switch, and a tiny
`bootloader/` (new, this directory) that is the only thing that actually
switches slots. See "Switching apps" and "The bootloader" below for why a
separate bootloader is required rather than the app rebooting straight into
the other slot.

## Partition layout

`partitions.json` (16MB flash total, matches `picotool info`'s "16MB flash"
report):

```
offset 0                                   partition table block (implicit)
        1,048,576 (0x00100000)   slot_a    start
        8,388,608 (0x00800000)   slot_a    end / slot_b start
       15,728,640 (0x00F00000)   slot_b    end / manifest start
       15,732,736 (0x00F01000)   manifest  end
       16,777,216 (0x01000000)   flash end
```

Arithmetic (JSON can't hold comments, so it lives here):

```
FLASH_TOTAL   = 16 MiB = 16 * 1024 * 1024        = 16,777,216 bytes
RESERVED      =  1 MiB = 1  * 1024 * 1024        =  1,048,576 bytes  (leading, unpartitioned)
SLOT_SIZE     =  7 MiB = 7  * 1024 * 1024        =  7,340,032 bytes  (slot_a and slot_b, each)
MANIFEST_SIZE = 4096 bytes

slot_a.start    = RESERVED                        = 1,048,576
slot_a.end      = slot_a.start + SLOT_SIZE         = 8,388,608
slot_b.start    = slot_a.end                       = 8,388,608
slot_b.end      = slot_b.start + SLOT_SIZE         = 15,728,640
manifest.start  = slot_b.end                       = 15,728,640
manifest.end    = manifest.start + MANIFEST_SIZE   = 15,732,736

trailing slack  = FLASH_TOTAL - manifest.end       = 1,044,480 bytes (~1020 KiB, left
                                                      unassigned; covered by `unpartitioned`)
```

Constraints kept from the retired layout (learned the hard way, no longer
written down anywhere else once `store/launcher/` is gone, so repeating them
here):

- **Sizes are integer bytes.** A string like `"1M"` is silently misparsed by
  `picotool partition create`. Every number above is a plain byte count.
- **Every partition needs an explicit `start`.** Without one, `picotool`
  auto-sizes/auto-positions partitions to fill the whole 16MB address space,
  which makes every partition expand and collide with its neighbour. All
  three partitions here set `start` explicitly.
- **`unpartitioned` is mandatory**, and must list the `absolute` family (see
  `partitions.json`'s top-level `unpartitioned` block) or partition-table
  creation fails / behaves unpredictably.
- **Nothing sits at offset 0.** The partition table block itself lives
  there. The leading 1MB reservation covers it with a wide margin (the
  reserved region was 1MB in the retired layout too, and that value was
  already working on this hardware, so it's kept rather than shaved down
  without being able to test the smaller alternative right now).

XIP (runtime) addresses are `0x10000000 + offset`, e.g. slot_a is
`0x10100000`..`0x10800000`, slot_b is `0x10800000`..`0x10F00000`, matching
the address style the retired launcher already used (`SLOT0_ADDR =
0x10100000`).

## Switching apps: `firmware/appswitch.h` / `firmware/appswitch.c`

**Not wired into `firmware/main.c`** (see the integration diff below — that
edit is left for you to apply and test; `apps/chrono/main.c` already wires
it in directly, see that file's `buttons_poll()`). The public API is exactly
two functions, and stays that way regardless of how switching is
implemented underneath: `appswitch_current_slot()` (which slot is this
build running from) and `appswitch_go_other()` (switch to the other one,
does not return on success). An earlier draft of this file described an
`appswitch_init()`/`appswitch_poll()` pair; that pattern was never
implemented — `appswitch.h` never declared those functions — and
`apps/chrono/main.c` calls `appswitch_go_other()` directly from its own
button handler instead, which is the pattern to follow when wiring up
`firmware/main.c` too.

Design: there is no on-device launcher at all. On a PWR (`SYS_OUT`, GPIO18)
press held longer than ~1.5s, the running app does NOT reboot directly into
the other slot. The first version tried exactly that, via
`rom_reboot(REBOOT2_FLAG_REBOOT_TYPE_FLASH_UPDATE | REBOOT2_FLAG_NO_RETURN_ON_SUCCESS, ...)`
aimed at the other slot's flash offset — the same bootrom call the retired
launcher used, just aimed at "the other slot" instead of "the tapped row" —
and it never switched. The RP2350 datasheet (section 5.1.16) says why,
verbatim: "Flash update and version downgrade have no effect when using a
single slot, or standalone (non A/B) partitions." `slot_a`/`slot_b` are
deliberately standalone partitions, not an A/B pair, so every flash-update
reboot was a no-op: the bootrom fell back to its default and re-booted slot
A, which from the outside looked exactly like switching into the same app
twice.

Linking `slot_a`/`slot_b` as an A/B pair to make `FLASH_UPDATE` work is not
the fix either — the same datasheet section says that boot type erases "the
first sector of the other image" every time, which would destroy the app
being switched *away from* on every single switch. A/B is an upgrade
mechanism (old version vs new version of the *same* app), not an app
switcher.

The actual mechanism, and the one the datasheet documents for exactly this
("implementing bootloaders", section 5.10.6), is `chain_image()`: search a
region of flash for a launchable image and jump to it, without touching
anything else. `chain_image()` has to be called by something other than the
image currently running — you cannot chain into a sibling while remaining
resident yourself — so this repo now has a tiny first-stage `bootloader/`
(this directory) whose only job is to call it. The long-press handler in a
running app now does the minimum needed to hand off to that bootloader: it
writes a "switch to slot X" request into a watchdog scratch register
(`firmware/bootreq.h`) and does a plain `watchdog_reboot(0, 0, 0)`. See "The
bootloader" below for the rest.

**How the app knows which slot it's in**: not a compile-time flag. It takes
the address of one of its own functions at runtime (`current_slot_addr()`)
and checks which slot's address range that falls in. This was chosen over a
build-time `-DAPP_SLOT=A/B` define because it can never drift out of sync
with reality: whatever address the running code is actually executing from
**is** the slot it's in, by construction, once the picotool finding below is
accounted for (a build must already be linked at its target slot's address,
so a running image's own code address is exactly its slot's address).

**GPIO18 polarity — confirmed, not guessed.** `vendor/demo/RP2350-Touch-AMOLED-1.8/C/01-LCD/main.c`:

```c
// PWR KEY
DEV_IRQ_SET(SYS_OUT, GPIO_IRQ_LEVEL_HIGH, &Touch_INT_callback);
...
else if(gpio == SYS_OUT)
{
    watchdog_reboot(0,0,0);
}
```

`GPIO_IRQ_LEVEL_HIGH` fires the callback while the pin reads high, and the
callback's only action for `SYS_OUT` is to reboot — so SYS_OUT reads **HIGH**
while PWR asserts. `DEV_GPIO_Init()` (`firmware/lib/Config/DEV_Config.c`)
configures GPIO18 as a plain input with **no pull**, consistent with a
PMIC-driven signal (AXP2101 `SYS_OUT`) rather than a switch to ground, which
would need a pull-up to read a stable idle level.

**GPIO18 hold behaviour — NOT determined, flagged rather than guessed.**
The vendor handler never measures duration: it reboots the instant the
level goes high. Nothing in `vendor/` shows whether SYS_OUT stays high for
the whole physical hold, or only pulses once (e.g. a PMIC "long-press
power-off" pulse). `appswitch.c` assumes a sustained high, because that is
the only assumption that lets a software long-press timer exist at all, and
documents the failure mode if it's wrong: the elapsed-time counter never
reaches 1.5s, so the switch simply never fires — safe (does nothing) rather
than firing on the wrong condition. **This needs confirming on real
hardware** (e.g. printf the raw GPIO18 level at ~50ms intervals while
holding PWR) before relying on it.

## The bootloader: `store/bootloader/`

A tiny, plain, normally-linked pico2 binary (no `firmware/lib/` drivers at
all — no display, no touch, no IMU) that lives in the unpartitioned space
below `0x00100000` (see "Partition layout" above), i.e. it links at the SDK
default `0x10000000`, exactly like a fresh `picotool load` with no `-p`
would put it. `CMakeLists.txt` additionally calls
`pico_override_flash_size(bootloader 0x00100000)` so an accidental size
regression is a link error, not a silent overlap into `slot_a`. Built size
today: ~34KB text + ~6KB bss, comfortably inside the 1MB budget.

At boot it:

1. Calls `rom_load_partition_table()` unconditionally. The partition table is
   **not** guaranteed resident on entry — `pico/bootrom.h`'s own docs for
   `rom_get_partition_table_info()` say it returns `BOOTROM_ERROR_NO_DATA`
   "if the partition table has not been loaded (e.g. from a watchdog or RAM
   boot)", and a watchdog reboot is exactly how `appswitch_go_other()` gets
   here. `force_reload=false` makes the call cheap when the table is already
   resident (a cold boot straight into the bootloader), so it's called every
   time rather than trying to detect which path led here.
2. Reads and clears the switch request via `firmware/bootreq.h`'s
   `bootreq_read_and_clear()` — see that header for exactly which watchdog
   scratch register is used and the evidence it doesn't collide with
   anything else in this SDK checkout. Defaults to slot 0 if no request is
   pending or the magic doesn't match.
3. Calls `get_partition_table_info()` for the target slot and computes the
   chain window straight from the datasheet's own bootloader example
   (section 5.10.6):
   ```c
   uint32_t partition_info[3];
   get_partition_table_info(partition_info, 3, PT_INFO_PARTITION_LOCATION_AND_FLAGS
       | PT_INFO_SINGLE_PARTITION | (boot_partition << 24));
   uint16_t first_sector_number = (partition_info[1]
       & PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_BITS)
       >> PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_LSB;
   uint16_t last_sector_number = (partition_info[1]
       & PICOBIN_PARTITION_LOCATION_LAST_SECTOR_BITS)
       >> PICOBIN_PARTITION_LOCATION_LAST_SECTOR_LSB;
   uint32_t data_start_addr = first_sector_number * 0x1000;
   uint32_t data_end_addr = (last_sector_number + 1) * 0x1000;
   uint32_t data_size = data_end_addr - data_start_addr;
   chain_image(workarea, 0xc00, (XIP_BASE + data_start_addr), data_size);
   ```
   `region_base` is an **XIP address** (`XIP_BASE + offset`), not a bare
   flash offset — the exact bug class that broke the original flash-update
   attempt, this time it would be the mirror image of that mistake (passing
   a flash offset where an XIP address is wanted, rather than the reverse).
4. If that fails, and the requested slot wasn't already slot 0, retries slot
   0. If that fails too, prints the failure and drops into BOOTSEL
   (`reset_usb_boot(0, 0)`) rather than bricking the device — a `picotool`
   session can always reach it over USB no matter what is or isn't in either
   slot.

`chain_image()` does not return on success, so `main()` "returning" is
always a failure path; the BOOTSEL fallback at the end is not a fallthrough
for an edge case, it's the only way `main()` can end.

**The work-area-size discrepancy — flagged, not resolved either way.** The
datasheet's own example above uses `0xc00` (3072 bytes) as the work area
size. The pico-sdk checkout this builds against (`pico/bootrom.h`,
`rom_load_partition_table()`/`rom_chain_image()`/`rom_pick_ab_partition()`
doc comments) states "The work area size currently required is 3264, so
3.25K is a good choice" — 192 bytes **larger** than the datasheet's number.
Since this code calls the SDK's compiled wrappers (not the datasheet's
pseudocode directly), `BOOTROM_ERROR_INSUFFICIENT_RESOURCES` would be
checked against whatever the actual ROM on this chip requires, which the SDK
header is presumably transcribing correctly for the ROM version it targets.
`store/bootloader/main.c` uses **3264**, the larger, SDK-documented number,
rather than the datasheet's `0xc00`. Not independently re-derived from the
ROM source; if a real board ever reports `BOOTROM_ERROR_INSUFFICIENT_RESOURCES`
from `rom_chain_image()`, this is the first thing to revisit.

**Which watchdog scratch register, and why it's safe**: `firmware/bootreq.h`
uses `WATCHDOG_SCRATCH0`. The header's own comment is the full derivation;
short version, every scratch-register consumer in this pico-sdk checkout was
checked before picking it — `watchdog_reboot()`/`watchdog_enable()` only
ever touch `SCRATCH4..7`, `pico_crt0`'s NO_FLASH-debugger-entry path (which
this build never takes) touches `SCRATCH2..7`, and `rom_get_boot_info()`
(what `appswitch_current_slot()` uses) goes through a bootrom software API,
not scratch registers at all. `SCRATCH0` and `SCRATCH1` have zero known
consumers; `SCRATCH0` is used, `SCRATCH1` is left free.

## The picotool finding (critical unknown, now answered)

**Question**: does `picotool load -p <partition>` relocate a normally-linked
UF2 into that partition, or must each app already be linked for its slot's
flash offset?

**Answer: it does NOT relocate.** Evidence, from picotool's own source
(`raspberrypi/picotool`, tag `2.3.0`, matching the `picotool v2.3.0` binary
installed at `~/pico/tools/picotool-dist/picotool/picotool.exe` — verified
with `picotool version`), function `load_command::execute()` in `main.cpp`:

```cpp
if (settings.load.partition >= 0) {
    auto partitions = get_partitions(con);
    ...
    uint32_t start = (*partitions)[settings.load.partition].start;
    uint32_t end = (*partitions)[settings.load.partition].end;
    settings.offset = start + FLASH_START;
    settings.offset_set = true;
    settings.partition_size = end - start;
}
...
bool ret = load_guts(con, file_access);
```

`-p` only sets `settings.offset` / `settings.partition_size`. Inside
`load_guts()`, the actual flash writes are driven entirely by
`get_coalesced_ranges(file_access, model)` — the memory ranges **embedded in
the file itself** (ELF segment virtual addresses, or a UF2 block's own
target address), not by `settings.offset`:

```cpp
for (auto mem_range : ranges) {
    ...
    raw_access.write_vector(base, file_buf); // base comes from mem_range.from,
                                              // i.e. from the file, not settings.offset
}
```

`settings.offset` (from `-p`) is used for exactly two things: a size bound
check (`flash_data_size > settings.partition_size`), and as the `dParam0` of
the post-load flash-update reboot when `-x` is given — telling the bootrom
which region to prefer, not where to write. Confirms the SDK side too:
`pico_standard_link`'s `pico_flash_region.template.ld` (the linker script
fragment every app includes) hardcodes
`FLASH(rx) : ORIGIN = 0x10000000, LENGTH = ${PICO_FLASH_SIZE_BYTES_STRING}`
— only `LENGTH` is parameterized by the SDK's own `pico_override_flash_size()`
helper; there is no built-in mechanism to change `ORIGIN`, and no
partition-aware CMake helper anywhere in `pico-sdk` 2.3
(`grep -r partition src --include=CMakeLists.txt` finds nothing relevant).

**Consequence**: each app slot needs its own build, linked with `FLASH
ORIGIN` set to that slot's real XIP address (`0x10100000` for slot_a,
`0x10800000` for slot_b), not just a copy of the same `main.uf2` loaded
twice with a different `-p`. That is why `store/catalog.json` holds a
`sourceDir` + `target` (a build recipe) rather than a path to a prebuilt
`.uf2`, and why `store/app.ts`'s `installApp()` calls `buildApp()` — a fresh
`cmake`/`ninja` build per install — before ever touching `picotool`.

The one place `-p` **is** the right, intended mechanism as-is: writing a raw
**BIN** file (the manifest). A `.bin` has no addresses of its own, so
`-p <partition> -t bin` is exactly "place this blob at that partition's
start" — that's how `store/app.ts` reads/writes the manifest partition.

## `store/catalog.json`

```json
{ "id": "sketchpad", "name": "Sketchpad", "description": "...",
  "sourceDir": "firmware", "target": "main" }
```

`sourceDir` is a path to a CMake project (relative to the repo root),
`target` is the executable name `pico_add_extra_outputs()` produces a
`.uf2` for. No `.uf2` path in the catalog on purpose — see the picotool
finding above.

## `store/app.ts`

Bun app, `127.0.0.1:5320` only, family-budget design system (vendored as
`store/family-budget.css` so the tool is self-contained). Shows the two
slots, what's in each (name from the manifest, occupancy from a raw scan
for the picobin block marker — mirrors the retired launcher's
`slot_occupied()`), the catalog, install/uninstall buttons, and a quit
button (there's no console to Ctrl+C once this runs headless). Mutating
routes (`/api/install`, `/api/uninstall`, `/api/quit`) require a
`x-rp2350-store: 1` header, which a same-origin `fetch()` can set but a
plain cross-origin form POST cannot — the CSRF guard from
`~/.claude/design/local-app.md`.

**"active" is an approximation**, and the UI says so: it reflects the last
slot the *store* told the bootrom to prefer (via a flash-update reboot at
install time), read back from the manifest. It is not a live introspection
of what the chip is executing right now — the whole point of `appswitch.c`
is that apps switch by rebooting, not by talking back to a host tool, so
there is no channel for the store to ask "what are you running" once a
device is out of BOOTSEL mode.

**Not run against real hardware.** The board was in active use by someone
else while this was written (hard constraint: no `picotool` against the
device, no flashing, no opening `COM4`). Every `picotool` invocation here is
based on its documented flag syntax (`picotool help load/save/erase/reboot`,
run offline) and its source for the load/partition semantics above, but the
first real install/uninstall should be watched by hand.

## Building and installing everything

Same environment as every other build in this repo (`../AGENTS.md`,
"Running it"):

```powershell
$env:PICO_SDK_PATH = "$env:USERPROFILE\pico\pico-sdk"
$env:PICO_TOOLCHAIN_PATH = "C:\Program Files (x86)\Arm GNU Toolchain arm-none-eabi\14.2 rel1"
$env:PATH = "C:\Users\sylve\.espressif\tools\cmake\3.30.2\bin;C:\Users\sylve\.espressif\tools\ninja\1.12.1;$env:PICO_TOOLCHAIN_PATH\bin;" + $env:PATH
```

**1. Build the bootloader** (once; it isn't per-slot, it's the thing that
reads the slots):

```powershell
cmake -S store/bootloader -B store/bootloader/build -G Ninja `
  -Dpicotool_DIR="$env:USERPROFILE\pico\tools\picotool-dist\picotool" `
  -Dpioasm_DIR="$env:USERPROFILE\pico\tools\sdk-tools\pioasm"
cmake --build store/bootloader/build
```

**2. Flash the partition table with the bootloader embedded**, so the device
boots straight into it instead of whatever was there before:

```powershell
& "$env:USERPROFILE\pico\tools\picotool-dist\picotool\picotool.exe" partition create `
  store/partitions.json store/partitions.uf2 store/bootloader/build/bootloader.elf
& "$env:USERPROFILE\pico\tools\picotool-dist\picotool\picotool.exe" load store/partitions.uf2 -f -x
```

`partition create <json> <out> <bootloader-elf>` is the exact three-argument
form the task brief specifies: the partition table JSON, an output UF2, and
the ELF to embed as the image that runs before any partition does. It writes
the partition table block *and* the bootloader into the unpartitioned region
in one file, so this one `load` puts both down together.

**3. Build and load each app into its slot.** Every app needs a per-slot
build — see "The picotool finding" above for why `picotool load -p` alone is
not enough (it does not relocate; the app must already be linked for that
slot's flash address). For the sketchpad into `slot_a` (id 0) and chrono
into `slot_b` (id 1):

```powershell
cmake -S firmware -B firmware/build-slot-a -G Ninja `
  -Dpicotool_DIR="$env:USERPROFILE\pico\tools\picotool-dist\picotool" `
  -Dpioasm_DIR="$env:USERPROFILE\pico\tools\sdk-tools\pioasm" `
  -DAPP_FLASH_ORIGIN=0x10100000 -DAPP_FLASH_LENGTH=0x700000
cmake --build firmware/build-slot-a
& "$env:USERPROFILE\pico\tools\picotool-dist\picotool\picotool.exe" load firmware/build-slot-a/main.uf2 -p 0 -f -x

cmake -S apps/chrono -B apps/chrono/build-slot-b -G Ninja `
  -Dpicotool_DIR="$env:USERPROFILE\pico\tools\picotool-dist\picotool" `
  -Dpioasm_DIR="$env:USERPROFILE\pico\tools\sdk-tools\pioasm" `
  -DAPP_FLASH_ORIGIN=0x10800000 -DAPP_FLASH_LENGTH=0x700000
cmake --build apps/chrono/build-slot-b
& "$env:USERPROFILE\pico\tools\picotool-dist\picotool\picotool.exe" load apps/chrono/build-slot-b/main.uf2 -p 1 -f -x
```

`-p 0`/`-p 1` here matter for two things, per "The picotool finding": the
size-bound check against the partition, and (with `-x`) the `dParam0` of the
post-load reboot — not for relocation, which is why `APP_FLASH_ORIGIN`/
`APP_FLASH_LENGTH` at build time is what actually has to be correct.
`firmware/CMakeLists.txt` needs the `APP_FLASH_ORIGIN`/`APP_FLASH_LENGTH`
diff below applied before this works for the sketchpad; `apps/chrono/CMakeLists.txt`
already has the equivalent block built in.

`store/app.ts`'s `installApp()`/`buildApp()` automate exactly this per-slot
build-then-load sequence from the catalog; the commands above are the manual
equivalent, for bringing up the two slots the first time or for debugging
outside the store UI.

None of this has been run against real hardware this session (hard
constraint: no `picotool` against the device). Every command above matches
its documented flag syntax (`picotool help partition/load`, checked
offline); the first real install should be watched by hand, same caveat as
everything else in this file that touches the device.

## Integration diffs (not applied — apply and test yourself)

### `firmware/main.c`

There is no `appswitch_init()`/`appswitch_poll()` to call — see "Switching
apps" above for why that pair never existed. Follow `apps/chrono/main.c`'s
`buttons_poll()` instead: read the AXP2101 long-press bit and call
`appswitch_go_other()` directly from wherever `firmware/main.c` already
notices the PWR long press (`buttons_poll()`, per `AGENTS.md`'s "The
buttons" section).

```diff
+#include "appswitch.h"
 ...
 int main(void) {
     DEV_Module_Init();
     QSPI_GPIO_Init(qspi);
     ...
     while (true) {
         ...
         // wherever the existing long-press bit (register 0x49, bit 0x04) is
         // read:
+        if (longPressDetected) {
+            appswitch_go_other();
+        }
         ...
```

### `firmware/CMakeLists.txt`

`appswitch.c` is already in `target_sources` (done, along with adding
`hardware_watchdog` to `target_link_libraries` — appswitch.c now calls
`watchdog_reboot()`, a real linked function, not just header-only `rom_*()`
wrappers). Still not applied: the `APP_FLASH_ORIGIN` / `APP_FLASH_LENGTH`
cache variables `store/app.ts` needs so a per-slot build actually links at
the right address (see the picotool finding — without this, every build
still links at `0x10000000` regardless of what `-p` says at load time).
`apps/chrono/CMakeLists.txt` already has this block, unconditional; the diff
below is the same block for `firmware/CMakeLists.txt`.

```diff
+
+# Per-slot flash placement for the app-store build (see ../store/README.md,
+# "picotool finding"). Only takes effect when the store passes these; a
+# plain `cmake -S firmware -B firmware/build` with neither set still
+# produces the same single-app image documented in AGENTS.md, linked at
+# the SDK default 0x10000000.
+if (APP_FLASH_ORIGIN)
+    if (NOT APP_FLASH_LENGTH)
+        message(FATAL_ERROR "APP_FLASH_ORIGIN set without APP_FLASH_LENGTH")
+    endif()
+    string(CONFIGURE "FLASH(rx) : ORIGIN = ${APP_FLASH_ORIGIN}, LENGTH = ${APP_FLASH_LENGTH}\n"
+           _app_flash_region_ld @ONLY)
+    file(GENERATE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/main/pico_flash_region.ld
+         CONTENT "${_app_flash_region_ld}")
+    pico_add_linker_script_override_path(main ${CMAKE_CURRENT_BINARY_DIR}/main
+                                          FILES pico_flash_region.ld)
+    target_compile_definitions(main PRIVATE "PICO_FLASH_SIZE_BYTES=${APP_FLASH_LENGTH}")
+endif()
```

`pico_add_linker_script_override_path()` is the same mechanism the SDK's
own `pico_override_flash_size()` uses (`pico_standard_link/CMakeLists.txt`)
for `LENGTH`; this just also parameterizes `ORIGIN`, which the SDK doesn't
expose. Needs a real build + `picotool info` check to confirm the resulting
`.uf2`'s addresses actually land where `APP_FLASH_ORIGIN` says — not tested
here (no device access this session).

## Open items / everywhere this was a guess, not a finding

- **SYS_OUT hold duration** (see above): confirmed HIGH-when-pressed, not
  confirmed sustained-vs-pulsed. `appswitch.c`'s long-press timer assumes
  sustained; verify on hardware.
- **`appswitch.c`/`.h`/`store/bootloader/` all build clean now** (`firmware/`,
  `apps/chrono/`, and `store/bootloader/` all compile and link with no
  errors or warnings, confirmed this session), but nothing has run on real
  hardware: no board access this session (hard constraint, see "Hard
  constraints" implied throughout this file — no `picotool` against the
  device). The long-press-to-switch path, the bootloader's
  `chain_image()` calls, and the BOOTSEL fallback are all unexercised on
  silicon.
- **The `chain_image()` work-area size (3264 vs the datasheet's 0xc00/3072)
  is a documented discrepancy, not a resolved one** — see "The bootloader"
  section above. The larger, SDK-header-documented number (3264) is what's
  actually used; if `rom_chain_image()` ever returns
  `BOOTROM_ERROR_INSUFFICIENT_RESOURCES` on real hardware, this is the
  first thing to revisit.
- **`WATCHDOG_SCRATCH0` as the switch-request channel is a static-analysis
  finding, not a hardware-confirmed one.** Every consumer of
  `WATCHDOG_SCRATCH*` in this pico-sdk checkout was traced by reading the
  SDK source (see `firmware/bootreq.h`'s header comment for the full list),
  and none touch `SCRATCH0`, but this has not been cross-checked against
  the actual RP2350 ROM's own internal use of scratch registers (only
  against pico-sdk's C source, which calls into the ROM rather than being
  it). Worth a real reboot-and-inspect pass on hardware before treating it
  as settled.
- **The `APP_FLASH_ORIGIN`/`APP_FLASH_LENGTH` CMake mechanism is unverified
  in practice.** It follows the SDK's own documented pattern for overriding
  `LENGTH`, extended to also override `ORIGIN`, but no build was run against
  it (no device, and this touches `firmware/CMakeLists.txt` which the task
  asked to leave as a diff, not applied). `apps/chrono/CMakeLists.txt`
  already has the equivalent block built in and unconditional, so it *is*
  exercised whenever chrono is built for a slot.
- **`store/app.ts` picotool calls are unrun.** Flag syntax is verified
  against `picotool help <cmd>` (offline, no device involved) and the
  `load`/`save`/`partition create` behaviour against picotool's own source;
  the actual install/uninstall/build flow has not been exercised
  end-to-end.
- **Power-cycle behaviour is now a property of the hardware, not an
  assumption about bootrom preference.** The retired launcher (and the
  first version of `appswitch.c`) relied on
  `REBOOT2_FLAG_REBOOT_TYPE_FLASH_UPDATE` preference persisting, which was
  never independently re-verified. The current design sidesteps that
  question entirely: `WATCHDOG_SCRATCH0` is documented to survive a
  `watchdog_reboot()` but not a power-on reset (it sits outside the
  always-on POWMAN domain), so a real power cycle already clears the switch
  request on its own, and the bootloader also clears it explicitly on every
  read as defence in depth. See `firmware/bootreq.h`. Still worth
  confirming the "does not survive POR" half on real hardware, since it's
  read from a datasheet description rather than measured.
- **Uninstalling the active slot** updates the manifest to point at the
  sibling (if occupied) but cannot change the bootrom's actual boot
  preference through picotool's CLI without a redundant reload; the UI
  surfaces this as a warning rather than pretending it's seamless.
