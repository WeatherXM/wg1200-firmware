#!/usr/bin/env python3
"""Safely replace only the signed WG1200 ESP32-S3 bootloader.

The normal PlatformIO upload target writes application slots only. This tool
updates the bootloader at offset 0x0 while preserving the partition table, NVS,
and the irreplaceable esp_secure_cert partition.
"""

from __future__ import annotations

import argparse
import datetime as dt
import glob
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


REPO_ROOT = Path(__file__).resolve().parent.parent
FIRMWARE_DIR = REPO_ROOT / "wg1200-firmware"
SECURE_CERT_OFFSET = 0xD000
SECURE_CERT_SIZE = 0x2000
NVS_OFFSET = 0xF000
NVS_SIZE = 0x4000
TLV_MAGIC = b"\x11\xba\x5e\xba"
SECTOR_SIZE = 0x1000


class ToolError(RuntimeError):
    pass


def run(command: list[str], *, capture: bool = False, cwd: Path = FIRMWARE_DIR) -> str:
    printable = " ".join(str(part) for part in command)
    print(f"+ {printable}")
    result = subprocess.run(
        command,
        cwd=cwd,
        text=True,
        stdout=subprocess.PIPE if capture else None,
        stderr=subprocess.STDOUT if capture else None,
    )
    output = result.stdout or ""
    if capture and output:
        print(output.rstrip())
    if result.returncode != 0:
        raise ToolError(f"Command failed with exit code {result.returncode}: {printable}")
    return output


def first_existing(candidates: list[Path | str]) -> str:
    for candidate in candidates:
        path = Path(candidate).expanduser()
        if path.is_file():
            return str(path)
        resolved = shutil.which(str(candidate))
        if resolved:
            return resolved
    raise ToolError(f"Required executable was not found: {', '.join(map(str, candidates))}")


def platformio_paths() -> tuple[str, str, str, str]:
    pio_home = Path(os.environ.get("PLATFORMIO_CORE_DIR", Path.home() / ".platformio"))
    python = first_existing([pio_home / "penv/bin/python", sys.executable])
    platformio = first_existing([pio_home / "penv/bin/platformio", "platformio", "pio"])
    esptool = first_existing(
        [
            pio_home / "packages/tool-esptoolpy/esptool.py",
            pio_home / "packages/tool-esptoolpy@1.40501.0/esptool.py",
        ]
    )
    espsecure = first_existing(
        [pio_home / "penv/bin/espsecure.py", pio_home / "penv/bin/espsecure"]
    )
    return python, platformio, esptool, espsecure


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def normalize_mac(value: str) -> str:
    compact = re.sub(r"[^0-9a-fA-F]", "", value).lower()
    if len(compact) != 12:
        raise ToolError(f"Invalid MAC address: {value}")
    return ":".join(compact[index:index + 2] for index in range(0, 12, 2))


def resolve_port(pattern: str) -> str:
    if not any(char in pattern for char in "*?["):
        return pattern
    matches = sorted(glob.glob(pattern))
    if not matches:
        raise ToolError(f"No serial port matches {pattern!r}")
    if len(matches) > 1:
        raise ToolError(f"Multiple serial ports match {pattern!r}: {', '.join(matches)}")
    return matches[0]


def load_build_config(environment: str) -> tuple[Path, dict]:
    build_dir = FIRMWARE_DIR / ".pio/build" / environment
    config_path = build_dir / "bootloader/config/sdkconfig.json"
    artifact = build_dir / "bootloader-signed.bin"
    if not config_path.is_file() or not artifact.is_file():
        raise ToolError(
            f"Missing build output for {environment}. Run PlatformIO first or omit --no-build."
        )
    with config_path.open("r", encoding="utf-8") as stream:
        config = json.load(stream)
    return artifact, config


