#!/usr/bin/env python3
"""
WeatherXM nRF52 KISS Modem Tester & Packet Decoder
==================================================
This utility allows direct diagnostic testing of the Seeed Studio XIAO nRF52 coprocessor 
running the Meshcore KISS firmware. By connecting the nRF52 directly via USB to your laptop,
you can initialize the modem, configure the LoRa transceiver settings, and capture and decode 
WS1001 and WS1300 weather station packets in real-time.

Requirements:
    pip install pyserial

Usage:
    python tools/kiss_modem_tester.py [--port /dev/cu.usbmodem12201] [--freq 868100000]
"""

import sys
import time
import glob
import re
import struct
import argparse

try:
    import serial
except ImportError:
    print("Error: 'pyserial' is not installed. Please install it using:")
    print("    pip install pyserial")
    sys.exit(1)

# KISS Framing Special Bytes
KISS_FEND  = 0xC0
KISS_FESC  = 0xDB
KISS_TFEND = 0xDC
KISS_TFESC = 0xDD

KISS_CMD_DATA        = 0x00
KISS_CMD_SETHARDWARE = 0x06

HW_CMD_SET_RADIO         = 0x09
HW_CMD_SET_SIGNAL_REPORT = 0x19
HW_CMD_GET_SENSORS       = 0x15
HW_CMD_SET_PREAMBLE      = 0x20

HW_RESP_OK               = 0xF0
HW_RESP_RX_META          = 0xF9

# --- HELPER FUNCTIONS FOR DECODING ---

def calc_crc(buff, polynomial=0x31, init=0):
    remainder = init
    for b in buff:
        remainder ^= b
        for _ in range(8):
            if remainder & 0x80:
                remainder = ((remainder << 1) ^ polynomial) & 0xFF
            else:
                remainder = (remainder << 1) & 0xFF
    return remainder

def calc_checksum(buff):
    return sum(buff) & 0xFF

# --- WS1001 DECODER ---
def decode_ws1001(pkt_bytes):
    try:
        pl_size = pkt_bytes[0]
        if pl_size > len(pkt_bytes):
            return {"error": f"Invalid payload size: {pl_size} > {len(pkt_bytes)}"}
            
        payload = pkt_bytes[:pl_size]
        signature = pkt_bytes[pl_size:]
        
        expected_crc = pkt_bytes[25] if len(pkt_bytes) > 25 else 0
        calced_crc = calc_crc(pkt_bytes[:25]) if len(pkt_bytes) >= 25 else 0
        
        expected_chk = pkt_bytes[26] if len(pkt_bytes) > 26 else 0
        calced_chk = calc_checksum(pkt_bytes[:26]) if len(pkt_bytes) >= 26 else 0
        
        fw_ver = pkt_bytes[1]
        station_code = pkt_bytes[2]
        station_id = (pkt_bytes[3] << 24) | (pkt_bytes[4] << 16) | (pkt_bytes[5] << 8) | pkt_bytes[6]
        pkt_count = (pkt_bytes[7] << 8) | pkt_bytes[8]
        tx_power = pkt_bytes[9] & 0x7F
        gps_fixing = (pkt_bytes[9] & 0x80) >> 7
        rssi = pkt_bytes[10] - 137
        bat_mv = pkt_bytes[11] * 20
        bat_low = bat_mv < 2600
        tx_interval = pkt_bytes[13] * 10

        # Temp
        temp_raw = (pkt_bytes[14] << 8) | pkt_bytes[15]
        temp_raw = temp_raw >> 4
        if temp_raw == 0x07FF:
            temp_c = None
        elif temp_raw & 0x0800:
            temp_f = (temp_raw | 0xF000) / 10.0
            temp_c = (temp_f - 32.0) * 5.0 / 9.0
        else:
            temp_f = temp_raw / 10.0
            temp_c = (temp_f - 32.0) * 5.0 / 9.0

        # Humidity
        hum_raw = ((pkt_bytes[15] << 8) | pkt_bytes[16]) & 0x03FF
        hum = hum_raw / 10.0

        # Wind Speed
        wind_speed_raw = pkt_bytes[17]
        wind_speed_raw = ((pkt_bytes[15] & 0x08) << 5) | wind_speed_raw
        wind_speed = wind_speed_raw / 10.0

        # Wind Gust
        wind_gust_raw = pkt_bytes[18]
        wind_gust_raw = ((pkt_bytes[15] & 0x04) << 6) | wind_gust_raw
        wind_gust = wind_gust_raw / 10.0

        # Wind Direction
        wind_dir = pkt_bytes[19] | ((pkt_bytes[20] & 0x80) << 1)

        # Illuminance
        illum = ((pkt_bytes[20] & 0x7F) << 8) | pkt_bytes[21]
        illum_val = illum * 10
        solar_rad = illum_val * 0.007892

        # UV Index
        uv = pkt_bytes[22] * 0.1

        # Precipitation
        precip = ((pkt_bytes[23] << 8) | pkt_bytes[24]) * 0.254

        return {
            "station_type": "WS1001",
            "station_id": station_id,
            "packet_count": pkt_count,
            "fw_version": fw_ver,
            "battery_mv": bat_mv,
            "battery_low": bat_low,
            "tx_power_dbm": tx_power,
            "gps_fixing": bool(gps_fixing),
            "temp_c": round(temp_c, 2) if temp_c is not None else None,
            "humidity_pct": hum,
            "wind_speed_ms": wind_speed,
            "wind_gust_ms": wind_gust,
            "wind_direction_deg": wind_dir,
            "illuminance_lux": illum_val,
            "solar_radiation_wm2": round(solar_rad, 2),
            "uv_index": uv,
            "precipitation_mm": round(precip, 2),
            "crc_ok": expected_crc == calced_crc,
            "chk_ok": expected_chk == calced_chk,
            "signature": signature.hex()
        }
    except Exception as e:
        return {"error": f"WS1001 Parse exception: {e}"}

