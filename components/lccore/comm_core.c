/**
  ******************************************************************************
  * @file    comm_core.c
  * @brief   aes-gw2 wire-protocol control plane (see comm_core.h).
  *
  *          Ported from arinc4i4o firmware src/comm/comm.c. Handler logic,
  *          validation order, ACK payloads and log lines follow the STM32
  *          sibling (and aes-gw2's fakelc simulator) on the wire, with two
  *          deliberate divergences, both unreachable via aes-gw2:
  *            - ADD_LABEL / TT_UPD_LBL / TT_BUILD on a channel that was
  *              never CHNL_SETUP return CHNL_NOT_INIT on all 4 channels,
  *              where the STM32 only routes ch0 through its TX engine and
  *              blind-ACKs OK on ch1-3.
  *            - ARINC config/query commands are TCP-only here (dropped
  *              without an ACK on UDP, like DISCRETE_SETUP); the gateway
  *              only ever configures over TCP.
  *
  *          Build personalities:
  *            - application (default): full ARINC + discrete command set;
  *              FW_UPDATE answers ERROR (not in bootloader mode).
  *            - RECOVERY_BUILD: the bootloader personality (arinc4i4o
  *              boot_comm.c): info/UID/PING/FW_UPDATE/JUMP/REBOOT only,
  *              everything else answers ERROR.
  ******************************************************************************
  */

#include "comm_core.h"
#include "proto.h"
#include "board_id.h"
#include "capabilities.h"
#include "identity.h"
#include "arinc_glue.h"
#include "discrete_glue.h"
#include "hid_glue.h"
#include "lc_log.h"

#ifdef RECOVERY_BUILD
#include "fwupdate_core.h"
#endif

#include <string.h>

/* ---- per-channel state, indexed [direction][channel] ------------------- */
typedef struct {
    bool     setup;         /* CHNL_SETUP received (initialised)  */
    bool     enabled;       /* CHNL_ENABLE is_enable flag         */
    uint8_t  speed;
    uint8_t  mode;
    uint16_t delay_us;
} chnl_state_t;

static chnl_state_t g_chnl[2][ARINC_NUM_CHANNELS];

static const comm_ops_t *g_ops;
static uint8_t g_tx_id;                 /* our outgoing packet counter */

/* ====================================================================== */
/* Channel state                                                          */
/* ====================================================================== */

static void channels_reset(void){
    memset(g_chnl, 0, sizeof(g_chnl));
    atx_glue_reset();       /* clear the (stub) TX engine state           */
    discrete_glue_reset();  /* disable discrete subsystem -> relays off   */
#ifndef RECOVERY_BUILD
    hid_glue_reset();       /* center axes, release buttons on USB        */
#endif
}

bool comm_core_any_channel_enabled(void){
    for(int d = 0; d < 2; d++){
        for(int c = 0; c < ARINC_NUM_CHANNELS; c++){
            if(g_chnl[d][c].enabled){
                return true;
            }
        }
    }
    return false;
}

/* ====================================================================== */
/* Outgoing packets                                                       */
/* ====================================================================== */

uint16_t comm_core_build_frame(uint8_t *pkt, uint8_t cmd,
                               const uint8_t *payload, uint8_t len){
    if(pkt == NULL || len > PROTO_MAX_PAYLOAD
       || (len > 0u && payload == NULL)){
        return 0;
    }
    pkt[0] = (uint8_t)(PROTO_HEAD & 0xFF);          /* 0xBB */
    pkt[1] = (uint8_t)((PROTO_HEAD >> 8) & 0xFF);   /* 0xAA */
    pkt[2] = g_tx_id++;
    pkt[3] = len;
    pkt[4] = cmd;
    if(len && payload){
        memcpy(pkt + PROTO_HEADER_SIZE, payload, len);
    }

    uint8_t csum = 0;
    for(uint16_t i = PROTO_CSUM_START; i < (uint16_t)(PROTO_HEADER_SIZE + len); i++){
        csum += pkt[i];
    }
    pkt[PROTO_HEADER_SIZE + len] = csum;

    return (uint16_t)(PROTO_HEADER_SIZE + len + PROTO_CSUM_SIZE);
}

void comm_core_send_tcp(uint8_t cmd, const uint8_t *payload, uint8_t len){
    if(len > PROTO_MAX_PAYLOAD || g_ops == NULL || g_ops->send_tcp == NULL){
        return;
    }
    uint8_t pkt[PROTO_MAX_PACKET];
    uint16_t total = comm_core_build_frame(pkt, cmd, payload, len);
    if(total > 0u){
        g_ops->send_tcp(pkt, total);
    }
}

/* CRITICAL: never log from here — this is also the log-flush path. */
bool comm_core_send_udp(uint8_t cmd, const uint8_t *payload, uint8_t len){
    if(len > PROTO_MAX_PAYLOAD || g_ops == NULL || g_ops->send_udp == NULL){
        return false;
    }
    uint8_t pkt[PROTO_MAX_PACKET];
    uint16_t total = comm_core_build_frame(pkt, cmd, payload, len);
    return total > 0u && g_ops->send_udp(pkt, total);
}

