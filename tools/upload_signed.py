#!/usr/bin/env python3
"""Build-signing-aware WG1200 application upload and certificate recovery."""
import sys
import os
import re
import subprocess
import datetime

# esp_secure_cert partition location (see partitions_16MB_wxm.csv).
# Holds the factory device certificate + DS-encrypted private key params.
# It is NOT rebuildable if lost, so we snapshot it byte-exact before every flash.
SECURE_CERT_OFFSET = "0xd000"
SECURE_CERT_SIZE = "0x2000"
TLV_MAGIC = b"\x11\xba\x5e\xba"  # 0xBA5EBA11, little-endian as stored on flash
PLATFORMIO_HOME = os.path.expanduser(
    os.environ.get("PLATFORMIO_CORE_DIR", os.path.join("~", ".platformio"))
)
REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def first_existing(*paths):
    for path in paths:
        expanded = os.path.expanduser(path)
        if os.path.isfile(expanded):
            return expanded
    raise FileNotFoundError(f"Required PlatformIO tool not found: {', '.join(paths)}")


def get_esptool_path():
    return first_existing(
        os.path.join(PLATFORMIO_HOME, "packages", "tool-esptoolpy", "esptool.py"),
        os.path.join(PLATFORMIO_HOME, "packages", "tool-esptoolpy@1.40501.0", "esptool.py"),
        os.path.expanduser("~/.platformio_wg1200/packages/tool-esptoolpy/esptool.py"),
        os.path.expanduser("~/.platformio/packages/tool-esptoolpy/esptool.py"),
    )


def get_platformio_python():
    return first_existing(
        os.path.join(PLATFORMIO_HOME, "penv", "bin", "python"),
        os.path.expanduser("~/.platformio_wg1200/penv/.espidf-6.0.1/bin/python"),
        os.path.expanduser("~/.platformio_wg1200/penv/bin/python"),
        os.path.expanduser("~/.platformio/penv/bin/python"),
        sys.executable,
    )


def get_espsecure_path():
    return first_existing(
        os.path.join(PLATFORMIO_HOME, "penv", "bin", "espsecure"),
        os.path.join(PLATFORMIO_HOME, "penv", "bin", "espsecure.py"),
        os.path.expanduser("~/.platformio_wg1200/penv/.espidf-6.0.1/bin/espsecure"),
        os.path.expanduser("~/.platformio_wg1200/penv/.espidf-6.0.1/bin/espsecure.py"),
        os.path.expanduser("~/.platformio_wg1200/penv/bin/espsecure"),
        os.path.expanduser("~/.platformio/penv/bin/espsecure"),
        os.path.expanduser("~/.platformio/penv/bin/espsecure.py"),
    )


def _extract_pem_cert(data):
    """Pull the first PEM certificate out of a raw partition dump, or None."""
    begin = b"-----BEGIN CERTIFICATE-----"
    end = b"-----END CERTIFICATE-----"
    start = data.find(begin)
    if start == -1:
        return None
    stop = data.find(end, start)
    if stop == -1:
        return None
    return data[start:stop + len(end)] + b"\n"


def derive_device_label(data):
    """Best-effort device serial from the cert CN, for naming the backup.

    The device certificate's CN is the device serial (see credentials.cpp
    DEV_CERT_ISSUER_NAME_FORMAT), e.g. "AB:CD:...:XX". Returns a filesystem-safe
    token (separators stripped) or None if the cert can't be read.
    """
    pem = _extract_pem_cert(data)
    if pem is None:
        return None
    try:
        from cryptography import x509
        from cryptography.x509.oid import NameOID
        cert = x509.load_pem_x509_certificate(pem)
        cn = cert.subject.get_attributes_for_oid(NameOID.COMMON_NAME)
        if not cn:
            return None
        serial = cn[0].value
    except Exception:
        return None
    label = re.sub(r"[^0-9A-Za-z]", "", serial)
    return label or None


