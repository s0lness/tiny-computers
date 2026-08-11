# chrono: second app, modelled on ../../firmware/CMakeLists.txt.
#
# Reuses the shared Waveshare drivers from ../../firmware/lib rather than
# copying them, so the hand-made patches documented in ../../README.md
# (the DisplayWindows off-by-one/truncation fixes) stay a single source of
# truth. Only Config, QSPI_PIO and AMOLED are needed: this app has no touch
# and no IMU, so Touch/QMI8658 are deliberately not pulled in.
#
# IMPORTANT: every add_subdirectory() below passes an explicit binary_dir
# under this app's own build tree. Without it, CMake computes a binary
# directory that mirrors the relative source path (../../firmware/lib/...),
# which would land build output *inside* firmware/lib itself — exactly the
# shared, hand-patched directory this app must never write into.

set(PICO_BOARD pico2 CACHE STRING "Board type")
cmake_minimum_required(VERSION 3.27)

include(pico_sdk_import.cmake)

project(main C CXX ASM)

pico_sdk_init()

add_executable(main)

set(WS_LIB ${CMAKE_CURRENT_LIST_DIR}/../../firmware/lib)

target_sources(main PRIVATE
    main.c
    ${CMAKE_CURRENT_LIST_DIR}/../../firmware/appswitch.c
    ${CMAKE_CURRENT_LIST_DIR}/../../firmware/bootbtn.c
    ${CMAKE_CURRENT_LIST_DIR}/../../firmware/menu.c
)

# Shared app-level modules live with the sketchpad rather than being copied.
target_include_directories(main PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../firmware)

# Binary dirs are relative to CMAKE_CURRENT_BINARY_DIR (this app's own
# build/ directory), so all generated object files land under
# apps/chrono/build*/, never under firmware/lib.
add_subdirectory(${WS_LIB}/Config    lib_config)
add_subdirectory(${WS_LIB}/QSPI_PIO  lib_qspi_pio)
add_subdirectory(${WS_LIB}/AMOLED    lib_amoled)

include_directories(${WS_LIB}/Config)
include_directories(${WS_LIB}/QSPI_PIO)
include_directories(${WS_LIB}/AMOLED)

target_link_libraries(main PRIVATE
    Config
    QSPI_PIO
    AMOLED
    pico_stdlib
    hardware_pio
    hardware_pwm
    hardware_dma
    hardware_spi
    hardware_i2c
    hardware_gpio
    hardware_adc
    # appswitch.c (see ../../firmware/appswitch.c) calls watchdog_reboot(), a
    # real compiled function in hardware_watchdog/watchdog.c, not just the
    # header-only rom_*() bootrom wrappers it used before. pico_stdlib does
    # not pull this in on its own.
    hardware_watchdog
)

pico_enable_stdio_usb(main 1)
pico_enable_stdio_uart(main 1)

pico_add_extra_outputs(main)

# Per-slot flash placement for the app-store build (see ../../store/README.md,
# "picotool finding": picotool load -p does NOT relocate a file, so each
# slot needs its own build linked at that slot's real flash address).
# Only takes effect when store/app.ts passes these; a plain
# `cmake -S apps/chrono -B apps/chrono/build` with neither set still
# produces a single-app image linked at the SDK default 0x10000000, exactly
# like a plain build of firmware/ does.
if (APP_FLASH_ORIGIN)
    if (NOT APP_FLASH_LENGTH)
        message(FATAL_ERROR "APP_FLASH_ORIGIN set without APP_FLASH_LENGTH")
    endif()
    string(CONFIGURE "FLASH(rx) : ORIGIN = ${APP_FLASH_ORIGIN}, LENGTH = ${APP_FLASH_LENGTH}\n"
           _app_flash_region_ld @ONLY)
    file(GENERATE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/main/pico_flash_region.ld
         CONTENT "${_app_flash_region_ld}")
    pico_add_linker_script_override_path(main ${CMAKE_CURRENT_BINARY_DIR}/main
                                          FILES pico_flash_region.ld)
    target_compile_definitions(main PRIVATE "PICO_FLASH_SIZE_BYTES=${APP_FLASH_LENGTH}")
endif()
