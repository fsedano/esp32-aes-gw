/**
  ******************************************************************************
  * @file    net_eth.c
  * @brief   W5500 over SPI (Waveshare ESP32-S3-ETH) + esp_netif DHCP client.
  *
  *          Pin map (board schematic):
  *            SCLK = GPIO13, MISO = GPIO12, MOSI = GPIO11,
  *            CS   = GPIO14, INT  = GPIO10, RST  = GPIO9
  ******************************************************************************
  */

#include "net_eth.h"

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_eth.h"
#include "esp_eth_mac_spi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"

#include "lc_log.h"

static const char *TAG = "net_eth";

#define W5500_SPI_HOST      SPI2_HOST
#define W5500_GPIO_SCLK     13
#define W5500_GPIO_MISO     12
#define W5500_GPIO_MOSI     11
#define W5500_GPIO_CS       14
#define W5500_GPIO_INT      10
#define W5500_GPIO_RST      9
#define W5500_SPI_CLOCK_MHZ 25

static volatile bool s_got_ip;
static esp_netif_t  *s_netif;

static void eth_event_handler(void *arg, esp_event_base_t base,
                              int32_t event_id, void *event_data){
    (void)arg; (void)base; (void)event_data;
    switch(event_id){
        case ETHERNET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "link up");
            break;
        case ETHERNET_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "link down");
            s_got_ip = false;
            break;
        case ETHERNET_EVENT_START:
            ESP_LOGI(TAG, "started");
            break;
        case ETHERNET_EVENT_STOP:
            ESP_LOGI(TAG, "stopped");
            s_got_ip = false;
            break;
        default:
            break;
    }
}

static void ip_event_handler(void *arg, esp_event_base_t base,
                             int32_t event_id, void *event_data){
    (void)arg; (void)base; (void)event_id;
    const ip_event_got_ip_t *ev = (const ip_event_got_ip_t *)event_data;
    ESP_LOGI(TAG, "ip " IPSTR " gw " IPSTR,
             IP2STR(&ev->ip_info.ip), IP2STR(&ev->ip_info.gw));
    LOG_INF("net: ip " IPSTR, IP2STR(&ev->ip_info.ip));
    s_got_ip = true;
}

esp_err_t net_eth_start(const uint8_t mac[6], const char *hostname){
    /* SPI bus for the W5500. */
    spi_bus_config_t buscfg = {
        .mosi_io_num   = W5500_GPIO_MOSI,
        .miso_io_num   = W5500_GPIO_MISO,
        .sclk_io_num   = W5500_GPIO_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(W5500_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));

    /* The W5500 INT line needs the GPIO ISR service. */
    esp_err_t isr = gpio_install_isr_service(0);
    if(isr != ESP_OK && isr != ESP_ERR_INVALID_STATE){
        return isr;
    }

    spi_device_interface_config_t devcfg = {
        .command_bits   = 16,   /* W5500 address phase   */
        .address_bits   = 8,    /* W5500 control phase   */
        .mode           = 0,
        .clock_speed_hz = W5500_SPI_CLOCK_MHZ * 1000 * 1000,
        .input_delay_ns = 80,   /* MISO round-trip; needed >= 20 MHz (IDF
                                   W5500 examples) */
        .spics_io_num   = W5500_GPIO_CS,
        .queue_size     = 20,
    };

    eth_w5500_config_t w5500_config = ETH_W5500_DEFAULT_CONFIG(W5500_SPI_HOST, &devcfg);
    w5500_config.int_gpio_num = W5500_GPIO_INT;

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr       = -1;
    phy_config.reset_gpio_num = W5500_GPIO_RST;

    esp_eth_mac_t *eth_mac = esp_eth_mac_new_w5500(&w5500_config, &mac_config);
    esp_eth_phy_t *eth_phy = esp_eth_phy_new_w5500(&phy_config);

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(eth_mac, eth_phy);
    esp_eth_handle_t eth_handle = NULL;
    ESP_ERROR_CHECK(esp_eth_driver_install(&eth_config, &eth_handle));

    /* The W5500 has no MAC of its own: program the eFuse-derived one. */
    ESP_ERROR_CHECK(esp_eth_ioctl(eth_handle, ETH_CMD_S_MAC_ADDR, (void *)mac));

    /* Default Ethernet netif with DHCP client. */
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    s_netif = esp_netif_new(&netif_cfg);
    ESP_ERROR_CHECK(esp_netif_attach(s_netif, esp_eth_new_netif_glue(eth_handle)));
    ESP_ERROR_CHECK(esp_netif_set_hostname(s_netif, hostname));

    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID,
                                               &eth_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP,
                                               &ip_event_handler, NULL));

    ESP_ERROR_CHECK(esp_eth_start(eth_handle));
    ESP_LOGI(TAG, "W5500 started, mac %02x:%02x:%02x:%02x:%02x:%02x hostname %s",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], hostname);
    return ESP_OK;
}

bool net_eth_ready(void){
    return s_got_ip;
}

uint32_t net_eth_get_ip4(void){
    esp_netif_ip_info_t info;
    if(s_netif != NULL && esp_netif_get_ip_info(s_netif, &info) == ESP_OK){
        return info.ip.addr;        /* already network byte order */
    }
    return 0;
}

void net_eth_get_ip_str(char *buf, size_t cap){
    esp_netif_ip_info_t info;
    if(s_netif != NULL && esp_netif_get_ip_info(s_netif, &info) == ESP_OK){
        snprintf(buf, cap, IPSTR, IP2STR(&info.ip));
    }else{
        snprintf(buf, cap, "0.0.0.0");
    }
}
