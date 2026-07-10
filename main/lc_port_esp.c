/**
  ******************************************************************************
  * @file    lc_port_esp.c
  * @brief   ESP-IDF implementation of the lccore port seam (lc_port.h).
  ******************************************************************************
  */

#include "lc_port.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "lc";

static SemaphoreHandle_t s_lock;

void lc_port_init(void){
    if(s_lock == NULL){
        s_lock = xSemaphoreCreateMutex();
    }
}

uint32_t lc_port_tick_ms(void){
    return (uint32_t)(esp_timer_get_time() / 1000LL);
}

void lc_port_lock(void){
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

void lc_port_unlock(void){
    xSemaphoreGive(s_lock);
}

void lc_port_console(const char *line){
    /* Developer mirror of every wire-log line (any level). */
    ESP_LOGI(TAG, "%s", line);
}
