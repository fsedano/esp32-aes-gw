/*
 * Modbus RTU backend for the external RS-485 discrete-I/O bus.
 *
 * UART0 is routed to GPIO43 (TX) / GPIO44 (RX) through an automatic-
 * direction RS-485 transceiver. Unit 1 is a Waveshare 32-channel relay
 * module. Unit 2 is an Ebyte M31-U host. The worker maps Waveshare outputs
 * first, followed by M31 outputs; M31 digital inputs start at input zero.
 */

#include "rs485_discrete.h"

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

#define RS485_UART                    UART_NUM_0
#define RS485_TX_GPIO                 43
#define RS485_RX_GPIO                 44
#define RS485_BAUD                    9600

#define WAVESHARE_UNIT                1u
#define WAVESHARE_MAX_OUTPUTS         32u
#define M31_UNIT                      2u
#define M31_MAX_OUTPUTS               32u
#define M31_MAX_INPUTS                1u

#define RS485_POLL_MS                 100u
#define RS485_REPLY_MS                200u
#define RS485_INTERFRAME_MS           4u
#define RS485_LINK_FAILURES           3u
#define RS485_STARTUP_PROBES          3u
#define RS485_STARTUP_RETRY_MS        250u

#define MODBUS_FC_READ_COILS          0x01u
#define MODBUS_FC_READ_INPUTS         0x02u
#define MODBUS_FC_WRITE_COIL          0x05u
#define MODBUS_COIL_ON                0xFF00u

/* Set to the GPIO wired to DE/RE for a manually-directed transceiver. */
#define RS485_DE_GPIO                 (-1)

typedef struct {
    SemaphoreHandle_t lock;
    bool enabled;
    uint64_t desired;
    uint8_t waveshare_outputs;
    uint8_t m31_outputs;
    uint8_t m31_inputs;
    discrete_backend_state_t state;
} rs485_shared_t;

typedef struct {
    unsigned failures;
    bool online;
} device_link_t;

static rs485_shared_t s_bus;

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

static int rtu_exchange(uint8_t unit, const uint8_t *request,
                        size_t request_len, uint8_t *response,
                        size_t expected_len){
    vTaskDelay(pdMS_TO_TICKS(RS485_INTERFRAME_MS));
    uart_flush_input(RS485_UART);

#if RS485_DE_GPIO >= 0
    gpio_set_level(RS485_DE_GPIO, 1);
#endif
    int written = uart_write_bytes(RS485_UART, (const char *)request,
                                   request_len);
    esp_err_t tx_err = uart_wait_tx_done(RS485_UART, pdMS_TO_TICKS(100));
#if RS485_DE_GPIO >= 0
    gpio_set_level(RS485_DE_GPIO, 0);
#endif
    if(written != (int)request_len || tx_err != ESP_OK){
        return -1;
    }

    int received = uart_read_bytes(RS485_UART, response, expected_len,
                                   pdMS_TO_TICKS(RS485_REPLY_MS));
    if(received < 5){
        return -1;
    }
    uint16_t got_crc = (uint16_t)response[received - 2]
                     | ((uint16_t)response[received - 1] << 8);
    if(response[0] != unit
       || modbus_crc16(response, (size_t)received - 2u) != got_crc){
        return -1;
    }
    return received;
}

static bool read_bits(uint8_t unit, uint8_t function, uint16_t address,
                      uint16_t count, uint32_t *bits){
    uint8_t request[8] = {
        unit, function,
        (uint8_t)(address >> 8), (uint8_t)address,
        (uint8_t)(count >> 8), (uint8_t)count,
        0, 0,
    };
    append_crc(request, 6);

    uint8_t byte_count = (uint8_t)((count + 7u) / 8u);
    uint8_t response[9] = {0};
    size_t expected = (size_t)byte_count + 5u;
    int received = rtu_exchange(unit, request, sizeof(request), response,
                                expected);
    if(received != (int)expected || response[1] != function
       || response[2] != byte_count){
        return false;
    }

    uint32_t value = 0;
    for(uint8_t i = 0; i < byte_count; i++){
        value |= (uint32_t)response[3u + i] << (8u * i);
    }
    if(count < 32u){
        value &= (UINT32_C(1) << count) - 1u;
    }
    *bits = value;
    return true;
}

