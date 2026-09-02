/**
  ******************************************************************************
  * @file    capabilities.h
  * @brief   Boot-time-fixed linecard capability descriptor (wire cmd 0x26).
  ******************************************************************************
  */

#ifndef LCCORE_CAPABILITIES_H
#define LCCORE_CAPABILITIES_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#define CAPS_DESC_VERSION  1u
#define CAPS_CHUNK_MAX     240u

/* Build the immutable version-1 descriptor from the boot-time discrete process
   image. The fixed USB HID complement is appended automatically. */
void capabilities_init(uint16_t discrete_inputs, uint16_t discrete_outputs);

/* True only after a descriptor was successfully built (application mode). */
bool capabilities_available(void);

/* Highest descriptor version served, or 0 when unavailable. */
uint8_t capabilities_version(void);

/* Immutable blob pointer; writes its byte length when len is non-NULL. */
const uint8_t *capabilities_blob(uint16_t *len);

/* Log the descriptor inventory in a human-readable wire-log entry. */
void capabilities_log_summary(void);

#ifdef __cplusplus
}
#endif

#endif /* LCCORE_CAPABILITIES_H */
