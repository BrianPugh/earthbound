cmake_minimum_required(VERSION 3.16)
project(earthbound_game C)
set(CMAKE_C_STANDARD 11)

file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS
    "core/*.c"
    "entity/*.c"
    "game/*.c"
    "intro/*.c"
    "platform/*.c"
    "snes/*.c"
    "verify/*.c"
    "game_main.c"
)
file(GLOB_RECURSE DATA_SOURCES CONFIGURE_DEPENDS "data/*.c")

add_library(earthbound_game STATIC ${SOURCES} ${DATA_SOURCES})

# Viewport resolution (defaults to SNES native 256x224)
set(VIEWPORT_WIDTH 256 CACHE STRING "Viewport width in pixels")
set(VIEWPORT_HEIGHT 224 CACHE STRING "Viewport height in pixels")

target_include_directories(earthbound_game PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    include/
)

target_compile_definitions(earthbound_game PUBLIC
    USA=1
    VIEWPORT_WIDTH=${VIEWPORT_WIDTH}
    VIEWPORT_HEIGHT=${VIEWPORT_HEIGHT}
)

if(MSVC)
    target_compile_options(earthbound_game PRIVATE /W4)
else()
    target_compile_options(earthbound_game PRIVATE -Wall -Wextra -Wno-unused-parameter -ffunction-sections -fdata-sections)
endif()

# Embedded assets - game assets are compiled into the binary via incbin.h
find_program(EBTOOLS_EXE ebtools REQUIRED)

