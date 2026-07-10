/**
  ******************************************************************************
  * @file    lc_log.h
  * @brief   Firmware log channel: leveled lines into a RAM ring, drained over
  *          UDP as CMD_LOG_MSG datagrams (wire doc §10), mirrored to the
  *          developer console via lc_port_console.
  *
  *          Direct port of arinc4i4o's log.c/.h with the STM32 PRIMASK
  *          critical sections replaced by lc_port_lock/unlock and HAL_GetTick
  *          by lc_port_tick_ms.
  *
  *          - Every formatted line goes to the console regardless of level;
  *            the ring only stores lines with level >= threshold.
  *          - Boot threshold is INFO; comm resets it to INFO on TCP session
  *            drop and changes it on CMD_SET_LOG_LEVEL.
  ******************************************************************************
  */

#ifndef LCCORE_LC_LOG_H
#define LCCORE_LC_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Wire log levels (proto.h PROTO_LOG_LEVEL_*, kept self-contained here). */
#define LOG_LEVEL_DEBUG 0
#define LOG_LEVEL_INFO  1

/* Max text bytes per stored line (wire record text field, §10.2). */
#define LOG_MAX_LINE    122

/* Reset the ring, counters and threshold (INFO). Call once early at boot. */
void    lc_log_init(void);

/* Runtime threshold. set clamps to DEBUG..INFO. */
void    lc_log_set_level(uint8_t level);
uint8_t lc_log_get_level(void);

/* printf-style line logging. Trailing \r\n is stripped; lines longer than
   LOG_MAX_LINE are capped and flagged truncated. */
void    lc_log_write(uint8_t level, const char *fmt, ...)
            __attribute__((format(printf, 2, 3)));

#define LOG_INF(...) lc_log_write(LOG_LEVEL_INFO,  __VA_ARGS__)
#define LOG_DBG(...) lc_log_write(LOG_LEVEL_DEBUG, __VA_ARGS__)

/* ---- network drain (comm log flush) ------------------------------------ */

/* True while the ring holds records. *bytes_ready = ring bytes pending,
   which equals the wire record bytes they will occupy. */
bool    lc_log_net_pending(uint16_t *bytes_ready);

/* Build one CMD_LOG_MSG payload: [seq u16 LE | dropped u16 LE | records...],
   popping whole records that fit in `max` bytes. Returns the payload length,
   0 if the ring is empty (seq is only consumed by a non-empty packet). */
uint8_t lc_log_net_build_packet(uint8_t *payload, uint8_t max);

/* Test-only: empty the ring and seed the seq / dropped counters so fixtures
   can pin exact wire bytes. */
void    lc_log_test_reset(uint16_t seq);

#ifdef __cplusplus
}
#endif

#endif /* LCCORE_LC_LOG_H */
