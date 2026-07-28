# Standalone host-test CMake project.
#
# Doesn't use idf.py at all — just compiles hunter_rmt.c (the pure-logic
# half that doesn't touch ESP-IDF headers when CONFIG_IDF_TARGET_LINUX=1)
# together with the test cases and a vendored copy of unity, and runs
# the resulting binary natively.
#
# Usage:
#   cd components/hunter_rmt/test
#   cmake -B build
#   cmake --build build
#   ./build/hunter_rmt_test

cmake_minimum_required(VERSION 3.16)
project(hunter_rmt_test C)

# Path to the parent hunter_rmt component and to ESP-IDF's vendored unity
# (sourced via IDF_PATH env var if set, otherwise fall back to /opt/esp-idf).
get_filename_component(HUNTER_RMT_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
if(DEFINED ENV{IDF_PATH})
    set(IDF_PATH $ENV{IDF_PATH})
else()
    set(IDF_PATH "/opt/esp-idf")
endif()
set(UNITY_DIR "${IDF_PATH}/components/unity/unity/src")

add_executable(hunter_rmt_test
    main/main.c
    ${HUNTER_RMT_DIR}/hunter_rmt.c
    ${UNITY_DIR}/unity.c
)

target_include_directories(hunter_rmt_test PRIVATE
    ${HUNTER_RMT_DIR}/include
    ${UNITY_DIR}
    ${CMAKE_CURRENT_LIST_DIR}      # finds sdkconfig.h stub
)

# Pretend we're on the linux target so hunter_rmt.h picks the test-friendly
# rmt_symbol_word_t typedef and skips the IDF-only code path.
target_compile_definitions(hunter_rmt_test PRIVATE
    CONFIG_IDF_TARGET_LINUX=1
)

# Stubs for the few IDF things hunter_rmt.c references in the linux branch
# (none currently — the linux branch only does validation). Add to this list
# if real IDF calls creep into the testable code path.

enable_testing()
add_test(NAME hunter_rmt_unit_tests COMMAND hunter_rmt_test)
