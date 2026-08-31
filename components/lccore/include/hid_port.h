/**
  ******************************************************************************
  * @file    hid_port.h
  * @brief   Platform seam for the USB HID joystick backend, mirroring
  *          lc_port.h: pure C11 interface, implemented twice.
  *
  *            - main/hid_usb.c        : TinyUSB device on the ESP32-S3 OTG
  *                                      port (application build only).
  *            - host_tests/support/   : capture stub for the host test suite.
  *
  *          The glue layer (hid_glue.c) is the only caller. All functions run
  *          on the comm task; the implementation is responsible for making the
  *          handoff to any USB stack task safe.
  ******************************************************************************
  */

#ifndef LCCORE_HID_PORT_H
#define LCCORE_HID_PORT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#include "proto.h"      /* HID_NUM_AXES */

/* One-time init of the USB HID backend (TinyUSB install on target; no-op
   counters on host). On the ESP32 this claims the USB OTG PHY, so the
   USB-Serial-JTAG console on that port stops at this point. Called from
   app_main after identity_init (the USB serial string needs the identity). */
void hid_port_init(void);

/* True when a USB host has enumerated the joystick and the bus is not
   suspended. Note: the board has no VBUS sensing, so cable unplug is
   observed as bus suspend. */
bool hid_port_mounted(void);

/* Latch the desired HID report (8 axes + 32 buttons). Cheap and
   non-blocking; callable on every SET. The port coalesces into at most one
   USB report per hid_port_loop() call. */
void hid_port_submit(const int16_t axes[HID_NUM_AXES], uint32_t buttons);

/* Pump: push a pending (dirty) report into the USB stack when it is mounted
   and the IN endpoint is free. Called every comm-task pass, including while
   the HID channel group is disabled (a reset-to-center report must drain
   even after the control session died). */
void hid_port_loop(void);

#ifdef __cplusplus
}
#endif

#endif /* LCCORE_HID_PORT_H */
