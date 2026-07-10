/* Log ring -> CMD_LOG_MSG payload builder, pinned to the golden fixture in
   WIRE_PROTOCOL.md §10.2 (shared with the STM32 firmware's test_log_ring.c
   and the gateway's proto tests — all sides MUST reproduce these bytes). */

#include "lc_log.h"
#include "lc_port.h"
#include "proto.h"
#include "test_util.h"

#include <string.h>

void host_port_set_tick(uint32_t ms);

int main(void){
    lc_port_init();
    lc_log_init();

    /* Golden fixture: seq=0x0102, dropped=0, INFO "boot ok" @1000,
       DEBUG "dbg" @1001. */
    lc_log_test_reset(0x0102);
    lc_log_set_level(LOG_LEVEL_DEBUG);      /* store DEBUG lines too */
    host_port_set_tick(1000);
    lc_log_write(LOG_LEVEL_INFO, "boot ok");
    host_port_set_tick(1001);
    lc_log_write(LOG_LEVEL_DEBUG, "dbg");

    uint8_t payload[PROTO_MAX_PAYLOAD];
    uint8_t n = lc_log_net_build_packet(payload, sizeof(payload));

    const uint8_t want[26] = {
        0x02, 0x01, 0x00, 0x00,                         /* seq, dropped */
        0x01, 0xE8, 0x03, 0x00, 0x00, 0x07,             /* INFO t=1000 len=7 */
        0x62, 0x6F, 0x6F, 0x74, 0x20, 0x6F, 0x6B,       /* "boot ok" */
        0x00, 0xE9, 0x03, 0x00, 0x00, 0x03,             /* DEBUG t=1001 len=3 */
        0x64, 0x62, 0x67,                               /* "dbg" */
    };
    CHECK_EQ_U32(n, sizeof(want));
    CHECK_MEM(payload, want, sizeof(want));

    /* Ring drained; next build returns 0 and does not consume a seq. */
    CHECK_EQ_U32(lc_log_net_build_packet(payload, sizeof(payload)), 0);
    CHECK(!lc_log_net_pending(NULL));

    /* seq advanced by exactly one datagram. */
    host_port_set_tick(1002);
    lc_log_write(LOG_LEVEL_INFO, "x");
    n = lc_log_net_build_packet(payload, sizeof(payload));
    CHECK(n == 4 + 6 + 1);
    CHECK(payload[0] == 0x03 && payload[1] == 0x01);    /* seq = 0x0103 */

    /* Threshold: INFO threshold drops DEBUG lines. */
    lc_log_set_level(LOG_LEVEL_INFO);
    lc_log_write(LOG_LEVEL_DEBUG, "invisible");
    CHECK(!lc_log_net_pending(NULL));
    lc_log_write(LOG_LEVEL_INFO, "visible");
    CHECK(lc_log_net_pending(NULL));
    (void)lc_log_net_build_packet(payload, sizeof(payload));

    /* Trailing \r\n stripped; long lines truncated + flagged. */
    lc_log_test_reset(0);
    host_port_set_tick(5);
    lc_log_write(LOG_LEVEL_INFO, "line\r\n");
    n = lc_log_net_build_packet(payload, sizeof(payload));
    CHECK(n == 4 + 6 + 4);
    CHECK(payload[9] == 4);                             /* text_len */
    CHECK_MEM(payload + 10, "line", 4);

    lc_log_test_reset(0);
    char big[200];
    memset(big, 'A', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    lc_log_write(LOG_LEVEL_INFO, "%s", big);
    n = lc_log_net_build_packet(payload, sizeof(payload));
    CHECK(n == 4 + 6 + PROTO_LOG_MAX_TEXT);
    CHECK((payload[4] & PROTO_LOG_FLAG_TRUNCATED) != 0);
    CHECK(payload[9] == PROTO_LOG_MAX_TEXT);

    /* Overflow: drop-oldest + dropped counter on the wire. */
    lc_log_test_reset(0);
    for(int i = 0; i < 100; i++){
        lc_log_write(LOG_LEVEL_INFO, "%03d-%s", i, big);    /* ~128B each */
    }
    n = lc_log_net_build_packet(payload, sizeof(payload));
    CHECK(n > 4);
    uint16_t dropped = (uint16_t)payload[2] | ((uint16_t)payload[3] << 8);
    CHECK(dropped > 0);
    /* Oldest surviving record is not record 0. */
    CHECK(memcmp(payload + 10, "000-", 4) != 0);

    return test_report("log_ring");
}
