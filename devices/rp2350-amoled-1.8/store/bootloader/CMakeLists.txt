# store/bootloader: the tiny first-stage image that decides which app slot
# to run and calls chain_image() into it. See ../README.md ("Switching apps"
# and "The bootloader") for why this exists instead of a flash-update reboot,
# and ../../firmware/bootreq.h for the scratch-register protocol it shares
# with ../../firmware/appswitch.c.
#
# Modelled on ../../apps/chrono/CMakeLists.txt, stripped further: this image
# pulls in none of firmware/lib/ (no display, no touch, no IMU) because its
# only job is to run for well under a millisecond and get out of the way.
# Only ../../firmware/bootreq.h is shared with the rest of the tree.
#
# Unlike apps/chrono and firmware/, this is NOT built per-slot. It is not a
# partition at all: it links at the SDK default ORIGIN 0x10000000, which
# lands it in the unpartitioned space below slot_a's 0x00100000 start (see
# ../partitions.json), and gets embedded into the partition table UF2 by
# `picotool partition create <json> <out> <this-elf>` (see ../README.md).

set(PICO_BOARD pico2 CACHE STRING "Board type")
cmake_minimum_required(VERSION 3.27)

include(pico_sdk_import.cmake)

project(bootloader C CXX ASM)

pico_sdk_init()

add_executable(bootloader)

target_sources(bootloader PRIVATE
    main.c
)

# Only for ../../firmware/bootreq.h — the switch-request protocol. Nothing
# else from firmware/ is pulled in (not appswitch.c/.h, not main.c).
target_include_directories(bootloader PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../firmware)

target_link_libraries(bootloader PRIVATE
    pico_stdlib
)

# USB CDC stdio, for the fallback/error path only — see main.c's top comment
# for exactly what this costs on the normal (fast, silent) path: nothing
# measurable, because stdio_init_all() does not block waiting for a host to
# enumerate.
pico_enable_stdio_usb(bootloader 1)
pico_enable_stdio_uart(bootloader 1)

pico_add_extra_outputs(bootloader)

# Hard guarantee, not just a policy of "keep it tiny": constrain this image's
# own linked FLASH length to exactly the 1MB unpartitioned region in front of
# slot_a (../partitions.json: slot_a.start = 0x00100000). If this bootloader
# ever grew past that, this turns it into a link error instead of a silent
# flash overlap into slot_a's territory.
pico_override_flash_size(bootloader 0x00100000)
