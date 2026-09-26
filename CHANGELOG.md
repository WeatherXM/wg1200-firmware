# Changelog - WG1200 plus Screenless WG1400 Firmware (Phase A)

All notable changes to the WG1200 firmware to support D1, D2 and future screenless gateway (**WG1400**) running in conjunction with MeshCore KISS modem firmware.

## [0.8.27] - 2026-09-22

### Added & Architecture
- **Canonical Pairing Lifecycle States (`struct.h`, `ws_handler.h`, `ws_handler.cpp`, `web_server.cpp`):**
  - Standardized four pairing lifecycle states across documentation, firmware telemetry, and HTTP API:
    - `standby_discovery`: Out-of-the-box or wiped gateway listening/sniffing for WeatherXM signals without cloud assignment. Decodes weather packets for local display without backend telemetry upload.
    - `cloud_verified`: Standard production mode. Assigned cloud station is paired, IDs match, and ownership challenge is confirmed. Full weather telemetry uplink active.
    - `local_override`: Explicitly locked to local NVS station (manually configured via Web UI or local profile active while cloud ID differs or is absent). Full weather telemetry uplink active.
    - `fallback_latch`: Automated fallback recovery (Phase 3/4) timed out on legitimate targets and latched onto an available WeatherXM station signal to maintain telemetry continuity.
  - Added `WsHandler::get_pairing_state_name()` exposing current lifecycle state.
  - Exposed `"pairing_state": "<state>"` in `/status` HTTP JSON telemetry.
  - Created architectural guide `docs/pairing_states.md` with transition diagrams, operational rules, and backend data contracts.

### Fixed & Stability
- **Web UI Fallback Lockout Trap (`web_server.cpp`):**
  - Resolved bug where configuring pair mode via `/set_pair_mode` or pairing locally via `/pair_local` called `set_fallback_phase(PHASE_MANUAL)`. This permanently disabled `process_fallback_state_machine()` and halted all automated recovery fallback timers.
  - Replaced `PHASE_MANUAL` assignment with `reset_fallback_state_machine()`, allowing recovery timers to run fresh on the user's selected configuration.
- **Desperate Fallback Candidate Filtering (`ws_handler.cpp`):**
  - Fixed candidate selection in desperate recovery phases (Phase 3 WS1001 scan and Phase 4 WS1300 sweep) to include stations matching the existing cloud ID or local profile, rather than excluding them, ensuring previously paired stations that wake up or recover during the day can still be re-latched.

## [0.8.26] - 2026-09-22

### Fixed & Stability
- **Automated Fallback Recovery Auto-Pairing (`ws_handler.cpp`):**
  - Exempted Phase 3 (WS1001 dedicated scan) and Phase 4 (WS1300 channel sweep) from manual discovery scan restrictions at the outer auto-pairing gate. Unconfigured or factory-reset gateways can now successfully auto-pair candidate stations in desperate mode without requiring UI intervention.
- **Remote Settings Station Pair Mode Management (`submit.cpp`):**
  - Resolved cloud pair mode adoption lockout on wiped/reset units where local profile memory mirroring previously prevented adoption of ThingsBoard station attributes.
  - Added guards around pair mode re-application to prevent interrupting active Web UI discovery scans or manual override modes, and ensure locally configured pairings (`PAIR_MODE_LOCAL`) or monitor mode (`PAIR_MODE_OFF`) are not overwritten by cloud sync.
  - Synchronized WS1300 cloud station serial adoption with local profile mirroring on unconfigured gateways.

## [0.8.25] - 2026-09-21

### Fixed & Stability
- **Station Low-Battery UI Alert Dynamic Clearing (`state_sniffer.cpp`):**
  - Resolved low-battery banner latching bug where the warning remained permanently on the LCD once triggered. Added dynamic `alert_handler(&_view_alert_ws_low_bat, ws_data_low_bat)` clearing when battery voltage recovers or is absent.
  - Initialized local stack variable `ws_data_low_bat` to prevent garbage reads if decoding fails.
- **WS1300 Battery Absent / Sentinel Handling (`ws_1300_decoders.cpp`):**
  - Updated `decode_meta_bat_low()` to return `val = false` (OK) when `batt_mv == 0xFF` or when no battery presence flags are set, preventing false low-battery alerts on solar/line-powered stations without batteries.

## [0.8.24] - 2026-09-20
- Version bump for test runner alignment.

## [0.8.23] - 2026-09-20

### Fixed & Stability
- **WS1300 Battery 0xFF Decoding Fix (`decode_meta_bat_mv`):**
  - Updated decoder to return `WsCommon::RetRes::ERROR_NOT_SUPPORTED` instead of `ERROR_BAD_RESULT` when `batt_mv == 0xFF` (transmitted by stations without internal battery monitoring or powered via solar/line power).
  - Prevented valid weather packets from being flagged as corrupt, allowing them to populate LCD widgets and cloud telemetry buffers.
- **Weather Packet Invalidation Safeguard (`ws_fields.cpp`):**
  - Restricted `bad_values_detected` trigger strictly to fields in `FieldCatType::WEATHER`. Non-weather metadata or GPS fields with bad or missing values can no longer invalidate meteorological sensor observations.
- **Web UI Paired Station Status Badge (`isLinkActive`):**
  - Fixed logic in `web_server.cpp` to verify `active_link` and match `ACTIVE_*` status titles (e.g. `ACTIVE_OK`), properly displaying green `PAIRED (ONLINE)` and `Current Target ✓` badges instead of hanging in `PAIRED (WAITING)` / `Connecting...`.
- **Wi-Fi Power Save & Stability:**
  - Disabled Wi-Fi modem sleep (`esp_wifi_set_ps(WIFI_PS_NONE)`) upon station connection, preventing packet drops and latency spikes during high-throughput local HTTP/WebSocket requests.
- **HTTP Server Concurrency:**
  - Increased HTTP daemon maximum open sockets to 7 and connection backlog to 8, improving responsiveness under concurrent REST API and dashboard polling.
- **Claiming Abort & Event Loop Panic Fix (`enable_claiming_task`):**
  - Resolved fatal `LoadProhibited` kernel panic (Guru Meditation Error, `EXCVADDR: 0x00000004` on Core 1) occurring when the user closed the TFT claiming modal while an MQTT claim request was in flight.
  - Replaced asynchronous hard task termination (`vTaskDelete(_claim_enable_task_handle)`) with cooperative cancellation using `_stop_claiming` flag and FreeRTOS task notification (`xTaskNotify`).
  - Replaced blocking delays with interruptible `delay_or_stop()` via `xTaskNotifyWait()`, ensuring `enable_claiming_task` aborts instantly without blocking.
  - Ensured `GlobalEvents::wait(MQTT_DATA_PUBLISHED)` cleanly unwinds and unregisters `notification_handler` from the ESP event loop (`esp_event_handler_instance_unregister_with`), eliminating dangling stack references and invalid task handle notifications upon MQTT ACK reception.
  - Added self-termination (`vTaskDelete(NULL)`) and defensive concurrency checks in `CLAIM_REQUESTED` to guarantee safe restart cycles.

### Added & UI Polish
- **Gzip-Compressed Web Console Delivery:**
  - Integrated automated pre-action build step in `pre_actions.py` compressing `g_dashboard_html` into gzip byte array `g_dashboard_html_gz` (`dashboard_html_gz.h`).
  - Serves compressed dashboard with `Content-Encoding: gzip`, reducing payload by ~70% and accelerating page load on mobile and low-bandwidth connections.
- **Pairing Mode & Scan UI Refinements:**
  - Removed redundant "Profile" dropdown from Pairing Mode panel to streamline pairing options.
  - Clarified station scan options with explicit `(Single Channel)` and `(Multi-Channel)` designations.
  - Added client-side real-time countdown timer for Scan ETA, synchronized on each polling interval.
  - Simplified and shortened the station scan confirmation modal description.
- **Screen 3 Daily Extremes Tracking & Arrow Display:**
  - Designed and integrated daily extremes tracking into `WsData` (`DailyExtremes`), automatically capturing daily minimum and maximum values across all meteorological parameters:
    - **Temperature**: Daily Min and Max (`↓ min°  ↑ max°`) on `TempDualTile`.
    - **Wind**: Live Speed & Gust with Daily Peak Gust (`Max ↑ gust`) on `WindDualTile`.
    - **Pressure**: Daily Min and Max (`↓ min  ↑ max`) in Montserrat 14 on the bottom row.
    - **Humidity**: Daily Min and Max (`↓ min%  ↑ max%`) on the bottom row.
    - **Daily Rain**: Daily accumulation with Daily Peak Rate (`Max ↑ rate/h`) on the bottom row.
    - **UV Index**: Live UV with Daily Peak UV (`Max ↑ uv`) on the bottom row.
    - **Solar Irradiance**: Live solar with Daily Peak Irradiance (`Max ↑ w/m2`) on the bottom row.
  - Automatically resets all daily min/max values at 00:00 (midnight) local time based on `Settings::get_tz_offset()`.
  - Automatically backfills daily extremes from `WeatherLogger::get_aggregates()` for today upon gateway reboots.
- **Header Unit Labels on Screen 3:**
  - Shifted measurement units to tile headers on Screen 3 (e.g., `Daily Rain (mm)`, `Pressure (hPa)`, `Humidity (%)`, `Solar (W/m2)`), freeing the entire bottom row for arrow-based daily extremes.
  - Configured `SimpleTile` with flexible font and alignment controls (`title_font`, `title_align`, `units_font`, `units_align`).
- **Screen 2 vs Screen 3 Precipitation Layout Split:**
  - **Screen 2 (TFT Tiles View)**: Displays live **Rain Rate** (`mm/h` or `in/h`) on the primary weather tile.
  - **Screen 3 (Detailed View with Trend Charts)**: Displays cumulative **Daily Rain** (`mm` or `in`) on the top-right tile with peak rain rate displayed in the bottom extreme row.
- **Screen 3 Typography, Font Harmonization & Glyph Fixes:**
  - **Feels-Like Degree Symbol (`°`) & Size Matching**: Switched `_lbl_feels_like` in `TempDualTile` to standard built-in `lv_font_montserrat_14`. Replaces custom ASCII-only font that lacked glyph `0x00B0`, restoring the degree symbol in `22°C` and harmonizing font sizing with the adjacent `↓ min  ↑ max` label.
  - **Solar Unit Glyphs (`W/m2`)**: Standardized title to `Solar (W/m2)` to eliminate `?` missing-glyph placeholder caused by superscript `²` (`0x00B2`) missing from `lv_font_montserrat_14`.
  - **Dynamic Label Alignment**: Enforced explicit `lv_obj_align_to` on text updates for `_lbl_feels_like` and `_lbl_direction` so labels remain correctly positioned relative to their titles after value changes.
- **Screen 3 Contrast & Visibility:**
  - **Wind Peak Gust Contrast**: Changed `_lbl_peak_gust` color from `UI_TEXT_COLOR_LIGHT` (white) to `UI_SIMPLE_TILE_WIND_TEXT_COLOR` (`UI_TEXT_COLOR_DARK` / `#050505`), fixing white-on-white text on the light lavender (`#EAEBF9`) wind tile.
  - **Wind Bottom Row Font Alignment**: Switched `_lbl_direction` to `&lv_font_montserrat_14` to ensure font consistency across the entire bottom row of `WindDualTile`.

## [0.8.22] - 2026-09-15

### Added & Home Assistant Integration
- **mDNS Service Advertisement (`_weatherxm._tcp`):**
  - Integrated ESP-IDF native `mdns` component advertising `_weatherxm._tcp` on port 80 as soon as Wi-Fi acquires an IP address.
  - Advertises TXT records: `id` (stable serial string or Wi-Fi MAC), `gateway_id`, `model` (`CONFIG_HW_NAME`), `api_version` (`"1.0.0"`), and `sw_version` (`APP_VERSION`).
  - Automatically unregisters and frees resources on Wi-Fi disconnect or IP loss.
  - Enables zero-configuration discovery in Home Assistant without manual IP input.
