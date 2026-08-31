# esp32-aes-gw — AES-ESP-DO32-HID linecard firmware

Firmware for an avionics linecard based on the **Waveshare ESP32-S3-ETH**
module (W5500 Ethernet over SPI). It speaks the aes-gw2 wire protocol
(SSDP discovery, TCP :5000 control, UDP :10737 stream) and advertises
itself as **`AES-ESP-DO32-HID`** (recovery/bootloader mode: **`BL-AES-ESP-DO32-HID`**,
`fw_type = 1`). The 32-output / 1-input discrete block is backed by an Ebyte
M31-U over Modbus RTU. The ARINC layer remains a protocol stub: commands
validate, keep state and ACK like the STM32 sibling (`stm32/arinc4i4o`), but
no ARINC pins are driven and no RX labels are produced yet.

The card's USB-C port is a **USB HID joystick** (8 × 16-bit axes + 32
buttons, TinyUSB) in the application build: the gateway drives axis/button
values over the wire protocol (`HID_SETUP 0x33` / `HID_SET 0x34` /
`HID_STATE 0x35`, wire doc §12) and a Windows PC on the USB port sees a
standard DirectInput joystick. Consequence: the USB-Serial-JTAG console on
that port only exists in the **recovery** build — the application claims
the OTG PHY at boot. Runtime logs go to the gateway over Ethernet; UART0
(TX GPIO43, RX GPIO44) is reserved for the external RS-485 bus. Firmware
updates are OTA over Ethernet; ROM download mode (hold BOOT) remains the
bench fallback.

Authoritative protocol spec: `aes-gw2/docs/WIRE_PROTOCOL.md`.

## UART / RS-485 M31-U discrete I/O

The application owns UART0 on TX GPIO43 / RX GPIO44 and talks to an Ebyte
M31-U using its factory Modbus RTU settings: unit address 1 at 9600 baud,
8N1. Any stack exposing at least one contiguous DO coil is considered online.
Up to 32 detected DOs map to gateway `dout.0` through `dout.31`; the first DI,
when present, maps to `din.0`.

`main/m31_modbus.c` runs all blocking UART work in a private task. It polls
relay readback with FC01 and input state with FC02, and applies changed relay
bits with FC05. `DISCRETE_STATE.relay_state` is therefore physical FC01
readback, not an optimistic echo of a gateway command. Three failed polling
cycles take the discrete link down; input validity is cleared and commands
are dropped until polling recovers, matching `aes-gw2`'s pending/retry model.

Outputs are fail-safe off at boot, on `DISCRETE_SETUP(enable=false)`, and on
TCP session loss. The worker keeps retrying all-off independently of Ethernet.
Until channel counts are added to the wire protocol, commands for outputs
beyond the detected range remain unconfirmed at the gateway.
`M31_DE_GPIO` defaults to `-1` for the board's auto-direction transceiver; set
it to the DE/RE GPIO if a manually-directed transceiver is fitted.

## Layout

```
components/lccore/   protocol core: framing/dispatch (comm_core), identity
                     (from the eFuse MAC), SSDP builders, log ring,
                     Fletcher32, FW_UPDATE state machine, hardware seams.
                     Plain C11, no ESP-IDF includes (lc_port.h seam) — the
                     same sources compile on the host for unit tests.
main/                ESP-IDF app: W5500 + esp_netif/DHCP, SSDP + UPnP
                     description.xml tasks, TCP/UDP transport, OTA plumbing,
                     M31-U Modbus RTU worker, recovery-build esp_ota+mbedtls
                     FW_UPDATE port.
host_tests/          plain cmake+ctest unit tests (no ESP-IDF needed).
tools/fwpack.py      pack an app .bin into the aes-gw2 OTA image container.
partitions.csv       8 MB: nvs | otadata | phy | factory (recovery, 1 MB)
                     | ota_0 (main app, ~6.9 MB).
cmake/gen_version.cmake  VERSION_STRING/VERSION_COMMIT from git state:
                     bare tag on a clean tagged HEAD (release), else
                     v<next-patch>-dev.<count>.g<sha>[.dirty] so the
                     gateway's fwrelease ordering places dev builds ABOVE
                     the tag they grew from and BELOW the next release
                     (format pinned by host_tests' version_format case).
```

## Build (ESP-IDF v5.5)

```sh
. ~/esp/esp-idf/export.sh
idf.py set-target esp32s3

# Application build (runs from ota_0)
idf.py build

# Recovery build (factory partition, advertises BL-AES-ESP-DO32-HID, serves FW_UPDATE)
idf.py -B build-recovery -DSDKCONFIG=sdkconfig.recovery \
       -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.recovery" \
       -DRECOVERY_BUILD=1 build
```

