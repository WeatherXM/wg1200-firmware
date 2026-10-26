# Changelog — Pairing Modes & Uplink Hygiene

Documentation-level changelog for the pairing-mode rework. Release-level entries
live in the repository root [CHANGELOG.md](../CHANGELOG.md).

## [0.8.39] - 2026-10-26

### Major Features & Improvements (v0.8.38 – v0.8.39)

| Area | What actually made it into v0.8.39 |
|---|---|
| **Boot Mode & Manual Target Persistence** | Fixed `WsHandler::init()` and `process_fallback_state_machine()` so gateways configured in `PAIR_MODE_CLOUD` boot directly into the Cloud profile (`PHASE_CLOUD_NVS`), stay locked to configured NVS or manual target stations (`PHASE_MANUAL`), and never overwrite Local NVS with a neighboring station during fallback. Added `POST /clear_local` (**CLEAR LOCAL**) to wipe stale Local NVS profiles. |
| **Multi-Gateway Diagnostics & Ownership Sync** | Unified `STATION LINK` and `STATION OWNERSHIP` badges across Dashboard and Settings tabs, cleared manual channel lock (`g_manual_listen_slot`) upon pairing, added a **Manual Channel Lock Active** warning banner, added `PK ✓` / `No PK` badges on discovered stations, and added a downlink collision / foreign-gateway pairing warning when `ownership_status === 'failed'` with a valid Public Key. |
| **Cross-Station Precipitation Protection & Chart Accuracy** | Tracked `_precip_rate_station_id` and `_daily_precip_station_id` in `WsData::update_precip_rate_params()` so switching stations mid-day resets rate tracking and re-anchors the daily rain baseline without cross-station counter jumps. Added dry-period decay reset (`>= 3600s`), physical sanity bounds (`300 mm/h`), historical CSV outlier repair, and accurate `Wet Time` calculation (`PrecipAccum_mm > 0`). |
| **Station Credential Adoption & LCD Hydration** | Adopted authentic 10-byte serial numbers and Ed25519 public keys from `BOOT`/`ADV` frames when a stored profile has a synthesized channel-hint serial, hydrated the TFT LCD display immediately from `WsData` on pair, and tightened Meshtastic/WS1300/WS1001 packet log filters. |

## [0.8.37] - 2026-10-22

### Major Features & Improvements (v0.8.28 – v0.8.37)

| Area | What actually made it into v0.8.37 |
|---|---|
| **Historical Weather Trends Overhaul** | Upgraded the 24-hour Historical Weather Trends charts and the SD Card CSV Viewer (`/sd/view`) with glassmorphic cards, color-coded summary badges (`Min / Avg / Max`, `Peak Gust`, `Prevailing Wind`, `3h Pressure Trend`, `Total Rain & Wet Time`), **Feels Like** & **Dew Point** series, dual-axis **Precipitation Rate & Cumulative** charts, single-line tabular legends, and `uPlot.sync` linked crosshairs and drag-to-zoom. |
| **Hourly Prevailing Wind Ribbon** | Added a canvas wind direction ribbon across the top of the Wind Trends chart (and SD CSV Viewer) using speed-weighted circular vector averaging (`atan2`) with directional flow arrows and 16-point cardinal labels (`N`, `NNE`, `SW`, etc.). |
| **Bidirectional LCD ↔ Web UI Units Sync** | Added **Display Units (Web & LCD Sync)** controls (`POST /set_units`, `GET/POST /api/v1/units`). Changing temperature, wind speed, precipitation, or pressure units on either the LCD touchscreen or the Web UI immediately updates and rescales all live LCD sparklines, dashboard cards, historical charts, and the SD CSV viewer. |
| **Zero-Wear `RTC_NOINIT_ATTR` Retention & Flash Wear Protection** | Moved primary persistence of daily rainfall baselines and daily min/max weather extremes into CRC32-protected `RTC_NOINIT_ATTR` SRAM (`_rtc_daily_cache`), backed by 24h SD CSV boot hydration and throttled 6-hour / midnight NVS checkpoints. Added delta-only `Settings::commit_safe()` NVS writes, `RTC_NOINIT_ATTR` LCD tab retention, and deduplicated `BootGuard` / station ownership / `dev_name` NVS writes, cutting NVS flash wear by >98%. |
| **Safe microSD Eject / Mount & Auto-Remount** | Added `POST /sd/eject` and `POST /sd/mount` Web UI controls and an LCD **"SD Card Ready to Eject"** alert popup with a **"Mount SD"** button and 60s reminder. Polls `EXPANDER_IO_SD_DET` on the TCA9535 IO expander so physically removing and re-inserting a card automatically remounts `/sdcard` and flushes queued RAM history. |
| **SD Card SPI Hardening & Pre-Inspection** | Added raw SPI Sector 0 / MBR / FAT VBR pre-inspection (`bsp_sdcard_inspect_card()`) to detect unformatted or unsupported cards without triggering `esp_vfs_select` panics. Hardened SPI bus isolation, continuous DMA `0xFF` clocking, multi-stage `ACMD41` recovery, command-level SPI telemetry in `/sd/diagnose`, and capacity-based dynamic formatting ETA (`~4.2s/GB`). |
| **OTA Mutual Exclusion, Wi-Fi & DRAM Hardening** | Added a shared FreeRTOS mutex (`Submit::try_lock_ota()`) coordinating Cloud and Web UI OTA updates, fed `SoftWatchdog` across flash erases and SHA-256 checks, prevented identical-image OTA slot switches, moved `dns_resolve_task` and `sd_writer_task` stacks to PSRAM (`EXT_RAM_BSS_ATTR`) to free internal DRAM, increased Wi-Fi manager queue depth to 16, and optimized RF scan dwell times (~48m down to ~11m). |

