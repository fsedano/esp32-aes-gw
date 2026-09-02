/**
  ******************************************************************************
  * @file    capabilities.c
  * @brief   Version-1 nested-TLV capability descriptor builder.
  ******************************************************************************
  */

#include "capabilities.h"

#include "board_id.h"
#include "lc_log.h"
#include "proto.h"

#include <stddef.h>
#include <string.h>

#define CAPS_BLOB_MAX             256u

#define CAPS_TAG_BOARD_INFO       0x01u
#define CAPS_TAG_PORT_GROUP       0x02u

#define CAPS_BOARD_DISPLAY_NAME   0x01u
#define CAPS_BOARD_ID             0x02u

#define CAPS_KIND_DISCRETE        0x02u
#define CAPS_KIND_HID_AXIS        0x05u
#define CAPS_KIND_HID_BUTTON      0x06u

#define CAPS_GROUP_LABEL          0x01u
#define CAPS_DISCRETE_OUT_DRIVER  0x10u
#define CAPS_OUT_DRIVER_RELAY     0x00u

#define CAPS_DISPLAY_NAME         "AES ESP32 RS-485 discrete I/O + USB HID"
#define CAPS_DISCRETE_LABEL       "RS-485 relay I/O"
#define CAPS_HID_AXIS_LABEL       "USB HID axes"
#define CAPS_HID_BUTTON_LABEL     "USB HID buttons"

_Static_assert(sizeof(CAPS_DISPLAY_NAME) - 1u <= 48u,
               "capability display name exceeds 48 bytes");
_Static_assert(sizeof(CAPS_DISCRETE_LABEL) - 1u <= 32u,
               "capability group label exceeds 32 bytes");
_Static_assert(sizeof(CAPS_HID_AXIS_LABEL) - 1u <= 32u,
               "capability group label exceeds 32 bytes");
_Static_assert(sizeof(CAPS_HID_BUTTON_LABEL) - 1u <= 32u,
               "capability group label exceeds 32 bytes");

typedef struct {
    uint8_t data[CAPS_BLOB_MAX];
    uint16_t len;
    bool ok;
} caps_builder_t;

static uint8_t s_blob[CAPS_BLOB_MAX];
static uint16_t s_blob_len;
static uint16_t s_discrete_inputs;
static uint16_t s_discrete_outputs;
static bool s_available;

#ifndef RECOVERY_BUILD
static void put_u8(caps_builder_t *b, uint8_t value){
    if(!b->ok || b->len >= sizeof(b->data)){
        b->ok = false;
        return;
    }
    b->data[b->len++] = value;
}

static void put_u16(caps_builder_t *b, uint16_t value){
    put_u8(b, (uint8_t)value);
    put_u8(b, (uint8_t)(value >> 8));
}

static void put_u32(caps_builder_t *b, uint32_t value){
    put_u8(b, (uint8_t)value);
    put_u8(b, (uint8_t)(value >> 8));
    put_u8(b, (uint8_t)(value >> 16));
    put_u8(b, (uint8_t)(value >> 24));
}

static void put_bytes(caps_builder_t *b, const void *data, uint16_t len){
    if(!b->ok || len > (uint16_t)(sizeof(b->data) - b->len)){
        b->ok = false;
        return;
    }
    memcpy(b->data + b->len, data, len);
    b->len = (uint16_t)(b->len + len);
}

/* Starts a TLV and returns the value offset used by end_tlv(). */
static uint16_t begin_tlv(caps_builder_t *b, uint8_t tag){
    put_u8(b, tag);
    put_u16(b, 0);
    return b->len;
}

static void end_tlv(caps_builder_t *b, uint16_t value_start){
    if(!b->ok || value_start < 3u || value_start > b->len){
        b->ok = false;
        return;
    }
    uint16_t value_len = (uint16_t)(b->len - value_start);
    b->data[value_start - 2u] = (uint8_t)value_len;
    b->data[value_start - 1u] = (uint8_t)(value_len >> 8);
}

