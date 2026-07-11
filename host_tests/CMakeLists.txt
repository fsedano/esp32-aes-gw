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

set(LCCORE_SOURCES
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

add_library(lccore STATIC ${LCCORE_SOURCES})
target_include_directories(lccore PUBLIC
    ${LCCORE}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/support
)
# Pinned version fields so payload-layout tests are independent of git state.
target_compile_definitions(lccore PUBLIC
    VERSION_STRING="v0.1.0-test"
    VERSION_COMMIT="deadbeef"
)

# Recovery personality: the same sources compiled with RECOVERY_BUILD so the
# recovery-only comm dispatch (CMD_FW_UPDATE + START-ACK chunk-size
# advertisement) is host-testable too.
add_library(lccore_recovery STATIC ${LCCORE_SOURCES})
target_include_directories(lccore_recovery PUBLIC
    ${LCCORE}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/support
)
target_compile_definitions(lccore_recovery PUBLIC
    RECOVERY_BUILD=1
    VERSION_STRING="v0.1.0-test"
    VERSION_COMMIT="deadbeef"
)

foreach(t framing ack_info discrete fletcher32 identity ssdp log_ring fwupdate)
    add_executable(test_${t} tests/test_${t}.c)
    target_link_libraries(test_${t} PRIVATE lccore)
    add_test(NAME ${t} COMMAND test_${t})
endforeach()

add_executable(test_fwupdate_comm tests/test_fwupdate_comm.c)
target_link_libraries(test_fwupdate_comm PRIVATE lccore_recovery)
add_test(NAME fwupdate_comm COMMAND test_fwupdate_comm)

# VERSION_STRING format contract (pure cmake, no compilation needed).
add_test(NAME version_format
    COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_version_format.cmake
)
