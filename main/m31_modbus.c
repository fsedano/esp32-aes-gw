/*
 * Ebyte M31-U Modbus/RTU backend for the AES discrete wire protocol.
 *
 * UART0 is routed to GPIO43 (TX) / GPIO44 (RX) through an automatic-
 * direction RS-485 transceiver. A private worker owns every blocking UART
 * transaction; the Ethernet protocol task only updates a desired mask and
 * copies lock-bounded physical snapshots.
 */

#include "m31_modbus.h"

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "discrete_glue.h"
#include "lc_log.h"

#define M31_UART                  UART_NUM_0
#define M31_TX_GPIO               43
#define M31_RX_GPIO               44
#define M31_BAUD                  9600
#define M31_UNIT                  1u

#define M31_MAX_OUTPUTS           32u
#define M31_MAX_INPUTS            1u
#define M31_POLL_MS               100u
#define M31_REPROBE_MS            1000u
#define M31_REPLY_MS              200u
#define M31_INTERFRAME_MS         4u
#define M31_LINK_FAILURES         3u

#define MODBUS_FC_READ_COILS      0x01u
#define MODBUS_FC_READ_INPUTS     0x02u
#define MODBUS_FC_WRITE_COIL      0x05u
#define MODBUS_COIL_ON            0xFF00u

/* Set to the GPIO wired to DE/RE for a manually-directed transceiver. The
   current board uses automatic direction control, hence -1. */
#define M31_DE_GPIO               (-1)

typedef struct {
    SemaphoreHandle_t lock;
    bool enabled;
    uint32_t desired;
    discrete_backend_state_t state;
} m31_shared_t;

static m31_shared_t s_m31;

static uint16_t modbus_crc16(const uint8_t *data, size_t len){
    uint16_t crc = 0xFFFFu;
    for(size_t i = 0; i < len; i++){
        crc ^= data[i];
        for(unsigned bit = 0; bit < 8; bit++){
            crc = (crc & 1u) ? (uint16_t)((crc >> 1) ^ 0xA001u)
                             : (uint16_t)(crc >> 1);
        }
    }
    return crc;
}

static void append_crc(uint8_t *frame, size_t body_len){
    uint16_t crc = modbus_crc16(frame, body_len);
    frame[body_len] = (uint8_t)crc;
    frame[body_len + 1u] = (uint8_t)(crc >> 8);
}

/* Exchange one RTU request. expected_len is the normal response length; a
   five-byte Modbus exception is accepted by the UART reader but rejected by
   the operation-specific parser. */
static int rtu_exchange(const uint8_t *request, size_t request_len,
                        uint8_t *response, size_t expected_len){
    /* Modbus RTU requires at least 3.5 character times of silence between
       frames. At 9600 8N1 that is 3.65 ms. */
    vTaskDelay(pdMS_TO_TICKS(M31_INTERFRAME_MS));
    uart_flush_input(M31_UART);

#if M31_DE_GPIO >= 0
    gpio_set_level(M31_DE_GPIO, 1);
#endif
    int written = uart_write_bytes(M31_UART, (const char *)request, request_len);
    esp_err_t tx_err = uart_wait_tx_done(M31_UART, pdMS_TO_TICKS(100));
#if M31_DE_GPIO >= 0
    gpio_set_level(M31_DE_GPIO, 0);
#endif
    if(written != (int)request_len || tx_err != ESP_OK){
        return -1;
    }

    int received = uart_read_bytes(M31_UART, response, expected_len,
                                   pdMS_TO_TICKS(M31_REPLY_MS));
    if(received < 5){
        return -1;
    }
    uint16_t got_crc = (uint16_t)response[received - 2]
                     | ((uint16_t)response[received - 1] << 8);
    if(response[0] != M31_UNIT
       || modbus_crc16(response, (size_t)received - 2u) != got_crc){
        return -1;
    }
    return received;
}

static bool read_bits(uint8_t function, uint16_t address, uint16_t count,
                      uint32_t *bits){
    uint8_t request[8] = {
        M31_UNIT, function,
        (uint8_t)(address >> 8), (uint8_t)address,
        (uint8_t)(count >> 8), (uint8_t)count,
        0, 0,
    };
    append_crc(request, 6);

    uint8_t byte_count = (uint8_t)((count + 7u) / 8u);
    uint8_t response[9] = {0}; /* unit + fc + count + up to 4 data + CRC */
    size_t expected = (size_t)byte_count + 5u;
    int received = rtu_exchange(request, sizeof(request), response, expected);
    if(received != (int)expected || response[1] != function
       || response[2] != byte_count){
        return false;
    }

    uint32_t value = 0;
    for(uint8_t i = 0; i < byte_count; i++){
        value |= (uint32_t)response[3u + i] << (8u * i);
    }
    if(count < 32u){
        value &= (1u << count) - 1u;
    }
    *bits = value;
    return true;
}

