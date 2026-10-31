# TailTrace Guard ESP32

This repository contains an ESP32 firmware project that listens for nearby Wi‑Fi probe requests and batches the data for upload to your TailTrace Guard API.

It is built with ESP-IDF and produces a flashable firmware image (.bin) for an ESP32 device.

## What it does
- Enables Wi‑Fi promiscuous mode
- Captures probe request source MAC addresses
- Reads RSSI values and timestamps
- Aggregates sightings into batches
- Sends JSON payloads to the TailTrace Guard API

## Requirements
- ESP-IDF v5.x installed and available in your shell
- An ESP32 development board
- A Wi‑Fi network for the device to connect to
- A TailTrace Guard API endpoint and bearer token

## Quick start

1. Install ESP-IDF: https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/
2. Export the environment:

   ```bash
   . $IDF_PATH/export.sh
   ```

3. Configure your Wi‑Fi and API values in `sdkconfig.defaults` and `main/main.c`.
4. Build:

   ```bash
   idf.py set-target esp32
   idf.py build
   ```

5. Flash to your board:

   ```bash
   idf.py -p /dev/ttyUSB0 flash
   ```

6. The firmware binary will be created in the `build/` directory. The generated app image is typically named like:

   ```text
   build/tailtraceguard_esp32.bin
   ```

## Notes
- This is a sensor node design. It captures and reports observations, but the heavier storage and analytics logic should remain in the TailTrace Guard dashboard backend.
- For production use, add deduplication, local ignore lists, retry logic, and stronger TLS validation.

## API contract
The device posts JSON like this:

```json
{
  "device_id": "esp32-tailtrace-sensor",
  "events": [
    {
      "mac": "AA:BB:CC:DD:EE:FF",
      "rssi": -62,
      "count": 4,
      "ts_ms": 1729600000000
    }
  ]
}
```

## Example firmware generation

```bash
export IDF_PATH=~/esp/esp-idf
. "$IDF_PATH/export.sh"
idf.py set-target esp32
idf.py build
ls -l build
```
