/**
  ******************************************************************************
  * @file    comm_task.c
  * @brief   Transport layer for the aes-gw2 wire protocol on lwip BSD
  *          sockets, mirroring arinc4i4o comm.c's socket service:
  *
  *          - TCP :5000, single client; a second connection is closed
  *            immediately. Connect/disconnect drive comm_core_session()
  *            (all channels reset on either edge; log threshold resets to
  *            INFO on disconnect). The accepted socket gets a bounded send
  *            timeout and short TCP keepalives so a half-open peer can
  *            never wedge the single client slot.
  *          - UDP :10737: the host endpoint is latched (and re-seated) only
  *            by a fully validated PING frame (framing + checksum, wire doc
  *            §10.4; the gateway PINGs at startup); dev->host traffic
  *            (LOG_MSG, DISCRETE_STATE, future RX labels) goes there.
  *          - periodic services: DEVICE_STATUS at ~1 Hz on TCP (app build),
  *            log-ring flush, discrete heartbeat, FW_UPDATE watchdog
  *            (recovery build).
  *          - link loss / DHCP re-address: all sockets are torn down, the
  *            session and host endpoint reset, and everything rebinds when
  *            the network returns.
  ******************************************************************************
  */

#include "comm_task.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lwip/sockets.h"
#include "lwip/inet.h"

#include "esp_log.h"

#include "comm_core.h"
#include "discrete_glue.h"
#include "find_me.h"
#include "hid_glue.h"
#include "hid_port.h"
#include "lc_log.h"
#include "lc_port.h"
#include "net_eth.h"
#include "ota_ops.h"
#include "proto.h"

#ifdef RECOVERY_BUILD
#include "fwupdate_core.h"
#endif

static const char *TAG = "comm";

#define COMM_PORT_TCP       5000
#define COMM_PORT_UDP       10737
#define COMM_RX_BUF_SIZE    1024        /* TCP reassembly buffer */
#define COMM_UDP_BUF_SIZE   512
#define COMM_SELECT_MS      20
#define COMM_STATUS_MS      1000        /* DEVICE_STATUS cadence  */
#define COMM_SND_TIMEOUT_S  3           /* blocking send() bound  */
#define COMM_KEEPIDLE_S     5           /* keepalive after idle   */
#define COMM_KEEPINTVL_S    2           /* probe interval         */
#define COMM_KEEPCNT        3           /* probes before drop     */

static int      s_tcp_listen = -1;
static int      s_tcp_client = -1;
static int      s_udp        = -1;

static uint8_t  s_rxbuf[COMM_RX_BUF_SIZE];
static uint16_t s_rxlen;
static uint8_t  s_udpbuf[COMM_UDP_BUF_SIZE];

/* Host UDP endpoint, latched and re-seated only by a fully validated PING
   frame (framing + checksum), so a stray datagram can never steal the
   dev->host stream. */
static struct sockaddr_in s_host_addr;
static bool               s_host_known;

static uint32_t s_last_log_flush;
static uint32_t s_last_status;

/* ====================================================================== */
/* comm_core ops                                                          */
/* ====================================================================== */

static void tcp_client_drop(void){
    if(s_tcp_client >= 0){
        close(s_tcp_client);
        s_tcp_client = -1;
        s_rxlen = 0;
        comm_core_session(false);
    }
}

static void ops_send_tcp(const uint8_t *frame, uint16_t len){
    if(s_tcp_client < 0){
        return;
    }
    uint16_t sent = 0;
    while(sent < len){
        int r = send(s_tcp_client, frame + sent, (size_t)(len - sent), 0);
        if(r <= 0){
            tcp_client_drop();      /* peer gone; loop will re-accept */
            return;
        }
        sent = (uint16_t)(sent + (uint16_t)r);
    }
}

