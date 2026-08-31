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

typedef struct {
    uint32_t base_channel;
    uint16_t bit_count;
    uint32_t relay_state;
    uint32_t input_state;
    uint32_t input_valid;
    uint8_t link;
    uint8_t seq;
} state_view_t;

static uint32_t bitmap_u32(const uint8_t *bitmap, uint16_t bytes){
    uint32_t value = 0;
    for(uint16_t i = 0; i < bytes && i < 4u; i++){
        value |= (uint32_t)bitmap[i] << (8u * i);
    }
    return value;
}

/* Fetch the next DISCRETE_STATE frame produced by discrete_glue_loop(). */
static const state_view_t *next_state(void){
    static state_view_t state;
    cap_reset();
    discrete_glue_loop();
    uint8_t cmd = 0, plen = 0;
    const uint8_t *pl = cap_frame(&cap_udp, 0, &cmd, &plen, NULL);
    if(pl == NULL || cmd != CMD_DISCRETE_STATE
       || plen < PROTO_DISCRETE_STATE_HEADER_SIZE){
        return NULL;
    }
    uint16_t count = (uint16_t)pl[4] | ((uint16_t)pl[5] << 8);
    uint16_t bytes = PROTO_DISCRETE_BITMAP_BYTES(count);
    if(count == 0u || count > 32u
       || plen != PROTO_DISCRETE_STATE_HEADER_SIZE + 3u * bytes){
        return NULL;
    }
    state.base_channel = (uint32_t)pl[0] | ((uint32_t)pl[1] << 8)
                       | ((uint32_t)pl[2] << 16) | ((uint32_t)pl[3] << 24);
    state.bit_count = count;
    state.link = pl[6];
    state.seq = pl[7];
    state.relay_state = bitmap_u32(pl + 8, bytes);
    state.input_state = bitmap_u32(pl + 8 + bytes, bytes);
    state.input_valid = bitmap_u32(pl + 8 + 2u * bytes, bytes);
    return &state;
}

static uint8_t build_set(uint8_t out[14], uint32_t base, uint16_t count,
                         uint32_t apply, uint32_t values){
    uint16_t bytes = PROTO_DISCRETE_BITMAP_BYTES(count);
    memset(out, 0, 14);
    out[0] = (uint8_t)base;
    out[1] = (uint8_t)(base >> 8);
    out[2] = (uint8_t)(base >> 16);
    out[3] = (uint8_t)(base >> 24);
    out[4] = (uint8_t)count;
    out[5] = (uint8_t)(count >> 8);
    for(uint16_t i = 0; i < bytes && i < 4u; i++){
        out[6 + i] = (uint8_t)(apply >> (8u * i));
        out[6 + bytes + i] = (uint8_t)(values >> (8u * i));
    }
    return (uint8_t)(PROTO_DISCRETE_SET_HEADER_SIZE + 2u * bytes);
}

