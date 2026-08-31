set(srcs
    "app_main.c"
    "lc_port_esp.c"
    "net_eth.c"
    "ssdp_task.c"
    "comm_task.c"
    "ota_ops.c"
    "rs485_test.c"
    "find_me.c"
    "fwup_port.c"       # body compiles empty unless RECOVERY_BUILD
)

# USB HID joystick backend (TinyUSB): application personality only. The
# recovery build never claims the OTG PHY, keeping USB-Serial-JTAG alive.
if(NOT RECOVERY_BUILD)
    list(APPEND srcs "hid_usb.c")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "."
    REQUIRES lccore
    PRIV_REQUIRES
        esp_eth
        esp_netif
        esp_event
        esp_timer
        esp_driver_spi
        esp_driver_gpio
        esp_driver_uart
        nvs_flash
        app_update
        mbedtls
        lwip
)
