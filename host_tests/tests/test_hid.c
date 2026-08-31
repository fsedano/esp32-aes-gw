/* HID joystick glue through the wire: SETUP/SET plumbing, HID_STATE frames
   over the UDP plane, USB centering on disable/session reset (§12). */

#include "comm_capture.h"
#include "comm_core.h"
#include "hid_glue.h"
#include "hid_port_stub.h"
#include "identity.h"
#include "lc_log.h"
#include "lc_port.h"
#include "proto.h"
#include "test_util.h"

#include <string.h>

void host_port_set_tick(uint32_t ms);

static const uint8_t MAC[6] = { 0x24, 0x0A, 0xC4, 0x11, 0x22, 0x33 };
static uint8_t g_frame[PROTO_MAX_PACKET];

static const uint8_t *req(uint8_t src, uint8_t pkt_id, uint8_t cmd,
                          const uint8_t *payload, uint8_t len){
    cap_reset();
    uint16_t flen = cap_build_req(g_frame, pkt_id, cmd, payload, len);
    comm_core_input(g_frame, flen, src);
    return cap_frame(&cap_tcp, 0, NULL, NULL, NULL);
}

static uint32_t ack_status(const uint8_t *pl){
    return (uint32_t)pl[1] | ((uint32_t)pl[2] << 8)
         | ((uint32_t)pl[3] << 16) | ((uint32_t)pl[4] << 24);
}

/* Fetch the next HID_STATE frame produced by hid_glue_loop(). The payload
   may be unaligned in the capture buffer, so copy it out. */
static const proto_hid_state_t *next_state(void){
    static proto_hid_state_t st;
    cap_reset();
    hid_glue_loop();
    uint8_t cmd = 0, plen = 0;
    const uint8_t *pl = cap_frame(&cap_udp, 0, &cmd, &plen, NULL);
    if(pl == NULL || cmd != CMD_HID_STATE || plen != sizeof(st)){
        return NULL;
    }
    memcpy(&st, pl, sizeof(st));
    return &st;
}

