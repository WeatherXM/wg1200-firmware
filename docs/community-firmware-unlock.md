# Community Firmware Unlock — Plan and Process

> **Status: Approved implementation plan — not yet shipped.**
> This document defines how WeatherXM will let owners of production WS1300 stations and
> WG1200 gateways run their own firmware. The architecture and trust model below are the
> implementation direction; public unlock is enabled only after the rollout gates in §10 pass.
> The document is kept identical in the
> [`ws1300-firmware`](https://github.com/WeatherXM/ws1300-firmware) and
> [`wg1200-firmware`](https://github.com/WeatherXM/wg1200-firmware) repositories.
>
> The unlock portal is **[flasher.weatherxm.com](https://flasher.weatherxm.com)**.

---

## 1. Goals

* Owners of WeatherXM hardware can run any firmware they like, including firmware built from
  these repositories, on devices that left the factory with Secure Boot and read protection enabled.
* Unlocking is a deliberate, **one-way** action, similar to unlocking the bootloader on an Android
  phone. After unlocking, a device is permanently a **community device**. It no longer counts as a
  genuine WeatherXM Network device and earns no rewards.
* Publishing the firmware source does not make a clone genuine or reward-eligible. Network trust
  comes from factory-enrolled hardware identity and backend state, not from keeping firmware secret.

## 2. Design principles

1. **No network trust depends on firmware secrecy.** Every firmware image must be publishable
   without exposing any credential that can grant `GENUINE` or reward-eligible status. The shared
   LoRa HMAC pre-shared key (`LORA_PRESH_KEY` / `WS1300_OPEN_PRESH_KEY`) is treated as a public
   protocol constant, not as proof that a device is genuine.
2. **Every device has its own hardware-held key.**
   * WS1300: an ECDSA P-256 key generated on the device and held by TF-M/PSA
     (`app/src/credentials.cpp`). SENSORS and GNSS packets are signed with it. The station serial
     number is derived from the hash of its public key.
   * WG1200: an RSA key used through the ESP32-S3 Digital Signature (DS) peripheral. The HMAC key
     that unlocks it is burned into eFuse and cannot be read back (`main/credentials.cpp`).
3. **Factory enrollment defines "genuine".** During manufacturing, each device's serial number and
   public key are recorded in the WeatherXM backend. A key the backend has not recorded is never
   genuine, no matter which firmware produced it.
4. **The backend blocklist is the source of truth.** Unlocking is first a backend state change. The
   device-side steps only make sure the state change happened before any unsigned code can run.
5. **Unlocking is one-way.** Nothing can move a device from *community* back to *genuine*.

## 3. Device states (backend)

| State | Meaning | Rewards | Data accepted |
|---|---|---|---|
| `GENUINE` | Enrolled at the factory, running official firmware, not unlocked | Yes | Yes |
| `UNLOCK_PENDING` | Owner has started an unlock on flasher.weatherxm.com; token not yet issued | Yes, until the token is issued | Yes |
| `UNLOCKED` | Unlock token issued. Device is on the blocklist | **No, permanently** | Community tier only (see §8) |
| *unknown key* | Public key not in the enrollment registry (DIY hardware, unlocked stations after they create a new key) | No | Community tier only |

Moving to `UNLOCKED` happens **before** the unlock token is returned to the owner. A token never
exists for a device that is still rewardable.

## 4. Role of flasher.weatherxm.com

[flasher.weatherxm.com](https://flasher.weatherxm.com) is the single entry point for owners who want
to unlock. It handles:

* **Sign-in and ownership.** The owner signs in with their WeatherXM account. Only the account the
  device is claimed to can unlock it.
* **Informed consent.** An explicit, irreversible confirmation screen explains the consequences
  (§7).
* **Device challenge relay.** The site connects to the device (Web Serial over USB for the WG1200,
  Web Bluetooth for the WS1300, which has no USB), reads a
  challenge signed by the device, and sends it to the backend.
* **Token delivery.** It receives the device-bound unlock token from the backend and delivers it
  to the device.
* **Flashing.** It writes the WeatherXM-signed chainloader and a community image to the WG1200
  over USB, and to the WS1300 over BLE DFU.
* **Status lookup.** It shows whether a given serial is `GENUINE` or `UNLOCKED`, which is useful
  for second-hand buyers.

## 5. WG1200 gateway (ESP32-S3) — signed chainloader

### 5.1 Why the gateway needs special handling

Secure Boot V2 eFuses on the ESP32-S3 are permanent. The production bootloader only boots apps
signed with the WeatherXM production key, and that can never be turned off. The eFuse HMAC key
behind the DS peripheral also cannot be erased or disabled. **Any code running on the chip,
including unsigned community code, can still ask the DS peripheral to sign.** Nothing on the
device can stop an unlocked gateway from producing "genuine" signatures, so the backend blocklist
(§3) is what stops rewards.

### 5.2 Owner process

1. Plug the gateway into a computer over USB and open
   [flasher.weatherxm.com](https://flasher.weatherxm.com) in a browser that supports Web Serial.
2. Sign in, select the gateway, and read and accept the irreversible-unlock warning.
3. The flasher reads a signed unlock challenge from the gateway's official firmware.
4. The backend verifies the challenge, marks the gateway `UNLOCKED`, and returns an unlock token.
5. The flasher writes the WeatherXM-signed **chainloader**, the token, and the community firmware
   the owner picked. This can be an official Open Edition release or the owner's own
   `pio run -e wg1200_open` build.
6. On first boot the chainloader verifies the token, permanently burns the `UNLOCKED` eFuse bit,
   and runs the community firmware. The ESP32-S3 production bootloader always continues to boot
   the WeatherXM-signed chainloader; the chainloader is the component that loads unsigned
   community code. It shows an *"Unlocked — community device"* warning at startup.

### 5.3 Technical design

**Unlock challenge (official firmware, new command over USB serial)**

```
challenge = { serial (eFuse KEY2), efuse_mac, fw_version, nonce_from_backend }
signature = DS_RSA_sign(SHA-256(challenge))
```

The backend checks the signature against the enrolled gateway public key and checks that the nonce
is fresh.

**Unlock token (issued by the backend)**

```
token = { version, serial, efuse_mac, issued_at, nonce }
token_sig = ECDSA_P256_sign(WXM_UNLOCK_KEY, SHA-256(token))
```

* `WXM_UNLOCK_KEY` is a dedicated key, separate from the Secure Boot signing key. It is kept
  offline or in an HSM behind the backend. Only its public key is built into the chainloader.
* The token only works on the gateway whose eFuse MAC and serial it names.

**Chainloader (a normal app signed with the WeatherXM production key)**

* It is installed like any signed OTA image, so the existing bootloader accepts it without changes.
  **The ROM/second-stage bootloader is never replaced in the field.**
* After unlocking, `otadata` always selects the chainloader. The current partition table
  (`partitions_*_wxm.csv`: `factory` + `ota_0` + `ota_1`) is kept. The chainloader occupies one app
  slot, and the other slots hold community images.
* Boot logic:
  1. If the `UNLOCKED` eFuse bit is **not** set and no valid token is present, boot the official
     firmware. The chainloader then does nothing.
  2. If a valid token for this chip is present and the bit is not yet set:
     * burn the `UNLOCKED` bit (one-way);
     * erase the `esp_secure_cert` partition, which holds the device certificate and the encrypted
       DS parameters, and erase the production NVS credentials. This is defence in depth only:
       flash is not encrypted, so an owner could have backed these up first. That is why the
       blocklist, not this erase, is what counts.
  3. If the `UNLOCKED` bit is set: load the selected community image (without checking a
     signature), print and display the unlocked warning, and jump to it.
* Community firmware must not call `esp_ota_set_boot_partition()` to replace the chainloader. The
  open firmware gets a small `wxm_chainloader` API to choose which slot the chainloader boots next,
  and OTA of community images goes through that API.

**Official firmware after unlock**

Official signed firmware can still be flashed. It reads the `UNLOCKED` bit, reports
`unlocked=true` in its telemetry and status page, and shows the warning. The backend keeps the
device `UNLOCKED` whatever firmware it runs.

## 6. WS1300 station (nRF5340) — BLE DFU with a signed chainloader

The WS1300 has **no USB port**. Owners unlock and flash it entirely over **Bluetooth LE**, using
the same MCUboot/SMP BLE DFU that official updates use.

### 6.1 Why the station also needs a chainloader

* MCUboot sits at `0x0` with no upgradable first-stage bootloader in front of it, and it
  write-protects itself at boot (`CONFIG_FPROTECT=y`). **It cannot be replaced in the field.** It
  checks the primary slot on every boot (`CONFIG_BOOT_VALIDATE_SLOT0=y`) and accepts only images
  signed with the WeatherXM production key.
* The debug port stays locked (`APPROTECT`). Firmware cannot unlock it, because only a full chip
  erase over SWD can do that.

A community image signed with `keys/open_dev_key.pem` therefore can never be booted directly by a
production station. As on the gateway, WeatherXM publishes a **production-signed chainloader
image**. It is installed by normal BLE DFU and runs community code that sits outside the region
covered by MCUboot's signature check.

**The station is stronger than the gateway in one way.** Its identity key is a non-exportable PSA
key inside TF-M. It cannot be backed up, and once destroyed it is gone. The station serial is
derived from the public key, so after unlock the station **permanently loses its enrolled
identity**. Any key created afterwards gives a new serial that is not in the registry.

### 6.2 Owner process

1. Open [flasher.weatherxm.com](https://flasher.weatherxm.com) in a browser with **Web Bluetooth**
   (Chrome/Edge on desktop or Android). Sign in, select the station, and accept the
   irreversible-unlock warning.
2. Wake the station's BLE, as you would for a normal BLE update, and connect to it from the flasher.
3. The flasher reads a signed unlock challenge from the station's official firmware. The backend
   verifies it, marks the station `UNLOCKED`, and returns an unlock token.
4. The flasher sends the token to the station over BLE. The official firmware verifies it,
   **destroys the station's identity key**, and writes a permanent `UNLOCKED` marker.
5. The flasher uses BLE DFU to install the WeatherXM-signed **chainloader** image.
6. The chainloader starts a BLE DFU service that takes community images: an official Open Edition
   release, or the owner's own build (§6.4). The flasher uploads the chosen image and the station
   reboots into it.

Later community updates go through the chainloader's BLE DFU service, either from the flasher or
from `tools/ws1300_dev_tui.py`.

### 6.3 Technical design

**Unlock challenge (official firmware, new BLE command)**

```
challenge = { serial, public_key, fw_version, nonce_from_backend }
signature = ECDSA_P256_sign(station identity key, SHA-256(challenge))
```

The backend checks the signature against the enrolled station public key and checks that the nonce
is fresh.

**Unlock token.** Same format and `WXM_UNLOCK_KEY` as the gateway (§5.3), bound to the station
serial and public key. The official firmware ships the unlock public key and verifies the token
before doing anything.

**Unlock step (official firmware, after verifying the token)**

1. Write a write-once `UNLOCKED` marker in TF-M internal trusted storage
   (`psa_its_set(..., PSA_STORAGE_FLAG_WRITE_ONCE)`). Code running in the non-secure domain cannot
   remove or overwrite it.
2. Clear pairing state and the LoRa gateway serial.
3. `psa_destroy_key(SIGNING_KEY_ID)` destroys the enrolled identity key.

The marker is written before key destruction so a power loss cannot leave the station without its
old key but without a durable unlocked state. On every subsequent boot, official firmware checks
the marker before `Credentials::init()`; if the marker exists it never generates a replacement
WeatherXM Network identity and completes any pending identity-key destruction before network use.

The token is bound to one station and issued only to its owner's account. So someone nearby with a
BLE connection cannot use the public chainloader to wipe another person's station.

**Chainloader image (signed with the WeatherXM production key)**

* It contains WeatherXM's TF-M build plus a small non-secure chainloader. The implementation
  reserves a fixed **community application region** inside the existing non-secure internal-flash
  window below `tfm_its` (`0xec000`). The boundary and size are encoded in a dedicated
  community partition map and shared linker constants; they are not inferred from incidental free
  space. The signed chainloader image is constrained to end before this region, and the build fails
  if the signed image overlaps it. MCUboot continues to validate the signed chainloader image; the
  chainloader deliberately validates and transfers control to the separate community region.
* Boot logic:
  1. **No `UNLOCKED` marker** (someone installed the chainloader without unlocking): do not run
     any community code. Only allow BLE DFU of an **official** production-signed image, which
     restores the station.
  2. **Marker present:** check that the community region holds a well-formed image (header, CRC,
     linked base address, matching TF-M interface version). If it does, jump to its non-secure
     entry point. If it doesn't, stay in chainloader BLE DFU mode.
* Community image updates arrive over BLE through the chainloader's own SMP service. They are
  staged in a dedicated external-flash area, not `mcuboot_secondary`, and copied to the community
  region. The staged copy is retained across a chainloader update so the chainloader can restore
  the community region after MCUboot swaps a new signed chainloader image. Official images keep
  going through `mcuboot_secondary`, so the owner can always return to official firmware without
  changing the backend `UNLOCKED` state.
* The network core keeps WeatherXM's signed BLE controller image (`hci_ipc`), which is open source
  already. Community firmware only replaces the application-core non-secure image.

**What community firmware gets:** full control of the application core's non-secure side (LoRa,
sensors, GNSS, BLE host, Meshtastic/MeshCore), plus the PSA crypto and storage services of
WeatherXM's TF-M. It can create its own key pair, but that key and the serial derived from it are
never in the enrollment registry.

**Official firmware after unlock.** If an owner reinstalls official firmware, it finds the
`UNLOCKED` marker, reports `unlocked=true`, does not create a network identity, and shows its
unlocked status over BLE. The backend keeps the station `UNLOCKED`.

### 6.4 Building community images for the chainloader

The implementation adds a `ws1300_community` build profile that produces a **non-secure-only**
image. It is linked at the community region's base address, built against the TF-M interface
version the chainloader ships, and packaged for chainloader BLE DFU. The current Open Edition
build (MCUboot + TF-M + app, signed with `keys/open_dev_key.pem`) is still for development boards
and for stations that were erased over SWD.

### 6.5 Alternative for developers: SWD hardware erase

If you can open the enclosure and reach the SWD pads, a full chip erase (`nrfjprog --recover`)
also unlocks the station. It wipes MCUboot, TF-M storage (including the identity key) and factory
data, after which you can flash the regular Open Edition build with a probe. Start the unlock on
[flasher.weatherxm.com](https://flasher.weatherxm.com) first so the backend state stays correct.
This is a lab path. Owners are expected to use BLE.

## 7. What unlocking means for the owner

The flasher shows these points and the owner must accept them:

* **Irreversible.** The device can never become a genuine WeatherXM Network device again.
* **No rewards.** The device is removed from reward eligibility for good. Rewards already earned
  are not affected.
* **Official firmware can still be installed** (over USB on the gateway, over BLE DFU on the
  station), but the device stays a
  community device.
* **Resale.** Anyone can see on flasher.weatherxm.com that the device is unlocked.
* **Warranty and support terms.** Public unlock is enabled only after the applicable warranty and
  support terms are published in the unlock UI; the owner accepts those terms before a token is issued.

## 8. Mandatory rollout requirements

WeatherXM will complete all of the following before public unlock is enabled. These requirements
ensure that publishing open firmware and unlocking hardware cannot create reward-eligible clones:

- [ ] **Factory enrollment registry.** Record serial + public key for every station and gateway at
      manufacturing. Backfill the fleet already deployed from existing records or first-contact
      data.
- [ ] **End-to-end signature checks in the backend.** Gateways pass station payloads through
      unchanged with the station's ECDSA signature, and the backend verifies them against the
      registry. It does not rely on the gateway having checked them.
- [ ] **GNSS packet signature check on the gateway** (TODO at
      `main/ws/supported/ws_1300/ws_1300.cpp` in `wg1200-firmware`).
- [ ] **Per-pair station ↔ gateway keys** (ECDH at pairing, or a pairing ticket signed by the
      backend). These authenticate gateway → station commands instead of the shared pre-shared key.
- [ ] **Treat the current production LoRa HMAC key as public.** Production gateway flash is not
      encrypted, so that key can already be read from shipped units.
- [ ] **Community-tier handling.** `UNLOCKED` and unknown-key devices are never reward-eligible
      and are never treated as genuine network observations. If their telemetry is accepted, it is
      stored and exposed only as explicitly labelled Community-tier data.

## 9. Implementation work items

| Component | Work |
|---|---|
| **WG1200 official firmware** | Unlock-challenge command over USB serial (DS-signed); read the `UNLOCKED` eFuse bit and report it in telemetry, status page and LCD |
| **WG1200 chainloader** | New signed app: token verification (P-256), eFuse bit burn, `esp_secure_cert`/NVS wipe, unsigned-image loader, boot warning, `wxm_chainloader` slot-selection API for community firmware |
| **WG1200 open firmware** | Build profile that runs under the chainloader; OTA through the chainloader API |
| **WS1300 official firmware** | BLE unlock-challenge command (signed with the identity key); token verification; identity-key destruction and write-once `UNLOCKED` ITS marker; report unlocked state |
| **WS1300 chainloader** | New production-signed image (TF-M + non-secure chainloader): marker check, community-image check and jump, BLE SMP service for community images, official-image fallback |
| **WS1300 open firmware** | `ws1300_community` profile: non-secure-only image linked at the community region, packaged for chainloader BLE DFU |
| **Backend** | Enrollment registry; `GENUINE → UNLOCK_PENDING → UNLOCKED` state machine; challenge verification; token signing with `WXM_UNLOCK_KEY`; reward exclusion; public status endpoint |
| **flasher.weatherxm.com** | Sign-in and ownership check; consent screen; Web Serial challenge/token relay and flashing for WG1200; Web Bluetooth challenge/token relay and BLE DFU for WS1300; status lookup |
| **Docs** | Update `CUSTOM_FIRMWARE.md` in both repos when each step ships |

## 10. Engineering constraints and release gates

The architecture above is the implementation plan. The following are release gates, not alternative
design directions:

* **WG1200 chainloader.** WeatherXM will implement the production-signed chainloader described in
  §5. Before public unlock is enabled, production-equivalent Secure Boot V2 hardware validation
  must demonstrate cold boot, watchdog/reset recovery, community-image validation/loading, OTA slot
  switching, interruption recovery, and return to an official signed image. The production
  second-stage bootloader is never replaced in the field.
* **WS1300 chainloader.** WeatherXM will implement the production-signed TF-M + non-secure
  chainloader described in §6. The implementation defines a fixed community application region,
  build-time overlap checks, a versioned TF-M veneer/ABI contract, and an external-flash staging
  area. Public unlock is enabled only after production-equivalent nRF5340 validation demonstrates
  repeated BLE community updates, chainloader updates, power-loss recovery at every destructive
  step, and return to official firmware without loss of the durable `UNLOCKED` state.
* **Browser/device support.** WG1200 unlock uses Web Serial in supported desktop Chromium browsers.
  WS1300 unlock uses Web Bluetooth in supported desktop Chromium browsers and Android. iOS/iPadOS
  does not provide native Web Bluetooth, so the supported iOS path is the WeatherXM native app
  (or another WeatherXM-approved native BLE client), not the web flasher.
* **WG1200 unlock eFuse.** WeatherXM will reserve a dedicated unused one-way user eFuse bit for
  `UNLOCKED`, verify that it does not overlap the existing `M8_SEC_ENABLE_BLOCK` use of
  `EFUSE_BLK3`, and write-protect the selected bit after it is burned.
* **Power-loss safety.** Every destructive transition is idempotent. Backend state becomes
  `UNLOCKED` before the token is returned; device-side persistent unlock state is committed
  before identity material is destroyed; interrupted flashing resumes safely.
* **Hardware attacks.** Fault-injection attacks against device secure boot/debug locks are outside
  the normal owner-unlock path. Enrollment and the backend blocklist ensure compromise of one
  physical device cannot create a new reward-eligible identity.
* **Community data.** Community devices may participate in explicitly labelled Community-tier
  telemetry, but never in rewards or genuine-network trust decisions.
* **Warranty/support gate.** The public unlock control remains disabled until the owner-facing
  warranty and support terms are published and included in the irreversible-consent flow.

---
