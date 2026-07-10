/**
  ******************************************************************************
  * @file    lc_log.c
  * @brief   Log ring + console mirror + CMD_LOG_MSG payload builder (§10).
  *
  *          Storage: one 4096-byte static ring of variable-size records
  *              [ text_len u8 | flags u8 | tick_ms u32 LE | text ]
  *          which is exactly the 6-byte overhead of the wire record, so the
  *          ring fill level equals the wire bytes it will drain into.
  *          Overflow drops whole oldest records (g_dropped++, u16 wrap).
  *
  *          Concurrency: pushes may come from any task, pops only from the
  *          comm task; every ring mutation runs under lc_port_lock.
  *          g_in_log is a recursion backstop: a log call from inside the
  *          flush path still mirrors to the console but skips the ring.
  ******************************************************************************
  */

#include "lc_log.h"
#include "lc_port.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define LOG_RING_SIZE       4096u
#define LOG_REC_OVERHEAD    6u          /* len + flags + tick_ms(4)           */
#define LOG_NET_HDR_SIZE    4u          /* seq u16 LE + dropped u16 LE        */
#define LOG_FLAG_LEVEL_MASK 0x0Fu
#define LOG_FLAG_TRUNCATED  0x80u
#define LOG_FMT_BUF_SIZE    128u        /* vsnprintf stack buffer             */

static uint8_t  g_ring[LOG_RING_SIZE];
static uint16_t g_head;                 /* next write index                   */
static uint16_t g_tail;                 /* oldest record index                */
static uint16_t g_used;                 /* bytes occupied                     */
static uint16_t g_dropped;              /* lines lost to overflow, wraps      */
static uint16_t g_seq;                  /* per-boot datagram counter, wraps   */
static uint8_t  g_level = LOG_LEVEL_INFO;
static volatile bool g_in_log;          /* recursion backstop                 */

/* ---- ring primitives (call with the port lock held) ---------------------- */

static inline void ring_put(uint8_t b){
    g_ring[g_head] = b;
    g_head = (uint16_t)((g_head + 1u) % LOG_RING_SIZE);
    g_used++;
}

static inline uint8_t ring_get(void){
    uint8_t b = g_ring[g_tail];
    g_tail = (uint16_t)((g_tail + 1u) % LOG_RING_SIZE);
    g_used--;
    return b;
}

/* Drop the oldest whole record and count the lost line. */
static void ring_drop_oldest(void){
    uint16_t rec = (uint16_t)(LOG_REC_OVERHEAD + g_ring[g_tail]);
    g_tail = (uint16_t)((g_tail + rec) % LOG_RING_SIZE);
    g_used = (uint16_t)(g_used - rec);
    g_dropped++;
}

static void ring_push(uint8_t flags, uint32_t tick, const char *text, uint8_t len){
    uint16_t need = (uint16_t)(LOG_REC_OVERHEAD + len);
    while((uint16_t)(LOG_RING_SIZE - g_used) < need){
        ring_drop_oldest();
    }
    ring_put(len);
    ring_put(flags);
    ring_put((uint8_t)(tick));
    ring_put((uint8_t)(tick >> 8));
    ring_put((uint8_t)(tick >> 16));
    ring_put((uint8_t)(tick >> 24));
    for(uint8_t i = 0; i < len; i++){
        ring_put((uint8_t)text[i]);
    }
}

/* ---- line intake ---------------------------------------------------------- */

static void log_emit(uint8_t level, const char *text, size_t len, bool truncated){
    if(level > LOG_LEVEL_INFO){
        level = LOG_LEVEL_INFO;
    }
    while(len && (text[len - 1] == '\n' || text[len - 1] == '\r')){
        len--;
    }
    if(len > LOG_MAX_LINE){
        len = LOG_MAX_LINE;
        truncated = true;
    }

    /* Console mirror: every line, every level (developer channel). */
    char line[LOG_MAX_LINE + 1];
    memcpy(line, text, len);
    line[len] = '\0';
    lc_port_console(line);

    if(g_in_log || level < g_level || len == 0){
        return;     /* recursion backstop / below threshold / empty */
    }

    uint8_t flags = (uint8_t)((level & LOG_FLAG_LEVEL_MASK)
                              | (truncated ? LOG_FLAG_TRUNCATED : 0u));
    uint32_t tick = lc_port_tick_ms();
    lc_port_lock();
    ring_push(flags, tick, text, (uint8_t)len);
    lc_port_unlock();
}

