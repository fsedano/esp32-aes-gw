/**
  ******************************************************************************
  * @file    identity.h
  * @brief   Stable board identity derived from the eFuse base MAC.
  *
  *          The STM32 sibling derives MAC/serial/hostname/UUID from its 96-bit
  *          hardware UID; the ESP32-S3 has no such UID, so the derivation is
  *          inverted: the factory-programmed eFuse MAC (6 bytes, globally
  *          unique) is expanded into a deterministic 12-byte UID, and every
  *          other identity artifact (24-char SSDP USN UUID, 12-hex-char
  *          serial, DHCP hostname) is derived from that UID exactly like
  *          arinc4i4o's board_info.c derives them from the STM32 UID.
  *
  *          The UID is also the authorization key for JUMP_TO_BOOTLOADER and
  *          REBOOT, so the derivation must never change between firmware
  *          versions (app and recovery must agree).
  ******************************************************************************
  */

#ifndef LCCORE_IDENTITY_H
#define LCCORE_IDENTITY_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

typedef uint8_t identity_uid_t[12];
typedef uint8_t identity_mac_t[6];
typedef char    identity_serial_t[13];    /* 12 hex chars + NUL */
typedef char    identity_uuid_t[25];      /* 24 hex chars + NUL */
typedef char    identity_hostname_t[16];  /* "A429E-" + 8 hex + NUL */

/* Seed the identity from the 6-byte base MAC (eFuse ETH MAC on target, a
   fixed test vector in host tests). Must be called before any getter. */
void identity_init(const uint8_t mac[6]);

const uint8_t *identity_uid(void);        /* 12 bytes                       */
const uint8_t *identity_mac(void);        /* the MAC passed to init         */
const char    *identity_uuid(void);       /* 24-char lowercase hex of UID   */
const char    *identity_serial(void);     /* 12 lowercase hex chars (UID[0..5]) */
const char    *identity_hostname(void);   /* BOARD_HOSTNAME_PREFIX + serial[0..7] */

#ifdef __cplusplus
}
#endif

#endif /* LCCORE_IDENTITY_H */
