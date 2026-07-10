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
#include "esp_timer.h"

static const char *TAG = "ota_ops";

/* Give lwip time to push the final ACK segment out before resetting. */
#define OTA_ACK_FLUSH_MS        150
/* Uptime after which the app has proven it does not crash-loop. */
#define OTA_MARK_VALID_MS       10000

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

#ifdef RECOVERY_BUILD
static bool s_boot_set_this_session;

void ota_note_boot_set(void){
    s_boot_set_this_session = true;
}
#endif

void ota_reboot(void){
    vTaskDelay(pdMS_TO_TICKS(OTA_ACK_FLUSH_MS));
#ifdef RECOVERY_BUILD
    /* STM32 semantics: REBOOT from the bootloader returns to the app when
       one is present. esp_ota_set_boot_partition() validates the ota_0
       image first, so an invalid/empty ota_0 keeps the selector on the
       factory recovery app (fail-safe). Skipped when FW_UPDATE already set
       the boot partition this session. */
    if(!s_boot_set_this_session){
        const esp_partition_t *ota0 = esp_partition_find_first(
            ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
        if(ota0 != NULL){
            esp_err_t err = esp_ota_set_boot_partition(ota0);
            if(err == ESP_OK){
                ESP_LOGI(TAG, "reboot -> application (ota_0)");
            }else{
                ESP_LOGW(TAG, "ota_0 not bootable (%s), staying on recovery",
                         esp_err_to_name(err));
            }
        }
    }
#endif
    esp_restart();
}

#ifndef RECOVERY_BUILD
static void mark_valid_cb(void *arg){
    (void)arg;
    /* No-op unless the running image is in ESP_OTA_IMG_PENDING_VERIFY. */
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    ESP_LOGI(TAG, "rollback: app marked valid after %d ms uptime (%s)",
             OTA_MARK_VALID_MS, esp_err_to_name(err));
}
#endif

void ota_rollback_timer_start(void){
#ifndef RECOVERY_BUILD
    static esp_timer_handle_t timer;
    const esp_timer_create_args_t args = {
        .callback = mark_valid_cb,
        .name     = "ota_valid",
    };
    if(timer == NULL && esp_timer_create(&args, &timer) != ESP_OK){
        ESP_LOGE(TAG, "rollback timer create failed, marking valid now");
        esp_ota_mark_app_valid_cancel_rollback();
        return;
    }
    esp_timer_start_once(timer, (uint64_t)OTA_MARK_VALID_MS * 1000);
#endif
}
