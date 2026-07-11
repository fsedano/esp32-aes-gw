/* Recovery-build comm dispatch of CMD_FW_UPDATE (compiled with
   RECOVERY_BUILD via the lccore_recovery library): the chunk-size
   negotiation lives at this level, so assert the on-wire ACK contract:

     - a successful START ACK carries exactly 2 extra bytes,
       u16 LE == FWUP_CHUNK_SIZE (240);
     - every other FW_UPDATE ACK (STEP, FINISH, rejected START) carries
       no extra — byte-identical to the legacy STM32 bootloader's ACKs.

   The upload state machine itself is covered by test_fwupdate.c; here it
   runs behind the real comm_core framing with fake flash/AES ops. */

#include "comm_capture.h"
#include "comm_core.h"
#include "fletcher32.h"
#include "fwupdate_core.h"
#include "identity.h"
#include "lc_log.h"
#include "lc_port.h"
#include "proto.h"
#include "test_util.h"

#include <string.h>

#ifndef RECOVERY_BUILD
#error "this test must be compiled with RECOVERY_BUILD (link lccore_recovery)"
#endif

static const uint8_t MAC[6] = { 0x24, 0x0A, 0xC4, 0x11, 0x22, 0x33 };

/* ---- fake fwup port (upload succeeds, bytes discarded) ------------------ */

static int  f_begin(uint32_t size){ (void)size; return 0; }
static int  f_write(const uint8_t *d, uint32_t n){ (void)d; (void)n; return 0; }
static int  f_end(void){ return 0; }
static void f_abort(void){}
static int  f_boot(void){ return 0; }
static int  f_aes_start(const uint8_t iv[16]){ (void)iv; return 0; }
static int  f_aes_decrypt(uint8_t *d, uint32_t n){ (void)d; (void)n; return 0; }

static const fwup_ops_t FAKE_OPS = {
    .flash_begin = f_begin, .flash_write = f_write, .flash_end = f_end,
    .flash_abort = f_abort,
    .set_boot = f_boot, .aes_start = f_aes_start, .aes_decrypt = f_aes_decrypt,
};

/* ---- helpers ------------------------------------------------------------- */