### Regenerate an application binary from an exact Git tag

The application version is derived from Git when CMake configures the build.
To produce a binary whose embedded version is exactly the release tag, `HEAD`
must point at that tag and there must be no staged or unstaged changes to
tracked files. Finish and commit all release changes before tagging:

```sh
release_tag=v0.2.2a0

# Create a new release tag. If this local tag already exists but has never
# been pushed or released, retarget it with: git tag -f "$release_tag" HEAD
git tag "$release_tag" HEAD

git describe --tags --abbrev=0 --exact-match HEAD  # must print $release_tag
git status --short --untracked-files=no            # must print nothing
```

Do not move a tag that has already been pushed or published; create a new
version tag instead. Force CMake to regenerate the version metadata, then
build the application:

```sh
. ~/esp/esp-idf/export.sh
idf.py reconfigure build

# Verify the metadata embedded in the generated application image.
python -m esptool image_info --version 2 build/esp32-aes-gw.bin \
    | grep -E 'Project name|App version'
```

The check must report `App version: v0.2.2a0` (or the value assigned to
`release_tag`). The regenerated application image is
`build/esp32-aes-gw.bin`. Use `reconfigure build`, rather than only `build`,
after creating or moving a tag so a cached CMake configuration cannot retain
the previous version.

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
Fletcher32-verified) → `REBOOT`.

The recovery build negotiates the STEP chunk size: a successful `FW_UPDATE`
START is ACKed with 2 extra bytes (u16 LE = `FWUP_CHUNK_SIZE`, 240)
advertising the max ciphertext bytes per STEP, cutting the lockstep round
trips ~7.5× vs. the STM32 bootloader's fixed 32. Legacy hosts that ignore
the extra (and keep sending 32-byte STEPs) still work — any multiple of 16
up to 240 is accepted — and STM32 cards, whose START ACK carries no extra,
keep getting 32-byte STEPs from an updated gateway.

`REBOOT` in recovery mode mirrors the STM32 bootloader: it first tries to
point the boot selector back at ota_0 — so a bare `JUMP_TO_BOOTLOADER` →
`REBOOT` round-trips back into the application — unless an upload already
set the boot partition this session. `esp_ota_set_boot_partition()`
validates the ota_0 image first, so with an invalid/empty ota_0 the
selector stays on the factory recovery app (fail-safe preserved); a
failed/aborted flash likewise leaves the card coming back in bootloader
mode. `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` is on: a freshly flashed app
marks itself valid ~10 s after boot (once it has proven it does not
crash-loop — deliberately not network-gated, so a power cycle on a
networkless bench cannot roll a good app back), and a crash-looping image
auto-reverts to recovery.

