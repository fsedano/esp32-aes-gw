# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What This Repo Is

Firmware for an avionics linecard based on an **ESP32-S3-ETH module**. It is a sibling of the existing STM32 linecard at `/Users/fsedano/code/stm32/arinc4i4o` and must speak the same wire protocol to the AES gateway at `/Users/fsedano/code/aes-gw2`.

**Initial deliverable:** firmware functionally equivalent to the arinc4i4o firmware, but with the actual pin-driving layer stubbed out, and advertising a **different board identifier** so the gateway can distinguish this card type from the STM32 one.

Toolchain: **ESP-IDF v5.5.1** installed at `~/esp/esp-idf` (activate with `source ~/esp/esp-idf/export.sh`). Board: **Waveshare ESP32-S3-ETH** — W5500 Ethernet over SPI: SCLK=GPIO13, MISO=GPIO12, MOSI=GPIO11, CS=GPIO14, INT=GPIO10, RST=GPIO9. USB (native USB-Serial-JTAG) appears as `/dev/cu.usbmodem*`.

Repos: this repo's `origin` is `fsedano/esp32-aes-gw` (private, source). **OTA release binaries go to a separate public repo `fsedano/sim-lc-esp32-aes-gw`** — this follows the existing convention (the STM32 card's FwRepo is `fsedano/sim-lc-A429-8BD`), and it is what the gateway's `FwRepo` points at for this product.

## Reference Repos (read these, don't guess the protocol)

- `/Users/fsedano/code/aes-gw2/docs/WIRE_PROTOCOL.md` — **the authoritative protocol spec** (framing, all commands, payload layouts, session lifecycle, simulator checklist).
- `/Users/fsedano/code/aes-gw2/docs/ADDING_LINECARDS.md` — how to register the new card type in the gateway.
- `/Users/fsedano/code/stm32/arinc4i4o/firmware/src/` — the reference implementation. `comm/comm.c` + `comm/proto.h` (protocol dispatcher), `eth/ssdp/ssdp.c` (discovery), `board_info.c/.h` (identity) are board-agnostic C and can largely carry over.
- `/Users/fsedano/code/stm32/arinc4i4o/firmware/sim/` — a "virtual linecard" host build: real protocol code with pins faked. **This is exactly the pattern for our pin-stub deliverable.**
- `/Users/fsedano/code/stm32/arinc4i4o/firmware/AUDIT.md` — porting surface (§7), concurrency inventory for the ARINC TX ISR, testing strategy (§9).
- `/Users/fsedano/code/aes-gw2/cmd/fakelc/` + `simulator/` — the gateway's own fake linecard; our firmware should be byte-indistinguishable from it on the wire.

## Protocol Summary (details in WIRE_PROTOCOL.md)

Three channels, all multibyte fields little-endian:

- **Discovery:** SSDP over UDP multicast 239.255.255.250:1900. The first token of the SSDP `SERVER` header (`<board_id>/<fw_version> UPnP/1.0 ...`) is what the gateway uses to identify the card type. A `BL-` prefix signals bootloader mode.
- **Control:** TCP port **5000**, single client. Request/ACK command protocol.
- **Stream:** UDP port **10737**. Device→host ARINC RX labels (cmd 0x0B), log batches (0x24), and the manual-TX fast path. The host sends 4× UDP PING at session start so the device learns the host's stream endpoint.

Framing on both TCP and UDP: `head 0xAABB (LE) | packet_id (1) | payload_size (1, max 250) | cmd (1) | payload | checksum (8-bit sum from packet_id through payload)`. ACK envelope: echoed `packet_id` + `status_code (u32 LE)` + extra.

Core commands the card must answer: `GET_FW_INFO 0x06` (the gateway considers a card "ready" once this answers; byte 52 = 0 app / 1 bootloader), `GET_HW_INFO 0x0D`, `GET_UID 0x09` (raw 12-byte reply, no ACK envelope; the UID is the auth key for `JUMP_TO_BOOTLOADER 0xAA` and `REBOOT 0xFB`), `PING 0xFA`, ARINC channel commands `0x10`–`0x17`, discrete `0x30`–`0x32`, `SET_LOG_LEVEL 0x25`, `FIND_ME 0x22`, `FW_UPDATE 0x0F` (bootloader mode only). A dropped TCP control session must reset all channels to disabled.

## Card Identity (the part that MUST differ from arinc4i4o)

This card's board_id is **`A429-ESP_4D`** (the equivalent of `BOARD_INFO_SHORT_ID` in the STM32 firmware, where all identity constants live in `board_info.h`/`board_info.c`). The bootloader-mode variant is `BL-A429-ESP_4D`. Do not reuse the tokens already registered in the gateway: `A429-8`, `A429-8B`, `A429-8BD`, `ARI-10`, `SSD-10`, `SSD3-10`.