- **Dedicated Local REST API Endpoints:**
  - `GET /api/v1/info`: Returns stable gateway ID, hardware model, firmware/API version, Wi-Fi parameters, station metadata, mesh configuration, and active measurement units.
  - `GET /api/v1/observations` & `GET /api/v1/observation`: Returns the latest normalized real-time weather observation directly from decoded station packets.
  - `GET /api/v1/observations?buffer=full`: Returns the full 15-minute high-frequency raw observation buffer streamed directly to the client via chunked HTTP transfer.
  - `GET /api/v1/history`: Returns aggregated 5-minute bucket history for the preceding 24 hours (up to 288 records) from the RAM history buffer (alias to `/history`).
- **Web Server Weather Logic Refactoring:**
  - Factored out duplicate weather observation extraction logic from `/status` into reusable `WeatherObservationData` and `extract_current_weather_data()` helper functions, ensuring consistent field parsing and unit conversions across all API handlers.
- **Precipitation Engine & Timing Overhaul:**
  - Replaced station-clock bucket tip timestamps (`field_pack->ts`) with gateway monotonic tick counters (`Utils::get_tick_sec()`), eliminating timestamp skew and clock drift where station RTC running ahead of gateway time caused `time(NULL) < newest_ts` and zeroed out rain rates.
  - Extended precipitation rate decay timeout `WS_DATA_PRECIP_RATE_DECAY_TIMEOUT_SEC` from 15 minutes (900s) to 45 minutes (2700s) to maintain continuous rate tracking during light rain / drizzle intervals.
  - Added single-tip rain start estimation on the first bucket tip ($0.254\text{ mm}$) following dry periods, computing an initial decay rate rather than holding 0.0 mm/h until two bucket tips occur.
- **Strict Paired Station Telemetry & Variable Gating:**
  - Gated `WsData::update_precip_rate_params()` and `GlobalEvents::publish(WS_DATA_RECEIVED)` in `ws_handler.cpp` behind `paired_and_not_foreign`.
  - Unpaired neighbor stations and foreign RF sniffed packets are strictly blocked from updating weather variables, UI views, or telemetry logs.
  - Preserved Phase 4 desperate sweep mode: candidate stations immediately latch, write to Local NVS, and transition to paired before packet ingestion.
- **Local Midnight Daily Cumulative Rain Tracking:**
  - Implemented `update_daily_precip()` and `get_daily_precip()` in `ws_data.h` / `ws_data.cpp` using `Settings::get_tz_offset()`.
  - Automatically resets accumulated rain to `0.00` at 00:00 (midnight) local time.
  - Gracefully recovers from station counter wrap/reset and backfills from `WeatherLogger::get_aggregates()` across gateway reboots.
- **REST API & Home Assistant Precipitation Telemetry:**
  - Updated `/status`, `/api/v1/observations`, and `/api/v1/observation` to expose `precip_daily` (daily rain), `precip_cml` (lifetime station total), and `precip_24h` (rolling 24-hour total).
  - Updated `llms.txt` and Home Assistant YAML configuration with `Weather Station Rain Daily` (`state_class: total`) and `Weather Station Rain Total` (`state_class: total_increasing`).
- **UI Dashboard Tile Enhancements:**
  - Changed default top-right dashboard tile title from `"Precip. Rate"` to `"Daily Rain"` with unit `mm` (or `in`).
  - During active rain (`rate > 0.05`), dynamically updates tile title to `"Rain (X.X/h)"` so live intensity and daily accumulation are simultaneously visible.
- **URI Capacity Expansion:**
  - Increased `httpd` server `max_uri_handlers` from 64 to 80 to accommodate new REST API endpoints.

### Fixed & Stability
- **Post-OTA / Boot Splash Screen Freeze & Watchdog Reset:**
  - Diagnosed root cause of device freezing on boot splash screen following OTA flashes: `UiViewSniffer::View::create()` was calling `lv_obj_clean(lv_scr_act())` synchronously to clear child objects while the splash screen was still active.
  - In LVGL `_lv_event_mark_deleted()`, a `while(e) { e = e->prev; }` traversal became trapped in an infinite loop due to cross-task event handling and cyclic pointers in `event_head` originating from temporary stack destruction of `UiViewSplash::View` in `Utils::normal_boot()`.
  - Replaced synchronous `lv_obj_clean(lv_scr_act())` with safe asynchronous teardown (`lv_scr_load(new_scr); lv_obj_del_async(old_scr);`) across all view transitions (`ui_view_sniffer.cpp`, `ui_view_splash.cpp`, `ui_view_wifi_config.cpp`, `ui_view_recovery.cpp`, and `credentials.cpp`).
  - Declared `static UiViewSplash::View view_splash;` in `Utils::normal_boot()` so splash event callbacks and instances remain valid in memory across boot.
  - Added cycle protection (`count > 32`) to `_lv_event_mark_deleted()` in `components/lvgl/src/core/lv_event.c` to prevent any cyclic pointer chain from locking CPU 1 or tripping the Task Watchdog timer.
- **Screenless Guard on Radio Standby:**
  - Guarded `Radio.Standby()` in `ws_handler.cpp` with `#ifndef CONFIG_SCREENLESS` to prevent radio lockups on headless builds.

## [0.8.21] - 2026-09-14

### Added & Architecture Overhaul
- **Automated 5-Phase Fallback & Recovery State Machine (~1 Hour Total Cycle):**
  - **Phase 1: Local NVS (5 min / 300s):** Radio listens on the saved local station frequency (FSK 868.0 MHz for WS1001 or designated LoRa channel for WS1300).
  - **Phase 2: Cloud NVS (5 min / 300s):** Radio listens on the cloud-assigned station profile if distinct from Local NVS (automatically skipped if local and cloud IDs match).
  - **Phase 3: Dedicated WS1001 Scan (5 min / 300s):** Radio locks strictly to Base frequency (868.0 MHz FSK). Accepts any valid WS1001 station ID heard, immediately saves the discovered station to Local NVS (preserving Cloud NVS pristine), enters `PHASE_LATCHED`, and halts scanning.
  - **Phase 4: Dedicated WS1300 Sweep (16 min / 960s):** Sweeps LoRa CH0 through CH7 (868.1 to 869.5 MHz), spending a full 2 minutes (120s) dwelling on each channel. Accommodates WS1300 stations backing off into slow submit modes (up to 120s transmission intervals). Any valid WS1300 packet (ADV, BOOT, or SENSORS for known stations) triggers Instant Latch, arms GPS ownership challenge, and upon confirmation saves strictly to Local NVS, transitioning to `PHASE_LATCHED`.
  - **Phase 5: Cooldown (30 min / 1800s):** Radio parks on the primary target station (Cloud NVS if configured, else Local NVS), completing a full ~56–61 minute (~1 hour) recovery cycle before repeating from Phase 1.
- **Developer & Environment Testing Flags:**
  - Added `#define FALLBACK_ENABLE_PHASE_3_WS1001` compile-time flag in `ws_handler.h` (defaults to `true`). Allows developers to set this flag to `false` to completely bypass Phase 3 and test Phase 4 in RF environments saturated with active WS1001 stations.
- **Web Console Real-Time Fallback Diagnostics:**
  - Updated web console pairing card and status header to display granular recovery phases with live second countdown timers:
    - `⏱ 1: LOCAL NVS (300s)`
    - `⏱ 2: CLOUD NVS (300s)`
    - `⏱ 3: WS1001 SCAN (300s)`
    - `⏱ 4: WS1300 SWEEP (960s)`
    - `⏱ 5: COOLDOWN (1800s)`

## [0.8.20] - 2026-09-13

### Added & UI Polish
- **Web Console Dashboard 5-Column Grid:**
  - Consolidated primary metrics into a single-row 5-column layout (`Wi-Fi`, `WeatherXM Cloud`, `LoRa Radio`, `Station Link`, `Station Ownership`), eliminating vertical clutter and unnecessary card spanning.
  - Kept dormant DOM placeholders for background script backward compatibility without occupying visual row space.
- **Weather Station Pairing Panel Overhaul (Option A - Smart Adaptive View):**
  - **Unified 50-50 Top Row:** Combined the Active Station strip and Candidate Stations container into `#station-pairing-top-row`.
  - **Dynamic Candidate Column:** Under default conditions (no other detected stations), the candidate column is hidden (`display: none`) and the active station spans full 100% width. When candidate stations are detected during RF scan or ambient reception, the row splits into a 50/50 horizontal layout with direct `[PAIR]` / `[PAIR ON REBOOT]` actions.
  - **Eliminated Duplicate Station Displays:** Completely removed the separate bottom "Recently Seen Stations" block, preventing redundant station ID repetition.
  - **Single Compact Strip for Matched State:** When Cloud and Local profiles match, collapses into a single horizontal strip (~44px high) showing station ID, model, source, sync badge, and ownership confirmation badge.
  - **Dual Card Comparison for Diverged State:** Displays only 2 comparison cards (Cloud vs Local) with direct repair and copy actions when profiles differ.
  - **Simplified Operational State Badge:** Replaced redundant hex ID in panel header with concise operational states (`LOCAL TARGET`, `CLOUD TARGET`, `SCANNING`, `UPLINK OFF`).
- **System Health Indicator & Navigation Polish:**
  - Replaced the button-styled live telemetry badge with a dedicated system health indicator.
  - **Normal Operating State:** Displays an unobtrusive pulsing green dot (8px, borderless, textless, non-clickable) with tooltip when Wi-Fi, Cloud, and Station links are all active.
  - **Problem State:** Renders an alert pill with clear diagnostic messaging (`● No Wi-Fi`, `● No Cloud Link`, `● No Station Link`).
  - Leaves `[🤖 AI / Home Assistant]` as the sole interactive navigation button in the header.
  - **Station Ownership "Verify" Logic:** Dynamically hides the `[Verify GPS]` button once ownership has been confirmed.
- **Meshtastic Brand Green Styling (`#5fd589`):**
  - Updated all Meshtastic elements (protocol badges, type tags, packet table accent borders, hover glows, filter buttons, and Live Mesh Evaluator cards) from generic cyan to official Meshtastic brand green (`#5fd589`).
- **Renamed Settings Controls & Clarified Descriptions:**
  - Clarified titles and subtitles for **WeatherXM Cloud**, **WeatherXM LoRa P2P Station Link**, and **Station LoRa TX Power**.

### Changed & Fixed
- **JavaScript Syntax Error Resolution:**
  - Fixed duplicate declaration of `const matchContainer` in `web_server.cpp` that halted script parsing and broke the web console on initial page load.
- **Gateway Editions Framework (WXM vs OPEN):**
  - Introduced compile-time and runtime edition controls (`CONFIG_GATEWAY_EDITION_WXM=1`, `Edition::WXM`, `Edition::OPEN`).
  - In WXM edition, protected the WeatherXM Cloud upload toggle from accidental deactivation, displaying guidance modal explaining cloud upload requirements while enforcing state in NVS.
- **Non-Destructive RF Scanning (`unpair(bool wipe_nvs)`):**
  - Radio scanning no longer wipes stored local station profiles from NVS, enabling non-destructive station discovery while preserving offline fallback.
- **Station Ownership State Persistence:**
  - Persisted station ownership confirmation (`ws_own_conf`, `ws_own_id`, `ws_own_ts`) across gateway reboots in NVS.
  - Station ownership challenge resolves immediately upon receiving busy flags, GNSS packets, or valid GPS coordinates from WS1300.
- **Cloud Profile Synchronization:**
  - Added `copy_cloud_profile_to_local()` and `/api/pair/copy_cloud` endpoint to seamlessly promote cloud-assigned pairing to the local offline profile.
