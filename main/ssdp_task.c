/**
  ******************************************************************************
  * @file    ssdp_task.c
  * @brief   SSDP over lwip BSD sockets.
  *
  *          Behaviour (wire doc §2 + arinc4i4o ssdp.c):
  *            - join 239.255.255.250:1900, TTL 2
  *            - NOTIFY ssdp:alive burst (x3) once the netif has an address,
  *              then one NOTIFY every ~30 s (hosts also use SSDP last-seen
  *              as a liveness signal)
  *            - answer every M-SEARCH with a unicast 200 OK (the STM32
  *              firmware answers regardless of the requested ST)
  *            - description.xml served over a minimal TCP :80 responder
  ******************************************************************************
  */

#include "ssdp_task.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lwip/sockets.h"
#include "lwip/inet.h"

#include "esp_log.h"

#include "board_id.h"
#include "identity.h"
#include "net_eth.h"
#include "ssdp_msg.h"

static const char *TAG = "ssdp";

#define SSDP_GROUP          "239.255.255.250"
#define SSDP_PORT           1900
#define SSDP_HTTP_PORT      80
#define SSDP_NOTIFY_MS      30000
#define SSDP_RX_BUF_SIZE    512
#define SSDP_TX_BUF_SIZE    1400

static char s_ip[16];

static void fill_ident(ssdp_ident_t *id){
    net_eth_get_ip_str(s_ip, sizeof(s_ip));
    id->uuid      = identity_uuid();
    id->ip        = s_ip;
    id->board_id  = BOARD_INFO_SHORT_ID;
    id->fw_tag    = BOARD_INFO_FW_TAG;
    id->serial    = identity_serial();
    id->http_port = SSDP_HTTP_PORT;
}

static int ssdp_open_socket(void){
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if(sock < 0){
        return -1;
    }
    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(SSDP_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if(bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0){
        close(sock);
        return -1;
    }

    struct ip_mreq imr = {0};
    imr.imr_multiaddr.s_addr = inet_addr(SSDP_GROUP);
    imr.imr_interface.s_addr = htonl(INADDR_ANY);
    if(setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &imr, sizeof(imr)) < 0){
        ESP_LOGW(TAG, "IP_ADD_MEMBERSHIP failed");
    }

    uint8_t ttl = 2;
    setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    return sock;
}

static void ssdp_send_notify(int sock, char *txbuf){
    ssdp_ident_t id;
    fill_ident(&id);
    size_t n = ssdp_build_notify(txbuf, SSDP_TX_BUF_SIZE, &id);
    if(n == 0){
        return;
    }
    struct sockaddr_in dst = {0};
    dst.sin_family      = AF_INET;
    dst.sin_port        = htons(SSDP_PORT);
    dst.sin_addr.s_addr = inet_addr(SSDP_GROUP);
    sendto(sock, txbuf, n, 0, (struct sockaddr *)&dst, sizeof(dst));
}

static void ssdp_task(void *arg){
    (void)arg;
    static char txbuf[SSDP_TX_BUF_SIZE];
    static uint8_t rxbuf[SSDP_RX_BUF_SIZE];

    for(;;){
        /* Wait for an address before advertising. */
        while(!net_eth_ready()){
            vTaskDelay(pdMS_TO_TICKS(200));
        }

        int sock = ssdp_open_socket();
        if(sock < 0){
            ESP_LOGE(TAG, "socket failed, retrying");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        /* Startup burst: three alive NOTIFYs back to back (§2.5). */
        for(int i = 0; i < 3; i++){
            ssdp_send_notify(sock, txbuf);
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        ESP_LOGI(TAG, "advertising %s/%s uuid %s",
                 BOARD_INFO_SHORT_ID, BOARD_INFO_FW_TAG, identity_uuid());

        TickType_t last_notify = xTaskGetTickCount();

        while(net_eth_ready()){
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(sock, &rfds);
            struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
            int r = select(sock + 1, &rfds, NULL, NULL, &tv);

            if(r > 0 && FD_ISSET(sock, &rfds)){
                struct sockaddr_in src;
                socklen_t slen = sizeof(src);
                int n = recvfrom(sock, rxbuf, sizeof(rxbuf) - 1, 0,
                                 (struct sockaddr *)&src, &slen);
                if(n > 0 && ssdp_is_msearch(rxbuf, (size_t)n)){
                    ssdp_ident_t id;
                    fill_ident(&id);
                    size_t len = ssdp_build_msearch_reply(txbuf, sizeof(txbuf), &id);
                    if(len > 0){
                        sendto(sock, txbuf, len, 0,
                               (struct sockaddr *)&src, sizeof(src));
                    }
                }
            }

            if((xTaskGetTickCount() - last_notify) >= pdMS_TO_TICKS(SSDP_NOTIFY_MS)){
                ssdp_send_notify(sock, txbuf);
                last_notify = xTaskGetTickCount();
            }
        }

        close(sock);        /* address lost: rebind once DHCP is back */
    }
}

/* ---- description.xml responder (TCP :80) -------------------------------- */

static void http_desc_task(void *arg){
    (void)arg;
    static char txbuf[SSDP_TX_BUF_SIZE];
    static char rxbuf[SSDP_RX_BUF_SIZE];

    for(;;){
        while(!net_eth_ready()){
            vTaskDelay(pdMS_TO_TICKS(200));
        }

        int lsock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
        if(lsock < 0){
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        int yes = 1;
        setsockopt(lsock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        struct sockaddr_in addr = {0};
        addr.sin_family      = AF_INET;
        addr.sin_port        = htons(SSDP_HTTP_PORT);
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        if(bind(lsock, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
           listen(lsock, 2) < 0){
            close(lsock);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        for(;;){
            int c = accept(lsock, NULL, NULL);
            if(c < 0){
                break;
            }
            struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
            setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            /* Drain the request line(s); any GET gets the description. */
            int n = recv(c, rxbuf, sizeof(rxbuf) - 1, 0);
            if(n > 0){
                ssdp_ident_t id;
                fill_ident(&id);
                size_t len = ssdp_build_description(txbuf, sizeof(txbuf), &id);
                size_t off = 0;
                while(off < len){
                    int w = send(c, txbuf + off, len - off, 0);
                    if(w <= 0){
                        break;
                    }
                    off += (size_t)w;
                }
            }
            shutdown(c, SHUT_RDWR);
            close(c);
        }
        close(lsock);
    }
}

void ssdp_start(void){
    xTaskCreate(ssdp_task, "ssdp", 4096, NULL, 4, NULL);
    xTaskCreate(http_desc_task, "http_desc", 4096, NULL, 3, NULL);
}
