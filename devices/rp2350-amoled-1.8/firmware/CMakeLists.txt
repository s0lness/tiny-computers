
set(PICO_BOARD pico2 CACHE STRING "Board type")

# This board is not a Pico 2. It carries a W25Q128JVSIQ, 16MB, which
# `picotool info` reports and the schematic confirms; pico2.h defaults
# PICO_FLASH_SIZE_BYTES to 4MB because that is what a real Pico 2 has, and
# nothing here used to correct it.
#
# That mattered the moment firmware/runtime/storage.c started keeping the
# dino's high score in the LAST sector, at 16MB - 4KB. This number is not
# just a capacity: the SDK uses it to size the flash region in the linker
# script and to set up the QMI's address window, so an access four times
# past it is outside what the hardware was told to serve. storage.c knew
# the board had 16MB and hardcoded the offset anyway rather than fixing the
# declaration, which left the read legal in intent and undefined in fact.
#
# Found after the first firmware that actually contained storage_init()
# froze on a white screen at boot: gfx_init() had filled the framebuffer
# white and pushed it, so the hang was after that and before the watchdog
# was armed (a hang after arming would reboot-loop, not sit still), which
# leaves storage_init() and the core1 launch, and storage_init() had never
# once executed on silicon.
# A plain integer, not an expression: this reaches the compiler as -D, and
# `(16 * 1024 * 1024)` there is "macro names must be identifiers".
set(PICO_FLASH_SIZE_BYTES 16777216 CACHE STRING "This board's real flash size, 16MB")

cmake_minimum_required(VERSION 3.27)

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

project(main C CXX ASM)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

add_executable(main)

# CORE1 RUNS ENTIRELY FROM RAM. CORE0 RUNS FROM FLASH BY XIP. This is the
# narrower form of the fix for the core1 freeze diagnosed in decision 0004:
# bootbtn.c samples the BOOT button by floating the flash chip select for
# ~100us at a time (read_cs_low()), which its own comment understands is
# fatal to any core fetching from flash at that moment - and it protects
# only the calling core. Core0 is covered by construction (the function is
# __no_inline_not_in_flash_func and runs with its interrupts off, and it is
# core0 doing the borrowing, so it can never race itself). Core1 is the one
# genuinely at risk, and everything it can reach - by direct branch, or by
# an escape site resolved and pinned down in
# tools/invariants/rules/rp2350-amoled-1.8.ts - is annotated
# `__not_in_flash_func`/`__not_in_flash()` (this firmware's own code:
# sensors.c, tilt.c) or pulled into RAM by the SDK's own placement (pico-sdk
# already excludes libgcc/libm/newlib's mem* objects from FLASH by default,
# and separately marks the exact functions that run WHILE flash itself is
# busy, like pico_multicore's flash-lockout IRQ victim, `__not_in_flash_func`
# for the identical reason). tools/invariants rule 1 is what makes this
# trustworthy rather than hopeful: it enforces "no executable byte at a
# flash VMA that core1 can reach" against the SAME reachability graph rule 0
# already builds and gates (every indirect or handler-installing call site
# on that path must be resolved or annotated), so a future refactor that
# adds one innocent call to core1's path fails the build instead of quietly
# reopening the hole - which is exactly how this bug returned the first
# time, per decision 0005's account of the deleted pre-runtime main.c.
#
# Why not keep copy_to_ram: decision 0016 (the framebuffer malloc) found an
# 11-app build 46,380 bytes short of the panel framebuffer's own allocation
# with copy_to_ram's whole-image RAM cost included - the board cannot be
# flashed until that is solved, and the framebuffer (329,728 bytes) cannot
# shrink. Core1's OWN reachable code is a few hundred functions smaller than
# the whole image (see docs/decisions/0006's own count: 30-ish functions
# reached from core1's roots, out of 500+ in the image), so RAM-pinning only
# what core1 can reach recovers most of copy_to_ram's SRAM cost while
# keeping its safety property for the one core that actually needs it.
#
# pico_set_binary_type is deliberately NOT called here: the SDK's "default"
# binary type (flash XIP, `__not_in_flash_func`-annotated code still placed
# in RAM by the linker script's own `.time_critical*` handling) is what this
# needs, and that is PICO_DEFAULT_BINARY_TYPE's own default for this board.