## [0.8.23] - 2026-09-20

### Major Features & Improvements (Screen 3 Daily Min/Max, WS1300 Decoder & UI Overhaul)

| Area | What actually made it into v0.8.23 |
|---|---|
| **WS1300 Decoder Fix** | Decoded `batt_mv == 0xFF` as `ERROR_NOT_SUPPORTED` instead of `ERROR_BAD_RESULT`, preventing rejection of valid weather packets from line/solar-powered stations. |
| **Field Invalidation Safeguard** | Restricted `bad_values_detected` trigger strictly to `FieldCatType::WEATHER`, ensuring missing metadata or GPS fields cannot invalidate meteorological sensor telemetry. |
| **Web UI Status & Concurrency** | Fixed `isLinkActive` check to evaluate `active_link` and `ACTIVE_*` statuses, displaying green `PAIRED (ONLINE)` and `Current Target ✓`. Increased HTTP server socket backlog and disabled Wi-Fi modem sleep. |
| **Gzip Compressed Dashboard** | Auto-compressed `g_dashboard_html` to `g_dashboard_html_gz` in `pre_actions.py`, shrinking dashboard payload by ~70% for faster page loads. |
| **Pairing & Scan Controls** | Removed redundant profile dropdown from Pairing Mode panel, added clear single-channel / multi-channel scan labels, and added real-time ETA countdown. |
| **Daily Extremes Tracking Engine** | Implemented `DailyExtremes` in `WsData` with automatic local midnight rollover (using `Settings::get_tz_offset()`), tracking min/max temperature, pressure, humidity, peak wind gust, peak rain rate, peak UV index, and peak solar irradiance. Backfills from `WeatherLogger::get_aggregates()` across reboots. |
| **Arrow-based Min/Max Display** | Enhanced Screen 3 (detailed charts dashboard) to display compact arrow-based daily extremes (`↓ min  ↑ max`) across all tiles using Montserrat 14 font and FontAwesome symbols. |
| **Header Measurement Units** | Migrated unit symbols to tile titles on Screen 3 (e.g. `Daily Rain (mm)`, `Pressure (hPa)`, `Humidity (%)`, `Solar (W/m2)`), dedicating the bottom row exclusively to daily extremes. |
| **Precipitation Display Split** | Display live instantaneous Rain Rate on Screen 2 (classic tiles view) and cumulative Daily Rain with peak rate on Screen 3 (detailed view). |
| **Screen 3 Typography & Glyphs** | Replaced custom ASCII font on `_lbl_feels_like` with standard `lv_font_montserrat_14`, restoring missing degree symbol (`°`) in `22°C` and equalizing font size with the min/max label. Standardized `Solar (W/m2)` header to avoid missing superscript `²` (`0x00B2`) glyph placeholder. |
| **Wind Tile Contrast Polish** | Changed peak gust label from white to `UI_SIMPLE_TILE_WIND_TEXT_COLOR` (`#050505`), eliminating invisible white-on-white text on the light-lavender wind tile. Standardized wind tile bottom row to `lv_font_montserrat_14`. |
| **Claiming Abort & Event Loop Panic Fix** | Fixed `LoadProhibited` crash (`EXCVADDR: 0x00000004` on Core 1) when closing TFT claiming view while MQTT claim request is in flight. Replaced `vTaskDelete` with cooperative cancellation and interruptible delays, ensuring clean `esp_event_handler_instance_unregister_with` unregistration from the global event loop. |

