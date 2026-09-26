#include "weather_logger.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <sys/stat.h>
#include <time.h>

#ifndef CONFIG_SCREENLESS
#include <dirent.h>
#include <sys/types.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "app_config.h"
#include "bsp_board.h"
#include "bsp_storage.h"
#include "credentials.h"
#include "utils.h"
#endif

#include "datetime_rtc.h"
#include "esp_log.h"
#include "settings.h"
#include "ws/include/ws_handler.h"
#include "ws/include/ws_gps.h"

static const char *TAG = "WeatherLogger";

namespace WeatherLogger {

namespace {

constexpr time_t AGGREGATE_INTERVAL_SEC = 300;
constexpr uint32_t RAW_RETENTION_SEC = 900;
constexpr size_t MAX_AGGREGATES = 288;

enum WeatherFieldPresence : uint32_t {
    FIELD_PRESENT_TEMP         = 1U << 0,
    FIELD_PRESENT_HUM          = 1U << 1,
    FIELD_PRESENT_WIND_SPEED   = 1U << 2,
    FIELD_PRESENT_WIND_DIR     = 1U << 3,
    FIELD_PRESENT_WIND_GUST    = 1U << 4,
    FIELD_PRESENT_PRECIP_RATE  = 1U << 5,
    FIELD_PRESENT_PRECIP_CML   = 1U << 6,
    FIELD_PRESENT_PRESSURE     = 1U << 7,
    FIELD_PRESENT_SOLAR_RAD    = 1U << 8,
};

constexpr uint32_t REQUIRED_NON_PRECIP_FIELDS =
    FIELD_PRESENT_TEMP | FIELD_PRESENT_HUM | FIELD_PRESENT_WIND_SPEED |
    FIELD_PRESENT_WIND_DIR | FIELD_PRESENT_WIND_GUST |
    FIELD_PRESENT_PRESSURE | FIELD_PRESENT_SOLAR_RAD;

std::mutex s_mutex;
std::vector<WsFields::FieldPack> s_raw_buffer;
std::vector<WeatherAggregate> s_aggregates;
time_t s_last_aggregated_interval = 0;
float s_last_precip_cml = NAN;
uint32_t s_last_precip_station_id = 0;
SdLoggerStatus s_sd_status;

int normalized_timezone_offset(int offset_hours)
{
    return (offset_hours >= -12 && offset_hours <= 14) ? offset_hours : 0;
}

#ifndef CONFIG_SCREENLESS

constexpr size_t SD_WRITE_QUEUE_LENGTH = 8;
constexpr uint32_t SD_WRITER_TASK_STACK_SIZE = 12 * 1024;
constexpr UBaseType_t SD_WRITER_STACK_WARN_BYTES = 3 * 1024;
constexpr UBaseType_t SD_WRITER_TASK_PRIORITY = 1;
constexpr TickType_t SD_RETRY_DELAY_TICKS = pdMS_TO_TICKS(100);
constexpr size_t CSV_PATH_BUFFER_SIZE = 96;
constexpr size_t CSV_ROW_BUFFER_SIZE = 1536;
constexpr size_t CSV_HEADER_BUFFER_SIZE = 512;
constexpr size_t SD_WRITER_ERROR_BUFFER_SIZE = 320;

// Exponential backoff for SD card mounting when missing (5m -> 15m -> 1h -> 6h -> 24h)
constexpr time_t SD_MOUNT_BACKOFF_INITIAL_SEC = 300;     // 5 minutes
constexpr time_t SD_MOUNT_BACKOFF_MAX_SEC = 86400;       // 24 hours (daily)

static time_t s_sd_mount_backoff_sec = SD_MOUNT_BACKOFF_INITIAL_SEC;
static time_t s_sd_next_mount_ts = 0;
static bool s_sd_probe_requested = true; // Probe once on boot
static bool s_sd_is_mounted = false;

time_t calculate_next_backoff(time_t current_backoff)
{
    if (current_backoff < 900) return 900;       // 15 min
    if (current_backoff < 3600) return 3600;     // 1 hour
    if (current_backoff < 21600) return 21600;   // 6 hours
    return SD_MOUNT_BACKOFF_MAX_SEC;             // 24 hours
}

constexpr const char *WEATHER_CSV_HEADER =
    "LocalTimestamp,UnixTimestamp,Temp_Avg_C,Hum_Avg_pct,WindSpeed_Avg_m_s,"
    "WindDir_Prevailing_deg,WindGust_Max_m_s,PrecipRate_Avg_mm_h,"
    "PrecipAccum_mm,Pressure_Avg_hPa,SolarRad_Avg_W_m2\n";

constexpr const char *METADATA_CSV_HEADER =
    "RecordType,LocalTimestamp,UnixTimestamp,Timezone,GatewayID,"
    "GatewayFirmware,StationID,StationType,PairedStationID,SourcePaired,"
    "GpsTimestamp,GpsLatitude,GpsLongitude,GpsSatellites,"
    "CoverageStartUnixTimestamp,CoverageEndUnixTimestamp,IntervalsObserved,"
    "PacketsReceived,ExpectedPackets,"
    "PacketCounterFirst,PacketCounterLast,PacketCounterGaps,TxIntervalSeconds,"
    "StationFirmware,BatteryMin_mV,BatteryLow,RSSI_Avg_dBm,QualityFlagsHex,"
    "QualityFlags,PresentFieldsHex,MissingFields,SchemaVersion\n";

enum MetadataColumn : size_t {
    META_RECORD_TYPE,
    META_LOCAL_TIMESTAMP,
    META_UNIX_TIMESTAMP,
    META_TIMEZONE,
    META_GATEWAY_ID,
    META_GATEWAY_FIRMWARE,
    META_STATION_ID,
    META_STATION_TYPE,
    META_PAIRED_STATION_ID,
    META_SOURCE_PAIRED,
    META_GPS_TIMESTAMP,
    META_GPS_LATITUDE,
    META_GPS_LONGITUDE,
    META_GPS_SATELLITES,
    META_COVERAGE_START,
    META_COVERAGE_END,
    META_INTERVALS_OBSERVED,
    META_PACKETS_RECEIVED,
    META_EXPECTED_PACKETS,
    META_PACKET_COUNTER_FIRST,
    META_PACKET_COUNTER_LAST,
    META_PACKET_COUNTER_GAPS,
    META_TX_INTERVAL_SECONDS,
    META_STATION_FIRMWARE,
    META_BATTERY_MIN_MV,
    META_BATTERY_LOW,
    META_RSSI_AVG_DBM,
    META_QUALITY_FLAGS_HEX,
    META_QUALITY_FLAGS,
    META_PRESENT_FIELDS_HEX,
    META_MISSING_FIELDS,
    META_SCHEMA_VERSION,
    META_COLUMN_COUNT,
};

QueueHandle_t s_sd_write_queue = nullptr;
TaskHandle_t s_sd_writer_task_handle = nullptr;

enum class SdWriteRequestType : uint8_t {
    WEATHER_AGGREGATE,
    GPS_METADATA,
};

struct GpsMetadataEvent {
    time_t received_timestamp = 0;
    uint32_t gps_timestamp = 0;
    uint32_t station_id = 0;
    uint32_t paired_station_id = 0;
    float latitude = NAN;
    float longitude = NAN;
    uint8_t satellites = 0;
    int8_t timezone_offset_hours = 0;
    bool satellites_available = false;
    bool source_is_paired = false;
    char station_type[7] = {};
};

struct SdWriteRequest {
    SdWriteRequestType type = SdWriteRequestType::WEATHER_AGGREGATE;
    WeatherAggregate aggregate;
    GpsMetadataEvent gps;
};

struct DailyMetadataState {
    bool active = false;
    char local_date[16] = {};
    char started_local_date[16] = {};
    WeatherAggregate aggregate;
    int64_t rssi_weighted_sum = 0;
    uint32_t rssi_weight = 0;
};

struct SdWriterContext {
    DailyMetadataState daily_metadata;
    SdWriteRequest request;
    char csv_path[CSV_PATH_BUFFER_SIZE] = {};
    char csv_row[CSV_ROW_BUFFER_SIZE] = {};
    char csv_header[CSV_HEADER_BUFFER_SIZE] = {};
    char error[SD_WRITER_ERROR_BUFFER_SIZE] = {};
};

SdWriterContext *s_sd_writer_context = nullptr;

enum class CsvFileKind {
    WEATHER,
    METADATA,
};

void set_status_error_locked(const char *message)
{
    s_sd_status.last_error_timestamp = time(nullptr);
    s_sd_status.last_error = message != nullptr ? message : "Unknown SD logging error";
}

void set_status_error(const char *message)
{
    std::lock_guard<std::mutex> lock(s_mutex);
    set_status_error_locked(message);
}

void note_retry()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    s_sd_status.retry_count++;
}

void note_write_result(CsvFileKind kind, bool success, const char *error)
{
    std::lock_guard<std::mutex> lock(s_mutex);
    if (kind == CsvFileKind::WEATHER) {
        success ? s_sd_status.weather_rows_written++ : s_sd_status.weather_rows_failed++;
    } else {
        success ? s_sd_status.metadata_rows_written++ : s_sd_status.metadata_rows_failed++;
    }

    if (success) {
        s_sd_status.last_success_timestamp = time(nullptr);
    } else {
        set_status_error_locked(error);
    }
}

void format_local_time(time_t unix_timestamp, int timezone_offset_hours,
                       char *date, size_t date_len, char *timestamp,
                       size_t timestamp_len, char *timezone,
                       size_t timezone_len)
{
    const int offset_hours = normalized_timezone_offset(timezone_offset_hours);
    const time_t local_timestamp = unix_timestamp + (offset_hours * 3600);
    struct tm local_tm = {};
    gmtime_r(&local_timestamp, &local_tm);

    snprintf(date, date_len, "%04d_%02d_%02d", local_tm.tm_year + 1900,
             local_tm.tm_mon + 1, local_tm.tm_mday);
    snprintf(timestamp, timestamp_len, "%04d-%02d-%02dT%02d:%02d:%02d%+03d:00",
             local_tm.tm_year + 1900, local_tm.tm_mon + 1, local_tm.tm_mday,
             local_tm.tm_hour, local_tm.tm_min, local_tm.tm_sec, offset_hours);
    snprintf(timezone, timezone_len, "UTC%+03d:00", offset_hours);
}

void format_local_time(const WeatherAggregate &agg, char *date, size_t date_len,
                       char *timestamp, size_t timestamp_len, char *timezone,
                       size_t timezone_len)
{
    format_local_time(agg.timestamp, agg.timezone_offset_hours, date, date_len,
                      timestamp, timestamp_len, timezone, timezone_len);
}

const char *format_float(float value, char *buffer, size_t buffer_len, int decimals)
{
    if (std::isfinite(value)) {
        snprintf(buffer, buffer_len, "%.*f", decimals, value);
    } else if (buffer_len > 0) {
        buffer[0] = '\0';
    }
    return buffer;
}

std::string quality_flags_to_string(uint32_t flags)
{
    if (flags == WEATHER_QUALITY_OK) {
        return "OK";
    }

    std::string result;
    const auto add = [&result](const char *label) {
        if (!result.empty()) result += '|';
        result += label;
    };
    if (flags & WEATHER_QUALITY_MISSING_FIELDS) add("MISSING_FIELDS");
    if (flags & WEATHER_QUALITY_LOW_PACKET_COUNT) add("LOW_PACKET_COUNT");
    if (flags & WEATHER_QUALITY_PACKET_COUNTER_GAP) add("PACKET_COUNTER_GAP");
    if (flags & WEATHER_QUALITY_PACKET_COUNTER_RESET) add("PACKET_COUNTER_RESET");
    if (flags & WEATHER_QUALITY_MIXED_STATION_IDS) add("MIXED_STATION_IDS");
    if (flags & WEATHER_QUALITY_BAD_VALUES) add("BAD_VALUES");
    if (flags & WEATHER_QUALITY_PRECIP_RATE_FALLBACK) add("PRECIP_RATE_FALLBACK");
    if (flags & WEATHER_QUALITY_UNPAIRED_SOURCE) add("UNPAIRED_SOURCE");
    if (flags & WEATHER_QUALITY_TX_INTERVAL_CHANGED) add("TX_INTERVAL_CHANGED");
    if (flags & WEATHER_QUALITY_PRECIP_COUNTER_RESET) add("PRECIP_COUNTER_RESET");
    if (flags & WEATHER_QUALITY_PRECIP_BASELINE_MISSING) add("PRECIP_BASELINE_MISSING");
    if (flags & WEATHER_QUALITY_STATION_ID_MISSING) add("STATION_ID_MISSING");
    return result;
}

std::string missing_fields_to_string(uint32_t fields)
{
    std::string result;
    const auto add_missing = [&result](bool present, const char *name) {
        if (present) return;
        if (!result.empty()) result += '|';
        result += name;
    };

    add_missing(fields & FIELD_PRESENT_TEMP, "temperature");
    add_missing(fields & FIELD_PRESENT_HUM, "humidity");
    add_missing(fields & FIELD_PRESENT_WIND_SPEED, "wind_speed");
    add_missing(fields & FIELD_PRESENT_WIND_DIR, "wind_direction");
    add_missing(fields & FIELD_PRESENT_WIND_GUST, "wind_gust");
    add_missing(fields & FIELD_PRESENT_PRECIP_RATE, "precip_rate");
    add_missing(fields & FIELD_PRESENT_PRECIP_CML, "precip_cumulative");
    add_missing(fields & FIELD_PRESENT_PRESSURE, "pressure");
    add_missing(fields & FIELD_PRESENT_SOLAR_RAD, "solar_radiation");
    return result.empty() ? "none" : result;
}

std::string decimal_string(long long value)
{
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%lld", value);
    return buffer;
}

std::string hex32_string(uint32_t value)
{
    char buffer[16];
    snprintf(buffer, sizeof(buffer), "0x%08lX", (unsigned long)value);
    return buffer;
}

std::string join_csv_fields(const std::vector<std::string> &fields)
{
    std::string row;
    for (size_t index = 0; index < fields.size(); ++index) {
        if (index != 0) row += ',';
        row += fields[index];
    }
    row += '\n';
    return row;
}

bool build_csv_record(const WeatherAggregate &agg, CsvFileKind kind,
                      char *path, size_t path_len, char *row, size_t row_len,
                      const char **header, char *error, size_t error_len)
{
    char date[16] = {};
    char local_timestamp[40] = {};
    char timezone[16] = {};
    format_local_time(agg, date, sizeof(date), local_timestamp,
                      sizeof(local_timestamp), timezone, sizeof(timezone));

    int path_written = 0;
    int row_written = 0;
    if (kind == CsvFileKind::WEATHER) {
        // The filename carries the development schema version.
        path_written = snprintf(path, path_len, "/sdcard/history_v1_%s.csv", date);
        *header = WEATHER_CSV_HEADER;

        char temp[24], hum[24], wind[24], wind_dir[24], gust[24];
        char precip_rate[24], precip_accum[24], pressure[24], solar[24];
        row_written = snprintf(
            row, row_len, "%s,%lld,%s,%s,%s,%s,%s,%s,%s,%s,%s\n",
            local_timestamp, (long long)agg.timestamp,
            format_float(agg.temp_avg, temp, sizeof(temp), 2),
            format_float(agg.hum_avg, hum, sizeof(hum), 1),
            format_float(agg.wind_speed_avg, wind, sizeof(wind), 2),
            format_float(agg.wind_dir_prevailing, wind_dir, sizeof(wind_dir), 1),
            format_float(agg.wind_gust_max, gust, sizeof(gust), 2),
            format_float(agg.precip_avg, precip_rate, sizeof(precip_rate), 2),
            format_float(agg.precip_accum, precip_accum, sizeof(precip_accum), 2),
            format_float(agg.pressure_avg, pressure, sizeof(pressure), 2),
            format_float(agg.solar_rad_avg, solar, sizeof(solar), 2));
    } else {
        path_written = snprintf(path, path_len, "/sdcard/metadata_v1_%s.csv", date);
        *header = METADATA_CSV_HEADER;

        std::vector<std::string> fields(META_COLUMN_COUNT);
        fields[META_RECORD_TYPE] = agg.intervals_observed == 0
            ? "DAILY_START" : "DAILY_SUMMARY";
        fields[META_LOCAL_TIMESTAMP] = local_timestamp;
        fields[META_UNIX_TIMESTAMP] = decimal_string(agg.timestamp);
        if (agg.intervals_observed > 0) {
            fields[META_COVERAGE_START] = decimal_string(agg.coverage_start_timestamp);
            fields[META_COVERAGE_END] = decimal_string(agg.coverage_end_timestamp);
            fields[META_INTERVALS_OBSERVED] = decimal_string(agg.intervals_observed);
        }
        fields[META_TIMEZONE] = timezone;
        fields[META_GATEWAY_ID] = Credentials::get_serial_str();
        fields[META_GATEWAY_FIRMWARE] = APP_VERSION;
        if (agg.station_id != 0) fields[META_STATION_ID] = hex32_string(agg.station_id);
        fields[META_STATION_TYPE] = agg.packer_name[0] != '\0' ? agg.packer_name : "unknown";
        if (agg.paired_station_id != 0) fields[META_PAIRED_STATION_ID] = hex32_string(agg.paired_station_id);
        fields[META_SOURCE_PAIRED] = agg.source_is_paired ? "true" : "false";
        if (agg.intervals_observed > 0) {
            fields[META_PACKETS_RECEIVED] = decimal_string(agg.packet_count);
            fields[META_EXPECTED_PACKETS] = decimal_string(agg.expected_packet_count);
        }
        if (agg.packet_counter_available) {
            fields[META_PACKET_COUNTER_FIRST] = decimal_string(agg.first_packet_counter);
            fields[META_PACKET_COUNTER_LAST] = decimal_string(agg.last_packet_counter);
        }
        if (agg.intervals_observed > 0) {
            fields[META_PACKET_COUNTER_GAPS] = decimal_string(agg.packet_counter_gaps);
        }
        if (agg.tx_interval_sec != 0) {
            fields[META_TX_INTERVAL_SECONDS] = decimal_string(agg.tx_interval_sec);
        }
        if (agg.station_fw_available) fields[META_STATION_FIRMWARE] = decimal_string(agg.station_fw_version);
        if (agg.battery_mv_available) fields[META_BATTERY_MIN_MV] = decimal_string(agg.battery_min_mv);
        if (agg.battery_low_available) fields[META_BATTERY_LOW] = agg.battery_low ? "true" : "false";
        if (agg.rssi_available) fields[META_RSSI_AVG_DBM] = decimal_string(agg.rssi_avg);
        if (agg.intervals_observed > 0) {
            fields[META_QUALITY_FLAGS_HEX] = hex32_string(agg.quality_flags);
            fields[META_QUALITY_FLAGS] = quality_flags_to_string(agg.quality_flags);
            fields[META_PRESENT_FIELDS_HEX] = hex32_string(agg.field_presence_mask);
            fields[META_MISSING_FIELDS] = missing_fields_to_string(agg.field_presence_mask);
        }
        fields[META_SCHEMA_VERSION] = "1";

        const std::string metadata_row = join_csv_fields(fields);
        row_written = snprintf(row, row_len, "%s", metadata_row.c_str());
    }

    if (path_written < 0 || static_cast<size_t>(path_written) >= path_len ||
        row_written < 0 || static_cast<size_t>(row_written) >= row_len) {
        snprintf(error, error_len, "Could not format %s CSV record",
                 kind == CsvFileKind::WEATHER ? "weather" : "metadata");
        return false;
    }
    return true;
}

bool build_gps_metadata_record(const GpsMetadataEvent &event,
                               char *path, size_t path_len,
                               char *row, size_t row_len,
                               char *error, size_t error_len)
{
    char date[16] = {};
    char local_timestamp[40] = {};
    char timezone[16] = {};
    format_local_time(event.received_timestamp, event.timezone_offset_hours,
                      date, sizeof(date), local_timestamp,
                      sizeof(local_timestamp), timezone, sizeof(timezone));

    const int path_written = snprintf(
        path, path_len, "/sdcard/metadata_v1_%s.csv", date);

    std::vector<std::string> fields(META_COLUMN_COUNT);
    fields[META_RECORD_TYPE] = "GPS";
    fields[META_LOCAL_TIMESTAMP] = local_timestamp;
    fields[META_UNIX_TIMESTAMP] = decimal_string(event.received_timestamp);
    fields[META_TIMEZONE] = timezone;
    fields[META_GATEWAY_ID] = Credentials::get_serial_str();
    fields[META_GATEWAY_FIRMWARE] = APP_VERSION;
    if (event.station_id != 0) fields[META_STATION_ID] = hex32_string(event.station_id);
    fields[META_STATION_TYPE] = event.station_type[0] != '\0' ? event.station_type : "unknown";
    if (event.paired_station_id != 0) {
        fields[META_PAIRED_STATION_ID] = hex32_string(event.paired_station_id);
    }
    fields[META_SOURCE_PAIRED] = event.source_is_paired ? "true" : "false";
    fields[META_SCHEMA_VERSION] = "1";
    fields[META_GPS_TIMESTAMP] = decimal_string(event.gps_timestamp);

    char latitude[24] = {};
    char longitude[24] = {};
    fields[META_GPS_LATITUDE] = format_float(
        event.latitude, latitude, sizeof(latitude), 6);
    fields[META_GPS_LONGITUDE] = format_float(
        event.longitude, longitude, sizeof(longitude), 6);
    if (event.satellites_available) {
        fields[META_GPS_SATELLITES] = decimal_string(event.satellites);
    }

    const std::string metadata_row = join_csv_fields(fields);
    const int row_written = snprintf(row, row_len, "%s", metadata_row.c_str());
    if (path_written < 0 || static_cast<size_t>(path_written) >= path_len ||
        row_written < 0 || static_cast<size_t>(row_written) >= row_len) {
        snprintf(error, error_len, "Could not format GPS metadata CSV record");
        return false;
    }
    return true;
}

bool ensure_sd_mounted(char *error, size_t error_len)
{
    struct stat st = {};
    if (stat("/sdcard", &st) == 0 && S_ISDIR(st.st_mode)) {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_sd_is_mounted = true;
        s_sd_mount_backoff_sec = SD_MOUNT_BACKOFF_INITIAL_SEC;
        s_sd_next_mount_ts = 0;
        s_sd_probe_requested = false;
        s_sd_status.mount_backoff_active = false;
        s_sd_status.next_mount_attempt_timestamp = 0;
        return true;
    }

    const esp_err_t mount_result = bsp_sdcard_init_default();
    if (mount_result != ESP_OK) {
        const time_t now = time(nullptr);
        const time_t backoff = s_sd_mount_backoff_sec;
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            s_sd_is_mounted = false;
            s_sd_probe_requested = false;
            s_sd_next_mount_ts = (now > 0 ? now : s_last_aggregated_interval) + backoff;
            s_sd_mount_backoff_sec = calculate_next_backoff(backoff);
            s_sd_status.mount_backoff_active = true;
            s_sd_status.next_mount_attempt_timestamp = s_sd_next_mount_ts;
        }
        ESP_LOGI(TAG, "SD mount failed (%s); backing off next mount attempt for %lld s (next at %lld)",
                 esp_err_to_name(mount_result), (long long)backoff, (long long)s_sd_next_mount_ts);
        snprintf(error, error_len, "SD mount failed: %s (backing off for %lld s)",
                 esp_err_to_name(mount_result), (long long)backoff);
        return false;
    }
    if (stat("/sdcard", &st) != 0 || !S_ISDIR(st.st_mode)) {
        const time_t now = time(nullptr);
        const time_t backoff = s_sd_mount_backoff_sec;
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            s_sd_is_mounted = false;
            s_sd_probe_requested = false;
            s_sd_next_mount_ts = (now > 0 ? now : s_last_aggregated_interval) + backoff;
            s_sd_mount_backoff_sec = calculate_next_backoff(backoff);
            s_sd_status.mount_backoff_active = true;
            s_sd_status.next_mount_attempt_timestamp = s_sd_next_mount_ts;
        }
        ESP_LOGW(TAG, "SD mount returned success but /sdcard is unavailable; backing off for %lld s",
                 (long long)backoff);
        snprintf(error, error_len, "SD mount returned success but /sdcard is unavailable");
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_sd_is_mounted = true;
        s_sd_mount_backoff_sec = SD_MOUNT_BACKOFF_INITIAL_SEC;
        s_sd_next_mount_ts = 0;
        s_sd_probe_requested = false;
        s_sd_status.mount_backoff_active = false;
        s_sd_status.next_mount_attempt_timestamp = 0;
    }
    return true;
}

