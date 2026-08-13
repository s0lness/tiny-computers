
set(PICO_BOARD pico2 CACHE STRING "Board type")
cmake_minimum_required(VERSION 3.27)

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

project(main C CXX ASM)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

add_executable(main)

pico_generate_pio_header(main ${CMAKE_CURRENT_LIST_DIR}/lib/QSPI_PIO/qspi.pio
                        OUTPUT_DIR ${CMAKE_CURRENT_LIST_DIR}/lib/QSPI_PIO)

# One binary holds every app, and switching between them is a function call
# rather than a reboot. See docs/decisions/0002-runtime-architecture.md: a
# reboot costs 182ms of which 150ms is the sleep-out the SH8601 physically
# requires, so no amount of tuning lets a reboot compete with a repaint.
#
# Adding an app is one file here plus one entry in g_apps[] in runtime.c.
target_sources(main PRIVATE
    # The runtime: owns the screen, the input and the main loop.
    runtime/runtime.c
    runtime/gfx.c
    runtime/sensors.c

    # The apps. None of them owns any hardware.
    apps/menu.c
    apps/sketch.c
    apps/chrono.c
    apps/timer.c
    apps/digits.c

    # BOOT is not a GPIO on this part: reading it borrows the flash chip
    # select with interrupts off, so it lives in its own file with the
    # explanation of what that costs.
    bootbtn.c
)

# devlink.c (the USB screenshot and touch-injection link) is out of the build
# for now. Its hooks were the sketchpad's own inject_down/move/up functions,
# which no longer exist at that layer: touch now arrives from core1 through a
# queue, so injection has to become a synthetic producer on that queue rather
# than a direct call into one app. That is worth doing, since screenshots are
# how this gets verified without a human photographing the screen, but it is
# not worth doing in the same change as the runtime itself.

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
)

pico_enable_stdio_usb(main 1)
pico_enable_stdio_uart(main 1)

pico_add_extra_outputs(main)
