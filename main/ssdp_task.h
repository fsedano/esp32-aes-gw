/**
  ******************************************************************************
  * @file    ssdp_task.h
  * @brief   SSDP discovery: multicast NOTIFY beacon + M-SEARCH responder,
  *          plus the UPnP description.xml endpoint on HTTP :80.
  ******************************************************************************
  */

#ifndef MAIN_SSDP_TASK_H
#define MAIN_SSDP_TASK_H

/* Spawn the SSDP task (UDP 239.255.255.250:1900) and the description.xml
   HTTP server task (:80). identity_init() must have run. */
void ssdp_start(void);

#endif /* MAIN_SSDP_TASK_H */