## [0.8.22] - 2026-09-15

### Major Features & Improvements (Home Assistant Integration)

| Area | What actually made it into v0.8.22 |
|---|---|
| **mDNS Discovery (`_weatherxm._tcp`)** | Added ESP-IDF mDNS service advertisement broadcasting `_weatherxm._tcp` on port 80 with stable gateway ID, hardware model, API version (`1.0.0`), and firmware version TXT records for automatic discovery in Home Assistant. |
| **System Info API (`GET /api/v1/info`)** | Added `/api/v1/info` endpoint exposing stable hardware identifier, MAC address, model name, firmware version, Wi-Fi parameters, station metadata, and measurement unit symbols. |
| **Real-time Observations API** | Added `GET /api/v1/observations` and `/api/v1/observation` returning latest parsed weather observations. Added `?buffer=full` query parameter returning the full 15-minute high-frequency raw observation buffer via chunked HTTP streaming. |
| **24-Hour History API** | Added `GET /api/v1/history` endpoint serving aggregated 5-minute historical weather buckets over the previous 24 hours (288 slots). |
| **Shared Weather Extraction** | Factored weather observation decoding and unit formatting out of `/status` into reusable `WeatherObservationData` structures and helper functions, eliminating code duplication between `/status` and `/api/v1/observations`. |
| **Boot Splash Hang Fix** | Fixed FreeRTOS Task Watchdog starvation in `ui_update_task` caused by synchronous `lv_obj_clean(lv_scr_act())` and cyclic event pointers in LVGL's `_lv_event_mark_deleted()`. Migrated all view transitions to safe asynchronous deletion (`lv_obj_del_async`), declared splash instance `static` across boot, and added cycle breaker to LVGL core. |

## [0.8.5] - 2026-08-18

### Major Features & Improvements (from v0.8.4)

| Area | What actually made it into v0.8.5 |
|---|---|
| **MeshCore Frame Decoding** | Added standard MeshCore `0x19` `GRP_DATA` frame parsing with `0x06` telemetry payload decoder in `meshcore_frame.h` and `ws_handler.cpp`, converting authenticated MeshCore packets into standard `FieldPack` structures with full sensor verification. |
| **Multi-Station Forwarding Router** | Added gateway-level forwarding switch `Settings::mesh_multi_station_forwarding_enabled` (NVS key `mesh_fwd_en`, default: `false`) controlling whether observations from un-paired external MeshCore sensor nodes are forwarded to ThingsBoard cloud ingestion. |
| **WS1300 v18 Downlink Protocol (`RespCmdV3`)** | Extended station downlink command struct to 29 bytes with 128-bit HMAC-SHA256 authentication, allowing dynamic over-the-air control of station Meshtastic and MeshCore transmit intervals and profile indices from the gateway. |
| **Web UI & REST API Controls** | Added station MeshCore sensor card and Gateway multi-station forwarding switch to the Web UI. Added REST endpoints `POST /ws1300_station_meshcore` and `GET/POST /api/v1/meshcore/config`. Expanded `/status` JSON response and raised `max_uri_handlers` to 48. |
| **Meshtastic Regional Channels** | Mapped full frequency plans (EU868, US915, AS923, AU915, IN865) and modem presets in `mesh_telemetry.cpp`. |