int main(void){
    lc_port_init();
    lc_log_init();
    identity_init(MAC);
    memset(&g_fake, 0, sizeof(g_fake));
    g_fake.state.input_valid = 1;
    g_fake.state.link = true;
    discrete_glue_bind(&FAKE_OPS);
    discrete_glue_configure(1, 16);
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
    const state_view_t *st = next_state();
    CHECK(st != NULL);
    CHECK_EQ_U32(st->base_channel, 0);
    CHECK_EQ_U32(st->bit_count, 16);
    CHECK_EQ_U32(st->relay_state, 0);
    CHECK_EQ_U32(st->input_state, 0);           /* one input, constant idle */
    CHECK_EQ_U32(st->input_valid, 1);           /* input 0 present + valid
                                                   (gateway registers
                                                   Extra{Inputs:1}) */
    CHECK_EQ_U32(st->link, 1);
    uint8_t state_cmd = 0, state_len = 0;
    const uint8_t *state_payload = cap_frame(&cap_udp, 0, &state_cmd,
                                             &state_len, NULL);
    static const uint8_t state_golden[] = {
        0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x01, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x01, 0x00,
    };
    CHECK(state_payload != NULL && state_cmd == CMD_DISCRETE_STATE);
    CHECK_EQ_U32(state_len, sizeof(state_golden));
    CHECK_MEM(state_payload, state_golden, sizeof(state_golden));
    uint8_t seq0 = st->seq;

    /* A SET updates backend intent but must not optimistically report the
       relay. Only a later physical snapshot confirms it. */
    uint8_t set[14];
    uint8_t set_len = build_set(set, 0, 16, 0x0000000F, 0x00000005);
    static const uint8_t set_golden[] = {
        0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
        0x0F, 0x00, 0x05, 0x00,
    };
    CHECK_EQ_U32(set_len, sizeof(set_golden));
    CHECK_MEM(set, set_golden, sizeof(set_golden));
    pl = req(COMM_SRC_TCP, 4, CMD_DISCRETE_SET, set, set_len);
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
    set_len = build_set(set, 1, 3, 0x7, 0x3);
    pl = req(COMM_SRC_UDP, 5, CMD_DISCRETE_SET, set, set_len);
    CHECK(pl == NULL);
    CHECK_EQ_U32(g_fake.desired, 0x7);
    g_fake.state.relay_state = 0x7;
    st = next_state();
    CHECK(st != NULL);
    CHECK_EQ_U32(st->relay_state, 0x7);

    /* Malformed ranges return the payload error on TCP; UDP remains silent.
       A valid range outside the declared 16 outputs has no backend effect. */
    uint8_t malformed[8] = {0};
    pl = req(COMM_SRC_TCP, 51, CMD_DISCRETE_SET, malformed, 5);
    CHECK(pl != NULL && ack_status(pl) == PROTO_ST_COMM_ERR_PAYLOAD_TOO_SHORT);
    pl = req(COMM_SRC_TCP, 52, CMD_DISCRETE_SET, malformed, 6); /* count=0 */
    CHECK(pl != NULL && ack_status(pl) == PROTO_ST_COMM_ERR_PAYLOAD_TOO_SHORT);
    malformed[4] = 9; /* wants two apply + two value bytes, only two follow */
    pl = req(COMM_SRC_TCP, 53, CMD_DISCRETE_SET, malformed, sizeof(malformed));
    CHECK(pl != NULL && ack_status(pl) == PROTO_ST_COMM_ERR_PAYLOAD_TOO_SHORT);
    unsigned calls_before_oor = g_fake.set_calls;
    set_len = build_set(set, 16, 1, 1, 1);
    pl = req(COMM_SRC_TCP, 54, CMD_DISCRETE_SET, set, set_len);
    CHECK(pl != NULL && ack_status(pl) == PROTO_ST_OK);
    CHECK_EQ_U32(g_fake.set_calls, calls_before_oor);

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
    set_len = build_set(set, 0, 1, 0x1, 0x1);
    (void)req(COMM_SRC_UDP, 60, CMD_DISCRETE_SET,
              set, set_len);
    CHECK_EQ_U32(g_fake.set_calls, calls_at_disable);
    g_fake.state.relay_state = 0;
    st = next_state();
    CHECK(st != NULL && st->relay_state == 0);
    host_port_set_tick(lc_port_tick_ms() + 600);
    cap_reset();
    discrete_glue_loop();
    CHECK_EQ_U32(cap_udp.frames, 0);

    /* Writes while disabled are silently dropped. */
    set_len = build_set(set, 0, 32, UINT32_MAX, UINT32_MAX);
    (void)req(COMM_SRC_UDP, 7, CMD_DISCRETE_SET, set, set_len);
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
    set_len = build_set(set, 0, 1, 0x1, 0x1);
    (void)req(COMM_SRC_UDP, 9, CMD_DISCRETE_SET, set, set_len);
    CHECK_EQ_U32(g_fake.set_calls, calls_before);

    /* On recovery the gateway retry is accepted and physical readback is
       still required before STATE confirms it. */
    g_fake.state.link = true;
    g_fake.state.input_valid = 1;
    st = next_state();
    CHECK(st != NULL && st->link == 1);
    (void)req(COMM_SRC_UDP, 10, CMD_DISCRETE_SET, set, set_len);
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
