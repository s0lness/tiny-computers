
set(PICO_BOARD pico2 CACHE STRING "Board type")
cmake_minimum_required(VERSION 3.27)

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

project(main C CXX ASM)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

add_executable(main)

# THE WHOLE BINARY EXECUTES FROM SRAM, NOT XIP. This is the fix for the
# core1 freeze diagnosed in docs/decisions/0004: bootbtn.c samples the BOOT
# button by floating the flash chip select for ~100us at a time
# (read_cs_low()), which its own comment understands is fatal to any core
# fetching from flash at that moment - and it protects only the calling
# core. Core0 is covered (the function is __no_inline_not_in_flash_func and
# runs with its interrupts off); core1, which executed every sensor loop
# from flash via XIP, was not, so any XIP-cache miss on core1 during a
# borrow fetched garbage instructions through a deselected chip. The
# deleted pre-runtime main.c had already hit and documented this ("a
# periodic poll was in here briefly and the app hung with a white screen
# and no input"), and solved it by not polling; the runtime refactor
# deleted that file and reintroduced the periodic poll for bootClicked.
#
# copy_to_ram is the total form of the fix, chosen over per-function
# __not_in_flash_func annotation deliberately: annotating core1's call
# graph means auditing every callee forever (time_us_64, sleep_ms, libm's
# sqrtf, libgcc's 64-bit division helpers, the multicore launch trampoline,
# the vendor FT3168 reset - all were on core1's path and all live in
# flash), and one future refactor adding one innocent call silently
# reopens the hole, which is precisely how this bug returned the first
# time. With the entire image copied to SRAM at boot there is no XIP
# execution on either core, ever, so the chip-select borrow is safe by
# construction and stays safe. Cost, measured before the change: 65KB
# .text + 4KB .rodata against roughly 355KB of free SRAM, and a few ms of
# boot-time copy.
pico_set_binary_type(main copy_to_ram)

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
    # sound_synth.c is the portable half (no PIO/DMA/i2c/pico-sdk
    # dependency): it also compiles into emu.wasm unmodified (emulator/wasm/
    # build.ts), so the emulator's WebAudio preview is genuinely this
    # firmware's own synthesis - see sound.c's header comment. sound.c is the
    # board half: the ES8311 bring-up and the PIO+DMA I2S path.
    runtime/sound_synth.c
    runtime/sound.c

    # The apps. None of them owns any hardware.
    apps/menu.c
    apps/sketch.c
    apps/chrono.c
    apps/timer.c
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
    # TEMPORARY: core1 fault capture (sensors.c's core1_install_fault_
    # handlers()), added while investigating core1 dying about a second
    # after boot. Remove alongside that code once the investigation closes.
    hardware_exception
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

pico_add_extra_outputs(main)