/* ACK envelope byte counts: [req_id | status(LE32)] + up to 244 extra bytes.
   GET_CAPABILITIES uses 242 (u16 total + a 240-byte descriptor chunk). */
#define COMM_ACK_ENVELOPE_LEN   5u
#define COMM_INFO_BLOCK_LEN     53u
#define COMM_ACK_EXTRA_MAX      (PROTO_MAX_PAYLOAD - COMM_ACK_ENVELOPE_LEN)
_Static_assert(COMM_ACK_EXTRA_MAX >= (2u + CAPS_CHUNK_MAX),
               "capability chunk must fit one ACK");

/* Standard ACK envelope: [req_id | status(LE32) | extra]. */
static void comm_send_ack(uint8_t cmd, uint8_t req_id, uint32_t status,
                          const uint8_t *extra, uint8_t extra_len){
    uint8_t payload[PROTO_MAX_PAYLOAD];
    if(extra_len > COMM_ACK_EXTRA_MAX){
        extra_len = COMM_ACK_EXTRA_MAX;
    }
    payload[0] = req_id;
    payload[1] = (uint8_t)(status);
    payload[2] = (uint8_t)(status >> 8);
    payload[3] = (uint8_t)(status >> 16);
    payload[4] = (uint8_t)(status >> 24);
    if(extra_len && extra){
        memcpy(payload + 5, extra, extra_len);
    }
    comm_core_send_tcp(cmd, payload, (uint8_t)(5 + extra_len));
}

/* Copy a C string into a fixed, NUL-padded field. */
static void str_field(uint8_t *dst, const char *src, uint8_t n){
    uint8_t i = 0;
    for(; i < n && src[i]; i++){ dst[i] = (uint8_t)src[i]; }
    for(; i < n; i++){ dst[i] = 0; }
}

/* ====================================================================== */
/* Handshake / info commands                                              */
/* ====================================================================== */

/* GET_FW_INFO ACK extra (53 bytes, proto.go PackFwInfo / UnpackFwInfo). */
void comm_core_pack_fw_info(uint8_t out[53]){
    memset(out, 0, COMM_INFO_BLOCK_LEN);
    /* bytes 0..5: build date Y,M,D,H,M,S (no RTC -> fixed, like the STM32) */
    out[0] = 26; out[1] = 7; out[2] = 1;
    str_field(out + 6,  VERSION_COMMIT, 8);         /* hash    */
    str_field(out + 14, BOARD_INFO_FW_BRANCH, 14);  /* branch  */
    str_field(out + 28, BOARD_INFO_FW_TAG, 24);     /* version */
    out[52] = BOARD_INFO_FW_TYPE;                   /* 0 app / 1 bootloader */
}

static void handle_get_fw_info(uint8_t req_id){
    uint8_t fw[COMM_INFO_BLOCK_LEN];
    comm_core_pack_fw_info(fw);
    comm_send_ack(CMD_GET_FW_INFO, req_id, PROTO_ST_OK, fw, sizeof(fw));
}

/* GET_HW_INFO ACK extra (53 bytes, proto.go PackHwInfo). */
void comm_core_pack_hw_info(uint8_t out[53]){
    memset(out, 0, COMM_INFO_BLOCK_LEN);
    out[0] = 1; out[1] = 0; out[2] = 0;        /* hw major.minor.fix */
    out[3] = 1; out[4] = 7; out[5] = 26;       /* day, month, year   */
    str_field(out + 6, BOARD_INFO_SHORT_ID, 16);
    memcpy(out + 22, identity_mac(), 6);
    memcpy(out + 28, identity_uid(), 12);
    str_field(out + 40, identity_serial(), 13);
}

static void handle_get_hw_info(uint8_t req_id){
    uint8_t hw[COMM_INFO_BLOCK_LEN];
    comm_core_pack_hw_info(hw);
    comm_send_ack(CMD_GET_HW_INFO, req_id, PROTO_ST_OK, hw, sizeof(hw));
}

/* GET_UID is legacy: reply with the raw 12-byte UID under cmd 0x09,
   NOT an ACK envelope. */
static void handle_get_uid(void){
    comm_core_send_tcp(CMD_GET_UID, identity_uid(), 12);
}

#ifndef RECOVERY_BUILD
/* GET_CAPABILITIES (0x26): stateless offset-based reads of the immutable
   boot descriptor. The 2-byte total length is repeated in every ACK. */
