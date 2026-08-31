# lccore: transport/OS-independent protocol core (framing, dispatch, identity,
# SSDP builders, log ring, Fletcher32, FW-update state machine, glue stubs).
# Plain C11, no ESP-IDF includes — the same sources are compiled by the
# host_tests/ project. Environment services come in through lc_port.h,
# implemented in main/ (ESP-IDF) and host_tests/support (host).

# VERSION_STRING / VERSION_COMMIT from git describe (cmake/gen_version.cmake).
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/gen_version.cmake)

idf_component_register(
    SRCS
        "comm_core.c"
        "capabilities.c"
        "identity.c"
        "fletcher32.c"
        "lc_log.c"
        "ssdp_msg.c"
        "arinc_glue.c"
        "discrete_glue.c"
        "hid_glue.c"
        "fwupdate_core.c"
    INCLUDE_DIRS "include"
)

target_compile_definitions(${COMPONENT_LIB} PUBLIC
    VERSION_STRING="${VERSION_STRING}"
    VERSION_COMMIT="${VERSION_COMMIT}"
)
