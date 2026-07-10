/**
  ******************************************************************************
  * @file    fletcher32.h
  * @brief   Fletcher32 exactly as the gateway's image packer computes it
  *          (aes-gw2/fwupdate/pack.go): the data is consumed as little-endian
  *          16-bit words, both running sums reduced mod 0xFFFF after every
  *          word. A trailing odd byte is ignored (never happens for firmware
  *          images, which are 16-byte padded).
  ******************************************************************************
  */

#ifndef LCCORE_FLETCHER32_H
#define LCCORE_FLETCHER32_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t sum1;
    uint32_t sum2;
    uint8_t  half;          /* buffered low byte of a split word */
    uint8_t  have_half;
} fletcher32_ctx_t;

/* Streaming interface: words may split across update() calls. */
void     fletcher32_init(fletcher32_ctx_t *ctx);
void     fletcher32_update(fletcher32_ctx_t *ctx, const uint8_t *data, size_t len);
uint32_t fletcher32_final(const fletcher32_ctx_t *ctx);   /* sum2<<16 | sum1 */

/* One-shot convenience. */
uint32_t fletcher32(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* LCCORE_FLETCHER32_H */