# --- WS1300 DECODER ---
def decode_ws1300(pkt_bytes):
    try:
        model_id = pkt_bytes[0]
        fw_ver = pkt_bytes[1]
        hw_ver = pkt_bytes[2]
        packet_type_val = pkt_bytes[3]
        
        station_id = struct.unpack(">I", pkt_bytes[4:8])[0]
        packet_id = struct.unpack(">H", pkt_bytes[8:10])[0]
        last_resp_rssi = pkt_bytes[10] - 137
        tx_power = pkt_bytes[11] & 0x1F
        tx_interval = pkt_bytes[12] * 10
        bat_mv = pkt_bytes[13] * 20
        
        aux2 = pkt_bytes[14]
        contains_sig = bool(aux2 & 0x01)
        acquiring_gps = bool(aux2 & 0x02)
        low_batt = bool(aux2 & 0x04)
        int_batt = bool(aux2 & 0x08)
        ext_batt = bool(aux2 & 0x10)

        ts = struct.unpack(">I", pkt_bytes[15:19])[0]
        sample_cnt = pkt_bytes[19]
        temp_raw = struct.unpack(">h", pkt_bytes[20:22])[0]
        temp = temp_raw / 10.0
        hum = pkt_bytes[22]
        wind_speed = struct.unpack(">H", pkt_bytes[23:25])[0] / 10.0
        wind_gust = struct.unpack(">H", pkt_bytes[25:27])[0] / 10.0
        wind_dir = struct.unpack(">H", pkt_bytes[27:29])[0]
        precip_cml = struct.unpack(">f", pkt_bytes[29:33])[0]
        precip_rate = struct.unpack(">H", pkt_bytes[33:35])[0] / 10.0
        uv = pkt_bytes[35]
        illum_raw = (pkt_bytes[36] << 16) | (pkt_bytes[37] << 8) | pkt_bytes[38]
        press = struct.unpack(">H", pkt_bytes[39:41])[0] / 10.0
        signature = pkt_bytes[41:]

        return {
            "station_type": "WS1300",
            "station_id": station_id,
            "packet_id": packet_id,
            "fw_version": fw_ver,
            "hw_version": hw_ver,
            "battery_mv": bat_mv,
            "battery_low": low_batt,
            "tx_power_dbm": tx_power,
            "gps_acquiring": acquiring_gps,
            "timestamp": ts,
            "sample_count": sample_cnt,
            "temp_c": temp,
            "humidity_pct": hum,
            "wind_speed_ms": wind_speed,
            "wind_gust_ms": wind_gust,
            "wind_direction_deg": wind_dir,
            "accumulated_precip_mm": precip_cml,
            "precip_rate_mm_h": precip_rate,
            "uv_index": uv,
            "illuminance_lux": illum_raw,
            "pressure_hpa": press,
            "signature": signature.hex()
        }
    except Exception as e:
        return {"error": f"WS1300 Parse exception: {e}"}

# --- KISS SERIAL ENCODER/ESCAPE ---

def escape_byte(b):
    if b == KISS_FEND:
        return [KISS_FESC, KISS_TFEND]
    elif b == KISS_FESC:
        return [KISS_FESC, KISS_TFESC]
    else:
        return [b]

def send_hardware_cmd(ser, sub_cmd, data_bytes=b""):
    payload = []
    payload.append(KISS_CMD_SETHARDWARE)
    payload.append(sub_cmd)
    for b in data_bytes:
        payload.extend(escape_byte(b))
        
    packet = [KISS_FEND]
    for b in payload:
        packet.append(b)
    packet.append(KISS_FEND)
    
    ser.write(bytes(packet))
    ser.flush()

# --- AUTO PORT DETECT ---

def auto_detect_port():
    # macOS, Linux, and Windows port glob checks
    patterns = ['/dev/cu.usbmodem*', '/dev/ttyACM*', '/dev/ttyUSB*', 'COM*']
    ports = []
    for pattern in patterns:
        ports.extend(glob.glob(pattern))
    return ports

# --- MAIN RUN LOOP ---

