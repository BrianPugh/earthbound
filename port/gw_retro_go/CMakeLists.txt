cmake_minimum_required(VERSION 3.16)
project(earthbound_gw_port C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# This CMakeLists.txt is a *compile-only* sanity check that the platform.h
# interface in src/ is satisfied by the gw_retro_go stubs. It deliberately
# does NOT pull in src/ via add_subdirectory(), because the libgame build
# requires ebtools + a donor ROM (earthbound.sfc) to extract assets, and
# we want this check to run in CI without those dependencies.
#
# The real build is driven by the firmware repo
# (game-and-watch-retro-go-sd)'s GNU Make pipeline, which compiles src/,
# this port's main.c, and its own Core/Src/porting/earthbound/gw_*.c
# implementations (which override these stubs at link time).
#
# Output: libearthbound_gw_port.a containing main.o + platform/*.o, all
# compiled against the current src/platform/platform.h. Mismatched
# signatures break the build; that's the entire point.
#
# Typical use (cross-compile for STM32H7B0VB, matching firmware ABI):
#   cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake
#   cmake --build build
#
# Host-side syntax check (no cross toolchain needed):
#   cmake -S . -B build-host
#   cmake --build build-host

# Source files: upstream entry point + every platform/*.c stub.
file(GLOB PLATFORM_SOURCES CONFIGURE_DEPENDS "platform/*.c")

add_library(earthbound_gw_port STATIC
    main.c
    ${PLATFORM_SOURCES}
)

target_include_directories(earthbound_gw_port PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/include
)

# Embedded-build flags. EB_EMBEDDED gates the no-op LOG_WARN/LOG_TRACE,
# the trap-version of FATAL, and the state_dump.c stub. EB_NORELOC_REQUIRED
# gates the EB_NORELOC attribute that places pointer tables in .noreloc
# (RAM) for the firmware's PatchCodeRodataOffset fix-up. Both are set
# here so the stubs compile under the same constraints they'll face when
# the firmware links them in.
target_compile_definitions(earthbound_gw_port PRIVATE
    EB_EMBEDDED=1
    EB_NORELOC_REQUIRED=1
)

target_compile_options(earthbound_gw_port PRIVATE
    -Wall
    -Wextra
    -Wno-unused-parameter
)
