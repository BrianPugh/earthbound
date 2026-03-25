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
set(VIEWPORT_WIDTH 240 CACHE STRING "Viewport width in pixels")
set(VIEWPORT_HEIGHT 240 CACHE STRING "Viewport height in pixels")

# RP2040 has only 264KB RAM; game BSS alone is ~250KB, leaving no room
# for the SPC700/DSP emulator's 64KB APU RAM allocation.
set(ENABLE_AUDIO OFF CACHE BOOL "" FORCE)

# Build game library from src/
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../../src
                 ${CMAKE_CURRENT_BINARY_DIR}/game_lib)

# Optimize game library: -Os globally to reduce code size (fits more in 16KB XIP
# cache).  PPU rendering overrides to -O3 via PPU_FORCE_SPEED_OPT pragma.
target_compile_options(earthbound_game PRIVATE -Os)

# Place hot PPU rendering functions in SRAM (avoids XIP cache contention),
# and force -O3 for the PPU rendering file via pragma.
target_compile_definitions(earthbound_game PRIVATE
    PPU_RAM_SECTION=".time_critical"
    PPU_FORCE_SPEED_OPT=1
    MAX_FRAME_SKIP=2
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

if(ENABLE_AUDIO)
    target_link_libraries(earthbound_pico
        hardware_timer
        hardware_irq
        hardware_sync
    )
endif()

# The C macro must reflect the full 16 MB so the SDK's flash_range_erase/program
# hard_assert accepts offsets into the save area (beyond the linker region).
target_compile_definitions(earthbound_pico PRIVATE PICO_FLASH_SIZE_BYTES=16777216)

target_compile_options(earthbound_pico PRIVATE -Wall -Wextra -Wno-unused-parameter)

# Disable USB serial — frees TinyUSB resources and avoids CDC enumeration.
# Use SWD (Pico Probe + OpenOCD) for debugging instead.
pico_enable_stdio_usb(earthbound_pico 0)
pico_enable_stdio_uart(earthbound_pico 0)

# Dual-core PPU rendering: distribute even/odd scanlines across both RP2040 cores.
option(ENABLE_DUAL_CORE_PPU "Distribute PPU rendering across both RP2040 cores" OFF)

if(ENABLE_DUAL_CORE_PPU)
    target_compile_definitions(earthbound_pico PRIVATE
        ENABLE_DUAL_CORE_PPU=1
        PICO_CORE1_STACK_SIZE=4096
    )
    target_compile_definitions(earthbound_game PRIVATE
        PPU_NUM_RENDER_CONTEXTS=2
    )
    target_link_libraries(earthbound_pico pico_multicore hardware_sync)
endif()

# Generate UF2 for drag-and-drop flashing
pico_add_extra_outputs(earthbound_pico)

# "make flash" — load UF2 via picotool (auto-reboots into BOOTSEL if needed)
# picotool reboot may return a non-zero exit code even on success, so ignore it.
add_custom_target(flash
    COMMAND picotool load -f $<TARGET_FILE_DIR:earthbound_pico>/earthbound_pico.uf2
    COMMAND picotool reboot || true
    DEPENDS earthbound_pico
    COMMENT "Flashing earthbound_pico.uf2 via picotool"
)