def main():
    parser = argparse.ArgumentParser(description="Meshcore nRF52 KISS Radio Diagnostician")
    parser.add_argument("--port", help="Serial port of connected nRF52 board")
    parser.add_argument("--freq", type=int, default=868100000, help="LoRa frequency in Hz (default: 868.1Mhz: 868100000)")
    parser.add_argument("--bw", type=int, default=125000, help="LoRa Bandwidth in Hz (default: 125000)")
    parser.add_argument("--sf", type=int, default=8, help="LoRa Spreading Factor (default: 8)")
    parser.add_argument("--cr", type=int, default=5, help="LoRa Coding Rate: 5=4/5, 6=4/6, etc. (default: 5)")
    parser.add_argument("--preamble", type=int, default=12, help="LoRa Preamble Length (default: 12)")
    args = parser.parse_args()

    # Determine Port
    port = args.port
    if not port:
        detected = auto_detect_port()
        if not detected:
            print("[-] Error: No active USB serial ports found! Please plug in the nRF52 board.")
            sys.exit(1)
        port = detected[0]
        print(f"[+] Auto-detected serial port: {port}")

    print(f"[+] Opening {port} at 115200 baud...")
    try:
        ser = serial.Serial(port, 115200, timeout=0.1)
        ser.dtr = False
        ser.rts = False
    except Exception as e:
        print(f"[-] Error: Could not open port {port}: {e}")
        sys.exit(1)

    print("[+] Initializing nRF52 radio parameters...")
    # 1. Enable RSSI/SNR signal reports on receive
    send_hardware_cmd(ser, HW_CMD_SET_SIGNAL_REPORT, bytes([1]))
    time.sleep(0.1)

    # 2. Configure RF parameters (Freq, BW, SF, CR)
    rf_data = struct.pack("<IIBB", args.freq, args.bw, args.sf, args.cr)
    send_hardware_cmd(ser, HW_CMD_SET_RADIO, rf_data)
    time.sleep(0.1)

    # 3. Configure preamble size
    send_hardware_cmd(ser, HW_CMD_SET_PREAMBLE, struct.pack("<H", args.preamble))
    time.sleep(0.1)

    print(f"\n=======================================================")
    print(f"   KISS MODEM MONITOR RUNNING (Freq: {args.freq/1e6:.1f} MHz, SF: {args.sf})")
    print(f"   Listening for raw WS1001/WS1300 radio emissions...  ")
    print(f"   Press Ctrl+C to terminate test.                     ")
    print(f"=======================================================\n")

    rx_frame = bytearray()
    rx_active = False
    rx_escaped = False

    last_rssi = 0
    last_snr = 0

    try:
        while True:
            char_in = ser.read(1)
            if not char_in:
                continue
            
            b = char_in[0]
            
            if b == KISS_FEND:
                if rx_active and len(rx_frame) > 0:
                    # Parse KISS Frame!
                    cmd = rx_frame[0]
                    
                    if cmd == KISS_CMD_DATA:
                        # Raw received packet payload
                        payload_bytes = bytes(rx_frame[1:])
                        print(f"[{time.strftime('%H:%M:%S')}] ---> Captured Frame Frame (Length: {len(payload_bytes)} bytes)")
                        print(f"  Metadata: RSSI = {last_rssi} dBm, SNR = {last_snr} dB")
                        print(f"  Raw Hex:  {payload_bytes.hex()}")
                        
                        # Inspect first byte to determine station model type
                        if len(payload_bytes) > 0:
                            model_byte = payload_bytes[0]
                            decoded_info = {}
                            if model_byte == 0x01: # WS1300
                                decoded_info = decode_ws1300(payload_bytes)
                            else: # WS1001 (payload size byte varies)
                                decoded_info = decode_ws1001(payload_bytes)
                                
                            if "error" in decoded_info:
                                print(f"  Decoded:  {decoded_info['error']}")
                            else:
                                print("  Decoded Metrics:")
                                for k, v in decoded_info.items():
                                    print(f"    - {k:22}: {v}")
                        print("-" * 55)
                        
                    elif cmd == KISS_CMD_SETHARDWARE:
                        sub_cmd = rx_frame[1]
                        if sub_cmd == HW_RESP_RX_META:
                            if len(rx_frame) >= 4:
                                last_snr = struct.unpack("b", bytes([rx_frame[2]]))[0]
                                last_rssi = struct.unpack("b", bytes([rx_frame[3]]))[0]
                                # Print debug log silently
                                # print(f"[DEBUG] SNR: {last_snr} dB, RSSI: {last_rssi} dBm")
                                
                rx_frame = bytearray()
                rx_active = True
                rx_escaped = False
                
            elif b == KISS_FESC:
                rx_escaped = True
            else:
                val = b
                if rx_escaped:
                    if b == KISS_TFEND:
                        val = KISS_FEND
                    elif b == KISS_TFESC:
                        val = KISS_FESC
                    rx_escaped = False
                if rx_active:
                    rx_frame.append(val)
                    
    except KeyboardInterrupt:
        print("\n[+] Capturing aborted by user. Exiting diagnostic test.")
    finally:
        ser.close()

if __name__ == "__main__":
    main()
