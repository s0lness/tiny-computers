# Standalone build for rtcprobe (see rtcprobe.c's header and
# docs/decisions/0011). Deliberately its own project rather than a second
# target inside firmware/CMakeLists.txt: a diagnostic must not be able to
# change a byte of the image the owner's device runs, and it must not make
# the product build depend on pico_flash.
#
#   cmake -S firmware/probe -B firmware/probe/build -G Ninja `
#     -Dpicotool_DIR="$env:USERPROFILE\pico\tools\picotool-dist\picotool" `
#     -Dpioasm_DIR="$env:USERPROFILE\pico\tools\sdk-tools\pioasm"
#   cmake --build firmware/probe/build
#
# produces firmware/probe/build/rtcprobe.uf2. Section D (the flash write)
# needs -DPROBE_WRITE_FLASH=1 as well, and is off otherwise.
set(PICO_BOARD pico2 CACHE STRING "Board type")
cmake_minimum_required(VERSION 3.27)

include(${CMAKE_CURRENT_LIST_DIR}/../pico_sdk_import.cmake)

project(rtcprobe C CXX ASM)

pico_sdk_init()

add_executable(rtcprobe rtcprobe.c)

# Same binary type as the product image (firmware/CMakeLists.txt, decision
# 0004): nothing executes from flash. Section D's numbers are only
# transferable to the real firmware if the probe runs under the same rule.
pico_set_binary_type(rtcprobe copy_to_ram)

if(DEFINED PROBE_WRITE_FLASH)
    target_compile_definitions(rtcprobe PRIVATE PROBE_WRITE_FLASH=${PROBE_WRITE_FLASH})
endif()

target_link_libraries(rtcprobe PRIVATE
    pico_stdlib
    hardware_i2c
    hardware_flash
    hardware_sync
    pico_multicore
    # pico_flash: flash_safe_execute() and flash_safe_execute_core_init().
    # The product firmware does NOT link this today; that it would have to
    # is part of what decision 0011 is answering.
    pico_flash
)

pico_enable_stdio_usb(rtcprobe 1)
pico_enable_stdio_uart(rtcprobe 1)

pico_add_extra_outputs(rtcprobe)
