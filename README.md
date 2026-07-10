# esp32-aes-gw — A429-ESP_4D linecard firmware

Firmware for an avionics linecard based on the **Waveshare ESP32-S3-ETH**
module (W5500 Ethernet over SPI). It speaks the aes-gw2 wire protocol
(SSDP discovery, TCP :5000 control, UDP :10737 stream) and advertises
itself as **`A429-ESP_4D`** (recovery/bootloader mode: **`BL-A429-ESP_4D`**,
`fw_type = 1`). The ARINC and discrete pin-driving layers are stubs: all
commands validate, keep state and ACK exactly like the STM32 sibling
(`stm32/arinc4i4o`), but no pins are driven and no RX labels are produced
yet.

Authoritative protocol spec: `aes-gw2/docs/WIRE_PROTOCOL.md`.

## Layout

```
components/lccore/   protocol core: framing/dispatch (comm_core), identity
                     (from the eFuse MAC), SSDP builders, log ring,
                     Fletcher32, FW_UPDATE state machine, glue stubs.
                     Plain C11, no ESP-IDF includes (lc_port.h seam) — the
                     same sources compile on the host for unit tests.
main/                ESP-IDF app: W5500 + esp_netif/DHCP, SSDP + UPnP
                     description.xml tasks, TCP/UDP transport, OTA plumbing,
                     recovery-build esp_ota+mbedtls FW_UPDATE port.
host_tests/          plain cmake+ctest unit tests (no ESP-IDF needed).
partitions.csv       8 MB: nvs | otadata | phy | factory (recovery, 1 MB)
                     | ota_0 (main app, ~6.9 MB).
cmake/gen_version.cmake  VERSION_STRING/VERSION_COMMIT from git describe.
```

## Build (ESP-IDF v5.5)

```sh
. ~/esp/esp-idf/export.sh
idf.py set-target esp32s3

# Application build (runs from ota_0)
idf.py build

# Recovery build (factory partition, advertises BL-A429-ESP_4D, serves FW_UPDATE)
idf.py -B build-recovery -DSDKCONFIG=sdkconfig.recovery \
       -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.recovery" \
       -DRECOVERY_BUILD=1 build
```

## Flash

The build system's default `flash` target writes the app to the *factory*
partition, which is where the **recovery** image lives:

```sh
# bootloader + partition table + recovery app (factory @ 0x20000)
idf.py -B build-recovery -p <PORT> flash

# main application into ota_0 (@ 0x120000)
esptool.py --chip esp32s3 -p <PORT> write_flash 0x120000 build/esp32-aes-gw.bin

# point the boot selector at ota_0 (otherwise the card boots the recovery app)
python $IDF_PATH/components/app_update/otatool.py -p <PORT> switch_ota_partition --name ota_0

idf.py -p <PORT> monitor
```

After that, updates go over the wire: the gateway's upgrade dialog drives
`JUMP_TO_BOOTLOADER` → `FW_UPDATE` (START/STEP/FINISH, AES-128-CBC,
Fletcher32-verified) → `REBOOT`. A failed/aborted flash leaves the boot
selector on the factory recovery app, so the card always comes back in
bootloader mode. `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` is on: a freshly
flashed app marks itself valid only once its control server is up, so a
crash-looping image auto-reverts to recovery.

The AES-128 image key is the **shared test key**
(`components/lccore/include/aes_key.h`, copied from
`arinc4i4o/bootloader/src/aes/aes_key.h`) so images packed by
`aes-gw2/fwupdate/pack.go` / the arinc4i4o `fwpack` flow work unchanged.
Replace it for production.

## Host tests (no ESP-IDF)

```sh
cmake -S host_tests -B host_tests/build
cmake --build host_tests/build
ctest --test-dir host_tests/build --output-on-failure
```

## Identity

Everything derives deterministically from the eFuse Ethernet MAC
(`esp_read_mac(..., ESP_MAC_ETH)`), mirroring how the STM32 card derives
its identity from the MCU's 96-bit UID:

- **UID (12 bytes)** = MAC ‖ (reversed-MAC XOR salt) — GET_UID raw reply,
  GET_HW_INFO field, and the auth key for JUMP_TO_BOOTLOADER/REBOOT.
  Frozen: app and recovery must agree forever.
- **UUID (24 hex chars)** = hex(UID) — SSDP USN device key.
- **Serial (12 hex chars)** = hex(MAC) — GET_HW_INFO + description.xml.
- **Hostname** = `A429E-` + first 8 serial chars (DHCP option 12).

## Gateway registration

The gateway ignores unknown board_ids: `A429-ESP_4D` must be registered in
`aes-gw2/linecard/protocol/arinc/products.go` (`RegisterProduct`, new
product ID, `BoardIDs: []string{"A429-ESP_4D"}`) before it appears in
discovery. See `aes-gw2/docs/ADDING_LINECARDS.md`. Note
`aes-gw2/fwrelease/github.go` caps release assets at 1 MiB — an ESP32-S3
app image will likely need that raised when OTA management is wired up.

## Not implemented yet (stubs / deferred)

- ARINC RX label stream (0x0B): no records are sent — RX hardware is a stub.
- ARINC TX pins: timetable/manual state is kept and validated, nothing is
  driven.
- Timetable bulk reads (0x15/0x17): ERROR ACK, same as the STM32 firmware.
- DEVICE_STATUS counters are all-zero (matches the gateway's fakelc).
- fwpack step to wrap the ota_0 `.bin` as a `[size|fletcher32|iv|ciphertext]`
  release asset (use `arinc4i4o/tools/fwpack` or `aes-gw2/fwupdate/pack.go`
  with the shared test key meanwhile).