/* CRITICAL: never log from here — this is also the log-flush path. */
static bool ops_send_udp(const uint8_t *frame, uint16_t len){
    if(!s_host_known || s_udp < 0){
        return false;
    }
    int r = sendto(s_udp, frame, len, 0,
                   (struct sockaddr *)&s_host_addr, sizeof(s_host_addr));
    return r == (int)len;
}

#ifndef RECOVERY_BUILD
static void ops_enter_bootloader(void){
    ota_enter_recovery();
}
#endif

static void ops_reboot(void){
    ota_reboot();
}

static void ops_find_me(uint32_t duration_s){
    find_me_start(duration_s);
}

static const comm_ops_t s_ops = {
    .send_tcp         = ops_send_tcp,
    .send_udp         = ops_send_udp,
#ifdef RECOVERY_BUILD
    .enter_bootloader = NULL,       /* already the bootloader: ACK only */
#else
    .enter_bootloader = ops_enter_bootloader,
#endif
    .reboot           = ops_reboot,
    .find_me          = ops_find_me,
};

/* ====================================================================== */
/* Socket service                                                         */
/* ====================================================================== */

static int tcp_listen_open(void){
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if(sock < 0){
        return -1;
    }
    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(COMM_PORT_TCP);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if(bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
       listen(sock, 1) < 0){
        close(sock);
        return -1;
    }
    return sock;
}

static int udp_open(void){
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if(sock < 0){
        return -1;
    }
    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(COMM_PORT_UDP);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if(bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0){
        close(sock);
        return -1;
    }
    return sock;
}

static void tcp_accept_service(void){
    struct sockaddr_in peer;
    socklen_t plen = sizeof(peer);
    int c = accept(s_tcp_listen, (struct sockaddr *)&peer, &plen);
    if(c < 0){
        return;
    }
    if(s_tcp_client >= 0){
        /* Single control client, like the W6100's one hardware socket:
           reject the newcomer. */
        ESP_LOGW(TAG, "second control connection rejected");
        close(c);
        return;
    }
    int yes = 1;
    setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
    /* Bound blocking sends: a stalled peer makes send() fail after
       COMM_SND_TIMEOUT_S and the client is dropped instead of wedging the
       comm task (and, in the recovery build, the single client slot). */
    struct timeval snd_to = { .tv_sec = COMM_SND_TIMEOUT_S, .tv_usec = 0 };
    setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &snd_to, sizeof(snd_to));
    /* Short keepalives: a half-open client (peer power-cycled, cable pulled)
       is detected and reaped even when we have nothing to send — the
       recovery build sends nothing unsolicited, so without this a dead
       client would hold the slot forever. */
    setsockopt(c, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof(yes));
    int ka_idle  = COMM_KEEPIDLE_S;
    int ka_intvl = COMM_KEEPINTVL_S;
    int ka_cnt   = COMM_KEEPCNT;
    setsockopt(c, IPPROTO_TCP, TCP_KEEPIDLE,  &ka_idle,  sizeof(ka_idle));
    setsockopt(c, IPPROTO_TCP, TCP_KEEPINTVL, &ka_intvl, sizeof(ka_intvl));
    setsockopt(c, IPPROTO_TCP, TCP_KEEPCNT,   &ka_cnt,   sizeof(ka_cnt));
    s_tcp_client = c;
    s_rxlen = 0;
    comm_core_session(true);        /* fresh control session */
}

static void tcp_client_service(void){
    uint16_t space = (uint16_t)(sizeof(s_rxbuf) - s_rxlen);
    if(space == 0){
        /* Full buffer with no parsable packet cannot happen (the scanner
           always consumes on >= one whole max-size frame), but stay safe. */
        s_rxlen = 0;
        space = sizeof(s_rxbuf);
    }
    int n = recv(s_tcp_client, s_rxbuf + s_rxlen, space, 0);
    if(n <= 0){
        tcp_client_drop();          /* dropped session -> all disabled */
        return;
    }
    s_rxlen = (uint16_t)(s_rxlen + (uint16_t)n);

    uint16_t off = comm_core_input(s_rxbuf, s_rxlen, COMM_SRC_TCP);
    if(s_tcp_client < 0){
        /* A handler's TCP send failed inside comm_core_input and
           tcp_client_drop() already reset s_rxlen: compacting now would
           compute s_rxlen - off with a stale s_rxlen and wrap. */
        return;
    }
    if(off > 0){
        uint16_t rem = (uint16_t)(s_rxlen - off);
        if(rem){
            memmove(s_rxbuf, s_rxbuf + off, rem);
        }
        s_rxlen = rem;
    }
}

