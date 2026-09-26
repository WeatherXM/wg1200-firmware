# WG1200 Custom & Community Firmware Guide

This guide explains how developers and system integrators (SIs) can build, customize, and flash firmware for the WeatherXM WG1200 gateway and compatible hardware.

---

## 1. Quick Start: Building Open Edition Firmware

Out of the box, the repository is configured to build an **Open / Community Edition** (`wg1200_open`).

### Prerequisites

* [VS Code](https://code.visualstudio.com/) with the [PlatformIO IDE](https://platformio.org/) extension installed, or the PlatformIO Core (`pio`) CLI.
* Python 3.10+
* Git

### Step 1: Clone and Build

```bash
# Clone the repository
git clone https://github.com/WeatherXM/wg1200-firmware.git
cd wg1200-firmware

# Build Open Edition for standard WG1200 hardware
pio run -e wg1200_open
```

### Step 2: Flash to Gateway

Connect your ESP32-S3 gateway board via USB:

```bash
# Upload Open Edition firmware and monitor serial output
pio run -e wg1200_open -t upload -t monitor
```

---

## 2. Hardware Safety: Open Edition vs. Production Edition

| Feature | Open / Community Edition (`wg1200_open`) | WeatherXM Production Edition (`wg1200_production`) |
|---|---|---|
| **Intended Hardware** | Generic ESP32-S3 boards, unlocked community gateways | WeatherXM Factory Manufacturing Only |
| **ESP32-S3 eFuses** | **Never touched** (`wxm_virt` NVS keys) | Permanently burned (`CONFIG_WXM_PRODUCTION_BURN_EFUSES=1`) |
| **Secure Boot** | Disabled; board can be flashed repeatedly | Enabled with WeatherXM production key |
| **WS1300 Station Auth**| Default Open Edition pre-shared key | WeatherXM network HMAC key |
| **Board Name** | `WXM-WG1200-OPEN` | `WXM-WG1200` |

> [!CAUTION]
> **Do not flash `wg1200_production` to generic development boards.**
> The production profile can burn physical hardware eFuses on the ESP32-S3. `wg1200_open` and `wg1200_debug` both use the open security policy; `wg1200_debug` differs only in build/debug behavior.

---

## 3. Station ↔ Gateway LoRa Communication

The WG1200 receives and decodes telemetry packets from WeatherXM weather stations:

* **WS1300 Stations**: Open Edition includes default pre-shared keys in [`main/ws/supported/ws_1300/include/ws_1300_keys.h`](main/ws/supported/ws_1300/include/ws_1300_keys.h). When used alongside a station running **[WS1300 Open Edition firmware](https://github.com/WeatherXM/ws1300-firmware)**, the gateway will immediately recognize, authenticate, and process observation packets over LoRa.
* **WS1001 Stations**: Supported via FineOffset WL69 RF framing in `main/ws/supported/ws_1001/`.

---

## 4. Local Web Dashboard & Telemetry Inspection

The gateway runs an embedded HTTP server providing local status and diagnostics:

* **Status Page**: `http://<gateway-ip>/status` (JSON summary of gateway uptime, Wi-Fi RSSI, paired station status, and weather observations).
* **Live LoRa Packet Log**: `http://<gateway-ip>/packet_log` (Decoded RF packet table showing frequency, RSSI, SNR, packet count, and sensor payload).
* **Web Configuration**: Built-in Wi-Fi provisioning portal when unconnected.

---

## 5. Alternative Hardware Targets

In addition to standard WG1200 hardware with LCD displays, the repository supports screenless and solar-powered configurations:

* **Seeed Studio XIAO ESP32-S3** (Screenless / Solar gateway):
  ```bash
  pio run -e xiao_esp32s3 -t upload
  ```
* **LilyGO T-SIM7000G / SIM7000** (Cellular backup):
  ```bash
  pio run -e lilygo_sim7000g_s3_standard -t upload
  ```

---

## 6. Unlocking a Production WG1200 Gateway

Production gateways have Secure Boot V2 permanently enabled in eFuse, so they only boot firmware
signed by WeatherXM. To run Open Edition or your own firmware on one, use
**[flasher.weatherxm.com](https://flasher.weatherxm.com)**. With the gateway plugged in over USB,
the flasher:

1. verifies you own the gateway and asks you to confirm the unlock;
2. gets a device-bound unlock token from WeatherXM;
3. installs the WeatherXM-signed **chainloader** plus your chosen community firmware.

On first boot the chainloader permanently burns an `UNLOCKED` eFuse bit. From then on it boots
unsigned community firmware and shows an unlocked warning at startup.

> [!WARNING]
> Unlocking cannot be undone. The gateway permanently becomes a **community device** and can no
> longer earn WeatherXM Network rewards, even if official firmware is flashed again later.

The full process and design are in
[`docs/community-firmware-unlock.md`](docs/community-firmware-unlock.md) (approved implementation plan; not yet shipped).

