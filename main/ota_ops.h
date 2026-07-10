/**
  ******************************************************************************
  * @file    ota_ops.h
  * @brief   Boot-partition management: bootloader-mode entry (factory
  *          recovery app), reboot, rollback self-validation.
  ******************************************************************************
  */

#ifndef MAIN_OTA_OPS_H
#define MAIN_OTA_OPS_H

/* JUMP_TO_BOOTLOADER accepted: flush the ACK, point the boot selector at the
   factory (recovery) partition and reset. Never returns. */
void ota_enter_recovery(void);

/* REBOOT accepted: flush the ACK and reset. In the recovery build, first try
   to point the boot selector back at ota_0 (mirrors the STM32, where a bare
   JUMP->REBOOT round-trips back into the application): the switch validates
   the ota_0 image and is skipped when an upload already set the boot
   partition this session; on an invalid/empty ota_0 the selector stays on
   factory (fail-safe preserved). Never returns. */
void ota_reboot(void);

/* Application build: arm a one-shot timer that marks the running image valid
   (cancels a pending OTA rollback) after the app has proven it does not
   crash-loop for a few seconds. Deliberately independent of the network so a
   power cycle on a networkless bench cannot roll a good app back to
   recovery. No-op in the recovery build. */
void ota_rollback_timer_start(void);

#ifdef RECOVERY_BUILD
/* Recovery build: FW_UPDATE switched the boot partition this session, so
   ota_reboot() must not override it. */
void ota_note_boot_set(void);
#endif

#endif /* MAIN_OTA_OPS_H */
