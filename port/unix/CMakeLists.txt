cmake_minimum_required(VERSION 3.16)
project(earthbound C)
set(CMAKE_C_STANDARD 11)

set(VIEWPORT_WIDTH 256 CACHE STRING "Viewport width in pixels")
set(VIEWPORT_HEIGHT 224 CACHE STRING "Viewport height in pixels")

# Build game library from src/
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../src
                 ${CMAKE_CURRENT_BINARY_DIR}/game_lib)

# Find SDL2
find_package(SDL2 QUIET)
if(NOT SDL2_FOUND)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED sdl2)
endif()

# Platform executable
file(GLOB PLATFORM_SOURCES CONFIGURE_DEPENDS "platform/*.c")
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
