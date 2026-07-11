set(srcs
    "app_main.c"
    "lc_port_esp.c"
    "net_eth.c"
    "ssdp_task.c"
    "comm_task.c"
    "ota_ops.c"
    "find_me.c"
    "fwup_port.c"       # body compiles empty unless RECOVERY_BUILD
)

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
        nvs_flash
        app_update
        mbedtls
        lwip
)