- **Remote Scan Idempotency (`last_scan_id`):**
  - Added NVS tracking for remote scan commands (`rs_scan`) via ThingsBoard shared attributes to prevent repeating scan cycles on reconnection.

## [0.8.14] - 2026-09-13

### Changed & Fixed
- **Boot Splash Screen Hang Resolution (LoRa SX126x Driver Recovery):**
  - **Bounded Timeout in `SX126xWaitOnBusy()`:** Replaced unbounded `while(1)` busy polling loop with a 500 ms bounded timeout. When an unacknowledged peripheral state occurs after a CPU soft reset (such as following a Web OTA update), the function logs an error with bus and pin diagnostics, pulses the SX1262 hardware reset line (`EXPANDER_IO_RADIO_RST`), and safely returns instead of deadlocking `app_main`.
  - **Eliminated FreeRTOS Watchdog Masking:** Because the old loop yielded execution to FreeRTOS with `vTaskDelay()`, the idle task was continuously scheduled and reset the Task Watchdog Timer, masking the deadlock from panic handlers. The new bounded timeout guarantees boot progression into `STATE_SNIFFER` and Wi-Fi initialization under all radio conditions.
  - **Bounded `SX126xGetDio1PinState()` Retries:** Added a 25-retry limit with error logging to prevent infinite loops during I2C bus read failures.
  - **Safe Fallback in `RadioGetFskBandwidthRegValue()`:** Replaced unhandled bandwidth `while(1);` hang with an error log and safe default register setting fallback.
  - **Detailed Boot Milestone Logging:** Added step-by-step `ESP_LOGI` diagnostics across `app_main` and `WsHandler::init()` tracking peripheral initialization milestones (`Tb`, `WsHandler`, `Radio.Init`, `init_state`, `Pressure`, `Submit`, `MeshTelemetry`, `StateSniffer`).

### Added & UI Polish
- **Collapsible Settings Panels (Decluttering):**
  - Added collapsible accordions with rotating chevron indicators (`▼` / `▶`) and real-time status badges for **Adaptive Weather Publishing**, **Meshtastic Network**, and **MeshCore Network**.
  - Panels default to closed on page load for a cleaner, compact settings page layout while allowing instant one-click expansion.
  - **Adaptive Weather Publishing D1 Mode Visibility:** In D1 mode (WS1001 paired), the Adaptive Weather Publishing panel is disabled and styled with a `Mesh Inactive` badge when both Meshtastic and MeshCore protocols are disabled, and automatically enables with active status badges when either mesh network is toggled on.

## [0.8.13] - 2026-09-13

### Changed & Fixed
- **Converted High-Frequency Telemetry Logs to ThingsBoard Client Attributes:**
  - **Packet Errors (`ws_last_err`):** Replaced repeating per-packet `SNIFFER_INVALID_PACKET_RECEIVED` and `SNIFFER_UNVERIFIED_PACKET_RECEIVED` telemetry rows with the `ws_last_err` client attribute (e.g., `"INVALID: 0x00004E6D (ret=3)"`, `"UNVERIFIED: 0x00004E6D"`, or `"OK"`). This prevents recurring unsupported or corrupted radio packets from filling the time-series database while maintaining immediate remote error visibility.
  - **Sensor Health (`ws_sensor_err`):** Replaced per-packet `CRITICAL` telemetry (`SNIFFER_INVALID_SENSOR_VALUES`) with the `ws_sensor_err` client attribute (`"BAD_VALUES"` / `"OK"`). Broken or out-of-range physical sensors no longer spam database records on every packet.
  - **Station GPS Lifecycle (`ws_gps_status`):** Converted `WS_GPS_FIX_REQUESTED`, `WS_GPS_FIX_RECEIVED`, and `WS_GPS_FIX_FAILED` telemetry warnings into a single client attribute (`ws_gps_status`: `"IDLE"`, `"REQUESTED"`, `"FIXED"`, `"FAILED"`).
  - **Link Quality & Packet Loss (`ws_ack_loss`, `ws_lost_packets`):** Replaced per-packet `SNIFFER_ACK_LOSS` and `SNIFFER_WS_PACKET_LOSS` warnings with client attributes: `ws_ack_loss` (boolean flag) and `ws_lost_packets` (cumulative count).
  - **Gateway Time Warning Rate-Limiting:** Removed repeating per-packet `SNIFFER_TIME_INVALID` warning telemetry when gateway time is unsynced; rate-limited local logging to once per state.
  - **Redundant Debug Telemetry Cleanup:** Removed `SNIFFER_PACKET_RECEIVED` and `SNIFFER_ACK_SENT` telemetry rows and raw payload hex dumps on valid normal packets, eliminating hundreds of duplicate entries per hour in `DEBUG` mode while keeping raw hex dumps exclusively for rejected/invalid packets.
  - **System Uptime Telemetry:** Removed minute-by-minute `UPTIME` telemetry logs from `submit_task`; gateway uptime continues to be published periodically as the `device_uptime` client attribute.

## [0.8.12] - 2026-09-13

### Added
- **LoRa Packets Web Table Firmware Version Display:**
  - Both semantic version and raw hex (e.g., `1.2.2 (0x42)`, `1.2.1 (0x41)`) are now displayed in both the **Proto** column and the **Decoded Parameters** column for received LoRa packets.
  - Added `white-space: nowrap;` and optimized table column widths to cleanly display full version identifiers without line wrapping.
- **Station Firmware Warning Client Attribute (`ws_fw_warn`):**
  - Added `ws_fw_warn` to ThingsBoard client attributes (`TB_ATTR_WS_FW_WARN`). If a station with firmware outside field production versions (`< 0x41` or `> 0x42`) is heard, the warning is published to device client attributes rather than repeating telemetry events, preserving cloud database capacity.

### Changed & Fixed
- **Recognize 0x41 (v1.2.1) and 0x42 (v1.2.2) as Field Production Versions:**
  - Updated WS1001 validation so stations running firmware `0x41` (v1.2.1) and `0x42` (v1.2.2) are recognized as standard production hardware and accepted cleanly without any warnings, pairing and operating normally.
  - Removed repeated telemetry warnings (`WS_BETA_STATION_DETECTED`) that could flood backend log tables; non-production versions log locally and update the single `ws_fw_warn` client attribute.

## [0.8.11] - 2026-09-13

### Added
- **Remote Packet Hex Logging for Invalid/Rejected Packets (`rs_dbg_v = 3`):**
  - When debug verbosity is set to `DEBUG` (`rs_dbg_v = 3`), any packet received by the gateway that fails validation or verification has its complete raw hex payload dumped to ThingsBoard telemetry (`log_msg`), enabling remote diagnosis of corrupted or unsupported packets directly from the cloud.
- **Remote Station Signal Telemetry (`SNIFFER_PACKET_RSSI`, `rs_dbg_v = 3`):**
  - When debug verbosity is set to `DEBUG` (`rs_dbg_v = 3`), the gateway publishes `SNIFFER_PACKET_RSSI` (`0x02000011`, decimal `33554449`) with `log_meta1 = rssi` (dBm) and `log_meta2 = snr` (dB) for all received LoRa packets.
  - Added `ws_rssi` and `ws_snr` to ThingsBoard client attributes under `DEBUG` verbosity mode so signal strength is visible directly in device attributes.

### Changed & Fixed
- **Robust WS1001 Packet Validation (Accept Valid CRC with Warnings):**
  - Restructured `Ws1001Helpers::packet_validate()` to verify packet length and calculate CRC (polynomial `0x31`) and checksum first.
  - If CRC and checksum match, packets are no longer rejected with `ERROR_NOT_SUPPORTED` due to unlisted firmware versions (e.g. `0x42`, `0x43`) or non-standard header fields; instead, a warning is logged and the packet is accepted for processing, auto-pairing, and ACK generation.
- **Immediate Station Signal Tracking:**
  - `_state.station_rssi`, `_state.station_snr`, and `_state.station_signal_available` are now updated immediately on every received packet prior to validation, ensuring radio signal metrics are available even if a packet is unverified or rejected.

## [0.8.10] - 2026-09-13

### Added
- **Dedicated WS1001-Only and WS1300-Only Scan Modes:**
  - Added dedicated scan profiles (`WS_TYPE_1001`, `WS_TYPE_1300`, and `WS_TYPE_ALL`) allowing users and remote operators to target specific station hardware without wasting radio dwell time on other frequencies/modulations.
  - **WS1001-Only Scan Mode:** The SX1262 remains locked continuously in LoRa RX on the regional base frequency (e.g. 868.0 MHz / 915.0 MHz) using WS1001 parameters (BW 125 kHz, SF7, CR 4/5, Preamble 12, CRC Off). It never hops to WS1300 channels, ensuring 100% duty cycle coverage on WS1001 periodic broadcasts (~16–20s interval).
  - **WS1300-Only Scan Mode:** Sweeps through the 9 channel slots (Slot 0 base + CH0..CH7) using WS1300 parameters (BW 125 kHz, SF7, CR 4/5, Preamble 8, CRC On), wrapping directly from slot 8 back to slot 0 without dwelling on WS1001 settings.
  - **Combined Dual Scan Mode:** Retained for cases where station hardware is unknown, alternating between WS1001 dwell and WS1300 sweep.
- **ThingsBoard Shared Attribute Scan Profile Selection (`rs_scan`):**
  - Enhanced `rs_scan` shared attribute parser to accept explicit station profiles: `"1001"`, `1001`, or `"ws1001"` triggers dedicated WS1001 scan; `"1300"`, `1300`, or `"ws1300"` triggers dedicated WS1300 scan; `"all"` or `"combo"` triggers combined scan; boolean `true` defaults to the device's configured station type (defaulting to WS1001).
- **Web UI & REST API Enhancements:**
  - Updated `POST /set_pair_mode?mode=scan&type=1001|1300|0` to pass the desired scan profile.
  - Web UI pairing controls automatically propagate the active station profile selection to the scan command.
  - Exposed `"scan_profile"` in `GET /status` telemetry and updated scan ETA / total cycle time calculations to reflect active scan profiles accurately.

### Fixed
- **LoRa Modulation Architecture for WS1001:**
  - Confirmed radio configuration strictly uses LoRa modulation (BW 125 kHz, SF7, CR 4/5, Preamble 12, CRC Off, private sync word `0x1424`) for WS1001 reception.
- **Scan Override Bug in Web Server:**
  - Removed unconditional override in `post_set_pair_mode_handler` that previously forced `g_receiver_profile = WS_TYPE_ALL` and `apply_station_change(WS_TYPE_ALL)`, preserving the user-selected profile during scanning.

## [0.8.9] - 2026-09-12

### Added
- **Remote Cloud Reboot via Shared Attribute (`rs_reboot`):**
  - ThingsBoard shared attribute `rs_reboot` (timestamp, sequence nonce, string integer, or boolean `true`) allows triggering remote gateway restarts directly from the ThingsBoard dashboard.
  - Stored in NVS under `"settings"` namespace (`reboot_id`) with strict deduplication (`req_id > last_reboot_id`), completely eliminating the risk of infinite reboot loops when ThingsBoard retains shared attributes indefinitely.
  - Setting `rs_reboot: false` or `0` resets the stored ID, allowing future boolean re-triggers.
  - Safe restart using `RestartReason::REMOTE_REBOOT`, recorded in RTC breadcrumbs and post-boot telemetry diagnostics.
- **Consecutive OTA Failure Recovery Watchdog:**
  - Automatic clean restart after 5 consecutive failed OTA attempts across ~25 minutes to reset lwIP sockets, TLS session buffers, and heap fragmentation.
  - Clean MQTT OTA state cleanup prior to rebooting with `RestartReason::OTA_FAILED`.
  - Counter is automatically reset on successful OTA update, or when the version online is already current/older.