Release binaries are packed with `tools/fwpack.py`
(`[size|fletcher32|iv|ciphertext]`, same container as
`aes-gw2/fwupdate/pack.go`) and published as release assets on
**`fsedano/sim-lc-esp32-aes-gw`** (the product's `FwRepo`). Note
`aes-gw2/fwrelease` caps downloaded release assets at 1 MiB — keep the
packed app image under that (or raise the cap) when wiring up managed OTA.

The AES-128 image key is the **shared test key**
(`components/lccore/include/aes_key.h`, copied from
`arinc4i4o/bootloader/src/aes/aes_key.h`) so images packed by
`aes-gw2/fwupdate/pack.go` / the arinc4i4o `fwpack` flow work unchanged.
Replace it for production.

## Release (encrypted OTA image)

The gateway flashes cards over the wire from a packed, encrypted image
published as a GitHub release asset on the product's `FwRepo`
(**`fsedano/sim-lc-esp32-aes-gw`**). `tools/fwpack.py` produces that image:
it 0xFF-pads the app binary to a 16-byte boundary, computes the Fletcher32
over the padded plaintext, AES-128-CBC encrypts it with a random IV under
the key in `components/lccore/include/aes_key.h`, and emits the container
`[ firm_size u32 LE | fletcher32 u32 LE | aes_iv 16 | ciphertext ]` — the
same format `aes-gw2/fwupdate/pack.go` reads.

Prerequisite (one-time): `pip install cryptography`.

For the usual build/pack/publish flow, use the helper script. It infers the
release tag from the firmware version, verifies the encrypted container, and
publishes it to the configured `FwRepo`:

```sh
# Clean exact-tag release
tools/release.sh

# Temporary bench release containing tracked working-tree changes. Dirty
# builds are published as GitHub prereleases; an explicit tag is optional.
tools/release.sh --allow-dirty
tools/release.sh --allow-dirty v0.2.3-test.1

# Exercise everything except GitHub publication.
tools/release.sh --allow-dirty --dry-run
```

The selected release tag is embedded in both the AES `GET_FW_INFO` response
and the ESP-IDF application descriptor, so the version shown by aes-gw2
matches the release. Tags are limited to 24 characters by the wire field;
long Git-derived dirty versions are shortened automatically. If repeated
dirty builds share the same commit, pass a fresh explicit tag. Environment
variables `FW_RELEASE_REPO`, `FW_RELEASE_TARGET`, `FW_RELEASE_BUILD_DIR`, and
`IDF_EXPORT` override the defaults; release builds use the isolated
`build-release/` directory. The equivalent manual procedure follows.

```sh
# 1. Tag the release commit. Use an exact, clean tag: gen_version.cmake then
#    stamps a bare "vX.Y.Z" — a dev/dirty suffix (v0.1.1-dev.N.g<sha>) is
#    ordered BELOW the release by the gateway's MinFwVersion gate and parks
#    the card. Verify `git describe` shows exactly the tag before building.
git tag v0.1.2
git describe --tags        # must print "v0.1.2", not "v0.1.2-1-g…"

# 2. Build the application image (this is what runs from ota_0 and is what
#    gets shipped — NOT the recovery build). Reconfigure so a cached build
#    cannot retain version metadata from before the tag was created.
source ~/esp/esp-idf/export.sh
idf.py reconfigure build
python -m esptool image_info --version 2 build/esp32-aes-gw.bin \
    | grep -E 'Project name|App version'

# 3. Pack the encrypted OTA image.
python tools/fwpack.py build/esp32-aes-gw.bin esp32-fw-Release-v0.1.2.bin

# 4. (optional) Verify the container locally before publishing: header size
#    must equal the ciphertext length, and it must round-trip through the
#    gateway's parser (aes-gw2/fwupdate.LoadImage) if you have that checkout.
python - <<'PY'
import struct
b = open('esp32-fw-Release-v0.1.2.bin','rb').read()
size, f32 = struct.unpack('<II', b[:8])
assert size == len(b) - 24 and size % 16 == 0, "bad container"
print(f'ok: {len(b)} bytes, ciphertext {size}, fletcher32 0x{f32:08x}')
PY

# 5. Publish the release. The asset MUST end in .bin (or .blob) — the
#    gateway's release checker (aes-gw2/fwrelease) ignores anything else.
gh release create v0.1.2 -R fsedano/sim-lc-esp32-aes-gw \
   --target main --title v0.1.2 \
   --notes "AES-ESP-DO32-HID firmware v0.1.2" \
   esp32-fw-Release-v0.1.2.bin
```

The gateway picks it up on its next release poll (or force one from the UI /
`POST /api/firmware/refresh`); the card then shows `fw_status:
upgrade_available` and the upgrade dialog drives the flash. Keep the packed
image under **1 MiB** — `aes-gw2/fwrelease` refuses larger assets (or raise
`maxAssetSize` there).

> Uses the shared **test** AES key. For production, replace the key in
> `components/lccore/include/aes_key.h` (and in whatever packs releases) and
> keep it out of the source tree.

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

The gateway ignores unknown board_ids: `AES-ESP-DO32-HID` is registered in
`aes-gw2/linecard/protocol/hid/products.go` (`RegisterProduct`, product
`AES_ESP_DO32_HID`, `BoardIDs: []string{"AES-ESP-DO32-HID"}`, primary discrete
capability `{Inputs:1, Outputs:32}` plus a HID
`Extra{Inputs:8, Outputs:32}` group — HID convention: Inputs = axes,
Outputs = buttons — `FwRepo: fsedano/sim-lc-esp32-aes-gw`, `MinFwVersion:
v0.2.0`). The pre-HID product `A429_ESP_4D` (board_id `A429-ESP_4D`,
registered in `.../discrete/products.go`) shares the same FwRepo, so
already-deployed cards are offered this firmware and become the HID product
after upgrading. See `aes-gw2/docs/ADDING_LINECARDS.md`.

## Not implemented yet (stubs / deferred)

- ARINC RX label stream (0x0B): no records are sent — RX hardware is a stub.
- ARINC TX pins: timetable/manual state is kept and validated, nothing is
  driven.
- Timetable bulk reads (0x15/0x17): ERROR ACK, same as the STM32 firmware.
- DEVICE_STATUS counters are all-zero (matches the gateway's fakelc).