/* True when the datagram starts with a whole, checksum-valid PING frame.
   Only such a frame may latch or re-seat the host UDP endpoint. */
static bool udp_is_valid_ping(const uint8_t *buf, uint16_t len){
    if(len < (uint16_t)(PROTO_HEADER_SIZE + PROTO_CSUM_SIZE)){
        return false;
    }
    if(buf[0] != (uint8_t)(PROTO_HEAD & 0xFF) ||            /* BB */
       buf[1] != (uint8_t)((PROTO_HEAD >> 8) & 0xFF) ||     /* AA */
       buf[4] != CMD_PING){
        return false;
    }
    uint8_t size = buf[3];
    if(len < (uint16_t)(PROTO_HEADER_SIZE + size + PROTO_CSUM_SIZE)){
        return false;
    }
    uint8_t csum = 0;
    for(uint16_t i = PROTO_CSUM_START; i < (uint16_t)(PROTO_HEADER_SIZE + size); i++){
        csum += buf[i];
    }
    return csum == buf[PROTO_HEADER_SIZE + size];
}

static void udp_service(void){
    struct sockaddr_in peer;
    socklen_t plen = sizeof(peer);
    int n = recvfrom(s_udp, s_udpbuf, sizeof(s_udpbuf), 0,
                     (struct sockaddr *)&peer, &plen);
    if(n <= 0){
        return;
    }

    /* Latch / re-seat the host endpoint (wire doc §4.4 + §10.4): only a
       fully validated PING may latch or re-seat, so garbage or spoofed
       datagrams cannot capture the dev->host stream. The gateway sends
       PINGs at startup, so a well-behaved host latches immediately. */
    bool is_ping = udp_is_valid_ping(s_udpbuf, (uint16_t)n);
    bool same_peer = s_host_known
                  && s_host_addr.sin_port == peer.sin_port
                  && s_host_addr.sin_addr.s_addr == peer.sin_addr.s_addr;
    if(is_ping && (!s_host_known || !same_peer)){
        s_host_addr = peer;
        s_host_known = true;
        LOG_INF("comm: UDP host %s:%d",
                inet_ntoa(peer.sin_addr), (int)ntohs(peer.sin_port));
    }

    /* A datagram is one or more framed packets; dispatch on the UDP path
       (fire-and-forget: no ACKs are generated for UDP). */
    comm_core_input(s_udpbuf, (uint16_t)n, COMM_SRC_UDP);
}

/* Drain the log ring to the host as CMD_LOG_MSG datagrams. Batching: flush
   when >= 200 bytes are pending or the oldest pending lines are ~100 ms old;
   at most 2 datagrams per pass. CRITICAL: nothing in this path may log. */
static void log_flush_service(void){
    if(!s_host_known){
        return;
    }
    uint16_t ready = 0;
    if(!lc_log_net_pending(&ready)){
        return;
    }
    uint32_t now = lc_port_tick_ms();
    if(ready < 200 && (now - s_last_log_flush) < 100){
        return;
    }

    uint8_t payload[PROTO_MAX_PAYLOAD];
    for(int i = 0; i < 2; i++){
        uint8_t n = lc_log_net_build_packet(payload, PROTO_MAX_PAYLOAD);
        if(n == 0){
            break;
        }
        if(!comm_core_send_udp(CMD_LOG_MSG, payload, n)){
            /* Send failed: the popped records are lost and show up as a
               seq gap on the host — same as UDP loss. */
            break;
        }
        s_last_log_flush = now;
    }
}

