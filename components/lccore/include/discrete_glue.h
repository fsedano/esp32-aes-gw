/**
  ******************************************************************************
  * @file    discrete_glue.h
  * @brief   Nonblocking seam between the comm wire-protocol layer and a
  *          discrete-I/O backend.
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
    uint64_t relay_state;       /* physical FC01 readback                 */
    uint64_t input_state;       /* physical FC02 samples                  */
    uint64_t input_valid;       /* sampled inputs present and trustworthy */
    bool     link;              /* backend is online and usable           */
} discrete_backend_state_t;

/* Every callback must be nonblocking: they run in the Ethernet comm task.
   The backend owns any worker task, UART transactions and synchronization. */
typedef struct {
    void (*set_enabled)(bool enabled);
    void (*set_outputs)(uint64_t apply_mask, uint64_t values);
    bool (*get_state)(discrete_backend_state_t *state);
} discrete_backend_ops_t;

/* Bind the platform backend. The ops table must remain valid forever. */
void discrete_glue_bind(const discrete_backend_ops_t *ops);

/* Set the immutable per-boot process-image widths advertised by the
   capability descriptor. STATE reports one range covering the larger width. */
void discrete_glue_configure(uint16_t input_count, uint16_t output_count);

/* One-time init. */
void discrete_glue_init(void);

/* Control-session reset: disable the subsystem, drive all relays off.
   Hooked into comm_core's channels_reset(). */
void discrete_glue_reset(void);

/* CMD_DISCRETE_SETUP: enable/disable + heartbeat period. Returns a PROTO_ST_*
   wire status code. */
uint32_t discrete_glue_setup(uint8_t enable, uint8_t flags, uint16_t report_ms);

/* Apply one snapshot range: every relay in range_mask takes its bit in
   values. seq older than the last accepted one (signed 8-bit delta) is
   dropped. Ignored while disabled; while the backend link is down the
   snapshot still feeds the stream watchdog but drives nothing. */
void discrete_glue_set(uint64_t range_mask, uint64_t values, uint8_t seq);

/* Periodic service (comm task): run the host-stream watchdog and emit
   pending / heartbeat STATE frames via comm_core_send_udp. */
void discrete_glue_loop(void);

#ifdef __cplusplus
}
#endif

#endif /* LCCORE_DISCRETE_GLUE_H */
