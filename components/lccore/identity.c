/**
  ******************************************************************************
  * @file    identity.c
  * @brief   Board identity derived from the eFuse base MAC (see identity.h).
  ******************************************************************************
  */

#include "identity.h"
#include "board_id.h"
#include <string.h>

static const char hexd[] = "0123456789abcdef";

/* Salt mixed into the expanded half of the UID so the 12 bytes are not a
   trivial doubling of the MAC. Arbitrary but FROZEN: the UID is the auth key
   for JUMP_TO_BOOTLOADER/REBOOT and both the app and the recovery firmware
   must derive the same value forever. */
static const uint8_t uid_salt[6] = { 0xA4, 0x29, 0xE5, 0x9D, 0x4D, 0x53 };

static identity_mac_t      g_mac;
static identity_uid_t      g_uid;
static identity_uuid_t     g_uuid;
static identity_serial_t   g_serial;
static identity_hostname_t g_hostname;

void identity_init(const uint8_t mac[6]){
    memcpy(g_mac, mac, 6);

    /* UID[0..5] = MAC verbatim; UID[6..11] = reversed MAC XOR salt. */
    for(uint32_t i = 0; i < 6; i++){
        g_uid[i]     = mac[i];
        g_uid[6 + i] = (uint8_t)(mac[5 - i] ^ uid_salt[i]);
    }

    /* UUID: 24 lowercase hex chars of the 12-byte UID (same rule as the
       STM32 ssdp.c uuid_str). Used verbatim in the SSDP USN header. */
    for(uint32_t i = 0; i < 12; i++){
        g_uuid[i * 2]     = hexd[(g_uid[i] >> 4) & 0x0F];
        g_uuid[i * 2 + 1] = hexd[g_uid[i] & 0x0F];
    }
    g_uuid[24] = '\0';

    /* Serial: 12 hex chars from UID[0..5] (== the MAC), same rule as
       board_info_get_serial_number. */
    for(uint32_t i = 0; i < 6; i++){
        g_serial[i * 2]     = hexd[(g_uid[i] >> 4) & 0x0F];
        g_serial[i * 2 + 1] = hexd[g_uid[i] & 0x0F];
    }
    g_serial[12] = '\0';

    /* Hostname: prefix + first 8 serial chars (fits the 15-char limit). */
    uint32_t n = 0;
    for(const char *p = BOARD_HOSTNAME_PREFIX; *p != '\0'; p++){
        g_hostname[n++] = *p;
    }
    for(uint32_t j = 0; j < 8; j++){
        g_hostname[n++] = g_serial[j];
    }
    g_hostname[n] = '\0';
}

const uint8_t *identity_uid(void)     { return g_uid; }
const uint8_t *identity_mac(void)     { return g_mac; }
const char    *identity_uuid(void)    { return g_uuid; }
const char    *identity_serial(void)  { return g_serial; }
const char    *identity_hostname(void){ return g_hostname; }
