/**
  ******************************************************************************
  * @file    arinc_glue.h
  * @brief   Seam between the comm wire-protocol layer and the ARINC TX/RX
  *          hardware, mirroring arinc4i4o's arinc_glue.h.
  *
  *          On this card the hardware layer is a STUB (no ARINC transceivers
  *          are driven yet): the glue maintains per-channel timetable/label
  *          state, validates arguments, returns the wire status codes the
  *          real engine would, and logs what it would have driven. comm_core
  *          never touches hardware — it calls only this seam, so the real
  *          engine can slot in later without touching the protocol layer.
  *
  *          Functions return PROTO_ST_* wire status codes directly (the STM32
  *          version returns engine status_t which comm maps; without an
  *          engine the indirection would be noise).
  ******************************************************************************
  */

#ifndef LCCORE_ARINC_GLUE_H
#define LCCORE_ARINC_GLUE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

/* One-time init of the (stub) TX engine. */
void atx_glue_init(void);
/* Disable all channels, clear timetables (control-session reset). */
void atx_glue_reset(void);

/* speed/mode use the proto.h numeric codes. chnl is pre-validated by comm
   (0 <= chnl < ARINC_NUM_CHANNELS). */
uint32_t atx_glue_setup(uint8_t chnl, uint8_t speed, uint8_t mode,
                        uint16_t manual_delay_us);
uint32_t atx_glue_add_label(uint8_t chnl, uint16_t label_id,
                            float timing_max_ms, float timing_min_ms);
uint32_t atx_glue_build(uint8_t chnl);
uint32_t atx_glue_enable(uint8_t chnl, bool enable);
uint32_t atx_glue_set_label(uint8_t chnl, uint32_t packet,
                            bool sdi_is_data, bool enable);
uint32_t atx_glue_set_label_manual(uint8_t chnl, uint32_t packet);

#ifdef __cplusplus
}
#endif

#endif /* LCCORE_ARINC_GLUE_H */
