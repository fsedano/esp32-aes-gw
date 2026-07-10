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

/* REBOOT accepted: flush the ACK and reset. Never returns. */
void ota_reboot(void);

/* Application build: cancel a pending rollback once the control plane is up
   (safe to call repeatedly / when no OTA is pending). */
void ota_mark_valid(void);

#endif /* MAIN_OTA_OPS_H */