def backup_secure_cert(port, speed, chip="esp32s3"):
    """Dump the esp_secure_cert partition before flashing (cheap insurance).

    A normal upload never touches 0xd000, but a stray erase_flash does and the
    partition cannot be reconstructed. We snapshot it to a timestamped file and
    sanity-check the result so the operator knows immediately what they captured.
    Returns True only when the saved image contains the expected TLV magic.
    Normal application uploads may treat a failure as a warning; full-chip
    erase operations must treat it as fatal.
    """
    backup_dir = "backups/secure_cert"
    os.makedirs(backup_dir, exist_ok=True)

    timestamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    out_file = os.path.join(backup_dir, f"secure_cert_{timestamp}.bin")

    print(f"\n--- Backing up esp_secure_cert partition "
          f"({SECURE_CERT_OFFSET}/{SECURE_CERT_SIZE}) before flashing ---")

    read_cmd = [
        get_platformio_python(),
        get_esptool_path(),
        "--no-stub",
        "--chip", chip,
        "--port", port,
        "--baud", speed,
        "--before", "default_reset",
        "--after", "no_reset",
        "read_flash",
        SECURE_CERT_OFFSET, SECURE_CERT_SIZE,
        out_file,
    ]

    res = subprocess.run(read_cmd)
    if res.returncode != 0:
        print("[WARN] esp_secure_cert backup FAILED (read_flash error).")
        print("[WARN] No fresh validated backup is available for this device.")
        return False

    try:
        with open(out_file, "rb") as f:
            data = f.read()
    except Exception as e:
        print(f"[WARN] Backup written but could not be re-read for validation: {e}")
        return False

    # Name the file after the device serial so multiple devices stay separable.
    # Manual override (WXM_BACKUP_LABEL) wins, otherwise read it from the cert CN.
    label = os.environ.get("WXM_BACKUP_LABEL") or derive_device_label(data)
    if label:
        final_file = os.path.join(backup_dir, f"secure_cert_{label}_{timestamp}.bin")
        try:
            os.rename(out_file, final_file)
            out_file = final_file
        except OSError as e:
            print(f"[WARN] Could not rename backup with label '{label}': {e}")

    if TLV_MAGIC in data:
        print(f"[OK] Valid secure_cert partition backed up -> {out_file}")
        if not label:
            print("[WARN] Could not read device serial from cert; filename has timestamp only.")
        return True
    elif data and all(b == 0xff for b in data):
        print(f"[WARN] Partition reads as BLANK/ERASED (all 0xFF). Saved anyway -> {out_file}")
        print("[WARN] This device appears to have NO factory credentials right now!")
    else:
        print(f"[WARN] Backed up {len(data)} bytes but TLV magic (0xBA5EBA11) NOT found.")
        print(f"[WARN] Partition may be corrupt or in an unexpected format. Saved -> {out_file}")
    return False


def erase_flow(port, speed, chip="esp32s3"):
    """Back up esp_secure_cert, THEN erase the whole flash.

    Invoked via `--erase` from the wrapped PlatformIO "Erase Flash" target so the
    one operation that actually wipes 0xd000 can no longer do so without a backup.
    """
    print("=== Safe Erase: snapshotting esp_secure_cert before wiping flash ===")
    if not backup_secure_cert(port, speed, chip):
        print("[ERROR] Refusing to erase: no freshly validated secure_cert backup exists.")
        sys.exit(1)

    print("\n--- Erasing entire flash (esptool erase_flash) ---")
    erase_cmd = [
        get_platformio_python(),
        get_esptool_path(),
        "--no-stub",
        "--chip", chip,
        "--port", port,
        "--baud", speed,
        "--before", "default_reset",
        "--after", "hard_reset",
        "erase_flash",
    ]
    res = subprocess.run(erase_cmd)
    if res.returncode != 0:
        print("[ERROR] erase_flash failed!")
        sys.exit(1)
    print("\n================== ERASE COMPLETE ==================\n")


def restore_secure_cert(port, speed, backup_file):
    """Write a backed up esp_secure_cert partition binary back to the device (0xd000)."""
    if not os.path.exists(backup_file):
        print(f"[ERROR] Backup file not found: {backup_file}")
        sys.exit(1)

    print(f"\n=== Restoring esp_secure_cert from {backup_file} to partition ({SECURE_CERT_OFFSET}) ===")
    write_cmd = [
        get_platformio_python(),
        get_esptool_path(),
        "--no-stub",
        "--chip", "esp32s3",
        "--port", port,
        "--baud", speed,
        "--before", "default_reset",
        "--after", "hard_reset",
        "write_flash",
        SECURE_CERT_OFFSET, backup_file
    ]
    res = subprocess.run(write_cmd)
    if res.returncode != 0:
        print("[ERROR] esp_secure_cert restore FAILED!")
        sys.exit(1)
    print("\n================== RESTORE COMPLETE ==================\n")


