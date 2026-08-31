/**
  ******************************************************************************
  * @file    discrete_glue.c
  * @brief   Discrete wire state machine backed by an asynchronous hardware
  *          worker. Reported relay/input values are physical readback only.
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
#define DISCRETE_DISABLE_TIMEOUT_MS 2000u

typedef struct {
    bool     enabled;
    bool     disabling;     /* waiting for physical all-off confirmation */
    uint32_t disable_started_ms;
    uint16_t report_ms;
    uint32_t desired;       /* host-commanded relay bits                  */
    discrete_backend_state_t hw;
    bool     hw_seen;
    uint8_t  seq;           /* per-send counter, wraps                    */
    bool     state_pending; /* a STATE frame must go out ASAP             */
    uint32_t last_state_ms; /* tick of the last STATE sent                */
} discrete_t;

static discrete_t g_disc;
static const discrete_backend_ops_t *g_backend;

void discrete_glue_bind(const discrete_backend_ops_t *ops){
    g_backend = ops;
    g_disc.hw_seen = false;
}

/* Pull a lock-bounded snapshot from the platform worker. No UART work is
   performed here. A changed physical state schedules immediate telemetry. */
static void refresh_hardware(void){
    discrete_backend_state_t next = {0};
    bool available = g_backend != NULL && g_backend->get_state != NULL
                  && g_backend->get_state(&next);
    if(!available){
        next.link = false;
    }
    if(!next.link){
        next.input_valid = 0;
    }

    if(!g_disc.hw_seen
       || next.relay_state != g_disc.hw.relay_state
       || next.input_state != g_disc.hw.input_state
       || next.input_valid != g_disc.hw.input_valid
       || next.link != g_disc.hw.link){
        g_disc.hw = next;
        g_disc.hw_seen = true;
        if(g_disc.enabled){
            g_disc.state_pending = true;
        }
    }
}

static void emit_state(void){
    proto_discrete_state_t st;
    st.relay_state = g_disc.hw.relay_state;
    st.input_state = g_disc.hw.input_state;
    st.input_valid = g_disc.hw.input_valid;
    st.link        = g_disc.hw.link ? 1u : 0u;
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
    if(g_backend != NULL && g_backend->set_enabled != NULL){
        g_backend->set_enabled(false);
    }
}

void discrete_glue_reset(void){
    bool was_enabled = g_disc.enabled;
    uint32_t had_bits = g_disc.hw.relay_state;
    if(g_backend != NULL && g_backend->set_enabled != NULL){
        /* Session loss is a fail-safe edge. The worker keeps retrying the
           zero target independently of the now-closed Ethernet session. */
        g_backend->set_enabled(false);
    }
    g_disc.enabled     = false;
    g_disc.disabling   = false;
    g_disc.desired     = 0;
    g_disc.report_ms   = DISCRETE_DEFAULT_REPORT_MS;
    g_disc.state_pending = false;
    if(was_enabled || had_bits){
        LOG_INF("discrete: reset, requesting all relays off");
    }
}

uint32_t discrete_glue_setup(uint8_t enable, uint8_t flags, uint16_t report_ms){
    (void)flags;                        /* reserved, always 0 today */
    if(enable){
        g_disc.desired   = 0;
        g_disc.enabled   = true;
        g_disc.disabling = false;
        g_disc.report_ms = (report_ms != 0) ? report_ms : DISCRETE_DEFAULT_REPORT_MS;
        if(g_backend != NULL && g_backend->set_enabled != NULL){
            g_backend->set_enabled(true);
        }
        refresh_hardware();
        g_disc.state_pending = true;    /* baseline STATE follows the ACK */
        LOG_INF("discrete: enabled, report every %ums", g_disc.report_ms);
    }else{
        /* Disable: queue the fail-safe zero target. The loop remains active
           just long enough to send confirmed all-off readback (or a bounded
           link-down final snapshot) before stopping the heartbeat. */
        g_disc.desired     = 0;
        if(g_backend != NULL && g_backend->set_enabled != NULL){
            g_backend->set_enabled(false);
        }
        if(g_disc.enabled){
            g_disc.disabling = true;
            g_disc.disable_started_ms = lc_port_tick_ms();
            g_disc.state_pending = true;
        }else{
            g_disc.disabling = false;
        }
        LOG_INF("discrete: disabled, requesting all relays off");
    }
    return PROTO_ST_OK;
}

void discrete_glue_set(uint32_t apply_mask, uint32_t values){
    if(!g_disc.enabled || g_disc.disabling){
        return;
    }
    refresh_hardware();
    if(!g_disc.hw.link){
        /* While link-down writes are silently dropped — wire doc §9.3. The
           gateway keeps them pending and retries after STATE reports link. */
        return;
    }
    uint32_t next = (g_disc.desired & ~apply_mask) | (values & apply_mask);
    if(next != g_disc.desired){
        LOG_DBG("discrete: desired relays 0x%08x", (unsigned)next);
    }
    g_disc.desired = next;
    if(g_backend != NULL && g_backend->set_outputs != NULL){
        g_backend->set_outputs(apply_mask, values);
    }
}

void discrete_glue_loop(void){
    if(!g_disc.enabled){
        return;
    }
    refresh_hardware();
    if(g_disc.disabling){
        uint32_t now = lc_port_tick_ms();
        bool off_confirmed = g_disc.hw.link && g_disc.hw.relay_state == 0;
        bool timed_out = (uint32_t)(now - g_disc.disable_started_ms)
                      >= DISCRETE_DISABLE_TIMEOUT_MS;
        if(off_confirmed || timed_out){
            /* Force one final snapshot attempt, then stop regardless of UDP
               availability. Physical all-off retries remain backend-owned. */
            g_disc.state_pending = true;
            emit_state();
            g_disc.enabled = false;
            g_disc.disabling = false;
            return;
        }
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