bool rollback_partial_append(const char *path, long original_size)
{
    FILE *rollback = fopen(path, "r+");
    if (rollback == nullptr) return false;
    const int fd = fileno(rollback);
    bool ok = fd >= 0 && ftruncate(fd, static_cast<off_t>(original_size)) == 0;
    if (ok) ok = fflush(rollback) == 0;
    if (ok) ok = fsync(fd) == 0;
    if (fclose(rollback) != 0) ok = false;
    return ok;
}

bool append_csv_once(const char *path, const char *header, const char *row,
                     char *error, size_t error_len, bool *out_mount_failed = nullptr)
{
    if (out_mount_failed != nullptr) {
        *out_mount_failed = false;
    }

    bsp_spi_lock();

    char mount_error[128] = {};
    if (!ensure_sd_mounted(mount_error, sizeof(mount_error))) {
        if (out_mount_failed != nullptr) {
            *out_mount_failed = true;
        }
        snprintf(error, error_len, "%s", mount_error);
        bsp_spi_unlock();
        return false;
    }

    errno = 0;
    FILE *file = fopen(path, "a+");
    if (file == nullptr) {
        const int saved_errno = errno;
        snprintf(error, error_len, "Open failed for %s: %s", path,
                 saved_errno != 0 ? strerror(saved_errno) : "unknown error");
        if (saved_errno == ENODEV || saved_errno == EIO) {
            bsp_sdcard_deinit_default();
            std::lock_guard<std::mutex> lock(s_mutex);
            s_sd_is_mounted = false;
            const time_t now = time(nullptr);
            s_sd_next_mount_ts = (now > 0 ? now : s_last_aggregated_interval) + s_sd_mount_backoff_sec;
            s_sd_mount_backoff_sec = calculate_next_backoff(s_sd_mount_backoff_sec);
            s_sd_status.mount_backoff_active = true;
            s_sd_status.next_mount_attempt_timestamp = s_sd_next_mount_ts;
        }
        bsp_spi_unlock();
        return false;
    }

    bool ok = true;
    bool append_started = false;
    int saved_errno = 0;
    long original_size = 0;

    if (fseek(file, 0, SEEK_END) != 0 || (original_size = ftell(file)) < 0) {
        ok = false;
        saved_errno = errno;
        snprintf(error, error_len, "Seek failed for %s: %s", path,
                 saved_errno != 0 ? strerror(saved_errno) : "unknown error");
    }

    if (ok && original_size > 0) {
        const size_t expected_header_len = strlen(header);
        if (s_sd_writer_context == nullptr) {
            ok = false;
            snprintf(error, error_len,
                     "SD writer context unavailable for %s", path);
        } else if (expected_header_len > CSV_HEADER_BUFFER_SIZE) {
            ok = false;
            snprintf(error, error_len,
                     "CSV header is too large to validate for %s", path);
        } else if (fseek(file, 0, SEEK_SET) != 0 ||
                   fread(s_sd_writer_context->csv_header, 1,
                         expected_header_len, file) !=
                       expected_header_len ||
                   memcmp(s_sd_writer_context->csv_header, header,
                          expected_header_len) != 0) {
            ok = false;
            snprintf(error, error_len,
                     "CSV header mismatch for %s; delete or rename the old file",
                     path);
        } else if (fseek(file, 0, SEEK_END) != 0) {
            ok = false;
            saved_errno = errno;
            snprintf(error, error_len, "Seek failed for %s: %s", path,
                     saved_errno != 0 ? strerror(saved_errno) : "unknown error");
        }
    }

    if (ok && original_size == 0) {
        append_started = true;
        const size_t header_len = strlen(header);
        if (fwrite(header, 1, header_len, file) != header_len) {
            ok = false;
            saved_errno = errno;
            snprintf(error, error_len, "Header write failed for %s: %s", path,
                     saved_errno != 0 ? strerror(saved_errno) : "short write");
        }
    }

    if (ok) {
        append_started = true;
        const size_t row_len = strlen(row);
        if (fwrite(row, 1, row_len, file) != row_len) {
            ok = false;
            saved_errno = errno;
            snprintf(error, error_len, "Row write failed for %s: %s", path,
                     saved_errno != 0 ? strerror(saved_errno) : "short write");
        }
    }

    if (ok && fflush(file) != 0) {
        ok = false;
        saved_errno = errno;
        snprintf(error, error_len, "Flush failed for %s: %s", path,
                 saved_errno != 0 ? strerror(saved_errno) : "unknown error");
    }

    if (ok && fsync(fileno(file)) != 0) {
        ok = false;
        saved_errno = errno;
        snprintf(error, error_len, "Sync failed for %s: %s", path,
                 saved_errno != 0 ? strerror(saved_errno) : "unknown error");
    }

    if (fclose(file) != 0) {
        if (ok) {
            saved_errno = errno;
            snprintf(error, error_len, "Close failed for %s: %s", path,
                     saved_errno != 0 ? strerror(saved_errno) : "unknown error");
        }
        ok = false;
    }

    if (!ok && append_started && !rollback_partial_append(path, original_size)) {
        const size_t used = strlen(error);
        if (used + 20 < error_len) {
            snprintf(error + used, error_len - used, "; rollback failed");
        }
    }

    if (!ok && (saved_errno == ENODEV || saved_errno == EIO)) {
        bsp_sdcard_deinit_default();
        std::lock_guard<std::mutex> lock(s_mutex);
        s_sd_is_mounted = false;
        const time_t now = time(nullptr);
        s_sd_next_mount_ts = (now > 0 ? now : s_last_aggregated_interval) + s_sd_mount_backoff_sec;
        s_sd_mount_backoff_sec = calculate_next_backoff(s_sd_mount_backoff_sec);
        s_sd_status.mount_backoff_active = true;
        s_sd_status.next_mount_attempt_timestamp = s_sd_next_mount_ts;
    }

    bsp_spi_unlock();
    return ok;
}

