# WG1200 SD Weather Storage

This document describes the WG1200 SD-card storage behavior implemented by
`WeatherLogger`. SD persistence is compiled out for `CONFIG_SCREENLESS` targets.

## Data lifecycle

1. A decoded weather-station `FieldPack` is published as `WS_DATA_RECEIVED`.
2. WG1200 keeps accepted packets in a 15-minute rolling RAM buffer.
3. When a packet crosses a five-minute Unix-time boundary, the just-completed
   interval is aggregated.
4. The aggregate is retained in the 288-slot (24-hour) RAM history and copied to
   an eight-record SD writer queue without waiting for filesystem I/O.
5. A priority-1 task pinned to core 0 writes the record. LoRa IRQ and packet
   handling run at priorities 8 and 5 on core 1. SD access still takes the shared
   SPI/radio mutex, so an individual filesystem transaction cannot be preempted,
   but no SD mount, write, flush, sync, close, or retry occurs in the weather
   event handler.

The writer has a 12 KiB task stack. Its large request, daily accumulator, CSV
row, path, header-validation, and error buffers live in one persistent
heap-backed writer context rather than nested task-stack frames. After processing
records, it logs the task's minimum observed free stack and warns if that margin
falls below 3 KiB. This is diagnostic only and does not block logging.

The first interval after boot is not complete until a packet arrives in the next
interval. A power loss before that next packet loses the unfinished RAM interval.
It also resets the in-progress daily metadata accumulator; completed five-minute
weather rows and already-written GPS metadata remain on SD. RAM history is not
reconstructed from SD after reboot.

## Local time and daily rotation

The timezone is the fixed whole-hour offset selected in gateway settings
(`UTC-12:00` through `UTC+14:00`). It is captured with each aggregate.

Daily filenames and `LocalTimestamp` use:

```text
local time = Unix timestamp + configured timezone offset
```

The timestamp is ISO 8601 and contains the offset, for example
`2026-08-11T17:05:00+03:00`. `UnixTimestamp` is retained as an unambiguous UTC
epoch value for programmatic processing. Daylight-saving changes are not
calculated automatically because the gateway setting is a fixed offset rather
than an IANA timezone.

A timezone setting change can move subsequent rows to a different local-day
file. Every metadata row records the offset that was actually used.

## Weather CSV

One versioned file is created per local day:

```text
/sdcard/history_v1_YYYY_MM_DD.csv
```

The `v1` suffix identifies the current development schema. If the schema is
changed incompatibly after release, the version should be incremented rather
than appending differently shaped rows to an existing daily file. WG1200 builds
enable FAT long-filename support because both versioned daily filenames exceed
the legacy FAT 8.3 limit.

Columns:

```csv
LocalTimestamp,UnixTimestamp,Temp_Avg_C,Hum_Avg_pct,WindSpeed_Avg_m_s,WindDir_Prevailing_deg,WindGust_Max_m_s,PrecipRate_Avg_mm_h,PrecipAccum_mm,Pressure_Avg_hPa,SolarRad_Avg_W_m2
```

- Temperature, humidity, wind speed, precipitation rate, pressure, and solar
  radiation are arithmetic means of available samples.
- Prevailing wind direction is a wind-speed-weighted circular mean.
- Wind gust is the maximum sample.
- Precipitation accumulation is the increase in the station cumulative counter.
  A station counter decrease is treated as a reset. If the cumulative counter is
  unavailable, the logger integrates the average rate over five minutes and sets
  `PRECIP_RATE_FALLBACK` in metadata.
- Missing measurements are empty CSV cells, not zeroes or `nan` strings.

## Metadata CSV

One metadata file is created per local day:

```text
/sdcard/metadata_v1_YYYY_MM_DD.csv
```

Metadata is not duplicated every five minutes. The file contains three row
types identified by `RecordType`:

- `DAILY_START` is the first attempted metadata record for each observed local
  day. It is written as soon as the first weather interval or GPS fix reaches
  the writer. Its GPS and coverage cells are deliberately empty: it records
  identity and configuration without claiming a fix or completed coverage.
- `DAILY_SUMMARY` is written when the local date rolls over. It summarizes the
  completed day's weather aggregates held by the background writer.
- `GPS` is written for every complete, valid GPS fix received from the active
  paired station. It contains `GpsTimestamp`, `GpsLatitude`, `GpsLongitude`, and
  `GpsSatellites`. Automatic fixes are normally daily, while manual GPS requests
  can add extra rows.

The daily accumulator is kept in RAM to avoid SD writes every five minutes.
`CoverageStartUnixTimestamp`, `CoverageEndUnixTimestamp`, and
`IntervalsObserved` state exactly what the summary covers. An unexpected reboot
resets the accumulator, so the eventual `DAILY_SUMMARY` row covers only the portion of
that local day observed since the most recent boot. If the gateway never runs
into the next local day, that unfinished summary is not written. GPS rows are
written immediately and are not affected by this daily-summary limitation.

This development build changes the `metadata_v1` header and places the GPS
columns directly after station identity so they are easy to find. The writer
validates an existing file's header before appending and reports a clear mismatch
instead of mixing schemas. Delete or rename metadata files created by the earlier
development build before expecting new rows in the same local-day filename.

It contains:

- Record type, local/Unix timestamp, applied UTC offset, coverage range, and
  number of observed five-minute intervals.
- Gateway ID (device certificate/eFuse serial) and gateway firmware version.
- Observed station ID and decoder/station type.
- Configured paired-station ID and whether the source matched it.
- Total received and expected packet counts for the covered daily period.
- First/last station packet counters and counter gaps.
- Station transmit interval and firmware version.
- Minimum reported battery voltage, low-battery state, and average RSSI.
- Bitwise-OR numeric and human-readable quality flags across observed intervals.
- Union of present fields and names of fields never observed during coverage.
- Metadata schema version. Measurement units are not repeated here because the
  canonical units are already encoded in the weather-history column names.
