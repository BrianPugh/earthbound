cmake_minimum_required(VERSION 3.13)

# Pull in Pico SDK (must be before project())
include(pico_sdk_import.cmake)

project(earthbound_pico C CXX ASM)
set(CMAKE_C_STANDARD 11)

# 16 MB flash chip, minus 8 KB reserved for save data at the end.
# This limits the linker's FLASH region so firmware can't silently overlap saves.
# The full 16 MB value is passed as a C macro (below) so the SDK's flash_range_*
# hard_assert accepts offsets into the save area.
math(EXPR PICO_FLASH_SIZE_BYTES "16 * 1024 * 1024 - 2 * 4096")
set(PICO_FLASH_SIZE_BYTES "${PICO_FLASH_SIZE_BYTES}")

# Disable heap — game doesn't use malloc. Saves 2048 bytes for USB stdio BSS.
add_compile_definitions(PICO_HEAP_SIZE=0)

# Run RP2040 at 250 MHz (SDK raises regulator voltage automatically).
# clk_peri tracks clk_sys at boot → SPI can reach 62.5 MHz (250/4).
# PLL: 12 MHz * 125 = 1500 MHz VCO / (6 * 1) = 250 MHz
add_compile_definitions(
    SYS_CLK_MHZ=250
    PLL_SYS_VCO_FREQ_HZ=1500000000
    PLL_SYS_POSTDIV1=6
    PLL_SYS_POSTDIV2=1
)

pico_sdk_init()


# 240x240 display
set(EB_VIEWPORT_WIDTH 240 CACHE STRING "Viewport width in pixels")
set(EB_VIEWPORT_HEIGHT 240 CACHE STRING "Viewport height in pixels")

# RP2040 has only 264KB RAM; game BSS alone is ~250KB, leaving no room
# for the SPC700/DSP emulator's 64KB APU RAM allocation.
set(EB_ENABLE_AUDIO OFF CACHE BOOL "" FORCE)

# Build game library from src/
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../../src
                 ${CMAKE_CURRENT_BINARY_DIR}/game_lib)

# Optimize game library: -Os globally to reduce code size (fits more in 16KB XIP
# cache).  PPU rendering overrides to -O3 via EB_PPU_FORCE_SPEED_OPT pragma.
target_compile_options(earthbound_game PRIVATE -Os)

# Embedded build: LOG_WARN/LOG_TRACE compile out (stdio is disabled anyway),
# FATAL traps, and the platform_savestate_* hooks come from the shared fail-safe
# stub (src/platform/savestate_backend_stub.c) until a flash backend exists.
# PUBLIC so main.c and the drivers see the same logging/backend contract.
target_compile_definitions(earthbound_game PUBLIC EB_EMBEDDED=1)

# Place hot PPU rendering functions in SRAM (avoids XIP cache contention),
# and force -O3 for the PPU rendering file via pragma.
target_compile_definitions(earthbound_game PRIVATE
    EB_PPU_RAM_SECTION=".time_critical"
    EB_PPU_FORCE_SPEED_OPT=1
    EB_MAX_FRAME_SKIP=2
)

# Shared drivers (display, HAL, generic RP2040 platform)
set(DRIVERS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../drivers)

# Board-specific platform files (e.g. input mapping)
file(GLOB BOARD_PLATFORM_SOURCES "platform/*.c")

# Platform executable
add_executable(earthbound_pico
    main.c
    ${BOARD_PLATFORM_SOURCES}
    # Shared RP2040 platform implementations
    ${DRIVERS_DIR}/platform/rp2040/timer.c
    ${DRIVERS_DIR}/platform/rp2040/save.c
    ${DRIVERS_DIR}/platform/rp2040/debug.c
    ${DRIVERS_DIR}/platform/rp2040/audio.c
    ${DRIVERS_DIR}/platform/rp2040/video.c
    ${DRIVERS_DIR}/platform/rp2040/worker.c
    # Shared display/HAL drivers
    ${DRIVERS_DIR}/display/st7789/st7789.c
    ${DRIVERS_DIR}/display/st7789/rp2040/st7789_rp2040.c
    ${DRIVERS_DIR}/hal/rp2040/hal_rp2040.c
)

target_include_directories(earthbound_pico PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${DRIVERS_DIR}
)

target_link_libraries(earthbound_pico
    earthbound_game
    pico_stdlib
    hardware_spi
    hardware_gpio
    hardware_pwm
    hardware_dma
    hardware_flash
    hardware_sync
)

