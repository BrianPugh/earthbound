# Shared compiler warning flags for the game library and every port target.
#
#   include(<repo>/cmake/EbWarnings.cmake)
#   eb_target_warnings(<target>)
#
# EB_WERROR turns warnings into errors. It defaults OFF here; a port that is
# warning-clean declares `option(EB_WERROR ... ON)` before including this file
# (or before add_subdirectory(src)), and a cache value (-DEB_WERROR=OFF) always
# wins.
include_guard(GLOBAL)

option(EB_WERROR "Treat compiler warnings as errors" OFF)

set(EB_WARNING_FLAGS
    -Wall
    -Wextra
    -Wno-unused-parameter
    -Wshadow
    -Wsign-compare
    -Wstrict-prototypes
    -Wmissing-prototypes
    -Wimplicit-fallthrough
    -Wvla
)

function(eb_target_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
        if(EB_WERROR)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE ${EB_WARNING_FLAGS})
        if(EB_WERROR)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()
