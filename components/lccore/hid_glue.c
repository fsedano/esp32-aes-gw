/**
  ******************************************************************************
  * @file    hid_glue.c
  * @brief   USB HID joystick subsystem: desired/confirmed report state
  *          machine over the hid_port seam (see hid_glue.h).
  ******************************************************************************
  */

#ifndef RECOVERY_BUILD

#include "hid_glue.h"
#include "hid_port.h"
#include "comm_core.h"
#include "lc_log.h"
#include "lc_port.h"
#include "proto.h"

#include <stdbool.h>
#include <string.h>

#define HID_DEFAULT_REPORT_MS   500u

typedef struct {
    bool     enabled;
    uint16_t report_ms;
    int16_t  axes[HID_NUM_AXES];    /* confirmed = latched into hid_port  */
    uint32_t buttons;
    bool     mounted_last;          /* edge detect for STATE-on-change    */
    uint8_t  seq;                   /* per-send counter, wraps            */
    bool     state_pending;         /* a STATE frame must go out ASAP     */
    uint32_t last_state_ms;         /* tick of the last STATE sent        */
} hid_t;

static hid_t g_hid;

static void emit_state(void){
    proto_hid_state_t st;
    memcpy(st.axes, g_hid.axes, sizeof(st.axes));
    st.buttons     = g_hid.buttons;
    st.usb_mounted = hid_port_mounted() ? 1 : 0;
    st.seq         = g_hid.seq;
    if(comm_core_send_udp(CMD_HID_STATE, (const uint8_t *)&st, sizeof(st))){
        g_hid.seq++;
        g_hid.state_pending = false;
        g_hid.last_state_ms = lc_port_tick_ms();
    }
    /* Send failed (host UDP endpoint not latched yet): stay pending; the
       loop retries once the endpoint is known. */
}

static void submit_current(void){
    hid_port_submit(g_hid.axes, g_hid.buttons);
}

void hid_glue_init(void){
    memset(&g_hid, 0, sizeof(g_hid));
    g_hid.report_ms    = HID_DEFAULT_REPORT_MS;
    g_hid.mounted_last = hid_port_mounted();
}

void hid_glue_reset(void){
    bool had_state = false;
    for(int i = 0; i < HID_NUM_AXES; i++){
        if(g_hid.axes[i] != 0){
            had_state = true;
        }
    }
    had_state = had_state || g_hid.buttons != 0;
    bool was_enabled = g_hid.enabled;

    g_hid.enabled   = false;
    memset(g_hid.axes, 0, sizeof(g_hid.axes));
    g_hid.buttons   = 0;
    g_hid.report_ms = HID_DEFAULT_REPORT_MS;
    /* Center the joystick on USB even though no STATE goes out (the control
       host is gone; the Windows PC is not). */
    submit_current();
    if(was_enabled || had_state){
        LOG_INF("hid: reset, axes centered, buttons released");
    }
}

uint32_t hid_glue_setup(uint8_t enable, uint8_t flags, uint16_t report_ms){
    (void)flags;                        /* reserved, always 0 today */
    if(enable){
        g_hid.enabled   = true;
        g_hid.report_ms = (report_ms != 0) ? report_ms : HID_DEFAULT_REPORT_MS;
        g_hid.state_pending = true;     /* baseline STATE follows the ACK */
        submit_current();               /* re-latch, harmless */
        LOG_INF("hid: enabled, report every %ums", g_hid.report_ms);
    }else{
        /* Disable: center/release, one final STATE, heartbeat stops. */
        memset(g_hid.axes, 0, sizeof(g_hid.axes));
        g_hid.buttons = 0;
        submit_current();
        if(g_hid.enabled){
            g_hid.state_pending = true;
            emit_state();
        }
        g_hid.enabled = false;
        LOG_INF("hid: disabled, axes centered, buttons released");
    }
    return PROTO_ST_OK;
}

void hid_glue_set(uint8_t axis_mask, const int16_t axes[HID_NUM_AXES],
                  uint32_t btn_apply_mask, uint32_t btn_values){
    if(!g_hid.enabled){
        /* While disabled writes are silently dropped — wire doc §12. */
        return;
    }
    bool changed = false;
    for(int i = 0; i < HID_NUM_AXES; i++){
        if((axis_mask & (1u << i)) && g_hid.axes[i] != axes[i]){
            g_hid.axes[i] = axes[i];
            changed = true;
        }
    }
    uint32_t next = (g_hid.buttons & ~btn_apply_mask) | (btn_values & btn_apply_mask);
    if(next != g_hid.buttons){
        g_hid.buttons = next;
        changed = true;
    }
    if(changed){
        /* The latch cannot fail: writes confirm immediately, like the
           discrete stub. usb_mounted carries the truth about whether a PC
           is actually reading the report. */
        submit_current();
        g_hid.state_pending = true;
    }
}

void hid_glue_loop(void){
    /* Always pump the USB layer, even while disabled: a reset-to-center
       report must drain after disable/session drop, and after a remount. */
    hid_port_loop();

    bool mounted = hid_port_mounted();
    if(mounted != g_hid.mounted_last){
        g_hid.mounted_last = mounted;
        if(g_hid.enabled){
            g_hid.state_pending = true;
        }
    }

    if(!g_hid.enabled){
        return;
    }
    if(g_hid.state_pending){
        emit_state();
        return;
    }
    uint32_t now = lc_port_tick_ms();
    if((uint32_t)(now - g_hid.last_state_ms) >= g_hid.report_ms){
        emit_state();
    }
}

#endif /* !RECOVERY_BUILD */
