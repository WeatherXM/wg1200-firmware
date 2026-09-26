#!/usr/bin/env python3
"""
Independent decoder for the frames emitted by mesh_frame_test.

Deliberately does NOT share code with the firmware encoder. Everything here is
written from the Meshtastic spec / upstream source:
  - PHY header layout      : RadioInterface.h PacketHeader
  - nonce construction     : CryptoEngine::initNonce
  - channel hash           : Channels::generateHash
  - protobuf field numbers : meshtastic/telemetry.proto, mesh.proto

Exits non-zero on any mismatch.
"""

import struct
import subprocess
import sys
import os
import hmac
import hashlib

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

DEFAULT_PSK = bytes([
    0xd4, 0xf1, 0xbb, 0x3a, 0x20, 0x29, 0x07, 0x59,
    0xf0, 0xbc, 0xff, 0xab, 0xcf, 0x4e, 0x69, 0x01,
])

PORTNUM_POSITION = 3
PORTNUM_NODEINFO = 4
PORTNUM_TELEMETRY = 67

failures = []


def check(label, got, want):
    ok = got == want
    if not ok:
        failures.append(f"{label}: got {got!r}, want {want!r}")
    print(f"  [{'PASS' if ok else 'FAIL'}] {label} = {got!r}")


def check_close(label, got, want, tol=1e-3):
    ok = got is not None and abs(got - want) < tol
    if not ok:
        failures.append(f"{label}: got {got!r}, want ~{want!r}")
    print(f"  [{'PASS' if ok else 'FAIL'}] {label} = {got!r}")


# ---------------------------------------------------------------- protobuf

def parse_fields(buf, is_telemetry_top=False, is_position_top=False):
    """Yield (field_number, wire_type, value) where value is int or bytes."""
    i = 0
    while i < len(buf):
        key, i = read_varint(buf, i)
        field, wire = key >> 3, key & 7
        if wire == 0:
            val, i = read_varint(buf, i)
        elif wire == 5:
            if is_telemetry_top and field == 1:
                val = struct.unpack_from("<I", buf, i)[0]
            elif is_position_top and (field == 1 or field == 2 or field == 4):
                val = struct.unpack_from("<i" if field != 4 else "<I", buf, i)[0]
            else:
                val = struct.unpack_from("<f", buf, i)[0]
            i += 4
        elif wire == 1:
            val = struct.unpack_from("<d", buf, i)[0]
            i += 8
        elif wire == 2:
            ln, i = read_varint(buf, i)
            val = buf[i:i + ln]
            i += ln
        else:
            raise ValueError(f"unsupported wire type {wire} for field {field}")
        yield field, wire, val


def read_varint(buf, i):
    shift = 0
    result = 0
    while True:
        b = buf[i]
        i += 1
        result |= (b & 0x7F) << shift
        if not (b & 0x80):
            return result, i
        shift += 7


def to_dict(buf, is_telemetry_top=False, is_position_top=False):
    return {f: v for f, _, v in parse_fields(buf, is_telemetry_top, is_position_top)}


# ---------------------------------------------------------------- crypto

def decrypt(payload, from_node, packet_id):
    nonce = struct.pack("<QII", packet_id, from_node, 0)
    cipher = Cipher(algorithms.AES(DEFAULT_PSK), modes.CTR(nonce))
    d = cipher.decryptor()
    return d.update(payload) + d.finalize()


def expected_channel_hash(name="LongFast"):
    h = 0
    for b in name.encode():
        h ^= b
    for b in DEFAULT_PSK:
        h ^= b
    return h


# ---------------------------------------------------------------- frame

raw_output = subprocess.check_output(["./mesh_frame_test"], text=True)

frames = {}
reported_hash = None

for line in raw_output.strip().split("\n"):
    if line.startswith("channel_hash "):
        reported_hash = int(line.split()[1], 16)
    elif "_sizeof: " in line:
        k, v = line.split(": ", 1)
        frames[k] = int(v)
    elif " " in line:
        k, v = line.split(" ", 1)
        try:
            frames[k] = bytes.fromhex(v)
        except ValueError:
            frames[k] = v


def parse_frame(frame):
    to, fr, pid = struct.unpack_from("<III", frame, 0)
    flags = frame[12]
    chan = frame[13]
    next_hop, relay_node = struct.unpack_from("<BB", frame, 14)

    return {
        "to": to,
        "from": fr,
        "id": pid,
        "hop_limit": flags & 0x07,
        "hop_start": (flags >> 5) & 0x07,
        "want_ack": bool(flags & 0x08),
        "via_mqtt": bool(flags & 0x10),
        "channel": chan,
        "next_hop": next_hop,
        "relay_node": relay_node,
        "encrypted": frame[16:],
    }


