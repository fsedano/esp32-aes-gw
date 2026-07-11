/**
  ******************************************************************************
  * @file    comm_capture.h
  * @brief   Capture comm_ops_t for driving comm_core in host tests: records
  *          every dev->host frame and every side-effect callback.
  ******************************************************************************
  */

#ifndef COMM_CAPTURE_H
#define COMM_CAPTURE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "comm_core.h"

#define CAP_MAX_FRAMES  32
#define CAP_BUF_SIZE    4096

typedef struct {
    uint8_t  data[CAP_BUF_SIZE];
    size_t   len;
    /* Offsets/lengths of each captured frame within data[]. */
    size_t   frame_off[CAP_MAX_FRAMES];
    uint16_t frame_len[CAP_MAX_FRAMES];
    int      frames;
} capture_t;

extern capture_t cap_tcp;
extern capture_t cap_udp;

/* Side-effect recorders. */
extern int      cap_bootloader_calls;
extern int      cap_reboot_calls;
extern int      cap_find_me_calls;
extern uint32_t cap_find_me_duration;
/* When false, the UDP send callback reports failure (host not latched). */
extern bool     cap_udp_enabled;

void cap_reset(void);
const comm_ops_t *cap_ops(void);

/* Validate frame `idx` in `c` (head, checksum) and return its payload
   pointer; *cmd / *plen / *pkt_id are filled. Returns NULL when invalid. */
const uint8_t *cap_frame(const capture_t *c, int idx,
                         uint8_t *cmd, uint8_t *plen, uint8_t *pkt_id);

/* Build a host->dev request frame with an explicit packet_id. Returns the
   total frame length (out must hold PROTO_MAX_PACKET bytes). */
uint16_t cap_build_req(uint8_t *out, uint8_t pkt_id, uint8_t cmd,
                       const uint8_t *payload, uint8_t len);

#endif /* COMM_CAPTURE_H */