static void wr_u32(uint8_t *p, uint32_t v){
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint32_t rd_u32(const uint8_t *p){
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint8_t g_next_id;

/* Send one CMD_FW_UPDATE payload through comm_core_input and return the
   captured ACK: *status is the u32 status code, *extra_len the number of
   extra bytes past the 5-byte envelope, extra the extra bytes. */
static void fw_roundtrip(const uint8_t *payload, uint8_t len,
                         uint32_t *status, uint8_t *extra_len,
                         const uint8_t **extra){
    uint8_t frame[PROTO_MAX_PACKET];
    uint8_t buf[PROTO_MAX_PACKET];

    cap_reset();
    uint16_t flen = cap_build_req(frame, g_next_id++, CMD_FW_UPDATE,
                                  payload, len);
    memcpy(buf, frame, flen);
    uint16_t used = comm_core_input(buf, flen, COMM_SRC_TCP);
    CHECK_EQ_U32(used, flen);
    CHECK_EQ_U32(cap_tcp.frames, 1);

    uint8_t cmd = 0, plen = 0, pid = 0;
    const uint8_t *pl = cap_frame(&cap_tcp, 0, &cmd, &plen, &pid);
    CHECK(pl != NULL);
    CHECK_EQ_U32(cmd, CMD_FW_UPDATE);
    CHECK(plen >= 5);
    *status    = rd_u32(pl + 1);            /* [0]=echoed req id            */
    *extra_len = (uint8_t)(plen - 5);
    *extra     = pl + 5;
}

int main(void){
    lc_port_init();
    lc_log_init();
    identity_init(MAC);
    fwup_init(&FAKE_OPS);
    cap_reset();
    comm_core_init(cap_ops());
    comm_core_session(true);                /* connect: fresh upload state  */

    /* Image: 1040 bytes (%16==0, >= FWUP_MIN_SIZE). */
    static uint8_t img[1040];
    for(size_t i = 0; i < sizeof(img); i++){
        img[i] = (uint8_t)((i * 7 + 3) & 0xFF);
    }
    const uint32_t csum = fletcher32(img, sizeof(img));
    static const uint8_t iv[16] = { 1, 2, 3, 4, 5, 6, 7, 8,
                                    9, 10, 11, 12, 13, 14, 15, 16 };

    uint32_t status; uint8_t extra_len; const uint8_t *extra;
    uint8_t pl[4 + FWUP_CHUNK_SIZE];

    /* --- successful START ACK advertises the max chunk ------------------- */
    uint8_t start[28];
    wr_u32(start, 0);
    wr_u32(start + 4, sizeof(img));
    wr_u32(start + 8, csum);
    memcpy(start + 12, iv, 16);
    fw_roundtrip(start, sizeof(start), &status, &extra_len, &extra);
    CHECK_EQ_U32(status, FWUP_OK);
    CHECK_EQ_U32(extra_len, 2);
    CHECK_EQ_U32((uint32_t)extra[0] | ((uint32_t)extra[1] << 8),
                 FWUP_CHUNK_SIZE);          /* u16 LE == 240 */

    /* --- STEP ACKs carry no extra (240-byte chunks + 80-byte tail) ------- */
    uint32_t index = 1;
    for(uint32_t off = 0; off < sizeof(img); off += FWUP_CHUNK_SIZE){
        uint32_t n = sizeof(img) - off;
        if(n > FWUP_CHUNK_SIZE){
            n = FWUP_CHUNK_SIZE;
        }
        wr_u32(pl, index++);
        memcpy(pl + 4, img + off, n);
        fw_roundtrip(pl, (uint8_t)(4 + n), &status, &extra_len, &extra);
        CHECK_EQ_U32(status, FWUP_OK);
        CHECK_EQ_U32(extra_len, 0);
    }

    /* --- FINISH ACK carries no extra -------------------------------------- */
    uint8_t fin[8];
    wr_u32(fin, 0xFFFFFFFFu);
    wr_u32(fin + 4, sizeof(img));
    fw_roundtrip(fin, sizeof(fin), &status, &extra_len, &extra);
    CHECK_EQ_U32(status, FWUP_OK);
    CHECK_EQ_U32(extra_len, 0);

    /* --- rejected START carries no extra (index 0 but bad size) ----------- */
    comm_core_session(true);                /* reset upload state           */
    wr_u32(start + 4, 1000);                /* not a multiple of 16         */
    fw_roundtrip(start, sizeof(start), &status, &extra_len, &extra);
    CHECK_EQ_U32(status, FWUP_ERR_FW_SIZE_NOT_MULT_16);
    CHECK_EQ_U32(extra_len, 0);

    /* --- a mid-upload duplicate START is not advertised either ------------ */
    comm_core_session(true);
    wr_u32(start + 4, sizeof(img));
    fw_roundtrip(start, sizeof(start), &status, &extra_len, &extra);
    CHECK_EQ_U32(status, FWUP_OK);
    CHECK_EQ_U32(extra_len, 2);
    fw_roundtrip(start, sizeof(start), &status, &extra_len, &extra);
    CHECK(status != FWUP_OK);               /* index 0 mid-upload = loss    */
    CHECK_EQ_U32(extra_len, 0);

    /* --- legacy host: ignores the advertisement, sends 32-byte STEPs ------ */
    comm_core_session(true);
    fw_roundtrip(start, sizeof(start), &status, &extra_len, &extra);
    CHECK_EQ_U32(status, FWUP_OK);
    wr_u32(pl, 1);
    memcpy(pl + 4, img, 32);
    fw_roundtrip(pl, 4 + 32, &status, &extra_len, &extra);
    CHECK_EQ_U32(status, FWUP_OK);
    CHECK_EQ_U32(extra_len, 0);

    return test_report("fwupdate_comm");
}