static void handle_get_capabilities(uint8_t req_id, const uint8_t *payload,
                                    uint8_t len){
    if(len < 3u){
        comm_send_ack(CMD_GET_CAPABILITIES, req_id,
                      PROTO_ST_COMM_ERR_PAYLOAD_TOO_SHORT, NULL, 0);
        return;
    }
    if(!capabilities_available()){
        comm_send_ack(CMD_GET_CAPABILITIES, req_id, PROTO_ST_ERROR, NULL, 0);
        return;
    }

    uint8_t version = payload[0];
    if(version != CAPS_DESC_VERSION){
        uint8_t supported[2] = { CAPS_DESC_VERSION, CAPS_DESC_VERSION };
        LOG_INF("caps: requested v%u unsupported; available v%u",
                version, CAPS_DESC_VERSION);
        comm_send_ack(CMD_GET_CAPABILITIES, req_id,
                      PROTO_ST_CAPS_ERR_VERSION_UNSUPPORTED,
                      supported, sizeof(supported));
        return;
    }

    uint16_t offset = (uint16_t)payload[1] | ((uint16_t)payload[2] << 8);
    uint16_t total_len = 0;
    const uint8_t *blob = capabilities_blob(&total_len);
    if(blob == NULL || offset > total_len){
        comm_send_ack(CMD_GET_CAPABILITIES, req_id, PROTO_ST_ERROR, NULL, 0);
        return;
    }

    uint16_t chunk_len = (uint16_t)(total_len - offset);
    if(chunk_len > CAPS_CHUNK_MAX){
        chunk_len = CAPS_CHUNK_MAX;
    }
    if(offset == 0u){
        capabilities_log_summary();
    }
    uint8_t extra[2u + CAPS_CHUNK_MAX];
    extra[0] = (uint8_t)total_len;
    extra[1] = (uint8_t)(total_len >> 8);
    if(chunk_len > 0){
        memcpy(extra + 2, blob + offset, chunk_len);
    }
    comm_send_ack(CMD_GET_CAPABILITIES, req_id, PROTO_ST_OK, extra,
                  (uint8_t)(chunk_len + 2u));
}
#endif

/* ====================================================================== */
/* Bootloader entry / reboot                                              */
/* ====================================================================== */

/* Both commands carry the 12-byte UID as an authorization key. */
static bool uid_key_matches(const uint8_t *payload, uint8_t len){
    if(len < 12){
        return false;
    }
    return memcmp(payload, identity_uid(), 12) == 0;
}

/* JUMP_TO_BOOTLOADER (0xAA): ACK, then hand off to the transport to switch
   the boot partition to the recovery app and reset. In the recovery build
   (ops->enter_bootloader == NULL) we are already the bootloader: just ACK
   OK, like arinc4i4o's boot_comm.c. */
static void handle_jmp_bootloader(uint8_t req_id, const uint8_t *payload, uint8_t len){
    if(!uid_key_matches(payload, len)){
        comm_send_ack(CMD_JUMP_TO_BOOTLOADER, req_id, PROTO_ST_ERROR, NULL, 0);
        return;
    }
    comm_send_ack(CMD_JUMP_TO_BOOTLOADER, req_id, PROTO_ST_OK, NULL, 0);
    if(g_ops->enter_bootloader != NULL){
        LOG_INF("comm: reboot into bootloader");
        g_ops->enter_bootloader();
    }
}

/* REBOOT (0xFB): plain reset. */
static void handle_reboot(uint8_t req_id, const uint8_t *payload, uint8_t len){
    if(!uid_key_matches(payload, len)){
        comm_send_ack(CMD_REBOOT, req_id, PROTO_ST_ERROR, NULL, 0);
        return;
    }
    comm_send_ack(CMD_REBOOT, req_id, PROTO_ST_OK, NULL, 0);
    LOG_INF("comm: reboot");
    if(g_ops->reboot != NULL){
        g_ops->reboot();
    }
}

/* ====================================================================== */
/* FIND_ME                                                                */
/* ====================================================================== */

/* FIND_ME (0x22): <I> duration_seconds. Blink the locator LED (or log). */
static void handle_find_me(uint8_t req_id, const uint8_t *payload, uint8_t len){
    if(len < 4){
        comm_send_ack(CMD_FIND_ME, req_id, PROTO_ST_COMM_ERR_PAYLOAD_TOO_SHORT, NULL, 0);
        return;
    }
    uint32_t duration = (uint32_t)payload[0]
                      | ((uint32_t)payload[1] << 8)
                      | ((uint32_t)payload[2] << 16)
                      | ((uint32_t)payload[3] << 24);
    comm_send_ack(CMD_FIND_ME, req_id, PROTO_ST_OK, NULL, 0);
    LOG_INF("comm: find_me for %us", (unsigned)duration);
    if(g_ops->find_me != NULL){
        g_ops->find_me(duration);
    }
}

#ifndef RECOVERY_BUILD

