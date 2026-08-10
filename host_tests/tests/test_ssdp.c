/* SSDP message builders: golden-byte comparison against the format the
   STM32 sibling emits (which both aes-gw2 and aviologic parse). */

#include "board_id.h"
#include "ssdp_msg.h"
#include "test_util.h"

#include <string.h>

static const ssdp_ident_t ID = {
    .uuid      = "240ac4112233970bf4594777",
    .ip        = "192.168.1.50",
    .board_id  = "A429-ESP_4DH",
    .fw_tag    = "v0.1.0",
    .serial    = "240ac4112233",
    .http_port = 80,
};

int main(void){
    char buf[2048];

    /* NOTIFY ssdp:alive — CRLF-exact golden string. */
    size_t n = ssdp_build_notify(buf, sizeof(buf), &ID);
    const char *want_notify =
        "NOTIFY * HTTP/1.1\r\n"
        "Host:239.255.255.250:1900\r\n"
        "Location:http://192.168.1.50:80/description.xml\r\n"
        "Cache-Control:max-age=10\r\n"
        "Server:A429-ESP_4DH/v0.1.0 UPnP/1.0\r\n"
        "USN:240ac4112233970bf4594777::upnp:rootdevice\r\n"
        "NT:upnp:rootdevice\r\n"
        "NTS:ssdp:alive\r\n\r\n";
    CHECK(n == strlen(want_notify));
    CHECK_MEM(buf, want_notify, n);

    /* M-SEARCH 200 OK. */
    n = ssdp_build_msearch_reply(buf, sizeof(buf), &ID);
    const char *want_reply =
        "HTTP/1.1 200 OK\r\n"
        "Host:239.255.255.250:1900\r\n"
        "Location:http://192.168.1.50:80/description.xml\r\n"
        "Cache-Control:max-age=10\r\n"
        "Server:A429-ESP_4DH/v0.1.0 UPnP/1.0\r\n"
        "USN:240ac4112233970bf4594777::upnp:rootdevice\r\n"
        "ST:upnp:rootdevice\r\n\r\n";
    CHECK(n == strlen(want_reply));
    CHECK_MEM(buf, want_reply, n);

    /* SERVER header contract: "<board_id>/<fw_version> UPnP/1.0", where
       board_id is the token the gateway keys products on. */
    buf[n] = '\0';
    char *server = strstr(buf, "Server:");
    CHECK(server != NULL);
    CHECK(strncmp(server, "Server:A429-ESP_4DH/", 20) == 0);
    /* Bootloader-mode contract: recovery build prefixes "BL-". */
#ifdef RECOVERY_BUILD
    CHECK(strncmp(BOARD_INFO_SHORT_ID, "BL-", 3) == 0);
#else
    CHECK(strncmp(BOARD_INFO_SHORT_ID, "A429-ESP_4DH", 13) == 0);
#endif

    /* 24-char UUID in the USN, no "uuid:" prefix (gateway strips it when
       present, aviologic requires exactly 24 chars for the device key). */
    char *usn = strstr(buf, "USN:");
    CHECK(usn != NULL);
    char *usn_end = strstr(usn, "::upnp:rootdevice");
    CHECK(usn_end != NULL && (usn_end - (usn + 4)) == 24);

    /* description.xml carries serial + UDN and a valid URLBase. */
    n = ssdp_build_description(buf, sizeof(buf), &ID);
    CHECK(n > 0);
    buf[n] = '\0';
    CHECK(strstr(buf, "HTTP/1.1 200 OK\r\n") == buf);
    CHECK(strstr(buf, "<URLBase>http://192.168.1.50:80/</URLBase>") != NULL);
    CHECK(strstr(buf, "<serialNumber>240ac4112233</serialNumber>") != NULL);
    CHECK(strstr(buf, "<UDN>240ac4112233970bf4594777</UDN>") != NULL);
    CHECK(strstr(buf, "<friendlyName>" BOARD_FRIENDLY_NAME "</friendlyName>") != NULL);

    /* Builders fail loudly (return 0) instead of truncating. */
    CHECK(ssdp_build_notify(buf, 32, &ID) == 0);
    CHECK(ssdp_build_msearch_reply(buf, 32, &ID) == 0);
    CHECK(ssdp_build_description(buf, 64, &ID) == 0);

    /* M-SEARCH detector. */
    CHECK(ssdp_is_msearch((const uint8_t *)"M-SEARCH * HTTP/1.1\r\n", 21));
    CHECK(!ssdp_is_msearch((const uint8_t *)"NOTIFY * HTTP/1.1\r\n", 19));
    CHECK(!ssdp_is_msearch((const uint8_t *)"M-SEA", 5));
    CHECK(!ssdp_is_msearch((const uint8_t *)"", 0));

    return test_report("ssdp");
}