- GPS timestamp, latitude, longitude, and satellite count on `GPS` rows.

## Quality flags

`QualityFlagsHex` is the bitwise combination below. `QualityFlags` contains the
same information as pipe-separated names.

| Bit | Hex | Name | Meaning |
|---:|---:|---|---|
| 0 | `0x00000001` | `MISSING_FIELDS` | At least one principal weather field was absent. |
| 1 | `0x00000002` | `LOW_PACKET_COUNT` | Received fewer packets than expected from the reported TX interval. |
| 2 | `0x00000004` | `PACKET_COUNTER_GAP` | The station packet counter advanced by more than one. |
| 3 | `0x00000008` | `PACKET_COUNTER_RESET` | The packet counter moved backwards without looking like a normal wrap. |
| 4 | `0x00000010` | `MIXED_STATION_IDS` | More than one station ID appeared in the interval. |
| 5 | `0x00000020` | `BAD_VALUES` | An accepted input pack carried the bad-value marker. |
| 6 | `0x00000040` | `PRECIP_RATE_FALLBACK` | Rain accumulation was estimated from precipitation rate. |
| 7 | `0x00000080` | `UNPAIRED_SOURCE` | Source was not the currently paired station. |
| 8 | `0x00000100` | `TX_INTERVAL_CHANGED` | Reported transmit interval changed within the slot. |
| 9 | `0x00000200` | `PRECIP_COUNTER_RESET` | Cumulative rain counter decreased and was treated as reset. |
| 10 | `0x00000400` | `PRECIP_BASELINE_MISSING` | No prior cumulative rain sample was available for a delta. |
| 11 | `0x00000800` | `STATION_ID_MISSING` | No station ID was present in accepted packets. |

`OK` is represented by `0x00000000`.

## Write, retry, and recovery behavior

Weather, daily-metadata, and GPS-metadata appends are separate operations, so the
SPI mutex is released between them and higher-priority radio work gets an
opportunity to proceed.

For each append, the writer:

1. Lazily mounts FAT at `/sdcard` without automatically formatting it.
2. Opens the daily file in append/read mode.
3. Writes the header when the file is empty.
4. Writes the row and checks the byte count.
5. Checks `fflush`, `fsync`, and `fclose`.
6. On failure, attempts to truncate the file back to its original length.
7. Releases SPI, waits 100 ms, and retries once.
8. After a second failure, skips that file row and reports the final error.

The first event observed for a local day attempts the GPS-empty `DAILY_START`
row. Each weather aggregate then updates the RAM daily summary and queues only
its weather CSV row. At local-day rollover, the previous `DAILY_SUMMARY` is
appended once. A valid GPS fix queues its own populated metadata row. Every
append uses the same one-retry-then-skip policy.

A full writer queue is non-blocking: a weather aggregate remains in the 24-hour
RAM history even if its weather row is skipped, while a GPS metadata event is
skipped independently. The queue error and relevant failed-row counter are
reported in `/sd/status`.

There is no automatic retention or deletion policy. Daily files accumulate until
the user deletes them, formats the card, or the filesystem becomes full.

## Web status and management

The SD Card Management panel displays:

- FAT filesystem mount state, separately from physical-card detection.
- Background writer state and pending queue depth.
- Successful and failed weather/metadata row counts.
- Retry count.
- Last successful write and the last terminal error.
- Card identity, capacity, SPI speed, filesystem capacity, partition layout, and
  the complete file list in one compact management card.

There is one **Refresh** action for the complete card. It deliberately runs the
hardware diagnostics first because diagnostics can mount a valid FAT card on
demand. Only after that finishes does the browser read `/sd/status` and
`/sd/list`. This ordering prevents a pre-mount “card is not mounted” list result
from being displayed beside a newer `MOUNTED` status. The displayed states mean:

- `MOUNTED`: the FAT VFS is mounted and file operations are available.
- `CARD PRESENT / FAT NOT MOUNTED`: hardware responds, but a writable FAT
  filesystem is unavailable.
- `NOT DETECTED`: the low-level diagnostic could not read a card.
- `Writer task: READY`: the asynchronous logger task is running; this is not a
  second indication that the card is mounted.

The machine-readable endpoint is `GET /sd/status`. Existing endpoints provide
file listing/download/deletion, diagnostics, write/read testing, and explicit
formatting. Ordinary weather logging never auto-formats an unreadable card.

CSV files in the SD file list have a **View** action next to **Download**. It
opens `GET /sd/view?file=<name>` in a new browser tab. The viewer fetches the
original bytes through `GET /sd/download`, parses CSV in the browser, and shows:

- A searchable, optionally newest-first table with sticky headers and alternating
  row colors.
- Highlighting for non-`OK` metadata quality flags.
- Temperature/humidity, wind/direction, precipitation, and pressure/solar charts
  when the weather-history schema is detected.

The viewer makes the chart grid visible before uPlot measures it, so every plot
uses the full available width of its card and continues to resize with the tab.

The viewer uses the uPlot JavaScript and CSS already bundled in LittleFS. CSV
parsing, filtering, table construction, and chart rendering all happen on the
client device; the ESP32 does no CSV reformatting or chart computation. The
downloaded CSV remains unchanged and is still the canonical stored data.

SD filename parameters are restricted to a single safe filename (no directory
components or traversal) for view, download, and deletion operations. The View
action is offered only for `.csv` files.

For troubleshooting apparent gaps, `GET /status` includes monotonic
`uptime_sec`. The separate `activity_duration_sec` field is only the duration of
the current weather-station activity state; it is not gateway uptime. RAM
history starts empty after every gateway restart.
