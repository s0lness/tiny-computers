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

Both are replaced by: two ~7MB app slots, no on-device menu, and a shared
`appswitch.c` helper (`firmware/appswitch.h/.c`, new files) that every app
links, watching PWR for a long press and rebooting straight into the other
slot.

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

New files, **not wired into `firmware/main.c`** (see the integration diff
below — that edit is left for you to apply and test). Every app is meant to
call `appswitch_init()` once at startup and `appswitch_poll()` once per main
loop iteration.

Design: there is no on-device launcher at all. On a PWR (`SYS_OUT`, GPIO18)
press held longer than ~1.5s, the running app reboots directly into
whichever slot it is **not** currently running from, via
`rom_reboot(REBOOT2_FLAG_REBOOT_TYPE_FLASH_UPDATE | REBOOT2_FLAG_NO_RETURN_ON_SUCCESS, ...)`
— the same bootrom call the retired launcher used, just aimed at "the other
slot" instead of "the tapped row".

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

## Integration diffs (not applied — apply and test yourself)

### `firmware/main.c`

```diff
+#include "appswitch.h"
 ...
 int main(void) {
     DEV_Module_Init();
+    appswitch_init();
     QSPI_GPIO_Init(qspi);
     ...
     while (true) {
+        appswitch_poll();
         int dMinX = PANEL_W, dMinY = PANEL_H, dMaxX = -1, dMaxY = -1;
         ...
```

Anywhere in the loop body is fine as long as it runs every iteration;
`appswitch_poll()` is non-blocking except for the one-time `sleep_ms(30)`
immediately before a reboot that doesn't return.

### `firmware/CMakeLists.txt`

Two changes: add `appswitch.c` to the sources, and accept the
`APP_FLASH_ORIGIN` / `APP_FLASH_LENGTH` cache variables `store/app.ts`
passes so a per-slot build actually links at the right address (see the
picotool finding — without this, every build still links at `0x10000000`
regardless of what `-p` says at load time).

```diff
 target_sources(main PRIVATE
     main.c
+    appswitch.c
 )
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
- **`appswitch.c`/`.h` are untested**: no build was attempted (would need
  wiring into `firmware/main.c`, which is explicitly not this task's job).
- **The `APP_FLASH_ORIGIN`/`APP_FLASH_LENGTH` CMake mechanism is unverified
  in practice.** It follows the SDK's own documented pattern for overriding
  `LENGTH`, extended to also override `ORIGIN`, but no build was run against
  it (no device, and this touches `firmware/CMakeLists.txt` which the task
  asked to leave as a diff, not applied).
- **`store/app.ts` picotool calls are unrun.** Flag syntax is verified
  against `picotool help <cmd>` (offline, no device involved) and the
  `load`/`save` behaviour against picotool's own source; the actual
  install/uninstall/build flow has not been exercised end-to-end.
- **Bootrom preference persistence across a normal power-cycle** (not just
  the immediate reboot) is assumed, matching how the retired launcher used
  `REBOOT2_FLAG_REBOOT_TYPE_FLASH_UPDATE`, but not independently
  re-verified here.
- **Uninstalling the active slot** updates the manifest to point at the
  sibling (if occupied) but cannot change the bootrom's actual boot
  preference through picotool's CLI without a redundant reload; the UI
  surfaces this as a warning rather than pretending it's seamless.
