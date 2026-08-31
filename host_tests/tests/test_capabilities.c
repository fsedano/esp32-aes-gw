/* Capability descriptor golden bytes + GET_CAPABILITIES wire behavior. */

#include "capabilities.h"
#include "comm_capture.h"
#include "comm_core.h"
#include "lc_log.h"
#include "lc_port.h"
#include "proto.h"
#include "test_util.h"

#include <string.h>

static uint8_t g_frame[PROTO_MAX_PACKET];

static uint32_t ack_status(const uint8_t *payload){
    return (uint32_t)payload[1]
         | ((uint32_t)payload[2] << 8)
         | ((uint32_t)payload[3] << 16)
         | ((uint32_t)payload[4] << 24);
}

static const uint8_t *request(uint8_t src, uint8_t id,
                              const uint8_t *payload, uint8_t len,
                              uint8_t *reply_len){
    cap_reset();
    uint16_t frame_len = cap_build_req(g_frame, id, CMD_GET_CAPABILITIES,
                                       payload, len);
    comm_core_input(g_frame, frame_len, src);
    return cap_frame(&cap_tcp, 0, NULL, reply_len, NULL);
}

int main(void){
    /* Golden descriptor for the live bench population discovered in the
       preceding bring-up: 16 relay outputs, one digital input, plus the
       fixed 8-axis / 32-button USB HID interface. */
    static const uint8_t expected[] = {
        0x01, 0x00, 0x00, 0x00,
        0x01, 0x39, 0x00,
          0x01, 0x24, 0x00,
            'A','E','S',' ','E','S','P','3','2',' ','M','3','1',' ',
            'd','i','s','c','r','e','t','e',' ','I','/','O',' ','+',' ',
            'U','S','B',' ','H','I','D',
          0x02, 0x0F, 0x00,
            'A','E','S','-','E','S','P','-','M','3','1','-','H','I','D',
        0x02, 0x27, 0x00,
          0x02, 0x00,
          0x01, 0x00, 0x00, 0x00,
          0x10, 0x00, 0x00, 0x00,
          0x00, 0x00, 0x00, 0x00,
          0x01, 0x12, 0x00,
            'M','3','1','-','U',' ','d','i','s','c','r','e','t','e',' ',
            'I','/','O',
          0x10, 0x01, 0x00, 0x00,
        0x02, 0x1D, 0x00,
          0x05, 0x00,
          0x00, 0x00, 0x00, 0x00,
          0x08, 0x00, 0x00, 0x00,
          0x00, 0x00, 0x00, 0x00,
          0x01, 0x0C, 0x00,
            'U','S','B',' ','H','I','D',' ','a','x','e','s',
        0x02, 0x20, 0x00,
          0x06, 0x00,
          0x00, 0x00, 0x00, 0x00,
          0x20, 0x00, 0x00, 0x00,
          0x00, 0x00, 0x00, 0x00,
          0x01, 0x0F, 0x00,
            'U','S','B',' ','H','I','D',' ','b','u','t','t','o','n','s',
    };

    lc_port_init();
    lc_log_init();
    capabilities_init(1, 16);
    CHECK(capabilities_available());
    CHECK_EQ_U32(capabilities_version(), 1);
    uint16_t blob_len = 0;
    const uint8_t *blob = capabilities_blob(&blob_len);
    CHECK(blob != NULL);
    CHECK_EQ_U32(blob_len, sizeof(expected));
    CHECK_MEM(blob, expected, sizeof(expected));

    cap_reset();
    comm_core_init(cap_ops());

    /* Whole descriptor fits the first chunk: ACK envelope + total + blob. */
    uint8_t req[3] = { 1, 0, 0 };
    uint8_t reply_len = 0;
    const uint8_t *ack = request(COMM_SRC_TCP, 0x20, req, sizeof(req),
                                 &reply_len);
    CHECK(ack != NULL && ack_status(ack) == PROTO_ST_OK);
    CHECK_EQ_U32(reply_len, 5u + 2u + sizeof(expected));
    CHECK_EQ_U32((uint16_t)ack[5] | ((uint16_t)ack[6] << 8), sizeof(expected));
    CHECK_MEM(ack + 7, expected, sizeof(expected));

    static const char summary[] =
        "caps: v1 173 bytes; discrete 1 DI/16 DO relay; HID 8 axes/32 buttons";
    uint8_t log_payload[PROTO_MAX_PAYLOAD];
    uint8_t log_len = lc_log_net_build_packet(log_payload,
                                               sizeof(log_payload));
    CHECK_EQ_U32(log_len, 4u + 6u + sizeof(summary) - 1u);
    CHECK_EQ_U32(log_payload[4], LOG_LEVEL_INFO);
    CHECK_EQ_U32(log_payload[9], sizeof(summary) - 1u);
    CHECK_MEM(log_payload + 10, summary, sizeof(summary) - 1u);

    /* offset == total is legal and returns an empty chunk. */
    req[1] = (uint8_t)blob_len; req[2] = (uint8_t)(blob_len >> 8);
    ack = request(COMM_SRC_TCP, 0x21, req, sizeof(req), &reply_len);
    CHECK(ack != NULL && ack_status(ack) == PROTO_ST_OK);
    CHECK_EQ_U32(reply_len, 7);

    /* Offset past end, unsupported version, short payload, and UDP copy. */
    uint16_t past = (uint16_t)(blob_len + 1u);
    req[1] = (uint8_t)past; req[2] = (uint8_t)(past >> 8);
    ack = request(COMM_SRC_TCP, 0x22, req, sizeof(req), &reply_len);
    CHECK(ack != NULL && ack_status(ack) == PROTO_ST_ERROR);

    req[0] = 0; req[1] = 0; req[2] = 0;
    ack = request(COMM_SRC_TCP, 0x23, req, sizeof(req), &reply_len);
    CHECK(ack != NULL
          && ack_status(ack) == PROTO_ST_CAPS_ERR_VERSION_UNSUPPORTED);
    CHECK_EQ_U32(reply_len, 7);
    CHECK(ack[5] == 1 && ack[6] == 1);

    req[0] = 2;
    ack = request(COMM_SRC_TCP, 0x26, req, sizeof(req), &reply_len);
    CHECK(ack != NULL
          && ack_status(ack) == PROTO_ST_CAPS_ERR_VERSION_UNSUPPORTED);
    CHECK_EQ_U32(reply_len, 7);
    CHECK(ack[5] == 1 && ack[6] == 1);

    ack = request(COMM_SRC_TCP, 0x24, req, 2, &reply_len);
    CHECK(ack != NULL
          && ack_status(ack) == PROTO_ST_COMM_ERR_PAYLOAD_TOO_SHORT);

    ack = request(COMM_SRC_UDP, 0x25, req, sizeof(req), &reply_len);
    CHECK(ack == NULL && cap_tcp.frames == 0);

    return test_report("capabilities");
}
