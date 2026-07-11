/**
  ******************************************************************************
  * @file    fwupdate_core.h
  * @brief   CMD_FW_UPDATE (0x0F) upload state machine — the recovery build's
  *          counterpart of arinc4i4o's bootloader flash_loader.c, with the
  *          flash/AES primitives behind a port seam (esp_ota + mbedtls on
  *          target, fakes in host tests).
  *
  *          Wire contract (aes-gw2/fwupdate/updater.go):
  *            START : <u32 index==0 | u32 firm_size | u32 fletcher32 | iv[16]>
  *            STEP  : <u32 index==prev+1 | ciphertext (<= FWUP_CHUNK_SIZE,
  *                     %16==0; legacy hosts send 32)>
  *            FINISH: <u32 index==0xFFFFFFFF | u32 firm_size>
  *
  *          The successful START ACK carries 2 extra bytes (u16 LE =
  *          FWUP_CHUNK_SIZE) advertising the max STEP chunk; the STM32
  *          bootloader ACKs with no extra, so hosts fall back to 32.
  *
  *          firm_size / fletcher32 describe the 0xFF-padded PLAINTEXT (which
  *          equals the ciphertext length for CBC). The Fletcher32 is computed
  *          incrementally over the decrypted stream and must match before the
  *          FINISH ACK; only then is the boot partition switched to the new
  *          application.
  *
  *          Returned status codes mirror the STM32 fl_status_t values; the
  *          gateway only distinguishes 0 (OK) from non-zero.
  ******************************************************************************
  */

#ifndef LCCORE_FWUPDATE_CORE_H
#define LCCORE_FWUPDATE_CORE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* Status codes (ACK status_code on the wire). Values track the STM32
   bootloader's fl_status_t where an equivalent exists. */
typedef enum {
    FWUP_OK = 0,
    FWUP_ERR_TIMEOUT                  = 101,
    FWUP_ERR_FW_CHECKSUM              = 103,
    FWUP_ERR_FLASH_BEGIN              = 105,   /* erase/prepare failed        */
    FWUP_ERR_FLASH_PROGRAM            = 106,
    FWUP_ERR_FW_SIZE_NOT_MULT_16      = 107,
    FWUP_ERR_FW_SIZE_OUT_OF_RANGE     = 109,
    FWUP_ERR_PAYLOAD_TOO_LONG         = 110,
    FWUP_ERR_INDEX_NOT_ZERO_WHEN_IDLE = 114,
    FWUP_ERR_PACKET_LOSS              = 115,
    FWUP_ERR_RECV_TOO_MANY_BYTES      = 116,
    FWUP_ERR_FIRM_SIZE_DISAGREE       = 117,
    FWUP_ERR_DATA_NOT_FACTOR_16       = 118,
    FWUP_ERR_SM_WRONG_STATE           = 119,
    FWUP_ERR_IMAGE_INVALID            = 120,   /* esp_ota_end rejected image  */
    FWUP_ERR_SET_BOOT                 = 121,
    FWUP_ERR_AES                      = 122,
} fwup_status_t;

/* Max firmware bytes per STEP: 4-byte index + 240 <= 250-byte wire payload,
   and a multiple of 16 for AES-CBC. Advertised to the host as a u16 LE in
   the START ACK's extra bytes (see comm_core.c CMD_FW_UPDATE); legacy hosts
   that ignore the extra may keep sending 32-byte STEPs (updater.go's
   updChunkSize default) — any multiple of 16 up to this cap is accepted. */
#define FWUP_CHUNK_SIZE          240u
/* Reject absurd images early (recovery + headers must fit ota_0; the port's
   flash_begin does the exact partition-size check). */
#define FWUP_MIN_SIZE            1024u
/* Abort an upload when no packet arrives for this long (flash_loader.c). */
#define FWUP_TIMEOUT_MS          2000u

typedef struct {
    /* Prepare the application partition for image_size bytes (erase).
       Returns 0 on success. */
    int (*flash_begin)(uint32_t image_size);
    /* Program the next len plaintext bytes. Returns 0 on success. */
    int (*flash_write)(const uint8_t *data, uint32_t len);
    /* Finalize + validate the written image. Returns 0 on success. */
    int (*flash_end)(void);
    /* Abort a half-done upload: release any open flash/OTA handle without
       finalizing. Optional (may be NULL); must be idempotent — it is called
       on every session reset, whether or not an upload was in flight. */
    void (*flash_abort)(void);
    /* Point the boot selector at the new application. Returns 0 on success. */
    int (*set_boot)(void);
    /* Start an AES-128-CBC decrypt session with the image IV. 0 on success. */
    int (*aes_start)(const uint8_t iv[16]);
    /* Decrypt len bytes (multiple of 16) in place, chaining across calls. */
    int (*aes_decrypt)(uint8_t *data, uint32_t len);
} fwup_ops_t;

/* One-time init (recovery main). */
void fwup_init(const fwup_ops_t *ops);

/* TCP control session edge (connect or disconnect): abort any half-done
   upload — including releasing the port's flash handle via flash_abort —
   and clear the latched error (mirrors boot_comm.c's
   flash_loader_init-on-connect). */
void fwup_session_reset(void);

/* Feed one CMD_FW_UPDATE payload; returns the wire ACK status. The payload
   is decrypted in place (mutated). */
uint32_t fwup_handle(uint8_t *payload, uint8_t len);

/* Timeout service; call periodically (~10..100 ms) from the comm task. */
void fwup_tick(void);

#ifdef __cplusplus
}
#endif

#endif /* LCCORE_FWUPDATE_CORE_H */
