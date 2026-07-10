/* ACK envelope semantics + GET_FW_INFO / GET_HW_INFO / GET_UID payload
   layouts + UID-keyed commands + FIND_ME + SET_LOG_LEVEL + the ARINC
   channel state machine (status codes per WIRE_PROTOCOL.md §4). */

#include "board_id.h"
#include "comm_capture.h"
#include "comm_core.h"
#include "identity.h"
#include "lc_log.h"
#include "lc_port.h"
#include "proto.h"
#include "test_util.h"

#include <string.h>

static const uint8_t MAC[6] = { 0x24, 0x0A, 0xC4, 0x11, 0x22, 0x33 };

static uint8_t g_frame[PROTO_MAX_PACKET];

/* Send one request over `src` and return the first reply frame's payload
   (NULL if none was sent). */
static const uint8_t *req(uint8_t src, uint8_t pkt_id, uint8_t cmd,
                          const uint8_t *payload, uint8_t len,
                          uint8_t *rcmd, uint8_t *rlen){
    cap_reset();
    uint16_t flen = cap_build_req(g_frame, pkt_id, cmd, payload, len);
    comm_core_input(g_frame, flen, src);
    return cap_frame(&cap_tcp, 0, rcmd, rlen, NULL);
}

static uint32_t ack_status(const uint8_t *pl){
    return (uint32_t)pl[1] | ((uint32_t)pl[2] << 8)
         | ((uint32_t)pl[3] << 16) | ((uint32_t)pl[4] << 24);
}