if(EB_ENABLE_AUDIO)
    target_link_libraries(earthbound_pico
        hardware_timer
        hardware_irq
        hardware_sync
    )
endif()

# The C macro must reflect the full 16 MB so the SDK's flash_range_erase/program
# hard_assert accepts offsets into the save area (beyond the linker region).
target_compile_definitions(earthbound_pico PRIVATE PICO_FLASH_SIZE_BYTES=16777216)

eb_target_warnings(earthbound_pico)

# Disable USB serial — frees TinyUSB resources and avoids CDC enumeration.
# Use SWD (Pico Probe + OpenOCD) for debugging instead.
pico_enable_stdio_usb(earthbound_pico 0)
pico_enable_stdio_uart(earthbound_pico 0)

# Dual-core PPU rendering: core 0 renders the top half of the screen, core 1
# the bottom half (drivers/platform/rp2040/worker.c). 56.6 fps vs 40.6 single
# core outside Ness's house at night, for +9.3 KB of main SRAM.
option(EB_ENABLE_DUAL_CORE_PPU "Split PPU rendering across both RP2040 cores" ON)

if(EB_ENABLE_DUAL_CORE_PPU)
    target_compile_definitions(earthbound_pico PRIVATE
        EB_ENABLE_DUAL_CORE_PPU=1
        # Core 1's stack shares SCRATCH_X with its two DMA line buffers
        # (2 x 480 B, worker.c); its deepest render path needs ~1.4 KB.
        PICO_CORE1_STACK_SIZE=3072
    )
    # PUBLIC: the drivers (worker.c, save.c) see the same ppu.h API.
    # Core 1 gets half the tile cache, and the renderer draws straight into
    # worker.c's DMA buffers instead of its own output lines.
    target_compile_definitions(earthbound_game PUBLIC
        EB_PPU_NUM_RENDER_CONTEXTS=2
        EB_PPU_SECONDARY_TILE_CACHE_BITS=7
        EB_PPU_EXTERNAL_LINE_OUT=1
    )
    target_link_libraries(earthbound_pico pico_multicore hardware_sync)
endif()

# Generate UF2 for drag-and-drop flashing
pico_add_extra_outputs(earthbound_pico)

# Static RAM budget: the build fails when .data + .bss exceeds RAM_BUDGET_BYTES
# (.data includes the .time_critical PPU functions copied to SRAM). Measured
# 2026-10-25, pico-sdk 2.2.0 + arm-none-eabi-gcc 15.3, Release, dual core:
# .data 35,472 + .bss 141,944 = 177,416 bytes (single core: 168,108; the
# second render context is the difference). The budget is a growth ratchet
# 4 KB above the dual-core figure, not a hardware limit: main SRAM is 256 KB
# (0x20000000-0x2003FFFF) and holds only the 192-byte vector table, .data and
# .bss (the heap is empty), so ~83 KB of it is free. The two 4 KB scratch
# banks hold the core 0 stack (SCRATCH_Y) and core 1's stack plus its DMA
# line buffers (SCRATCH_X; the save sector buffer in single-core builds).
# Raise the budget only for intended growth, in the same commit, saying why.
# It is a cache variable, so an existing build dir keeps its old value until
# reconfigured with -U RAM_BUDGET_BYTES.
set(RAM_BUDGET_BYTES 181512 CACHE STRING "Max .data + .bss bytes for earthbound_pico")
get_filename_component(EB_ARM_TOOLCHAIN_DIR "${CMAKE_C_COMPILER}" DIRECTORY)
find_program(EB_ARM_SIZE arm-none-eabi-size HINTS "${EB_ARM_TOOLCHAIN_DIR}" REQUIRED)
add_custom_command(TARGET earthbound_pico POST_BUILD
    COMMAND ${CMAKE_COMMAND}
        -DSIZE_TOOL=${EB_ARM_SIZE}
        -DELF=$<TARGET_FILE:earthbound_pico>
        -DBUDGET=${RAM_BUDGET_BYTES}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_ram_budget.cmake
    VERBATIM
)

# "make flash" — load UF2 via picotool (auto-reboots into BOOTSEL if needed)
# picotool reboot may return a non-zero exit code even on success, so ignore it.
add_custom_target(flash
    COMMAND picotool load -f $<TARGET_FILE_DIR:earthbound_pico>/earthbound_pico.uf2
    COMMAND picotool reboot || true
    DEPENDS earthbound_pico
    COMMENT "Flashing earthbound_pico.uf2 via picotool"
)
