/**
  ******************************************************************************
  * @file    hid_glue.h
  * @brief   Seam between the comm wire-protocol layer and the USB HID
  *          joystick subsystem (8 axes + 32 buttons), wire doc §12.
  *
  *          Mirrors discrete_glue: maintains the desired/confirmed report
  *          (writes confirm immediately — the latch into the USB port layer
  *          cannot fail), emits CMD_HID_STATE over UDP on change of
  *          axes/buttons/usb_mounted plus the report_ms heartbeat. The
  *          actual USB device lives behind hid_port.h.
  *
  *          Application personality only: in RECOVERY_BUILD the translation
  *          unit compiles empty and comm_core routes 0x33/0x34 to the
  *          default ERROR ACK.
  ******************************************************************************
  */

#ifndef LCCORE_HID_GLUE_H
#define LCCORE_HID_GLUE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "proto.h"      /* HID_NUM_AXES */

/* One-time init. */
void hid_glue_init(void);

/* Control-session reset: disable the subsystem, center all axes and release
   all buttons on USB (no STATE emitted — the host is gone). Hooked into
   comm_core's channels_reset(). */
void hid_glue_reset(void);

/* CMD_HID_SETUP: enable/disable + heartbeat period. Returns a PROTO_ST_*
   wire status code. */
uint32_t hid_glue_setup(uint8_t enable, uint8_t flags, uint16_t report_ms);

/* CMD_HID_SET: apply masked axes (bit i of axis_mask -> axes[i]) and merge
   desired button bits (btn_apply_mask/btn_values). */
void hid_glue_set(uint8_t axis_mask, const int16_t axes[HID_NUM_AXES],
                  uint32_t btn_apply_mask, uint32_t btn_values);

/* Periodic service (comm task): pump the USB port layer, detect mount
   changes, emit pending / heartbeat STATE frames via comm_core_send_udp. */
void hid_glue_loop(void);

#ifdef __cplusplus
}
#endif

#endif /* LCCORE_HID_GLUE_H */
