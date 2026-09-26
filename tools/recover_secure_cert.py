#!/usr/bin/env python3
"""Restore the WG1200 factory certificate partition from a verified backup."""
import sys
import os
import subprocess
import glob
import datetime

SECURE_CERT_OFFSET = "0xd000"
SECURE_CERT_SIZE = 0x2000
TLV_MAGIC = b"\x11\xba\x5e\xba"
REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FIRMWARE_DIR = os.path.join(REPO_ROOT, "wg1200-firmware")
PLATFORMIO_HOME = os.path.expanduser(
    os.environ.get("PLATFORMIO_CORE_DIR", os.path.join("~", ".platformio"))
)

def get_esptool_path():
    candidates = (
        os.path.join(PLATFORMIO_HOME, "packages", "tool-esptoolpy", "esptool.py"),
        os.path.join(PLATFORMIO_HOME, "packages", "tool-esptoolpy@1.40501.0", "esptool.py"),
    )
    for candidate in candidates:
        if os.path.isfile(candidate):
            return candidate
    return candidates[0]

def get_platformio_python():
    candidate = os.path.join(PLATFORMIO_HOME, "penv", "bin", "python")
    return candidate if os.path.isfile(candidate) else sys.executable

def find_serial_ports():
    # Find all matching /dev/cu.usbmodem* or /dev/cu.usbserial* ports
    ports = glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/cu.usbserial*")
    return ports

def format_timestamp(timestamp):
    try:
        return datetime.datetime.fromtimestamp(timestamp).strftime("%Y-%m-%d %H:%M:%S")
    except Exception:
        return "Unknown Date"

def scan_backups():
    """Scan standard locations for secure_cert backup files."""
    backups = []

    # Canonical upload backups live under the firmware workspace. Retain the
    # former locations so older backups remain discoverable after consolidation.
    backup_dirs = [
        os.path.join(FIRMWARE_DIR, "backups", "secure_cert"),
        os.path.join(REPO_ROOT, "backups", "secure_cert"),
        os.path.join(os.getcwd(), "backups", "secure_cert"),
    ]
    seen = set()
    for backup_dir in backup_dirs:
        files = glob.glob(os.path.join(backup_dir, "*.bin"))
        for f in files:
            f = os.path.abspath(f)
            if f in seen:
                continue
            seen.add(f)
            mtime = os.path.getmtime(f)
            backups.append({
                "path": f,
                "name": os.path.basename(f),
                "mtime": mtime,
                "size": os.path.getsize(f),
                "type": "Backup Folder"
            })
            
    # Also retain support for the original one-off dump name.
    root_dump = os.path.join(REPO_ROOT, "secure_cert_dump.bin")
    if os.path.exists(root_dump):
        mtime = os.path.getmtime(root_dump)
        backups.append({
            "path": root_dump,
            "name": "secure_cert_dump.bin",
            "mtime": mtime,
            "size": os.path.getsize(root_dump),
            "type": "Root Workspace"
        })
        
    # Sort backups by modification time (newest first)
    backups.sort(key=lambda x: x["mtime"], reverse=True)
    return backups