int main(void){
    lc_port_init();
    lc_log_init();
    identity_init(MAC);
    cap_reset();
    comm_core_init(cap_ops());
    host_port_set_tick(0);

    /* 1. Nothing before SETUP(enable=1); the USB pump still runs. */
    int loops0 = hid_stub_loops;
    cap_reset();
    hid_glue_loop();
    CHECK_EQ_U32(cap_udp.frames, 0);
    CHECK(hid_stub_loops == loops0 + 1);

    /* 2. Short SETUP -> COMM_ERR_PAYLOAD_TOO_SHORT. */
    uint8_t short_pl[2] = { 1, 0 };
    const uint8_t *pl = req(COMM_SRC_TCP, 1, CMD_HID_SETUP, short_pl, 2);
    CHECK(pl != NULL && ack_status(pl) == PROTO_ST_COMM_ERR_PAYLOAD_TOO_SHORT);

    /* 3. SETUP over UDP is ignored (config is TCP-only). */
    proto_hid_setup_t su = { .enable = 1, .flags = 0, .report_ms = 0 };
    pl = req(COMM_SRC_UDP, 2, CMD_HID_SETUP, (const uint8_t *)&su, sizeof(su));
    CHECK(pl == NULL);
    cap_reset();
    hid_glue_loop();
    CHECK_EQ_U32(cap_udp.frames, 0);

    /* 4. SETUP(enable=1, report_ms=0 -> 500) over TCP: ACK OK + baseline
       STATE (centered, released, unmounted stub). */
    pl = req(COMM_SRC_TCP, 3, CMD_HID_SETUP, (const uint8_t *)&su, sizeof(su));
    CHECK(pl != NULL && ack_status(pl) == PROTO_ST_OK);
    const proto_hid_state_t *st = next_state();
    CHECK(st != NULL);
    for(int i = 0; i < HID_NUM_AXES; i++){
        CHECK_EQ_U32((uint32_t)(uint16_t)st->axes[i], 0);
    }
    CHECK_EQ_U32(st->buttons, 0);
    CHECK_EQ_U32(st->usb_mounted, 0);
    uint8_t seq0 = st->seq;

    /* 5. HID_SET over TCP: masked axes + masked button merge, ACK + STATE,
       and the same values latched into the USB stub. */
    proto_hid_set_t set;
    memset(&set, 0, sizeof(set));
    set.axis_mask      = 0x05;              /* axes 0 and 2 */
    set.axes[0]        = 1000;
    set.axes[2]        = -32767;
    set.axes[3]        = 4444;              /* not in mask: must be ignored */
    set.btn_apply_mask = 0x0000000F;
    set.btn_values     = 0x00000005;
    pl = req(COMM_SRC_TCP, 4, CMD_HID_SET, (const uint8_t *)&set, sizeof(set));
    CHECK(pl != NULL && ack_status(pl) == PROTO_ST_OK);
    st = next_state();
    CHECK(st != NULL);
    CHECK(st->axes[0] == 1000);
    CHECK(st->axes[2] == -32767);
    CHECK(st->axes[3] == 0);                /* unmasked axis untouched */
    CHECK_EQ_U32(st->buttons, 0x5);
    CHECK((uint8_t)(st->seq - seq0) == 1);  /* seq increments per send */
    CHECK(hid_stub_axes[0] == 1000 && hid_stub_axes[2] == -32767);
    CHECK_EQ_U32(hid_stub_buttons, 0x5);

    /* 6. Short HID_SET on TCP -> ACK TOO_SHORT, no state change. */
    pl = req(COMM_SRC_TCP, 5, CMD_HID_SET, (const uint8_t *)&set, sizeof(set) - 1);
    CHECK(pl != NULL && ack_status(pl) == PROTO_ST_COMM_ERR_PAYLOAD_TOO_SHORT);
    cap_reset();
    hid_glue_loop();
    CHECK_EQ_U32(cap_udp.frames, 0);        /* nothing became pending */

    /* 7. HID_SET over UDP: fire-and-forget (no ACK), still applied. Masked
       merge only touches masked bits. */
    memset(&set, 0, sizeof(set));
    set.axis_mask      = 0x02;
    set.axes[1]        = -2000;
    set.btn_apply_mask = 0x00000002;
    set.btn_values     = 0x00000002;
    pl = req(COMM_SRC_UDP, 6, CMD_HID_SET, (const uint8_t *)&set, sizeof(set));
    CHECK(pl == NULL);
    st = next_state();
    CHECK(st != NULL);
    CHECK(st->axes[0] == 1000 && st->axes[1] == -2000 && st->axes[2] == -32767);
    CHECK_EQ_U32(st->buttons, 0x7);

    /* 8. No-change SET: ACKed but no STATE emitted. */
    pl = req(COMM_SRC_TCP, 7, CMD_HID_SET, (const uint8_t *)&set, sizeof(set));
    CHECK(pl != NULL && ack_status(pl) == PROTO_ST_OK);
    cap_reset();
    hid_glue_loop();
    CHECK_EQ_U32(cap_udp.frames, 0);

    /* 9. Heartbeat: silent until report_ms elapses. */
    host_port_set_tick(lc_port_tick_ms() + 600);
    st = next_state();
    CHECK(st != NULL);
    CHECK_EQ_U32(st->buttons, 0x7);

    /* 10. USB mount change triggers a STATE. */
    hid_port_stub_set_mounted(true);
    st = next_state();
    CHECK(st != NULL);
    CHECK_EQ_U32(st->usb_mounted, 1);

    /* 11. SETUP(enable=0): final all-zero STATE, USB centered, heartbeat
       stops. */
    su.enable = 0;
    cap_reset();
    uint16_t flen = cap_build_req(g_frame, 8, CMD_HID_SETUP,
                                  (const uint8_t *)&su, sizeof(su));
    comm_core_input(g_frame, flen, COMM_SRC_TCP);
    CHECK(cap_frame(&cap_tcp, 0, NULL, NULL, NULL) != NULL);
    uint8_t cmd = 0, plen2 = 0;
    const uint8_t *spl = cap_frame(&cap_udp, 0, &cmd, &plen2, NULL);
    CHECK(spl != NULL && cmd == CMD_HID_STATE);
    proto_hid_state_t fin;
    memcpy(&fin, spl, sizeof(fin));
    for(int i = 0; i < HID_NUM_AXES; i++){
        CHECK(fin.axes[i] == 0);
    }
    CHECK_EQ_U32(fin.buttons, 0);
    CHECK(hid_stub_axes[1] == 0);
    CHECK_EQ_U32(hid_stub_buttons, 0);
    host_port_set_tick(lc_port_tick_ms() + 600);
    cap_reset();
    hid_glue_loop();
    CHECK_EQ_U32(cap_udp.frames, 0);

    /* 12. Writes while disabled are silently dropped. */
    memset(&set, 0, sizeof(set));
    set.axis_mask      = 0xFF;
    set.axes[0]        = 3210;
    set.btn_apply_mask = 0xFFFFFFFF;
    set.btn_values     = 0xFFFFFFFF;
    (void)req(COMM_SRC_UDP, 9, CMD_HID_SET, (const uint8_t *)&set, sizeof(set));
    su.enable = 1;
    pl = req(COMM_SRC_TCP, 10, CMD_HID_SETUP, (const uint8_t *)&su, sizeof(su));
    CHECK(pl != NULL);
    st = next_state();
    CHECK(st != NULL);
    CHECK(st->axes[0] == 0);                /* dropped write did not stick */
    CHECK_EQ_U32(st->buttons, 0);

    /* 13. TCP session loss: subsystem disabled, USB centered/released. */
    memset(&set, 0, sizeof(set));
    set.axis_mask      = 0x01;
    set.axes[0]        = 12345;
    set.btn_apply_mask = 0x1;
    set.btn_values     = 0x1;
    (void)req(COMM_SRC_TCP, 11, CMD_HID_SET, (const uint8_t *)&set, sizeof(set));
    CHECK(hid_stub_axes[0] == 12345);
    comm_core_session(false);
    CHECK(hid_stub_axes[0] == 0);           /* centered on session drop */
    CHECK_EQ_U32(hid_stub_buttons, 0);
    host_port_set_tick(lc_port_tick_ms() + 600);
    cap_reset();
    hid_glue_loop();
    CHECK_EQ_U32(cap_udp.frames, 0);        /* disabled: no heartbeat */

    return test_report("hid");
}
