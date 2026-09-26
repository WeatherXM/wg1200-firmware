#!/usr/bin/env python3
"""
WG1200 Long-Term Stability Logger & Serial Monitor
=================================================
This utility is designed for long-term stability testing of the weather station
firmware (e.g., running for days to diagnose connection drops). It reads the
device's serial interface, injects microsecond-accurate wall-clock timestamps,
and writes to a hybrid logging system:
  1. Rotating Raw Logs: Keeps full history without filling up the disk.
  2. Incidents Log: Captures only significant events (disconnects, reboots, crashes)
     with contextual pre-event and post-event logs for easy debugging.

Requirements:
    pip install pyserial

Usage:
    python tools/long_term_stability_logger.py [--port /dev/cu.usbserial-112210] [--baud 115200]
    python tools/long_term_stability_logger.py --test  # Simulate crash and reconnection sequences
"""

import sys
import os
import time
import glob
import re
import argparse
import collections
from datetime import datetime

# Check for pyserial dependency
try:
    import serial
except ImportError:
    print("\033[31mError: 'pyserial' is not installed.\033[0m")
    print("Please install it using your active python environment:")
    print("    pip install pyserial")
    print("\nStarting in simulated TEST mode instead...")
    # We will automatically set sys.argv to run the test if serial is missing,
    # but let's let the script's normal flow handle it if argparse is executed.

# ANSI Color Codes
COLOR_RESET = "\033[0m"
COLOR_BOLD = "\033[1m"
COLOR_RED = "\033[31m"      # Errors / Crashes
COLOR_YELLOW = "\033[33m"   # Warnings
COLOR_GREEN = "\033[32m"    # Connections / Success
COLOR_CYAN = "\033[36m"     # Boots / Events
COLOR_MAGENTA = "\033[35m"  # Timestamps injected by host
COLOR_GRAY = "\033[90m"     # Normal verbose/info logs
COLOR_WHITE = "\033[37m"

class LogRotator:
    """Manages rotating raw files to prevent out-of-space issues."""
    def __init__(self, base_path, max_bytes=20 * 1024 * 1024, backup_count=10):
        self.base_path = base_path
        self.max_bytes = max_bytes
        self.backup_count = backup_count
        self.current_file = None
        self.current_bytes = 0
        self._open_new_file()

    def _open_new_file(self):
        if self.current_file:
            self.current_file.close()

        # Find a clean filename or rotate existing
        if os.path.exists(self.base_path):
            for i in range(self.backup_count - 1, 0, -1):
                old_file = f"{self.base_path}.{i}"
                new_file = f"{self.base_path}.{i+1}"
                if os.path.exists(old_file):
                    if os.path.exists(new_file):
                        os.remove(new_file)
                    os.rename(old_file, new_file)
            os.rename(self.base_path, f"{self.base_path}.1")

        self.current_file = open(self.base_path, "a", encoding="utf-8", errors="replace")
        self.current_bytes = os.path.getsize(self.base_path) if os.path.exists(self.base_path) else 0

    def write(self, data):
        data_bytes = data.encode("utf-8", errors="replace")
        if self.current_bytes + len(data_bytes) > self.max_bytes:
            self._open_new_file()
        self.current_file.write(data)
        self.current_file.flush()
        self.current_bytes += len(data_bytes)

    def close(self):
        if self.current_file:
            self.current_file.close()