/* ====================================================================== */
/* Channel commands (application personality only)                        */
/* ====================================================================== */
static uint32_t apply_chnl_setup(const proto_chnl_setup_t *c){
    if(c->direction > DIR_RX){
        return PROTO_ST_ARINC_ERR_CHNL_DIR_UNKNOWN;
    }
    if(c->chnl >= ARINC_NUM_CHANNELS){
        return (c->direction == DIR_TX) ? PROTO_ST_ARINC_TX_ERR_CHNL_OOR
                                        : PROTO_ST_ARINC_RX_ERR_CHNL_OOR;
    }
    if(c->speed > SPEED_50KHZ){
        return PROTO_ST_ARINC_ERR_SPEED_UNKNOWN;
    }
    if(c->mode > MODE_RX_RAW){
        return PROTO_ST_ARINC_ERR_INVALID_CHNL_MODE;
    }
    chnl_state_t *s = &g_chnl[c->direction][c->chnl];
    s->setup    = true;
    s->enabled  = false;        /* setup (re)configures -> must re-enable */
    s->speed    = c->speed;
    s->mode     = c->mode;
    s->delay_us = c->config_delay_us;

    if(c->direction == DIR_TX){
        uint32_t r = atx_glue_setup(c->chnl, c->speed, c->mode, c->config_delay_us);
        if(r != PROTO_ST_OK){
            return r;
        }
    }
    /* RX channels: accepted; the RX hardware layer is a stub (no labels
       are produced yet). */
    return PROTO_ST_OK;
}

static uint32_t apply_chnl_enable(const proto_chnl_enable_t *c){
    if(c->direction > DIR_RX){
        return PROTO_ST_ARINC_ERR_CHNL_DIR_UNKNOWN;
    }
    if(c->chnl >= ARINC_NUM_CHANNELS){
        return (c->direction == DIR_TX) ? PROTO_ST_ARINC_TX_ERR_CHNL_OOR
                                        : PROTO_ST_ARINC_RX_ERR_CHNL_OOR;
    }
    chnl_state_t *s = &g_chnl[c->direction][c->chnl];
    if(!s->setup){
        return (c->direction == DIR_TX) ? PROTO_ST_ARINC_TX_ERR_CHNL_NOT_INIT
                                        : PROTO_ST_ARINC_RX_ERR_CHNL_NOT_INIT;
    }
    s->enabled = (c->is_enable != 0);
    if(c->direction == DIR_TX){
        uint32_t r = atx_glue_enable(c->chnl, s->enabled);
        if(r != PROTO_ST_OK){
            return r;
        }
    }
    return PROTO_ST_OK;
}

/* Human-readable enum names for the important-event log lines. */
static const char *dir_name(uint8_t dir){
    return dir == DIR_RX ? "RX" : "TX";
}
static const char *speed_name(uint8_t speed){
    switch(speed){
        case SPEED_12KHZ:  return "12.5kHz";
        case SPEED_100KHZ: return "100kHz";
        case SPEED_50KHZ:  return "50kHz";
        default:           return "?";
    }
}
static const char *mode_name(uint8_t mode){
    switch(mode){
        case MODE_TX_TIMETABLE: return "timetable";
        case MODE_TX_MANUAL:    return "manual";
        case MODE_RX_NORMAL:    return "normal";
        case MODE_RX_RAW:       return "raw";
        default:                return "?";
    }
}

/* Both channel commands accept one or more fixed-size records in a single
   payload; the ACK reports the status of the first failing record (or OK)
   plus the channel/direction it referred to. */
static void handle_chnl_setup(uint8_t req_id, const uint8_t *payload, uint8_t len){
    uint32_t status = PROTO_ST_OK;
    uint8_t  last_chnl = 0, last_dir = 0;
    uint8_t  off = 0;

    if(len < sizeof(proto_chnl_setup_t)){
        status = PROTO_ST_ERROR;
    }
    while((uint16_t)(off + sizeof(proto_chnl_setup_t)) <= len){
        const proto_chnl_setup_t *c = (const proto_chnl_setup_t *)(payload + off);
        last_chnl = c->chnl;
        last_dir  = c->direction;
        status = apply_chnl_setup(c);
        off = (uint8_t)(off + sizeof(proto_chnl_setup_t));
        if(status != PROTO_ST_OK){
            LOG_INF("comm: ch%u %s setup rejected (status=%lu)",
                    c->chnl, dir_name(c->direction), (unsigned long)status);
            break;
        }
        LOG_INF("comm: ch%u %s setup: %s, mode %s",
                c->chnl, dir_name(c->direction),
                speed_name(c->speed), mode_name(c->mode));
    }

    uint8_t extra[2] = { last_chnl, last_dir };
    comm_send_ack(CMD_ARINC_CHNL_SETUP, req_id, status, extra, sizeof(extra));
}

static void handle_chnl_enable(uint8_t req_id, const uint8_t *payload, uint8_t len){
    uint32_t status = PROTO_ST_OK;
    uint8_t  last_chnl = 0, last_dir = 0;
    uint8_t  off = 0;

    if(len < sizeof(proto_chnl_enable_t)){
        status = PROTO_ST_ERROR;
    }
    while((uint16_t)(off + sizeof(proto_chnl_enable_t)) <= len){
        const proto_chnl_enable_t *c = (const proto_chnl_enable_t *)(payload + off);
        last_chnl = c->chnl;
        last_dir  = c->direction;
        status = apply_chnl_enable(c);
        off = (uint8_t)(off + sizeof(proto_chnl_enable_t));
        if(status != PROTO_ST_OK){
            LOG_INF("comm: ch%u %s %s rejected (status=%lu)",
                    c->chnl, dir_name(c->direction),
                    c->is_enable ? "enable" : "disable", (unsigned long)status);
            break;
        }
        LOG_INF("comm: ch%u %s %s", c->chnl, dir_name(c->direction),
                c->is_enable ? "enabled" : "disabled");
    }

    uint8_t extra[2] = { last_chnl, last_dir };
    comm_send_ack(CMD_ARINC_CHNL_ENABLE, req_id, status, extra, sizeof(extra));
}

