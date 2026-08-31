/* Discrete backend through the wire: asynchronous physical confirmation,
   link qualification, STATE telemetry and fail-safe reset behavior (§9). */

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

typedef struct {
    bool enabled;
    uint32_t desired;
    unsigned set_calls;
    discrete_backend_state_t state;
} fake_backend_t;

static fake_backend_t g_fake;

static void fake_set_enabled(bool enabled){
    g_fake.enabled = enabled;
    g_fake.desired = 0;
}

static void fake_set_outputs(uint32_t apply_mask, uint32_t values){
    if(g_fake.enabled && g_fake.state.link){
        g_fake.desired = (g_fake.desired & ~apply_mask) | (values & apply_mask);
        g_fake.set_calls++;
    }
}

static bool fake_get_state(discrete_backend_state_t *state){
    *state = g_fake.state;
    return true;
}

static const discrete_backend_ops_t FAKE_OPS = {
    .set_enabled = fake_set_enabled,
    .set_outputs = fake_set_outputs,
    .get_state = fake_get_state,
};

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
    memset(&g_fake, 0, sizeof(g_fake));
    g_fake.state.input_valid = 1;
    g_fake.state.link = true;
    discrete_glue_bind(&FAKE_OPS);
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
    CHECK_EQ_U32(st->input_state, 0);           /* one input, constant idle */
    CHECK_EQ_U32(st->input_valid, 1);           /* input 0 present + valid
                                                   (gateway registers
                                                   Extra{Inputs:1}) */
    CHECK_EQ_U32(st->link, 1);
    uint8_t seq0 = st->seq;

    /* A SET updates backend intent but must not optimistically report the
       relay. Only a later physical snapshot confirms it. */
    proto_discrete_set_t set = { .apply_mask = 0x0000000F, .values = 0x00000005 };
    pl = req(COMM_SRC_TCP, 4, CMD_DISCRETE_SET, (const uint8_t *)&set, sizeof(set));
    CHECK(pl != NULL && ack_status(pl) == PROTO_ST_OK);
    CHECK_EQ_U32(g_fake.desired, 0x5);
    CHECK_EQ_U32(g_fake.set_calls, 1);
    cap_reset();
    discrete_glue_loop();
    CHECK_EQ_U32(cap_udp.frames, 0);            /* no FC01 confirmation yet */
    g_fake.state.relay_state = 0x5;
    st = next_state();
    CHECK(st != NULL);
    CHECK_EQ_U32(st->relay_state, 0x5);
    CHECK((uint8_t)(st->seq - seq0) == 1);      /* seq increments per send */

    /* DISCRETE_SET over UDP: fire-and-forget (no ACK), still applied.
       Masked merge: only bit 1 touched. */
    set.apply_mask = 0x00000002; set.values = 0x00000002;
    pl = req(COMM_SRC_UDP, 5, CMD_DISCRETE_SET, (const uint8_t *)&set, sizeof(set));
    CHECK(pl == NULL);
    CHECK_EQ_U32(g_fake.desired, 0x7);
    g_fake.state.relay_state = 0x7;
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

    /* SETUP(enable=0): backend is immediately fail-safe disabled; heartbeat
       stops after a physical all-off snapshot is observed. */
    su.enable = 0;
    cap_reset();
    uint16_t flen = cap_build_req(g_frame, 6, CMD_DISCRETE_SETUP,
                                  (const uint8_t *)&su, sizeof(su));
    comm_core_input(g_frame, flen, COMM_SRC_TCP);
    CHECK(cap_frame(&cap_tcp, 0, NULL, NULL, NULL) != NULL);
    CHECK(!g_fake.enabled);
    CHECK_EQ_U32(g_fake.desired, 0);
    unsigned calls_at_disable = g_fake.set_calls;
    set.apply_mask = 0x1; set.values = 0x1;
    (void)req(COMM_SRC_UDP, 60, CMD_DISCRETE_SET,
              (const uint8_t *)&set, sizeof(set));
    CHECK_EQ_U32(g_fake.set_calls, calls_at_disable);
    g_fake.state.relay_state = 0;
    st = next_state();
    CHECK(st != NULL && st->relay_state == 0);
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

    /* Physical link loss is reported immediately by the next loop. Writes
       received while down are dropped so reconnect cannot replay stale I/O. */
    g_fake.state.link = false;
    g_fake.state.input_valid = 1;               /* glue must suppress it */
    st = next_state();
    CHECK(st != NULL);
    CHECK_EQ_U32(st->link, 0);
    CHECK_EQ_U32(st->input_valid, 0);
    unsigned calls_before = g_fake.set_calls;
    set.apply_mask = 0x1; set.values = 0x1;
    (void)req(COMM_SRC_UDP, 9, CMD_DISCRETE_SET, (const uint8_t *)&set, sizeof(set));
    CHECK_EQ_U32(g_fake.set_calls, calls_before);

    /* On recovery the gateway retry is accepted and physical readback is
       still required before STATE confirms it. */
    g_fake.state.link = true;
    g_fake.state.input_valid = 1;
    st = next_state();
    CHECK(st != NULL && st->link == 1);
    (void)req(COMM_SRC_UDP, 10, CMD_DISCRETE_SET, (const uint8_t *)&set, sizeof(set));
    CHECK_EQ_U32(g_fake.set_calls, calls_before + 1);
    g_fake.state.relay_state = 1;
    st = next_state();
    CHECK(st != NULL && st->relay_state == 1);

    /* TCP session loss: subsystem disabled, relays off (§9.5 trigger 3). */
    comm_core_session(false);
    CHECK(!g_fake.enabled);
    CHECK_EQ_U32(g_fake.desired, 0);
    host_port_set_tick(lc_port_tick_ms() + 600);
    cap_reset();
    discrete_glue_loop();
    CHECK_EQ_U32(cap_udp.frames, 0);            /* disabled: no heartbeat */

    return test_report("discrete");
}