bool write_csv_once(const WeatherAggregate &agg, CsvFileKind kind,
                    char *error, size_t error_len, bool *out_mount_failed = nullptr)
{
    if (s_sd_writer_context == nullptr) {
        snprintf(error, error_len,
                 "SD writer context unavailable for weather CSV");
        return false;
    }

    // The writer-owned buffers remain alive through the FAT append without
    // consuming task stack or allocating/freeing heap on every five-minute row.
    char *path = s_sd_writer_context->csv_path;
    char *row = s_sd_writer_context->csv_row;
    path[0] = '\0';
    row[0] = '\0';
    const char *header = nullptr;
    if (!build_csv_record(agg, kind, path, CSV_PATH_BUFFER_SIZE, row,
                          CSV_ROW_BUFFER_SIZE, &header, error, error_len)) {
        return false;
    }
    return append_csv_once(path, header, row, error, error_len, out_mount_failed);
}

bool write_csv_with_one_retry(const WeatherAggregate &agg, CsvFileKind kind,
                              char *final_error, size_t final_error_len,
                              bool *out_mount_failed = nullptr)
{
    char first_error[256] = {};
    bool mount_failed = false;
    if (write_csv_once(agg, kind, first_error, sizeof(first_error), &mount_failed)) {
        return true;
    }

    // Do NOT retry if mount itself failed (card missing / backoff active)
    if (mount_failed) {
        if (out_mount_failed != nullptr) {
            *out_mount_failed = true;
        }
        snprintf(final_error, final_error_len, "%s", first_error);
        return false;
    }

    note_retry();
    ESP_LOGW(TAG, "%s write failed; retrying once: %s",
             kind == CsvFileKind::WEATHER ? "Weather CSV" : "Metadata CSV",
             first_error);
    vTaskDelay(SD_RETRY_DELAY_TICKS);

    char retry_error[256] = {};
    if (write_csv_once(agg, kind, retry_error, sizeof(retry_error))) {
        ESP_LOGW(TAG, "%s write recovered on retry",
                 kind == CsvFileKind::WEATHER ? "Weather CSV" : "Metadata CSV");
        return true;
    }

    snprintf(final_error, final_error_len, "%s failed twice: %s",
             kind == CsvFileKind::WEATHER ? "Weather CSV" : "Metadata CSV",
             retry_error);
    return false;
}