/* ARINC_SEND_LABEL_MANUAL (0x12): items <B I>; ACK only on TCP (§4.12). */
#define SEND_LBL_MAN_ITEM_SIZE  (1u + 4u)

static void handle_send_label_manual(uint8_t req_id, const uint8_t *payload,
                                     uint8_t len, uint8_t src){
    uint32_t status    = PROTO_ST_OK;
    uint8_t  last_chnl = 0;
    uint8_t  off       = 0;

    if(len < SEND_LBL_MAN_ITEM_SIZE){
        status = PROTO_ST_ERROR;
    }
    while((uint16_t)(off + SEND_LBL_MAN_ITEM_SIZE) <= len){
        uint8_t  ch   = payload[off];
        uint32_t word = (uint32_t)payload[off + 1]
                      | ((uint32_t)payload[off + 2] << 8)
                      | ((uint32_t)payload[off + 3] << 16)
                      | ((uint32_t)payload[off + 4] << 24);
        last_chnl = ch;
        off = (uint8_t)(off + SEND_LBL_MAN_ITEM_SIZE);

        if(ch >= ARINC_NUM_CHANNELS){
            status = PROTO_ST_ARINC_TX_ERR_CHNL_OOR;
            break;
        }
        chnl_state_t *s = &g_chnl[DIR_TX][ch];
        if(!s->setup){
            status = PROTO_ST_ARINC_TX_ERR_CHNL_NOT_INIT;
            break;
        }
        uint32_t r = atx_glue_set_label_manual(ch, word);
        if(r != PROTO_ST_OK){
            status = r;
            break;
        }
    }

    if(src == COMM_SRC_TCP){
        uint8_t extra[2] = { last_chnl, DIR_TX };
        comm_send_ack(CMD_ARINC_SEND_LBL_MAN, req_id, status, extra, sizeof(extra));
    }
}

/* Read a little-endian float from a (possibly unaligned) payload offset. */
static float le_float(const uint8_t *p){
    float f;
    memcpy(&f, p, sizeof(f));    /* ESP32-S3 is little-endian, matching the wire */
    return f;
}

/* ARINC_ADD_LABEL (0x13): items <B H f f>. */
#define ADD_LABEL_ITEM_SIZE  (1u + 2u + 4u + 4u)

static void handle_add_label(uint8_t req_id, const uint8_t *payload, uint8_t len){
    uint32_t status    = PROTO_ST_OK;
    uint8_t  last_chnl = 0;
    uint8_t  off       = 0;

    if(len < ADD_LABEL_ITEM_SIZE){
        status = PROTO_ST_ERROR;
    }
    while((uint16_t)(off + ADD_LABEL_ITEM_SIZE) <= len){
        uint8_t  ch    = payload[off];
        uint16_t lblno = (uint16_t)payload[off + 1] | ((uint16_t)payload[off + 2] << 8);
        float    tmax  = le_float(payload + off + 3);
        float    tmin  = le_float(payload + off + 7);
        last_chnl = ch;
        off = (uint8_t)(off + ADD_LABEL_ITEM_SIZE);

        if(ch >= ARINC_NUM_CHANNELS){
            status = PROTO_ST_ARINC_TX_ERR_CHNL_OOR;
            break;
        }
        uint32_t r = atx_glue_add_label(ch, lblno, tmax, tmin);
        if(r != PROTO_ST_OK){
            status = r;
            LOG_INF("comm: add_label ch=%u lbl=0%03o rejected (status=%lu)",
                    ch, lblno, (unsigned long)status);
            break;
        }
        LOG_DBG("comm: add_label ch=%u lbl=0%03o period %d..%d ms",
                ch, lblno, (int)tmin, (int)tmax);
    }

    uint8_t extra[2] = { last_chnl, DIR_TX };
    comm_send_ack(CMD_ARINC_ADD_LABEL, req_id, status, extra, sizeof(extra));
}

/* ARINC_TIMETABLE_UPD_LBL (0x16): items <B I>,
   config = (channel & 0x0F) | (sdi_is_data << 4) | (enable << 5). */
#define UPD_LBL_ITEM_SIZE   (1u + 4u)

