/**
  ******************************************************************************
  * @file    arinc_glue.c
  * @brief   STUB ARINC engine: state + validation + logging, no pins.
  ******************************************************************************
  */

#include "arinc_glue.h"
#include "proto.h"
#include "lc_log.h"

#include <string.h>

/* Per-TX-channel timetable capacity. The STM32 engine allocates label slots
   in a schedule table; the stub just tracks declarations. */
#define ATX_MAX_LABELS  128

typedef struct {
    uint16_t label_id;
    float    t_max_ms;
    float    t_min_ms;
    uint32_t word;          /* last word written via TT_UPD_LBL */
    bool     tx_enabled;    /* label transmit flag from TT_UPD_LBL */
    bool     word_set;
} atx_label_t;

typedef struct {
    bool        setup;
    bool        enabled;
    uint8_t     speed;
    uint8_t     mode;
    uint16_t    delay_us;
    bool        built;          /* timetable BUILD completed */
    uint16_t    n_labels;
    atx_label_t labels[ATX_MAX_LABELS];
} atx_chnl_t;

static atx_chnl_t g_tx[ARINC_NUM_CHANNELS];

void atx_glue_init(void){
    memset(g_tx, 0, sizeof(g_tx));
}

void atx_glue_reset(void){
    memset(g_tx, 0, sizeof(g_tx));
    LOG_DBG("atx: reset (all channels disabled)");
}

uint32_t atx_glue_setup(uint8_t chnl, uint8_t speed, uint8_t mode,
                        uint16_t manual_delay_us){
    atx_chnl_t *c = &g_tx[chnl];
    /* (Re)setup clears the channel's timetable, like the real engine. */
    memset(c, 0, sizeof(*c));
    c->setup    = true;
    c->speed    = speed;
    c->mode     = mode;
    c->delay_us = manual_delay_us;
    return PROTO_ST_OK;
}

uint32_t atx_glue_add_label(uint8_t chnl, uint16_t label_id,
                            float timing_max_ms, float timing_min_ms){
    atx_chnl_t *c = &g_tx[chnl];
    if(!c->setup){
        return PROTO_ST_ARINC_TX_ERR_CHNL_NOT_INIT;
    }
    if(c->n_labels >= ATX_MAX_LABELS){
        return PROTO_ST_ARINC_ERR_QUEUE_FULL;
    }
    atx_label_t *l = &c->labels[c->n_labels++];
    l->label_id = label_id;
    l->t_max_ms = timing_max_ms;
    l->t_min_ms = timing_min_ms;
    c->built = false;               /* table changed -> needs a new BUILD */
    return PROTO_ST_OK;
}

uint32_t atx_glue_build(uint8_t chnl){
    atx_chnl_t *c = &g_tx[chnl];
    if(!c->setup){
        return PROTO_ST_ARINC_TX_ERR_CHNL_NOT_INIT;
    }
    c->built = true;
    LOG_DBG("atx: ch%u timetable built (%u labels) [stub]", chnl, c->n_labels);
    return PROTO_ST_OK;
}

uint32_t atx_glue_enable(uint8_t chnl, bool enable){
    atx_chnl_t *c = &g_tx[chnl];
    if(!c->setup){
        return PROTO_ST_ARINC_TX_ERR_CHNL_NOT_INIT;
    }
    c->enabled = enable;
    LOG_DBG("atx: ch%u would be %s [stub, no pins driven]",
            chnl, enable ? "started" : "stopped");
    return PROTO_ST_OK;
}

uint32_t atx_glue_set_label(uint8_t chnl, uint32_t packet,
                            bool sdi_is_data, bool enable){
    atx_chnl_t *c = &g_tx[chnl];
    if(!c->setup){
        return PROTO_ST_ARINC_TX_ERR_CHNL_NOT_INIT;
    }
    /* Match by bit-reversed label byte (+ SDI when it is not data bits),
       like the real timetable does. The stub keys on the low label byte. */
    uint8_t lbl = (uint8_t)(packet & 0xFF);
    for(uint16_t i = 0; i < c->n_labels; i++){
        atx_label_t *l = &c->labels[i];
        uint8_t decl_lbl = (uint8_t)(l->label_id & 0xFF);
        if(decl_lbl != lbl){
            continue;
        }
        if(!sdi_is_data){
            uint8_t decl_sdi = (uint8_t)((l->label_id >> 8) & 0x03);
            uint8_t pkt_sdi  = (uint8_t)((packet >> 8) & 0x03);
            if(decl_sdi != pkt_sdi){
                continue;
            }
        }
        l->word       = packet;
        l->tx_enabled = enable;
        l->word_set   = true;
        LOG_DBG("atx: ch%u label word 0x%08x %s [stub]",
                chnl, (unsigned)packet, enable ? "enabled" : "disabled");
        return PROTO_ST_OK;
    }
    /* Unknown label slot: the stub accepts it (the STM32 firmware accepts
       updates for channels without hardware the same way). */
    LOG_DBG("atx: ch%u upd_lbl for undeclared label 0x%02x accepted [stub]",
            chnl, lbl);
    return PROTO_ST_OK;
}

uint32_t atx_glue_set_label_manual(uint8_t chnl, uint32_t packet){
    atx_chnl_t *c = &g_tx[chnl];
    if(!c->setup){
        return PROTO_ST_ARINC_TX_ERR_CHNL_NOT_INIT;
    }
    LOG_DBG("atx: ch%u would transmit 0x%08x [stub, no pins driven]",
            chnl, (unsigned)packet);
    return PROTO_ST_OK;
}
