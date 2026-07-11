/**
  ******************************************************************************
  * @file    fwup_port.c
  * @brief   Recovery-build port of the FW_UPDATE state machine's flash/AES
  *          primitives: esp_ota (writes go to ota_0) + mbedtls AES-128-CBC
  *          with the shared image key.
  ******************************************************************************
  */

#ifdef RECOVERY_BUILD

#include "fwup_port.h"

#include <string.h>

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"

#include "mbedtls/aes.h"

#include "aes_key.h"
#include "fwupdate_core.h"
#include "ota_ops.h"

static const char *TAG = "fwup_port";

static const esp_partition_t *s_part;
static esp_ota_handle_t       s_handle;
static bool                   s_open;

static mbedtls_aes_context    s_aes;
static uint8_t                s_iv[16];

/* Abort an in-flight upload and release the esp_ota handle. Idempotent;
   called by the core on every session reset and by flash_begin when a
   previous upload was abandoned. */
static void fwp_flash_abort(void){
    if(s_open){
        esp_ota_abort(s_handle);
        s_open = false;
        ESP_LOGW(TAG, "upload aborted, ota handle released");
    }
}

static int fwp_flash_begin(uint32_t image_size){
    fwp_flash_abort();              /* abandoned previous upload */
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                      ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    if(s_part == NULL){
        ESP_LOGE(TAG, "ota_0 partition not found");
        return -1;
    }
    if(image_size > s_part->size){
        ESP_LOGE(TAG, "image (%u) larger than ota_0 (%u)",
                 (unsigned)image_size, (unsigned)s_part->size);
        return -1;
    }
    /* OTA_WITH_SEQUENTIAL_WRITES: erase sector-by-sector as the (strictly
       sequential) writes land, instead of erasing the whole range here —
       a multi-MB synchronous erase would blow the gateway's 5 s ACK
       timeout on START. esp_ota_end()'s image validation is erase-mode
       independent (it verifies over wrote_size). */
    esp_err_t err = esp_ota_begin(s_part, OTA_WITH_SEQUENTIAL_WRITES, &s_handle);
    if(err != ESP_OK){
        ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
        return -1;
    }
    s_open = true;
    return 0;
}

static int fwp_flash_write(const uint8_t *data, uint32_t len){
    if(!s_open){
        return -1;
    }
    esp_err_t err = esp_ota_write(s_handle, data, len);
    if(err != ESP_OK){
        ESP_LOGE(TAG, "esp_ota_write: %s", esp_err_to_name(err));
        return -1;
    }
    return 0;
}

static int fwp_flash_end(void){
    if(!s_open){
        return -1;
    }
    s_open = false;
    /* Validates the app image (magic, segments, appended SHA256). The
       trailing 0xFF padding the packer added is ignored by the image
       parser, which uses the image's own length fields. */
    esp_err_t err = esp_ota_end(s_handle);
    if(err != ESP_OK){
        ESP_LOGE(TAG, "esp_ota_end: %s", esp_err_to_name(err));
        return -1;
    }
    return 0;
}

static int fwp_set_boot(void){
    esp_err_t err = esp_ota_set_boot_partition(s_part);
    if(err != ESP_OK){
        ESP_LOGE(TAG, "set_boot_partition(ota_0): %s", esp_err_to_name(err));
        return -1;
    }
    ota_note_boot_set();    /* REBOOT must not second-guess this upload */
    return 0;
}

static int fwp_aes_start(const uint8_t iv[16]){
    mbedtls_aes_init(&s_aes);
    if(mbedtls_aes_setkey_dec(&s_aes, boot_aes_key, 128) != 0){
        return -1;
    }
    memcpy(s_iv, iv, 16);
    return 0;
}

static int fwp_aes_decrypt(uint8_t *data, uint32_t len){
    /* In-place CBC decrypt; s_iv chains across chunks. */
    return mbedtls_aes_crypt_cbc(&s_aes, MBEDTLS_AES_DECRYPT, len,
                                 s_iv, data, data) == 0 ? 0 : -1;
}

static const fwup_ops_t s_fwup_ops = {
    .flash_begin = fwp_flash_begin,
    .flash_write = fwp_flash_write,
    .flash_end   = fwp_flash_end,
    .flash_abort = fwp_flash_abort,
    .set_boot    = fwp_set_boot,
    .aes_start   = fwp_aes_start,
    .aes_decrypt = fwp_aes_decrypt,
};

void fwup_port_init(void){
    fwup_init(&s_fwup_ops);
}

#endif /* RECOVERY_BUILD */