static bool write_coil(uint16_t address, bool on){
    uint16_t value = on ? MODBUS_COIL_ON : 0u;
    uint8_t request[8] = {
        M31_UNIT, MODBUS_FC_WRITE_COIL,
        (uint8_t)(address >> 8), (uint8_t)address,
        (uint8_t)(value >> 8), (uint8_t)value,
        0, 0,
    };
    uint8_t response[8] = {0};
    append_crc(request, 6);
    int received = rtu_exchange(request, sizeof(request), response,
                                sizeof(response));
    return received == (int)sizeof(response)
        && memcmp(request, response, sizeof(request)) == 0;
}

/* M31 maps host and expansion channels contiguously. Probe addresses rather
   than request counts because firmware 1.3 masks the count field to 8 bits.
   The returned count is capped at the product's 32-bit wire width. */
static uint8_t probe_count(uint8_t function, uint8_t limit){
    uint32_t ignored;
    if(!read_bits(function, 0, 1, &ignored)){
        return 0;
    }
    uint8_t low = 0;
    uint8_t high = (uint8_t)(limit - 1u);
    while(low < high){
        uint8_t middle = (uint8_t)((low + high + 1u) / 2u);
        if(read_bits(function, middle, 1, &ignored)){
            low = middle;
        }else{
            high = (uint8_t)(middle - 1u);
        }
    }
    return (uint8_t)(low + 1u);
}

static void shared_publish(uint32_t relays, uint32_t inputs,
                           uint32_t input_valid, bool link){
    xSemaphoreTake(s_m31.lock, portMAX_DELAY);
    s_m31.state.relay_state = relays;
    s_m31.state.input_state = inputs;
    s_m31.state.input_valid = input_valid;
    s_m31.state.link = link;
    xSemaphoreGive(s_m31.lock);
}

static uint32_t shared_target(uint8_t output_count){
    xSemaphoreTake(s_m31.lock, portMAX_DELAY);
    uint32_t target = s_m31.enabled ? s_m31.desired : 0u;
    xSemaphoreGive(s_m31.lock);
    if(output_count < 32u){
        target &= (1u << output_count) - 1u;
    }
    return target;
}

/* Drop queued commands when the physical link becomes unavailable. The
   gateway retains its pending mask and will retransmit after link recovery. */
static void shared_link_down(uint32_t last_relays){
    xSemaphoreTake(s_m31.lock, portMAX_DELAY);
    s_m31.desired = last_relays;
    s_m31.state.relay_state = last_relays;
    s_m31.state.input_valid = 0;
    s_m31.state.link = false;
    xSemaphoreGive(s_m31.lock);
}

static void backend_set_enabled(bool enabled){
    xSemaphoreTake(s_m31.lock, portMAX_DELAY);
    s_m31.enabled = enabled;
    /* Every enable edge starts from fail-safe off. The gateway deliberately
       does not replay relay state after a control-session bounce. */
    s_m31.desired = 0;
    xSemaphoreGive(s_m31.lock);
}

static void backend_set_outputs(uint32_t apply_mask, uint32_t values){
    xSemaphoreTake(s_m31.lock, portMAX_DELAY);
    if(s_m31.enabled && s_m31.state.link){
        s_m31.desired = (s_m31.desired & ~apply_mask)
                      | (values & apply_mask);
    }
    xSemaphoreGive(s_m31.lock);
}

static bool backend_get_state(discrete_backend_state_t *state){
    if(state == NULL || s_m31.lock == NULL){
        return false;
    }
    xSemaphoreTake(s_m31.lock, portMAX_DELAY);
    *state = s_m31.state;
    xSemaphoreGive(s_m31.lock);
    return true;
}

static const discrete_backend_ops_t s_backend_ops = {
    .set_enabled = backend_set_enabled,
    .set_outputs = backend_set_outputs,
    .get_state = backend_get_state,
};