## [0.8.4] - 2026-08-14

### Major Features & Improvements (from v0.8.2)

| Area | What actually made it into v0.8.4 |
|---|---|
| **SD weather history** | The relatively simple synchronous SD logger in v0.8.2 was turned into a much more robust asynchronous weather-history system. Five-minute aggregates feed a 24-hour/288-slot RAM history and a background SD writer with an 8-record queue. Large writer buffers were moved off the FreeRTOS stack, the writer stack went to 12 KiB, and minimum free stack is monitored. This specifically fixes the first-five-minute-write crash/reset problem. The original v0.8.2 code wrote directly to SD from `log_to_sd()`; the new architecture decouples filesystem I/O from packet handling. |
| **Versioned daily CSV data** | Weather is stored in `history_v1_YYYY_MM_DD.csv`, with local and Unix timestamps plus temperature, humidity, average wind, prevailing direction, max gust, average precip rate, accumulated precipitation, pressure and solar radiation. Missing observations become empty cells rather than misleading zeros/NaNs. |
| **Rich daily metadata / data-quality tracking** | A separate `metadata_v1_YYYY_MM_DD.csv` now carries `DAILY_START`, `DAILY_SUMMARY`, and `GPS` records. It records gateway/station identity, firmware, coverage window, packet counts and counter gaps, TX interval, battery, RSSI, fields present/missing, GPS, timezone and a fairly comprehensive set of quality flags. Daily metadata is accumulated in RAM rather than redundantly written every five minutes. |
| **More resilient SD writes** | File headers are schema-checked before append. Writes check flush/sync/close results; on failure the logger attempts to truncate back to the pre-write length and retries once. Ordinary weather logging does **not** automatically format an unreadable card. |
| **SD management + browser CSV viewer** | The Web UI gained a much richer SD management surface: filesystem/card state, writer/queue status, successful/failed row counts, retries, last write/error, card/capacity/partition data and file listing. `GET /sd/status` exposes it programmatically. CSV files get a new **View** action in addition to Download/Delete. |
| **Interactive weather-history visualization** | `/sd/view?file=...` displays a client-side CSV viewer with search, newest-first sorting, sticky headers and quality-flag highlighting. Weather-history files get charts for temperature/humidity, wind/direction, precipitation, and pressure/solar. The browser does the parsing/plotting; the ESP32 serves the original CSV. View/download/delete filenames were also hardened against directory traversal. |
| **Reliable “PAIR ON REBOOT” for discovered WS1300s** | A sensor-only WS1300 found during scanning can now be selected with **PAIR ON REBOOT**. The gateway locks onto that exact station ID **and its observed channel before the operator reboots the station**, so the one-shot BOOT packet is not missed while sweeping nine slots. A successful operation replaces only the Local profile; the Cloud assignment stays cached. Known stations can also expose the kick/repair action separately. |
| **Polite Meshtastic packet scheduler** | The configured broadcast interval `I` became the base of four schedules: Environment = `1×I`, NodeInfo = `2×I`, DeviceMetrics = `3×I`, Position = `6×I`. Normally only one due frame is transmitted per safe post-weather-station radio window, with weather taking priority when multiple packet types are due. The Web UI explains this cadence. |
| **Listen-before-talk / CAD** | Before a Meshtastic transmission, WG1400 waits a randomized **50–250 ms**, then performs **two SX1262 CAD passes**. A busy channel or even an indeterminate/timed-out CAD result causes transmission to be deferred instead of blindly transmitting. |
| **Shared-radio ownership protection** | A major reliability fix adds a recursive ownership mutex around the shared SX1262. Station retunes, pairing changes and Meshtastic transmissions can no longer interleave their radio reconfiguration sequences. The Meshtastic borrow holds the lock through TX completion and restoration of the weather-station link. This addresses the observed “header OK / payload CRC bad” Meshtastic failures caused by another task retuning the radio mid-frame. A radio-contention counter was also added for diagnostics. |
| **Meshtastic SyncWord protection** | `Radio.Send()` now saves the current LoRa SyncWord before `SX126xSetPacketParams()` and restores it afterward, because that operation could clobber the Meshtastic SyncWord. The Meshtastic TX path also rewrites and verifies the expected `0x24/0xB4` register pair immediately before transmitting and aborts rather than knowingly transmitting an undecodable frame. |
| **Configurable Meshtastic transmit power** | There is now a persistent Web UI/API toggle (`tx_power_high`). In the **final v0.8.4 code**, low-power mode is **2 dBm**. High-power mode is **22 dBm for EU868** and **14 dBm for other supported regions**. `GET /api/v1/meshtastic/config` exposes the state/current power and POST can change it. |
| **NodeInfo freshness** | Boot still establishes Meshtastic identity, and the normal scheduler refreshes NodeInfo at `2×I`. If the gateway's advertised identity changes, the refresh is armed immediately. The manual “Broadcast now” path additionally sends NodeInfo first if it has become stale by `3×I`, then sends the requested environmental telemetry. |
| **Zero weather values are real values** | Final v0.8.4 explicitly serializes fields based on their `has_*` status rather than suppressing zeroes. So **0 m/s wind, 0 m/s gust, 0° wind direction, 0 mm 1h/24h rain and 0 lux** survive into Meshtastic telemetry instead of being interpreted as “missing”. This is broader than the release-note wording, which mainly calls out calm wind and dry conditions. |
| **GPS-derived timezone auto-configuration** | A received or retained GPS longitude automatically sets the fixed timezone offset using approximately `round(longitude / 15°)`, clamped to UTC−12…UTC+14, persists it to settings, and updates the TFT timezone control. Thus the local timestamps/day boundaries used by the logger can self-configure from station location. This remains a **fixed whole-hour offset**, not an IANA timezone/DST resolver. |