- **Remote Force Scan Shared Attribute (`rs_scan`):**
  - ThingsBoard shared attribute `rs_scan: true` immediately commands the gateway to enter a 10-minute auto-scan, prioritizing WS1001, allowing remote unsticking of customer devices without local intervention.
- **Server-Side Station ID Provisioning & Auto-Adoption:**
  - Added `rs_ws_id` and `rs_scan` to `RS_TB_ATTRIBUTE_REQ_JSON` and requested `clientKeys: "ws_fo_id"`.
  - Added parsing for numeric/hex `rs_ws_id` on ThingsBoard shared attributes for WS1001 stations.
  - Automatic fallback adoption of legacy `ws_fo_id` from client attributes if cloud profile is missing.
- **WS1001 Continuous RX Watchdog:**
  - Defensive re-arm of SX1262 continuous reception (`sx126x_rx_cont()`) upon consecutive packet timeouts for WS1001.
  - Full radio re-initialization after prolonged silence to recover from any SPI or receiver lockups.
- **Local HTTP API & LLM Documentation:**
  - Added `/llms.txt` and `/llms-full.txt` endpoints adhering to the llms.txt standard, exposing complete REST endpoint schemas and Home Assistant `configuration.yaml` templates.

### Fixed
- **MQTT Multi-Fragment Reassembly (OTA Download Resilience):**
  - Fixed critical OTA download failure over WAN / remote connections caused by TCP/TLS packet fragmentation in `esp-mqtt`.
  - In `esp-mqtt`, when a 4096-byte chunk arrives across multiple TCP packets (e.g. PPPoE MTU 1492 / MSS 1452 bytes), multiple `MQTT_EVENT_DATA` events are dispatched with `current_data_offset > 0` and `event->topic = NULL`.
  - Previously, `tb.cpp` immediately signaled completion upon receiving the first fragment (e.g. 1452 bytes), causing a "Short chunk" mismatch and immediately failing Chunk 0.
  - `tb.cpp` now accumulates incoming fragments at their respective `current_data_offset` and only signals completion (`xTaskNotify`) once `current_data_offset + data_len >= total_data_len`.
  - Added a 1-second backoff delay between chunk retry attempts and preserved granular failure reasons in telemetry error reporting.

## [0.8.8] - 2026-09-12

### Added
- **Dual-Stage Auto-Cycle Scanning:**
  - Alternating scan cycle between WS1001 base frequency (60s fast pass on cycle 0, 300s deep pass on cycle 1+) and WS1300 9-channel sweep (9 slots × 22s = 198s total) to ensure an unpaired gateway never sits idle.
  - Web UI diagnostics dynamically display the active scan phase (WS1001 base vs WS1300 channel sweep), current cycle count, and cycle ETA.
- **Sleep-Aware Station Retention (Overnight Low-Battery Protection):**
  - Paired stations are no longer prematurely unpaired upon reaching the 5-10 minute inactivity timeout (`ActivityStatus::INACTIVE`).
  - For paired WS1001 stations, the receiver remains continuously locked on the region base frequency overnight without hopping through WS1300 channels.
  - Foreign station auto-pairing is blocked while paired, preventing neighbor stations from hijacking the gateway during low-battery night sleep.
  - Upon solar recharge at dawn, reception immediately transitions back to `ACTIVE_OK` on the first received packet.
- **24-Hour Abandonment Fallback:**
  - Extended inactivity threshold: only after 24 hours of continuous silence (`WS_INACTIVE_ABANDON_SEC = 24 * 3600`) does the gateway assume a paired station is permanently decommissioned or replaced, falling back to open auto-scan.
- **Cloud Profile Prioritization:**
  - When scanning finds a station matching configured cloud credentials, the gateway restores and locks to `PAIR_MODE_CLOUD` mode instead of generic `PAIR_MODE_LOCAL`.

### Changed
- **Default Telemetry Configuration:**
  - Meshtastic environmental telemetry and MeshCore gateway telemetry are now disabled by default on clean installations and wiped NVS configurations (`.mesh_env_telemetry_enabled = false`).

## [0.8.7] - 2026-09-12

### Added
- **Adaptive Weather Publishing & Threshold Submission Triggers:**
  - Implemented configurable threshold evaluation across 11 weather variables (`AdaptivePublish`):
    - Temperature (default: 2.0 °C), Humidity (10 %RH), Pressure (3.0 hPa), Wind Speed (3.0 m/s), Wind Gust (5.0 m/s), Wind Direction (shortest circular angle: 45°), Rain Accumulation (0.5 mm), Rain Rate (2.0 mm/h), Illuminance (25,000 lux), Solar Radiation (200 W/m²), and UV Index (2.0).
  - Multi-condition event detection and reason tagging: rain start/stop transitions (`REASON_RAIN_STARTED`, `REASON_RAIN_STOPPED`), periodic heartbeats (`REASON_HEARTBEAT`), and manual triggers (`REASON_MANUAL`).
  - Wire-visible reason bitmasks and diagnostic reporting for all adaptive weather packet transmissions.
  - Gateway mesh ~5s cooldown guard between transmissions.
  - Phase-offsetting (`interval / 2`) between gateway status telemetry and station weather broadcasts to prevent channel collisions and RF congestion.
- **SD Card Storage & Offline Logging Hardening:**
  - Enhanced SD card writer task error handling, retry resilience, and mount recovery in `bsp_sdcard.c` and `weather_logger.cpp`.
  - Hardened file system synchronization and write flushes for raw weather observations and 5-minute historical aggregates (`MAX_AGGREGATES = 288`).
  - Robust directory traversal and corruption prevention during high-throughput logging.
- **Meshtastic & MeshCore Protocol & Identity Enhancements:**
  - **Hardware Certificate Fingerprinting:** Derived immutable, deterministic Meshtastic Node IDs (`!xxxxxxxx`) and MAC addresses directly from the gateway's secure element device certificate fingerprint in `credentials.cpp`.
  - **Custom Node Naming Support:** Added operator-configurable Long Name and Short Name overrides for Meshtastic and MeshCore interfaces, persisted in NVS (`mesh_long_name`, `mesh_short_name`), with live Web UI controls and REST endpoints.
  - Updated default device naming conventions to WG1200 series with `G-` short name prefixes (e.g. `!abcd1234` $\rightarrow$ `G-1234`).
- **LoRa Collision Avoidance & Channel Guarding:**
  - Added pre-submit LoRa collision guard in `submit.cpp` and `ws_handler.cpp` to monitor channel activity and avoid transmitting mesh packets during active weather station reception.
  - Increased packet timeout tolerance and tuned LoRa collision avoidance timing windows to eliminate self-interference.
  - Added 5-minute throttling for attribute and OTA queries to minimize mesh overhead.
- **Web Dashboard & Real-Time Protocol Visualization:**
  - Added categorized live protocol logging in the Web UI with color-coded streams for Weather Station (WS1001/WS1300), Meshtastic, and MeshCore packets.
  - Expanded in-memory ring buffer log capacity for comprehensive on-device diagnostic inspections.
  - Added interactive Web UI cards for adaptive publish threshold configuration and real-time delta tracking.
- **Weather Station Reception & Sliding Window Tracking:**
  - Implemented rolling window packet reception algorithm (`packet_reception.h`) tracking sequence counter increments and missed packets.
  - Replaced crude packet counters with robust percentage reception metrics across both WS1001 and WS1300 stations.
  - Improved sequence tracking wrap-around handling and out-of-order packet filtering.
- **Legacy WS1001 Boot NV Migration (OTA 0.6.1 Compatibility):**
  - Added automatic boot-time migration in `Settings::init()` for gateways updated via OTA from legacy firmware (v0.6.1).
  - Automatically migrates existing local pairing credentials (`ws_id`, `ws_type`, `ws_sn`, `ws_pubkey`) to cloud profile storage (`cloud_ws_*`), setting `cloud_ws_valid = true` and `pair_mode = PAIR_MODE_CLOUD`.
  - Enables instant, offline auto-pairing with paired WS1001 stations on first boot after OTA update without requiring web console interaction or ThingsBoard connectivity.
- **Conditional ThingsBoard Client Attributes Reconciliation:**
  - Added fallback client attribute query (`clientKeys: "ws_fo_id,ws_id"`) triggered only when NV is completely unconfigured or wiped.
  - Automatically reconstructs WS1001 pairing configuration from ThingsBoard `ws_fo_id` and commits to NV.
  - Gateways with intact NV configuration preserve network bandwidth by requesting shared attributes only.
- **Station ID Attribute Overwrite Guard:**
  - Hardened client attribute telemetry submission in `tb_json.cpp` to never publish `0x00000000` when unpaired or during transitional states.
- **Radio Profile WS1001 Safeguard:**
  - Explicitly binds receiver profile to `WS_TYPE_1001` when configured for WS1001, avoiding WS1300 channel sweep and frequency hopping.
- **Platform & Bootloader Resilience:**
  - Wrapped panic and watchdog restart handlers to prevent spurious or cascading resets from triggering factory partition rollbacks.
  - Updated digital signature signing to use thread-safe hardware acceleration with AES disabled, eliminating hardware resource contention during cryptographic operations.

## [0.8.5] - 2026-08-21

### Added
- **Default Telemetry Interval & Flood Routing Airtime Advisory:**
  - Updated default submit intervals for Meshtastic and MeshCore gateways and paired stations to 60 minutes (1 hour) to reduce mesh congestion.
  - Added interactive educational warning banners in the Web UI that automatically alert operators when choosing intervals shorter than 60 minutes regarding flood routing, duty cycle limitations, and channel saturation.
- **Dynamic RF Parameter Configuration (Meshtastic & MeshCore):**
  - Added full user-configurable radio parameters: Frequency (Hz), Bandwidth (kHz), Spreading Factor (5–12), Coding Rate (4/5–4/8), and explicit Transmit Power (dBm).
  - Added support for all 8 standard Meshtastic modem presets (LongFast, LongSlow, MediumFast, MediumSlow, ShortFast, ShortSlow, LongModerate, ShortTurbo) and Custom override (255).
  - Added support for MeshCore channel profiles (LongRange, Medium, Fast, ShortTurbo, EU/UK Narrow) and Custom override (255).
- **Gateway as MeshCore Sensor Node:**
  - Implemented standalone gateway environmental and status telemetry broadcasting using standard MeshCore frame encoding (`0x19 GRP_DATA` / `0x06` telemetry).
  - Added interactive Web UI card with profile select, live TX status, and manual "Broadcast MeshCore Packet Now" button.
  - Added REST endpoint `POST /api/v1/meshcore/send_telemetry`.
- **MeshCore Telemetry Ingestion (`0x19 GRP_DATA` / `0x06` Telemetry):** Added standard MeshCore wire-format frame decoding into canonical `FieldPack` sensor structures with cryptographic header parsing.
- **MeshCore Multi-Station Forwarding Router:** Added toggleable cloud ingestion routing for un-paired MeshCore sensor nodes (`mesh_multi_station_forwarding_enabled`), persisted in NVS (`mesh_fwd_en`, default: disabled).
- **WS1300 v18 Downlink Protocol Evolution (`RespCmdV3`):** Extended station remote control downlink packet to fixed 29-byte format with 128-bit HMAC-SHA256 authentication, allowing dynamic over-the-air setting of station Meshtastic and MeshCore transmit intervals and profile indices.
- **Remote Observation Queuing & Radio Leasing:** Implemented decoupled observation queueing and time-budgeted SX1262 radio leasing for non-interfering mesh frame transmission.
- **Wi-Fi Provisioning Hardening:** Open AP mode for initial setup and atomic credential commit only after successful Wi-Fi connection test.
- **Web UI & REST API Convergence:**
  - Added "Station as MeshCore Sensor Node" interactive control card and "MeshCore Multi-Station Forwarding" toggle switch to the Web UI.
  - Added REST endpoints `POST /ws1300_station_meshcore` and `GET/POST /api/v1/meshcore/config`.
  - Added `mesh_forwarding_enabled`, `station_meshcore_interval_min`, and `station_meshcore_command_pending` to `/status` JSON telemetry.
  - Raised `max_uri_handlers = 48` in `web_server.cpp`.
