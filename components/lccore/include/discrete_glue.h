/**
  ******************************************************************************
  * @file    discrete_glue.h
  * @brief   Nonblocking seam between the comm wire-protocol layer and a
  *          discrete-I/O backend (the ESP application binds the M31-U
  *          Modbus/RTU worker).
  ******************************************************************************
  */

#ifndef LCCORE_DISCRETE_GLUE_H
#define LCCORE_DISCRETE_GLUE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t relay_state;       /* physical FC01 readback                 */
    uint32_t input_state;       /* physical FC02 samples                  */
    uint32_t input_valid;       /* sampled inputs present and trustworthy */
    bool     link;              /* backend is online and usable           */
} discrete_backend_state_t;

/* Every callback must be nonblocking: they run in the Ethernet comm task.
   The backend owns any worker task, UART transactions and synchronization. */
typedef struct {
    void (*set_enabled)(bool enabled);
    void (*set_outputs)(uint32_t apply_mask, uint32_t values);
    bool (*get_state)(discrete_backend_state_t *state);
} discrete_backend_ops_t;

/* Bind the platform backend. The ops table must remain valid forever. */
void discrete_glue_bind(const discrete_backend_ops_t *ops);

/* One-time init. */
void discrete_glue_init(void);

/* Control-session reset: disable the subsystem, drive all relays off.
   Hooked into comm_core's channels_reset(). */
void discrete_glue_reset(void);

/* CMD_DISCRETE_SETUP: enable/disable + heartbeat period. Returns a PROTO_ST_*
   wire status code. */
uint32_t discrete_glue_setup(uint8_t enable, uint8_t flags, uint16_t report_ms);

/* CMD_DISCRETE_SET: merge desired relay bits (apply_mask/values). Writes are
   silently dropped while disabled or while the backend link is down. */
void discrete_glue_set(uint32_t apply_mask, uint32_t values);

/* Periodic service (comm task): emit pending / heartbeat STATE frames via
   comm_core_send_udp. */
void discrete_glue_loop(void);

#ifdef __cplusplus
}
#endif

#endif /* LCCORE_DISCRETE_GLUE_H */
