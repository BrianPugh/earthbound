# Post-build RAM budget check (cmake -P script).
#
#   cmake -DSIZE_TOOL=arm-none-eabi-size -DELF=x.elf -DBUDGET=N -P check_ram_budget.cmake
#
# Sums the .data and .bss sections of ELF (static RAM: .data also holds the
# .time_critical functions copied to SRAM) and fails when the total exceeds
# BUDGET bytes. Prints the numbers either way so the build log records them.
foreach(var SIZE_TOOL ELF BUDGET)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "check_ram_budget.cmake: ${var} not set")
    endif()
endforeach()

execute_process(
    COMMAND "${SIZE_TOOL}" -A "${ELF}"
    OUTPUT_VARIABLE out
    RESULT_VARIABLE rc
)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${SIZE_TOOL} -A ${ELF} failed (${rc})")
endif()

set(data 0)
set(bss 0)
string(REPLACE "\n" ";" lines "${out}")
foreach(line IN LISTS lines)
    if(line MATCHES "^\\.data[ \t]+([0-9]+)")
        set(data ${CMAKE_MATCH_1})
    elseif(line MATCHES "^\\.bss[ \t]+([0-9]+)")
        set(bss ${CMAKE_MATCH_1})
    endif()
endforeach()

math(EXPR total "${data} + ${bss}")
math(EXPR headroom "${BUDGET} - ${total}")
if(total GREATER BUDGET)
    message(FATAL_ERROR
        "RAM budget exceeded: .data ${data} + .bss ${bss} = ${total} bytes > RAM_BUDGET_BYTES ${BUDGET} "
        "(${headroom} bytes). Find the new static (arm-none-eabi-nm --size-sort -S on the ELF) and remove it, "
        "or raise RAM_BUDGET_BYTES in port/waveshare/pico-lcd-1.3/CMakeLists.txt if the growth is intended.")
endif()
message(STATUS "RAM: .data ${data} + .bss ${bss} = ${total} of RAM_BUDGET_BYTES ${BUDGET} (${headroom} bytes headroom)")
