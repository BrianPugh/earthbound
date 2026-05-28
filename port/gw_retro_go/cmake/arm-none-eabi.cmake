# arm-none-eabi cross-compile toolchain for the Game & Watch (STM32H7B0VB).
#
# Used by port/gw_retro_go/CMakeLists.txt to compile the platform stubs with
# the same Cortex-M7 ABI as the host firmware. This is a *compile-only* sanity
# check; the firmware repo (game-and-watch-retro-go-sd) is the real build
# system and produces the linked binary.
#
# Invoke from port/gw_retro_go/ as:
#   cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake
#   cmake --build build

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

# Static library only — CMake's try_compile would otherwise fail trying to
# link a hosted executable against a freestanding toolchain.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# Allow the user to override via -DTOOLCHAIN_PREFIX=... if their toolchain
# isn't on PATH; otherwise resolve from PATH.
if(NOT DEFINED TOOLCHAIN_PREFIX)
    set(TOOLCHAIN_PREFIX arm-none-eabi-)
endif()

set(CMAKE_C_COMPILER ${TOOLCHAIN_PREFIX}gcc)
set(CMAKE_ASM_COMPILER ${TOOLCHAIN_PREFIX}gcc)
set(CMAKE_AR ${TOOLCHAIN_PREFIX}ar)
set(CMAKE_OBJCOPY ${TOOLCHAIN_PREFIX}objcopy)
set(CMAKE_OBJDUMP ${TOOLCHAIN_PREFIX}objdump)

# Cortex-M7 with FPv5-D16 hard-float ABI — matches the firmware's
# Makefile.common (CPU, FPU, FLOAT-ABI variables).
set(MCU_FLAGS "-mcpu=cortex-m7 -mtune=cortex-m7 -mthumb -mno-unaligned-access -mfpu=fpv5-d16 -mfloat-abi=hard")

set(CMAKE_C_FLAGS_INIT "${MCU_FLAGS} -ffunction-sections -fdata-sections")
set(CMAKE_ASM_FLAGS_INIT "${MCU_FLAGS}")

# Don't search the host filesystem for libraries/headers.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