class StabilityMonitor:
    def __init__(self, port, baud, log_dir, max_log_size=20*1024*1024, max_backups=5, reset_on_connect=True):
        self.port = port
        self.baud = baud
        self.log_dir = log_dir
        self.max_log_size = max_log_size
        self.max_backups = max_backups
        self.reset_on_connect = reset_on_connect
        
        # Ensure log directory exists
        os.makedirs(self.log_dir, exist_ok=True)
        
        # Files setup
        timestamp_str = datetime.now().strftime("%Y%m%d_%H%M%S")
        self.raw_log_path = os.path.join(self.log_dir, f"raw_stability_{timestamp_str}.log")
        self.incidents_log_path = os.path.join(self.log_dir, f"incidents_{timestamp_str}.log")
        
        self.raw_rotator = LogRotator(self.raw_log_path, max_bytes=self.max_log_size, backup_count=self.max_backups)
        self.incidents_file = open(self.incidents_log_path, "a", encoding="utf-8", errors="replace")
        
        # Circular buffer for context logs (preceding 10 lines)
        self.context_buffer = collections.deque(maxlen=15)
        
        # State tracking
        self.last_lines_since_incident = 0
        self.post_incident_buffer = []
        self.active_incident = None
        self.active_incident_ts = None
        self.active_incident_trigger_line = ""
        
        # Incident compilation pattern matchers
        self.rules = [
            # OTA / Firmware Updates
            {"pattern": r"OTA|esp_ota|fw_checksum|fw_size|ota_verdict|ota_bytes|ota_chunk|OTA_TB|writing slot|updating firmware|partition", "tag": "OTA_EVENT", "color": COLOR_CYAN},
            {"pattern": r"v0\.1\.9|v0\.8\.2|0\.1\.9|0\.8\.2|current_fw_version|firmware version|FW Version|build date", "tag": "VERSION_INFO", "color": COLOR_GREEN},
            
            # Crash / Reset / Boot
            {"pattern": r"Guru Meditation Error|Backtrace:|Panic|LoadProhibited", "tag": "CORE_CRASH", "color": COLOR_RED},
            {"pattern": r"rst:0x|SPI Speed|boot:|esp_restart", "tag": "SYSTEM_REBOOT", "color": COLOR_CYAN},
            {"pattern": r"watchdog|WDT|Task watchdog", "tag": "WATCHDOG_RESET", "color": COLOR_RED},
            
            # WiFi Drops
            {"pattern": r"WIFI_DISCONNECTED|wifi disconnect|print_disconnect_reason|reason:", "tag": "WIFI_DISCONNECTED", "color": COLOR_YELLOW},
            {"pattern": r"WIFI_CONNECTION_FAILED", "tag": "WIFI_CONNECT_FAILED", "color": COLOR_YELLOW},
            
            # ThingsBoard Drops
            {"pattern": r"MQTT disconnected|Connection FAILED|TB downtime", "tag": "CLOUD_DISCONNECTED", "color": COLOR_YELLOW},
            {"pattern": r"MQTT Error|esp-tls: -|Transport socket errno", "tag": "CLOUD_NETWORK_ERROR", "color": COLOR_RED},
            
            # Standard Errors
            {"pattern": r"\[E\]|ESP_LOGE|ERROR|Assertion failed", "tag": "CRITICAL_ERROR", "color": COLOR_RED},
        ]

    def _format_line(self, line):
        """Analyze line contents to apply appropriate terminal coloring."""
        stripped = line.strip()
        # Look for custom matches
        for rule in self.rules:
            if re.search(rule["pattern"], stripped, re.IGNORECASE):
                return f"{rule['color']}{line}{COLOR_RESET}"
        
        # Default highlights
        if " [W] " in stripped or "ESP_LOGW" in stripped or "WARNING" in stripped:
            return f"{COLOR_YELLOW}{line}{COLOR_RESET}"
        elif " [I] " in stripped or "ESP_LOGI" in stripped or "INFO" in stripped:
            if "SUCCESS" in stripped or "connected" in stripped.lower() or "got ip" in stripped.lower():
                return f"{COLOR_GREEN}{line}{COLOR_RESET}"
            return f"{COLOR_WHITE}{line}{COLOR_RESET}"
        elif " [D] " in stripped or "ESP_LOGD" in stripped or "DEBUG" in stripped:
            return f"{COLOR_GRAY}{line}{COLOR_RESET}"
        
        return line

    def log_incident(self, tag, trigger_line):
        """Start tracking an incident and write context to the incident log."""
        now_str = datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]
        
        self.active_incident = tag
        self.active_incident_ts = now_str
        self.active_incident_trigger_line = trigger_line
        self.last_lines_since_incident = 5  # Capture 5 lines after the incident

        header = f"\n{'='*80}\n[{now_str}] INCIDENT DETECTED: {tag}\n{'-'*80}\n"
        self.incidents_file.write(header)
        self.incidents_file.write("--- CONTEXT BEFORE INCIDENT ---\n")
        for ctx_line in self.context_buffer:
            self.incidents_file.write(ctx_line)
        self.incidents_file.write(f"==> TRIGGER LINE: {trigger_line}")
        self.incidents_file.write("--- CONTEXT AFTER INCIDENT ---\n")
        self.incidents_file.flush()

    def process_line(self, raw_line):
        """Processes a single line from the serial stream."""
        now_str = datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]
        formatted_raw_line = raw_line.rstrip('\r\n')
        
        # Prepend PC microsecond-accurate wall-clock timestamp
        timestamped_line = f"[{now_str}] {formatted_raw_line}\n"
        
        # 1. Unconditionally write to raw rotator
        self.raw_rotator.write(timestamped_line)
        
        # 2. Check for incident triggers
        matched_tag = None
        for rule in self.rules:
            if re.search(rule["pattern"], formatted_raw_line, re.IGNORECASE):
                # Avoid triggering nested incidents immediately
                if self.active_incident is None or self.last_lines_since_incident == 0:
                    matched_tag = rule["tag"]
                    break

        if matched_tag:
            self.log_incident(matched_tag, timestamped_line)
        elif self.active_incident is not None and self.last_lines_since_incident > 0:
            # We are in the post-incident capture phase
            self.incidents_file.write(timestamped_line)
            self.last_lines_since_incident -= 1
            if self.last_lines_since_incident == 0:
                self.incidents_file.write(f"{'='*80}\n")
                self.incidents_file.flush()
                self.active_incident = None

        # Add line to context rolling buffer
        self.context_buffer.append(timestamped_line)
        
        # 3. Print colorized line to stdout
        colored_line = self._format_line(timestamped_line)
        # Prepend the injected timestamp in magenta, rest of the line formatted
        ts_part = f"{COLOR_MAGENTA}[{now_str}]{COLOR_RESET} "
        payload_part = colored_line[len(now_str) + 3:] # skip the '[timestamp] ' part of the colorized line
        sys.stdout.write(ts_part + payload_part)
        sys.stdout.flush()

    def run(self):
        """Core loop reading from serial port."""
        print(f"{COLOR_BOLD}{COLOR_GREEN}========================================================================")
        print(f"WG Long-Term Stability Logger & Parser Initialized")
        print(f"========================================================================{COLOR_RESET}")
        print(f"Target Port:          {COLOR_CYAN}{self.port}{COLOR_RESET}")
        print(f"Baud Rate:            {COLOR_CYAN}{self.baud}{COLOR_RESET}")
        print(f"Raw Rotating Log:     {COLOR_CYAN}{self.raw_log_path}{COLOR_RESET}")
        print(f"Incident Event Log:   {COLOR_CYAN}{self.incidents_log_path}{COLOR_RESET}")
        print(f"------------------------------------------------------------------------")
        print(f"Press {COLOR_YELLOW}Ctrl+C{COLOR_RESET} to safely terminate monitoring at any time.")
        print(f"------------------------------------------------------------------------\n")
        
        try:
            ser = serial.Serial(
                port=self.port,
                baudrate=self.baud,
                bytesize=serial.EIGHTBITS,
                parity=serial.PARITY_NONE,
                stopbits=serial.STOPBITS_ONE,
                timeout=1.0,
                rtscts=False,
                dsrdtr=False
            )
        except Exception as e:
            print(f"{COLOR_RED}{COLOR_BOLD}Error opening serial port {self.port}:{COLOR_RESET} {e}")
            print("Please make sure the device is connected and the port is not used by another program (e.g. PlatformIO Monitor, Screen, Arduino).")
            return

        if self.reset_on_connect:
            # Toggle DTR/RTS to reset board on connection (standard ESP32 behaviour)
            ser.dtr = False
            ser.rts = False
            time.sleep(0.1)
            ser.dtr = True
            ser.rts = True
        
        # Clear buffer
        ser.reset_input_buffer()
        
        buffer = bytearray()
        try:
            while True:
                try:
                    data = ser.read(ser.in_waiting or 1)
                    if data:
                        buffer.extend(data)
                        while b'\n' in buffer:
                            line_bytes, _, remaining = buffer.partition(b'\n')
                            buffer = remaining
                            try:
                                line = line_bytes.decode('utf-8', errors='replace')
                                self.process_line(line)
                            except Exception as decode_err:
                                # Fallback for decoding errors
                                self.process_line(f"[DECODE ERROR] Raw: {line_bytes!r}")
                except (serial.SerialException, OSError) as port_err:
                    print(f"\n{COLOR_RED}{COLOR_BOLD}Serial connection lost!{COLOR_RESET} {port_err}")
                    print("Attempting to reconnect in 1 second...")
                    ser.close()
                    time.sleep(1)
                    while True:
                        try:
                            ser.open()
                            ser.reset_input_buffer()
                            print(f"{COLOR_GREEN}Successfully reconnected to serial port!{COLOR_RESET}")
                            break
                        except Exception:
                            time.sleep(1)
                
                # Tiny sleep to reduce CPU usage when port is idle
                time.sleep(0.005)
                
        except KeyboardInterrupt:
            print(f"\n{COLOR_YELLOW}Logging session manually stopped by user.{COLOR_RESET}")
        finally:
            ser.close()
            self.raw_rotator.close()
            self.incidents_file.close()
            print(f"Logs successfully flushed and saved to: {COLOR_CYAN}{self.log_dir}{COLOR_RESET}")


