"""Authenticated LAN OTA using the uploader shipped with Arduino-ESP32 3.3.11."""
from __future__ import annotations

import argparse
import importlib.util
import os
from pathlib import Path
import socket


def validate_firmware(path: Path) -> None:
    if not path.name.endswith(".ino.bin"):
        raise ValueError("Select the application .ino.bin, not a bootloader or merged image")
    # Existing default partition table: two 0x140000 application slots.
    size = path.stat().st_size
    if size < 24 or size > 0x140000:
        raise ValueError("Firmware does not fit the default OTA application slot")
    with path.open("rb") as stream:
        header = stream.read(24)
    # ESP image magic and little-endian ESP32-C6 chip id (13).
    if header[0] != 0xE9 or int.from_bytes(header[12:14], "little") != 13:
        raise ValueError("Expected an ESP32-C6 application .ino.bin, not a merged image")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--address", default="codexmeter-c6.local")
    parser.add_argument("--firmware", type=Path, required=True)
    parser.add_argument("--espota", type=Path, required=True)
    parser.add_argument("--host-port", type=int, default=3233)
    args = parser.parse_args()
    password = os.environ.pop("CODEX_C6_OTA_PASSWORD", "")
    if len(password) < 16:
        parser.error("Set CODEX_C6_OTA_PASSWORD locally (at least 16 characters)")
    try:
        validate_firmware(args.firmware)
        address = socket.gethostbyname(args.address)
        spec = importlib.util.spec_from_file_location("espota", args.espota)
        if spec is None or spec.loader is None:
            raise ValueError("Cannot load the Arduino-ESP32 uploader")
        uploader = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(uploader)
        uploader.PROGRESS = False
        uploader.TIMEOUT = 10
        # Import directly so the password is never exposed in process arguments.
        return uploader.serve(address, "0.0.0.0", 3232, args.host_port,
                              password, False, str(args.firmware), uploader.FLASH)
    except (OSError, ValueError):
        print("OTA preflight failed: check host, C6 application image, and uploader path.")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