def signature_digest(espsecure: str, image: Path, *, quiet: bool = False) -> str | None:
    cmd = "signature_info_v2"
    result = subprocess.run(
        [espsecure, cmd, str(image)],
        cwd=FIRMWARE_DIR,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    output = result.stdout or ""
    if not quiet:
        print(output.rstrip())
    if result.returncode != 0 or "Signature block 0 is valid" not in output:
        return None
    match = re.search(r"Public key digest for block 0:\s*([0-9a-fA-F ]+)", output)
    if not match:
        return None
    return re.sub(r"\s+", "", match.group(1)).lower()


def validate_artifact(
    environment: str, espsecure: str
) -> tuple[Path, int, int, str]:
    artifact, config = load_build_config(environment)
    bootloader_offset = int(config.get("BOOTLOADER_OFFSET_IN_FLASH", -1))
    partition_offset = int(config.get("PARTITION_TABLE_OFFSET", -1))

    factory_reset_requirements = {
        "BOOTLOADER_FACTORY_RESET": True,
        "BOOTLOADER_NUM_PIN_FACTORY_RESET": 38,
        "BOOTLOADER_FACTORY_RESET_PIN_LOW": True,
        "BOOTLOADER_OTA_DATA_ERASE": True,
        "BOOTLOADER_DATA_FACTORY_RESET": "nvs",
        "BOOTLOADER_HOLD_TIME_GPIO": 5,
        "BOOTLOADER_WDT_ENABLE": True,
        "BOOTLOADER_WDT_TIME_MS": 9000,
    }
    for key, expected in factory_reset_requirements.items():
        actual = config.get(key)
        if actual != expected:
            raise ToolError(
                f"Refusing artifact: {key} must be {expected!r}, got {actual!r}"
            )
    if config.get("SECURE_BOOT_V2_ENABLED") is not True:
        raise ToolError("Refusing artifact: Secure Boot V2 is not enabled")
    if config.get("SECURE_BOOT_BUILD_SIGNED_BINARIES") is not True:
        raise ToolError("Refusing artifact: signed binary generation is not enabled")
    if bootloader_offset != 0:
        raise ToolError(f"Unexpected ESP32-S3 bootloader offset: 0x{bootloader_offset:x}")
    if partition_offset <= bootloader_offset:
        raise ToolError(f"Invalid partition table offset: 0x{partition_offset:x}")
    if artifact.stat().st_size > partition_offset - bootloader_offset:
        raise ToolError(
            f"Bootloader is {artifact.stat().st_size} bytes and overlaps the partition table"
        )

    digest = signature_digest(espsecure, artifact)
    if not digest:
        raise ToolError("The bootloader artifact does not contain a valid RSA signature block")

    key_path = config.get("SECURE_BOOT_SIGNING_KEY")
    if key_path and Path(key_path).expanduser().is_file():
        run(
            [
                espsecure,
                "verify_signature",
                "--version",
                "2",
                "--keyfile",
                str(Path(key_path).expanduser()),
                str(artifact),
            ]
        )

    print("Artifact validation passed:")
    print(f"  image:              {artifact}")
    print(f"  size:               {artifact.stat().st_size} bytes")
    print(f"  SHA-256:            {sha256(artifact)}")
    print(f"  signing-key digest: {digest}")
    print(f"  write range:        0x{bootloader_offset:05x}-0x{artifact.stat().st_size - 1:05x}")
    print(f"  partition table:    0x{partition_offset:05x} (not written)")
    return artifact, bootloader_offset, partition_offset, digest


def esptool_base(python: str, esptool: str, port: str, baud: int) -> list[str]:
    return [
        python,
        esptool,
        "--no-stub",
        "--chip",
        "esp32s3",
        "--port",
        port,
        "--baud",
        str(baud),
    ]


def read_device_identity(base: list[str]) -> tuple[str, str]:
    mac_output = run(base + ["--before", "default_reset", "--after", "no_reset", "read_mac"], capture=True)
    match = re.search(r"MAC:\s*([0-9a-fA-F:.-]{12,})", mac_output)
    if not match:
        raise ToolError("Could not parse the connected ESP32-S3 MAC address")
    mac = normalize_mac(match.group(1))

    security = run(base + ["--before", "no_reset", "--after", "no_reset", "get_security_info"], capture=True)
    lower = security.lower()
    if "secure boot: enabled" not in lower and "secure boot is enabled" not in lower:
        raise ToolError("Connected device does not report Secure Boot as enabled")
    if "flash encryption: enabled" in lower or "flash encryption is enabled" in lower:
        raise ToolError("Flash-encrypted bootloader replacement is not supported by this tool")
    return mac, security


def reset_device(base: list[str]) -> None:
    run(base + ["--before", "no_reset", "--after", "hard_reset", "run"])


def best_effort_reset(base: list[str]) -> None:
    try:
        reset_device(base)
    except Exception as exc:
        print(f"[WARN] Could not reset the device automatically: {exc}", file=sys.stderr)


def read_flash(base: list[str], offset: int, size: int, output: Path) -> None:
    run(
        base
        + [
            "--before",
            "no_reset",
            "--after",
            "no_reset",
            "read_flash",
            hex(offset),
            hex(size),
            str(output),
        ]
    )
    if not output.is_file() or output.stat().st_size != size:
        raise ToolError(f"Flash backup has the wrong size: {output}")


def installed_signature_digest(espsecure: str, region: Path) -> str | None:
    data = region.read_bytes()
    for end in range(SECTOR_SIZE * 2, len(data) + 1, SECTOR_SIZE):
        with tempfile.NamedTemporaryFile(suffix=".bin") as candidate:
            candidate.write(data[:end])
            candidate.flush()
            digest = signature_digest(espsecure, Path(candidate.name), quiet=True)
            if digest:
                return digest
    return None


def create_backups(
    base: list[str], mac: str, partition_offset: int, espsecure: str, expected_digest: str
) -> Path:
    stamp = dt.datetime.now().strftime("%Y%m%d_%H%M%S")
    backup_dir = FIRMWARE_DIR / "backups/bootloader_recovery" / f"{mac.replace(':', '')}_{stamp}"
    backup_dir.mkdir(parents=True, exist_ok=False)

    cert = backup_dir / "esp_secure_cert_0xd000.bin"
    nvs = backup_dir / "nvs_0xf000.bin"
    bootloader = backup_dir / "installed_bootloader_region_0x0.bin"
    read_flash(base, SECURE_CERT_OFFSET, SECURE_CERT_SIZE, cert)
    read_flash(base, NVS_OFFSET, NVS_SIZE, nvs)
    read_flash(base, 0, partition_offset, bootloader)

    cert_data = cert.read_bytes()
    if TLV_MAGIC not in cert_data or all(byte == 0xFF for byte in cert_data):
        raise ToolError(
            f"esp_secure_cert backup failed validation; nothing was flashed. Backup: {cert}"
        )

    installed_digest = installed_signature_digest(espsecure, bootloader)
    if not installed_digest:
        raise ToolError(
            "Could not validate the installed bootloader signature; nothing was flashed"
        )
    if installed_digest != expected_digest:
        raise ToolError(
            "Signing-key mismatch between installed and replacement bootloaders; nothing was flashed\n"
            f"  installed:   {installed_digest}\n"
            f"  replacement: {expected_digest}"
        )

    manifest = {
        "device_mac": mac,
        "created_at": dt.datetime.now(dt.timezone.utc).isoformat(),
        "installed_signing_key_digest": installed_digest,
        "files": {
            path.name: {"size": path.stat().st_size, "sha256": sha256(path)}
            for path in (cert, nvs, bootloader)
        },
    }
    manifest_path = backup_dir / "manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"Mandatory recovery backups validated: {backup_dir}")
    return backup_dir


def confirm_device(mac: str, args: argparse.Namespace) -> None:
    if args.yes:
        if not args.expected_mac:
            raise ToolError("--yes requires --expected-mac to prevent flashing the wrong device")
        if normalize_mac(args.expected_mac) != mac:
            raise ToolError(f"Connected MAC {mac} does not match --expected-mac {args.expected_mac}")
        return

    print("\nThis operation replaces the signed bootloader at 0x0.")
    typed = input(f"Type the connected device MAC ({mac}) to continue: ").strip()
    if normalize_mac(typed) != mac:
        raise ToolError("Confirmation did not match; nothing was flashed")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Validate and safely update the signed WG1200 ESP32-S3 bootloader."
    )
    parser.add_argument("--port", help="Serial port or a glob resolving to exactly one port")
    parser.add_argument(
        "--environment",
        choices=("wg1200_release", "wg1200_debug"),
        default="wg1200_release",
    )
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--no-build", action="store_true", help="Use the existing build artifact")
    parser.add_argument(
        "--validate-only",
        action="store_true",
        help="Validate the local signed artifact without connecting to a device",
    )
    parser.add_argument(
        "--execute",
        action="store_true",
        help="Actually write the bootloader; without this flag the device is inspected only",
    )
    parser.add_argument(
        "--backup-only",
        action="store_true",
        help="Validate and save recovery backups without writing the bootloader",
    )
    parser.add_argument("--yes", action="store_true", help="Non-interactive confirmation")
    parser.add_argument("--expected-mac", help="Required with --yes")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if not FIRMWARE_DIR.is_dir():
        raise ToolError(f"Firmware directory not found: {FIRMWARE_DIR}")
    if args.baud <= 0:
        raise ToolError("--baud must be positive")
    if args.baud > 115200:
        print(f"Capping baud from {args.baud} to 115200 for ROM-loader reliability")
        args.baud = 115200

    python, platformio, esptool, espsecure = platformio_paths()
    if not args.no_build:
        run([platformio, "run", "--environment", args.environment])

    artifact, offset, partition_offset, digest = validate_artifact(args.environment, espsecure)
    if args.validate_only:
        return 0
    if not args.port:
        raise ToolError("--port is required unless --validate-only is used")

    port = resolve_port(args.port)
    base = esptool_base(python, esptool, port, args.baud)
    try:
        mac, _ = read_device_identity(base)
    except BaseException:
        best_effort_reset(base)
        raise
    print(f"Connected device: {mac} on {port}")

    if not args.execute and not args.backup_only:
        reset_device(base)
        print("Dry run complete. Re-run with --execute to back up and flash the bootloader.")
        return 0

    try:
        backup_dir = create_backups(base, mac, partition_offset, espsecure, digest)
        if args.backup_only:
            reset_device(base)
            print("Backup-only validation completed; nothing was flashed.")
            return 0
        confirm_device(mac, args)
    except BaseException:
        best_effort_reset(base)
        raise

    print(f"Writing {artifact.name} at 0x{offset:x}; recovery backups: {backup_dir}")
    try:
        run(
            base
            + [
                "--before",
                "no_reset",
                "--after",
                "no_reset",
                "write_flash",
                "--force",
                "--verify",
                "--flash_mode",
                "keep",
                "--flash_freq",
                "keep",
                "--flash_size",
                "keep",
                hex(offset),
                str(artifact),
            ]
        )
        run(
            base
            + [
                "--before",
                "no_reset",
                "--after",
                "hard_reset",
                "verify_flash",
                hex(offset),
                str(artifact),
            ]
        )
    except BaseException:
        best_effort_reset(base)
        raise
    print("Bootloader update and read-back verification completed successfully.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (ToolError, KeyboardInterrupt, EOFError) as exc:
        print(f"[ERROR] {exc}", file=sys.stderr)
        raise SystemExit(1)