pico_generate_pio_header(main ${CMAKE_CURRENT_LIST_DIR}/lib/QSPI_PIO/qspi.pio
                        OUTPUT_DIR ${CMAKE_CURRENT_LIST_DIR}/lib/QSPI_PIO)

# I2S TX for the ES8311 speaker codec (firmware/runtime/sound.c). Same
# pico_generate_pio_header pattern as qspi.pio above, output alongside the
# .pio source rather than into lib/QSPI_PIO - this is the sound service's
# own PIO program, not the display's.
pico_generate_pio_header(main ${CMAKE_CURRENT_LIST_DIR}/runtime/sound_i2s.pio
                        OUTPUT_DIR ${CMAKE_CURRENT_LIST_DIR}/runtime)

# One binary holds every app, and switching between them is a function call
# rather than a reboot. See docs/decisions/0002-runtime-architecture.md: a
# reboot costs 182ms of which 150ms is the sleep-out the SH8601 physically
# requires, so no amount of tuning lets a reboot compete with a repaint.
#
# Adding an app is one file here plus one entry in g_apps[] in runtime.c.
target_sources(main PRIVATE
    # The runtime: owns the screen, the input and the main loop.
    # runtime_core.c is the portable half (docs/decisions/0003): the arena,
    # the app table, switching, and per-frame dispatch. It compiles for the
    # board here and for wasm32-freestanding in emulator/wasm/build.ts, so
    # do not add anything to it that depends on pico-sdk or hardware/*.
    runtime/runtime_core.c
    runtime/runtime.c
    runtime/gfx.c
    runtime/sensors.c
    # tilt.c is the orientation signal, and it is portable for the same
    # reason sound_synth.c below is: it also compiles into emu.wasm
    # unmodified, so the emulator's gravity is filtered by this firmware's
    # own filter, with the same time constant and the same axis mapping,
    # rather than by a browser-side second implementation that would drift.
    # sensors.c (core1) feeds it; nothing here touches i2c1.
    runtime/tilt.c
    # sound_synth.c is the portable half (no PIO/DMA/i2c/pico-sdk
    # dependency): it also compiles into emu.wasm unmodified (emulator/wasm/
    # build.ts), so the emulator's WebAudio preview is genuinely this
    # firmware's own synthesis - see sound.c's header comment. sound.c is the
    # board half: the ES8311 bring-up and the PIO+DMA I2S path.
    runtime/sound_synth.c
    runtime/sound.c
    # storage: the one place this firmware ever calls flash_range_erase or
    # flash_range_program - docs/decisions/0011. Board-only (real flash);
    # the emulator's stand-in lives in emulator/wasm/emu_shim.c instead, so
    # this file is NOT in emulator/wasm/build.ts's source list.
    runtime/storage.c

    # The apps. None of them owns any hardware.
    apps/menu.c
    apps/sketch.c
    apps/chrono.c
    apps/timer.c
    apps/four.c
    # The bubble level. Reads no hardware directly: like every other app it
    # takes orientation through app_frame_t.tilt (runtime/tilt.c).
    apps/level.c
    # The clock. In g_apps[] like every other app; it too reads orientation
    # through app_frame_t.tilt rather than owning any hardware.
    apps/clock.c
    # Morpion (noughts and crosses). Owns no hardware either.
    apps/morpion.c
    # Four games merged 2026-08-15, each appended in the order it arrived.
    # None owns any hardware: dino reads plain touch (tap to jump) plus the
    # storage service above for its high score; bowling reads a finger-flick
    # drag/release; tiltball and breakout read app_frame_t.tilt like level,
    # and tiltball also plays through sound.h on capture.
    apps/dino.c
    apps/bowling.c
    apps/tiltball.c
    apps/breakout.c
    # Multiplication-tables practice (6/7/8 tables), the twelfth app -
    # decision 0013's own ceiling. Owns no hardware; reads plain touch
    # (an iOS-keyboard-style numpad with a loupe, see the file's own
    # header) like four.c/morpion.c do.
    apps/tables.c
    apps/digits.c
    # Round-silhouette helpers (ring/wedge row math), used by menu.c's
    # stopwatch icon. See shapes.h for why it is not also wired into
    # timer.c yet.
    apps/shapes.c

    # BOOT is not a GPIO on this part: reading it borrows the flash chip
    # select with interrupts off, so it lives in its own file with the
    # explanation of what that costs.
    bootbtn.c

    # devlink: the USB screenshot / touch-injection / app-navigation link an
    # agent uses to see and drive the board without a human. Its old hooks
    # called straight into the sketchpad's own inject_down/move/up
    # functions, which stopped existing when touch moved onto core1's
    # lock-free queue; runtime.c now wires it to sensors_inject_touch() (a
    # second, core0-owned ring merged into that same queue by timestamp, so
    # devlink never becomes a second producer of core1's ring) instead, so
    # every app gets injected touches for free, not just the sketchpad. See
    # devlink.h, runtime.c's "devlink wiring" section and
    # tools/README-devlink.md for the full story.
    devlink.c
)