static bool apply_target(uint32_t current, uint32_t target, uint8_t count){
    uint32_t changed = (current ^ target);
    if(count < 32u){
        changed &= (1u << count) - 1u;
    }
    for(uint8_t ch = 0; ch < count; ch++){
        uint32_t bit = 1u << ch;
        if((changed & bit) != 0u && !write_coil(ch, (target & bit) != 0u)){
            return false;
        }
    }
    return true;
}

static void m31_task(void *arg){
    (void)arg;
    uint8_t output_count = 0;
    uint8_t input_count = 0;
    uint32_t relays = 0;
    uint32_t inputs = 0;
    unsigned failures = 0;
    bool announced_up = false;
    bool announced_shape = false;

    for(;;){
        if(output_count == 0){
            output_count = probe_count(MODBUS_FC_READ_COILS, M31_MAX_OUTPUTS);
            input_count = probe_count(MODBUS_FC_READ_INPUTS, M31_MAX_INPUTS);

            /* Until the wire protocol carries actual channel counts, any
               installation with at least one mapped output is usable. */
            if(output_count == 0){
                shared_publish(relays, 0, 0, false);
                if(!announced_shape){
                    LOG_INF("m31: no output channels detected; link down");
                    announced_shape = true;
                }
                vTaskDelay(pdMS_TO_TICKS(M31_REPROBE_MS));
                continue;
            }
            announced_shape = false;
            failures = 0;
        }

        bool ok = read_bits(MODBUS_FC_READ_COILS, 0, output_count, &relays);
        if(ok && input_count > 0){
            ok = read_bits(MODBUS_FC_READ_INPUTS, 0, input_count, &inputs);
        }else if(input_count == 0){
            inputs = 0;
        }
        if(ok){
            /* Re-read the shared target after each pass. If a TCP session
               drops while an FC05 is already in flight, the next pass sees
               the new zero target and opens that relay immediately. */
            for(unsigned pass = 0; pass < 3u && ok; pass++){
                uint32_t target = shared_target(output_count);
                if(relays == target){
                    break;
                }
                ok = apply_target(relays, target, output_count)
                  && read_bits(MODBUS_FC_READ_COILS, 0, output_count, &relays);
            }
            if(ok){
                failures = 0;
                /* Publish only confirming FC01/FC02 reads, never an FC05
                   echo (which confirms receipt, not physical state). */
                shared_publish(relays, inputs, input_count > 0 ? 1u : 0u,
                               true);
                if(!announced_up){
                    LOG_INF("m31: online, detected %u DO/%u DI over UART0 %d 8N1",
                            output_count, input_count, M31_BAUD);
                    announced_up = true;
                }
            }
        }

        if(!ok){
            failures++;
            if(failures >= M31_LINK_FAILURES){
                shared_link_down(relays);
                output_count = 0;
                input_count = 0;
                if(announced_up){
                    LOG_INF("m31: Modbus link down after %u failed polls",
                            failures);
                }
                announced_up = false;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(M31_POLL_MS));
    }
}

void m31_modbus_init(void){
    memset(&s_m31, 0, sizeof(s_m31));
    s_m31.lock = xSemaphoreCreateMutex();
    if(s_m31.lock == NULL){
        LOG_INF("m31: mutex allocation failed");
        return;
    }
    discrete_glue_bind(&s_backend_ops);

    const uart_config_t config = {
        .baud_rate = M31_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_param_config(M31_UART, &config);
    if(err == ESP_OK){
        err = uart_set_pin(M31_UART, M31_TX_GPIO, M31_RX_GPIO,
                           UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }

#if M31_DE_GPIO >= 0
    if(err == ESP_OK){
        gpio_config_t de_config = {
            .pin_bit_mask = 1ULL << M31_DE_GPIO,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        err = gpio_config(&de_config);
        if(err == ESP_OK){
            gpio_set_level(M31_DE_GPIO, 0);
        }
    }
#endif

    if(err == ESP_OK){
        err = uart_driver_install(M31_UART, 256, 0, 0, NULL, 0);
    }
    if(err != ESP_OK){
        LOG_INF("m31: UART0 initialization failed: %s", esp_err_to_name(err));
        return;
    }

    LOG_INF("m31: starting Modbus RTU on UART0 GPIO%d/%d, unit %u",
            M31_TX_GPIO, M31_RX_GPIO, M31_UNIT);
    if(xTaskCreate(m31_task, "m31_modbus", 4096, NULL, 5, NULL) != pdPASS){
        LOG_INF("m31: worker task creation failed");
        uart_driver_delete(M31_UART);
    }
}
