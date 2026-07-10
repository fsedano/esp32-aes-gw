/**
  ******************************************************************************
  * @file    ota_ops.c
  * @brief   Boot-partition management (see ota_ops.h).
  ******************************************************************************
  */

#include "ota_ops.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"

static const char *TAG = "ota_ops";

/* Give lwip time to push the final ACK segment out before resetting. */
#define OTA_ACK_FLUSH_MS    150

void ota_enter_recovery(void){
    const esp_partition_t *factory = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
    vTaskDelay(pdMS_TO_TICKS(OTA_ACK_FLUSH_MS));
    if(factory != NULL){
        esp_err_t err = esp_ota_set_boot_partition(factory);
        if(err != ESP_OK){
            ESP_LOGE(TAG, "set_boot_partition(factory): %s", esp_err_to_name(err));
        }
    }else{
        ESP_LOGE(TAG, "no factory partition found");
    }
    esp_restart();
}

void ota_reboot(void){
    vTaskDelay(pdMS_TO_TICKS(OTA_ACK_FLUSH_MS));
    esp_restart();
}

void ota_mark_valid(void){
#ifndef RECOVERY_BUILD
    /* No-op unless the running image is in ESP_OTA_IMG_PENDING_VERIFY. */
    esp_ota_mark_app_valid_cancel_rollback();
#endif
}
