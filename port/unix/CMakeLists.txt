cmake_minimum_required(VERSION 3.16)
project(earthbound C)
set(CMAKE_C_STANDARD 11)

# Emit compile_commands.json so clangd/IDEs resolve include paths and types.
# A root-level symlink (../../compile_commands.json) points at this file.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(EB_VIEWPORT_WIDTH 256 CACHE STRING "Viewport width in pixels")
set(EB_VIEWPORT_HEIGHT 224 CACHE STRING "Viewport height in pixels")

# Build game library from src/
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../src
                 ${CMAKE_CURRENT_BINARY_DIR}/game_lib)

# Find SDL2
find_package(SDL2 QUIET)
if(NOT SDL2_FOUND)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED sdl2)
endif()

# Platform executable. Use GLOB_RECURSE (not plain GLOB): a non-recursive
# `file(GLOB ... CONFIGURE_DEPENDS ...)` silently returns empty on some
# filesystems (notably the Docker Desktop virtiofs mount used by the i386 build
# in port/unix/docker), which drops every platform/*.c object and breaks the
# link. GLOB_RECURSE + CONFIGURE_DEPENDS works there and matches src/CMakeLists.txt.
# platform/ has no subdirectories, so this is equivalent to a flat glob.
file(GLOB_RECURSE PLATFORM_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/platform/*.c")
add_executable(earthbound main.c ${PLATFORM_SOURCES})

target_link_libraries(earthbound earthbound_game)

if(TARGET SDL2::SDL2)
    target_link_libraries(earthbound SDL2::SDL2)
else()
    target_include_directories(earthbound PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(earthbound ${SDL2_LIBRARIES})
    target_link_directories(earthbound PRIVATE ${SDL2_LIBRARY_DIRS})
endif()

# Platform code needs SDL2 headers and game library headers (inherited via PUBLIC)
target_include_directories(earthbound PRIVATE
    ${SDL2_INCLUDE_DIRS}
)

if(MSVC)
    target_compile_options(earthbound PRIVATE /W4)
else()
    target_compile_options(earthbound PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

# Sanitizer support
option(ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)

set(_SANITIZERS "")
if(ENABLE_ASAN)
    list(APPEND _SANITIZERS "address")
endif()
if(ENABLE_UBSAN)
    list(APPEND _SANITIZERS "undefined")
endif()

if(_SANITIZERS)
    list(JOIN _SANITIZERS "," _SANITIZER_FLAGS)
    target_compile_options(earthbound PRIVATE -fsanitize=${_SANITIZER_FLAGS} -fno-omit-frame-pointer)
    target_link_options(earthbound PRIVATE -fsanitize=${_SANITIZER_FLAGS})
endif()