## [0.8.1] - 2026-07-30

### Added
- **Independent persisted station profiles:** CLOUD and LOCAL now keep separate ID,
  type, serial, and public-key records plus a persisted selected source. CLOUD and
  LOCAL can be switched without destroying the other saved profile; SCAN replaces
  only LOCAL and never mutates the independent `rs_ws_*` cloud assignment.
- **Expanded pairing identity/status diagnostics:** `/status` now exposes the
  selected and active profile source, raw local/cloud IDs and serials, profile
  completeness, configured target vs. target packets seen, and separate
  uplink-enabled/uplink-active state.
- **Station Pairing panel profile visibility:** cloud/local IDs and serials remain
  visible at all times, including incomplete legacy profiles, and switching between
  different saved targets requires confirmation.

### Changed
- **Persisted pairing-mode semantics refined around independent profiles:** the
  `Settings::pair_mode` enum introduced in v0.7.5 keeps the same OFF / CLOUD / LOCAL /
  SCAN modes, but their behavior now follows the separately persisted profiles:
  - **OFF** — monitor only: preserves the selected CLOUD or LOCAL profile and its
    radio target while disabling weather/GPS uplink.
  - **CLOUD** — selects the profile cached from ThingsBoard shared attributes
    (`rs_ws_model` / `rs_ws_sn` / `rs_ws_pubkey`). A complete local profile remains
    saved and can be selected again later.
  - **LOCAL** — selects the locally paired profile; uplink active. It remains
    selectable even while a different cloud profile is currently active.
  - **SCAN** — channel sweep + auto-pair on ADV/BOOT; successful pairing replaces
    only LOCAL and automatically transitions to LOCAL (persisted).
  - Migration/self-healing was updated for the profile model: LOCAL without a
    complete local profile falls back to SCAN; SCAN with a complete local profile
    resolves to LOCAL.
- **Explicit targeted station repair:** merely entering SCAN no longer
  automatically reboots an already-heard WS1300. Reboot-to-repair is restricted to
  an explicit targeted station-kick/repair action, preventing a nearby station from
  winning the pairing race ahead of the intended target.
- **Cloud adoption made explicit:** SCAN no longer emits an automatic adoption
  request that could be used to overwrite `rs_ws_*`; the cloud assignment remains
  unchanged until a future explicit, authenticated user-authorized adoption flow.

### Fixed
- **Cloud serial/ID split-brain:** the old parser accepted only 16/8-character
  forms and then parsed any other string as decimal. A normal 20-character WS1300
  serial beginning `39FA...` therefore became decimal `39` (`0x00000027`) in the
  web UI while the retained full serial remained on the LCD. Serial parsing is
  now strict and the ID must match the first four serial bytes.