static bool write_coil(uint8_t unit, uint16_t address, bool on){
    uint16_t value = on ? MODBUS_COIL_ON : 0u;
    uint8_t request[8] = {
        unit, MODBUS_FC_WRITE_COIL,
        (uint8_t)(address >> 8), (uint8_t)address,
        (uint8_t)(value >> 8), (uint8_t)value,
        0, 0,
    };
    uint8_t response[8] = {0};
    append_crc(request, 6);
    int received = rtu_exchange(unit, request, sizeof(request), response,
                                sizeof(response));
    return received == (int)sizeof(response)
        && memcmp(request, response, sizeof(request)) == 0;
}

static uint8_t probe_count(const char *name, uint8_t unit, uint8_t function,
                           uint8_t limit){
    uint32_t ignored;
    bool found = read_bits(unit, function, 0, 1, &ignored);
    LOG_INF("rs485: discovery %s unit %u FC%02x address 0: %s",
            name, unit, function, found ? "present" : "no response");
    if(!found){
        return 0;
    }
    uint8_t low = 0;
    uint8_t high = (uint8_t)(limit - 1u);
    while(low < high){
        uint8_t middle = (uint8_t)((low + high + 1u) / 2u);
        found = read_bits(unit, function, middle, 1, &ignored);
        LOG_INF("rs485: discovery %s unit %u FC%02x address %u: %s",
                name, unit, function, middle,
                found ? "present" : "not present");
        if(found){
            low = middle;
        }else{
            high = (uint8_t)(middle - 1u);
        }
    }
    uint8_t count = (uint8_t)(low + 1u);
    LOG_INF("rs485: discovery %s unit %u FC%02x count %u",
            name, unit, function, count);
    return count;
}

static uint32_t mask32(uint8_t count){
    return count >= 32u ? UINT32_MAX
                        : (UINT32_C(1) << count) - 1u;
}

static uint64_t shared_target(uint8_t output_count){
    xSemaphoreTake(s_bus.lock, portMAX_DELAY);
    uint64_t target = s_bus.enabled ? s_bus.desired : 0u;
    xSemaphoreGive(s_bus.lock);
    if(output_count < 64u){
        target &= (UINT64_C(1) << output_count) - 1u;
    }
    return target;
}

static void shared_publish(uint64_t relays, uint32_t inputs,
                           bool input_valid, bool link){
    xSemaphoreTake(s_bus.lock, portMAX_DELAY);
    if(s_bus.state.link && !link && s_bus.enabled){
        s_bus.desired = relays;
    }
    s_bus.state.relay_state = relays;
    s_bus.state.input_state = inputs;
    s_bus.state.input_valid = input_valid ? 1u : 0u;
    s_bus.state.link = link;
    xSemaphoreGive(s_bus.lock);
}

static void backend_set_enabled(bool enabled){
    xSemaphoreTake(s_bus.lock, portMAX_DELAY);
    s_bus.enabled = enabled;
    s_bus.desired = 0;
    xSemaphoreGive(s_bus.lock);
}

static void backend_set_outputs(uint64_t apply_mask, uint64_t values){
    xSemaphoreTake(s_bus.lock, portMAX_DELAY);
    if(s_bus.enabled && s_bus.state.link){
        s_bus.desired = (s_bus.desired & ~apply_mask)
                      | (values & apply_mask);
    }
    xSemaphoreGive(s_bus.lock);
}

static bool backend_get_state(discrete_backend_state_t *state){
    if(state == NULL || s_bus.lock == NULL){
        return false;
    }
    xSemaphoreTake(s_bus.lock, portMAX_DELAY);
    *state = s_bus.state;
    xSemaphoreGive(s_bus.lock);
    return true;
}

static const discrete_backend_ops_t s_backend_ops = {
    .set_enabled = backend_set_enabled,
    .set_outputs = backend_set_outputs,
    .get_state = backend_get_state,
};

