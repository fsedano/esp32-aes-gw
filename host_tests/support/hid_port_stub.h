/* Test hooks for the host hid_port capture stub (support/hid_port_stub.c). */

#ifndef HID_PORT_STUB_H
#define HID_PORT_STUB_H

#include <stdbool.h>
#include <stdint.h>

#include "proto.h"

/* Last submitted report + call counters. */
extern int16_t  hid_stub_axes[HID_NUM_AXES];
extern uint32_t hid_stub_buttons;
extern int      hid_stub_submits;
extern int      hid_stub_loops;

/* Simulate USB enumeration/suspend from the test. */
void hid_port_stub_set_mounted(bool mounted);

#endif /* HID_PORT_STUB_H */
