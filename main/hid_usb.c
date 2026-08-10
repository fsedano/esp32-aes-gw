/**
  ******************************************************************************
  * @file    hid_usb.c
  * @brief   TinyUSB implementation of the hid_port.h seam: the ESP32-S3
  *          presents a USB HID joystick (8 x 16-bit axes + 32 buttons) on
  *          the OTG port. Application build only — the recovery build keeps
  *          the USB-Serial-JTAG console on that port.
  *
  *          Concurrency: two tasks touch this state — the comm task (via
  *          hid_port_submit / hid_port_loop) and the tusb task created by
  *          tinyusb_driver_install (mount/suspend/get_report callbacks).
  *          The latched report + dirty flag live under a portMUX spinlock;
  *          tud_hid_report() is only ever called from the comm task, which
  *          is the one-sender pattern TinyUSB's FreeRTOS OSAL supports.
  *          Mount/suspend flags are single volatile bools (word-atomic).
  ******************************************************************************
  */

#ifndef RECOVERY_BUILD

#include "hid_port.h"

#include "freertos/FreeRTOS.h"

#include "esp_log.h"
#include "tinyusb.h"
#include "class/hid/hid_device.h"

#include "identity.h"
#include "proto.h"

#include <string.h>

static const char *TAG = "hid_usb";

/* IN report: must match the report descriptor below. No report ID. */
typedef struct __attribute__((packed)) {
    int16_t  axes[HID_NUM_AXES];    /* X Y Z Rx Ry Rz Slider Dial */
    uint32_t buttons;               /* bit i = button i+1         */
} hid_report_t;
_Static_assert(sizeof(hid_report_t) == 20, "HID report layout");

static portMUX_TYPE  s_mux = portMUX_INITIALIZER_UNLOCKED;
static hid_report_t  s_report;      /* latched desired report          */
static bool          s_dirty;       /* report differs from last sent   */
static volatile bool s_mounted;     /* tud_mount/umount edge           */
static volatile bool s_suspended;   /* tud_suspend/resume edge         */

/* ---------------------------------------------------------------------- */
/* Descriptors                                                            */
/* ---------------------------------------------------------------------- */

static const uint8_t s_hid_report_desc[] = {
    0x05, 0x01,         /* Usage Page (Generic Desktop)          */
    0x09, 0x04,         /* Usage (Joystick)                      */
    0xA1, 0x01,         /* Collection (Application)              */
    0x16, 0x01, 0x80,   /*   Logical Minimum (-32767)            */
    0x26, 0xFF, 0x7F,   /*   Logical Maximum (32767)             */
    0x75, 0x10,         /*   Report Size (16)                    */
    0x95, 0x08,         /*   Report Count (8)                    */
    0x09, 0x30,         /*   Usage (X)                           */
    0x09, 0x31,         /*   Usage (Y)                           */
    0x09, 0x32,         /*   Usage (Z)                           */
    0x09, 0x33,         /*   Usage (Rx)                          */
    0x09, 0x34,         /*   Usage (Ry)                          */
    0x09, 0x35,         /*   Usage (Rz)                          */
    0x09, 0x36,         /*   Usage (Slider)                      */
    0x09, 0x37,         /*   Usage (Dial)                        */
    0x81, 0x02,         /*   Input (Data,Var,Abs)                */
    0x05, 0x09,         /*   Usage Page (Button)                 */
    0x19, 0x01,         /*   Usage Minimum (Button 1)            */
    0x29, 0x20,         /*   Usage Maximum (Button 32)           */
    0x15, 0x00,         /*   Logical Minimum (0)                 */
    0x25, 0x01,         /*   Logical Maximum (1)                 */
    0x75, 0x01,         /*   Report Size (1)                     */
    0x95, 0x20,         /*   Report Count (32)                   */
    0x81, 0x02,         /*   Input (Data,Var,Abs)                */
    0xC0,               /* End Collection                        */
};

/* Espressif VID with a PID in the community 0x8xxx space. Lab/private
   product; if this ever ships publicly, request a real PID via
   github.com/espressif/usb-pids (free) instead. */
#define HID_USB_VID     0x303A
#define HID_USB_PID     0x8110

static const tusb_desc_device_t s_desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = HID_USB_VID,
    .idProduct          = HID_USB_PID,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01,
};

enum { ITF_NUM_HID = 0, ITF_NUM_TOTAL };
#define HID_EP_IN           0x81
#define CONFIG_TOTAL_LEN    (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN)

