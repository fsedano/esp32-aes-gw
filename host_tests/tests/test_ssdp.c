/* SSDP message builders: golden-byte comparison with WIRE_PROTOCOL.md. */

#include "board_id.h"
#include "ssdp_msg.h"
#include "test_util.h"

#include <string.h>

static const ssdp_ident_t ID = {
    .uuid      = "240ac4112233970bf4594777",
    .ip        = "192.168.1.50",
    .board_id  = "AES-ESP-M31-HID",
    .fw_tag    = "v0.1.0",
    .serial    = "240ac4112233",
    .http_port = 80,
    .caps_version = 1,
};

int main(void){
    char buf[2048];

    /* NOTIFY ssdp:alive — CRLF-exact golden string. */
    size_t n = ssdp_build_notify(buf, sizeof(buf), &ID);
    const char *want_notify =
        "NOTIFY * HTTP/1.1\r\n"
        "HOST: 239.255.255.250:1900\r\n"
        "CACHE-CONTROL: max-age=1800\r\n"
        "LOCATION: http://192.168.1.50:80/description.xml\r\n"
        "SERVER: AES-ESP-M31-HID/v0.1.0 UPnP/1.0\r\n"
        "X-AES-CAPS: 1\r\n"
        "USN: uuid:240ac4112233970bf4594777::upnp:rootdevice\r\n"
        "NT: upnp:rootdevice\r\n"
        "NTS: ssdp:alive\r\n\r\n";
    CHECK(n == strlen(want_notify));
    CHECK_MEM(buf, want_notify, n);

    /* M-SEARCH 200 OK. */
    n = ssdp_build_msearch_reply(buf, sizeof(buf), &ID);
    const char *want_reply =
        "HTTP/1.1 200 OK\r\n"
        "CACHE-CONTROL: max-age=1800\r\n"
        "EXT:\r\n"
        "LOCATION: http://192.168.1.50:80/description.xml\r\n"
        "SERVER: AES-ESP-M31-HID/v0.1.0 UPnP/1.0\r\n"
        "X-AES-CAPS: 1\r\n"
        "ST: upnp:rootdevice\r\n"
        "USN: uuid:240ac4112233970bf4594777::upnp:rootdevice\r\n\r\n";
    CHECK(n == strlen(want_reply));
    CHECK_MEM(buf, want_reply, n);

    /* SERVER header contract: "<board_id>/<fw_version> UPnP/1.0", where
       board_id is the token the gateway keys products on. */
    buf[n] = '\0';
    char *server = strstr(buf, "SERVER: ");
    CHECK(server != NULL);
    CHECK(strncmp(server, "SERVER: AES-ESP-M31-HID/", 24) == 0);
    CHECK(strstr(buf, "X-AES-CAPS: 1\r\n") != NULL);
    /* Bootloader-mode contract: recovery build prefixes "BL-". */
#ifdef RECOVERY_BUILD
    CHECK(strncmp(BOARD_INFO_SHORT_ID, "BL-", 3) == 0);
#else
    CHECK(strcmp(BOARD_INFO_SHORT_ID, "AES-ESP-M31-HID") == 0);
#endif

    /* Recovery identity snapshots omit the capability admission hint. */
    ssdp_ident_t no_caps = ID;
    no_caps.caps_version = 0;
    n = ssdp_build_msearch_reply(buf, sizeof(buf), &no_caps);
    CHECK(n > 0);
    buf[n] = '\0';
    CHECK(strstr(buf, "X-AES-CAPS:") == NULL);

    /* The USN carries the required uuid: prefix and a 24-character UUID. */
    char *usn = strstr(buf, "USN: uuid:");
    CHECK(usn != NULL);
    char *usn_end = strstr(usn, "::upnp:rootdevice");
    CHECK(usn_end != NULL && (usn_end - (usn + 10)) == 24);

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