bool write_gps_metadata_once(const GpsMetadataEvent &event,
                             char *error, size_t error_len, bool *out_mount_failed = nullptr)
{
    if (s_sd_writer_context == nullptr) {
        snprintf(error, error_len,
                 "SD writer context unavailable for GPS metadata CSV");
        return false;
    }

    char *path = s_sd_writer_context->csv_path;
    char *row = s_sd_writer_context->csv_row;
    path[0] = '\0';
    row[0] = '\0';
    if (!build_gps_metadata_record(event, path, CSV_PATH_BUFFER_SIZE,
                                   row, CSV_ROW_BUFFER_SIZE, error,
                                   error_len)) {
        return false;
    }
    return append_csv_once(path, METADATA_CSV_HEADER, row, error, error_len, out_mount_failed);
}

bool write_gps_metadata_with_one_retry(const GpsMetadataEvent &event,
                                       char *final_error,
                                       size_t final_error_len,
                                       bool *out_mount_failed = nullptr)
{
    char first_error[256] = {};
    bool mount_failed = false;
    if (write_gps_metadata_once(event, first_error, sizeof(first_error), &mount_failed)) {
        return true;
    }

    if (mount_failed) {
        if (out_mount_failed != nullptr) {
            *out_mount_failed = true;
        }
        snprintf(final_error, final_error_len, "%s", first_error);
        return false;
    }

    note_retry();
    ESP_LOGW(TAG, "GPS metadata write failed; retrying once: %s", first_error);
    vTaskDelay(SD_RETRY_DELAY_TICKS);

    char retry_error[256] = {};
    if (write_gps_metadata_once(event, retry_error, sizeof(retry_error))) {
        ESP_LOGW(TAG, "GPS metadata write recovered on retry");
        return true;
    }

    snprintf(final_error, final_error_len,
             "GPS metadata CSV failed twice: %s", retry_error);
    return false;
}


void get_aggregate_local_date(const WeatherAggregate &aggregate,
                              char *local_date, size_t local_date_len)
{
    char unused_timestamp[40] = {};
    char unused_timezone[16] = {};
    format_local_time(aggregate, local_date, local_date_len,
                      unused_timestamp, sizeof(unused_timestamp),
                      unused_timezone, sizeof(unused_timezone));
}

WeatherAggregate daily_start_from_gps(const GpsMetadataEvent &event)
{
    WeatherAggregate start;
    start.timestamp = event.received_timestamp;
    start.station_id = event.station_id;
    start.paired_station_id = event.paired_station_id;
    start.source_is_paired = event.source_is_paired;
    start.timezone_offset_hours = event.timezone_offset_hours;
    snprintf(start.packer_name, sizeof(start.packer_name), "%s",
             event.station_type[0] != '\0' ? event.station_type : "unknown");
    return start;
}

