/**
  ******************************************************************************
  * @file    find_me.c
  * @brief   FIND_ME locator blink.
  *
  *          The Waveshare ESP32-S3-ETH has no simple user LED wired to a
  *          plain GPIO (the on-board RGB LED is an addressable WS2812), so
  *          the blink target is compile-time configurable and DISABLED by
  *          default: FIND_ME just logs. Define FIND_ME_LED_GPIO to a valid
  *          output pin to get a 5 Hz blink for the requested duration.
  ******************************************************************************
  */

#include "find_me.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

#ifndef FIND_ME_LED_GPIO
#define FIND_ME_LED_GPIO    (-1)    /* -1: no LED, log only */
#endif

static const char *TAG = "find_me";
static volatile int64_t s_deadline_us;

#if FIND_ME_LED_GPIO >= 0
static void find_me_task(void *arg){
    (void)arg;
    for(;;){
        if(esp_timer_get_time() < s_deadline_us){
            gpio_set_level(FIND_ME_LED_GPIO, 1);
            vTaskDelay(pdMS_TO_TICKS(100));
            gpio_set_level(FIND_ME_LED_GPIO, 0);
            vTaskDelay(pdMS_TO_TICKS(100));
        }else{
            gpio_set_level(FIND_ME_LED_GPIO, 0);
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}
#endif

void find_me_init(void){
#if FIND_ME_LED_GPIO >= 0
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << FIND_ME_LED_GPIO,
        .mode         = GPIO_MODE_OUTPUT,
    };
    gpio_config(&cfg);
    xTaskCreate(find_me_task, "find_me", 2048, NULL, 2, NULL);
#endif
}

void find_me_start(uint32_t duration_s){
    s_deadline_us = esp_timer_get_time() + (int64_t)duration_s * 1000000LL;
#if FIND_ME_LED_GPIO >= 0
    ESP_LOGI(TAG, "blinking LED (GPIO%d) for %us", FIND_ME_LED_GPIO,
             (unsigned)duration_s);
#else
    ESP_LOGI(TAG, "FIND ME! (no locator LED on this board) for %us",
             (unsigned)duration_s);
#endif
}
