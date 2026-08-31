# ESP32-S3 M31/HID linecard (AES-ESP-M31-HID) — ESP-IDF v5.5 project.
#
# Application build (ota_0 partition):
#   idf.py set-target esp32s3
#   idf.py build
#
# Recovery build (factory partition, "BL-" bootloader personality):
#   idf.py -B build-recovery -DSDKCONFIG=sdkconfig.recovery \
#          -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.recovery" \
#          -DRECOVERY_BUILD=1 build

cmake_minimum_required(VERSION 3.16)

# release.sh uses this for bench releases built from a dirty tree. Keep the
# override constrained to a tag-like value that fits GET_FW_INFO's 24-byte
# version field; normal builds leave it empty and derive the version from Git.
set(FW_VERSION_OVERRIDE "" CACHE STRING
    "Version embedded in the app descriptor and AES wire metadata")
if(FW_VERSION_OVERRIDE)
    if(NOT FW_VERSION_OVERRIDE MATCHES "^v[0-9A-Za-z][0-9A-Za-z._-]*$")
        message(FATAL_ERROR
            "FW_VERSION_OVERRIDE must be a tag-like value beginning with v")
    endif()
    string(LENGTH "${FW_VERSION_OVERRIDE}" FW_VERSION_OVERRIDE_LEN)
    if(FW_VERSION_OVERRIDE_LEN GREATER 24)
        message(FATAL_ERROR
            "FW_VERSION_OVERRIDE exceeds GET_FW_INFO's 24-byte field")
    endif()
    # ESP-IDF reads PROJECT_VER while project() is being evaluated.
    set(PROJECT_VER "${FW_VERSION_OVERRIDE}")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

if(RECOVERY_BUILD)
    idf_build_set_property(COMPILE_DEFINITIONS "RECOVERY_BUILD=1" APPEND)
endif()

project(esp32-aes-gw)