static bool apply_target(uint8_t unit, uint32_t current, uint32_t target,
                         uint8_t count){
    uint32_t changed = (current ^ target) & mask32(count);
    for(uint8_t ch = 0; ch < count; ch++){
        uint32_t bit = UINT32_C(1) << ch;
        if((changed & bit) != 0u
           && !write_coil(unit, ch, (target & bit) != 0u)){
            return false;
        }
    }
    return true;
}

static bool service_outputs(uint8_t unit, uint8_t count, uint32_t target,
                            uint32_t *relays){
    if(!read_bits(unit, MODBUS_FC_READ_COILS, 0, count, relays)){
        return false;
    }
    if(*relays == target){
        return true;
    }
    return apply_target(unit, *relays, target, count)
        && read_bits(unit, MODBUS_FC_READ_COILS, 0, count, relays);
}

static void update_link(device_link_t *link, bool ok, const char *name,
                        uint8_t unit){
    if(ok){
        link->failures = 0;
        if(!link->online){
            LOG_INF("rs485: %s unit %u online", name, unit);
        }
        link->online = true;
        return;
    }

    if(link->failures < RS485_LINK_FAILURES){
        link->failures++;
    }
    if(link->online && link->failures >= RS485_LINK_FAILURES){
        link->online = false;
        LOG_INF("rs485: %s unit %u link down after %u failed polls",
                name, unit, link->failures);
    }
}

static void rs485_task(void *arg){
    (void)arg;
    const uint8_t waveshare_outputs = s_bus.waveshare_outputs;
    const uint8_t m31_outputs = s_bus.m31_outputs;
    const uint8_t m31_inputs = s_bus.m31_inputs;
    const uint8_t total_outputs = (uint8_t)(waveshare_outputs + m31_outputs);
    uint32_t waveshare_relays = 0;
    uint32_t m31_relays = 0;
    uint32_t m31_input_state = 0;
    device_link_t waveshare_link = {0};
    device_link_t m31_link = {0};

    if(total_outputs == 0u){
        shared_publish(0, 0, false, false);
        LOG_INF("rs485: no output channels in boot population; link down");
        vTaskDelete(NULL);
        return;
    }

    for(;;){
        uint64_t target = shared_target(total_outputs);

        bool waveshare_ok = false;
        if(waveshare_outputs > 0u){
            uint32_t device_target = (uint32_t)target
                                   & mask32(waveshare_outputs);
            waveshare_ok = service_outputs(WAVESHARE_UNIT,
                                           waveshare_outputs,
                                           device_target,
                                           &waveshare_relays);
            update_link(&waveshare_link, waveshare_ok, "Waveshare",
                        WAVESHARE_UNIT);
        }

        bool m31_ok = false;
        bool m31_input_ok = false;
        if(m31_outputs > 0u){
            uint32_t device_target = (uint32_t)(target >> waveshare_outputs)
                                   & mask32(m31_outputs);
            m31_ok = service_outputs(M31_UNIT, m31_outputs, device_target,
                                     &m31_relays);
            if(m31_ok && m31_inputs > 0u){
                m31_input_ok = read_bits(M31_UNIT, MODBUS_FC_READ_INPUTS, 0,
                                         m31_inputs, &m31_input_state);
            }
            update_link(&m31_link, m31_ok, "M31", M31_UNIT);
        }

        uint64_t relays = waveshare_relays
                        | ((uint64_t)m31_relays << waveshare_outputs);
        bool link = (waveshare_outputs > 0u && waveshare_link.online)
                 || (m31_outputs > 0u && m31_link.online);
        bool input_valid = m31_inputs > 0u && m31_link.online
                        && m31_input_ok;
        shared_publish(relays, m31_input_state, input_valid, link);

        vTaskDelay(pdMS_TO_TICKS(RS485_POLL_MS));
    }
}

