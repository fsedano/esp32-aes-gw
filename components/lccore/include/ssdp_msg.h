/**
  ******************************************************************************
  * @file    ssdp_msg.h
  * @brief   SSDP / UPnP message builders, byte-for-byte compatible with the
  *          STM32 sibling's ssdp.c (which is what the gateway and aviologic
  *          are proven to parse).
  *
  *          The builders are pure string generators so they compile on the
  *          host for golden-byte unit tests; the sockets live in main/.
  ******************************************************************************
  */

#ifndef LCCORE_SSDP_MSG_H
#define LCCORE_SSDP_MSG_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

/* Identity snapshot consumed by the builders. All strings NUL-terminated. */
typedef struct {
    const char *uuid;        /* 24-char USN uuid                       */
    const char *ip;          /* dotted-quad device IP                  */
    const char *board_id;    /* SSDP SERVER first token (BOARD_INFO_SHORT_ID) */
    const char *fw_tag;      /* version string after the '/'           */
    const char *serial;      /* description.xml <serialNumber>         */
    uint16_t    http_port;   /* description.xml port (80 on target)    */
} ssdp_ident_t;

/* NOTIFY ssdp:alive (multicast). Returns bytes written (no NUL), 0 if cap
   is too small. */
size_t ssdp_build_notify(char *buf, size_t cap, const ssdp_ident_t *id);

/* Unicast M-SEARCH 200 OK response. */
size_t ssdp_build_msearch_reply(char *buf, size_t cap, const ssdp_ident_t *id);

/* Full HTTP response carrying the UPnP description.xml document. */
size_t ssdp_build_description(char *buf, size_t cap, const ssdp_ident_t *id);

/* True when the datagram is an M-SEARCH request (the only method we answer;
   the STM32 firmware answers every M-SEARCH regardless of its ST). */
int ssdp_is_msearch(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* LCCORE_SSDP_MSG_H */