void ensure_daily_start_record(DailyMetadataState &state,
                               const WeatherAggregate &source,
                               char *error, size_t error_len)
{
    char local_date[16] = {};
    get_aggregate_local_date(source, local_date, sizeof(local_date));
    if (strcmp(state.started_local_date, local_date) == 0) return;

    WeatherAggregate start = source;
    start.coverage_start_timestamp = 0;
    start.coverage_end_timestamp = 0;
    start.intervals_observed = 0;
    start.packet_count = 0;
    start.expected_packet_count = 0;
    start.packet_counter_available = false;
    start.packet_counter_gaps = 0;
    start.field_presence_mask = 0;
    start.quality_flags = WEATHER_QUALITY_OK;
    start.rssi_available = false;

    error[0] = '\0';
    bool mount_failed = false;
    const bool metadata_ok = write_csv_with_one_retry(
        start, CsvFileKind::METADATA, error, error_len, &mount_failed);
    if (!mount_failed) {
        note_write_result(CsvFileKind::METADATA, metadata_ok, error);
    }
    if (metadata_ok) {
        ESP_LOGI(TAG, "Logged GPS-empty daily-start metadata for %s", local_date);
    } else if (!mount_failed) {
        ESP_LOGE(TAG, "%s", error);
    }
    // One retry then skip applies to this record too. Remember the attempt so a
    // persistent card fault does not cause another write on every 5-minute row.
    snprintf(state.started_local_date, sizeof(state.started_local_date), "%s",
             local_date);
}

void start_daily_metadata(DailyMetadataState &state,
                          const WeatherAggregate &aggregate,
                          const char *local_date)
{
    char started_local_date[16] = {};
    snprintf(started_local_date, sizeof(started_local_date), "%s",
             state.started_local_date);
    state = {};
    snprintf(state.started_local_date, sizeof(state.started_local_date), "%s",
             started_local_date);
    state.active = true;
    snprintf(state.local_date, sizeof(state.local_date), "%s", local_date);
    state.aggregate = aggregate;
    state.aggregate.coverage_start_timestamp = aggregate.timestamp;
    state.aggregate.coverage_end_timestamp =
        aggregate.timestamp + AGGREGATE_INTERVAL_SEC;
    state.aggregate.intervals_observed = 1;
    if (aggregate.rssi_available) {
        state.rssi_weight = std::max<uint32_t>(aggregate.packet_count, 1U);
        state.rssi_weighted_sum =
            static_cast<int64_t>(aggregate.rssi_avg) * state.rssi_weight;
    }
}

void add_daily_metadata(DailyMetadataState &state,
                        const WeatherAggregate &aggregate)
{
    WeatherAggregate &daily = state.aggregate;
    daily.timestamp = aggregate.timestamp;
    daily.coverage_end_timestamp =
        aggregate.timestamp + AGGREGATE_INTERVAL_SEC;
    if (daily.intervals_observed < UINT16_MAX) daily.intervals_observed++;

    if (daily.station_id == 0) {
        daily.station_id = aggregate.station_id;
    } else if (aggregate.station_id != 0 &&
               daily.station_id != aggregate.station_id) {
        daily.quality_flags |= WEATHER_QUALITY_MIXED_STATION_IDS;
    }
    daily.paired_station_id = aggregate.paired_station_id;
    daily.source_is_paired = daily.source_is_paired && aggregate.source_is_paired;
    daily.packet_count += aggregate.packet_count;
    daily.expected_packet_count += aggregate.expected_packet_count;
    daily.packet_counter_gaps += aggregate.packet_counter_gaps;
    daily.field_presence_mask |= aggregate.field_presence_mask;
    daily.quality_flags |= aggregate.quality_flags;
    daily.timezone_offset_hours = aggregate.timezone_offset_hours;

    if (aggregate.packet_counter_available) {
        if (!daily.packet_counter_available) {
            daily.first_packet_counter = aggregate.first_packet_counter;
            daily.packet_counter_available = true;
        }
        daily.last_packet_counter = aggregate.last_packet_counter;
    }
    if (aggregate.tx_interval_sec != 0) {
        if (daily.tx_interval_sec != 0 &&
            daily.tx_interval_sec != aggregate.tx_interval_sec) {
            daily.quality_flags |= WEATHER_QUALITY_TX_INTERVAL_CHANGED;
        }
        daily.tx_interval_sec = aggregate.tx_interval_sec;
    }
    if (aggregate.station_fw_available) {
        daily.station_fw_available = true;
        daily.station_fw_version = aggregate.station_fw_version;
    }
    if (aggregate.battery_mv_available) {
        if (!daily.battery_mv_available ||
            aggregate.battery_min_mv < daily.battery_min_mv) {
            daily.battery_min_mv = aggregate.battery_min_mv;
        }
        daily.battery_mv_available = true;
    }
    if (aggregate.battery_low_available) {
        daily.battery_low_available = true;
        daily.battery_low = daily.battery_low || aggregate.battery_low;
    }
    if (aggregate.rssi_available) {
        const uint32_t weight = std::max<uint32_t>(aggregate.packet_count, 1U);
        state.rssi_weighted_sum +=
            static_cast<int64_t>(aggregate.rssi_avg) * weight;
        state.rssi_weight += weight;
        daily.rssi_available = true;
        daily.rssi_avg = static_cast<int16_t>(
            state.rssi_weighted_sum / state.rssi_weight);
    }
    if (daily.packer_name[0] == '\0' && aggregate.packer_name[0] != '\0') {
        snprintf(daily.packer_name, sizeof(daily.packer_name), "%s",
                 aggregate.packer_name);
    }
}

void accumulate_daily_metadata(DailyMetadataState &state,
                               const WeatherAggregate &aggregate,
                               char *error, size_t error_len)
{
    char local_date[16] = {};
    get_aggregate_local_date(aggregate, local_date, sizeof(local_date));

    if (state.active && strcmp(state.local_date, local_date) != 0) {
        error[0] = '\0';
        bool mount_failed = false;
        const bool metadata_ok = write_csv_with_one_retry(
            state.aggregate, CsvFileKind::METADATA, error, error_len, &mount_failed);
        if (!mount_failed) {
            note_write_result(CsvFileKind::METADATA, metadata_ok, error);
        }
        if (metadata_ok) {
            ESP_LOGI(TAG,
                     "Logged daily metadata summary for %s (%u intervals)",
                     state.local_date,
                     static_cast<unsigned int>(
                         state.aggregate.intervals_observed));
        } else if (!mount_failed) {
            ESP_LOGE(TAG, "%s", error);
        }
        char already_started_date[16] = {};
        if (strcmp(state.started_local_date, local_date) == 0) {
            snprintf(already_started_date, sizeof(already_started_date), "%s",
                     state.started_local_date);
        }
        state = {};
        snprintf(state.started_local_date, sizeof(state.started_local_date),
                 "%s", already_started_date);
    }

    ensure_daily_start_record(state, aggregate, error, error_len);
    if (!state.active) {
        start_daily_metadata(state, aggregate, local_date);
    } else {
        add_daily_metadata(state, aggregate);
    }
}

void accumulate_daily_metadata_in_memory(DailyMetadataState &state,
                                         const WeatherAggregate &aggregate)
{
    char local_date[16] = {};
    get_aggregate_local_date(aggregate, local_date, sizeof(local_date));

    if (state.active && strcmp(state.local_date, local_date) != 0) {
        char already_started_date[16] = {};
        if (strcmp(state.started_local_date, local_date) == 0) {
            snprintf(already_started_date, sizeof(already_started_date), "%s",
                     state.started_local_date);
        }
        state = {};
        snprintf(state.started_local_date, sizeof(state.started_local_date),
                 "%s", already_started_date);
    }

    snprintf(state.started_local_date, sizeof(state.started_local_date), "%s", local_date);
    if (!state.active) {
        start_daily_metadata(state, aggregate, local_date);
    } else {
        add_daily_metadata(state, aggregate);
    }
}

bool can_attempt_sd_write()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_sd_is_mounted) {
        return true;
    }
    struct stat st = {};
    if (stat("/sdcard", &st) == 0 && S_ISDIR(st.st_mode)) {
        s_sd_is_mounted = true;
        s_sd_status.mount_backoff_active = false;
        s_sd_status.next_mount_attempt_timestamp = 0;
        return true;
    }
    if (s_sd_probe_requested) {
        return true;
    }
    const time_t now = time(nullptr);
    if (s_sd_next_mount_ts == 0 || (now > 0 && now >= s_sd_next_mount_ts)) {
        return true;
    }
    s_sd_status.mount_backoff_active = true;
    s_sd_status.next_mount_attempt_timestamp = s_sd_next_mount_ts;
    return false;
}