def resolve_port(port):
    if "*" in port or "?" in port:
        import glob
        matched = sorted(glob.glob(port))
        if not matched:
            print(f"[ERROR] No serial port matches wildcard: {port}")
            sys.exit(1)
        if len(matched) > 1:
            print(f"[ERROR] Multiple serial ports match {port}: {', '.join(matched)}")
            print("Specify one exact port to avoid operating on the wrong device.")
            sys.exit(1)
        resolved = matched[0]
        print(f"--- Resolved wildcard port '{port}' to: {resolved} ---")
        return resolved
    return port


def get_otadata_offset():
    # 1. Environment variable override
    offset = os.environ.get("WXM_OTADATA_OFFSET")
    if offset:
        return offset

    # 2. Parse from platformio.ini and matching partitions CSV
    if os.path.exists("platformio.ini"):
        try:
            with open("platformio.ini", "r") as f:
                content = f.read()
            m = re.search(r"default_envs\s*=\s*(\S+)", content)
            default_env = m.group(1) if m else "wg1200_release"

            if "wg1200" in default_env or "lilygo" in default_env:
                partitions_csv = "partitions_16MB_wxm.csv"
            elif "xiao" in default_env:
                partitions_csv = "partitions_8MB_wxm.csv"
            else:
                partitions_csv = "partitions_16MB_wxm.csv"

            if os.path.exists(partitions_csv):
                with open(partitions_csv, "r") as f:
                    for line in f:
                        if line.strip().startswith("#") or not line.strip():
                            continue
                        parts = [p.strip() for p in line.split(",")]
                        if len(parts) > 3 and parts[0] == "otadata":
                            return parts[3]
        except Exception:
            pass

    return "0x13000" # fallback default for WG1200


