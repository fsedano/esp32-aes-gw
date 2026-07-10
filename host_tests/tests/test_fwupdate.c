/* FW_UPDATE state machine (START/STEP/FINISH) with fake flash + identity
   "cipher" ops: sequencing, Fletcher32 verification gate before set_boot,
   error/latch behaviour and the upload watchdog. The wire payloads are
   built exactly as aes-gw2/fwupdate/updater.go builds them. */

#include "fletcher32.h"
#include "fwupdate_core.h"
#include "lc_log.h"
#include "lc_port.h"
#include "test_util.h"

#include <string.h>

void host_port_set_tick(uint32_t ms);

/* ---- fake port ---------------------------------------------------------- */

#define FAKE_PART_SIZE  (64 * 1024)
static uint8_t  fake_flash[FAKE_PART_SIZE];
static uint32_t fake_flash_len;
static int      fake_begin_calls, fake_end_calls, fake_boot_calls;
static uint8_t  fake_iv[16];
static int      fake_write_fail;    /* fail the Nth write when > 0 */

static int f_begin(uint32_t size){
    if(size > FAKE_PART_SIZE){
        return -1;
    }
    memset(fake_flash, 0xEE, sizeof(fake_flash));
    fake_flash_len = 0;
    fake_begin_calls++;
    return 0;
}
static int f_write(const uint8_t *d, uint32_t n){
    if(fake_write_fail > 0 && --fake_write_fail == 0){
        return -1;
    }
    memcpy(fake_flash + fake_flash_len, d, n);
    fake_flash_len += n;
    return 0;
}
static int f_end(void){ fake_end_calls++; return 0; }
static int f_boot(void){ fake_boot_calls++; return 0; }
static int f_aes_start(const uint8_t iv[16]){ memcpy(fake_iv, iv, 16); return 0; }
static int f_aes_decrypt(uint8_t *d, uint32_t n){ (void)d; (void)n; return 0; }

static const fwup_ops_t FAKE_OPS = {
    .flash_begin = f_begin, .flash_write = f_write, .flash_end = f_end,
    .set_boot = f_boot, .aes_start = f_aes_start, .aes_decrypt = f_aes_decrypt,
};

static void fake_reset(void){
    fake_flash_len = 0;
    fake_begin_calls = fake_end_calls = fake_boot_calls = 0;
    fake_write_fail = 0;
}

/* ---- payload builders (mirror updater.go) -------------------------------- */