void sd_writer_task(void *)
{
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_sd_status.writer_running = true;
    }

    std::unique_ptr<SdWriterContext> context(
        new (std::nothrow) SdWriterContext());
    if (!context) {
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            s_sd_status.writer_running = false;
            set_status_error_locked("Could not allocate SD writer context");
            s_sd_writer_task_handle = nullptr;
        }
        ESP_LOGE(TAG, "Could not allocate SD writer context");
        vTaskDelete(nullptr);
        return;
    }
    s_sd_writer_context = context.get();

    bool stack_headroom_logged = false;
    for (;;) {
        SdWriteRequest &request = context->request;
        DailyMetadataState &daily_metadata = context->daily_metadata;
        char *error = context->error;
        if (xQueueReceive(s_sd_write_queue, &request, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (!can_attempt_sd_write()) {
            if (request.type != SdWriteRequestType::GPS_METADATA) {
                accumulate_daily_metadata_in_memory(daily_metadata, request.aggregate);
            }
            continue;
        }

        error[0] = '\0';
        if (request.type == SdWriteRequestType::GPS_METADATA) {
            const WeatherAggregate daily_start =
                daily_start_from_gps(request.gps);
            ensure_daily_start_record(daily_metadata, daily_start,
                                      error, SD_WRITER_ERROR_BUFFER_SIZE);
            error[0] = '\0';
            bool mount_failed = false;
            const bool metadata_ok = write_gps_metadata_with_one_retry(
                request.gps, error, SD_WRITER_ERROR_BUFFER_SIZE, &mount_failed);
            if (!mount_failed) {
                note_write_result(CsvFileKind::METADATA, metadata_ok, error);
            }
            if (metadata_ok) {
                ESP_LOGI(TAG, "Logged GPS metadata row for %lld",
                         (long long)request.gps.received_timestamp);
            } else if (!mount_failed) {
                ESP_LOGE(TAG, "%s", error);
            }
        } else {
            const WeatherAggregate &agg = request.aggregate;
            accumulate_daily_metadata(daily_metadata, agg, error,
                                      SD_WRITER_ERROR_BUFFER_SIZE);
            error[0] = '\0';
            bool mount_failed = false;
            const bool weather_ok = write_csv_with_one_retry(
                agg, CsvFileKind::WEATHER, error,
                SD_WRITER_ERROR_BUFFER_SIZE, &mount_failed);
            if (!mount_failed) {
                note_write_result(CsvFileKind::WEATHER, weather_ok, error);
            }
            if (weather_ok) {
                ESP_LOGI(TAG, "Logged 5-minute weather row for %lld",
                         (long long)agg.timestamp);
            } else if (!mount_failed) {
                ESP_LOGE(TAG, "%s", error);
            }
        }

        const UBaseType_t stack_free_bytes =
            uxTaskGetStackHighWaterMark(nullptr);
        if (stack_free_bytes < SD_WRITER_STACK_WARN_BYTES) {
            ESP_LOGW(TAG,
                     "SD writer stack headroom is low: %u bytes minimum free",
                     static_cast<unsigned int>(stack_free_bytes));
        } else if (!stack_headroom_logged) {
            ESP_LOGI(TAG, "SD writer stack minimum free: %u bytes",
                     static_cast<unsigned int>(stack_free_bytes));
            stack_headroom_logged = true;
        }
    }
}


void queue_sd_write(const WeatherAggregate &agg)
{
    SdWriteRequest request;
    request.type = SdWriteRequestType::WEATHER_AGGREGATE;
    request.aggregate = agg;
    if (s_sd_write_queue == nullptr ||
        xQueueSend(s_sd_write_queue, &request, 0) != pdTRUE) {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_sd_status.weather_rows_failed++;
        set_status_error_locked("SD writer queue full; aggregate skipped");
        ESP_LOGE(TAG, "SD writer queue full; aggregate %lld skipped",
                 (long long)agg.timestamp);
    }
}

void queue_gps_metadata_write(const GpsMetadataEvent &event)
{
    SdWriteRequest request;
    request.type = SdWriteRequestType::GPS_METADATA;
    request.gps = event;
    if (s_sd_write_queue == nullptr ||
        xQueueSend(s_sd_write_queue, &request, 0) != pdTRUE) {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_sd_status.metadata_rows_failed++;
        set_status_error_locked("SD writer queue full; GPS metadata skipped");
        ESP_LOGE(TAG, "SD writer queue full; GPS metadata %lld skipped",
                 (long long)event.received_timestamp);
    }
}

#endif // !CONFIG_SCREENLESS

} // namespace

void init()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    s_raw_buffer.clear();
    s_aggregates.clear();
    s_last_aggregated_interval = 0;
    s_last_precip_cml = NAN;
    s_last_precip_station_id = 0;
    s_sd_status = {};
#ifndef CONFIG_SCREENLESS
    s_sd_mount_backoff_sec = SD_MOUNT_BACKOFF_INITIAL_SEC;
    s_sd_next_mount_ts = 0;
    s_sd_probe_requested = true;
    s_sd_is_mounted = false;
#endif

#ifndef CONFIG_SCREENLESS
    if (s_sd_write_queue == nullptr) {
        s_sd_write_queue = xQueueCreate(SD_WRITE_QUEUE_LENGTH, sizeof(SdWriteRequest));
    }
    if (s_sd_write_queue == nullptr) {
        set_status_error_locked("Could not create SD writer queue");
        ESP_LOGE(TAG, "%s", s_sd_status.last_error.c_str());
    } else if (s_sd_writer_task_handle == nullptr) {
        const BaseType_t created = xTaskCreatePinnedToCore(
            sd_writer_task, "sd_writer", SD_WRITER_TASK_STACK_SIZE, nullptr,
            SD_WRITER_TASK_PRIORITY, &s_sd_writer_task_handle, 0);
        if (created != pdPASS) {
            s_sd_writer_task_handle = nullptr;
            set_status_error_locked("Could not create low-priority SD writer task");
            ESP_LOGE(TAG, "%s", s_sd_status.last_error.c_str());
        }
    } else {
        s_sd_status.writer_running = true;
    }
#endif

    ESP_LOGI(TAG, "Weather Logger initialized successfully.");
}