static void handle_tt_upd_lbl(uint8_t req_id, const uint8_t *payload, uint8_t len){
    uint32_t status    = PROTO_ST_OK;
    uint8_t  last_chnl = 0;
    uint8_t  off       = 0;

    if(len < UPD_LBL_ITEM_SIZE){
        status = PROTO_ST_ERROR;
    }
    while((uint16_t)(off + UPD_LBL_ITEM_SIZE) <= len){
        uint8_t  config = payload[off];
        uint32_t packet = (uint32_t)payload[off + 1]
                        | ((uint32_t)payload[off + 2] << 8)
                        | ((uint32_t)payload[off + 3] << 16)
                        | ((uint32_t)payload[off + 4] << 24);
        uint8_t  ch     = config & 0x0Fu;
        bool     sdi_is_data = ((config >> 4) & 1u) != 0u;
        bool     enable = ((config >> 5) & 1u) != 0u;
        last_chnl = ch;
        off = (uint8_t)(off + UPD_LBL_ITEM_SIZE);

        if(ch >= ARINC_NUM_CHANNELS){
            status = PROTO_ST_ARINC_TX_ERR_CHNL_OOR;
            break;
        }
        uint32_t r = atx_glue_set_label(ch, packet, sdi_is_data, enable);
        if(r != PROTO_ST_OK){
            status = r;
            break;
        }
    }

    uint8_t extra[2] = { last_chnl, DIR_TX };
    comm_send_ack(CMD_ARINC_TT_UPD_LBL, req_id, status, extra, sizeof(extra));
}

/* ARINC_TIMETABLE_BUILD (0x14): <B> channel. The stub build always succeeds
   (no schedule allocation to fail), so the diagnostic extra stays at the
   1-byte channel form. */
static void handle_tt_build(uint8_t req_id, const uint8_t *payload, uint8_t len){
    uint32_t status = PROTO_ST_OK;
    uint8_t  ch     = 0;

    if(len >= 1u){
        ch = payload[0];
        if(ch >= ARINC_NUM_CHANNELS){
            status = PROTO_ST_ARINC_TX_ERR_CHNL_OOR;
        }else{
            status = atx_glue_build(ch);
            if(status != PROTO_ST_OK){
                LOG_INF("comm: TT_BUILD ch=%d rejected, status=%lu",
                        ch, (unsigned long)status);
            }
        }
    }else{
        status = PROTO_ST_ERROR;
    }

    uint8_t extra[1] = { ch };
    comm_send_ack(CMD_ARINC_TT_BUILD, req_id, status, extra, sizeof(extra));
}

/* ====================================================================== */
/* Discrete-channel commands                                              */
/* ====================================================================== */

/* DISCRETE_SETUP (0x30): TCP only; a copy arriving over UDP is ignored so
   the fire-and-forget data path never mutates config. */
static void handle_discrete_setup(uint8_t req_id, const uint8_t *payload,
                                  uint8_t len, uint8_t src){
    if(src != COMM_SRC_TCP){
        return;                                 /* config is TCP-only */
    }
    uint32_t status;
    if(len != sizeof(proto_discrete_setup_t)){
        status = PROTO_ST_COMM_ERR_PAYLOAD_TOO_SHORT;
    }else{
        const proto_discrete_setup_t *c = (const proto_discrete_setup_t *)payload;
        status = discrete_glue_setup(c->enable, c->flags, c->report_ms);
    }
    comm_send_ack(CMD_DISCRETE_SETUP, req_id, status, NULL, 0);
}

/* DISCRETE_SET (0x31): both transports; ACK only when it arrived on TCP. */
static void handle_discrete_set(uint8_t req_id, const uint8_t *payload,
                                uint8_t len, uint8_t src){
    uint32_t status = PROTO_ST_OK;
    if(len < PROTO_DISCRETE_SET_HEADER_SIZE){
        status = PROTO_ST_COMM_ERR_PAYLOAD_TOO_SHORT;
    }else{
        uint32_t base_channel = (uint32_t)payload[0]
                              | ((uint32_t)payload[1] << 8)
                              | ((uint32_t)payload[2] << 16)
                              | ((uint32_t)payload[3] << 24);
        uint16_t bit_count = (uint16_t)payload[4]
                           | ((uint16_t)payload[5] << 8);
        uint16_t bitmap_bytes = PROTO_DISCRETE_BITMAP_BYTES(bit_count);
        uint16_t expected = (uint16_t)(PROTO_DISCRETE_SET_HEADER_SIZE
                                    + 2u * bitmap_bytes);
        uint64_t range_end = (uint64_t)base_channel + bit_count;
        if(bit_count == 0 || expected != len || range_end > UINT32_MAX + 1ull){
            status = PROTO_ST_COMM_ERR_PAYLOAD_TOO_SHORT;
        }else{
            const uint8_t *apply = payload + PROTO_DISCRETE_SET_HEADER_SIZE;
            const uint8_t *values = apply + bitmap_bytes;
            uint64_t local_apply = 0;
            uint64_t local_values = 0;
            for(uint16_t i = 0; i < bit_count; i++){
                uint32_t channel = base_channel + i;
                if(channel >= 64u || (apply[i / 8u] & (1u << (i % 8u))) == 0u){
                    continue;
                }
                uint64_t mask = UINT64_C(1) << channel;
                local_apply |= mask;
                if((values[i / 8u] & (1u << (i % 8u))) != 0u){
                    local_values |= mask;
                }
            }
            if(local_apply != 0u){
                discrete_glue_set(local_apply, local_values);
            }
        }
    }
    if(src == COMM_SRC_TCP){
        comm_send_ack(CMD_DISCRETE_SET, req_id, status, NULL, 0);
    }
}