void rs485_discrete_init(void){
    memset(&s_bus, 0, sizeof(s_bus));
    s_bus.lock = xSemaphoreCreateMutex();
    if(s_bus.lock == NULL){
        LOG_INF("rs485: mutex allocation failed");
        return;
    }
    discrete_glue_bind(&s_backend_ops);

    const uart_config_t config = {
        .baud_rate = RS485_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_param_config(RS485_UART, &config);
    if(err == ESP_OK){
        err = uart_set_pin(RS485_UART, RS485_TX_GPIO, RS485_RX_GPIO,
                           UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }

#if RS485_DE_GPIO >= 0
    if(err == ESP_OK){
        gpio_config_t de_config = {
            .pin_bit_mask = UINT64_C(1) << RS485_DE_GPIO,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        err = gpio_config(&de_config);
        if(err == ESP_OK){
            gpio_set_level(RS485_DE_GPIO, 0);
        }
    }
#endif

    if(err == ESP_OK){
        err = uart_driver_install(RS485_UART, 256, 0, 0, NULL, 0);
    }
    if(err != ESP_OK){
        LOG_INF("rs485: UART0 initialization failed: %s",
                esp_err_to_name(err));
        return;
    }

    for(unsigned attempt = 0; attempt < RS485_STARTUP_PROBES; attempt++){
        LOG_INF("rs485: discovery attempt %u/%u", attempt + 1u,
                RS485_STARTUP_PROBES);
        if(s_bus.waveshare_outputs == 0u){
            s_bus.waveshare_outputs = probe_count("Waveshare",
                                                  WAVESHARE_UNIT,
                                                  MODBUS_FC_READ_COILS,
                                                  WAVESHARE_MAX_OUTPUTS);
        }
        if(s_bus.m31_outputs == 0u){
            s_bus.m31_outputs = probe_count("M31", M31_UNIT,
                                            MODBUS_FC_READ_COILS,
                                            M31_MAX_OUTPUTS);
        }
        if(s_bus.m31_outputs > 0u && s_bus.m31_inputs == 0u){
            s_bus.m31_inputs = probe_count("M31", M31_UNIT,
                                           MODBUS_FC_READ_INPUTS,
                                           M31_MAX_INPUTS);
        }
        if(s_bus.waveshare_outputs > 0u && s_bus.m31_outputs > 0u
           && s_bus.m31_inputs > 0u){
            break;
        }
        if(attempt + 1u < RS485_STARTUP_PROBES){
            vTaskDelay(pdMS_TO_TICKS(RS485_STARTUP_RETRY_MS));
        }
    }

    LOG_INF("rs485: boot population Waveshare unit %u %u DO; M31 unit %u %u DO/%u DI; UART0 GPIO%d/%d %d 8N1",
            WAVESHARE_UNIT, s_bus.waveshare_outputs,
            M31_UNIT, s_bus.m31_outputs, s_bus.m31_inputs,
            RS485_TX_GPIO, RS485_RX_GPIO, RS485_BAUD);
    if(s_bus.waveshare_outputs > 0u){
        LOG_INF("rs485: Waveshare unit %u mapped to dout.0..%u",
                WAVESHARE_UNIT, s_bus.waveshare_outputs - 1u);
    }
    if(s_bus.m31_outputs > 0u){
        uint8_t first = s_bus.waveshare_outputs;
        uint8_t last = (uint8_t)(first + s_bus.m31_outputs - 1u);
        if(s_bus.m31_inputs > 0u){
            LOG_INF("rs485: M31 unit %u mapped to dout.%u..%u, din.0..%u",
                    M31_UNIT, first, last, s_bus.m31_inputs - 1u);
        }else{
            LOG_INF("rs485: M31 unit %u mapped to dout.%u..%u",
                    M31_UNIT, first, last);
        }
    }
    if(xTaskCreate(rs485_task, "rs485_discrete", 4096, NULL, 5, NULL)
       != pdPASS){
        LOG_INF("rs485: worker task creation failed");
        uart_driver_delete(RS485_UART);
    }
}

uint16_t rs485_discrete_output_count(void){
    return (uint16_t)s_bus.waveshare_outputs + s_bus.m31_outputs;
}

uint16_t rs485_discrete_input_count(void){
    return s_bus.m31_inputs;
}
