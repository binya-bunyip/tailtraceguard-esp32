#!/usr/bin/env python3
"""Create a versioned full-flash image from ESP-IDF's generated flash inputs."""

import argparse
import hashlib
import json
import re
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
BUILD_DIR = ROOT / "build"
DIST_DIR = ROOT / "dist"
EXPECTED_FLASH_FILES = {
    "0x1000": "bootloader/bootloader.bin",
    "0x8000": "partition_table/partition-table.bin",
    "0xe000": "ota_data_initial.bin",
    "0x10000": "tailtraceguard_esp32.bin",
}
FLASH_SIZE_BYTES = 4 * 1024 * 1024


def sha256_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as firmware:
        for chunk in iter(lambda: firmware.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--version", required=True)
    parser.add_argument("--commit", required=True)
    args = parser.parse_args()

    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9.+-]*", args.version):
        parser.error("version must contain only letters, digits, dot, plus, or hyphen")
    if not re.fullmatch(r"[0-9a-fA-F]{40}", args.commit):
        parser.error("commit must be a full 40-character Git SHA")

    flasher_args_path = BUILD_DIR / "flasher_args.json"
    with flasher_args_path.open(encoding="utf-8") as source:
        flasher_args = json.load(source)

    actual_flash_files = flasher_args["flash_files"]
    if actual_flash_files != EXPECTED_FLASH_FILES:
        raise SystemExit(
            "Generated flash inputs differ from the expected partition layout: "
            f"{actual_flash_files!r}"
        )
    flash_settings = flasher_args["flash_settings"]
    if flash_settings.get("flash_size") != "4MB":
        raise SystemExit("Generated flash settings do not specify 4MB flash")

    for relative_path in actual_flash_files.values():
        if not (BUILD_DIR / relative_path).is_file():
            raise SystemExit(f"Required flash image is missing: {relative_path}")

    DIST_DIR.mkdir(exist_ok=True)
    asset_name = f"tailtraceguard-esp32-{args.version}-full-flash.bin"
    asset_path = DIST_DIR / asset_name
    merge_command = [
        "esptool.py",
        "--chip",
        "esp32",
        "merge_bin",
        "--output",
        str(asset_path),
        "--flash_mode",
        flash_settings["flash_mode"],
        "--flash_freq",
        flash_settings["flash_freq"],
        "--flash_size",
        "4MB",
        "--fill-flash-size",
        "4MB",
    ]
    for offset, relative_path in sorted(actual_flash_files.items(), key=lambda item: int(item[0], 16)):
        merge_command.extend([offset, str(BUILD_DIR / relative_path)])
    subprocess.run(merge_command, cwd=ROOT, check=True)

    if asset_path.stat().st_size != FLASH_SIZE_BYTES:
        raise SystemExit("Merged image is not exactly the configured 4 MiB flash size")

    asset_checksum = sha256_file(asset_path)
    manifest = {
        "manifest_version": 1,
        "firmware_version": args.version,
        "source_commit": args.commit.lower(),
        "target": "esp32",
        "target_board": "Espressif ESP32-DevKitC v4; hardware behavior not verified",
        "flash_size_bytes": FLASH_SIZE_BYTES,
        "assets": [
            {
                "name": asset_name,
                "type": "full-flash",
                "flash_address": "0x0",
                "sha256": asset_checksum,
            }
        ],
    }
    (DIST_DIR / "manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
    )
    (DIST_DIR / "checksums.sha256").write_text(
        f"{asset_checksum}  {asset_name}\n", encoding="ascii"
    )


if __name__ == "__main__":
    main()