/* ====================================================================== */
/* HID joystick commands (wire doc §12)                                   */
/* ====================================================================== */

/* HID_SETUP (0x33): TCP only; a copy arriving over UDP is ignored so the
   fire-and-forget data path never mutates config (discrete parity). */
static void handle_hid_setup(uint8_t req_id, const uint8_t *payload,
                             uint8_t len, uint8_t src){
    if(src != COMM_SRC_TCP){
        return;                                 /* config is TCP-only */
    }
    uint32_t status;
    if(len < sizeof(proto_hid_setup_t)){
        status = PROTO_ST_COMM_ERR_PAYLOAD_TOO_SHORT;
    }else{
        proto_hid_setup_t c;                    /* payload may be unaligned */
        memcpy(&c, payload, sizeof(c));
        status = hid_glue_setup(c.enable, c.flags, c.report_ms);
    }
    comm_send_ack(CMD_HID_SETUP, req_id, status, NULL, 0);
}

/* HID_SET (0x34): both transports; ACK only when it arrived on TCP. */
static void handle_hid_set(uint8_t req_id, const uint8_t *payload,
                           uint8_t len, uint8_t src){
    uint32_t status = PROTO_ST_OK;
    if(len < sizeof(proto_hid_set_t)){
        status = PROTO_ST_COMM_ERR_PAYLOAD_TOO_SHORT;
    }else{
        proto_hid_set_t c;                      /* payload may be unaligned */
        memcpy(&c, payload, sizeof(c));
        /* c is packed, so c.axes sits at an odd offset — copy into an
           aligned array before handing out an int16_t* (Xtensa faults on
           misaligned 16-bit loads). */
        int16_t axes[HID_NUM_AXES];
        memcpy(axes, c.axes, sizeof(axes));
        hid_glue_set(c.axis_mask, axes, c.btn_apply_mask, c.btn_values);
    }
    if(src == COMM_SRC_TCP){
        comm_send_ack(CMD_HID_SET, req_id, status, NULL, 0);
    }
}

#endif /* !RECOVERY_BUILD */