void push_raw(const WsFields::FieldPack *pack)
{
    if (pack == nullptr) return;

    std::lock_guard<std::mutex> lock(s_mutex);
    s_raw_buffer.push_back(*pack);

    const uint32_t latest_ts = pack->ts;
    while (!s_raw_buffer.empty() && latest_ts >= s_raw_buffer.front().ts &&
           latest_ts - s_raw_buffer.front().ts > RAW_RETENTION_SEC) {
        s_raw_buffer.erase(s_raw_buffer.begin());
    }

    if (!DatetimeRtc::cur_time_valid()) {
        return;
    }

    const time_t cur_interval = (pack->ts / AGGREGATE_INTERVAL_SEC) * AGGREGATE_INTERVAL_SEC;
    if (s_last_aggregated_interval == 0) {
        s_last_aggregated_interval = cur_interval;
        return;
    }

    if (cur_interval <= s_last_aggregated_interval) {
        return;
    }

    for (time_t target_interval = s_last_aggregated_interval + AGGREGATE_INTERVAL_SEC;
         target_interval <= cur_interval; target_interval += AGGREGATE_INTERVAL_SEC) {
        const time_t start_ts = target_interval - AGGREGATE_INTERVAL_SEC;
        const time_t end_ts = target_interval;

        float temp_sum = 0.0f, hum_sum = 0.0f, wind_sum = 0.0f;
        float gust_max = 0.0f, precip_sum = 0.0f, press_sum = 0.0f, solar_sum = 0.0f;
        float wind_sin_sum = 0.0f, wind_cos_sum = 0.0f;
        int temp_count = 0, hum_count = 0, wind_count = 0, direction_count = 0;
        int gust_count = 0, precip_count = 0, pressure_count = 0, solar_count = 0;
        int rssi_sum = 0, rssi_count = 0;

        float precip_accum_slot = 0.0f;
        bool cumulative_precip_found = false;
        uint32_t field_presence = 0;
        uint32_t quality_flags = WEATHER_QUALITY_OK;
        uint16_t packet_count = 0;
        uint32_t station_id = 0;
        uint16_t tx_interval_sec = 0;
        uint32_t first_packet_counter = 0;
        uint32_t last_packet_counter = 0;
        uint32_t packet_counter_gaps = 0;
        uint32_t previous_packet_counter = 0;
        uint32_t counter_station_id = 0;
        bool packet_counter_available = false;
        bool previous_packet_counter_available = false;
        uint16_t battery_min_mv = UINT16_MAX;
        bool battery_mv_available = false;
        bool battery_low_available = false;
        bool battery_low = false;
        uint8_t station_fw_version = 0;
        bool station_fw_available = false;
        char packer_name[7] = {};

        for (auto raw : s_raw_buffer) {
            if (raw.ts < static_cast<uint32_t>(start_ts) ||
                raw.ts >= static_cast<uint32_t>(end_ts)) {
                continue;
            }

            packet_count++;
            if (raw.bad_values_detected) quality_flags |= WEATHER_QUALITY_BAD_VALUES;
            if (packer_name[0] == '\0') {
                snprintf(packer_name, sizeof(packer_name), "%s", raw.packer_name);
            }

            uint32_t packet_station_id = 0;
            if (WsFields::get_field_val_from_pack(
                    &raw, WsFields::FieldType::META_ID, nullptr,
                    &packet_station_id) == WsCommon::RetRes::OK) {
                if (station_id == 0) station_id = packet_station_id;
                else if (station_id != packet_station_id)
                    quality_flags |= WEATHER_QUALITY_MIXED_STATION_IDS;
            }

            uint32_t packet_counter = 0;
            if (WsFields::get_field_val_from_pack(
                    &raw, WsFields::FieldType::META_PCK_COUNT, nullptr,
                    &packet_counter) == WsCommon::RetRes::OK) {
                if (!packet_counter_available) {
                    first_packet_counter = packet_counter;
                    packet_counter_available = true;
                }
                if (previous_packet_counter_available &&
                    (counter_station_id == 0 || packet_station_id == 0 ||
                     counter_station_id == packet_station_id)) {
                    const uint32_t delta = packet_counter - previous_packet_counter;
                    if (delta > 1 && delta < 0x80000000U) {
                        packet_counter_gaps += delta - 1;
                        quality_flags |= WEATHER_QUALITY_PACKET_COUNTER_GAP;
                    } else if (delta >= 0x80000000U) {
                        quality_flags |= WEATHER_QUALITY_PACKET_COUNTER_RESET;
                    }
                } else if (previous_packet_counter_available) {
                    previous_packet_counter_available = false;
                }
                previous_packet_counter = packet_counter;
                last_packet_counter = packet_counter;
                counter_station_id = packet_station_id;
                previous_packet_counter_available = true;
            }

            uint16_t tx_interval = 0;
            if (WsFields::get_field_val_from_pack(
                    &raw, WsFields::FieldType::META_TX_INTERVAL, nullptr,
                    &tx_interval) == WsCommon::RetRes::OK && tx_interval > 0) {
                if (tx_interval_sec == 0) tx_interval_sec = tx_interval;
                else if (tx_interval_sec != tx_interval)
                    quality_flags |= WEATHER_QUALITY_TX_INTERVAL_CHANGED;
            }

            uint16_t battery_mv = 0;
            if (WsFields::get_field_val_from_pack(
                    &raw, WsFields::FieldType::META_BAT_MV, nullptr,
                    &battery_mv) == WsCommon::RetRes::OK) {
                battery_mv_available = true;
                battery_min_mv = std::min(battery_min_mv, battery_mv);
            }
            bool packet_battery_low = false;
            if (WsFields::get_field_val_from_pack(
                    &raw, WsFields::FieldType::META_BAT_LOW, nullptr,
                    &packet_battery_low) == WsCommon::RetRes::OK) {
                battery_low_available = true;
                battery_low = battery_low || packet_battery_low;
            }
            int packet_rssi = 0;
            if (WsFields::get_field_val_from_pack(
                    &raw, WsFields::FieldType::META_RSSI, nullptr,
                    &packet_rssi) == WsCommon::RetRes::OK) {
                rssi_sum += packet_rssi;
                rssi_count++;
            }
            uint8_t packet_station_fw = 0;
            if (WsFields::get_field_val_from_pack(
                    &raw, WsFields::FieldType::META_FW_VER, nullptr,
                    &packet_station_fw) == WsCommon::RetRes::OK) {
                station_fw_version = packet_station_fw;
                station_fw_available = true;
            }

            float value = 0.0f;
            if (WsFields::get_field_val_from_pack(&raw, WsFields::FieldType::WEATHER_TEMP, nullptr, &value) == WsCommon::RetRes::OK) {
                temp_sum += value; temp_count++; field_presence |= FIELD_PRESENT_TEMP;
            }
            if (WsFields::get_field_val_from_pack(&raw, WsFields::FieldType::WEATHER_HUM, nullptr, &value) == WsCommon::RetRes::OK) {
                hum_sum += value; hum_count++; field_presence |= FIELD_PRESENT_HUM;
            }
            if (WsFields::get_field_val_from_pack(&raw, WsFields::FieldType::WEATHER_WIND_SPEED, nullptr, &value) == WsCommon::RetRes::OK) {
                wind_sum += value; wind_count++; field_presence |= FIELD_PRESENT_WIND_SPEED;
            }

            uint16_t direction = 0;
            float wind_speed = 0.0f;
            const bool wind_speed_available = WsFields::get_field_val_from_pack(
                &raw, WsFields::FieldType::WEATHER_WIND_SPEED, nullptr,
                &wind_speed) == WsCommon::RetRes::OK;
            if (WsFields::get_field_val_from_pack(
                    &raw, WsFields::FieldType::WEATHER_WIND_DIR, nullptr,
                    &direction) == WsCommon::RetRes::OK) {
                const float radians = static_cast<float>(direction) *
                    (3.14159265358979323846f / 180.0f);
                const float weight = wind_speed_available && wind_speed > 0.0f ? wind_speed : 1.0f;
                wind_sin_sum += weight * sinf(radians);
                wind_cos_sum += weight * cosf(radians);
                direction_count++;
                field_presence |= FIELD_PRESENT_WIND_DIR;
            }
            if (WsFields::get_field_val_from_pack(&raw, WsFields::FieldType::WEATHER_WIND_GUST, nullptr, &value) == WsCommon::RetRes::OK) {
                gust_max = std::max(gust_max, value); gust_count++; field_presence |= FIELD_PRESENT_WIND_GUST;
            }
            if (WsFields::get_field_val_from_pack(&raw, WsFields::FieldType::WEATHER_PRECIP_RATE, nullptr, &value) == WsCommon::RetRes::OK) {
                precip_sum += value; precip_count++; field_presence |= FIELD_PRESENT_PRECIP_RATE;
            }

            float cumulative_precip = 0.0f;
            if (WsFields::get_field_val_from_pack(
                    &raw, WsFields::FieldType::WEATHER_PRECIP_CML, nullptr,
                    &cumulative_precip) == WsCommon::RetRes::OK) {
                cumulative_precip_found = true;
                field_presence |= FIELD_PRESENT_PRECIP_CML;
                if (!std::isnan(s_last_precip_cml) &&
                    (s_last_precip_station_id == 0 || packet_station_id == 0 ||
                     s_last_precip_station_id == packet_station_id)) {
                    if (cumulative_precip >= s_last_precip_cml) {
                        precip_accum_slot += cumulative_precip - s_last_precip_cml;
                    } else {
                        precip_accum_slot += cumulative_precip;
                        quality_flags |= WEATHER_QUALITY_PRECIP_COUNTER_RESET;
                    }
                } else {
                    quality_flags |= WEATHER_QUALITY_PRECIP_BASELINE_MISSING;
                }
                s_last_precip_cml = cumulative_precip;
                s_last_precip_station_id = packet_station_id;
            }
            if (WsFields::get_field_val_from_pack(&raw, WsFields::FieldType::WEATHER_PRESSURE, nullptr, &value) == WsCommon::RetRes::OK) {
                press_sum += value; pressure_count++; field_presence |= FIELD_PRESENT_PRESSURE;
            }
            if (WsFields::get_field_val_from_pack(&raw, WsFields::FieldType::WEATHER_SOLAR_RAD, nullptr, &value) == WsCommon::RetRes::OK) {
                solar_sum += value; solar_count++; field_presence |= FIELD_PRESENT_SOLAR_RAD;
            }
        }

        if (!cumulative_precip_found && precip_count > 0) {
            precip_accum_slot = (precip_sum / precip_count) * (5.0f / 60.0f);
            quality_flags |= WEATHER_QUALITY_PRECIP_RATE_FALLBACK;
        }

        const bool any_weather_data = temp_count > 0 || hum_count > 0 ||
            wind_count > 0 || direction_count > 0 || gust_count > 0 ||
            precip_count > 0 || cumulative_precip_found || pressure_count > 0 ||
            solar_count > 0;
        if (!any_weather_data) {
            continue;
        }

        WeatherAggregate agg;
        agg.timestamp = start_ts;
        agg.temp_avg = temp_count > 0 ? temp_sum / temp_count : NAN;
        agg.hum_avg = hum_count > 0 ? hum_sum / hum_count : NAN;
        agg.wind_speed_avg = wind_count > 0 ? wind_sum / wind_count : NAN;
        if (direction_count > 0 &&
            (fabsf(wind_sin_sum) > 1e-4f || fabsf(wind_cos_sum) > 1e-4f)) {
            float average_degrees = atan2f(wind_sin_sum, wind_cos_sum) *
                (180.0f / 3.14159265358979323846f);
            if (average_degrees < 0.0f) average_degrees += 360.0f;
            agg.wind_dir_prevailing = average_degrees;
        }
        agg.wind_gust_max = gust_count > 0 ? gust_max : NAN;
        agg.precip_avg = precip_count > 0 ? precip_sum / precip_count : NAN;
        agg.precip_accum = (cumulative_precip_found || precip_count > 0) ? precip_accum_slot : NAN;
        agg.pressure_avg = pressure_count > 0 ? press_sum / pressure_count : NAN;
        agg.solar_rad_avg = solar_count > 0 ? solar_sum / solar_count : NAN;

        agg.station_id = station_id;
        agg.paired_station_id = WsHandler::get_paired_id();
        agg.packet_count = packet_count;
        agg.tx_interval_sec = tx_interval_sec;
        agg.expected_packet_count = tx_interval_sec > 0
            ? static_cast<uint32_t>((AGGREGATE_INTERVAL_SEC + tx_interval_sec - 1) / tx_interval_sec)
            : 0;
        agg.packet_counter_available = packet_counter_available;
        agg.first_packet_counter = first_packet_counter;
        agg.last_packet_counter = last_packet_counter;
        agg.packet_counter_gaps = packet_counter_gaps;
        agg.field_presence_mask = field_presence;
        agg.battery_mv_available = battery_mv_available;
        agg.battery_low_available = battery_low_available;
        agg.battery_min_mv = battery_mv_available && battery_min_mv != UINT16_MAX ? battery_min_mv : 0;
        agg.battery_low = battery_low;
        agg.rssi_available = rssi_count > 0;
        agg.rssi_avg = rssi_count > 0 ? static_cast<int16_t>(rssi_sum / rssi_count) : 0;
        agg.station_fw_available = station_fw_available;
        agg.station_fw_version = station_fw_version;
        agg.timezone_offset_hours = static_cast<int8_t>(normalized_timezone_offset(Settings::get_tz_offset()));
        agg.source_is_paired = WsHandler::get_pair_state() == WsHandler::PairState::PAIRED &&
            station_id != 0 && station_id == agg.paired_station_id;
        snprintf(agg.packer_name, sizeof(agg.packer_name), "%s", packer_name);

        if ((field_presence & REQUIRED_NON_PRECIP_FIELDS) != REQUIRED_NON_PRECIP_FIELDS ||
            (field_presence & (FIELD_PRESENT_PRECIP_RATE | FIELD_PRESENT_PRECIP_CML)) == 0) {
            quality_flags |= WEATHER_QUALITY_MISSING_FIELDS;
        }
        if (agg.expected_packet_count > 0 && packet_count < agg.expected_packet_count) {
            quality_flags |= WEATHER_QUALITY_LOW_PACKET_COUNT;
        }
        if (station_id == 0) quality_flags |= WEATHER_QUALITY_STATION_ID_MISSING;
        if (!agg.source_is_paired) quality_flags |= WEATHER_QUALITY_UNPAIRED_SOURCE;
        agg.quality_flags = quality_flags;
        agg.valid = true;

        if (s_aggregates.size() >= MAX_AGGREGATES) {
            s_aggregates.erase(s_aggregates.begin());
        }
        s_aggregates.push_back(agg);

#ifndef CONFIG_SCREENLESS
        queue_sd_write(agg);
#endif
    }

    s_last_aggregated_interval = cur_interval;
}