NODE_NUM = 0xa1b2c3d4
PACKET_ID = 0x11223344

print("== channel hash ==")
check("channel_hash", reported_hash, expected_channel_hash())
check("channel_hash is the well-known LongFast value 0x08", reported_hash, 0x08)

print("\n== telemetry frame ==")
f = parse_frame(frames["telemetry_frame"])
check("to (broadcast)", f["to"], 0xFFFFFFFF)
check("from", f["from"], NODE_NUM)
check("id", f["id"], PACKET_ID)
check("hop_limit", f["hop_limit"], 0)
check("hop_start", f["hop_start"], 0)
check("want_ack", f["want_ack"], False)
check("via_mqtt", f["via_mqtt"], False)
check("channel hash in header", f["channel"], expected_channel_hash())
check("next_hop", f["next_hop"], 0)
check("relay_node", f["relay_node"], 0)
check("frame length within PHY limit", len(frames["telemetry_frame"]) <= 255, True)

data = to_dict(decrypt(f["encrypted"], f["from"], f["id"]))
check("Data.portnum", data.get(1), PORTNUM_TELEMETRY)

telem = to_dict(data[2], is_telemetry_top=True)
check("Telemetry.time", telem.get(1), 1769472000)
assert 3 in telem, "Telemetry.environment_metrics (field 3) missing - field 2 is device_metrics"
env = to_dict(telem[3])

check_close("EnvironmentMetrics.temperature (1)", env.get(1), 21.5)
check_close("EnvironmentMetrics.relative_humidity (2)", env.get(2), 63.0)
check_close("EnvironmentMetrics.barometric_pressure (3)", env.get(3), 1013.25)
check_close("EnvironmentMetrics.lux (9)", env.get(9), 12000.0)
check("EnvironmentMetrics.wind_direction (13)", env.get(13), 275)
check_close("EnvironmentMetrics.wind_speed (14)", env.get(14), 4.2)
check_close("EnvironmentMetrics.wind_gust (16)", env.get(16), 7.8)
check_close("EnvironmentMetrics.rainfall_1h (19)", env.get(19), 1.25)
check_close("EnvironmentMetrics.rainfall_24h (20)", env.get(20), 5.50)
check("EnvironmentMetrics.radiation (18) NOT set", 18 in env, False)

print("\n== device metrics frame ==")
f_dev = parse_frame(frames["device_metrics_frame"])
data_dev = to_dict(decrypt(f_dev["encrypted"], f_dev["from"], f_dev["id"]))
check("Data.portnum", data_dev.get(1), PORTNUM_TELEMETRY)

telem_dev = to_dict(data_dev[2], is_telemetry_top=True)
check("Telemetry.time", telem_dev.get(1), 1769472000)
assert 2 in telem_dev, "Telemetry.device_metrics (field 2) missing"
dev_m = to_dict(telem_dev[2])
check("DeviceMetrics.battery_level (1)", dev_m.get(1), 101)
check_close("DeviceMetrics.voltage (2)", dev_m.get(2), 4.8)
check("DeviceMetrics.uptime_seconds (5)", dev_m.get(5), 3600)

print("\n== nodeinfo frame ==")
f2 = parse_frame(frames["nodeinfo_frame"])
data2 = to_dict(decrypt(f2["encrypted"], f2["from"], f2["id"]))
check("Data.portnum", data2.get(1), PORTNUM_NODEINFO)

user = to_dict(data2[2])
check("User.id (1)", user.get(1), b"!a1b2c3d4")
check("User.long_name (2)", user.get(2), b"WeatherXM WG1200 C3D4")
check("User.short_name (3)", user.get(3), b"GC3D")
check("User.hw_model (5) = PRIVATE_HW", user.get(5), 255)
check("User.role (7) = SENSOR", user.get(7), 6)
check("User.public_key (8)", user.get(8), bytes(range(1, 33)))
check("User.is_unmessagable (9) = True", user.get(9), 1)

print("\n== position frame ==")
f3 = parse_frame(frames["position_frame"])
data3 = to_dict(decrypt(f3["encrypted"], f3["from"], f3["id"]))
check("Data.portnum", data3.get(1), PORTNUM_POSITION)

pos = to_dict(data3[2], is_position_top=True)
check_close("Position.latitude_i (1)", pos.get(1), int(37.983810 * 1e7), tol=20)
check_close("Position.longitude_i (2)", pos.get(2), int(23.727539 * 1e7), tol=20)
check("Position.time (4)", pos.get(4), 1769472000)

print("\n== WS1300 downlink command structs ==")
check("RespCmdV1_sizeof", int(frames.get("RespCmdV1_sizeof", 0)), 26)
check("RespCmdV2_sizeof", int(frames.get("RespCmdV2_sizeof", 0)), 27)
check("RespCmdV3_sizeof", int(frames.get("RespCmdV3_sizeof", 0)), 29)

