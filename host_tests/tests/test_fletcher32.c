/* Fletcher32 against golden vectors computed with the gateway's exact
   algorithm (aes-gw2/fwupdate/pack.go: LE 16-bit words, both sums mod
   0xFFFF per word, trailing odd byte dropped). */

#include "fletcher32.h"
#include "test_util.h"

int main(void){
    CHECK_EQ_U32(fletcher32((const uint8_t *)"", 0), 0x00000000u);

    const uint8_t v1[] = { 0x01, 0x02 };
    CHECK_EQ_U32(fletcher32(v1, sizeof(v1)), 0x02010201u);

    const uint8_t v2[] = { 0x01, 0x02, 0x03, 0x04 };
    CHECK_EQ_U32(fletcher32(v2, sizeof(v2)), 0x08050604u);

    /* Odd length: pack.go drops the trailing byte. */
    CHECK_EQ_U32(fletcher32((const uint8_t *)"abcde", 5), 0x2926C6C4u);
    CHECK_EQ_U32(fletcher32((const uint8_t *)"abcd", 4),
                 fletcher32((const uint8_t *)"abcde", 5));

    CHECK_EQ_U32(fletcher32((const uint8_t *)"abcdefgh", 8), 0xEBE19591u);

    /* All-0xFF quirk: every word is congruent to 0 mod 0xFFFF (this is what
       the image padding bytes contribute). */
    uint8_t ff[100];
    memset(ff, 0xFF, sizeof(ff));
    CHECK_EQ_U32(fletcher32(ff, sizeof(ff)), 0x00000000u);

    /* Streaming interface: arbitrary (odd) split points must match the
       one-shot value, including a word split across updates. */
    fletcher32_ctx_t ctx;
    fletcher32_init(&ctx);
    fletcher32_update(&ctx, (const uint8_t *)"abc", 3);
    fletcher32_update(&ctx, (const uint8_t *)"defgh", 5);
    CHECK_EQ_U32(fletcher32_final(&ctx), 0xEBE19591u);

    fletcher32_init(&ctx);
    for(int i = 0; i < 8; i++){
        fletcher32_update(&ctx, (const uint8_t *)"abcdefgh" + i, 1);
    }
    CHECK_EQ_U32(fletcher32_final(&ctx), 0xEBE19591u);

    /* Larger pattern (also used by test_fwupdate). */
    uint8_t pat[1040];
    for(size_t i = 0; i < sizeof(pat); i++){
        pat[i] = (uint8_t)((i * 7 + 3) & 0xFF);
    }
    CHECK_EQ_U32(fletcher32(pat, sizeof(pat)), 0xC6BCDAA0u);

    return test_report("fletcher32");
}