/* ====================================================================== */
/* Dispatch + framing                                                     */
/* ====================================================================== */
static void comm_dispatch(uint8_t cmd, uint8_t req_id,
                          uint8_t *payload, uint8_t len, uint8_t src){
    /* Config and query commands are TCP-only: a copy arriving over the
       fire-and-forget UDP path is dropped without an ACK or state change,
       mirroring handle_discrete_setup (and because their TCP ACKs must
       never be triggered from UDP). Only SEND_LBL_MAN and DISCRETE_SET are
       dual-transport (wire doc §4.12/§9.3); the gateway configures over
       TCP exclusively. */
    if(src != COMM_SRC_TCP){
        switch(cmd){
            case CMD_GET_FW_INFO:
            case CMD_GET_CAPABILITIES:
            case CMD_GET_HW_INFO:
            case CMD_GET_UID:
            case CMD_ARINC_CHNL_SETUP:
            case CMD_ARINC_CHNL_ENABLE:
            case CMD_ARINC_ADD_LABEL:
            case CMD_ARINC_TT_UPD_LBL:
            case CMD_ARINC_TT_BUILD:
                return;
            default:
                break;
        }
    }

    switch(cmd){
        case CMD_GET_FW_INFO:       handle_get_fw_info(req_id); break;
        case CMD_GET_HW_INFO:       handle_get_hw_info(req_id); break;
        case CMD_GET_UID:           handle_get_uid();           break;

#ifndef RECOVERY_BUILD
        case CMD_GET_CAPABILITIES:
            handle_get_capabilities(req_id, payload, len);
            break;
        case CMD_ARINC_CHNL_SETUP:  handle_chnl_setup(req_id, payload, len);  break;
        case CMD_ARINC_CHNL_ENABLE: handle_chnl_enable(req_id, payload, len); break;
        case CMD_ARINC_SEND_LBL_MAN:
            handle_send_label_manual(req_id, payload, len, src);
            break;
        case CMD_ARINC_ADD_LABEL:   handle_add_label(req_id, payload, len);  break;
        case CMD_ARINC_TT_UPD_LBL:  handle_tt_upd_lbl(req_id, payload, len); break;
        case CMD_ARINC_TT_BUILD:    handle_tt_build(req_id, payload, len);   break;

        case CMD_DISCRETE_SETUP:    handle_discrete_setup(req_id, payload, len, src); break;
        case CMD_DISCRETE_SET:      handle_discrete_set(req_id, payload, len, src);   break;

        case CMD_HID_SETUP:         handle_hid_setup(req_id, payload, len, src); break;
        case CMD_HID_SET:           handle_hid_set(req_id, payload, len, src);   break;

        case CMD_SET_LOG_LEVEL:
            /* TCP only; silently ignored on UDP (wire doc §10.3). */
            if(src == COMM_SRC_TCP){
                if(len >= 1 && payload[0] <= LOG_LEVEL_INFO){
                    lc_log_set_level(payload[0]);
                    uint8_t lvl = lc_log_get_level();
                    comm_send_ack(CMD_SET_LOG_LEVEL, req_id, PROTO_ST_OK, &lvl, 1);
                }else{
                    comm_send_ack(CMD_SET_LOG_LEVEL, req_id, PROTO_ST_ERROR, NULL, 0);
                }
            }
            break;
#else /* RECOVERY_BUILD */
        case CMD_FW_UPDATE:
            if(src == COMM_SRC_TCP){
                /* Chunk-size negotiation: a successful START (index 0) is
                   ACKed with 2 extra bytes (u16 LE = FWUP_CHUNK_SIZE)
                   advertising the max STEP chunk. The STM32 bootloader
                   sends no extra here, so hosts that don't understand the
                   advertisement — or talk to legacy cards — keep the
                   32-byte default. All other FW_UPDATE ACKs are unchanged. */
                int is_start = (len >= 4 &&
                                payload[0] == 0 && payload[1] == 0 &&
                                payload[2] == 0 && payload[3] == 0);
                uint32_t status = fwup_handle(payload, len);
                if(is_start && status == FWUP_OK){
                    uint8_t max_chunk[2] = {
                        (uint8_t)(FWUP_CHUNK_SIZE & 0xFF),
                        (uint8_t)((FWUP_CHUNK_SIZE >> 8) & 0xFF),
                    };
                    comm_send_ack(CMD_FW_UPDATE, req_id, status,
                                  max_chunk, sizeof(max_chunk));
                }else{
                    comm_send_ack(CMD_FW_UPDATE, req_id, status, NULL, 0);
                }
            }
            break;
#endif

        case CMD_FIND_ME:
            if(src == COMM_SRC_TCP){
                handle_find_me(req_id, payload, len);
            }
            break;

        case CMD_JUMP_TO_BOOTLOADER:
            if(src == COMM_SRC_TCP){
                handle_jmp_bootloader(req_id, payload, len);
            }
            break;
        case CMD_REBOOT:
            if(src == COMM_SRC_TCP){
                handle_reboot(req_id, payload, len);
            }
            break;

        case CMD_PING:
            /* PING is ACKed on TCP, silent on UDP. */
            if(src == COMM_SRC_TCP){
                comm_send_ack(CMD_PING, req_id, PROTO_ST_OK, NULL, 0);
            }
            break;
        default:
            /* Unknown command (including FW_UPDATE in the application build
               and the ARINC/discrete set in the recovery build): ACK an
               error so the gateway's waiter does not hang. */
            if(src == COMM_SRC_TCP){
                comm_send_ack(cmd, req_id, PROTO_ST_ERROR, NULL, 0);
            }
            break;
    }
}

uint16_t comm_core_input(uint8_t *buf, uint16_t len, uint8_t src){
    uint16_t off = 0;

    while((uint16_t)(len - off) >= PROTO_HEADER_SIZE){
        uint8_t *p    = buf + off;
        uint16_t avail = (uint16_t)(len - off);
        uint16_t head  = (uint16_t)p[0] | ((uint16_t)p[1] << 8);

        if(head != PROTO_HEAD){
            off++;
            continue;
        }

        uint8_t  size  = p[3];
        uint8_t  cmd   = p[4];
        uint16_t total = (uint16_t)(PROTO_HEADER_SIZE + size + PROTO_CSUM_SIZE);

        if(size > PROTO_MAX_PAYLOAD){
            off++;
            continue;
        }

        if(avail < total){
            break;              /* incomplete: wait for more bytes */
        }

        uint8_t csum = 0;
        for(uint16_t i = PROTO_CSUM_START; i < (uint16_t)(PROTO_HEADER_SIZE + size); i++){
            csum += p[i];
        }
        if(csum != p[PROTO_HEADER_SIZE + size]){
            off++;              /* bad checksum: resync */
            continue;
        }

        comm_dispatch(cmd, p[2], p + PROTO_HEADER_SIZE, size, src);
        off = (uint16_t)(off + total);
    }

    return off;
}

/* ====================================================================== */
/* Public API                                                             */
/* ====================================================================== */
void comm_core_init(const comm_ops_t *ops){
    g_ops   = ops;
    g_tx_id = 0;
    channels_reset();
}

void comm_core_session(bool connected){
    if(connected){
        channels_reset();       /* fresh control session */
#ifdef RECOVERY_BUILD
        fwup_session_reset();   /* fresh session -> fresh upload state */
#endif
        LOG_INF("comm: TCP control connected");
    }else{
        channels_reset();       /* dropped session -> all disabled */
#ifdef RECOVERY_BUILD
        fwup_session_reset();   /* abort a half-done upload right away */
#endif
        lc_log_set_level(LOG_LEVEL_INFO);  /* threshold resets with it */
        LOG_INF("comm: TCP control disconnected");
    }
}