def get_active_partition(port, speed):
    """Read the otadata partition and determine the active boot slot.

    Returns a tuple of (address, name) where address is e.g. "0x20000" and name is "factory".
    If reading or parsing fails, returns (None, None).
    """
    otadata_offset = get_otadata_offset()
    temp_otadata = "backups/secure_cert/temp_otadata.bin"
    os.makedirs(os.path.dirname(temp_otadata), exist_ok=True)

    print(f"\n--- Reading otadata partition (at {otadata_offset}) to detect the active boot slot ---")
    read_cmd = [
        get_platformio_python(),
        get_esptool_path(),
        "--no-stub",
        "--chip", "esp32s3",
        "--port", port,
        "--baud", speed,
        "--before", "default_reset",
        "--after", "no_reset",
        "read_flash",
        otadata_offset, "0x2000",
        temp_otadata,
    ]

    res = subprocess.run(read_cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if res.returncode != 0:
        print("[WARN] Reading otadata partition failed; cannot determine active boot partition.")
        return None, None

    try:
        with open(temp_otadata, "rb") as f:
            data = f.read()
    except Exception as e:
        print(f"[WARN] Failed to read temp otadata file: {e}")
        return None, None
    finally:
        if os.path.exists(temp_otadata):
            try:
                os.remove(temp_otadata)
            except OSError:
                pass

    if len(data) < 8192:
        print("[WARN] otadata partition dump is too short.")
        return None, None

    import struct
    seq0 = struct.unpack("<I", data[0:4])[0]
    seq1 = struct.unpack("<I", data[4096:4100])[0]

    print(f"--- Parsed otadata seq0: 0x{seq0:08X}, seq1: 0x{seq1:08X} ---")

    if seq0 == 0xFFFFFFFF and seq1 == 0xFFFFFFFF:
        return "0x20000", "factory"

    seq0_valid = (seq0 != 0xFFFFFFFF and seq0 > 0)
    seq1_valid = (seq1 != 0xFFFFFFFF and seq1 > 0)

    if not seq0_valid and not seq1_valid:
        return "0x20000", "factory"

    active_seq = 0
    if seq0_valid and seq1_valid:
        active_seq = max(seq0, seq1)
    elif seq0_valid:
        active_seq = seq0
    else:
        active_seq = seq1

    slot = (active_seq - 1) % 2
    if slot == 0:
        return "0x420000", "ota_0"
    else:
        return "0x820000", "ota_1"


def main():
    # Direct restore subcommand
    if "--restore" in sys.argv:
        args = [a for a in sys.argv[1:] if a != "--restore"]
        if len(args) < 2:
            print("Usage: upload_signed.py --restore <backup_file> <port> [speed]")
            sys.exit(1)
        restore_secure_cert(resolve_port(args[1]), args[2] if len(args) > 2 else "921600", args[0])
        return

    # Safe-erase mode: wraps `pio run -t erase` to back up 0xd000 first.
    if "--erase" in sys.argv:
        args = [a for a in sys.argv[1:] if a != "--erase"]
        if len(args) < 1:
            print("Usage: upload_signed.py --erase <port> [speed] [chip]")
            sys.exit(1)
        erase_flow(
            resolve_port(args[0]),
            args[1] if len(args) > 1 else "921600",
            args[2] if len(args) > 2 else "esp32s3",
        )
        return

    # Main upload flow arguments parsing
    flash_all = "--flash-all" in sys.argv or os.environ.get("WXM_FLASH_ALL") == "1"
    args = [a for a in sys.argv[1:] if a != "--flash-all"]

    if len(args) < 2:
        print("Usage: upload_signed.py <port> <speed> [binary_path] [--flash-all]")
        sys.exit(1)

    port = resolve_port(args[0])
    speed = args[1]

    target_bin = args[2] if len(args) > 2 else None

    if target_bin:
        if not os.path.exists(target_bin):
            print(f"[ERROR] Specified binary not found: {target_bin}")
            sys.exit(1)
        print(f"Using specified binary: {target_bin}")
    else:
        # 1. Parse version
        try:
            with open("main/include/app_config.h", "r") as f:
                app_config = f.read()
            version_matches = re.findall(r'#define APP_VERSION_BASE\s+"([^"]+)"', app_config)
            if not version_matches:
                version_matches = re.findall(r'#define APP_VERSION\s+"([^"]+)"', app_config)
            if not version_matches:
                version_matches = re.findall(r'#define APP_VERSION_BASE "(\d{1,2}\.\d{1,2}\.\d{1,2})"', app_config)
            if not version_matches:
                version_matches = re.findall(r'#define APP_VERSION "(\d{1,2}\.\d{1,2}\.\d{1,2})"', app_config)

            version = version_matches[0]
        except Exception as e:
            print(f"Error parsing version from app_config.h: {e}")
            sys.exit(1)

        print(f"--- Detected firmware version: {version} ---")

        # Determine which binary was built
        import glob
        release_bins = glob.glob(f".pio/build/wg1200_release/fw_release_*.bin") or glob.glob(f".pio/build/release/fw_release_*.bin")
        debug_bins = glob.glob(f".pio/build/wg1200_debug/fw_debug_*.bin") or glob.glob(f".pio/build/debug/fw_debug_*.bin")

        # Filter out signed binaries if we are looking for unsigned
        unsigned_release_bins = [b for b in release_bins if not b.endswith("-signed.bin")]
        unsigned_debug_bins = [b for b in debug_bins if not b.endswith("-signed.bin")]

        if unsigned_release_bins:
            unsigned_bin_release = unsigned_release_bins[0]
        else:
            unsigned_bin_release = f".pio/build/wg1200_release/fw_release_{version}.bin"

        if unsigned_debug_bins:
            unsigned_bin_debug = unsigned_debug_bins[0]
        else:
            unsigned_bin_debug = f".pio/build/wg1200_debug/fw_debug_{version}.bin"

        key_candidates = (
            os.path.join(REPO_ROOT, "secrets", "wg1200_secure_boot_key.pem"),
            os.path.join(REPO_ROOT, "secrets", "secure_boot_signing_key.pem"),
        )
        key_file = next((path for path in key_candidates if os.path.isfile(path)), key_candidates[0])
        has_key = os.path.exists(key_file)

        if os.path.exists(unsigned_bin_release):
            target_bin = f"signed_fw_release_{version}.bin"

            if not has_key:
                print(f"\n[ERROR] Secure Boot signing key not found at {key_file}!")
                print("Please make sure you have copied the PEM key into the 'secrets' folder.")
                sys.exit(1)

            print(f"Signing release binary: {unsigned_bin_release} -> {target_bin}")
            sign_cmd = [
                get_espsecure_path(),
                "sign-data",
                "--version", "2",
                "--keyfile", key_file,
                "--output", target_bin,
                unsigned_bin_release
            ]

            if os.path.exists(target_bin):
                os.remove(target_bin)

            res = subprocess.run(sign_cmd)
            if res.returncode != 0:
                print("[ERROR] Binary signing failed!")
                sys.exit(1)
            print("Binary signed successfully!")

        elif os.path.exists(unsigned_bin_debug):
            if has_key:
                # Since the user's primary development device is a production gateway with burned eFuses,
                # we sign the debug binary so it can boot on secure hardware!
                target_bin = f"signed_fw_debug_{version}.bin"
                print(f"Signing debug binary for secure hardware: {unsigned_bin_debug} -> {target_bin}")

                sign_cmd = [
                    get_espsecure_path(),
                    "sign-data",
                    "--version", "2",
                    "--keyfile", key_file,
                    "--output", target_bin,
                    unsigned_bin_debug
                ]

                if os.path.exists(target_bin):
                    os.remove(target_bin)

                res = subprocess.run(sign_cmd)
                if res.returncode != 0:
                    print("[ERROR] Debug binary signing failed!")
                    sys.exit(1)
                print("Debug binary signed successfully!")
            else:
                target_bin = unsigned_bin_debug
                print(f"Using unsigned debug binary (no key at {key_file}): {unsigned_bin_debug}")
                print("Note: This will only boot on unlocked/development hardware with secure boot disabled.")

        else:
            print("\n[ERROR] Compiled binary not found!")
            print(f"Expected release build at: {unsigned_bin_release}")
            print(f"Expected debug build at: {unsigned_bin_debug}")
            print("Please build the environment before uploading.")
            sys.exit(1)

    # 2.5 Back up the irreplaceable esp_secure_cert partition before flashing (off by default).
    if os.environ.get("WXM_UPLOAD_BACKUP") == "1":
        backup_secure_cert(port, speed)
    else:
        print("--- Skipping esp_secure_cert partition backup (set WXM_UPLOAD_BACKUP=1 to enable) ---")

    # Copy the signed/unsigned binary and corresponding ELF to the releases/ folder
    releases_dir = "releases"
    os.makedirs(releases_dir, exist_ok=True)
    if target_bin and os.path.exists(target_bin):
        try:
            import shutil
            import glob
            shutil.copy2(target_bin, os.path.join(releases_dir, os.path.basename(target_bin)))
            print(f"--- [Copied] {target_bin} -> {releases_dir}/ ---")

            # Find and copy matching ELF
            elf_pattern = os.path.basename(target_bin).replace('signed_fw_', 'fw_').replace('-signed', '').replace('.bin', '')
            elf_files = glob.glob(f".pio/build/*/{elf_pattern}.elf")
            if not elf_files:
                elf_files = glob.glob(f".pio/build/*/*.elf")
            for elf in elf_files:
                shutil.copy2(elf, os.path.join(releases_dir, os.path.basename(elf)))
                print(f"--- [Copied] {elf} -> {releases_dir}/ ---")
        except Exception as e:
            print(f"--- [Artifact Copy Warning] Could not copy files to releases/: {e} ---")


    # 3. Flash binary
    esptool_py = get_esptool_path()

    flash_offsets = []
    if flash_all:
        print("--- '--flash-all' flag active: Flashing ALL 3 partitions (factory, ota_0, ota_1) ---")
        flash_offsets = [("0x20000", "factory"), ("0x420000", "ota_0"), ("0x820000", "ota_1")]
    else:
        # Detect active boot slot dynamically to flash only that slot
        addr, name = get_active_partition(port, speed)
        if addr and name:
            print(f"--- Detected active boot partition: {name} ({addr}) ---")
            print(f"--- Flashing ONLY active partition ({name} at {addr}) ---")
            flash_offsets = [(addr, name)]
        else:
            print("--- Active boot slot detection fallback: Flashing single default partition (factory at 0x20000) ---")
            flash_offsets = [("0x20000", "factory")]

    print(f"Flashing {target_bin} to device on port {port} at {speed} baud...")

    flash_args = []
    for addr, name in flash_offsets:
        flash_args.extend([addr, target_bin])

    flash_cmd = [
        get_platformio_python(),
        esptool_py,
        "--no-stub",
        "--chip", "esp32s3",
        "--port", port,
        "--baud", speed,
        "--before", "default_reset",
        "--after", "hard_reset",
        "write_flash",
        "--flash_mode", "dio",
        "--flash_freq", "80m",
        "--flash_size", "16MB",
    ] + flash_args
    
    res = subprocess.run(flash_cmd)
    if res.returncode != 0:
        print("[ERROR] Flashing failed!")
        sys.exit(1)
        
    print("\n================== UPLOAD SUCCESSFUL ==================\n")


if __name__ == "__main__":
    main()