- **Profile source runtime consistency:** signature verification now uses the
  public key from the active cloud/local profile, and a successful SCAN clears the
  runtime auto-pair flag when it settles into LOCAL.

## [0.7.5] - 2026-07-19

### Added
- **First-class persisted pairing mode (`Settings::pair_mode`, NVS key `pair_mode`):**
  the four modes (OFF / CLOUD / LOCAL / SCAN), previously derived by the Web UI from
  `cloud_pair_en` plus RAM-only auto-pair/sniffer flags, became a single persisted
  enum so the operator's mode survives reboot:
  - **OFF** — monitor-only state: sniffer on, auto-pair off and unpaired. Packets are
    decoded/displayed but are not buffered or uplinked. Unlike the old flag-derived
    state, OFF survives reboot instead of silently returning to SCAN.
  - **CLOUD** — uses the station identity assigned through ThingsBoard shared
    attributes. The original implementation kept local auto-pair available as an
    offline fallback, with the next remote-settings sync able to restore the cloud
    assignment.
  - **LOCAL** — locks to the locally paired station with uplink active. In this
    release it was the result of a successful SCAN rather than a directly selectable
    saved profile while unpaired.
  - **SCAN** — channel sweep + auto-pair on ADV/BOOT; automatically transitions to
    LOCAL (persisted) on success.
  - Initial migration: `cloud_pair_en=1` → CLOUD, otherwise an already-paired gateway
    → LOCAL, otherwise → SCAN. Inconsistent legacy states were healed at boot.
- **Atomic pairing-mode API:** `POST /set_pair_mode?mode=off|cloud|local|scan`
  applies a mode in firmware as one operation instead of the Web UI's former
  multi-call sequence. `auto` is accepted as a legacy alias for `scan`; legacy
  `/toggle_autopair`, `/set_cloud_pairing`, and `/unpair` paths re-derive and persist
  the mode so the enum cannot drift away from the runtime flags.
- **`POST /ws1300_kick_station[?id=0x<station_id>][&cancel=1]`:** arms a one-shot
  authenticated LoRa CMD-reboot for the next sniffed WS1300 packet, including while
  the gateway is unpaired. Because the station protocol has no unpair command,
  rebooting a station makes it emit a pairable `STA_BOOT` so SCAN can re-adopt it.
  The CMD HMAC is keyed on this gateway's serial, so a station owned by a different
  gateway rejects it. The optional ID filter prevents a bystander from consuming the
  one-shot. `/status` exposes `ws1300_kick_armed`.
- **Web UI "Station Pairing" panel:** added a dedicated panel above the LoRa Packets
  Sniffer with Pairing Mode controls/captions/tooltips, a Station Profile group
  (renaming the generic profile "Scan" button to "Any" to avoid confusion with SCAN
  mode), and Radio controls for region/channel/sniffer. The header shows the live
  pairing-mode badge and uplink state.

### Fixed
- **Unpaired gateways could uplink weather data:** with sniffing disabled while
  unpaired, the full station handler could still buffer SENSORS packets and
  `submit_ws_data()` could drain them to ThingsBoard, mixing multiple in-range
  stations upstream. Weather buffering and submission are now both hard-gated on
  `PairState::PAIRED`.
- **Stale weather buffered across pairing changes:** receive/aggregate weather
  buffers are flushed on unpair, on pairing to a different station ID, and when a
  re-init such as cloud sync changes or removes the paired station, preventing old
  station data from being attributed to the new station.
- **`max_uri_handlers` raised 24 → 32:** 27 HTTP handlers were being registered
  against a 24-handler limit, causing the final registrations to fail silently.
- **Sniff sweep channel capture:** sweep dwell is now enforced against wall-clock
  time since the slot was tuned rather than only on receive timeout. A nearby station
  transmitting just under the old ~22-second dwell can no longer pin the sweep on
  one channel forever. After a station kick, the sweep holds that channel for 60
  seconds so the rebooting station's one-shot `STA_BOOT` can be received.