print("\n== MeshCore GRP_DATA frame ==")
mc_raw = frames.get("meshcore_grp_data_frame")
assert mc_raw, "meshcore_grp_data_frame output missing"
if isinstance(mc_raw, str):
    mc_raw = bytes.fromhex(mc_raw)
check("MeshCore header (0x19: GRP_DATA | FLOOD)", mc_raw[0], 0x19)
check("MeshCore path_len (0)", mc_raw[1], 0x00)
expected_mc_hash = hashlib.sha256(DEFAULT_PSK).digest()[0]
check("MeshCore channel_hash (SHA256(PSK)[0] == 0xE8)", mc_raw[2], expected_mc_hash)
check("MeshCore channel_hash is 0xE8", mc_raw[2], 0xE8)
mc_mac_wire = mc_raw[3:5]
mc_ciphertext = mc_raw[5:]
mc_mac_calc = hmac.new(DEFAULT_PSK, mc_ciphertext, hashlib.sha256).digest()[:2]
check("MeshCore HMAC-SHA256 (2 bytes)", mc_mac_wire, mc_mac_calc)

cipher_mc = Cipher(algorithms.AES(DEFAULT_PSK), modes.ECB())
decryptor_mc = cipher_mc.decryptor()
mc_plain = decryptor_mc.update(mc_ciphertext) + decryptor_mc.finalize()

data_type, data_len = struct.unpack_from("<HB", mc_plain, 0)
check("MeshCore data_type (0x5758)", data_type, 0x5758)
check("MeshCore data_len (28)", data_len, 28)

(station_id, ts, temp, hum, press, w_spd, w_gst, w_dir, prcp, s_rad, uv, bat, val_mask) = struct.unpack_from("<IIhBHHHHHHBHH", mc_plain, 3)
check("MeshCore station_id", station_id, 0x12345678)
check("MeshCore timestamp", ts, 1769472000)
check("MeshCore temperature (215 = 21.5C)", temp, 215)
check("MeshCore humidity (63%)", hum, 63)
check("MeshCore pressure (10132 = 1013.2hPa)", press, 10132)
check("MeshCore wind_speed (42 = 4.2m/s)", w_spd, 42)
check("MeshCore wind_gust (78 = 7.8m/s)", w_gst, 78)
check("MeshCore wind_dir (275 deg)", w_dir, 275)
check("MeshCore precip_cml (125 = 12.5mm)", prcp, 125)
check("MeshCore solar_rad (4500 = 450.0W/m2)", s_rad, 4500)
check("MeshCore uv_index (52 = 5.2)", uv, 52)
check("MeshCore battery_mv (3300mV)", bat, 3300)
check("MeshCore valid_mask (0x0FFF)", val_mask, 0x0FFF)

print("\n== MeshCore Signed Sensor GRP_DATA frame ==")
mc_signed_raw = frames.get("meshcore_signed_grp_data_frame")
assert mc_signed_raw, "meshcore_signed_grp_data_frame output missing"
if isinstance(mc_signed_raw, str):
    mc_signed_raw = bytes.fromhex(mc_signed_raw)
check("MeshCore signed header (0x19: GRP_DATA | FLOOD)", mc_signed_raw[0], 0x19)
check("MeshCore signed path_len (0)", mc_signed_raw[1], 0x00)
check("MeshCore signed channel_hash (0xE8)", mc_signed_raw[2], 0xE8)
mc_signed_mac_wire = mc_signed_raw[3:5]
mc_signed_ciphertext = mc_signed_raw[5:]
mc_signed_mac_calc = hmac.new(DEFAULT_PSK, mc_signed_ciphertext, hashlib.sha256).digest()[:2]
check("MeshCore signed HMAC-SHA256 (2 bytes)", mc_signed_mac_wire, mc_signed_mac_calc)

decryptor_mc_signed = Cipher(algorithms.AES(DEFAULT_PSK), modes.ECB()).decryptor()
mc_signed_plain = decryptor_mc_signed.update(mc_signed_ciphertext) + decryptor_mc_signed.finalize()

s_data_type, s_data_len, s_subtype = struct.unpack_from("<HBB", mc_signed_plain, 0)
check("MeshCore signed data_type (0x5758)", s_data_type, 0x5758)
check("MeshCore signed data_len (106)", s_data_len, 106)
check("MeshCore signed subtype (0x01)", s_subtype, 0x01)
dummy_expected = bytes([(i + 1) & 0xFF for i in range(105)])
check("MeshCore signed 105B payload match", mc_signed_plain[4:4+105], dummy_expected)

print()
if failures:
    print(f"FAILED ({len(failures)}):")
    for x in failures:
        print("  -", x)
    sys.exit(1)

print("All checks passed.")