# appswitch.c and bootreq.h are deliberately NOT built any more. They existed
# to reboot into the other flash slot, which is what this architecture
# replaced; leaving a reboot-into-another-image path compiled into a binary
# that no longer has a second image to go to is a hazard, not dead weight.
# The partition table in store/ stays, repurposed as a golden fallback for
# crash recovery, and that path will bring its own entry point when it lands.

add_subdirectory(./lib/Config)
add_subdirectory(./lib/QSPI_PIO)
add_subdirectory(./lib/AMOLED)
add_subdirectory(./lib/Touch)
add_subdirectory(./lib/QMI8658)

include_directories(./lib/Config)
include_directories(./lib/QSPI_PIO)
include_directories(./lib/AMOLED)
include_directories(./lib/Touch)
include_directories(./lib/QMI8658)
include_directories(./runtime)
include_directories(./apps)
include_directories(.)

target_link_libraries(main PRIVATE
    Config
    QSPI_PIO
    AMOLED
    Touch
    QMI8658
    pico_stdlib
    hardware_pio
    hardware_pwm
    hardware_dma
    hardware_spi
    hardware_i2c
    hardware_gpio
    hardware_adc
    # The runtime feeds a hardware watchdog once per loop, so a hung app
    # reboots on its own. The manual recovery is a ten-second button ritual
    # that the owner knows and the child does not, and the owner will not be
    # in the room.
    hardware_watchdog
    # sensors.c runs touch, the IMU and the PMIC on core1 so core0 never
    # blocks on i2c1: the touch read alone was about 695us, roughly 98
    # percent of frame time.
    pico_multicore
    # __dmb(), for the lock-free single-producer/single-consumer touch queue
    # between the cores.
    hardware_sync
    # core1 fault capture (sensors.c's core1_install_fault_handlers()).
    # Originally added while investigating core1 dying about a second after
    # boot; kept permanently (both as a real safety net and because the
    # invariant checker's reachability graph hardcodes core1_fault_handler
    # as a root - see tools/invariants/rules/rp2350-amoled-1.8.ts).
    hardware_exception
    # storage.c's flash_range_erase()/flash_range_program() (the dino high
    # score - docs/decisions/0011).
    hardware_flash
    # flash_safe_execute()/flash_safe_execute_core_init() - the multicore
    # lockout storage.c parks core1 with for the duration of a save. Not
    # linked before dino.c; decision 0011 called this out explicitly as
    # something a future flash-writing app would have to add.
    pico_flash
)

