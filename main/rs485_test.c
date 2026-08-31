/*
 * Continuous UART test pattern for RS-485 cabling bring-up.
 *
 * The Waveshare ESP32-S3-ETH routes its exposed UART connector to UART0:
 * GPIO43 is TX and GPIO44 is RX.  The ESP-IDF UART console is disabled so
 * this peripheral and these pins are owned exclusively by the RS-485 path.
 */

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lc_log.h"
#include "rs485_test.h"

#define RS485_TEST_UART       UART_NUM_0
#define RS485_TEST_TX_GPIO    43
#define RS485_TEST_RX_GPIO    44
#define RS485_TEST_BAUD       115200
#define RS485_TEST_PERIOD_MS  500

/*
 * Set this to the GPIO connected to the transceiver's DE/RE input when the
 * adapter does not have automatic direction control.  Leave at -1 for an
 * auto-direction adapter.  DE is asserted only while a frame is being sent.
 */
#define RS485_TEST_DE_GPIO    (-1)

static const uint8_t s_pattern[] = {
    0x55, 0xAA, 0x00, 0xFF,
    'R', 'S', '4', '8', '5', '-', 'T', 'E', 'S', 'T',
    '\r', '\n'
};

static void rs485_test_task(void *arg){
    (void)arg;

    for(;;){
#if RS485_TEST_DE_GPIO >= 0
        gpio_set_level(RS485_TEST_DE_GPIO, 1);
#endif

        int written = uart_write_bytes(RS485_TEST_UART,
                                       (const char *)s_pattern,
                                       sizeof(s_pattern));
        if(written != (int)sizeof(s_pattern)){
            LOG_INF("rs485 test: short UART write %d/%u bytes", written,
                    (unsigned)sizeof(s_pattern));
        }

        /* Do not release DE until the final stop bit has left the UART. */
        uart_wait_tx_done(RS485_TEST_UART, pdMS_TO_TICKS(100));
#if RS485_TEST_DE_GPIO >= 0
        gpio_set_level(RS485_TEST_DE_GPIO, 0);
#endif

        vTaskDelay(pdMS_TO_TICKS(RS485_TEST_PERIOD_MS));
    }
}

void rs485_test_init(void){
    const uart_config_t config = {
        .baud_rate = RS485_TEST_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_param_config(RS485_TEST_UART, &config);
    if(err != ESP_OK){
        LOG_INF("rs485 test: UART config failed: %s", esp_err_to_name(err));
        return;
    }

    err = uart_set_pin(RS485_TEST_UART, RS485_TEST_TX_GPIO, RS485_TEST_RX_GPIO,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if(err != ESP_OK){
        LOG_INF("rs485 test: UART pin setup failed: %s", esp_err_to_name(err));
        return;
    }

#if RS485_TEST_DE_GPIO >= 0
    gpio_config_t de_config = {
        .pin_bit_mask = 1ULL << RS485_TEST_DE_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&de_config);
    if(err != ESP_OK){
        LOG_INF("rs485 test: DE GPIO setup failed: %s", esp_err_to_name(err));
        return;
    }
    gpio_set_level(RS485_TEST_DE_GPIO, 0);
#endif

    err = uart_driver_install(RS485_TEST_UART, 256, 0, 0, NULL, 0);
    if(err != ESP_OK){
        LOG_INF("rs485 test: UART driver install failed: %s",
                esp_err_to_name(err));
        return;
    }

    LOG_INF("rs485 test: sending %u-byte pattern on UART0 TX=GPIO%d at %d baud",
            (unsigned)sizeof(s_pattern), RS485_TEST_TX_GPIO, RS485_TEST_BAUD);
    if(xTaskCreate(rs485_test_task, "rs485_test", 3072, NULL, 5, NULL)
       != pdPASS){
        LOG_INF("rs485 test: task creation failed");
        uart_driver_delete(RS485_TEST_UART);
    }
}
