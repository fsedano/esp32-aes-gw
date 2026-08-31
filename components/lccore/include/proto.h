/**
  ******************************************************************************
  * @file    proto.h
  * @brief   aes-gw2 wire-protocol definitions (framing, opcodes, status,
  *          channel command layouts).
  *
  *          Ported from the STM32 sibling linecard (arinc4i4o firmware
  *          src/comm/proto.h). Ground truth is the aes-gw2 repo:
  *            - packet/packet.go  : framing + checksum (head 0xAABB, LE)
  *            - proto/proto.go    : opcodes, status codes, struct layouts
  *            - docs/WIRE_PROTOCOL.md
  *
  *          The ESP32-S3 is little-endian like the wire format, so packed
  *          structs map directly onto received payloads.
  ******************************************************************************
  */

#ifndef LCCORE_PROTO_H
#define LCCORE_PROTO_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* ====================================================================== */
/* Framing  (packet/packet.go)                                            */
/*   [ head(2,LE=0xAABB) | id(1) | size(1) | cmd(1) | payload(N) | csum(1) ] */
/*   csum = 8-bit sum of bytes [2 .. 5+N-1] (id..last payload byte).       */
/* ====================================================================== */
#define PROTO_HEAD          0xAABB          /* LE on the wire -> bytes BB AA  */
#define PROTO_HEADER_SIZE   5
#define PROTO_CSUM_SIZE     1
#define PROTO_CSUM_START    2               /* checksum coverage starts here  */
#define PROTO_MAX_PAYLOAD   250             /* size field max (0xFA)          */
#define PROTO_MAX_PACKET    (PROTO_HEADER_SIZE + PROTO_MAX_PAYLOAD + PROTO_CSUM_SIZE)

/* ====================================================================== */
/* Opcodes  (proto/proto.go)                                              */
/* ====================================================================== */
#define CMD_GET_FW_INFO         0x06
#define CMD_GET_UID             0x09
#define CMD_ARINC_RECV_LABELS   0x0B        /* dev->host, UDP                 */
#define CMD_GET_CAPABILITIES    0x0C        /* host->dev, TCP, chunked ACK    */
#define CMD_GET_HW_INFO         0x0D
#define CMD_DEVICE_STATUS       0x0E        /* dev->host, TCP periodic        */
#define CMD_FW_UPDATE           0x0F        /* host->dev, recovery build only */
#define CMD_ARINC_CHNL_SETUP    0x10
#define CMD_ARINC_CHNL_ENABLE   0x11
#define CMD_ARINC_SEND_LBL_MAN  0x12
#define CMD_ARINC_ADD_LABEL     0x13
#define CMD_ARINC_TT_BUILD      0x14
#define CMD_ARINC_TT_GET_LIST   0x15
#define CMD_ARINC_TT_UPD_LBL    0x16
#define CMD_ARINC_TT_GET_TBL    0x17
#define CMD_FIND_ME             0x22
#define CMD_LOG_MSG             0x24        /* dev->host, UDP, unsolicited    */
#define CMD_SET_LOG_LEVEL       0x25        /* host->dev, TCP, standard ACK   */
#define CMD_DISCRETE_SETUP      0x30        /* host->dev, TCP (ACKed)         */
#define CMD_DISCRETE_SET        0x31        /* host->dev, UDP (or TCP+ACK)    */
#define CMD_DISCRETE_STATE      0x32        /* dev->host, UDP                 */
#define CMD_HID_SETUP           0x33        /* host->dev, TCP (ACKed)         */
#define CMD_HID_SET             0x34        /* host->dev, UDP (or TCP+ACK)    */
#define CMD_HID_STATE           0x35        /* dev->host, UDP                 */
#define CMD_JUMP_TO_BOOTLOADER  0xAA
#define CMD_PING                0xFA
#define CMD_REBOOT              0xFB

/* ====================================================================== */
/* Status codes  (subset, proto/proto.go)                                 */
/* ====================================================================== */
#define PROTO_ST_OK                          0u
#define PROTO_ST_ERROR                       2u
#define PROTO_ST_COMM_ERR_PAYLOAD_TOO_SHORT  5u
#define PROTO_ST_ARINC_ERR_SPEED_UNKNOWN     10u
#define PROTO_ST_ARINC_ERR_CHNL_DIR_UNKNOWN  11u
#define PROTO_ST_ARINC_ERR_INVALID_CHNL_MODE 12u
#define PROTO_ST_ARINC_ERR_QUEUE_FULL        13u
#define PROTO_ST_ARINC_TX_ERR_CHNL_OOR       14u    /* channel out of range          */
#define PROTO_ST_ARINC_TX_ERR_CHNL_NOT_INIT  15u
#define PROTO_ST_ARINC_RX_ERR_CHNL_OOR       19u
#define PROTO_ST_ARINC_RX_ERR_CHNL_NOT_INIT  21u
#define PROTO_ST_CAPS_ERR_VERSION_UNSUPPORTED 70u

/* ====================================================================== */
/* Channel enums  (proto/proto.go)                                        */
/* ====================================================================== */
#define DIR_TX              0
#define DIR_RX              1

#define SPEED_12KHZ         0
#define SPEED_100KHZ        1
#define SPEED_50KHZ         2

#define MODE_TX_TIMETABLE   0
#define MODE_TX_MANUAL      1
#define MODE_RX_NORMAL      2
#define MODE_RX_RAW         3

/* Number of ARINC channels per direction on this card (4 in / 4 out, same
   validation convention as the STM32 sibling: chnl >= N is out of range). */
#define ARINC_NUM_CHANNELS  4

/* ====================================================================== */
/* Command payload layouts                                                */
/* ====================================================================== */

/* ARINC_CHNL_SETUP (0x10), 6 bytes <BBBBH>.
   A payload may carry several of these records back to back.            */
