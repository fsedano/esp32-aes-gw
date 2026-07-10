/**
  ******************************************************************************
  * @file    lc_port.h
  * @brief   OS/board seam for the protocol core.
  *
  *          Everything in components/lccore is plain C11 with no ESP-IDF (or
  *          any other OS) includes; the few environment services it needs are
  *          declared here and implemented twice:
  *            - main/lc_port_esp.c   : ESP-IDF (FreeRTOS mutex, esp_timer, ...)
  *            - host_tests/support/  : host build for unit tests
  *
  *          This mirrors how arinc4i4o compiles its comm/ssdp/log code both
  *          on-target and in its host simulator.
  ******************************************************************************
  */

#ifndef LCCORE_LC_PORT_H
#define LCCORE_LC_PORT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

/* Milliseconds since boot (wraps at 2^32, like HAL_GetTick). */
uint32_t lc_port_tick_ms(void);

/* Short critical section guarding the log ring (pushes may come from any
   task; pops only from the comm task). Non-recursive. */
void lc_port_lock(void);
void lc_port_unlock(void);

/* Developer-console mirror of one log line (no trailing newline included).
   Called for every line regardless of the wire log threshold. */
void lc_port_console(const char *line);

#ifdef __cplusplus
}
#endif

#endif /* LCCORE_LC_PORT_H */