int main(void){
    lc_port_init();
    lc_log_init();
    identity_init(MAC);
    cap_reset();
    comm_core_init(cap_ops());

    uint8_t rcmd, rlen;
    const uint8_t *pl;

    /* --- GET_FW_INFO: 53-byte extra, §4.5 layout ------------------------- */
    pl = req(COMM_SRC_TCP, 0x21, CMD_GET_FW_INFO, NULL, 0, &rcmd, &rlen);
    CHECK(pl != NULL);
    CHECK(rcmd == CMD_GET_FW_INFO);
    CHECK_EQ_U32(rlen, 5 + 53);
    CHECK(pl[0] == 0x21);                       /* echoed packet_id */
    CHECK_EQ_U32(ack_status(pl), PROTO_ST_OK);
    const uint8_t *fw = pl + 5;
    /* hash: 8 ASCII NUL-padded == VERSION_COMMIT */
    uint8_t field[24];
    memset(field, 0, sizeof(field));
    memcpy(field, VERSION_COMMIT, strlen(VERSION_COMMIT) > 8 ? 8 : strlen(VERSION_COMMIT));
    CHECK_MEM(fw + 6, field, 8);
    /* branch: 14 bytes NUL-padded */
    memset(field, 0, sizeof(field));
    memcpy(field, BOARD_INFO_FW_BRANCH, strlen(BOARD_INFO_FW_BRANCH));
    CHECK_MEM(fw + 14, field, 14);
    /* tag: 24 bytes NUL-padded == VERSION_STRING */
    memset(field, 0, sizeof(field));
    memcpy(field, BOARD_INFO_FW_TAG, strlen(BOARD_INFO_FW_TAG));
    CHECK_MEM(fw + 28, field, 24);
    /* byte 52: fw_type (0 application in this build) */
    CHECK_EQ_U32(fw[52], BOARD_INFO_FW_TYPE);

    /* --- GET_HW_INFO: §4.6 layout ---------------------------------------- */
    pl = req(COMM_SRC_TCP, 0x22, CMD_GET_HW_INFO, NULL, 0, &rcmd, &rlen);
    CHECK(pl != NULL && rcmd == CMD_GET_HW_INFO);
    CHECK_EQ_U32(rlen, 5 + 53);
    CHECK(pl[0] == 0x22);
    CHECK_EQ_U32(ack_status(pl), PROTO_ST_OK);
    const uint8_t *hw = pl + 5;
    memset(field, 0, sizeof(field));
    memcpy(field, BOARD_INFO_SHORT_ID, strlen(BOARD_INFO_SHORT_ID));
    CHECK_MEM(hw + 6, field, 16);               /* type string      */
    CHECK_MEM(hw + 22, MAC, 6);                 /* raw MAC          */
    CHECK_MEM(hw + 28, identity_uid(), 12);     /* raw UID          */
    memset(field, 0, sizeof(field));
    memcpy(field, identity_serial(), 12);
    CHECK_MEM(hw + 40, field, 13);              /* serial, NUL-pad  */

    /* --- GET_UID: raw 12 bytes, NO ACK envelope (§4.7) -------------------- */
    pl = req(COMM_SRC_TCP, 0x23, CMD_GET_UID, NULL, 0, &rcmd, &rlen);
    CHECK(pl != NULL && rcmd == CMD_GET_UID);
    CHECK_EQ_U32(rlen, 12);
    CHECK_MEM(pl, identity_uid(), 12);

    /* --- PING: ACK on TCP, silent on UDP ---------------------------------- */
    pl = req(COMM_SRC_TCP, 0x24, CMD_PING, NULL, 0, &rcmd, &rlen);
    CHECK(pl != NULL && ack_status(pl) == PROTO_ST_OK);
    pl = req(COMM_SRC_UDP, 0x25, CMD_PING, NULL, 0, &rcmd, &rlen);
    CHECK(pl == NULL);

    /* --- SET_LOG_LEVEL (§10.3) -------------------------------------------- */
    uint8_t lvl = 0;                            /* DEBUG */
    pl = req(COMM_SRC_TCP, 0x26, CMD_SET_LOG_LEVEL, &lvl, 1, &rcmd, &rlen);
    CHECK(pl != NULL && ack_status(pl) == PROTO_ST_OK);
    CHECK_EQ_U32(rlen, 5 + 1);
    CHECK(pl[5] == 0);                          /* level now in effect */
    CHECK_EQ_U32(lc_log_get_level(), 0);
    lvl = 7;                                    /* invalid */
    pl = req(COMM_SRC_TCP, 0x27, CMD_SET_LOG_LEVEL, &lvl, 1, &rcmd, &rlen);
    CHECK(pl != NULL && ack_status(pl) == PROTO_ST_ERROR);
    CHECK_EQ_U32(lc_log_get_level(), 0);        /* unchanged */
    lvl = 1;
    pl = req(COMM_SRC_UDP, 0x28, CMD_SET_LOG_LEVEL, &lvl, 1, &rcmd, &rlen);
    CHECK(pl == NULL);                          /* UDP: ignored entirely */
    CHECK_EQ_U32(lc_log_get_level(), 0);

    /* --- FIND_ME (§4.14) --------------------------------------------------- */
    uint8_t dur[4] = { 10, 0, 0, 0 };
    pl = req(COMM_SRC_TCP, 0x29, CMD_FIND_ME, dur, 4, &rcmd, &rlen);
    CHECK(pl != NULL && ack_status(pl) == PROTO_ST_OK);
    CHECK_EQ_U32(cap_find_me_calls, 1);
    CHECK_EQ_U32(cap_find_me_duration, 10);

    /* --- JUMP_TO_BOOTLOADER / REBOOT: UID-keyed (§4.15) -------------------- */
    uint8_t bad_uid[12] = {0};
    pl = req(COMM_SRC_TCP, 0x2A, CMD_JUMP_TO_BOOTLOADER, bad_uid, 12, &rcmd, &rlen);
    CHECK(pl != NULL && ack_status(pl) == PROTO_ST_ERROR);
    CHECK_EQ_U32(cap_bootloader_calls, 0);
    pl = req(COMM_SRC_TCP, 0x2B, CMD_JUMP_TO_BOOTLOADER, identity_uid(), 12, &rcmd, &rlen);
    CHECK(pl != NULL && ack_status(pl) == PROTO_ST_OK);
    CHECK(pl[0] == 0x2B);
    CHECK_EQ_U32(cap_bootloader_calls, 1);
    pl = req(COMM_SRC_TCP, 0x2C, CMD_REBOOT, identity_uid(), 12, &rcmd, &rlen);
    CHECK(pl != NULL && ack_status(pl) == PROTO_ST_OK);
    CHECK_EQ_U32(cap_reboot_calls, 1);
    /* Truncated key (req() resets the capture counters). */
    pl = req(COMM_SRC_TCP, 0x2D, CMD_REBOOT, identity_uid(), 8, &rcmd, &rlen);
    CHECK(pl != NULL && ack_status(pl) == PROTO_ST_ERROR);
    CHECK_EQ_U32(cap_reboot_calls, 0);

    /* --- ARINC channel state machine (§4.8/§4.9) ---------------------------- */
    /* ENABLE before SETUP -> *_CHNL_NOT_INIT. */
    proto_chnl_enable_t en = { .chnl = 1, .direction = DIR_RX, .is_enable = 1 };
    pl = req(COMM_SRC_TCP, 0x30, CMD_ARINC_CHNL_ENABLE,
             (const uint8_t *)&en, sizeof(en), &rcmd, &rlen);
    CHECK(pl != NULL && ack_status(pl) == PROTO_ST_ARINC_RX_ERR_CHNL_NOT_INIT);
    CHECK_EQ_U32(rlen, 5 + 2);
    CHECK(pl[5] == 1 && pl[6] == DIR_RX);       /* extra: channel, direction */

    /* SETUP with bad direction / speed / channel. */
    proto_chnl_setup_t su = { .chnl = 1, .direction = 5, .speed = SPEED_100KHZ,
                              .mode = MODE_RX_NORMAL, .config_delay_us = 0 };
    pl = req(COMM_SRC_TCP, 0x31, CMD_ARINC_CHNL_SETUP,
             (const uint8_t *)&su, sizeof(su), &rcmd, &rlen);
    CHECK(pl != NULL && ack_status(pl) == PROTO_ST_ARINC_ERR_CHNL_DIR_UNKNOWN);
    su.direction = DIR_RX; su.speed = 9;
    pl = req(COMM_SRC_TCP, 0x32, CMD_ARINC_CHNL_SETUP,
             (const uint8_t *)&su, sizeof(su), &rcmd, &rlen);
    CHECK(pl != NULL && ack_status(pl) == PROTO_ST_ARINC_ERR_SPEED_UNKNOWN);
    su.speed = SPEED_100KHZ; su.chnl = ARINC_NUM_CHANNELS;
    pl = req(COMM_SRC_TCP, 0x33, CMD_ARINC_CHNL_SETUP,
             (const uint8_t *)&su, sizeof(su), &rcmd, &rlen);
    CHECK(pl != NULL && ack_status(pl) == PROTO_ST_ARINC_RX_ERR_CHNL_OOR);
    su.direction = DIR_TX;
    pl = req(COMM_SRC_TCP, 0x34, CMD_ARINC_CHNL_SETUP,
             (const uint8_t *)&su, sizeof(su), &rcmd, &rlen);
    CHECK(pl != NULL && ack_status(pl) == PROTO_ST_ARINC_TX_ERR_CHNL_OOR);

    /* Good SETUP + ENABLE. */
    su.chnl = 1; su.direction = DIR_RX; su.mode = MODE_RX_NORMAL;
    pl = req(COMM_SRC_TCP, 0x35, CMD_ARINC_CHNL_SETUP,
             (const uint8_t *)&su, sizeof(su), &rcmd, &rlen);
    CHECK(pl != NULL && ack_status(pl) == PROTO_ST_OK);
    pl = req(COMM_SRC_TCP, 0x36, CMD_ARINC_CHNL_ENABLE,
             (const uint8_t *)&en, sizeof(en), &rcmd, &rlen);
    CHECK(pl != NULL && ack_status(pl) == PROTO_ST_OK);
    CHECK(comm_core_any_channel_enabled());

    /* TX flow: setup, manual send before/after. */
    uint8_t man[5] = { 0 /* chnl */, 0x78, 0x56, 0x34, 0x12 };
    pl = req(COMM_SRC_TCP, 0x37, CMD_ARINC_SEND_LBL_MAN, man, sizeof(man), &rcmd, &rlen);
    CHECK(pl != NULL && ack_status(pl) == PROTO_ST_ARINC_TX_ERR_CHNL_NOT_INIT);
    proto_chnl_setup_t tx = { .chnl = 0, .direction = DIR_TX, .speed = SPEED_100KHZ,
                              .mode = MODE_TX_MANUAL, .config_delay_us = 100 };
    pl = req(COMM_SRC_TCP, 0x38, CMD_ARINC_CHNL_SETUP,
             (const uint8_t *)&tx, sizeof(tx), &rcmd, &rlen);
    CHECK(pl != NULL && ack_status(pl) == PROTO_ST_OK);
    pl = req(COMM_SRC_TCP, 0x39, CMD_ARINC_SEND_LBL_MAN, man, sizeof(man), &rcmd, &rlen);
    CHECK(pl != NULL && ack_status(pl) == PROTO_ST_OK);
    /* UDP fast path: fire-and-forget, no ACK. */
    pl = req(COMM_SRC_UDP, 0x3A, CMD_ARINC_SEND_LBL_MAN, man, sizeof(man), &rcmd, &rlen);
    CHECK(pl == NULL);

    /* Timetable flow: ADD_LABEL + UPD_LBL + BUILD all ACK OK. */
    uint8_t add[11] = { 0, 0x53, 0x00,           /* chnl, label_id */
                        0x00, 0x00, 0xC8, 0x42,  /* 100.0f  */
                        0x00, 0x00, 0x48, 0x42 };/* 50.0f   */
    pl = req(COMM_SRC_TCP, 0x3B, CMD_ARINC_ADD_LABEL, add, sizeof(add), &rcmd, &rlen);
    CHECK(pl != NULL && ack_status(pl) == PROTO_ST_OK);
    uint8_t upd[5] = { (0 & 0x0F) | (1u << 4) | (1u << 5), 0x53, 0x00, 0x00, 0x00 };
    pl = req(COMM_SRC_TCP, 0x3C, CMD_ARINC_TT_UPD_LBL, upd, sizeof(upd), &rcmd, &rlen);
    CHECK(pl != NULL && ack_status(pl) == PROTO_ST_OK);
    uint8_t bld[1] = { 0 };
    pl = req(COMM_SRC_TCP, 0x3D, CMD_ARINC_TT_BUILD, bld, sizeof(bld), &rcmd, &rlen);
    CHECK(pl != NULL && ack_status(pl) == PROTO_ST_OK);
    CHECK_EQ_U32(rlen, 5 + 1);
    CHECK(pl[5] == 0);                          /* extra: channel */

    /* Bulk transfers are not implemented: ERROR ACK (matches the STM32
       firmware and the fakelc simulator's default arm). */
    pl = req(COMM_SRC_TCP, 0x3E, CMD_ARINC_TT_GET_LIST, bld, 1, &rcmd, &rlen);
    CHECK(pl != NULL && rcmd == CMD_ARINC_TT_GET_LIST
          && ack_status(pl) == PROTO_ST_ERROR);

    /* --- session drop resets every channel (§5 / CLAUDE.md) ----------------- */
    comm_core_session(false);
    CHECK(!comm_core_any_channel_enabled());
    CHECK_EQ_U32(lc_log_get_level(), LOG_LEVEL_INFO);   /* threshold reset */
    pl = req(COMM_SRC_TCP, 0x3F, CMD_ARINC_CHNL_ENABLE,
             (const uint8_t *)&en, sizeof(en), &rcmd, &rlen);
    CHECK(pl != NULL && ack_status(pl) == PROTO_ST_ARINC_RX_ERR_CHNL_NOT_INIT);

    return test_report("ack_info");
}