The gateway silently ignores unregistered board_ids, so `A429-ESP_4D` must also be registered gateway-side: add a `linecard.RegisterProduct(...)` entry in `/Users/fsedano/code/aes-gw2/linecard/protocol/arinc/products.go` `init()` with a new product ID (e.g. `A429_ESP_4D`), `BoardIDs: []string{"A429-ESP_4D"}`, capabilities, and (later) `FwRepo`/`MinFwVersion` for OTA management. `RegisterProduct` panics on duplicate IDs, so collisions surface at gateway startup.

Identity requirements on the wire: a stable 24-char UUID consistent across all SSDP messages (USN header), and a stable 12-byte UID reported in GET_UID/GET_HW_INFO. The STM32 derives MAC/serial/hostname/UUID from its 96-bit hardware UID; on ESP32-S3 derive equivalents from eFuse MAC.

## Architecture to Preserve

The arinc4i4o firmware has a clean seam the port should keep: `comm.c` never touches hardware — it calls only `arinc_glue.*` and `discrete_glue.*`. For the initial deliverable, implement those glue layers as stubs (accept commands, return `STAT_OK`, optionally decode/log what would have been driven — see `sim/hal_sim.c` for how the simulator does this).

## OTA Design

Requirement: identical UX to the STM32 card — the gateway's upgrade dialog works unchanged, and the card is always recoverable from bootloader mode. The dialog is driven entirely by `aes-gw2/fwupdate/updater.go` `Run()` stage callbacks (connecting → jumping → flashing → rebooting → verifying), so honoring the wire contract gets the identical dialog for free.

**Chosen approach: factory "recovery" app as the bootloader personality.** ESP-IDF partition table: `factory` = a small recovery firmware (SSDP advertising `BL-A429-ESP_4D` + fw_type=1, TCP:5000 with GET_FW_INFO/GET_UID/FW_UPDATE/REBOOT), `ota_0` = the main app. Flow mapping:

- `JUMP_TO_BOOTLOADER(uid)` in the app → `esp_ota_set_boot_partition(factory)` + restart. The gateway polls GET_FW_INFO for up to 30 s (`waitForMode`) expecting fw_type=1.
- `FW_UPDATE` in recovery: START carries the 24-byte image header `[firm_size u32 | fletcher32 u32 | aes_iv 16]`; STEPs carry 32-byte AES-128-CBC ciphertext chunks; decrypt and `esp_ota_write` plaintext to `ota_0`. FINISH → verify Fletcher32 (little-endian 16-bit words, both sums mod 0xFFFF, over the 0xFF-padded plaintext — see `aes-gw2/fwupdate/pack.go`) before ACKing, then set boot partition to `ota_0` only on success.
- `REBOOT(uid)` → restart; gateway polls until fw_type=0 and reads the new version.
- Recoverability: `factory` is never rewritten by OTA, and a failed/aborted flash leaves boot pointing at factory — the card comes back in bootloader mode, re-flashable from the dialog, exactly like the STM32 card. Additionally enable `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`: the app self-marks valid only once it's serving GET_FW_INFO, so a crash-looping new image auto-reverts without user action. ROM USB download mode remains the bench-level last resort.

Rejected alternative: "bootloader as a mode of the main app" (single image, flag in NVS) — a broken app can't reach updater mode, violating the recoverable-from-bootloader requirement.

Constraints discovered in the gateway code:

- `aes-gw2/fwrelease/github.go` caps firmware asset downloads at **1 MiB** (`maxAssetSize`). An ESP32-S3 app with the network stack will likely exceed this — bump that constant when registering the product. Keep the recovery app lean regardless.
- Upload is one TCP round trip per 32-byte chunk (`updChunkSize` in `updater.go`); a ~1 MB image is ~33 k round trips, i.e. minutes, not seconds. The dialog's progress bar makes this acceptable; if it becomes painful, a per-product chunk size (framing allows up to ~224 bytes/chunk) is a later gateway+firmware change.
- The AES-128 key is a shared secret between the image packer and the card (STM32 test key in `arinc4i4o/bootloader/src/aes/aes_key.h`; packer reference in `arinc4i4o/tools/fwpack` and `aes-gw2/fwupdate/pack.go`). This repo needs its own fwpack step that wraps the ESP-IDF app `.bin` in the `[size|fletcher32|iv|ciphertext]` container as a GitHub release asset (`.bin`/`.blob` suffix required by the release checker).
- GET_FW_INFO responses must be ≤ 30 s after reset in both directions of the mode switch, and the FINISH ACK must beat the 5 s `updAckTimeout` — Fletcher32 over ~1.5 MB of flash is comfortably within that.

## Build Commands

None yet. Once the ESP-IDF project is scaffolded, record here: `idf.py set-target esp32s3`, build/flash/monitor commands, and how to run host-side protocol tests (the arinc4i4o pattern of compiling `comm.c`/`ssdp.c` against fake headers on the host — see its `firmware/docs/TESTING.md` — is worth replicating).

Useful for integration testing: the arinc4i4o virtual linecard (`cmake -S firmware/sim -B firmware/build-sim && cmake --build firmware/build-sim`) and the gateway's `fakelc` show what a correct implementation looks like on the wire. Note macOS gotcha: AirPlay squats on port 5000.
