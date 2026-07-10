# Host-native unit tests for the protocol core (no ESP-IDF needed) — the
# arinc4i4o pattern of compiling the REAL firmware protocol sources against a
# host port layer. Usage:
#   cmake -S host_tests -B host_tests/build
#   cmake --build host_tests/build
#   ctest --test-dir host_tests/build --output-on-failure

cmake_minimum_required(VERSION 3.16)
project(lc_host_tests C)

set(CMAKE_C_STANDARD 11)
enable_testing()

add_compile_options(-g -O1 -Wall -Wextra)

set(LCCORE ${CMAKE_CURRENT_SOURCE_DIR}/../components/lccore)

add_library(lccore STATIC
    ${LCCORE}/comm_core.c
    ${LCCORE}/identity.c
    ${LCCORE}/fletcher32.c
    ${LCCORE}/lc_log.c
    ${LCCORE}/ssdp_msg.c
    ${LCCORE}/arinc_glue.c
    ${LCCORE}/discrete_glue.c
    ${LCCORE}/fwupdate_core.c
    support/host_port.c
    support/comm_capture.c
)
target_include_directories(lccore PUBLIC
    ${LCCORE}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/support
)
# Pinned version fields so payload-layout tests are independent of git state.
target_compile_definitions(lccore PUBLIC
    VERSION_STRING="v0.1.0-test"
    VERSION_COMMIT="deadbeef"
)

foreach(t framing ack_info discrete fletcher32 identity ssdp log_ring fwupdate)
    add_executable(test_${t} tests/test_${t}.c)
    target_link_libraries(test_${t} PRIVATE lccore)
    add_test(NAME ${t} COMMAND test_${t})
endforeach()
