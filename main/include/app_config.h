#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#define APP_VERSION_BASE "0.8.39"


#if __has_include("build_version.h")
#include "build_version.h"
#else
#define APP_VERSION APP_VERSION_BASE
#endif

#include "const.h"
#include "settings.h"

//
// Data submission
//
/** Interval at which acquired data is submitted to backend (uS) */
#ifdef WXM_ENV_PROD
const uint64_t SUBMIT_TELEMETRY_INTERVAL_US = 20 * 1e6;
#else
const uint64_t SUBMIT_TELEMETRY_INTERVAL_US = 20 * 1e6;
#endif

/**
 * Interval at which client attributes are submitted to backend (sec)
 * Must be multiple of telemetry interval
 */
const uint64_t SUBMIT_CLIENT_ATTR_INTERVAL_SEC = 300;

/** Interval at which OTA firmware attributes are queried from backend (sec) */
const uint64_t SUBMIT_OTA_CHECK_INTERVAL_SEC = 300;

/** Interval at which remote settings attributes are queried from backend (sec) */
const uint64_t SUBMIT_REMOTE_SETTINGS_INTERVAL_SEC = 300;

/** Interval at which received WS packets will be aggregated into single packets
 * Packets are received at ~Xsec intervals, so it is recommended that this is a multiple of X */
const uint32_t WS_DATA_AGGREGATE_INTERVAL_SEC = 60;

/** Submit weather station data to IPFS? */
const bool IPFS_SUBMIT_ENABLED = false;

/** LCD max backlight level. All backlight levels (eg. 50% fade) will be calculated against this */
const u8 LCD_BACKLIGHT_LEVEL = 90;

/** LCD backlight fade timeout */
const int LCD_BACKLIGHT_FADE_TIME_SEC = 60;

/** DNS fallback address. If empty default dns secondary server from DHCP is used */
const char DNS_FALLBACK_ADDRESS[] = "8.8.8.8";

//
// Misc
//
#define NTP_SERVER "pool.ntp.org"

//
// Default settings
//
/** Default settings to use when keys not yet set */
const Settings::Data DEFAULT_SETTINGS
{
	.freq_region = WsFreqRegion::WS_FREQ_REGION_EU868,
	.temp_unit = TempUnit::TEMP_UNIT_C,
	.tz_offset = 0,
	.wind_speed_unit = WindSpeedUnit::WIND_SPEED_UNIT_M_S,
	.precip_unit = PrecipUnit::PRECIP_UNIT_MM,
	.pressure_unit = PressureUnit::PRESSURE_UNIT_HPA,
	.idle_backlight_level = BacklightLevel::BACKLIGHT_LEVEL_50,
	.scr_rot = ScreenRotation::SCR_ROT_DEGREES_0,
    .mqtt_broker_id = MqttBroker::PROD,
    .mqtt_disabled = false,
    .cloud_pairing_enabled = true,
	.pair_mode = PairMode::PAIR_MODE_CLOUD,
	.pair_source = PairSource::PAIR_SOURCE_CLOUD,
	.local_pair_sync_pending = false,
	.debug_verbosity = Log::Level::NONE,
	.last_tab = 1,
	.ws_id = -1, // Negative value means not paired
	.ws_type = WsType::WS_TYPE_1001,
	.cloud_ws_id = -1,
	.cloud_ws_type = WsType::WS_TYPE_NONE,
	.mesh_env_telemetry_enabled = false,
	.mesh_env_telemetry_interval_sec = 3600,
	.mesh_env_telemetry_hop_limit = 3,
	.mesh_tx_power_high = false,
	.mesh_modem_preset = 0,
	.mesh_freq_hz = 0,
	.mesh_bw_khz = 0,
	.mesh_sf = 0,
	.mesh_cr = 0,
	.mesh_tx_power_dbm = 0,
	.mesh_custom_name_enabled = false,
	.mesh_custom_name = {0},
	.mesh_custom_short_name = {0},
	.mesh_multi_station_forwarding_enabled = false,
	.meshcore_gw_telemetry_enabled = false,
	.meshcore_gw_telemetry_interval_sec = 3600,
	.meshcore_gw_tx_power_high = false,
	.meshcore_gw_profile_idx = 0,
	.meshcore_gw_freq_hz = 0,
	.meshcore_gw_bw_khz = 0,
	.meshcore_gw_sf = 0,
	.meshcore_gw_cr = 0,
	.meshcore_gw_tx_power_dbm = 0,
	.gateway_radio_mode = Settings::GatewayRadioMode::WXM_GATEWAY,
	.weather_policy = AdaptivePublish::default_policy(),
	.last_reboot_id = 0,
	.last_scan_id = 0,
	.station_ownership_confirmed = false,
	.station_ownership_id = 0,
	.station_ownership_timestamp = 0
};

//
// Debug
//
/* Print JSONs submitted to TB */
const bool PRINT_TB_JSON_ON_SUBMIT = true;

#endif