pico_enable_stdio_usb(main 1)
pico_enable_stdio_uart(main 1)

# The board must never look dead just because a host opened the USB serial
# port and is not reading it: pico-sdk's stdio_usb driver (stdio_usb.c,
# stdio_usb_out_chars()) already bounds any ONE blocked write to
# PICO_STDIO_USB_STDOUT_TIMEOUT_US before giving up and dropping it, default
# 500ms. That is fine for a single stall nobody notices, but this board also
# has a 4-second watchdog (runtime.c's RUNTIME_WATCHDOG_MS) and a once-a-
# second profiler tick, so 500ms is a visible stutter for no benefit: a host
# that IS draining sees a USB Full-Speed round trip in microseconds, not
# milliseconds, so shortening this changes nothing for the case that works
# today and removes the worst case for the one that does not.
target_compile_definitions(main PRIVATE PICO_STDIO_USB_STDOUT_TIMEOUT_US=2000)

# Touch sensitivity sweep knob (firmware/runtime/sensors.c's
# FT3168_TOUCH_THRESHOLD comment: FocalTech FT5x06-family register 0x80,
# ID_G_THGROUP, lower = more sensitive). Optional and off by default -
# sensors.c's own #ifndef supplies the built-in starting value - so a
# candidate can be tried with no source edit:
#   cmake -S firmware -B firmware/build -DFT3168_TOUCH_THRESHOLD=20
if(DEFINED FT3168_TOUCH_THRESHOLD)
    target_compile_definitions(main PRIVATE FT3168_TOUCH_THRESHOLD=${FT3168_TOUCH_THRESHOLD})
endif()

# Touch pipeline diagnostics (sensors.h's TOUCH_POLL_SELFTEST comment).
# Optional, off by default. Previously this only had one way to turn on:
# hand-edit sensors.h's #define and remember to revert it before shipping -
# exactly the kind of change a diff review can miss. A cmake flag removes
# that risk entirely for the default build:
#   cmake -S firmware -B firmware/build-diag -DTOUCH_POLL_SELFTEST=1
if(DEFINED TOUCH_POLL_SELFTEST)
    target_compile_definitions(main PRIVATE TOUCH_POLL_SELFTEST=${TOUCH_POLL_SELFTEST})
endif()

# Sketchpad live tuning over devlink (firmware/apps/sketch.c's
# SKETCH_LIVE_TUNE comment, firmware/devlink.c's TUNE command). Optional,
# off by default, same reasoning and same pattern as TOUCH_POLL_SELFTEST
# just above: a shipped build carries the tunables as plain #define
# constants with no runtime variable and no way to leave a knob in a bad
# position, and turning this on is a build flag, not a source edit to
# remember to revert:
#   cmake -S firmware -B firmware/build-tune -DSKETCH_LIVE_TUNE=1
if(DEFINED SKETCH_LIVE_TUNE)
    target_compile_definitions(main PRIVATE SKETCH_LIVE_TUNE=${SKETCH_LIVE_TUNE})
endif()

# firmware/linker_overrides/default_text_excludes.incl: extends pico-sdk's
# own list of what does NOT go to FLASH under the default binary type (the
# mechanism that already RAM-places libgcc/libc/libm) to also cover the
# pico-sdk components and the one vendor file (firmware/lib/Touch/FT3168.c)
# on core1's reachable path - see that file's own header for the full
# reasoning, the complete list, and why this is a linker-script override
# rather than a second `-T` script.
#
# A same-named `.data` block appended via a second `-T` script (repeating
# the output section name, the ordinary way to extend an SDK linker script)
# was tried first and does NOT work here: GNU ld assigns each input section
# to the FIRST output section rule that matches it, in the order the
# combined script's SECTIONS commands are written, and pico-sdk's own
# FLASH `.text` output section (with its own broad `*(.text*)` wildcard)
# is written before any later, appended `.data` rule could ever run - so it
# always wins the race regardless of `-T` ordering on the link command
# line. pico_add_linker_script_override_path() sidesteps this entirely: it
# REPLACES default_text_excludes.incl outright (an official SDK extension
# point - see memmap_default.ld's own header comment), so the exclusion
# happens inside the SAME rule that already wins the race for libgcc/libc/
# libm, rather than trying to out-race it from outside.
pico_add_linker_script_override_path(main ${CMAKE_CURRENT_LIST_DIR}/linker_overrides GLOB_FILES)

