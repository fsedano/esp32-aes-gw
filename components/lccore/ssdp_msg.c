/**
  ******************************************************************************
  * @file    ssdp_msg.c
  * @brief   SSDP / UPnP message builders (see ssdp_msg.h).
  *
  *          Messages follow the SSDP contract in WIRE_PROTOCOL.md section 2.
  ******************************************************************************
  */

#include "ssdp_msg.h"
#include "board_id.h"

#include <stdio.h>
#include <string.h>

size_t ssdp_build_notify(char *buf, size_t cap, const ssdp_ident_t *id){
    char caps[24] = "";
    if(id->caps_version > 0){
        snprintf(caps, sizeof(caps), "X-AES-CAPS: %u\r\n",
                 (unsigned)id->caps_version);
    }
    int n = snprintf(buf, cap,
        "NOTIFY * HTTP/1.1\r\n"
        "HOST: 239.255.255.250:1900\r\n"
        "CACHE-CONTROL: max-age=1800\r\n"
        "LOCATION: http://%s:%u/description.xml\r\n"
        "SERVER: %s/%s UPnP/1.0\r\n"
        "%s"
        "USN: uuid:%s::upnp:rootdevice\r\n"
        "NT: upnp:rootdevice\r\n"
        "NTS: ssdp:alive\r\n\r\n",
        id->ip, (unsigned)id->http_port, id->board_id, id->fw_tag,
        caps, id->uuid);
    return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}

size_t ssdp_build_msearch_reply(char *buf, size_t cap, const ssdp_ident_t *id){
    char caps[24] = "";
    if(id->caps_version > 0){
        snprintf(caps, sizeof(caps), "X-AES-CAPS: %u\r\n",
                 (unsigned)id->caps_version);
    }
    int n = snprintf(buf, cap,
        "HTTP/1.1 200 OK\r\n"
        "CACHE-CONTROL: max-age=1800\r\n"
        "EXT:\r\n"
        "LOCATION: http://%s:%u/description.xml\r\n"
        "SERVER: %s/%s UPnP/1.0\r\n"
        "%s"
        "ST: upnp:rootdevice\r\n"
        "USN: uuid:%s::upnp:rootdevice\r\n\r\n",
        id->ip, (unsigned)id->http_port, id->board_id, id->fw_tag,
        caps, id->uuid);
    return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}

size_t ssdp_build_description(char *buf, size_t cap, const ssdp_ident_t *id){
    int n = snprintf(buf, cap,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/xml\r\n"
        "Connection: close\r\n"
        "X-XSS-Protection: 1; mode=block\r\n"
        "X-Frame-Options: SAMEORIGIN\r\n"
        "X-Content-Type-Options: nosniff\r\n"
        "Content-Security-Policy: default-src 'self'\r\n"
        "Cache-Control: no-store\r\n"
        "Referrer-Policy: no-referrer\r\n\r\n"
        "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>"
        "<root xmlns=\"urn:schemas-upnp-org:device-1-0\">"
        "<specVersion><major>1</major><minor>0</minor></specVersion>"
        "<URLBase>http://%s:%u/</URLBase>"
        "<device><deviceType>urn:schemas-upnp-org:device:Basic:1</deviceType>"
        "<friendlyName>" BOARD_FRIENDLY_NAME "</friendlyName>"
        "<manufacturer>AES (Lucas Angarola)</manufacturer>"
        "<modelName>" BOARD_MODEL_NAME "</modelName>"
        "<modelNumber>" BOARD_INFO_MODEL "</modelNumber>"
        "<serialNumber>%s</serialNumber>"
        "<presentationURL>index.html</presentationURL>"
        "<UDN>%s</UDN></device></root>\r\n",
        id->ip, (unsigned)id->http_port, id->serial, id->uuid);
    return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}

int ssdp_is_msearch(const uint8_t *data, size_t len){
    static const char ms[] = "M-SEARCH";
    return len >= (sizeof(ms) - 1) && memcmp(data, ms, sizeof(ms) - 1) == 0;
}
