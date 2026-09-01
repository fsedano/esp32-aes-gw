/* Framing: parse, checksum, one-byte resync, chunk tolerance, and the exact
   on-wire ACK bytes from WIRE_PROTOCOL.md §4.1. */

#include "comm_capture.h"
#include "comm_core.h"
#include "identity.h"
#include "lc_log.h"
#include "lc_port.h"
#include "proto.h"
#include "test_util.h"

#include <string.h>

static const uint8_t MAC[6] = { 0x24, 0x0A, 0xC4, 0x11, 0x22, 0x33 };

int main(void){
    lc_port_init();
    lc_log_init();
    identity_init(MAC);
    cap_reset();
    comm_core_init(cap_ops());

    uint8_t frame[PROTO_MAX_PACKET];
    uint8_t buf[1024];

    /* --- golden PING round trip (§4.1's minimal OK ACK) ------------------ */
    uint16_t flen = cap_build_req(frame, 0x07, CMD_PING, NULL, 0);
    const uint8_t want_req[] = { 0xBB, 0xAA, 0x07, 0x00, 0xFA, 0x01 };
    CHECK(flen == sizeof(want_req));
    CHECK_MEM(frame, want_req, flen);

    memcpy(buf, frame, flen);
    uint16_t used = comm_core_input(buf, flen, COMM_SRC_TCP);
    CHECK_EQ_U32(used, flen);
    CHECK_EQ_U32(cap_tcp.frames, 1);
    /* ACK: AA BB | id=0 | 05 | FA | pid 00 00 00 00 | csum. */
    const uint8_t want_ack[] = { 0xBB, 0xAA, 0x00, 0x05, 0xFA,
                                 0x07, 0x00, 0x00, 0x00, 0x00, 0x06 };
    CHECK_EQ_U32(cap_tcp.frame_len[0], sizeof(want_ack));
    CHECK_MEM(cap_tcp.data, want_ack, sizeof(want_ack));

    /* --- resync: garbage || valid || garbage || valid || tail ------------ */
    cap_reset();
    comm_core_init(cap_ops());
    size_t off = 0;
    const uint8_t junk1[] = { 0x00, 0x13, 0x37, 0xFF };
    memcpy(buf + off, junk1, sizeof(junk1)); off += sizeof(junk1);
    flen = cap_build_req(frame, 1, CMD_PING, NULL, 0);
    memcpy(buf + off, frame, flen); off += flen;
    const uint8_t junk2[] = { 0x42, 0x00, 0x11, 0x22, 0x33, 0x44, 0x55 };
    memcpy(buf + off, junk2, sizeof(junk2)); off += sizeof(junk2);
    flen = cap_build_req(frame, 2, CMD_PING, NULL, 0);
    memcpy(buf + off, frame, flen); off += flen;
    const uint8_t tail[] = { 0x99, 0x98 };                /* < header size  */
    memcpy(buf + off, tail, sizeof(tail)); off += sizeof(tail);

    used = comm_core_input(buf, (uint16_t)off, COMM_SRC_TCP);
    CHECK_EQ_U32(cap_tcp.frames, 2);        /* both PINGs recovered */
    CHECK_EQ_U32(used, off - sizeof(tail)); /* short tail kept for later */
    uint8_t cmd, plen, pid;
    const uint8_t *pl = cap_frame(&cap_tcp, 0, &cmd, &plen, &pid);
    CHECK(pl != NULL && cmd == CMD_PING && plen == 5 && pl[0] == 1);
    pl = cap_frame(&cap_tcp, 1, &cmd, &plen, &pid);
    CHECK(pl != NULL && cmd == CMD_PING && plen == 5 && pl[0] == 2);

    /* --- fake head inside garbage: the parser first waits for the phantom
           packet to complete ("buffer too short"), then resyncs through it
           on the checksum failure once enough bytes arrive ---------------- */
    cap_reset();
    comm_core_init(cap_ops());
    off = 0;
    /* Phantom packet claims 32 payload bytes (total 38). */
    const uint8_t fake_head[] = { 0xBB, 0xAA, 0x00, 0x20, 0x00 };
    memcpy(buf + off, fake_head, sizeof(fake_head)); off += sizeof(fake_head);
    flen = cap_build_req(frame, 9, CMD_PING, NULL, 0);
    memcpy(buf + off, frame, flen); off += flen;
    used = comm_core_input(buf, (uint16_t)off, COMM_SRC_TCP);
    CHECK_EQ_U32(used, 0);                  /* waiting for the phantom      */
    CHECK_EQ_U32(cap_tcp.frames, 0);
    /* More stream bytes arrive; the phantom completes, fails its checksum,
       and the parser resyncs onto the real PING behind it. */
    memset(buf + off, 0x00, 29); off += 29; /* now 40 >= 38 available       */
    used = comm_core_input(buf, (uint16_t)off, COMM_SRC_TCP);
    CHECK_EQ_U32(cap_tcp.frames, 1);        /* recovered after resync       */
    pl = cap_frame(&cap_tcp, 0, &cmd, &plen, &pid);
    CHECK(pl != NULL && cmd == CMD_PING && pl[0] == 9);
    CHECK_EQ_U32(used, off - (PROTO_HEADER_SIZE - 1));  /* zero tail scanned */

    /* --- bad checksum: skipped byte-by-byte, following packet recovered -- */
    cap_reset();
    comm_core_init(cap_ops());
    off = 0;
    flen = cap_build_req(frame, 3, CMD_PING, NULL, 0);
    frame[flen - 1] ^= 0xFF;                /* corrupt checksum */
    memcpy(buf + off, frame, flen); off += flen;
    flen = cap_build_req(frame, 4, CMD_PING, NULL, 0);
    memcpy(buf + off, frame, flen); off += flen;
    used = comm_core_input(buf, (uint16_t)off, COMM_SRC_TCP);
    CHECK_EQ_U32(cap_tcp.frames, 1);        /* only the intact one */
    pl = cap_frame(&cap_tcp, 0, &cmd, &plen, &pid);
    CHECK(pl != NULL && pl[0] == 4);
    CHECK_EQ_U32(used, off);

    /* --- incomplete packet: consume nothing, then parse whole ------------ */
    cap_reset();
    comm_core_init(cap_ops());
    uint8_t payload[10] = {0};
    flen = cap_build_req(frame, 5, CMD_PING, payload, sizeof(payload));
    used = comm_core_input(frame, (uint16_t)(flen - 4), COMM_SRC_TCP);
    CHECK_EQ_U32(used, 0);
    CHECK_EQ_U32(cap_tcp.frames, 0);
    used = comm_core_input(frame, flen, COMM_SRC_TCP);
    CHECK_EQ_U32(used, flen);
    CHECK_EQ_U32(cap_tcp.frames, 1);

    /* --- pure garbage with no head: everything except the last 4 bytes is
           consumed (a partial head might still be forming) ---------------- */
    cap_reset();
    comm_core_init(cap_ops());
    memset(buf, 0x55, 64);
    used = comm_core_input(buf, 64, COMM_SRC_TCP);
    CHECK_EQ_U32(used, 64 - (PROTO_HEADER_SIZE - 1));
    CHECK_EQ_U32(cap_tcp.frames, 0);

    /* --- max-size payload round trip ------------------------------------- */
    cap_reset();
    comm_core_init(cap_ops());
    CHECK_EQ_U32(PROTO_MAX_PAYLOAD, 249);
    CHECK_EQ_U32(PROTO_MAX_PACKET, 255);
    uint8_t big[PROTO_MAX_PAYLOAD];
    memset(big, 0xA5, sizeof(big));
    flen = cap_build_req(frame, 6, 0x77 /* unknown */, big, PROTO_MAX_PAYLOAD);
    CHECK_EQ_U32(flen, PROTO_MAX_PACKET);
    used = comm_core_input(frame, flen, COMM_SRC_TCP);
    CHECK_EQ_U32(used, flen);
    /* Unknown command: ERROR ACK so the host's waiter never hangs. */
    pl = cap_frame(&cap_tcp, 0, &cmd, &plen, &pid);
    CHECK(pl != NULL && cmd == 0x77 && plen == 5);
    uint32_t status = (uint32_t)pl[1] | ((uint32_t)pl[2] << 8)
                    | ((uint32_t)pl[3] << 16) | ((uint32_t)pl[4] << 24);
    CHECK_EQ_U32(status, PROTO_ST_ERROR);
    CHECK(pl[0] == 6);

    /* The one-byte payload length can represent 250, but the protocol's
       total packet limit cannot. Reject it before touching the destination. */
    uint8_t too_big[250] = {0};
    memset(frame, 0x5A, sizeof(frame));
    flen = comm_core_build_frame(frame, 0x77, too_big, sizeof(too_big));
    CHECK_EQ_U32(flen, 0);
    for(size_t i = 0; i < sizeof(frame); i++){
        CHECK(frame[i] == 0x5A);
    }

    /* A non-empty frame needs actual payload storage. */
    flen = comm_core_build_frame(frame, 0x77, NULL, 1);
    CHECK_EQ_U32(flen, 0);
    for(size_t i = 0; i < sizeof(frame); i++){
        CHECK(frame[i] == 0x5A);
    }

    /* Reject an oversized inbound frame and recover the valid frame after
       it without dispatching the invalid command. */
    cap_reset();
    comm_core_init(cap_ops());
    memset(buf, 0, 256);
    buf[0] = (uint8_t)(PROTO_HEAD & 0xFFu);
    buf[1] = (uint8_t)(PROTO_HEAD >> 8);
    buf[2] = 0x44;
    buf[3] = 250;
    buf[4] = 0x77;
    uint8_t oversized_csum = 0;
    for(size_t i = PROTO_CSUM_START; i < 255; i++){
        oversized_csum += buf[i];
    }
    buf[255] = oversized_csum;
    flen = cap_build_req(frame, 0x45, CMD_PING, NULL, 0);
    memcpy(buf + 256, frame, flen);
    used = comm_core_input(buf, (uint16_t)(256 + flen), COMM_SRC_TCP);
    CHECK_EQ_U32(used, 256 + flen);
    CHECK_EQ_U32(cap_tcp.frames, 1);
    pl = cap_frame(&cap_tcp, 0, &cmd, &plen, &pid);
    CHECK(pl != NULL && cmd == CMD_PING && plen == 5 && pl[0] == 0x45);

    return test_report("framing");
}
