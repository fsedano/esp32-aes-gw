/**
  ******************************************************************************
  * @file    aes_key.h
  * @brief   AES-128 firmware-image key.
  *
  *          !!! TEST KEY — NOT FOR PRODUCTION !!!
  *
  *          Copied from the STM32 sibling's bootloader
  *          (arinc4i4o/bootloader/src/aes/aes_key.h) so the gateway's image
  *          packer (aes-gw2/fwupdate/pack.go and the fwpack tool) produces
  *          images this card can decrypt without changes. Replace it (here
  *          and in the packer) for production devices.
  ******************************************************************************
  */

#ifndef LCCORE_AES_KEY_H
#define LCCORE_AES_KEY_H

#include <stdint.h>

/* AES-128 testing key. Not for production! */
static const uint8_t boot_aes_key[16] = {
    0xfa, 0x72, 0xe9, 0x3e, 0x21, 0x55, 0x39, 0xb2,
    0x51, 0xe1, 0x08, 0x5d, 0xb1, 0x55, 0x3a, 0x22
};

#endif /* LCCORE_AES_KEY_H */