set(EMBED_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(EMBED_BIN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../asm/bin")
set(EMBED_MANIFEST "${EMBED_BIN_DIR}/assets.manifest")
set(EMBED_INCBIN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendor/incbin")

# Packed assets: human-friendly exports (assets/) are packed back to binary
# in the build directory and used as overrides over asm/bin/.
set(EMBED_PACKED_DIR "${CMAKE_CURRENT_BINARY_DIR}/packed_assets")
set(ASSETS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/assets")
set(PACK_ALL_STAMP "${CMAKE_CURRENT_BINARY_DIR}/pack_all.stamp")
set(PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

set(EMBED_ARRAY "${EMBED_GEN_DIR}/embedded_assets_array.c")
set(EMBED_INC "${EMBED_GEN_DIR}/embedded_assets.inc.c")
set(EMBED_HEADER "${EMBED_GEN_DIR}/embedded_assets.h")
set(EMBED_IDS "${EMBED_GEN_DIR}/asset_ids.h")
set(EMBED_DECL "${EMBED_GEN_DIR}/embedded_assets_decl.h")

# Glob all human-friendly asset files so pack-all re-runs when they change
file(GLOB_RECURSE ASSET_SOURCE_FILES CONFIGURE_DEPENDS "${ASSETS_DIR}/*")

add_custom_command(
    OUTPUT ${PACK_ALL_STAMP}
    COMMAND ${EBTOOLS_EXE} pack-all
        ${ASSETS_DIR}
        ${EMBED_PACKED_DIR}
        -b ${EMBED_BIN_DIR}
        -y ${PROJECT_ROOT}/earthbound.yml
        -c ${PROJECT_ROOT}/commondefs.yml
    COMMAND ${CMAKE_COMMAND} -E touch ${PACK_ALL_STAMP}
    DEPENDS ${ASSET_SOURCE_FILES}
    COMMENT "Packing all human-friendly assets to binary"
)

# Audio subsystem option (declared early so embed-registry exclusions can check it)
option(ENABLE_AUDIO "Enable audio subsystem (lakesnes SPC700/DSP emulator)" ON)

# Optional asset exclusions (e.g., audio packs when audio is disabled)
set(EMBED_EXCLUDE_ARGS "")
if(NOT ENABLE_AUDIO)
    list(APPEND EMBED_EXCLUDE_ARGS --exclude "audiopacks/*")
endif()

# Exclude individual overworld sprite GFX files -- the C port loads
# pre-packed bank files (overworld_sprites/banks/*.bin) instead.
list(APPEND EMBED_EXCLUDE_ARGS --exclude "overworld_sprites/gfx/*")
list(APPEND EMBED_EXCLUDE_ARGS --exclude "US/overworld_sprites/gfx/*")

# Exclude monolithic PSI arrangement .arr.lzhal files -- the C port uses
# .arr.bundled (8-frame bundles) for streaming decompression instead.
list(APPEND EMBED_EXCLUDE_ARGS --exclude "psianims/arrangements/*.arr.lzhal")

add_custom_command(
    OUTPUT ${EMBED_ARRAY} ${EMBED_INC} ${EMBED_HEADER} ${EMBED_IDS} ${EMBED_DECL}
    COMMAND ${EBTOOLS_EXE} embed-registry
        ${EMBED_MANIFEST}
        ${EMBED_BIN_DIR}
        ${EMBED_GEN_DIR}
        ${EMBED_INCBIN_DIR}
        --custom-dir ${EMBED_PACKED_DIR}
        --locale US
        ${EMBED_EXCLUDE_ARGS}
    DEPENDS ${EMBED_MANIFEST} ${PACK_ALL_STAMP}
    COMMENT "Generating embedded asset registry"
)

target_sources(earthbound_game PRIVATE ${EMBED_ARRAY})
target_include_directories(earthbound_game PUBLIC ${EMBED_GEN_DIR})

# Suppress warnings in generated code
set_source_files_properties(${EMBED_ARRAY} PROPERTIES COMPILE_FLAGS "-w")


if(ENABLE_AUDIO)
    add_library(lakesnes_audio STATIC
        vendor/lakesnes/snes/apu.c
        vendor/lakesnes/snes/spc.c
        vendor/lakesnes/snes/dsp.c
        vendor/lakesnes/snes/statehandler.c
    )
    target_include_directories(lakesnes_audio PRIVATE vendor/lakesnes/snes/)
    target_compile_options(lakesnes_audio PRIVATE -w)

    target_link_libraries(earthbound_game lakesnes_audio)
    target_include_directories(earthbound_game PUBLIC vendor/lakesnes/snes/)
    target_compile_definitions(earthbound_game PUBLIC ENABLE_AUDIO=1)
endif()

# Side-by-side ROM verification using LakeSnes emulator
option(ENABLE_VERIFY "Enable side-by-side ROM verification" OFF)

if(ENABLE_VERIFY)
    file(GLOB LAKESNES_SOURCES "vendor/lakesnes/snes/*.c")
    add_library(lakesnes STATIC ${LAKESNES_SOURCES})
    target_include_directories(lakesnes PRIVATE vendor/lakesnes/)
    target_compile_options(lakesnes PRIVATE
        -include ${CMAKE_CURRENT_SOURCE_DIR}/vendor/lakesnes/lk_prefix.h
        -w
    )
    target_compile_definitions(lakesnes PRIVATE ENABLE_VERIFY=1)

    target_link_libraries(earthbound_game lakesnes)
    target_compile_definitions(earthbound_game PUBLIC ENABLE_VERIFY=1)
    target_include_directories(earthbound_game PUBLIC vendor/lakesnes/)
endif()

# Generate music_generated.h from music_tracks.json
set(MUSIC_JSON "${CMAKE_CURRENT_SOURCE_DIR}/assets/music_tracks.json")
set(MUSIC_HEADER "${EMBED_GEN_DIR}/music_generated.h")

add_custom_command(
    OUTPUT ${MUSIC_HEADER}
    COMMAND ${EBTOOLS_EXE} generate music-header
        ${MUSIC_JSON} ${MUSIC_HEADER}
    DEPENDS ${MUSIC_JSON}
    COMMENT "Generating music_generated.h"
)
add_custom_target(music_header DEPENDS ${MUSIC_HEADER})

# Generate items_generated.h from items.json
set(ITEMS_JSON "${CMAKE_CURRENT_SOURCE_DIR}/assets/items/items.json")
set(ITEMS_HEADER "${EMBED_GEN_DIR}/items_generated.h")

add_custom_command(
    OUTPUT ${ITEMS_HEADER}
    COMMAND ${EBTOOLS_EXE} generate items-header ${ITEMS_JSON} ${ITEMS_HEADER}
    DEPENDS ${ITEMS_JSON}
    COMMENT "Generating items_generated.h"
)
add_custom_target(items_header DEPENDS ${ITEMS_HEADER})

# earthbound_game needs items_generated.h (included via constants.h)
add_dependencies(earthbound_game items_header)

# Build-time struct info generator for state dump analysis
# Skip when cross-compiling — gen_struct_info is a host tool that can't run on the target.
if(NOT CMAKE_CROSSCOMPILING)
    add_executable(gen_struct_info tools/gen_struct_info.c)
    target_include_directories(gen_struct_info PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} include/ ${EMBED_GEN_DIR})
    target_compile_definitions(gen_struct_info PRIVATE
        USA=1
        VIEWPORT_WIDTH=${VIEWPORT_WIDTH}
        VIEWPORT_HEIGHT=${VIEWPORT_HEIGHT}
        $<$<BOOL:${ENABLE_AUDIO}>:ENABLE_AUDIO=1>
    )
    add_dependencies(gen_struct_info items_header music_header)

    set(STRUCT_INFO_JSON "${CMAKE_CURRENT_BINARY_DIR}/struct_info.json")
    add_custom_command(
        OUTPUT ${STRUCT_INFO_JSON}
        COMMAND gen_struct_info > ${STRUCT_INFO_JSON}
        DEPENDS gen_struct_info ${ITEMS_HEADER} ${MUSIC_HEADER}
        COMMENT "Generating struct_info.json"
    )
    add_custom_target(struct_info ALL DEPENDS ${STRUCT_INFO_JSON})
endif()

# Sanitizer support (usage: cmake -DENABLE_ASAN=ON -DENABLE_UBSAN=ON ...)
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
    set(_SANITIZER_COMPILE_FLAGS -fsanitize=${_SANITIZER_FLAGS} -fno-omit-frame-pointer)
    set(_SANITIZER_LINK_FLAGS -fsanitize=${_SANITIZER_FLAGS})

    target_compile_options(earthbound_game PRIVATE ${_SANITIZER_COMPILE_FLAGS})
    target_link_options(earthbound_game PRIVATE ${_SANITIZER_LINK_FLAGS})
    if(TARGET lakesnes_audio)
        target_compile_options(lakesnes_audio PRIVATE ${_SANITIZER_COMPILE_FLAGS})
        target_link_options(lakesnes_audio PRIVATE ${_SANITIZER_LINK_FLAGS})
    endif()

    if(TARGET lakesnes)
        target_compile_options(lakesnes PRIVATE ${_SANITIZER_COMPILE_FLAGS})
        target_link_options(lakesnes PRIVATE ${_SANITIZER_LINK_FLAGS})
    endif()
endif()
