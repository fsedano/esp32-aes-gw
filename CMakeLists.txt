# ESP32-S3 ARINC429 linecard (A429-ESP_4DH) — ESP-IDF v5.5 project.
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

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

if(RECOVERY_BUILD)
    idf_build_set_property(COMPILE_DEFINITIONS "RECOVERY_BUILD=1" APPEND)
endif()

project(esp32-aes-gw)