pico_add_extra_outputs(main)

# Fail the build on a violation of tools/invariants (docs/decisions/0006):
# static checks over the just-linked .elf/.map that catch regressions of
# known hazards (decision 0005's flash/core1 collision, decision 0004's
# stdio-lock-on-core1 finding) at build time instead of on hardware, days
# later. Runs after pico_add_extra_outputs() above, which is what actually
# produces the .map file this reads.
find_program(BUN_EXECUTABLE bun)
if(BUN_EXECUTABLE)
    add_custom_command(TARGET main POST_BUILD
        COMMAND ${BUN_EXECUTABLE} run
                ${CMAKE_CURRENT_LIST_DIR}/../tools/invariants/runner.ts
                $<TARGET_FILE:main>
                $<TARGET_FILE:main>.map
        COMMENT "Checking firmware invariants (tools/invariants, docs/decisions/0006)"
        VERBATIM
    )
else()
    message(WARNING "bun not found on PATH: tools/invariants did not run - see docs/decisions/0006")
endif()

# The build's own identity, so a host can refuse to compare its tree
# against a board running something else. A differential harness once
# diffed three apps on the board against four in the tree and called the
# result a 28% rendering divergence; the board was simply older, and
# nothing it could say would have shown that.
#
# tools/gate/fingerprint.ts hashes every firmware source and writes
# build_fingerprint.h here; devlink.c includes it (guarded by
# __has_include, so a build without bun still compiles and honestly
# reports "unknown") and answers the FP command with it. The host computes
# the SAME hash from its own checkout - one definition of the fingerprint,
# not a C one and a TypeScript one that agree on the day they are written.
#
# A CUSTOM TARGET, NOT `PRE_BUILD`, and the difference is not cosmetic.
# `add_custom_command(TARGET main PRE_BUILD)` is a Visual Studio-only
# concept: under Ninja and Makefiles CMake silently downgrades it to
# PRE_LINK, which runs AFTER every .c has already been compiled. Wired that
# way, the first build compiled devlink.c with no header to include and
# produced exactly the failure this mechanism exists to prevent - an image
# that confidently answered "unknown" while a correct fingerprint sat in
# the build directory beside it. Verified by looking for the string in the
# linked .elf rather than by reading the CMake docs, which is the only
# reason it was caught.
#
# `add_custom_target(... ALL)` + `add_dependencies` orders it strictly
# before any of main's objects, and it runs on EVERY build (a fingerprint
# computed once at configure time is stale the moment anyone edits a .c,
# which is the failure it exists to prevent). The script rewrites the
# header only when the value actually changes, so the ordinary case - build
# again, nothing edited - does not recompile devlink.c.
if(BUN_EXECUTABLE)
    set(FINGERPRINT_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
    add_custom_target(build_fingerprint ALL
        COMMAND ${BUN_EXECUTABLE} run
                ${CMAKE_CURRENT_LIST_DIR}/../tools/gate/fingerprint.ts
                --header ${FINGERPRINT_DIR}/build_fingerprint.h
        BYPRODUCTS ${FINGERPRINT_DIR}/build_fingerprint.h
        COMMENT "Stamping the build fingerprint (tools/gate/fingerprint.ts)"
        VERBATIM
    )
    add_dependencies(main build_fingerprint)
    target_include_directories(main PRIVATE ${FINGERPRINT_DIR})
else()
    message(WARNING "bun not found on PATH: this image will report FP unknown - see tools/gate/README.md")
endif()
