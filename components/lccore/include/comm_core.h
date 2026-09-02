/**
  ******************************************************************************
  * @file    comm_core.h
  * @brief   Transport-independent aes-gw2 wire-protocol control plane.
  *
  *          Port of arinc4i4o's comm.c with the WIZnet socket service moved
  *          behind a callback seam so the same dispatcher runs on ESP-IDF
  *          (lwip BSD sockets, main/comm_task.c) and on the host (unit
  *          tests). The core owns:
  *            - frame parsing + one-byte resync (comm_scan)
  *            - the shared outgoing packet_id counter
  *            - the ACK envelope and every command handler
  *            - per-channel ARINC state (via arinc_glue) and the discrete
  *              seam (discrete_glue)
  *
  *          The transport owns the sockets, the single-client TCP policy and
  *          the host-UDP-endpoint latch; it feeds received bytes in through
  *          comm_core_input() and implements the send callbacks.
  ******************************************************************************
  */

#ifndef LCCORE_COMM_CORE_H
#define LCCORE_COMM_CORE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

/* Transport tag for received packets. */
#define COMM_SRC_TCP    0
#define COMM_SRC_UDP    1

typedef struct {
    /* Write one fully framed packet to the TCP control client. Must be a
       no-op when no client is connected. */
    void (*send_tcp)(const uint8_t *frame, uint16_t len);
    /* Send one fully framed packet to the latched host UDP endpoint.
       Returns false when no endpoint is latched / send failed. */
    bool (*send_udp)(const uint8_t *frame, uint16_t len);
    /* JUMP_TO_BOOTLOADER accepted (ACK already queued): switch the boot
       partition to the recovery app and reset. NULL in the recovery build
       (already in bootloader mode: the command just ACKs OK). */
    void (*enter_bootloader)(void);
    /* REBOOT accepted (ACK already queued): plain reset. */
    void (*reboot)(void);
    /* FIND_ME accepted: blink the locator LED (or log) for duration_s. */
    void (*find_me)(uint32_t duration_s);
} comm_ops_t;

/* One-time init. Resets channel state, packet ids and the glue stubs. */
void comm_core_init(const comm_ops_t *ops);

/* TCP control session lifecycle (transport calls these on connect (true) /
   disconnect (false)): both reset all channels; disconnect also resets the
   log threshold to INFO (wire doc §10.1). */
void comm_core_session(bool connected);

/* Parse buf[0..len) (one-byte resync on bad head/checksum, stop at the first
   incomplete packet) and dispatch every whole packet. Returns bytes consumed
   so a TCP caller can compact its reassembly buffer. UDP callers pass whole
   datagrams and may ignore the return value. */
uint16_t comm_core_input(uint8_t *buf, uint16_t len, uint8_t src);

/* Build a framed packet [BB AA | id | size | cmd | payload | csum] into pkt
   (PROTO_MAX_PACKET bytes) using the shared outgoing id counter. Returns the
   total frame length, or 0 for invalid pointers or an oversized payload. */
uint16_t comm_core_build_frame(uint8_t *pkt, uint8_t cmd,
                               const uint8_t *payload, uint8_t len);

/* Frame + send a dev->host packet on the given plane. The UDP variant
   returns false when the host endpoint is not latched yet. */
void comm_core_send_tcp(uint8_t cmd, const uint8_t *payload, uint8_t len);
bool comm_core_send_udp(uint8_t cmd, const uint8_t *payload, uint8_t len);

/* GET_FW_INFO / GET_HW_INFO ACK extra payloads (53 bytes each), exposed for
   the host tests. identity_init() must have run for hw_info. */
void comm_core_pack_fw_info(uint8_t out[53]);
void comm_core_pack_hw_info(uint8_t out[53]);

/* True while any ARINC channel is enabled. */
bool comm_core_any_channel_enabled(void);

#ifdef __cplusplus
}
#endif

#endif /* LCCORE_COMM_CORE_H */
