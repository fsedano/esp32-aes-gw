/**
  ******************************************************************************
  * @file    fwupdate_core.c
  * @brief   CMD_FW_UPDATE upload state machine (see fwupdate_core.h).
  ******************************************************************************
  */

#include "fwupdate_core.h"
#include "fletcher32.h"
#include "lc_log.h"
#include "lc_port.h"

#include <string.h>

typedef enum {
    FWUP_IDLE = 0,
    FWUP_UPLOAD,
} fwup_state_t;

typedef struct {
    fwup_state_t     state;
    uint32_t         firm_size;         /* padded plaintext == ciphertext len */
    uint32_t         firm_checksum;     /* announced Fletcher32               */
    uint32_t         bytes_counter;
    uint32_t         packet_index_prev;
    uint32_t         last_rx_ms;        /* upload watchdog                    */
    fwup_status_t    status;            /* latched abort reason               */
    fletcher32_ctx_t fletch;            /* over the decrypted stream          */
    int              flash_open;        /* flash_begin succeeded, no end yet  */
} fwup_t;

static fwup_t g_fw;
static const fwup_ops_t *g_fwops;

static uint32_t rd_u32(const uint8_t *p){
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void fwup_reset_var(void){
    g_fw.state             = FWUP_IDLE;
    g_fw.bytes_counter     = 0;
    g_fw.packet_index_prev = 0;
    g_fw.status            = FWUP_OK;
    g_fw.last_rx_ms        = 0;
    g_fw.flash_open        = 0;
    fletcher32_init(&g_fw.fletch);
}

void fwup_init(const fwup_ops_t *ops){
    g_fwops = ops;
    fwup_reset_var();
    g_fw.firm_size = 0;
}

void fwup_session_reset(void){
    /* Release the port's flash handle no matter how the last upload ended:
       a checksum-fail or a mid-upload disconnect leaves it open (the core
       only tracks its own flash_open flag), and the port hook is
       idempotent. */
    if(g_fwops != NULL && g_fwops->flash_abort != NULL){
        g_fwops->flash_abort();
    }
    fwup_reset_var();
    g_fw.firm_size = 0;
}

static uint32_t fwup_finish(void){
    if(g_fw.bytes_counter != g_fw.firm_size){
        LOG_INF("fwup: size mismatch %u", (unsigned)g_fw.bytes_counter);
        return FWUP_ERR_FIRM_SIZE_DISAGREE;
    }

    uint32_t f32 = fletcher32_final(&g_fw.fletch);
    if(f32 != g_fw.firm_checksum){
        LOG_INF("fwup: checksum mismatch calc=%08x want=%08x",
                (unsigned)f32, (unsigned)g_fw.firm_checksum);
        return FWUP_ERR_FW_CHECKSUM;
    }

    if(g_fwops->flash_end() != 0){
        g_fw.flash_open = 0;
        return FWUP_ERR_IMAGE_INVALID;
    }
    g_fw.flash_open = 0;

    if(g_fwops->set_boot() != 0){
        return FWUP_ERR_SET_BOOT;
    }

    g_fw.state = FWUP_IDLE;
    LOG_INF("fwup: upload verified (%u bytes), boot -> new app",
            (unsigned)g_fw.firm_size);
    return FWUP_OK;
}

uint32_t fwup_handle(uint8_t *payload, uint8_t len){
    if(g_fwops == NULL){
        return FWUP_ERR_SM_WRONG_STATE;
    }
    if(len < sizeof(uint32_t)){
        return FWUP_ERR_SM_WRONG_STATE;
    }

    uint32_t status = FWUP_OK;

    switch(g_fw.state){
        case FWUP_IDLE: {
            uint32_t packet_index = rd_u32(payload);

            /* First packet must carry index 0 (START). */
            if(packet_index != 0){
                if(g_fw.status != FWUP_OK){
                    /* Report the error that aborted the previous upload. */
                    return g_fw.status;
                }
                return FWUP_ERR_INDEX_NOT_ZERO_WHEN_IDLE;
            }
            /* START: <u32 0 | u32 size | u32 fletcher32 | iv[16]> = 28 bytes */
            if(len < 4 + 4 + 4 + 16){
                return FWUP_ERR_SM_WRONG_STATE;
            }

            fwup_reset_var();
            g_fw.firm_size     = rd_u32(payload + 4);
            g_fw.firm_checksum = rd_u32(payload + 8);

            if((g_fw.firm_size % 16) != 0){
                return FWUP_ERR_FW_SIZE_NOT_MULT_16;
            }
            if(g_fw.firm_size < FWUP_MIN_SIZE){
                LOG_INF("fwup: firmware size suspicious: %u",
                        (unsigned)g_fw.firm_size);
                return FWUP_ERR_FW_SIZE_OUT_OF_RANGE;
            }

            if(g_fwops->aes_start(payload + 12) != 0){
                return FWUP_ERR_AES;
            }
            if(g_fwops->flash_begin(g_fw.firm_size) != 0){
                /* Covers "too big for the partition" as well. */
                return FWUP_ERR_FLASH_BEGIN;
            }
            g_fw.flash_open = 1;

            LOG_INF("fwup: upload start: %u bytes", (unsigned)g_fw.firm_size);
            g_fw.last_rx_ms = lc_port_tick_ms();
            g_fw.state = FWUP_UPLOAD;
            break;
        }

        case FWUP_UPLOAD: {
            uint32_t packet_index = rd_u32(payload);

            if(g_fw.status != FWUP_OK){
                g_fw.state = FWUP_IDLE;
                return g_fw.status;
            }

            /* FINISH marker */
            if(packet_index == 0xFFFFFFFFu){
                status = fwup_finish();
                if(status != FWUP_OK){
                    fwup_reset_var();
                }
                break;
            }

            if(packet_index != (g_fw.packet_index_prev + 1)){
                LOG_INF("fwup: packet loss t:%u t-1:%u",
                        (unsigned)packet_index, (unsigned)g_fw.packet_index_prev);
                g_fw.state = FWUP_IDLE;
                status = FWUP_ERR_PACKET_LOSS;
                break;
            }
            g_fw.packet_index_prev = packet_index;

            if(g_fw.bytes_counter >= g_fw.firm_size){
                fwup_reset_var();
                status = FWUP_ERR_RECV_TOO_MANY_BYTES;
                break;
            }

            uint32_t data_size = (uint32_t)len - sizeof(uint32_t);
            if(data_size > FWUP_CHUNK_SIZE){
                g_fw.state = FWUP_IDLE;
                status = FWUP_ERR_PAYLOAD_TOO_LONG;
                break;
            }
            /* AES-CBC operates on 16-byte blocks. */
            if((data_size % 16) != 0){
                g_fw.state = FWUP_IDLE;
                status = FWUP_ERR_DATA_NOT_FACTOR_16;
                break;
            }

            /* Decrypt in place, checksum the plaintext, then program. */
            if(g_fwops->aes_decrypt(payload + 4, data_size) != 0){
                g_fw.state = FWUP_IDLE;
                status = FWUP_ERR_AES;
                break;
            }
            fletcher32_update(&g_fw.fletch, payload + 4, data_size);

            if(g_fwops->flash_write(payload + 4, data_size) != 0){
                g_fw.state = FWUP_IDLE;
                status = FWUP_ERR_FLASH_PROGRAM;
                break;
            }

            g_fw.bytes_counter += data_size;
            g_fw.last_rx_ms = lc_port_tick_ms();   /* keep the watchdog alive */
            break;
        }

        default:
            status = FWUP_ERR_SM_WRONG_STATE;
            break;
    }

    return status;
}

void fwup_tick(void){
    if(g_fw.state != FWUP_UPLOAD){
        return;
    }
    if((uint32_t)(lc_port_tick_ms() - g_fw.last_rx_ms) > FWUP_TIMEOUT_MS){
        LOG_INF("fwup: upload timeout, aborted");
        g_fw.state  = FWUP_IDLE;
        g_fw.status = FWUP_ERR_TIMEOUT;
    }
}
