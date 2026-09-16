#!/usr/bin/env python3
"""Upload an application or LittleFS image through the ArduinoOTA listener."""

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
DEFAULT_IMAGE = ROOT / "build" / "esp32s3-serialusb-network.bin"
DEFAULT_FILESYSTEM_IMAGE = ROOT / "build" / "littlefs.bin"
DEFAULT_CONFIG = ROOT / "main" / "config.h"


def read_config_password(config_path: Path) -> str | None:
    if not config_path.is_file():
        return None

    defines: dict[str, str] = {}
    pattern = re.compile(r"^\s*#define\s+(OTA_PASSWORD|HTTP_PASSWORD)\s+(.+?)\s*$")
    for line in config_path.read_text(encoding="utf-8").splitlines():
        match = pattern.match(line)
        if match:
            defines[match.group(1)] = match.group(2).split("//", 1)[0].strip()

    value = defines.get("OTA_PASSWORD")
    if value is None:
        return None
    if value in defines:
        value = defines[value]
    if len(value) >= 2 and value[0] == '"' and value[-1] == '"':
        return value[1:-1]
    return value


def find_espota() -> Path | None:
    configured = os.environ.get("ESPOTA_PATH")
    candidates = [
        Path(configured) if configured else None,
        Path.home() / ".platformio/packages/framework-arduinoespressif32/tools/espota.py",
        Path.home() / ".arduino15/packages/esp32/hardware/esp32/3.3.10/tools/espota.py",
    ]
    for candidate in candidates:
        if candidate and candidate.is_file():
            return candidate
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("device", help="ESP32 IP address or hostname")
    parser.add_argument("-f", "--file", type=Path,
                        help="image file (defaults to the application or LittleFS build image)")
    parser.add_argument("-p", "--port", type=int, default=3232,
                        help="ESP32 OTA UDP port (default: 3232)")
    parser.add_argument("-I", "--host-ip", default="0.0.0.0",
                        help="local address for the callback server (default: 0.0.0.0)")
    parser.add_argument("-P", "--host-port", type=int,
                        help="local callback port (default: random available OTA port)")
    parser.add_argument("-a", "--password",
                        help="OTA password (otherwise read from main/config.h or OTA_PASSWORD)")
    parser.add_argument("--espota", type=Path,
                        help="path to espota.py (otherwise search common installations)")
    parser.add_argument("-d", "--debug", action="store_true")
    parser.add_argument("-r", "--progress", action="store_true")
    parser.add_argument("-s", "--spiffs", action="store_true",
                        help="upload a LittleFS image instead of application firmware")
    args = parser.parse_args()

    default_image = DEFAULT_FILESYSTEM_IMAGE if args.spiffs else DEFAULT_IMAGE
    image_arg = args.file if args.file is not None else default_image
    image = image_arg if image_arg.is_absolute() else ROOT / image_arg
    if not image.is_file():
        parser.error(f"firmware image not found: {image}")

    espota = args.espota or find_espota()
    if not espota or not espota.is_file():
        parser.error("espota.py not found; pass --espota PATH or set ESPOTA_PATH")

    password = args.password
    if password is None:
        password = os.environ.get("OTA_PASSWORD")
    if password is None:
        password = read_config_password(DEFAULT_CONFIG)
    if password is None:
        parser.error("OTA password not found; pass --password or set OTA_PASSWORD")

    command = [sys.executable, str(espota), "-i", args.device, "-I", args.host_ip,
               "-p", str(args.port), "-a", password, "-f", str(image)]
    if args.host_port is not None:
        command.extend(["-P", str(args.host_port)])
    if args.debug:
        command.append("-d")
    if args.progress:
        command.append("-r")
    if args.spiffs:
        command.append("-s")

    return subprocess.run(command, cwd=ROOT).returncode


if __name__ == "__main__":
    raise SystemExit(main())