static void put_string_tlv(caps_builder_t *b, uint8_t tag, const char *text){
    size_t n = strlen(text);
    if(n > UINT16_MAX){
        b->ok = false;
        return;
    }
    uint16_t start = begin_tlv(b, tag);
    put_bytes(b, text, (uint16_t)n);
    end_tlv(b, start);
}

static void begin_group(caps_builder_t *b, uint8_t kind,
                        uint32_t inputs, uint32_t outputs,
                        const char *label, uint16_t *group_start){
    *group_start = begin_tlv(b, CAPS_TAG_PORT_GROUP);
    put_u8(b, kind);
    put_u8(b, 0);                    /* reserved */
    put_u32(b, inputs);
    put_u32(b, outputs);
    put_u32(b, 0);                   /* ch_count */
    put_string_tlv(b, CAPS_GROUP_LABEL, label);
}
#endif

void capabilities_init(uint16_t discrete_inputs, uint16_t discrete_outputs){
    s_available = false;
    s_blob_len = 0;
    s_discrete_inputs = 0;
    s_discrete_outputs = 0;

#ifdef RECOVERY_BUILD
    (void)discrete_inputs;
    (void)discrete_outputs;
    return;
#else
    caps_builder_t b = { .len = 0, .ok = true };

    put_u8(&b, CAPS_DESC_VERSION);
    put_u8(&b, 0);                    /* flags */
    put_u16(&b, 0);                   /* reserved */

    uint16_t board = begin_tlv(&b, CAPS_TAG_BOARD_INFO);
    put_string_tlv(&b, CAPS_BOARD_DISPLAY_NAME, CAPS_DISPLAY_NAME);
    put_string_tlv(&b, CAPS_BOARD_ID, BOARD_INFO_SHORT_ID);
    end_tlv(&b, board);

    /* First group is the primary protocol. Omit it only when no discrete
       process image was discovered; HID then becomes the primary group. */
    if(discrete_inputs > 0 || discrete_outputs > 0){
        uint16_t group;
        begin_group(&b, CAPS_KIND_DISCRETE, discrete_inputs,
                    discrete_outputs, CAPS_DISCRETE_LABEL, &group);
        uint8_t relay = CAPS_OUT_DRIVER_RELAY;
        uint16_t driver = begin_tlv(&b, CAPS_DISCRETE_OUT_DRIVER);
        put_bytes(&b, &relay, 1);
        end_tlv(&b, driver);
        end_tlv(&b, group);
    }

    uint16_t axes;
    begin_group(&b, CAPS_KIND_HID_AXIS, 0, HID_NUM_AXES,
                CAPS_HID_AXIS_LABEL, &axes);
    end_tlv(&b, axes);

    uint16_t buttons;
    begin_group(&b, CAPS_KIND_HID_BUTTON, 0, HID_NUM_BUTTONS,
                CAPS_HID_BUTTON_LABEL, &buttons);
    end_tlv(&b, buttons);

    if(!b.ok || b.len == 0){
        return;
    }
    memcpy(s_blob, b.data, b.len);
    s_blob_len = b.len;
    s_discrete_inputs = discrete_inputs;
    s_discrete_outputs = discrete_outputs;
    s_available = true;
#endif
}

bool capabilities_available(void){
    return s_available;
}

uint8_t capabilities_version(void){
    return s_available ? CAPS_DESC_VERSION : 0u;
}

const uint8_t *capabilities_blob(uint16_t *len){
    if(len != NULL){
        *len = s_available ? s_blob_len : 0u;
    }
    return s_available ? s_blob : NULL;
}

void capabilities_log_summary(void){
    if(!s_available){
        return;
    }
    LOG_INF("caps: v%u %u bytes; discrete %u DI/%u DO relay; HID %u axes/%u buttons",
            CAPS_DESC_VERSION, s_blob_len,
            s_discrete_inputs, s_discrete_outputs,
            HID_NUM_AXES, HID_NUM_BUTTONS);
}
