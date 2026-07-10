/**
  ******************************************************************************
  * @file    comm_capture.c
  * @brief   Capture comm_ops_t implementation (see comm_capture.h).
  ******************************************************************************
  */

#include "comm_capture.h"
#include "proto.h"

#include <string.h>

capture_t cap_tcp;
capture_t cap_udp;

int      cap_bootloader_calls;
int      cap_reboot_calls;
int      cap_find_me_calls;
uint32_t cap_find_me_duration;
bool     cap_udp_enabled = true;

static void cap_store(capture_t *c, const uint8_t *frame, uint16_t len){
    if(c->frames >= CAP_MAX_FRAMES || c->len + len > CAP_BUF_SIZE){
        return;
    }
    memcpy(c->data + c->len, frame, len);
    c->frame_off[c->frames] = c->len;
    c->frame_len[c->frames] = len;
    c->len += len;
    c->frames++;
}

static void ops_send_tcp(const uint8_t *frame, uint16_t len){
    cap_store(&cap_tcp, frame, len);
}

static bool ops_send_udp(const uint8_t *frame, uint16_t len){
    if(!cap_udp_enabled){
        return false;
    }
    cap_store(&cap_udp, frame, len);
    return true;
}

static void ops_enter_bootloader(void){ cap_bootloader_calls++; }
static void ops_reboot(void){ cap_reboot_calls++; }
static void ops_find_me(uint32_t d){ cap_find_me_calls++; cap_find_me_duration = d; }

static const comm_ops_t g_cap_ops = {
    .send_tcp         = ops_send_tcp,
    .send_udp         = ops_send_udp,
    .enter_bootloader = ops_enter_bootloader,
    .reboot           = ops_reboot,
    .find_me          = ops_find_me,
};

void cap_reset(void){
    memset(&cap_tcp, 0, sizeof(cap_tcp));
    memset(&cap_udp, 0, sizeof(cap_udp));
    cap_bootloader_calls = 0;
    cap_reboot_calls     = 0;
    cap_find_me_calls    = 0;
    cap_find_me_duration = 0;
    cap_udp_enabled      = true;
}

const comm_ops_t *cap_ops(void){
    return &g_cap_ops;
}

uint16_t cap_build_req(uint8_t *out, uint8_t pkt_id, uint8_t cmd,
                       const uint8_t *payload, uint8_t len){
    out[0] = (uint8_t)(PROTO_HEAD & 0xFF);          /* 0xBB */
    out[1] = (uint8_t)((PROTO_HEAD >> 8) & 0xFF);   /* 0xAA */
    out[2] = pkt_id;
    out[3] = len;
    out[4] = cmd;
    if(len && payload){
        memcpy(out + PROTO_HEADER_SIZE, payload, len);
    }
    uint8_t csum = 0;
    for(uint16_t i = PROTO_CSUM_START; i < (uint16_t)(PROTO_HEADER_SIZE + len); i++){
        csum += out[i];
    }
    out[PROTO_HEADER_SIZE + len] = csum;
    return (uint16_t)(PROTO_HEADER_SIZE + len + PROTO_CSUM_SIZE);
}

const uint8_t *cap_frame(const capture_t *c, int idx,
                         uint8_t *cmd, uint8_t *plen, uint8_t *pkt_id){
    if(idx >= c->frames){
        return NULL;
    }
    const uint8_t *p = c->data + c->frame_off[idx];
    uint16_t len = c->frame_len[idx];

    if(len < PROTO_HEADER_SIZE + PROTO_CSUM_SIZE){
        return NULL;
    }
    uint16_t head = (uint16_t)p[0] | ((uint16_t)p[1] << 8);
    if(head != PROTO_HEAD){
        return NULL;
    }
    uint8_t size = p[3];
    if(len != (uint16_t)(PROTO_HEADER_SIZE + size + PROTO_CSUM_SIZE)){
        return NULL;
    }
    uint8_t csum = 0;
    for(uint16_t i = PROTO_CSUM_START; i < (uint16_t)(PROTO_HEADER_SIZE + size); i++){
        csum += p[i];
    }
    if(csum != p[PROTO_HEADER_SIZE + size]){
        return NULL;
    }
    if(cmd)   { *cmd    = p[4]; }
    if(plen)  { *plen   = size; }
    if(pkt_id){ *pkt_id = p[2]; }
    return p + PROTO_HEADER_SIZE;
}