- **Meshtastic Multi-Region & Preset Channel Plan:** Added complete regional frequency plan (EU868, US915, AS923, AU915, IN865) and modem preset mapping in `mesh_telemetry.cpp`.

## [0.8.4] - 2026-08-14

### Added
- **Meshtastic Transmit Power Web UI Toggle:** Added a Web UI toggle switch to select between Low Power (10 dBm default) and Full Transmit Power (22 dBm for EU868 / 14 dBm for other regions).
- **REST API Parameter:** Added `tx_power_high=true|false` parameter to `POST /api/v1/meshtastic/config` and added `tx_power_high` and `tx_power_dbm` fields to `GET /api/v1/meshtastic/config` JSON responses.

### Changed
- **Default Meshtastic Transmit Power Set to 10 dBm:** Temporary/default Meshtastic transmit power is set to 10 dBm to prevent near-field LNA receiver saturation on nearby nodes (e.g., 2 meters apart).

### Fixed
- **SX1262 SyncWord Register Clobbering in `Radio.Send()`:** Fixed an issue where `Radio.Send()` invoked `SX126xSetPacketParams()`, which reset internal LoRa SyncWord registers (`0x0740`/`0x0741`) to default `0x1424`. Restored active SyncWord registers across `SX126xSetPacketParams()` in `components/lora/radio.c`.

## [0.8.3] - 2026-08-10

### Added
- **Polite Meshtastic telemetry scheduler:** The Web UI interval is now the
  base cadence `I`: environmental telemetry is sent every `1 × I`, NodeInfo
  every `2 × I`, device metrics every `3 × I`, and Position every `6 × I`.
  Only one due frame is emitted per safe post-station radio window. Before
  each frame, the WG1200 waits a short randomized contention period and runs
  two SX1262 LoRa CAD passes; a busy or indeterminate channel defers the frame
  to the next safe window instead of transmitting over it.

### Fixed
- **Meshtastic environmental telemetry failing PHY CRC on receivers (regression
  since v0.8.0):** Environmental frames were transmitted but rejected by
  receivers with a payload CRC mismatch before protobuf decoding, while other
  Meshtastic frame types mostly worked. Root cause: four code paths reachable
  from the web server task and from `state_sniffer` (`apply_station_change`,
  `notify_sniff_mode_toggled`, `set_manual_listen_slot`, `unpair`) could retune
  the shared SX1262 with no coordination against an in-flight Meshtastic
  transmit. Landing mid-frame during the ~930 ms SF11 airtime corrupts the tail
  of the packet after the receiver has already validated the explicit header
  (which carries its own CRC) - reported downstream as a payload CRC failure,
  not a lost packet. Environmental telemetry is sent far more often than the
  other Meshtastic frame types (NodeInfo, Device, Position), so it absorbed
  almost all of the resulting contention.
  Fixed by adding a recursive radio ownership mutex (`radio_lock()` /
  `radio_unlock()`) covering every function that reprograms the radio,
  including the four call sites above and the full Meshtastic borrow in
  `sx126x_tx_meshtastic()` (config through TX-done restore, not just the
  pre-transmit setup). The restore that used to run in the TX-done IRQ
  callback now runs inline in the borrowing task to avoid the callback
  deadlocking against the lock; the callback only publishes a sequence counter
  during a mesh borrow. Also moved the post-write sync-word register readback
  from immediately after the write (up to ~750 ms before the actual transmit,
  across the LBT contention pause and two CAD passes) to immediately before
  `Radio.Send()`. Added `get_radio_contention_count()` - a counter of retune
  attempts deferred because a mesh transmit was in flight - as field evidence
  that the race was real and is now being caught rather than silently
  corrupting frames.
  - **SX1262 crystal loading capacitance trim (carrier frequency alignment):**
    Corrected `RadioStandby()` in `components/lora/radio.c` to set `xt_config = 5` (`0x05`), matching the $8.0\text{ pF}$ load capacitance requirement of crystal `X4` (`32 MHz, 10 ppm, 8 pF`) on the WG1200 v1.3 hardware schematic. Per Semtech `DS_SX1261-2`, `0x05` provides $13.6\text{ pF}$ per pin ($C_{\text{pin}} = 13.6 + (n-5) \times 0.47\text{ pF}$), which in series with $\sim 1.2\text{ pF}$ PCB stray capacitance lands $C_L$ precisely at $8.0\text{ pF}$. Replaced legacy vendor value `16` ($\sim 10.6\text{ pF}$ over-loaded), which pulled carrier frequency offset and caused receiver PHY CRC Mismatch errors (`error=-7`) over long SF11 Meshtastic airtime frames (~723 ms). Improves both Meshtastic SF11 frame decoding and WS1001 station link IF passband alignment.
  - **Monotonic non-zero telemetry timestamps (receiver deduplication fix):**
    `build_telemetry()` now falls back to monotonic uptime (`now_sec()`) when wall time is un-synced (`time(NULL) <= 1600000000u`), preventing Meshtastic client receivers (`TelemetryModule`) from discarding subsequent telemetry frames as duplicate stale packets ($0 \le 0$).
  - **Boot identity announcement & non-zero uptime guard:**
    Guaranteed `now_sec() >= 1` so that $t=0$ uptime does not collide with the `g_last_nodeinfo_ts == 0` sentinel. `MeshTelemetry::tick()` broadcasts `NodeInfo` first on boot so client nodes establish `nodeDB` identity before environmental telemetry arrives, falling through gracefully if a transmission fails.
  - **Meshtastic environmental airtime:** Environmental protobufs now preserve
    the core temperature/humidity/pressure readings while omitting zero-valued
    optional wind, rain, and light readings. This shortens calm, dry, or dark
    SF11 packets and reduces collision exposure.
- **Five-minute SD logger crash and destructive factory fallback:** Moved the
  CSV path, row, header-validation, request, daily-summary, and error buffers out
  of nested FreeRTOS stack frames, doubled the low-priority SD writer reserve to
  12 KiB, and added a runtime minimum-free-stack diagnostic. This prevents the
  first five-minute write from overflowing the writer stack and panic-resetting
  into the bootloader's active-low factory-reset path, which could erase NVS and
  select the factory application.
- **Reliable local WS1300 pairing after discovery:** A sensor-only WS1300 in the
  recent-station directory now offers `PAIR ON REBOOT`. The action enters exact-ID
  targeted SCAN and locks the station's observed channel before asking for a
  reboot, so the one-shot BOOT packet is not left to chance during the nine-slot
  sweep. A successful pair replaces only Local; the independent Cloud assignment
  remains cached and selectable.

## [0.8.2] - 2026-07-30

### Fixed
- **WS1001 Base Channel Pairing Fix:** Fixed station pairing logic for WS1001 devices so that pressing "PAIR" locks channel selection to `base` (slot 0) instead of using WS1300 serial-number-based channel calculation (`CH0..CH7`). WS1001 stations operate exclusively on the base channel.

### Added
- **Configurable Meshtastic Hop Limit:** Added a "Hop limit" dropdown (options 1 to 5, default 3) next to "Broadcast interval" in the web UI. Transmitted Meshtastic telemetry frames dynamically adjust `hop_limit` and `hop_start` header flags to match the configured NVS setting.

## [0.8.1] - 2026-07-30

### Added
- **Four-slot station directory:** The Web UI now presents the persisted Cloud
  and Local profiles plus the two most recently decoded stations. Recent
  stations are RAM-only and reset at boot, so normal LoRa traffic causes no NVS
  writes. ADV/BOOT entries offer exact-ID passive PAIR; sensor-only entries are
  clearly LISTEN-only unless they match a saved WS1300 eligible for targeted
  REPAIR.
- **Explicit gateway operating state:** Dashboard and pairing controls now
  distinguish CLOUD TARGET, LOCAL TARGET, PASSIVE SCAN, TARGETED REPAIR,
  LISTEN ONLY, and OFF. Targeted pair/repair actions show their exact station
  ID and state that successful pairing replaces Local while preserving Cloud.
- **Independent cloud/local station profiles:** The cloud-assigned WS identity and
  locally scanned WS identity are now persisted separately. CLOUD and LOCAL select
  between them without destroying the other profile; OFF preserves the selected
  source, and SCAN replaces only LOCAL.
- **Pairing identity diagnostics in the Web UI:** Cloud assignment ID/SN, local
  pairing ID/SN, selected source, actual radio source, profile completeness, and
  configured-vs-target-traffic state are always visible. Switching between different
  saved targets requires confirmation.
- **WG1200 reboot/NVS Phase 1 diagnostics:** Records reset source, controlled
  restart reason, bootloader factory-reset handoff, bootloader SHA-256, app-entry
  GPIO10/GPIO38 levels, and NVS usage outside NVS and in ThingsBoard attributes.
- **Backlight wake attribution:** All application backlight writes now pass
  through one stateful API and report boot, button, OTA, or settings wake causes.
- **WS1300 v15+ LoRa Downlink `RESP_CMD` V2 Support**: Added remote downlink configuration for "Station as Meshtastic Sensor Node" for WS1300 station firmware v15 or higher.
- **Station Meshtastic Web UI Controls**: Added Web UI toggle and interval dropdown (5, 10, 15, 30, 60 minutes) to configure direct station Meshtastic telemetry broadcasts over LoRa.
- **Richer Meshtastic Telemetry**:
  - `DeviceMetrics`: Added station battery voltage, low battery flag, and system uptime transmission over Meshtastic Port 67.
  - `rainfall_24h`: Added 24-hour accumulated rainfall field to `EnvironmentMetrics` derived from `WeatherLogger` SRAM aggregates.
  - `is_unmessagable = true`: Marked node as read-only sensor in NodeInfo broadcasts.
  - Fixed Lux decoding scaling and nighttime error sentinel filtering.

### Fixed
- **SCAN could capture the wrong WS1300:** Merely entering SCAN no longer sends
  authenticated reboot commands to already-heard stations. Reboot-to-repair is
  now restricted to the explicit, targeted station-kick action, so a nearby
  station's BOOT cannot win ahead of the intended station's ADV.
- **Profile source runtime consistency:** Signature verification now uses the
  public key from the active cloud/local profile, and a successful SCAN clears
  the runtime auto-pair flag when it settles into LOCAL.
- **Cloud WS1300 serial parsed as decimal prefix:** A 20-hex-character
  `rs_ws_sn` such as `39FA23...` could fall through to `strtoul(..., 10)` and
  become station ID `39` (`0x00000027`) while the LCD retained the full serial.
  Cloud serials are now length/type checked, decoded as 10 bytes, and validated
  against the derived 32-bit station ID.
- **Unsafe SCAN-to-cloud adoption:** SCAN no longer emits an automatic adoption
  request that a server rule could use to overwrite `rs_ws_*`. The cloud
  assignment remains unchanged until an explicit future user-authorized flow.
- Removed the `LV_EVENT_GET_SELF_SIZE` callback that could wake a dark screen
  during ordinary LVGL layout updates without any user input.
- Hardened controlled restarts against the historical GPIO10 and current GPIO38
  bootloader factory-reset inputs, including waiting for the active-low display
  button to be released.
- Prevented touch-calibration and optional LoRaWAN helpers from blindly erasing
  the shared default NVS partition. `NEW_VERSION_FOUND` is now preserved rather
  than erased; `NO_FREE_PAGES` recovery records a pre-erase partition hash.

## [0.8.0] - 2026-07-27

**Prototype. WG1200 hardware only.** Not yet reviewed with the Meshtastic project.

