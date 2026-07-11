/**
  ******************************************************************************
  * @file    fwup_port.h
  * @brief   Recovery build: wire the FW_UPDATE core to esp_ota + mbedtls.
  ******************************************************************************
  */

#ifndef MAIN_FWUP_PORT_H
#define MAIN_FWUP_PORT_H

#ifdef RECOVERY_BUILD
void fwup_port_init(void);
#endif

#endif /* MAIN_FWUP_PORT_H */
