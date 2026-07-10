/* Discrete stub through the wire: SETUP/SET plumbing, DISCRETE_STATE frames
   over the UDP plane, all-off on session reset (§9). */

#include "comm_capture.h"
#include "comm_core.h"
#include "discrete_glue.h"
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

/* Fetch the next DISCRETE_STATE frame produced by discrete_glue_loop(). */
static const proto_discrete_state_t *next_state(void){
    cap_reset();
    discrete_glue_loop();
    uint8_t cmd = 0, plen = 0;
    const uint8_t *pl = cap_frame(&cap_udp, 0, &cmd, &plen, NULL);
    if(pl == NULL || cmd != CMD_DISCRETE_STATE || plen != 14){
        return NULL;
    }
    return (const proto_discrete_state_t *)pl;
}

int main(void){
    lc_port_init();
    lc_log_init();
    identity_init(MAC);
    cap_reset();
    comm_core_init(cap_ops());
    host_port_set_tick(0);

    /* Nothing before SETUP(enable=1). */
    cap_reset();
    discrete_glue_loop();
    CHECK_EQ_U32(cap_udp.frames, 0);

    /* Short SETUP -> COMM_ERR_PAYLOAD_TOO_SHORT. */
    uint8_t short_pl[2] = { 1, 0 };
    const uint8_t *pl = req(COMM_SRC_TCP, 1, CMD_DISCRETE_SETUP, short_pl, 2);
    CHECK(pl != NULL && ack_status(pl) == PROTO_ST_COMM_ERR_PAYLOAD_TOO_SHORT);

    /* SETUP over UDP is ignored (config is TCP-only). */
    proto_discrete_setup_t su = { .enable = 1, .flags = 0, .report_ms = 0 };
    pl = req(COMM_SRC_UDP, 2, CMD_DISCRETE_SETUP, (const uint8_t *)&su, sizeof(su));
    CHECK(pl == NULL);
    cap_reset();
    discrete_glue_loop();
    CHECK_EQ_U32(cap_udp.frames, 0);

    /* SETUP(enable=1, report_ms=0 -> 500) over TCP: ACK OK + baseline STATE. */
    pl = req(COMM_SRC_TCP, 3, CMD_DISCRETE_SETUP, (const uint8_t *)&su, sizeof(su));
    CHECK(pl != NULL && ack_status(pl) == PROTO_ST_OK);
    const proto_discrete_state_t *st = next_state();
    CHECK(st != NULL);
    CHECK_EQ_U32(st->relay_state, 0);
    CHECK_EQ_U32(st->input_state, 0);
    CHECK_EQ_U32(st->input_valid, 0);           /* nothing wired */
    CHECK_EQ_U32(st->link, 1);
    uint8_t seq0 = st->seq;

    /* DISCRETE_SET over TCP: masked merge + ACK + STATE confirming. */
    proto_discrete_set_t set = { .apply_mask = 0x0000000F, .values = 0x00000005 };
    pl = req(COMM_SRC_TCP, 4, CMD_DISCRETE_SET, (const uint8_t *)&set, sizeof(set));
    CHECK(pl != NULL && ack_status(pl) == PROTO_ST_OK);
    st = next_state();
    CHECK(st != NULL);
    CHECK_EQ_U32(st->relay_state, 0x5);
    CHECK((uint8_t)(st->seq - seq0) == 1);      /* seq increments per send */

    /* DISCRETE_SET over UDP: fire-and-forget (no ACK), still applied.
       Masked merge: only bit 1 touched. */
    set.apply_mask = 0x00000002; set.values = 0x00000002;
    pl = req(COMM_SRC_UDP, 5, CMD_DISCRETE_SET, (const uint8_t *)&set, sizeof(set));
    CHECK(pl == NULL);
    st = next_state();
    CHECK(st != NULL);
    CHECK_EQ_U32(st->relay_state, 0x7);

    /* Heartbeat: no change -> silent until report_ms elapses. */
    cap_reset();
    discrete_glue_loop();
    CHECK_EQ_U32(cap_udp.frames, 0);
    host_port_set_tick(lc_port_tick_ms() + 600);
    st = next_state();
    CHECK(st != NULL);
    CHECK_EQ_U32(st->relay_state, 0x7);

    /* SETUP(enable=0): relays off + final STATE, heartbeat stops. */
    su.enable = 0;
    cap_reset();
    uint16_t flen = cap_build_req(g_frame, 6, CMD_DISCRETE_SETUP,
                                  (const uint8_t *)&su, sizeof(su));
    comm_core_input(g_frame, flen, COMM_SRC_TCP);
    CHECK(cap_frame(&cap_tcp, 0, NULL, NULL, NULL) != NULL);
    uint8_t cmd = 0, plen2 = 0;
    const uint8_t *spl = cap_frame(&cap_udp, 0, &cmd, &plen2, NULL);
    CHECK(spl != NULL && cmd == CMD_DISCRETE_STATE);
    CHECK_EQ_U32(((const proto_discrete_state_t *)spl)->relay_state, 0);
    host_port_set_tick(lc_port_tick_ms() + 600);
    cap_reset();
    discrete_glue_loop();
    CHECK_EQ_U32(cap_udp.frames, 0);

    /* Writes while disabled are silently dropped. */
    set.apply_mask = 0xFFFFFFFF; set.values = 0xFFFFFFFF;
    (void)req(COMM_SRC_UDP, 7, CMD_DISCRETE_SET, (const uint8_t *)&set, sizeof(set));
    su.enable = 1;
    pl = req(COMM_SRC_TCP, 8, CMD_DISCRETE_SETUP, (const uint8_t *)&su, sizeof(su));
    CHECK(pl != NULL);
    st = next_state();
    CHECK(st != NULL);
    CHECK_EQ_U32(st->relay_state, 0);           /* dropped write did not stick */

    /* TCP session loss: subsystem disabled, relays off (§9.5 trigger 3). */
    set.apply_mask = 0x1; set.values = 0x1;
    (void)req(COMM_SRC_TCP, 9, CMD_DISCRETE_SET, (const uint8_t *)&set, sizeof(set));
    comm_core_session(false);
    host_port_set_tick(lc_port_tick_ms() + 600);
    cap_reset();
    discrete_glue_loop();
    CHECK_EQ_U32(cap_udp.frames, 0);            /* disabled: no heartbeat */

    return test_report("discrete");
}
