# TailTrace Guard ESP32

This repository builds generic firmware for Espressif's `esp32` target using ESP-IDF v5.2.3. The exact development-board model is not identified in this repository; the configured flash size is 4 MiB.

## Download a firmware release

Download the versioned `full-flash` binary, `checksums.sha256`, and `manifest.json` from the project's [GitHub Releases](https://github.com/binya-bunyip/tailtraceguard-esp32/releases). Releases are created only from version tags such as `v1.2.3`; a release is available only after that tag has been published and its workflow succeeds.

The binary contains the ESP-IDF bootloader, custom partition table, initial OTA data, and application image combined for flashing at address `0x0`. It is padded to the configured 4 MiB flash size. It does not contain Wi-Fi credentials, device tokens, private keys, or device-specific provisioning data.

Before flashing, confirm the board uses a compatible generic ESP32 and has 4 MiB flash. Disconnect the board from other power sources, connect its USB serial port, and verify the checksum in the directory containing the downloaded files:

```bash
sha256sum -c checksums.sha256
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

The firmware enables Wi-Fi promiscuous mode, listens for Wi-Fi probe-request management frames on 2.4 GHz channels 1 through 13, and counts observations locally. It does not connect to Wi-Fi or send telemetry. It does not currently scan Bluetooth.

Device-specific Wi-Fi credentials and registered bearer tokens must be provided after download using a private provisioning method. No such method exists in this repository, so the released binary is generic test firmware and is not end-to-end ready for telemetry or website use. Do not put credentials in source code or public build configuration.

## Telemetry contract status

The documented endpoint is `POST https://tail-guard.polsia.io/api/telemetry` with `Authorization: Bearer <registered device token>`, `Content-Type: application/json`, and the required body fields `observedAt`, `observedAddress`, `protocol`, `rssiDbm`, and `frequencyMHz`; optional GPS coordinates are also mentioned by the contract. The old `device_id`/`events` batch payload is not used or documented as a compatible request.

The existing capture code can identify a Wi-Fi source address and received signal strength. It does not establish a wall-clock value or representation for `observedAt`, nor does it currently retain the radio frequency in MHz. The repository also does not define the GPS coordinate shape, the Bluetooth observation source, or a private provisioning method. These are unresolved integration requirements, so this firmware intentionally sends no telemetry. The offline tests check the declared contract names and protocol values; they do not call the live API or validate website integration. No live API connectivity, provisioning, or firmware-page discovery has been tested.

## Automated builds and releases

Pull requests, branch pushes, and manually started workflow runs build and test in the pinned `espressif/idf:v5.2.3` environment for target `esp32`. They upload a temporary package artifact and do not publish a release unless the run is explicitly for a `vMAJOR.MINOR.PATCH` version tag. A valid version-tag run creates a GitHub Release with the generated firmware, `checksums.sha256`, and `manifest.json`.

The manifest is JSON with this shape:

```json
{
  "manifest_version": 1,
  "firmware_version": "v1.2.3",
  "source_commit": "<40-character Git commit SHA>",
  "target": "esp32",
  "target_board": "Generic ESP32; exact board model is unspecified",
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