### Added
- **Meshtastic environmental telemetry beacon (`main/ws/mesh_telemetry.cpp`).**
  Broadcasts the paired station's latest observation as a standard Meshtastic
  `TELEMETRY_APP` / `EnvironmentMetrics` packet on the public default channel, so
  any stock Meshtastic node in range shows live weather on its Environment screen
  with zero configuration. Transmit only — the gateway never receives, decodes,
  routes or rebroadcasts Meshtastic traffic, and is not a mesh participant.
  - Fields sent: `temperature` (°C), `relative_humidity` (%),
    `barometric_pressure` (hPa), `lux`, `wind_direction` (deg), `wind_speed` (m/s),
    `wind_gust` (m/s), `rainfall_1h` (mm). WsFields base units are already SI and
    match the Meshtastic definitions exactly, so no conversion is applied.
  - Solar radiation (W/m²) is deliberately **not** sent. The only candidate field,
    `EnvironmentMetrics.radiation`, is ionizing radiation in µR/h and would be
    rendered as a Geiger reading.
  - Periodic `NODEINFO_APP` announcement (every 30 min) so the station appears with
    a name and `SENSOR` role rather than as an anonymous `!hhhhhhhh`. Hardware model
    is reported as `PRIVATE_HW` — this is not a Meshtastic device and does not claim
    to be one.
  - Node number is derived from the eFuse MAC and is therefore **stable across
    reboots**. A per-boot or per-packet random identity would inject a new ghost node
    into every nearby user's node database on every transmit.
- **`main/ws/include/mesh_frame.h`** — platform-independent Meshtastic frame
  encoder: PHY header, AES-128-CTR, and hand-written protobuf serialization.
  Hand-encoded rather than generated so that no GPL-3.0 sources from
  `meshtastic/protobufs` are linked into this firmware.
- **`tools/mesh_frame_test/`** — wire-format test. Builds frames with the real
  encoder and decodes them with an independent Python implementation written from
  the Meshtastic spec, asserting every header field, the channel hash, the crypto,
  and every protobuf field number. Run `./run.sh`.
- `WsHandler::sx126x_tx_meshtastic()` — borrows the shared SX1262 for a single
  foreign-parameter transmit and hands it back.
- NVS-backed settings `mesh_env_telemetry_enabled` (default on) and
  `mesh_env_telemetry_interval_sec` (default 600 s).
- HTTP API:
  - `POST/GET /api/v1/meshtastic/send_telemetry` — queues a transmit for the next
    safe radio window and returns immediately. Poll `sent_count` to confirm.
  - `GET/POST /api/v1/meshtastic/config` — enablement, interval, and the node's
    Meshtastic identity, frequency and channel.
- **GPS Position Packet:** Transmits standard Meshtastic Position packets (`PORTNUM_POSITION_APP = 3`, containing `latitude_i`, `longitude_i`, and `time`) immediately before environmental telemetry packets whenever a valid, unexpired GPS fix is present (`WsGps`).

### Fixed
- **Protobuf Telemetry `time` field encoding:** Updated `mesh_frame.h` to encode `Telemetry.time` (field 1) using 32-bit fixed wire type (`FIXED32`), matching the Meshtastic protobuf specification and allowing standard Meshtastic nodes (e.g. Wio Tracker) to decode environmental telemetry packets seamlessly.
- **Node Short Name formatting:** Formatted node short name as `D2` + MAC_LAST (4 characters e.g. `D23C`) to strictly satisfy the 4-character protocol limit for Meshtastic `short_name`.
- **Boot Telemetry Guard:** Updated `MeshTelemetry::tick()` in `mesh_telemetry.cpp` to require both an active station pairing (`WsHandler::get_paired_id() != 0`) and valid weather observation data before transmitting any Meshtastic packet (NodeInfo or Telemetry) upon boot.

### Airtime and radio-sharing notes
- **`hop_limit` is 0.** Direct neighbours receive the packet; nobody rebroadcasts
  it. One station observation therefore costs the mesh exactly one airtime event.
  At `hop_limit` 3 every station would multiply its traffic by the size of its
  neighbourhood — across a deployed fleet that is a denial of service on someone
  else's network. Do not raise this without talking to the Meshtastic project first.
- **`want_ack` is clear** and no reply is ever expected. Fire and forget.
- TX power is fixed at a conservative 14 dBm in all regions.
- The SX1262 is shared with the weather station link. A Meshtastic transmit retunes
  the modem (869.525 MHz, SF11/BW250/CR4-5, sync word 0x2b) and the gateway is deaf
  to its own station for roughly one second. To bound that exposure the beacon is
  driven from inside the packet handler task, immediately after a station packet has
  been serviced — the point in the cycle where the next station transmission is
  furthest away. There is deliberately **no** independent telemetry task; a second
  task touching the radio would race the station link and cost observations.
- Restore is handled by the existing `sx126x_tx_cb` → `set_radio_freq` +
  `sx126x_rx_cont` path, which reapplies the station frequency, modem config and
  private sync word. The frequency restore is now unconditional after a Meshtastic
  transmit; the previous `PAIRED` guard would have stranded an unpaired gateway on
  the Meshtastic frequency.

### Known limitations
- Region mapping covers **EU868 only** as verified (869.525 MHz). US915 (906.875 MHz)
  is included but unverified against real hardware. All other regions return no
  frequency and the beacon stays silent rather than guessing a slot.
- Screenless builds (WG1400) are **not supported**: those targets drive the radio
  over the MeshCore KISS link, which needs its own framing support.
  `sx126x_tx_meshtastic()` is a logging stub there.
- No listen-before-talk / CAD before transmit. Stock Meshtastic nodes do this;
  required before any wider deployment.
- No `Position` packet, so the station does not appear on the Meshtastic map.
- Trademark and GPL questions around shipping a "Meshtastic" feature in proprietary
  firmware are open and need a legal answer before release.

## [0.7.7.8] - 2026-07-25

### Added
- **SHA-256 verification of OTA downloads (`submit.cpp`):** the `fw_checksum` and
  `fw_checksum_algorithm` attributes were previously *required to be present and then
  never used*, leaving the Secure Boot signature at `esp_ota_end()` as the only integrity
  gate. That proves an image is authentic but not that it is the image ThingsBoard
  intended to serve, and it reports nothing diagnosable when it fails. The image is now
  hashed and checked before the update is finalized.
  - Verification hashes the image **read back from flash**, not the chunks as they stream
    in. A streaming digest must hold one SHA-256 context open for the whole multi-minute
    transfer, competing with TLS and the DS peripheral for the shared hardware SHA engine;
    a disturbed save/restore there corrupts the digest even when every flashed byte is
    correct. Hashing afterwards uses one short-lived context and additionally verifies what
    actually landed in flash, catching write errors a RAM-side digest cannot see.
  - Reads into an internal-RAM buffer, because the ESP32-S3 hardware SHA crypto DMA cannot
    coherently read external PSRAM.
  - **Fails closed:** nothing reaches `esp_ota_end()` unless the flash contents match the
    expected digest. Valid only while flash encryption remains disabled.
- **Per-chunk length validation:** a short chunk is now rejected and retried instead of
  being written silently, which previously produced a truncated image that only failed far
  later at signature verification.
- **Final byte-count check** (`bytes_written == fw_size`) before verification.
- **Early size rejection:** an image larger than the OTA slot is refused before anything is
  erased.
- **OTA failure telemetry** (`OTA_TB_FAILED_DETAILS_JSON_FMT`) carrying `ota_verdict`,
  `ota_checksum_expected`, `ota_checksum_flash`, `ota_chunk`, `ota_bytes_written` and
  `ota_bytes_expected`, so a failed update can be diagnosed from ThingsBoard without a
  serial console. Verdicts: `download-failed`, `length-mismatch`, `verify-failed`,
  `checksum-mismatch`.

### Fixed
- **OTA handle leak on every failed download (`submit.cpp`):** the failure path returned
  without `esp_ota_abort()`, leaking the handle and leaving the partition partially
  written. All post-`esp_ota_begin()` failure paths now abort exactly once.
- **Buffer overrun reading the OTA version attribute (`submit.cpp`):** `version_str` was
  `char[10]` filled by `strncpy(.., sizeof(buf))`, so any attribute of 10 characters or
  more (e.g. `0.7.7.7-g9b573c4`) left the buffer **without a NUL terminator** and every
  later read overran it. Now `char[32]`, length-validated and explicitly terminated.
- **Divide-by-zero in OTA progress logging (`submit.cpp`):** `bytes_written / (fw_size / 100)`
  panicked for any `fw_size < 100`, a value that passes the existing `< 1` guard. Now
  computed as `(bytes_written * 100) / fw_size`.
- **Lost-response race on every OTA chunk (`submit.cpp`, `tb.cpp`):** the response wait was
  armed *inside* `wait_response()`, i.e. after the chunk request had already been
  published. A reply arriving faster than the task could arm was discarded, costing a full
  response timeout plus a retry. Added `start_wait_response()` / `await_response()` /
  `cancel_wait_response()` so the wait is armed **before** publishing. Purely additive —
  `wait_response()` is unchanged and its other callers are unaffected.
- **Web UI OTA used `esp_ota_end()` on abort paths (`web_server.cpp`):** five error paths
  (allocation failure, connection lost, bad header magic, write failure, payload too small)
  now call `esp_ota_abort()`. The single genuine commit call is unchanged.
- **OTA request topic no longer borrows `_mqtt_resp.topic`**, the field the receive path
  matches on; it is built in its own buffer.

### Changed
- Bumped the WG1200 firmware version from `0.7.7.7` to `0.7.7.8`.
- `esp_ota_begin()` is called with the real image size instead of `OTA_SIZE_UNKNOWN`, so
  only the sectors actually needed are erased rather than the full 4 MB slot.

### Notes
- ThingsBoard firmware records **must** carry a correct `fw_checksum` matching the exact
  signed binary served. A record whose checksum belongs to a different build (signed vs
  unsigned images differ by exactly 4096 bytes, the signature sector) will now be rejected
  rather than silently installed.
- Changing a device's assigned firmware **while a download is in flight** yields a torn
  image (chunks from two builds) and will surface as `checksum-mismatch`. Freeze firmware
  assignments during a rollout wave.

## [0.7.7.7] - 2026-07-24

### Added
- **Automatic Station Kick during SCAN:** Extended unpaired channel scanning so that receiving a WS1300 telemetry/SENSE packet (`SENSORS` or `GNSS`) automatically transmits an authenticated CMD reboot (station kick). If the station was previously paired with this gateway's serial, it accepts the reboot, restarts, and emits a pairable `STA_BOOT` packet, allowing the gateway to automatically complete local pairing.
- **Backoff GPS Fix Requests:** Updated GPS fix request logic to use a single 5-minute retry on failure before backing off for 8 hours, preventing station battery exhaustion.

### Changed
- **Streamlined Web UI Pairing Mode Panel:**
  - Simplified station pairing comparison to a concise, clean format: `Local (NVS): 0x...  Cloud (Rx): 0x... ✓ / ✗`.
  - Displayed Local vs Cloud match indication in both **Cloud** and **Local** modes.
  - Relocated station profile description above the `📡 Request GPS Fix` button.
  - Moved channel sweep progress panel under the **Radio** group panel and restricted display strictly to **SCAN** mode (`pairState === 'auto'`).
- **Enhanced Cloud Attribute Parser (`submit.cpp`):**
  - Added fallback JSON parsing for both wrapped `shared` objects and flat root objects from ThingsBoard.
  - Supported `rs_ws_sn`, `rs_ws_id`, `ws_sn`, and `ws_id` attribute keys across 16-hex, 8-hex (`0xBFE45699`), decimal string, and integer JSON formats.
- Bumped the WG1200 firmware version from `0.7.7.6` to `0.7.7.7`.

### Fixed
- **Unblocked Request GPS Fix Button:** Decoupled `.disabled` class styling from parent station container to `ctrl-station-btns`, ensuring the GPS fix button remains active and clickable when paired.

