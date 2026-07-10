/**
  ******************************************************************************
  * @file    net_eth.h
  * @brief   W5500 (SPI) Ethernet bring-up + esp_netif with DHCP client.
  ******************************************************************************
  */

#ifndef MAIN_NET_ETH_H
#define MAIN_NET_ETH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* Install the W5500 driver on the Waveshare ESP32-S3-ETH pins, create the
   default Ethernet netif (DHCP client), set the hostname and start the
   link. `mac` is programmed into the W5500. */
esp_err_t net_eth_start(const uint8_t mac[6], const char *hostname);

/* True once DHCP has bound an address. */
bool net_eth_ready(void);

/* Current IPv4 address in network byte order, 0 while unbound. Lets the
   socket tasks detect address changes and pin multicast membership. */
uint32_t net_eth_get_ip4(void);

/* Current IPv4 as a dotted-quad string ("0.0.0.0" while unbound). */
void net_eth_get_ip_str(char *buf, size_t cap);

#endif /* MAIN_NET_ETH_H */
