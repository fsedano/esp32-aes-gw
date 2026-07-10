/**
  ******************************************************************************
  * @file    discrete_glue.h
  * @brief   Seam between the comm wire-protocol layer and the discrete
  *          (relay + digital-input) subsystem, mirroring arinc4i4o.
  *
  *          STUB implementation: no relay card, no input pins. The stub
  *          maintains the desired/confirmed relay state (confirming writes
  *          immediately, link=1), reports input_valid=0 (nothing wired) and
  *          emits CMD_DISCRETE_STATE over UDP on change plus the report_ms
  *          heartbeat, exactly like the wire spec §9.
  ******************************************************************************
  */

#ifndef LCCORE_DISCRETE_GLUE_H
#define LCCORE_DISCRETE_GLUE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* One-time init. */
void discrete_glue_init(void);

/* Control-session reset: disable the subsystem, drive all relays off.
   Hooked into comm_core's channels_reset(). */
void discrete_glue_reset(void);

/* CMD_DISCRETE_SETUP: enable/disable + heartbeat period. Returns a PROTO_ST_*
   wire status code. */
uint32_t discrete_glue_setup(uint8_t enable, uint8_t flags, uint16_t report_ms);

/* CMD_DISCRETE_SET: merge desired relay bits (apply_mask/values). */
void discrete_glue_set(uint32_t apply_mask, uint32_t values);

/* Periodic service (comm task): emit pending / heartbeat STATE frames via
   comm_core_send_udp. */
void discrete_glue_loop(void);

#ifdef __cplusplus
}
#endif

#endif /* LCCORE_DISCRETE_GLUE_H */