#ifndef RECOVERY_BUILD
/* DEVICE_STATUS (0x0E) at ~1 Hz over TCP while a client is connected. The
   counters are not implemented yet (all zero) — matching the gateway's own
   fakelc simulator, which sends an all-zero block. */
static void status_service(void){
    if(s_tcp_client < 0){
        return;
    }
    uint32_t now = lc_port_tick_ms();
    if((uint32_t)(now - s_last_status) < COMM_STATUS_MS){
        return;
    }
    s_last_status = now;
    proto_device_status_t st;
    memset(&st, 0, sizeof(st));
    comm_core_send_tcp(CMD_DEVICE_STATUS, (const uint8_t *)&st, sizeof(st));
}
#endif

/* ====================================================================== */
/* Task                                                                   */
/* ====================================================================== */

/* Tear down every socket and forget the host endpoint; the outer loop
   rebinds once the network is back. Dropping the client also resets the
   protocol session (channels/discrete/upload state). */
static void comm_sockets_close(void){
    tcp_client_drop();
    if(s_tcp_listen >= 0){
        close(s_tcp_listen);
        s_tcp_listen = -1;
    }
    if(s_udp >= 0){
        close(s_udp);
        s_udp = -1;
    }
    s_host_known = false;
}

static void comm_task(void *arg){
    (void)arg;

    for(;;){
        while(!net_eth_ready()){
#ifndef RECOVERY_BUILD
            /* Keep the USB pump alive with the network down: a pending
               reset-to-center report (e.g. after a session drop) must still
               reach the PC, and a remount must re-push the latched state. */
            hid_port_loop();
#endif
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        uint32_t bound_ip = net_eth_get_ip4();

        s_tcp_listen = tcp_listen_open();
        s_udp        = udp_open();
        if(s_tcp_listen < 0 || s_udp < 0){
            ESP_LOGE(TAG, "socket bind failed, retrying");
            comm_sockets_close();
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        ESP_LOGI(TAG, "control :%d / stream :%d up", COMM_PORT_TCP, COMM_PORT_UDP);
        LOG_INF("comm: control plane up");

        while(net_eth_ready() && net_eth_get_ip4() == bound_ip){
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(s_tcp_listen, &rfds);
            FD_SET(s_udp, &rfds);
            int maxfd = (s_tcp_listen > s_udp) ? s_tcp_listen : s_udp;
            if(s_tcp_client >= 0){
                FD_SET(s_tcp_client, &rfds);
                if(s_tcp_client > maxfd){
                    maxfd = s_tcp_client;
                }
            }

            struct timeval tv = { .tv_sec = 0, .tv_usec = COMM_SELECT_MS * 1000 };
            int r = select(maxfd + 1, &rfds, NULL, NULL, &tv);

            if(r > 0){
                if(FD_ISSET(s_tcp_listen, &rfds)){
                    tcp_accept_service();
                }
                if(s_tcp_client >= 0 && FD_ISSET(s_tcp_client, &rfds)){
                    tcp_client_service();
                }
                if(FD_ISSET(s_udp, &rfds)){
                    udp_service();
                }
            }

            /* Periodic services. */
            log_flush_service();
#ifndef RECOVERY_BUILD
            discrete_glue_loop();
            hid_glue_loop();
            status_service();
#else
            fwup_tick();
#endif
        }

        /* Link lost or the DHCP address changed: tear everything down and
           rebind (stale sockets and a stale host endpoint would silently
           blackhole traffic otherwise). */
        ESP_LOGW(TAG, "network lost/readdressed, rebinding");
        comm_sockets_close();
    }
}

void comm_start(void){
    comm_core_init(&s_ops);
    xTaskCreate(comm_task, "comm", 6144, NULL, 5, NULL);
}