void log_gps_fix(uint32_t station_id, const char *station_type,
                 WsGps::GpsData *gps_data)
{
#ifdef CONFIG_SCREENLESS
    (void)station_id;
    (void)station_type;
    (void)gps_data;
#else
    if (gps_data == nullptr || !WsGps::gps_data_is_usable(gps_data)) return;

    GpsMetadataEvent event;
    if (WsFields::get_field_val_from_pack(
            &gps_data->field_pack, WsFields::FieldType::GPS_TS, nullptr,
            &event.gps_timestamp) != WsCommon::RetRes::OK ||
        WsFields::get_field_val_from_pack(
            &gps_data->field_pack, WsFields::FieldType::GPS_LAT, nullptr,
            &event.latitude) != WsCommon::RetRes::OK ||
        WsFields::get_field_val_from_pack(
            &gps_data->field_pack, WsFields::FieldType::GPS_LON, nullptr,
            &event.longitude) != WsCommon::RetRes::OK) {
        return;
    }

    uint8_t satellites = 0;
    if (WsFields::get_field_val_from_pack(
            &gps_data->field_pack, WsFields::FieldType::GPS_SATS, nullptr,
            &satellites) == WsCommon::RetRes::OK) {
        event.satellites = satellites;
        event.satellites_available = true;
    }
    event.received_timestamp = time(nullptr);
    if (event.received_timestamp <= 0) {
        event.received_timestamp = event.gps_timestamp;
    }
    event.station_id = station_id;
    event.paired_station_id = WsHandler::get_paired_id();
    event.source_is_paired =
        WsHandler::get_pair_state() == WsHandler::PairState::PAIRED &&
        station_id != 0 && station_id == event.paired_station_id;
    event.timezone_offset_hours = static_cast<int8_t>(
        normalized_timezone_offset(Settings::get_tz_offset()));
    snprintf(event.station_type, sizeof(event.station_type), "%s",
             station_type != nullptr ? station_type : "unknown");
    queue_gps_metadata_write(event);
#endif
}

std::vector<WsFields::FieldPack> get_raw_buffer()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    return s_raw_buffer;
}

std::vector<WeatherAggregate> get_aggregates()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    return s_aggregates;
}

SdLoggerStatus get_sd_status()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    SdLoggerStatus status = s_sd_status;
#ifndef CONFIG_SCREENLESS
    if (s_sd_write_queue != nullptr) {
        status.pending_records = uxQueueMessagesWaiting(s_sd_write_queue);
    }
    const time_t now = time(nullptr);
    status.mount_backoff_active = !s_sd_is_mounted && s_sd_next_mount_ts > 0 &&
                                  (now <= 0 || now < s_sd_next_mount_ts);
    status.next_mount_attempt_timestamp = status.mount_backoff_active ? s_sd_next_mount_ts : 0;
#endif
    return status;
}

bool is_sd_mounted()
{
#ifdef CONFIG_SCREENLESS
    return false;
#else
    struct stat st = {};
    const bool mounted = stat("/sdcard", &st) == 0 && S_ISDIR(st.st_mode);
    std::lock_guard<std::mutex> lock(s_mutex);
    s_sd_is_mounted = mounted;
    return mounted;
#endif
}

bool format_sd()
{
#ifdef CONFIG_SCREENLESS
    ESP_LOGW(TAG, "SD card formatting is not supported on screenless targets");
    return false;
#else
    Utils::set_idle_task_watchdog_state(false);
    bsp_spi_lock();
    ESP_LOGI(TAG, "Formatting SD Card...");
    const esp_err_t result = bsp_sdcard_format();
    bsp_spi_unlock();
    Utils::set_idle_task_watchdog_state(true);
    if (result == ESP_OK) {
        ESP_LOGI(TAG, "SD Card formatted successfully");
        std::lock_guard<std::mutex> lock(s_mutex);
        s_sd_is_mounted = true;
        s_sd_mount_backoff_sec = SD_MOUNT_BACKOFF_INITIAL_SEC;
        s_sd_next_mount_ts = 0;
        s_sd_probe_requested = false;
        s_sd_status.mount_backoff_active = false;
        s_sd_status.next_mount_attempt_timestamp = 0;
        s_sd_status.last_error.clear();
        s_sd_status.last_error_timestamp = 0;
        return true;
    }

    char error[160];
    snprintf(error, sizeof(error), "SD format failed: %s", esp_err_to_name(result));
    set_status_error(error);
    ESP_LOGE(TAG, "%s", error);
    return false;
#endif
}

std::vector<std::pair<std::string, size_t>> list_sd_files()
{
#ifdef CONFIG_SCREENLESS
    return {};
#else
    bsp_spi_lock();
    std::vector<std::pair<std::string, size_t>> files;
    DIR *dir = opendir("/sdcard");
    if (dir == nullptr) {
        bsp_spi_unlock();
        return files;
    }
    struct dirent *entry = nullptr;
    while ((entry = readdir(dir)) != nullptr) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        std::string filepath = "/sdcard/";
        filepath += entry->d_name;
        struct stat st = {};
        if (stat(filepath.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
            files.push_back({entry->d_name, static_cast<size_t>(st.st_size)});
        }
    }
    closedir(dir);
    bsp_spi_unlock();
    return files;
#endif
}

bool delete_sd_file(const std::string &filename)
{
#ifdef CONFIG_SCREENLESS
    (void)filename;
    return false;
#else
    bsp_spi_lock();
    const std::string filepath = "/sdcard/" + filename;
    const bool result = unlink(filepath.c_str()) == 0;
    if (result) ESP_LOGI(TAG, "Deleted file %s", filepath.c_str());
    else ESP_LOGE(TAG, "Failed to delete %s: %s", filepath.c_str(), strerror(errno));
    bsp_spi_unlock();
    return result;
#endif
}

void request_sd_probe()
{
#ifdef CONFIG_SCREENLESS
    return;
#else
    std::lock_guard<std::mutex> lock(s_mutex);
    s_sd_probe_requested = true;
    s_sd_next_mount_ts = 0;
    s_sd_mount_backoff_sec = SD_MOUNT_BACKOFF_INITIAL_SEC;
    s_sd_status.mount_backoff_active = false;
    s_sd_status.next_mount_attempt_timestamp = 0;
#endif
}

} // namespace WeatherLogger

