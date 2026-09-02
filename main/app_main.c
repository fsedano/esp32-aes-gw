/**
  ******************************************************************************
  * @file    app_main.c
  * @brief   ESP32-S3 ARINC429 linecard entry point.
  *
  *          Boot sequence: NVS -> port/log -> identity (eFuse ETH MAC) ->
  *          W5500 Ethernet + DHCP -> SSDP + description.xml + comm tasks.
  *          The same binary builds two personalities:
  *            - application (default), lives in ota_0
  *            - recovery (-DRECOVERY_BUILD=1), lives in the factory
  *              partition and serves FW_UPDATE (the "bootloader" the
  *              gateway's upgrade dialog talks to)
  ******************************************************************************
  */

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "nvs_flash.h"

#include "board_id.h"
#include "capabilities.h"
#include "comm_task.h"
#include "discrete_glue.h"
#include "find_me.h"
#include "fwup_port.h"
#include "hid_port.h"
#include "identity.h"
#include "ota_ops.h"
#include "lc_log.h"
#include "lc_port.h"
#include "net_eth.h"
#include "rs485_discrete.h"
#include "ssdp_task.h"

static const char *TAG = "app_main";

void app_main(void){
    esp_err_t err = nvs_flash_init();
    if(err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND){
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    lc_port_init();
    lc_log_init();

    /* The Modbus RTU worker owns UART0 and binds the nonblocking backend
       consumed by the Ethernet wire protocol. */
#ifndef RECOVERY_BUILD
    rs485_discrete_init();
    uint16_t discrete_inputs = rs485_discrete_input_count();
    uint16_t discrete_outputs = rs485_discrete_output_count();
    discrete_glue_configure(discrete_inputs, discrete_outputs);
    capabilities_init(discrete_inputs, discrete_outputs);
#endif

    /* Identity: everything (UID, UUID, serial, hostname) derives from the
       eFuse-based Ethernet MAC, which is also programmed into the W5500. */
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_ETH));
    identity_init(mac);

    ESP_LOGI(TAG, "%s %s (%s)", BOARD_INFO_SHORT_ID, BOARD_INFO_FW_TAG,
             VERSION_COMMIT);
    ESP_LOGI(TAG, "uuid %s serial %s hostname %s",
             identity_uuid(), identity_serial(), identity_hostname());
    LOG_INF("FRAN was here; serial=%s hostname=%s", identity_serial(), identity_hostname());
    LOG_INF("boot: %s %s uuid %s", BOARD_INFO_SHORT_ID, BOARD_INFO_FW_TAG,
            identity_uuid());

#ifndef RECOVERY_BUILD
    /* USB HID joystick: claims the OTG PHY, so the USB-Serial-JTAG console
       on the USB-C port stops here. Runtime logs use the Ethernet wire
       protocol and UART0 is reserved for RS-485. Needs identity for the USB
       serial string, hence after identity_init. */
    hid_port_init();
#endif

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(net_eth_start(mac, identity_hostname()));

    find_me_init();
#ifdef RECOVERY_BUILD
    fwup_port_init();
#endif

    /* A freshly OTA'd image marks itself valid after ~10 s of uptime (it has
       proven it does not crash-loop). Deliberately not gated on the network:
       a power cycle on a networkless bench must not roll a good app back to
       recovery. No-op in the recovery build. */
    ota_rollback_timer_start();

    ssdp_start();
    comm_start();
}