static void wr_u32(uint8_t *p, uint32_t v){
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint32_t send_start(uint32_t size, uint32_t csum, const uint8_t iv[16]){
    uint8_t pl[28];
    wr_u32(pl, 0);
    wr_u32(pl + 4, size);
    wr_u32(pl + 8, csum);
    memcpy(pl + 12, iv, 16);
    return fwup_handle(pl, sizeof(pl));
}

static uint32_t send_step(uint32_t index, const uint8_t *data, uint8_t n){
    uint8_t pl[4 + FWUP_CHUNK_SIZE];
    wr_u32(pl, index);
    memcpy(pl + 4, data, n);
    return fwup_handle(pl, (uint8_t)(4 + n));
}

static uint32_t send_finish(uint32_t size){
    uint8_t pl[8];
    wr_u32(pl, 0xFFFFFFFFu);
    wr_u32(pl + 4, size);
    return fwup_handle(pl, sizeof(pl));
}

/* Full happy-path upload of `img[0..len)`. */
static uint32_t upload_all(const uint8_t *img, uint32_t len, uint32_t csum){
    static const uint8_t iv[16] = { 1, 2, 3, 4, 5, 6, 7, 8,
                                    9, 10, 11, 12, 13, 14, 15, 16 };
    uint32_t st = send_start(len, csum, iv);
    if(st != FWUP_OK){
        return st;
    }
    uint32_t index = 1;
    for(uint32_t off = 0; off < len; off += FWUP_CHUNK_SIZE){
        uint32_t n = len - off;
        if(n > FWUP_CHUNK_SIZE){
            n = FWUP_CHUNK_SIZE;
        }
        st = send_step(index++, img + off, (uint8_t)n);
        if(st != FWUP_OK){
            return st;
        }
    }
    return send_finish(len);
}

int main(void){
    lc_port_init();
    lc_log_init();
    host_port_set_tick(0);

    /* Image: 1040 bytes (%16==0, >= FWUP_MIN_SIZE), known Fletcher32. */
    static uint8_t img[1040];
    for(size_t i = 0; i < sizeof(img); i++){
        img[i] = (uint8_t)((i * 7 + 3) & 0xFF);
    }
    const uint32_t IMG_CSUM = 0xC6BCDAA0u;      /* pinned in test_fletcher32 */
    CHECK_EQ_U32(fletcher32(img, sizeof(img)), IMG_CSUM);

    /* --- happy path -------------------------------------------------------- */
    fake_reset();
    fwup_init(&FAKE_OPS);
    CHECK_EQ_U32(upload_all(img, sizeof(img), IMG_CSUM), FWUP_OK);
    CHECK_EQ_U32(fake_begin_calls, 1);
    CHECK_EQ_U32(fake_end_calls, 1);
    CHECK_EQ_U32(fake_boot_calls, 1);           /* boot switched only now */
    CHECK_EQ_U32(fake_flash_len, sizeof(img));
    CHECK_MEM(fake_flash, img, sizeof(img));
    const uint8_t want_iv[16] = { 1, 2, 3, 4, 5, 6, 7, 8,
                                  9, 10, 11, 12, 13, 14, 15, 16 };
    CHECK_MEM(fake_iv, want_iv, 16);

    /* A second upload on the same session works (gateway retry). */
    CHECK_EQ_U32(upload_all(img, sizeof(img), IMG_CSUM), FWUP_OK);

    /* --- STEP before START ------------------------------------------------- */
    fake_reset();
    fwup_init(&FAKE_OPS);
    CHECK_EQ_U32(send_step(1, img, 32), FWUP_ERR_INDEX_NOT_ZERO_WHEN_IDLE);

    /* --- START validation --------------------------------------------------- */
    CHECK_EQ_U32(send_start(1000, 0, want_iv), FWUP_ERR_FW_SIZE_NOT_MULT_16);
    CHECK_EQ_U32(send_start(512, 0, want_iv), FWUP_ERR_FW_SIZE_OUT_OF_RANGE);
    CHECK_EQ_U32(send_start(FAKE_PART_SIZE + 16, 0, want_iv), FWUP_ERR_FLASH_BEGIN);

    /* --- out-of-order STEP -> PACKET_LOSS, latched -------------------------- */
    fake_reset();
    fwup_init(&FAKE_OPS);
    CHECK_EQ_U32(send_start(sizeof(img), IMG_CSUM, want_iv), FWUP_OK);
    CHECK_EQ_U32(send_step(1, img, 32), FWUP_OK);
    CHECK_EQ_U32(send_step(3, img + 32, 32), FWUP_ERR_PACKET_LOSS);
    /* Back in IDLE: a non-START is rejected (same as the STM32 loader,
       which only latches the abort reason for watchdog timeouts)... */
    CHECK_EQ_U32(send_step(4, img, 32), FWUP_ERR_INDEX_NOT_ZERO_WHEN_IDLE);
    /* ...and a fresh START recovers. */
    CHECK_EQ_U32(upload_all(img, sizeof(img), IMG_CSUM), FWUP_OK);

    /* --- bad checksum: FINISH rejected, boot partition untouched ------------ */
    fake_reset();
    fwup_init(&FAKE_OPS);
    CHECK_EQ_U32(upload_all(img, sizeof(img), IMG_CSUM ^ 1), FWUP_ERR_FW_CHECKSUM);
    CHECK_EQ_U32(fake_boot_calls, 0);

    /* --- short image: FINISH size gate --------------------------------------- */
    fake_reset();
    fwup_init(&FAKE_OPS);
    CHECK_EQ_U32(send_start(sizeof(img), IMG_CSUM, want_iv), FWUP_OK);
    CHECK_EQ_U32(send_step(1, img, 32), FWUP_OK);
    CHECK_EQ_U32(send_finish(sizeof(img)), FWUP_ERR_FIRM_SIZE_DISAGREE);
    CHECK_EQ_U32(fake_boot_calls, 0);

    /* --- non-16-multiple STEP -------------------------------------------------- */
    fake_reset();
    fwup_init(&FAKE_OPS);
    CHECK_EQ_U32(send_start(sizeof(img), IMG_CSUM, want_iv), FWUP_OK);
    CHECK_EQ_U32(send_step(1, img, 20), FWUP_ERR_DATA_NOT_FACTOR_16);

    /* --- flash write failure ---------------------------------------------------- */
    fake_reset();
    fwup_init(&FAKE_OPS);
    fake_write_fail = 2;                        /* second write fails */
    CHECK_EQ_U32(send_start(sizeof(img), IMG_CSUM, want_iv), FWUP_OK);
    CHECK_EQ_U32(send_step(1, img, 32), FWUP_OK);
    CHECK_EQ_U32(send_step(2, img + 32, 32), FWUP_ERR_FLASH_PROGRAM);

    /* --- watchdog: 2 s of silence aborts the upload ------------------------------ */
    fake_reset();
    fwup_init(&FAKE_OPS);
    host_port_set_tick(10000);
    CHECK_EQ_U32(send_start(sizeof(img), IMG_CSUM, want_iv), FWUP_OK);
    host_port_set_tick(10000 + FWUP_TIMEOUT_MS + 100);
    fwup_tick();
    CHECK_EQ_U32(send_step(1, img, 32), FWUP_ERR_TIMEOUT);
    /* session reset clears the latch */
    fwup_session_reset();
    CHECK_EQ_U32(upload_all(img, sizeof(img), IMG_CSUM), FWUP_OK);

    return test_report("fwupdate");
}
