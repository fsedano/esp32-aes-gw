/**
  ******************************************************************************
  * @file    fletcher32.c
  * @brief   Fletcher32 matching aes-gw2/fwupdate/pack.go (see fletcher32.h).
  ******************************************************************************
  */

#include "fletcher32.h"

void fletcher32_init(fletcher32_ctx_t *ctx){
    ctx->sum1 = 0;
    ctx->sum2 = 0;
    ctx->half = 0;
    ctx->have_half = 0;
}

static inline void feed_word(fletcher32_ctx_t *ctx, uint32_t w){
    ctx->sum1 = (ctx->sum1 + w) % 0xFFFFu;
    ctx->sum2 = (ctx->sum2 + ctx->sum1) % 0xFFFFu;
}

void fletcher32_update(fletcher32_ctx_t *ctx, const uint8_t *data, size_t len){
    size_t i = 0;
    if(ctx->have_half && len > 0){
        feed_word(ctx, (uint32_t)ctx->half | ((uint32_t)data[0] << 8));
        ctx->have_half = 0;
        i = 1;
    }
    for(; i + 1 < len; i += 2){
        feed_word(ctx, (uint32_t)data[i] | ((uint32_t)data[i + 1] << 8));
    }
    if(i < len){
        ctx->half = data[i];
        ctx->have_half = 1;
    }
}

uint32_t fletcher32_final(const fletcher32_ctx_t *ctx){
    /* pack.go's loop condition (i+1 < len) silently drops a trailing odd
       byte; mirror that by ignoring a pending half word. */
    return (ctx->sum2 << 16) | ctx->sum1;
}

uint32_t fletcher32(const uint8_t *data, size_t len){
    fletcher32_ctx_t ctx;
    fletcher32_init(&ctx);
    fletcher32_update(&ctx, data, len);
    return fletcher32_final(&ctx);
}