## [0.7.7.6] - 2026-07-24

### Fixed
- **First-boot LVGL crash:** `ButtonComponent::icon()` now deletes its title
  through the component ownership API and child realignment skips objects which
  no longer exist. This fixes the dangling LVGL label reached by the alert close
  button's `.icon(...).size(...)` construction chain.
- **False factory reset on controlled reboot:** all application restart paths
  now use a centralized safe restart which detaches GPIO10/GPIO38 from their
  peripherals, drives both pins high, allows them to settle, and keeps them high
  during the watchdog fallback. An ESP-IDF shutdown handler also covers normal
  `esp_restart()` calls originating in third-party components.

### Changed
- Bumped the WG1200 firmware version from `0.7.7.5` to `0.7.7.6`.

## [0.7.7.5] - 2026-07-23

### Changed
- **Robust Web UI OTA Firmware Flashing:**
  - Upgraded Web UI OTA socket handler to allocate a 4KB heap buffer (`malloc(4096)`), speeding up file reception while preventing stack overflow.
  - Added strict magic header validation (`0xE7`) on the first chunk to reject invalid or non-binary files before writing to flash.
  - Added a floor check (`total_written < 100,000 bytes`) to cleanly abort incomplete/truncated Wi-Fi uploads prior to calling `esp_ota_end()`.
  - Added detailed byte-count and error logging during OTA verification.

## [0.7.7.4] - 2026-07-23

### Changed
- **Dashboard Stat Cards Reorganization:**
  - Moved packet reception summary (`Reception: X% (Duration: Ys)`) to the **LoRa Module** panel.
  - Updated **LoRa Module** primary green value to display active frequency, channel, and % packet reception (e.g. `868.000 MHz · WS1001 · 100%`).
  - Added LCD freeze and LVGL task pause screen during Web UI OTA updates to eliminate display flicker.

## [0.7.7.3] - 2026-07-23

### Added
- **Historical Precipitation Chart:** Added a 4th chart in Historical Weather Trends displaying 5-minute **Precipitation Rate** (mm/h) and **24h Cumulative Precipitation** (mm).
- **Robust Cumulative Rain Tracking:** Aggregates track incremental rain deltas across raw station packets with automatic counter reset and rollover detection (e.g. station reboot or zeroing), ensuring smooth 24h monotonic cumulative rain curves.

### Changed
- **Historical Charts 2x2 Matrix Layout:** Rearranged the Historical Weather Trends view into a balanced 2x2 grid layout (2 columns on desktop, 1 on mobile).
- **Web UI Polling Optimization:** Adjusted background JS refresh timers to align with actual sensor broadcast cadences:
  - **Live Weather Cards & Live Trends Chart:** Updated polling interval to **20 seconds** (`20000ms`), matching the ~16–20s RF transmit interval of weather stations.
  - **LoRa Sniff Packet Log:** Kept responsive **3-second** (`3000ms`) polling for live raw packet capture.

## [0.7.7.2] - 2026-07-23

### Changed
- **Web UI Chart Auto-Scaling:** Refactored chart scale options in the Web UI dashboard and history views to use hybrid auto-scaling across all weather parameters:
  - **Wind & Gust (`Wind m/s`):** Minimum ceiling of 10 m/s (`Math.max(10, Math.ceil(max * 1.15))`), preventing low-wind compression while scaling for storms.
  - **Temperature (`Temp °C`):** Dynamic padding (at least 2°C margin) around min/max bounds across live and historical views.
  - **Barometric Pressure (`hPa`):** Dynamic padding (at least 5 hPa) around current min/max bounds to highlight barometric trends and support high-altitude stations.
  - **Solar Radiation (`W/m²`):** Minimum ceiling of 200 W/m² (`Math.max(200, Math.ceil(max * 1.1))`) for clear visibility on overcast/winter days.
  - **Precipitation Rate (`mm/h`):** Minimum ceiling of 5 mm/h (`Math.max(5, Math.ceil(max * 1.25))`) to make light/moderate rain events visible.
- **SRAM Raw Buffer Optimization:** Reduced raw live packet retention window from 20 to 15 minutes (900 seconds) to lower peak memory allocation during HTTP GET requests by 25%.
- **Web UI Chart Polish:** Updated chart subtitles for clarity and changed Wind Gust stroke color to yellow (`#ffd600`) across live and history views.

## [0.7.7.1] - 2026-07-23

### Added
- **Layered Wi-Fi self-recovery:** The gateway now handles lost-IP events, gives DHCP a bounded recovery window, refreshes the Wi-Fi association and DHCP lease after repeated internet failures, escalates to complete Wi-Fi driver restarts with increasing backoff, and permits only one guarded device restart during a continuous outage.
- **Wi-Fi recovery diagnostics:** Association logs now include BSSID, channel, and authentication mode, while internet failures and each escalation action are counted and reported.

### Changed
- **Replacement and multi-AP discovery:** Station connections now scan all channels and choose by signal instead of stopping at the first matching SSID.

## [0.7.7] - 2026-07-21

### Added
- **Standard ThingsBoard OTA Version Reporting on Boot:** Added `current_fw_version` and `current_fw_title` reporting as client-side attributes on boot. This ensures ThingsBoard's integrated OTA state machine receives the updated version immediately after rebooting, transitioning the update to a successful state and showing the correct version on dashboards.

## [0.7.6] - 2026-07-20

### Added
- **`POST /force_pair?id=0x<station_id>[&sn=<20 hex>][&pubkey=<128 hex>]`:** pairs directly by numeric station ID, bypassing the ADV/BOOT handshake. Mirrors the cloud (ThingsBoard shared-attribute) pairing path — writes `ws_id`/`ws_sn`/`ws_pubkey` and lets the existing `SETTINGS_CHANGED` re-init apply it. Valid because the WS1300 protocol matches non-ADV packets on the 4-byte station ID alone and keys every response HMAC on the *gateway's* serial, so the stored SN/pubkey are never used to build or verify `ACK`/`CMD`/`TIME_SYNC`. Diagnostic aid for separating "is the RF/HMAC link healthy" from "is the pairing handshake working" without power-cycling a station; the SN (if known) also selects the listen channel directly.
- **First-class persisted pairing mode (`Settings::pair_mode`):** the OFF/CLOUD/LOCAL/SCAN control (previously derived from three independent flags) is now a single persisted enum with a boot-time migration and self-healing of inconsistent states. OFF now survives reboot (previously silently reverted to SCAN and could auto-pair with any station heard). `POST /set_pair_mode?mode=off|cloud|local|scan` applies a mode atomically; legacy endpoints (`/toggle_autopair`, `/set_cloud_pairing`, `/unpair`) keep the persisted mode in sync.
- **One-shot local pairing publish to the cloud:** after a successful SCAN pairing, the station identity (`local_ws_id/model/sn/pubkey`) is published once as ThingsBoard client attributes, so the cloud (Android app or a rule chain) can adopt a locally re-paired station — see `docs/TODO.md` for the remaining server-side step (devices cannot write shared attributes).
- **`POST /ws1300_kick_station[?id=0x<id>]`:** sends an authenticated LoRa CMD-reboot to a sniffed (unpaired-gateway) WS1300, forcing it to re-emit a pairable `STA_BOOT` — the LoRa-level workaround for the station protocol having no unpair command. HMAC-gated to this gateway's serial.
- **Web UI "Station Pairing" panel:** dedicated, labeled panel above the LoRa Packets Sniffer with per-mode captions, an uplink-active indicator, and a Station Profile group ("Scan" renamed to "Any" to avoid clashing with the SCAN pairing mode).
- **Dynamic Bootloader Detection & Version Reporting:** The application now dynamically scans SPI flash at offset `0x0` to discover the bootloader's compile date/signature. This compile date and its corresponding factory reset pin mapping are published as a `gw_bootloader_version` ThingsBoard client attribute once on boot and upon any successful local station pairing.
- **Unit Bootloader Status Note:** Only units **`Powerful Sangria Sun`** and **`Flat Veronica Easterlies`** have been updated with the new GPIO 38 bootloader binary. All other units remain on the old `v0.1.9` bootloader (GPIO 10). Older units are now fully protected from false factory resets on soft reboots via software GPIO stabilization prior to resetting.

### Fixed
- **Paired-link outages lasting many minutes (post-pair "appears broken") — four fixes in the WS1300 paired flow:**
  - *Base-probe spiral:* after a 5-minute silence released the frequency lock, the gateway alternated onto the legacy base frequency (where a v11+ station never transmits) with probe dwells stretched by the miss-streak interval model — up to 240 s per timeout, turning a brief outage into 10+ minutes. The gateway now remembers which frequency the station was actually confirmed on (`_paired_freq_confirmed`/`_paired_confirmed_on_base`), dwells 5 timeouts there and only 1 on the other side, and probe waits are capped at 60 s (`PAIRED_PROBE_WAIT_MAX_SEC`) — RX is continuous, so short waits only make the probe converge faster.
  - *TX-power tug-of-war:* when the station raised its own TX power after missing responses (its only self-recovery), the gateway compared against its stale `_current_power_level` and commanded the station straight back down, keeping a marginal link broken and preventing the station's interval-stretch fallback (which requires max power) from ever engaging. The gateway now adopts the station's raised level and restarts the power-decrease window; the sweet-spot descent itself is unchanged.
  - *Stale adaptive-power statics across pairings:* the sniff-based auto-pair path never reset the `Ws1300Helpers` statics (`_current_power_level`, `_last_packet_index`, …), so a re-pair started with the previous session's counters — garbage lost-packet counts and a possible wrong power CMD on the first exchanges. `init_sta_status()` is now called when auto-pairing.
  - *Radio deafness recovery:* the `TxTimeout` radio event was unregistered — a timed-out TX left the radio in standby with nothing to re-enter RX while the frequency lock suppressed retuning. `TxTimeout` now recovers like TX-done, and as a belt-and-braces measure the paired timeout path re-arms continuous RX from the second consecutive miss.
- **Spurious `ACTIVE_BAD_DATA` on the first packet after a station reboot:** a single malformed sample flipped the station status straight to `ACTIVE_BAD_DATA` (and logged a warning). The first `SENSORS` packet after a station reboot legitimately carries no sensor readings yet, so a freshly (re)paired station always tripped it. `ACTIVE_BAD_DATA` now requires `WS_BAD_DATA_STREAK_THRESHOLD` (3) consecutive bad-value packets, reset by any good packet; individual bad packets are still logged and still kept out of the telemetry buffer.
- **CMD response padding bits were uninitialized stack garbage:** the `RespCmd` `unused_*` bit-fields went on air with random values (HMAC-covered, so stations accepted them). The packet is now zeroed before filling.
- **Wi-Fi AP Bypass "Exit" Reboot Loop (Accidental Factory Reset on Soft-Reboot):**
  - *Change:* Implemented software GPIO stabilization (`Utils::prepare_safe_restart_gpios()`) to actively drive both GPIO 10 (old bootloader) and GPIO 38 (new bootloader) **HIGH** prior to any system soft reset (`esp_restart()`).
  - *Rationale:* Tapping the capacitive "Exit" button held the screen interrupt pin (GPIO 38) LOW. Soft-resetting the CPU while the user's finger was still on the screen caused the bootloader to boot with GPIO 38 read as LOW, misinterpreting it as a physical button hold, formatting NVS, and trapping the unit in a configuration loop. Actively driving these lines HIGH prevents accidental factory resets on all bootloader versions.
