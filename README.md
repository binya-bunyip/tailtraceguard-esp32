# TailTrace Guard ESP32

This repository builds for Espressif's `esp32` target using ESP-IDF v5.2.3. The intended board is Espressif ESP32-DevKitC v4 with 4 MiB flash, as specified for this device; physical hardware behavior has not been verified here.

## Download a firmware release

Download the versioned `full-flash` binary, `checksums.sha256`, and `manifest.json` from the project's [GitHub Releases](https://github.com/binya-bunyip/tailtraceguard-esp32/releases). Releases are created only from version tags such as `v1.2.3`; a release is available only after that tag has been published and its workflow succeeds.

The binary contains the ESP-IDF bootloader, custom partition table, initial OTA data, and application image combined for flashing at address `0x0`. It is padded to the configured 4 MiB flash size. It does not contain Wi-Fi credentials, device tokens, private keys, or device-specific provisioning data.

Release `v1.0.0` predates the USB provisioning and Wi-Fi telemetry implementation described below; it only captures and counts Wi-Fi probe requests. Release `v1.1.0` is built from current `main` commit `9d0a1402e885c78ae6d12dfaa0900a9e2d5a31a7` and contains the Wi-Fi telemetry and USB serial provisioning source described here. Physical hardware behavior and live API delivery remain unverified for both releases.

Before flashing, confirm the board is Espressif ESP32-DevKitC v4 with 4 MiB flash. Disconnect the board from other power sources, connect its USB serial port, and verify the checksum in the directory containing the downloaded files:

```bash
sha256sum -c checksums.sha256
```

To verify that GitHub attested the package assets for this repository and workflow, install the GitHub CLI and run this for the firmware binary (repeat with `manifest.json` and `checksums.sha256` to verify those assets too):

```bash
gh attestation verify tailtraceguard-esp32-v1.2.3-full-flash.bin \
  --repo binya-bunyip/tailtraceguard-esp32 \
  --signer-workflow binya-bunyip/tailtraceguard-esp32/.github/workflows/firmware.yml \
  --predicate-type https://github.com/binya-bunyip/tailtraceguard-esp32/attestation-types/firmware-package/v1
```

Erase and flash the complete image (replace `PORT` and the asset filename with the local values):

```bash
esptool.py --chip esp32 --port PORT erase_flash
esptool.py --chip esp32 --port PORT --baud 460800 write_flash \
  --flash_mode dio --flash_freq 40m --flash_size 4MB \
  0x0 tailtraceguard-esp32-v1.2.3-full-flash.bin
```

Erasing removes existing data and settings on the device. Use a release that matches the actual board target and flash size. The versioned asset name, source commit, target, flash address, and SHA-256 are recorded in `manifest.json`.

## Firmware behavior and setup status

The current source captures Wi-Fi probe requests on 2.4 GHz channels 1 through 13. It records the frame's source MAC, RSSI, receive channel (converted to MHz), and UTC timestamp after SNTP synchronization. It pauses promiscuous scanning to connect to the configured network and uploads each observation separately over HTTPS with the registered bearer token. The HTTPS client uses ESP-IDF's CA certificate bundle.

On first boot without provisioned settings, connect a trusted USB serial terminal to the board at 115200 baud. Disable the terminal's local echo, then enter the Wi-Fi network name, password (blank for an open network), and registered device token at the prompts. The firmware does not print the values. They are stored in NVS in plaintext, as approved for physically trusted setup; anyone with physical flash access can recover them. Do not provision in an untrusted location. To replace credentials, erase only the NVS partition while the board is in its bootloader, then reset it so the firmware prompts again:

```bash
esptool.py --chip esp32 --port PORT erase_region 0x9000 0x5000
```

No credentials are embedded in source, build defaults, CI, or generic release binaries. The provisioning path does not currently provide NVS/flash encryption.

## Telemetry contract status

The documented endpoint is `POST https://tail-guard.polsia.io/api/telemetry` with `Authorization: Bearer <registered device token>` and `Content-Type: application/json`. The required JSON fields are `observedAt` (ISO 8601 with timezone), `observedAddress` (Wi-Fi MAC or Bluetooth LE private address), `protocol` (`wifi` or `bluetooth`), `rssiDbm`, and `frequencyMHz`. Optional GPS data has the shape `{"latitude": ..., "longitude": ...}`. The old `device_id`/`events` batch payload is not sent or documented as a compatible request.

Bluetooth telemetry remains blocked. The ESP-IDF 5.2.3 BLE GAP scan report available to this firmware provides an address and RSSI but not the advertising receive channel; the required `frequencyMHz` cannot be determined without controller-level channel reporting or a different scanner. The firmware therefore does not scan Bluetooth or submit Bluetooth observations. GPS is optional and no GPS receiver is identified on the board.

The offline tests validate representative request objects, UTC formatting, Wi-Fi frequency mapping, and the sender's documented field mapping without contacting the live API. A successful compile and source-level tests do not verify serial provisioning on hardware, RF capture accuracy, network/time synchronization in deployment, server acceptance, BLE scanning, GPS, or integration with the TailTrace Guard website. End-to-end and hardware behavior remain unverified.

A successful CI compile confirms that the source builds for the configured ESP32 target; it does not verify physical-board compatibility, RF capture behavior, telemetry delivery, credential provisioning, or website integration.

## Automated builds and releases

Pull requests, branch pushes, and manually started workflow runs build and test in the pinned `espressif/idf:v5.2.3` environment for target `esp32`. They upload a temporary package artifact and do not publish a release unless the run is explicitly for a `vMAJOR.MINOR.PATCH` version tag. A valid version-tag run creates a GitHub Release with the generated firmware, `checksums.sha256`, and `manifest.json`, and adds keyless GitHub attestations for all three assets. Attestations are bound to their asset digests and identify the source tag/commit, workflow run, ESP-IDF build image, and target.

The release manifest is JSON with this shape:

```json
{
  "manifest_version": 1,
  "firmware_version": "v1.2.3",
  "source_commit": "<40-character Git commit SHA>",
  "target": "esp32",
  "target_board": "Espressif ESP32-DevKitC v4; hardware behavior not verified",
  "flash_size_bytes": 4194304,
  "assets": [
    {
      "name": "tailtraceguard-esp32-v1.2.3-full-flash.bin",
      "type": "full-flash",
      "flash_address": "0x0",
      "sha256": "<64-character SHA-256>"
    }
  ]
}
```

## Optional local development

Install ESP-IDF v5.2.3 and its supported tools, then from the repository root:

```bash
. "$IDF_PATH/export.sh"
idf.py set-target esp32
idf.py build
python -m unittest discover -s tests
```

The application binary is `build/tailtraceguard_esp32.bin`; it is not a complete flash package. ESP-IDF generates the required bootloader, partition table, and OTA data images plus their exact offsets in `build/flasher_args.json`. The release workflow combines those images and validates the resulting package. Do not flash the application binary alone as if it were the complete firmware.