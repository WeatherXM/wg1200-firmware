#pragma once

#include <vector>
#include <string>
#include <utility>
#include <ctime>
#include <cmath>
#include <cstdint>
#include "ws/include/ws_fields.h"

namespace WsGps { struct GpsData; }

enum WeatherQualityFlag : uint32_t {
    WEATHER_QUALITY_OK                      = 0,
    WEATHER_QUALITY_MISSING_FIELDS          = 1U << 0,
    WEATHER_QUALITY_LOW_PACKET_COUNT        = 1U << 1,
    WEATHER_QUALITY_PACKET_COUNTER_GAP      = 1U << 2,
    WEATHER_QUALITY_PACKET_COUNTER_RESET    = 1U << 3,
    WEATHER_QUALITY_MIXED_STATION_IDS       = 1U << 4,
    WEATHER_QUALITY_BAD_VALUES              = 1U << 5,
    WEATHER_QUALITY_PRECIP_RATE_FALLBACK    = 1U << 6,
    WEATHER_QUALITY_UNPAIRED_SOURCE         = 1U << 7,
    WEATHER_QUALITY_TX_INTERVAL_CHANGED     = 1U << 8,
    WEATHER_QUALITY_PRECIP_COUNTER_RESET    = 1U << 9,
    WEATHER_QUALITY_PRECIP_BASELINE_MISSING = 1U << 10,
    WEATHER_QUALITY_STATION_ID_MISSING      = 1U << 11,
};

struct WeatherAggregate {
    time_t timestamp = 0;
    float temp_avg = NAN;
    float hum_avg = NAN;
    float wind_speed_avg = NAN;
    float wind_dir_prevailing = NAN;
    float wind_gust_max = NAN;
    float precip_avg = NAN;
    float precip_accum = NAN;
    float pressure_avg = NAN;
    float solar_rad_avg = NAN;

    uint32_t station_id = 0;
    uint32_t paired_station_id = 0;
    uint32_t first_packet_counter = 0;
    uint32_t last_packet_counter = 0;
    uint32_t packet_counter_gaps = 0;
    uint32_t field_presence_mask = 0;
    uint32_t quality_flags = WEATHER_QUALITY_OK;
    uint32_t packet_count = 0;
    uint32_t expected_packet_count = 0;
    time_t coverage_start_timestamp = 0;
    time_t coverage_end_timestamp = 0;
    uint16_t intervals_observed = 0;
    uint16_t tx_interval_sec = 0;
    uint16_t battery_min_mv = 0;
    int16_t rssi_avg = 0;
    uint8_t station_fw_version = 0;
    int8_t timezone_offset_hours = 0;
    bool packet_counter_available = false;
    bool battery_mv_available = false;
    bool battery_low_available = false;
    bool battery_low = false;
    bool rssi_available = false;
    bool station_fw_available = false;
    bool source_is_paired = false;
    char packer_name[7] = {};
    bool valid = false;
};

struct SdLoggerStatus {
    bool writer_running = false;
    size_t pending_records = 0;
    uint32_t weather_rows_written = 0;
    uint32_t metadata_rows_written = 0;
    uint32_t weather_rows_failed = 0;
    uint32_t metadata_rows_failed = 0;
    uint32_t retry_count = 0;
    time_t last_success_timestamp = 0;
    time_t last_error_timestamp = 0;
    bool mount_backoff_active = false;
    time_t next_mount_attempt_timestamp = 0;
    std::string last_error;
};

namespace WeatherLogger {
    void init();
    void push_raw(const WsFields::FieldPack* pack);
    void log_gps_fix(uint32_t station_id, const char *station_type,
                     WsGps::GpsData *gps_data);
    
    // Thread-safe accessors
    std::vector<WsFields::FieldPack> get_raw_buffer();
    std::vector<WeatherAggregate> get_aggregates();
    
    // SD Card Management Helpers
    bool is_sd_mounted();
    bool format_sd();
    std::vector<std::pair<std::string, size_t>> list_sd_files();
    bool delete_sd_file(const std::string& filename);
    SdLoggerStatus get_sd_status();
    void request_sd_probe();
}