/* ---- public API ------------------------------------------------------------ */

void lc_log_init(void){
    lc_port_lock();
    g_head    = 0;
    g_tail    = 0;
    g_used    = 0;
    g_dropped = 0;
    g_seq     = 0;
    g_level   = LOG_LEVEL_INFO;
    g_in_log  = false;
    lc_port_unlock();
}

void lc_log_set_level(uint8_t level){
    if(level > LOG_LEVEL_INFO){
        level = LOG_LEVEL_INFO;
    }
    g_level = level;
}

uint8_t lc_log_get_level(void){
    return g_level;
}

void lc_log_write(uint8_t level, const char *fmt, ...){
    char buf[LOG_FMT_BUF_SIZE];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if(n >= 0){
        bool trunc = (n >= (int)sizeof(buf));
        size_t len = trunc ? (sizeof(buf) - 1u) : (size_t)n;
        log_emit(level, buf, len, trunc);
    }
}

/* ---- network drain ----------------------------------------------------------- */

bool lc_log_net_pending(uint16_t *bytes_ready){
    lc_port_lock();
    uint16_t used = g_used;
    lc_port_unlock();
    if(bytes_ready){
        *bytes_ready = used;
    }
    return used != 0;
}

uint8_t lc_log_net_build_packet(uint8_t *payload, uint8_t max){
    if(payload == NULL || max < (uint8_t)(LOG_NET_HDR_SIZE + LOG_REC_OVERHEAD + 1u)){
        return 0;
    }
    g_in_log = true;                /* no ring inserts from inside the flush */

    uint8_t off = 0;
    lc_port_lock();
    bool any = (g_used != 0);
    uint16_t dropped = g_dropped;
    lc_port_unlock();

    if(any){
        payload[0] = (uint8_t)(g_seq);      /* g_seq: comm task only */
        payload[1] = (uint8_t)(g_seq >> 8);
        payload[2] = (uint8_t)(dropped);
        payload[3] = (uint8_t)(dropped >> 8);
        off = LOG_NET_HDR_SIZE;

        /* One short lock per record so writers are never blocked across a
           whole 250-byte drain. Records pushed or dropped between sections
           are picked up or skipped consistently. */
        for(;;){
            lc_port_lock();
            if(g_used == 0){
                lc_port_unlock();
                break;
            }
            uint16_t rec = (uint16_t)(LOG_REC_OVERHEAD + g_ring[g_tail]);
            if((uint16_t)off + rec > (uint16_t)max){
                lc_port_unlock();
                break;              /* whole records only; rest waits */
            }
            uint8_t text_len = ring_get();
            uint8_t flags    = ring_get();
            payload[off++] = flags;
            payload[off++] = ring_get();    /* tick_ms LE, stored LE */
            payload[off++] = ring_get();
            payload[off++] = ring_get();
            payload[off++] = ring_get();
            payload[off++] = text_len;
            for(uint8_t i = 0; i < text_len; i++){
                payload[off++] = ring_get();
            }
            lc_port_unlock();
        }

        if(off > LOG_NET_HDR_SIZE){
            g_seq++;                /* one seq per emitted datagram */
        }else{
            off = 0;                /* nothing fit: not a packet */
        }
    }

    g_in_log = false;
    return off;
}

void lc_log_test_reset(uint16_t seq){
    lc_port_lock();
    g_head    = 0;
    g_tail    = 0;
    g_used    = 0;
    g_dropped = 0;
    g_seq     = seq;
    g_in_log  = false;
    lc_port_unlock();
}
