/**
  ******************************************************************************
  * @file    discrete_glue.c
  * @brief   STUB discrete subsystem: relay/input state machine, no hardware.
  ******************************************************************************
  */

#include "discrete_glue.h"
#include "comm_core.h"
#include "lc_log.h"
#include "lc_port.h"
#include "proto.h"

#include <stdbool.h>
#include <string.h>

#define DISCRETE_DEFAULT_REPORT_MS  500u

/* The gateway registers this product with Extra{Inputs:1, Outputs:32}
   (aes-gw2/linecard/protocol/discrete/products.go, mirroring A429-8BD): one
   digital input must be present and valid so a bound din.0 behaves like a
   real (idle) input instead of a permanently-invalid one. The stub keeps it
   at constant 0 (log-only, no pins). */
#define DISCRETE_NUM_INPUTS         1u

typedef struct {
    bool     enabled;
    uint16_t report_ms;
    uint32_t desired;       /* host-commanded relay bits                  */
    uint32_t relay_state;   /* "confirmed" state; stub confirms instantly */
    uint8_t  seq;           /* per-send counter, wraps                    */
    bool     state_pending; /* a STATE frame must go out ASAP             */
    uint32_t last_state_ms; /* tick of the last STATE sent                */
} discrete_t;

static discrete_t g_disc;

static void emit_state(void){
    proto_discrete_state_t st;
    st.relay_state = g_disc.relay_state;
    st.input_state = 0;                 /* one input, constant idle [stub] */
    st.input_valid = (1u << DISCRETE_NUM_INPUTS) - 1u;  /* present + valid */
    st.link        = 1;                 /* virtual relay block: always up */
    st.seq         = g_disc.seq;
    if(comm_core_send_udp(CMD_DISCRETE_STATE, (const uint8_t *)&st, sizeof(st))){
        g_disc.seq++;
        g_disc.state_pending = false;
        g_disc.last_state_ms = lc_port_tick_ms();
    }
    /* Send failed (host UDP endpoint not latched yet): stay pending; the
       loop retries once the endpoint is known. */
}

void discrete_glue_init(void){
    memset(&g_disc, 0, sizeof(g_disc));
    g_disc.report_ms = DISCRETE_DEFAULT_REPORT_MS;
}

void discrete_glue_reset(void){
    bool was_enabled = g_disc.enabled;
    uint32_t had_bits = g_disc.relay_state;
    g_disc.enabled     = false;
    g_disc.desired     = 0;
    g_disc.relay_state = 0;
    g_disc.report_ms   = DISCRETE_DEFAULT_REPORT_MS;
    if(was_enabled || had_bits){
        LOG_INF("discrete: reset, all relays off [stub]");
    }
}

uint32_t discrete_glue_setup(uint8_t enable, uint8_t flags, uint16_t report_ms){
    (void)flags;                        /* reserved, always 0 today */
    if(enable){
        g_disc.enabled   = true;
        g_disc.report_ms = (report_ms != 0) ? report_ms : DISCRETE_DEFAULT_REPORT_MS;
        g_disc.state_pending = true;    /* baseline STATE follows the ACK */
        LOG_INF("discrete: enabled, report every %ums [stub]", g_disc.report_ms);
    }else{
        /* Disable: relays off, one final STATE, heartbeat stops. */
        g_disc.desired     = 0;
        g_disc.relay_state = 0;
        if(g_disc.enabled){
            g_disc.state_pending = true;
            emit_state();
        }
        g_disc.enabled = false;
        LOG_INF("discrete: disabled, all relays off [stub]");
    }
    return PROTO_ST_OK;
}

void discrete_glue_set(uint32_t apply_mask, uint32_t values){
    if(!g_disc.enabled){
        /* While disabled (or link down on real hardware) writes are
           silently dropped — wire doc §9.3. */
        return;
    }
    uint32_t next = (g_disc.desired & ~apply_mask) | (values & apply_mask);
    if(next != g_disc.desired){
        LOG_DBG("discrete: would drive relays 0x%08x [stub, no pins]",
                (unsigned)next);
    }
    g_disc.desired = next;
    /* No Modbus round-trip on the stub: writes confirm immediately. */
    if(g_disc.relay_state != g_disc.desired){
        g_disc.relay_state = g_disc.desired;
        g_disc.state_pending = true;
    }
}

void discrete_glue_loop(void){
    if(!g_disc.enabled){
        return;
    }
    if(g_disc.state_pending){
        emit_state();
        return;
    }
    uint32_t now = lc_port_tick_ms();
    if((uint32_t)(now - g_disc.last_state_ms) >= g_disc.report_ms){
        emit_state();
    }
}