static const uint8_t s_desc_configuration[] = {
    /* config number, interface count, string index, total length,
       attributes, power (mA) */
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0, 100),
    /* interface, string index, boot protocol, report descriptor len,
       EP IN address, size, polling interval (ms) */
    TUD_HID_DESCRIPTOR(ITF_NUM_HID, 4, HID_ITF_PROTOCOL_NONE,
                       sizeof(s_hid_report_desc), HID_EP_IN, 64, 1),
};

/* Index 3 (serial) is patched at init time from the board identity so
   Windows keeps per-device joystick calibration across USB ports. */
static const char *s_string_desc[] = {
    (const char[]){ 0x09, 0x04 },       /* 0: language = English (US)  */
    "fsedano",                          /* 1: manufacturer             */
    "A429-ESP_4DH Joystick",            /* 2: product                  */
    "000000000000",                     /* 3: serial (placeholder)     */
    "A429-ESP_4DH HID",                 /* 4: HID interface            */
};

/* ---------------------------------------------------------------------- */
/* hid_port.h implementation                                              */
/* ---------------------------------------------------------------------- */

void hid_port_init(void){
    s_string_desc[3] = identity_serial();

    const tinyusb_config_t cfg = {
        .device_descriptor        = &s_desc_device,
        .string_descriptor        = s_string_desc,
        .string_descriptor_count  = sizeof(s_string_desc) / sizeof(s_string_desc[0]),
        .external_phy             = false,
        .configuration_descriptor = s_desc_configuration,
    };
    ESP_ERROR_CHECK(tinyusb_driver_install(&cfg));
    ESP_LOGI(TAG, "USB HID joystick up (VID %04x PID %04x)",
             HID_USB_VID, HID_USB_PID);
}

bool hid_port_mounted(void){
    return s_mounted && !s_suspended;
}

void hid_port_submit(const int16_t axes[HID_NUM_AXES], uint32_t buttons){
    portENTER_CRITICAL(&s_mux);
    memcpy(s_report.axes, axes, sizeof(s_report.axes));
    s_report.buttons = buttons;
    s_dirty = true;
    portEXIT_CRITICAL(&s_mux);
}

void hid_port_loop(void){
    if(!s_dirty || !tud_mounted() || !tud_hid_ready()){
        return;
    }
    hid_report_t snap;
    portENTER_CRITICAL(&s_mux);
    snap    = s_report;
    s_dirty = false;
    portEXIT_CRITICAL(&s_mux);
    if(!tud_hid_report(0 /* no report id */, &snap, sizeof(snap))){
        /* Endpoint raced busy: retry next pass with the latest state. */
        portENTER_CRITICAL(&s_mux);
        s_dirty = true;
        portEXIT_CRITICAL(&s_mux);
    }
}

/* ---------------------------------------------------------------------- */
/* TinyUSB callbacks (run in the tusb task)                               */
/* ---------------------------------------------------------------------- */

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance){
    (void)instance;
    return s_hid_report_desc;
}

/* Host GET_REPORT control request: reply with the current latched state. */
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type,
                               uint8_t *buffer, uint16_t reqlen){
    (void)instance; (void)report_id; (void)report_type;
    if(reqlen < sizeof(hid_report_t)){
        return 0;
    }
    portENTER_CRITICAL(&s_mux);
    memcpy(buffer, &s_report, sizeof(hid_report_t));
    portEXIT_CRITICAL(&s_mux);
    return sizeof(hid_report_t);
}

/* No OUT reports in the descriptor. */
void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type,
                           uint8_t const *buffer, uint16_t bufsize){
    (void)instance; (void)report_id; (void)report_type;
    (void)buffer; (void)bufsize;
}

void tud_mount_cb(void){
    s_mounted = true;
    /* A freshly enumerated host gets the current latched state. */
    portENTER_CRITICAL(&s_mux);
    s_dirty = true;
    portEXIT_CRITICAL(&s_mux);
}

void tud_umount_cb(void){
    s_mounted = false;
}

void tud_suspend_cb(bool remote_wakeup_en){
    (void)remote_wakeup_en;
    s_suspended = true;
}

void tud_resume_cb(void){
    s_suspended = false;
    portENTER_CRITICAL(&s_mux);
    s_dirty = true;
    portEXIT_CRITICAL(&s_mux);
}

#endif /* !RECOVERY_BUILD */
