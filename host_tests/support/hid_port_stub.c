/* Host capture implementation of hid_port.h: records what the glue would
   have pushed onto USB, with a settable mounted flag. */

#include "hid_port_stub.h"
#include "hid_port.h"

#include <string.h>

int16_t  hid_stub_axes[HID_NUM_AXES];
uint32_t hid_stub_buttons;
int      hid_stub_submits;
int      hid_stub_loops;

static bool g_mounted;

void hid_port_init(void){
    memset(hid_stub_axes, 0, sizeof(hid_stub_axes));
    hid_stub_buttons = 0;
    hid_stub_submits = 0;
    hid_stub_loops   = 0;
    g_mounted        = false;
}

bool hid_port_mounted(void){
    return g_mounted;
}

void hid_port_submit(const int16_t axes[HID_NUM_AXES], uint32_t buttons){
    memcpy(hid_stub_axes, axes, sizeof(hid_stub_axes));
    hid_stub_buttons = buttons;
    hid_stub_submits++;
}

void hid_port_loop(void){
    hid_stub_loops++;
}

void hid_port_stub_set_mounted(bool mounted){
    g_mounted = mounted;
}
