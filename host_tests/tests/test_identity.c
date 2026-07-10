/* Identity derivation from a fixed MAC. The expected values are hardcoded
   golden strings: the UID doubles as the JUMP_TO_BOOTLOADER/REBOOT auth key
   and the UUID as the SSDP device key, so any change to the derivation is a
   wire-visible break this test must catch. */

#include "identity.h"
#include "test_util.h"

int main(void){
    const uint8_t mac[6] = { 0x24, 0x0A, 0xC4, 0x11, 0x22, 0x33 };
    identity_init(mac);

    CHECK_MEM(identity_mac(), mac, 6);

    /* UID: MAC verbatim + reversed-MAC XOR salt {A4,29,E5,9D,4D,53}. */
    const uint8_t want_uid[12] = {
        0x24, 0x0A, 0xC4, 0x11, 0x22, 0x33,
        0x97, 0x0B, 0xF4, 0x59, 0x47, 0x77,
    };
    CHECK_MEM(identity_uid(), want_uid, 12);

    /* UUID: 24 lowercase hex chars of the UID (SSDP USN). */
    CHECK_STR(identity_uuid(), "240ac4112233970bf4594777");
    CHECK(strlen(identity_uuid()) == 24);

    /* Serial: 12 hex chars of UID[0..5]. */
    CHECK_STR(identity_serial(), "240ac4112233");

    /* Hostname: prefix + first 8 serial chars (<= 15 chars total). */
    CHECK_STR(identity_hostname(), "A429E-240ac411");
    CHECK(strlen(identity_hostname()) <= 15);

    /* Determinism / stability across re-init. */
    identity_init(mac);
    CHECK_MEM(identity_uid(), want_uid, 12);

    /* A different MAC produces a different identity. */
    const uint8_t mac2[6] = { 0x24, 0x0A, 0xC4, 0x11, 0x22, 0x34 };
    identity_init(mac2);
    CHECK(memcmp(identity_uid(), want_uid, 12) != 0);
    CHECK(strcmp(identity_uuid(), "240ac4112233970bf4594777") != 0);

    return test_report("identity");
}