def run_simulation():
    """Runs a complete test simulation to verify script functionality."""
    print(f"{COLOR_CYAN}{COLOR_BOLD}=== Stability Logger TEST MODE Simulation ==={COLOR_RESET}")
    sim_dir = "./logs_test"
    monitor = StabilityMonitor(port="SIMULATED", baud=115200, log_dir=sim_dir, max_log_size=2000, max_backups=2)
    
    simulated_stream = [
        "rst:0x1 (POWERON_RESET),boot:0x13 (SPI_FAST_FLASH_BOOT)",
        "SPI Speed      : 80MHz",
        "SPI Mode       : DIO",
        "SPI Flash Size : 16MB",
        "[I] [Main] Main: NVS init",
        "[I] [Main] Initializing WiFi...",
        "[D] [WiFi] Scan start...",
        "[I] [WiFi] Found target SSID 'MyHome_WiFi' (RSSI: -65dB)",
        "[I] [WiFi] Connecting to MyHome_WiFi...",
        "[I] [WiFi] WiFi connected, got IP: 192.168.1.104",
        "[I] [TB] Host: mqtt.weatherxm.com:8883",
        "[I] [TB] Connecting to ThingsBoard broker...",
        "[W] [TB] Connection SUCCESS",
        "[I] [Submit] Successfully submitted weather data telemetry packet #1",
        "[I] [Submit] Successfully submitted weather data telemetry packet #2",
        "[I] [Submit] Successfully submitted weather data telemetry packet #3",
        "[W] [WiFi] WIFI_DISCONNECTED, reason: 8 (WIFI_REASON_ASSOC_EXPIRE)",
        "[W] [WiFi] print_disconnect_reason: ASSOCIATION EXPIRED (8)",
        "[W] [TB] MQTT disconnected",
        "[W] [TB] Connection FAILED",
        "[W] [TB] Connection FAILED",
        "[I] [WiFi] Reconnecting to MyHome_WiFi...",
        "[I] [WiFi] WiFi connected, got IP: 192.168.1.104",
        "[I] [TB] Connecting to ThingsBoard broker...",
        "[W] [TB] Connection SUCCESS",
        "[I] [Submit] Successfully submitted weather data telemetry packet #4",
        "[E] [Main] Assertion failed in main.cpp:214: heap_caps_check_integrity_all()",
        "Guru Meditation Error: Core  0 panic'ed (Unhandled debug exception)",
        "Backtrace:0x40081234:0x3ffb5120 0x40081290:0x3ffb5140 0x400d4567:0x3ffb5160",
        "Rebooting...",
        "rst:0xc (SW_CPU_RESET),boot:0x13 (SPI_FAST_FLASH_BOOT)",
        "SPI Speed      : 80MHz",
        "[I] [Main] Main: NVS init",
        "[I] [WiFi] Connecting to MyHome_WiFi...",
        "[I] [WiFi] WiFi connected, got IP: 192.168.1.104",
        "[W] [TB] Connection SUCCESS",
    ]

    for line in simulated_stream:
        monitor.process_line(line)
        time.sleep(0.2)
        
    monitor.raw_rotator.close()
    monitor.incidents_file.close()
    
    print(f"\n{COLOR_GREEN}{COLOR_BOLD}Simulation Complete!{COLOR_RESET}")
    print(f"Check raw log at:       {COLOR_CYAN}{monitor.raw_log_path}{COLOR_RESET}")
    print(f"Check incidents log at: {COLOR_CYAN}{monitor.incidents_log_path}{COLOR_RESET}")
    
    # Let's show contents of incidents log to verify
    print(f"\n{COLOR_BOLD}--- Incidents Log Contents ---{COLOR_RESET}")
    with open(monitor.incidents_log_path, "r") as f:
        print(f.read())
    print(f"{COLOR_BOLD}------------------------------{COLOR_RESET}")


