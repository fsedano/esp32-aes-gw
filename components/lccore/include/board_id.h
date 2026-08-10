/**
  ******************************************************************************
  * @file    board_id.h
  * @brief   Board identity constants for the ESP32-S3 ARINC429 linecard.
  *
  *          Mirrors arinc4i4o's board_info.h. The SSDP SERVER board_id token
  *          is what the gateway uses to identify the card type; this card
  *          advertises "A429-ESP_4DH" (registered gateway-side as a distinct
  *          product from the STM32 "A429-8BD" and from the pre-HID
  *          "A429-ESP_4D"). The recovery build advertises
  *          the same identity with a "BL-" prefix and fw_type = 1, which is
  *          how clients recognize a board sitting in bootloader mode.
  ******************************************************************************
  */

#ifndef LCCORE_BOARD_ID_H
#define LCCORE_BOARD_ID_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#ifndef VERSION_STRING
#define VERSION_STRING "v0.0.0-dev"
#endif
#ifndef VERSION_COMMIT
#define VERSION_COMMIT "00000000"
#endif

#ifdef RECOVERY_BUILD
#define BOARD_FRIENDLY_NAME     "Arinc429 ESP Interface HID (BL)"
#define BOARD_MODEL_NAME        "Arinc429 ESP 4I/4O HID"
#define BOARD_INFO_MODEL        "BL-A429-ESPH"
#define BOARD_INFO_SHORT_ID     "BL-A429-ESP_4DH"
#define BOARD_INFO_FW_TAG       ("bl-" VERSION_STRING)
#define BOARD_INFO_FW_TYPE      ((uint8_t) 1)   /* GET_FW_INFO: bootloader */
#define BOARD_INFO_FW_BRANCH    "recovery"
#else
#define BOARD_FRIENDLY_NAME     "Arinc429 ESP Interface HID"
#define BOARD_MODEL_NAME        "Arinc429 ESP 4I/4O HID"
#define BOARD_INFO_MODEL        "A429-ESPH"
#define BOARD_INFO_SHORT_ID     "A429-ESP_4DH"
#define BOARD_INFO_FW_TAG       (VERSION_STRING)
#define BOARD_INFO_FW_TYPE      ((uint8_t) 0)   /* GET_FW_INFO: application */
#define BOARD_INFO_FW_BRANCH    "esp32"
#endif

/* DHCP hostname prefix; full hostname = prefix + first 8 serial hex chars. */
#define BOARD_HOSTNAME_PREFIX   "A429E-"

#ifdef __cplusplus
}
#endif

#endif /* LCCORE_BOARD_ID_H */