def main():
    print("====================================================")
    print("      WeatherXM Gateway Secure Certificate Recovery ")
    print("====================================================\n")

    backup_file = None
    selected_port = None
    baud_rate = "2000000"

    # Quick CLI Argument Support
    if len(sys.argv) > 1:
        if sys.argv[1] in ("-h", "--help"):
            print("Usage:")
            print("  python3 tools/recover_secure_cert.py                      (Interactive Wizard)")
            print("  python3 tools/recover_secure_cert.py <backup_file>        (Specify backup file)")
            print("  python3 tools/recover_secure_cert.py <backup_file> <port> (Specify backup and port)")
            sys.exit(0)
        
        arg1 = sys.argv[1]
        # Check if arg1 is a port or file
        if os.path.exists(arg1):
            backup_file = arg1
            if len(sys.argv) > 2:
                selected_port = sys.argv[2]
        else:
            # It might be a port, or it might be a non-existent file
            if arg1.startswith("/dev/"):
                selected_port = arg1
            else:
                print(f"[ERROR] Specified backup file does not exist: {arg1}")
                sys.exit(1)

    # Interactive Scan if backup file not specified
    if not backup_file:
        backups = scan_backups()
        if not backups:
            print("[ERROR] No secure_cert backup files found!")
            print("Please run an upload or erase first to automatically generate a backup,")
            print("or place your backup binary in the workspace root as 'secure_cert_dump.bin'.")
            sys.exit(1)
        
        print("[*] Found the following secure certificate backups:")
        for idx, b in enumerate(backups):
            date_str = format_timestamp(b["mtime"])
            print(f"  {idx + 1:2d}. {b['name']}  ({b['size']} bytes) - {date_str} [{b['type']}]")
            
        print("")
        while True:
            try:
                choice = input(f"Select a backup to restore (1-{len(backups)}) [1]: ").strip()
                if not choice:
                    backup_file = backups[0]["path"]
                    break
                idx = int(choice) - 1
                if 0 <= idx < len(backups):
                    backup_file = backups[idx]["path"]
                    break
                else:
                    print(f"Please enter a number between 1 and {len(backups)}.")
            except ValueError:
                print("Invalid input. Please enter a valid number.")
            except (KeyboardInterrupt, EOFError):
                print("\nOperation cancelled.")
                sys.exit(0)

    print(f"\n[*] Selected backup file: {os.path.abspath(backup_file)}")

    try:
        with open(backup_file, "rb") as stream:
            backup_data = stream.read()
    except OSError as exc:
        print(f"[ERROR] Could not read backup: {exc}")
        sys.exit(1)
    if len(backup_data) != SECURE_CERT_SIZE:
        print(f"[ERROR] Refusing restore: expected {SECURE_CERT_SIZE} bytes, got {len(backup_data)}.")
        sys.exit(1)
    if TLV_MAGIC not in backup_data or all(byte == 0xff for byte in backup_data):
        print("[ERROR] Refusing restore: backup does not contain a valid secure-cert TLV image.")
        sys.exit(1)
    print("[*] Backup size and secure-cert TLV magic validated.")

    # Detect port if not specified
    if not selected_port:
        ports = find_serial_ports()
        if len(ports) == 1:
            selected_port = ports[0]
            print(f"[*] Auto-detected port: {selected_port}")
        elif len(ports) > 1:
            print("[!] Multiple serial ports detected:")
            for idx, port in enumerate(ports):
                # Prioritize the native JTAG usbmodem port
                is_preferred = "usbmodem" in port
                pref_str = " (Preferred)" if is_preferred else ""
                print(f"  {idx + 1}. {port}{pref_str}")
                if is_preferred and not selected_port:
                    selected_port = port
            
            # Offer selection if no default/preferred port, or if user wants to override
            print("")
            while True:
                try:
                    default_idx = ports.index(selected_port) + 1 if selected_port in ports else 1
                    choice = input(f"Select serial port (1-{len(ports)}) [{default_idx}]: ").strip()
                    if not choice:
                        if not selected_port:
                            selected_port = ports[0]
                        break
                    idx = int(choice) - 1
                    if 0 <= idx < len(ports):
                        selected_port = ports[idx]
                        break
                    else:
                        print(f"Please enter a number between 1 and {len(ports)}.")
                except ValueError:
                    print("Invalid input. Please enter a valid number.")
                except (KeyboardInterrupt, EOFError):
                    print("\nOperation cancelled.")
                    sys.exit(0)
        else:
            print("[ERROR] No connected ESP32-S3 devices found (tried /dev/cu.usbmodem* and /dev/cu.usbserial*)!")
            print("Please connect your device to your computer via USB.")
            sys.exit(1)

    esptool_path = get_esptool_path()
    python_path = get_platformio_python()
    
    if not os.path.exists(esptool_path):
        print(f"[ERROR] esptool.py not found at: {esptool_path}")
        sys.exit(1)
        
    flash_offset = SECURE_CERT_OFFSET
    
    cmd = [
        python_path,
        esptool_path,
        "--no-stub",
        "--chip", "esp32s3",
        "--port", selected_port,
        "--baud", baud_rate,
        "--before", "default_reset",
        "--after", "hard_reset",
        "write_flash",
        flash_offset,
        backup_file
    ]
    
    print("\n[*] Preparing to restore secure cert partition...")
    print(f"[*] Port:   {selected_port}")
    print(f"[*] Offset: {flash_offset} (esp_secure_cert partition)")
    print(f"[*] Cmd:    {' '.join(cmd)}")
    print("\n----------------------------------------------------")
    
    try:
        res = subprocess.run(cmd, check=True)
        if res.returncode == 0:
            print("----------------------------------------------------")
            print("\n[SUCCESS] Original secure certificate partition restored successfully!")
            print("[*] Your device's factory credentials are now fully recovered.")
            print("[*] Reminder: Make sure to compile your firmware with physical eFuses enabled")
            print("    (-DCONFIG_WXM_PRODUCTION_BURN_EFUSES=1) to use these recovered credentials.\n")
    except subprocess.CalledProcessError as e:
        print("----------------------------------------------------")
        print(f"\n[ERROR] Flashing failed with return code {e.returncode}!")
        sys.exit(1)

if __name__ == "__main__":
    main()