- **Unpaired gateways could uplink weather data:** with sniffing off while unpaired, SENSORS packets were still buffered and submitted; with multiple stations in range this mixed their data upstream. Weather buffering and submission are now hard-gated on `PairState::PAIRED`, and buffers are flushed on unpair and on any pairing change.
- **Sniff sweep channel capture:** the channel hop only fired on a receive timeout, and every received packet restarted the wait, so a station broadcasting faster than the 22 s dwell (e.g. a neighboring station) could pin the sweep on its channel forever, starving the rest of the sweep. The dwell is now enforced on wall-clock time since the channel was tuned.
- **`max_uri_handlers` raised 24 → 32:** 27 HTTP endpoints were being registered against a 24-handler cap; the last registrations were silently failing.
- **Pair/ACK responses transmitted on the wrong frequency (`main/ws/ws_handler.cpp`):**
  - *Change:* Responses are now always transmitted on the frequency the station's packet was received on; retuning to the expected paired frequency happens only after TX completes (`sx126x_tx_cb`, or inline for the KISS build). Removed the "exit base fallback before TX" logic.
  - *Rationale:* The station's 300 ms RX window is always on its own TX frequency. Retuning before TX guaranteed a legacy (pre-v11, base-frequency) station could never receive its `RESP_PAIR` — a permanent pairing deadlock confirmed with live RF captures.
- **Auto-pair `RESP_PAIR` was never transmitted:** the response was built after the (single) TX call had already run. The TX now happens after the auto-pair block, so the very first sniffed ADV gets answered.
- **Paired gateway booted onto the legacy base frequency:** a paired WS1300 gateway now boots listening on the serial-derived channel (where all v11+ stations transmit); the probe still finds legacy prototypes on base. Learned fallback/lock state survives settings re-inits.
- **Frequency probe starvation:** while the station has never been heard (`ActivityStatus::NONE`) timeout waits stay at ~22 s instead of growing with the miss streak, so the base/channel probe cycles quickly; the frequency lock is released again if a confirmed station goes `INACTIVE` (e.g. reflashed from legacy to channelized firmware).
- **Credential hijack via 4-byte ID collision (`ws_1300.cpp`):** while paired, an ADV whose full 10-byte serial differs from the stored one is rejected (`ERROR_BAD_ID`) instead of silently overwriting the stored serial/public key. A changed public key with a matching serial (station reset/reflash) is still accepted.
- **ADV/BOOT packets polluting telemetry:** META-only packs from ADV/BOOT exchanges are no longer added to the telemetry buffer, where `WsData::add()` used to pad them with the gateway's own barometer reading and upload them as weather data.

### Changed
- **Restored the stable v0.6.2 bootloader factory-reset pin and timing:** The visible USER/display button remains GPIO38; a 1.5-second hold only turns the screen off and does not restart into the bootloader. The bootloader factory-reset input is active-low GPIO10 with a 5-second hold and 9-second watchdog, matching every stable release from v0.1.4 through v0.7.0 (including v0.6.2). Holding the GPIO10-connected hardware control across reset/power-up erases NVS and OTA selection data and returns to the stock `factory` application. The physical GPIO10 switch mapping must be confirmed per hardware revision.

## [0.7.1-rc3] - 2026-07-12

### Added
- **Interactive Button Callback Diagnostics & Watchdog Safety:**
  - Added real-time log statements and strict return-code validations to the physical USER button single-click and long-press event registrations.

### Fixed
- **Watchdog Reboot Prevention on Long-Press:**
  - Standardized physical button hold events to prevent task watchdog starvation by allowing standard FreeRTOS yielding during hold monitoring.

## [0.7.1-rc2] - 2026-07-11

### Added
- **Optimized TLS Verification Order (`main/https_client.cpp`):**
  - *Change:* Swapped the TLS verification priority to verify against target-curated embedded WXM roots on the first attempt, falling back to the global CA bundle on the second attempt.
  - *Rationale:* Resolves a periodic 20-second connection latency and fully eliminates `esp-x509-crt-bundle: Failed to verify certificate` warning logs. Secure connections now complete on the very first try instantly.
- **Unique Descriptive HTTP User-Agent Headers (`platformio.ini`, `main/https_client.cpp`):**
  - *Change:* Introduced compile-time `CONFIG_HW_NAME` environment macros and mapped them to dynamic, RFC-compliant descriptive `User-Agent` headers in the format: `<Hardware-Profile>/<Firmware-Version>`.
  - *Rationale:* Follows standard HTTP API guidelines, allowing backend servers to identify specific hardware types (e.g., `WXM-WG1200`, `WXM-XIAO-ESP32S3`, `WXM-LILYGO-SIM7000`) and firmware versions during connection handshakes.

### Fixed
- **Instant Wi-Fi Credentials Flash Persistence (`components/esp32-wifi-manager/src/http_app.c`):**
  - *Change:* Invoked `wifi_manager_save_sta_config()` inside the captive portal's HTTP POST `/connect.json` handler to write credentials to NVS immediately upon submission, and relaxed password checks to support open (password-less) Wi-Fi networks.
  - *Rationale:* Previously, credentials were only written to NVS *after* a successful connection event (`WM_EVENT_STA_GOT_IP`). If connection was slow, failed due to AP/STA co-existence overhead, or the device was power-cycled during connection, the credentials were never saved. Persisting them immediately ensures they are retained across restarts, allowing the device to boot into pure STA mode (which is much more stable and connects immediately).
- **Bootloader Factory Reset NVS Wipe Prevention (`main/main.cpp`):**
  - *Change:* Modified the physical USER button `BUTTON_LONG_PRESS_HOLD` callback to wait for physical button release before triggering `esp_restart()`.
  - *Rationale:* ESP-IDF's 2nd-stage bootloader features `BOOTLOADER_FACTORY_RESET` on pin 38 (USER button). In previous releases, long-pressing the button would trigger a restart while the user was still holding the button. This caused the bootloader to boot while pin 38 was LOW, immediately wiping the NVS partition (WiFi credentials and settings). Now, the device safely waits for button release before initiating the restart, preventing any accidental factory resets.

## [0.7.1-rc1] - 2026-07-11

### Fixed
- **WiFi Manager Deadlock and Beacon Timeout Recovery (`main/wifi.cpp`):**
  - *Change:* Restrained the background DNS synchronization loop (`dns_resolve_sync`) from overwriting the WiFi status unless the link is fully connected.
  - *Change:* Added a 30-second safety timeout on the active link connection loop (`connect_sync`).
  - *Change:* Configured the reconnection loop to gracefully accept `CONNECTED_NO_INTERNET` and `CONNECTED` states.
  - *Rationale:* Prevents background tasks from corrupting state variables during disconnects, eliminates infinite thread loops, and ensures the WiFi Manager always recovers gracefully.

### Build System & Release Cleanup
- **Fixed "Self-Dirtying Repository" Loop:**
  - *Change:* Untracked and ignored `components/esp32-wifi-manager/src/fw_version.h` by adding it to `.gitignore` and running `git rm --cached`.
  - *Rationale:* The build script (`pre_actions.py`) auto-generates `fw_version.h` on every build. Because it was tracked by Git, every compilation modified it, making the repository dirty and forcing a `-dirty` suffix into the version string and binary filenames. Untracking it permanently guarantees clean, official releases.

## [Phase A] - 2026-06-19

### Added
- **Centralized UART KISS Client (`main/ws/include/kiss_client.h`, `main/ws/kiss_client.cpp`):**
  - Implemented high-reliability FEND/FESC serial framing, unescaping, and packet validation.
  - Added native support for reading weather packets asynchronously and querying barometric sensors.
  - Implemented a CayenneLPP `0x73` decoder to parse Bosch BMP280 pressure sensor data received from the nRF52.
- **CMake & Build configuration for Screenless Hardware:**
  - Added `-DCONFIG_SCREENLESS` preprocessor directive inside `platformio.ini` build flags and CMake definitions (`main/CMakeLists.txt`) to completely separate and isolate physical display, backlight, and LVGL UI code for the headless WG1400 hardware profile.

### Modified
- **Screenless Refactoring (`main/main.cpp`):**
  - Wrapped display/LVGL theme initializations, physical backlight pins, and sniffer display threads inside `#ifndef CONFIG_SCREENLESS` blocks.
- **Barometric Pressure Task Routing (`main/pressure.cpp`):**
  - Bypassed physical on-board BMP390 I2C sensor driver initializations under `CONFIG_SCREENLESS`.
  - Configured `pressure_update_task` to perform asynchronous queries to fetch barometric pressure values from the nRF52's BMP280 sensor via the KISS client.
- **Packet Handling & KISS Modem Integration (`main/ws/ws_handler.cpp`):**
  - Bypassed direct physical Semtech SPI Radio drivers under `CONFIG_SCREENLESS`.
  - Configured physical UART serial lines on standard pins `TX=D6` (GPIO 43) and `RX=D7` (GPIO 44) for communications with the nRF52, with flow control disabled.
  - Launched `kiss_rx_task` to read serial packets in the background and insert incoming LoRa weather packet frames directly into the gateway's weather queue.
  - Forward-declared `sx126x_rx_cb` to allow clean background routing of parsed frames.

### Architectural Highlight: Centralized UART RX Race Condition Resolution
- **The Issue:** In the initial screenless design, the `kiss_rx_task` (reading LoRa frames) and `pressure_update_task` (querying BMP280 sensors) would have read from the same UART port concurrently. Although ESP-IDF's UART reads are thread-safe, calling read simultaneously from two threads splits the incoming byte stream across tasks, corrupting both LoRa packets and sensor telemetry.
- **The Solution:** We centralized all serial reading exclusively within `KissClient::read_packet()`. The `pressure_update_task` sends a sensor query over TX, then waits for a static flag `g_pressure_updated`. The background `kiss_rx_task` reads all bytes from the UART buffer, routes standard LoRa frames to the weather queue, and routes sensor telemetry packets to a decoder that updates the pressure state and sets the flag, completely preventing data corruption.

## [Phase B & C] - 2026-06-19

### Added
- **Sleek Glassmorphic Diagnostics Web UI (`main/ws/include/web_server.h`, `main/ws/web_server.cpp`):**
  - Integrated an asynchronous local web dashboard using custom modern HSL-tailored colors, Glassmorphism backdrop-filters, and clean responsive grids.
  - Implemented background API polling routes (`/status`, `/packet_log`, `/ota`) to visualize real-time packet logs, RSSI/SNR signal levels, Thingsboard transmission success rates, WiFi RSSI/SSID, and local BMP280 pressure data.
- **Web-Based OTA Firmware Uploads (`main/ws/web_server.cpp`):**
  - Built an asynchronous, robust `/ota` endpoint handling binary firmware chunks.
  - Added a responsive drag-and-drop web UI with upload progress bars that automatically updates the ESP32-S3 boot partition and triggers a system restart on completion.
- **Deep Sleep Power Management (`main/main.cpp`):**
  - Implemented a background power scheduler `sleep_scheduler_task` that maintains a 60-second active window to process and upload telemetry before sleeping.
  - Safely releases UART serial interfaces and transitions the ESP32-S3 into deep sleep mode for 5 minutes, preserving power while the nRF52 co-processor remains active.
- **Cellular Failover Connection Driver (`main/include/cellular.h`, `main/cellular.cpp`):**
  - Implemented an AT command UART driver for SIM7000 (classic ESP32) and SIM7000G (ESP32-S3) modems.
  - Added secure GPRS network attachment sequences, APN registration, and Thingsboard telemetry MQTT publishing over raw TCP sockets.
  - Utilizes hardware-specific GPIO pins for PWRKEY and serial interfaces on both Classic and S3 targets.

### Modified
- **WiFi Connect Events (`main/wifi.cpp`):**
  - Configured `WebServer::start()` and `WebServer::stop()` triggers on WiFi IP assignment and disconnect events.
- **Telemetry Upload Gateway Fallback (`main/tb.cpp`):**
  - Added compile-guarded `#ifdef CONFIG_CELLULAR_BACKUP` failover branches.
  - Automatically routes weather telemetry packets to `Cellular::publish_telemetry()` if WiFi is disconnected or standard WiFi MQTT publishing fails.