typedef struct __attribute__((packed)) {
    uint8_t  chnl;
    uint8_t  direction;
    uint8_t  speed;
    uint8_t  mode;
    uint16_t config_delay_us;       /* only meaningful for TX manual mode */
} proto_chnl_setup_t;

/* ARINC_CHNL_ENABLE (0x11), 3 bytes <BBB>.
   is_enable: 0 = disable, 1 = enable.                                    */
typedef struct __attribute__((packed)) {
    uint8_t  chnl;
    uint8_t  direction;
    uint8_t  is_enable;
} proto_chnl_enable_t;

/* ====================================================================== */
/* Log channel  (WIRE_PROTOCOL.md §10)                                    */
/* ====================================================================== */
#define PROTO_LOG_LEVEL_DEBUG      0
#define PROTO_LOG_LEVEL_INFO       1

#define PROTO_LOG_HDR_SIZE         4        /* seq + dropped                */
#define PROTO_LOG_REC_OVERHEAD     6        /* flags + tick_ms + text_len   */
#define PROTO_LOG_MAX_TEXT         122
#define PROTO_LOG_FLAG_LEVEL_MASK  0x0F
#define PROTO_LOG_FLAG_TRUNCATED   0x80

/* DISCRETE_SETUP (0x30), 4 bytes <BBH>: enable/disable the discrete subsystem
   and set the STATE heartbeat period. flags is reserved (0). report_ms 0 ->
   default (500). TCP only, ACKed. */
typedef struct __attribute__((packed)) {
    uint8_t  enable;
    uint8_t  flags;
    uint16_t report_ms;
} proto_discrete_setup_t;

/* DISCRETE_SET (0x31): a 6-byte range header followed by two
   ceil(bit_count/8)-byte bitmaps (apply, values). Bit i addresses
   base_channel+i. Idempotent; UDP fire-and-forget or TCP with ACK. */
typedef struct __attribute__((packed)) {
    uint32_t base_channel;
    uint16_t bit_count;
} proto_discrete_set_header_t;

/* DISCRETE_STATE (0x32): an 8-byte range header followed by three
   ceil(bit_count/8)-byte bitmaps (relay_state, input_state, input_valid).
   Device->host over UDP on change and every report_ms. */
typedef struct __attribute__((packed)) {
    uint32_t base_channel;
    uint16_t bit_count;
    uint8_t  link;
    uint8_t  seq;
} proto_discrete_state_header_t;

#define PROTO_DISCRETE_SET_HEADER_SIZE    6u
#define PROTO_DISCRETE_STATE_HEADER_SIZE  8u
#define PROTO_DISCRETE_BITMAP_BYTES(n)    (((uint16_t)(n) + 7u) / 8u)

_Static_assert(sizeof(proto_discrete_set_header_t) ==
               PROTO_DISCRETE_SET_HEADER_SIZE,
               "DISCRETE_SET range header layout drift");
_Static_assert(sizeof(proto_discrete_state_header_t) ==
               PROTO_DISCRETE_STATE_HEADER_SIZE,
               "DISCRETE_STATE range header layout drift");

/* USB HID joystick channel group (8 axes + 32 buttons), wire doc §12.
   The card presents a HID gamepad to the PC on its USB port; the host
   drives axis/button values over these commands. */
#define HID_NUM_AXES        8
#define HID_NUM_BUTTONS     32

/* HID_SETUP (0x33), 4 bytes <BBH>: enable/disable the HID subsystem and set
   the STATE heartbeat period. flags is reserved (0). report_ms 0 ->
   default (500). TCP only, ACKed. */
typedef struct __attribute__((packed)) {
    uint8_t  enable;
    uint8_t  flags;
    uint16_t report_ms;
} proto_hid_setup_t;

/* HID_SET (0x34), 25 bytes <B 8h I I>: axis i is written iff bit i of
   axis_mask is set (axes int16, -32767..32767, center 0); buttons merge
   desired = (desired & ~btn_apply_mask) | (btn_values & btn_apply_mask).
   Idempotent; UDP fire-and-forget or TCP with ACK. */
typedef struct __attribute__((packed)) {
    uint8_t  axis_mask;
    int16_t  axes[HID_NUM_AXES];
    uint32_t btn_apply_mask;
    uint32_t btn_values;
} proto_hid_set_t;

/* HID_STATE (0x35), 22 bytes <8h I B B>: dev->host over UDP on any change of
   axes/buttons/usb_mounted and every report_ms. usb_mounted reflects whether
   a USB host has the joystick enumerated and not suspended — it is a link
   indicator only; SETs latch and confirm regardless. */
typedef struct __attribute__((packed)) {
    int16_t  axes[HID_NUM_AXES];
    uint32_t buttons;
    uint8_t  usb_mounted;
    uint8_t  seq;
} proto_hid_state_t;

_Static_assert(sizeof(proto_hid_setup_t) == 4,  "HID_SETUP layout");
_Static_assert(sizeof(proto_hid_set_t)   == 25, "HID_SET layout");
_Static_assert(sizeof(proto_hid_state_t) == 22, "HID_STATE layout");

/* DEVICE_STATUS (0x0E), 36 bytes <9I>: dev->host over TCP, ~1 Hz. */
typedef struct __attribute__((packed)) {
    uint32_t snd_pps;
    uint32_t rcv_pps;
    uint32_t snd_bps;
    uint32_t rcv_bps;
    uint32_t snd_queue_ovf;
    uint32_t rcv_queue_ovf;
    uint32_t checksum_err;
    uint32_t head_err;
    uint32_t packet_loss;
} proto_device_status_t;

#ifdef __cplusplus
}
#endif

#endif /* LCCORE_PROTO_H */
