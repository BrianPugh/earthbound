# Host-only battle-math unit test (CODE-QUALITY.md L2). Links libgame built
# with EB_HOST_TEST, which exposes a few static routines to the test.
#
#   cmake -S tests/c -B tests/c/build && cmake --build tests/c/build
#   tests/c/build/battle_math_test tests/c/battle_cases.txt
#
# tests/test_battle_math.py does this (it skips without extracted assets).
cmake_minimum_required(VERSION 3.16)
project(eb_c_tests C)
set(CMAKE_C_STANDARD 11)

option(EB_WERROR "Treat compiler warnings as errors" ON)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../src ${CMAKE_CURRENT_BINARY_DIR}/game_lib)
target_compile_definitions(earthbound_game PUBLIC EB_HOST_TEST=1)

add_executable(battle_math_test battle_math_test.c null_platform.c)
target_link_libraries(battle_math_test earthbound_game)
eb_target_warnings(battle_math_test)