def autodetect_ports():
    """Autodetect macOS usb serial or usbmodem devices."""
    patterns = [
        "/dev/cu.usbserial-*",
        "/dev/cu.usbmodem*",
        "/dev/cu.SLAB_USBtoUART*",
        "/dev/ttyUSB*",
        "/dev/ttyACM*"
    ]
    ports = []
    for pattern in patterns:
        ports.extend(glob.glob(pattern))
    return sorted(list(set(ports)))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="WG1200 Long-Term Stability Logger")
    parser.add_argument("--port", help="Serial port (e.g. /dev/cu.usbserial-112210). Auto-detects if omitted.")
    parser.add_argument("--baud", type=int, default=115200, help="Baud rate (default: 115200)")
    parser.add_argument("--log-dir", default="./logs", help="Directory to save logs (default: ./logs)")
    parser.add_argument("--no-reset", action="store_true", help="Do not toggle DTR/RTS to reset board on connection")
    parser.add_argument("--test", action="store_true", help="Run simulated test mode to verify parser")
    
    args = parser.parse_args()
    
    if args.test:
        run_simulation()
        sys.exit(0)
        
    port = args.port
    if not port:
        detected_ports = autodetect_ports()
        if not detected_ports:
            print(f"{COLOR_YELLOW}No USB-serial ports autodetected.{COLOR_RESET}")
            print("Please specify the port explicitly with --port, or verify the device is connected.")
            print("Supported macOS pattern examples: /dev/cu.usbserial-*, /dev/cu.usbmodem*")
            sys.exit(1)
        
        if len(detected_ports) == 1:
            port = detected_ports[0]
            print(f"Autodetected single port: {COLOR_GREEN}{port}{COLOR_RESET}")
        else:
            print("\nMultiple ports detected. Please choose one:")
            for idx, p in enumerate(detected_ports):
                print(f"  [{idx}] {p}")
            try:
                choice = input(f"Select port [0-{len(detected_ports)-1}]: ")
                port = detected_ports[int(choice)]
            except (KeyboardInterrupt, ValueError, IndexError):
                print("\nNo port selected. Exiting.")
                sys.exit(1)
                
    monitor = StabilityMonitor(port=port, baud=args.baud, log_dir=args.log_dir, reset_on_connect=not args.no_reset)
    monitor.run()
