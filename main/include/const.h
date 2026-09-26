#ifndef CONST_H
#define CONST_H

#include "common.h"
#include "struct.h"
#include "driver/i2c.h"
#include "driver/uart.h"

//
// TB
//
/** Telemetry submit topic */
const char TB_TELEMETRY_TOPIC[] = "v1/devices/me/telemetry";

/** Attribute request topic. With constant request_id appended. */
const char TB_ATTRIBUTE_REQ_TOPIC[] = "v1/devices/me/attributes/request/1";

/** Response topic for attribute requests */
const char TB_ATTRIBUTE_RESP_TOPIC[] = "v1/devices/me/attributes/response/+";

/** Topic to wait on for attribute responses */
const char TB_ATTRIBUTE_WAIT_TOPIC[] = "v1/devices/me/attributes/response";

/** Attribute publish topic. */
const char TB_ATTRIBUTE_PUBLISH_TOPIC[] = "v1/devices/me/attributes";

/** Claim publish topic. */
const char TB_CLAIM_PUBLISH_TOPIC[] = "v1/devices/me/claim";

/** Size of interna; recv/send buffer size */
const int TB_MQTT_BUFFER_SIZE = 20480;

/** QoS used in all operations by default */
const int TB_MQTT_DEFAULT_QOS = 1;

/** QoS used for telemetry */
const int TB_MQTT_TELEMETRY_QOS = 1;

/** QoS used for OTA chunks */
const int TB_MQTT_OTA_QOS = 1;

/** Default MQTT topic length for buffers */
const int TB_MQTT_DEFAULT_TOPIC_LEN = 120;

/** Time to wait on response on topic before failing */
const int TB_MQTT_DEFAULT_WAIT_RESP_TIMEOUT_MS = 35000;

/** Time to try reconnecting to TB */
const int TB_MQTT_RECONNECT_INTERVAL_MS = 10000;

/** Time to try reconnecting to TB */
const int TB_MQTT_CONNECTION_TIMEOUT_MS = 25000;

/** TB ard. JSON doc size */
const int TB_JSON_DOC_SIZE = 2048;

/** JSON doc serialization output buffer size */
const int TB_JSON_BUFF_SIZE = 2048;

/** Claim duration millis*/
const int TB_CLAIM_ENABLE_DURATION_MS = 20000;

/** Claim refresh millis*/
const int TB_CLAIM_ENABLE_REFRESH_MS = TB_CLAIM_ENABLE_DURATION_MS - 5000;

/** Claim retry millis */
const int TB_CLAIM_ENABLE_RETRY_MS = 5000;

/** Claim timeout */
const int TB_CLAIM_ENABLE_TIMEOUT_MS = 5 * 60 * 1000;

/** Claim publish timeout */
const int TB_CLAIM_ENABLE_PUBLISH_TIMEOUT_MS = 4000;

/** Claim enable max consecutive fails */
const int TB_CLAIM_ENABLE_MAX_FAILS = 5;

/** Packet signing interval. Sign one packet every X packets*/
const int TB_PACKET_SIGNING_INTERVAL = 1;

//
// TB keys: General
//
const char TB_KEY_TS[] = "ts";
const char TB_KEY_VALUES[] = "values";
const char TB_KEY_SHARED_ATTRIBUTES[] = "shared";
const char TB_KEY_PACKET_BASE64_POSTFIX[] = "packet_b64";
const char TB_KEY_PACKET_SIGNATURE_POSTFIX[] = "packet_sig";
const char TB_KEY_FIRMWARE_VERSION[] = "fw_v";
const char TB_KEY_WEATHER_STATION_MODEL[] = "ws_model";

//
// TB keys: Claim
//
const char TB_KEY_CLAIM_SECRET_KEY[] = "secretKey";
const char TB_KEY_CLAIM_DURATION_MS[] = "durationMs";

//
// TB Keys: WS aggregate data
//
const char TB_KEY_WS_TEMP[] = "temperature";
const char TB_KEY_WS_HUM[] = "humidity";
const char TB_KEY_WS_PRECIP_CML[] = "precipitation_accumulated";
const char TB_KEY_WS_PRECIP_RATE[] = "precipitation_rate";
const char TB_KEY_WS_W_SPEED[] = "wind_speed";
const char TB_KEY_WS_W_GUST[] = "wind_gust";
const char TB_KEY_WS_W_DIR[] = "wind_direction";
const char TB_KEY_WS_UV[] = "fo_uv";
const char TB_KEY_WS_UV_INDEX[] = "uv_index";
const char TB_KEY_WS_LIGHT[] = "illuminance";
const char TB_KEY_WS_SOL_RAD[] = "solar_irradiance";
const char TB_KEY_WS_LOW_BAT[] = "low_bat";

const char TB_KEY_IPFS_HASH[] = "ipfs_hash";
const char TB_KEY_WS_PRESSURE[] = "pressure";

const char TB_KEY_WS_FW_V[] = "ws_fw_v";
const char TB_KEY_WS_PKT_COUNT[] = "ws_pkt_count";
const char TB_KEY_WS_TX_POWER[] = "ws_tx_power";
const char TB_KEY_WS_RSSI[] = "ws_rssi";
const char TB_KEY_WS_CUR_INTERVAL[] = "ws_cur_interval";
const char TB_KEY_WS_BAT_MV[] = "ws_bat_mv";
const char TB_KEY_WS_SAMPLES[] = "samples";

const char TB_KEY_WEATHER_DATA_SIGNATURE_PREFIX[] = "ws_";


//
// TB keys: OTA
//
const char TB_KEY_OTA_FW_VERSION[] = "fw_version";
const char TB_KEY_OTA_FW_SIZE[] = "fw_size";
const char TB_KEY_OTA_FW_CHECKSUM[] = "fw_checksum";
const char TB_KEY_OTA_FW_CHECKSUM_ALGO[] = "fw_checksum_algorithm";
//
// TB Keys: GPS
//
const char TB_KEY_GPS_LAT[] = "gps_lat";
const char TB_KEY_GPS_LON[] = "gps_lon";
const char TB_KEY_GPS_ALT[] = "gps_alt";
const char TB_KEY_GPS_SATS[] = "gps_sats";
const char TB_KEY_GPS_SPEED[] = "gps_speed";
const char TB_KEY_GPS_DIR[] = "gps_dir";
const char TB_KEY_GPS_SIGNATURE_PREFIX[] = "gps_";
const char TB_KEY_GPS_VERIFIED[] = "gps_verified";
const char TB_KEY_GPS_TS[] = "gps_ts";

//
// SPS Keys: Signed Packet Structure Keys
//
const char SPS_KEY_FW_VER[] = "fw_v";
const char SPS_KEY_WS_MODEL[] = "ws_m";
const char SPS_KEY_TS[] = "ts";

//
// SPS Keys: WS Signed Packet Structure Keys
//
const char SPS_KEY_WS_TEMP[] = "t";
const char SPS_KEY_WS_HUM[] = "h";
const char SPS_KEY_WS_PRECIP_CML[] = "pr_a";
const char SPS_KEY_WS_PRECIP_RATE[] = "pr_r";
const char SPS_KEY_WS_W_SPEED[] = "w_sp";
const char SPS_KEY_WS_W_GUST[] = "w_gu";
const char SPS_KEY_WS_W_DIR[] = "w_dr";
const char SPS_KEY_WS_UV_INDEX[] = "uvi";
const char SPS_KEY_WS_LIGHT[] = "ilm";
const char SPS_KEY_WS_SOL_RAD[] = "sol";
const char SPS_KEY_WS_CELL_ID[] = "clid";
const char SPS_KEY_WS_PRESSURE[] = "press";
const char SPS_KEY_WS_SAMPLES[] = "samp";

//
// SPS Keys: GPS Signed Packet Structure Keys
//
const char SPS_KEY_GPS_LAT[] = "gps_lat";
const char SPS_KEY_GPS_LON[] = "gps_lon";
const char SPS_KEY_GPS_ALT[] = "gps_alt";
const char SPS_KEY_GPS_SATS[] = "gps_sats";
const char SPS_KEY_GPS_VERIFIED[] = "gps_verif";


// Define the bit position and block of the M8_SEC_ENABLE eFuse field
#define EFUSE_BLOCK_SIZE 192
#define M8_SEC_ENABLE_BIT 183
#define M8_SEC_ENABLE_BLOCK EFUSE_BLK3

//
// AWS root CA certificate resource pointers
//
extern const u8 AWS_ROOT_CA_CERT_PEM_START[] asm("_binary_aws_root_ca_pem_start");
extern const u8 AWS_ROOT_CA_CERT_PEM_END[] asm("_binary_aws_root_ca_pem_end");

//
// Serial number
//
/** serial number buff size */
#define DEVICE_SERIAL_NUM_BYTE_LENGTH 14
/** Serial number display string size. Display string includes colon chars. */
#define DEVICE_SERIAL_NUM_STRING_BUFF_SIZE (2*DEVICE_SERIAL_NUM_BYTE_LENGTH + DEVICE_SERIAL_NUM_BYTE_LENGTH)
/** Size of an ouput buffer for a signing operation */
#define DEVICE_SIG_BUFF_SIZE 256

//
// Tasks
//

// Packet handler task
const char PACKET_HANDLER_TASK_NAME[] = "packet_handler";
const int PACKET_HANDLER_TASK_STACK_SIZE = 8 * 1024;
const int PACKET_HANDLER_TASK_PRIORITY = 5;

// GPS Task
const char GPS_READ_TASK_NAME[] = "gps";
const int GPS_READ_TASK_STACK_SIZE = 4096;
const int GPS_READ_TASK_PRIORITY = 1;

// Data submit task
const char SUBMIT_TASK_NAME[] = "submit";
const int SUBMIT_TASK_STACK_SIZE = 4*4096;
const int SUBMIT_TASK_PRIORITY = 2;
const uint32_t SUBMIT_TASK_SWD_TIMEOUT_MS = 10 * 60 * 1000; //ten minutes

// TB MQTT task (created and managed by esp_mqtt_client)
const int TB_MQTT_TASK_SIZE = 8192;
const int TB_MQTT_TASK_PRIORITY = 4;

// TB keepalive task
const char TB_KEEPALIVE_TASK_NAME[] = "tb_keepalive";
const int TB_KEEPALIVE_TASK_SIZE = 4096;
const int TB_KEEPALIVE_TASK_PRIORITY = 1;

// WiFi keepalive
const char WIFI_MANAGER_WXM_TASK_NAME[] = "wifi_manager";
const int WIFI_MANAGER_WXM_TASK_STACK_SIZE = 4096;
const int WIFI_MANAGER_WXM_TASK_PRIORITY = 1;

// Diagnostics monitoring task
const char DIAG_MONITOR_TASK_NAME[] = "diag";
const int DIAG_MONITOR_TASK_STACK_SIZE = 4096;
const int DIAG_MONITOR_TASK_PRIORITY = 1;

// Backlight monitoring task
const char BACKLIGHT_TASK_NAME[] = "backlight";
const int BACKLIGHT_TASK_STACK_SIZE = 2048;
const int BACKLIGHT_TASK_PRIORITY = 2;

// UI update task
const char UI_UPDATE_TASK_NAME[] = "ui_upd";
const int UI_UPDATE_TASK_STACK_SIZE = 16*1024;
const int UI_UPDATE_TASK_PRIORITY = 2;

// Pressure update task
const char PRESSURE_UPDATE_TASK_NAME[] = "pressure_upd";
const int PRESSURE_UPDATE_TASK_STACK_SIZE = 2*1024;
const int PRESSURE_UPDATE_TASK_PRIORITY = 2;

// NTP sync
const char NTP_SYNC_TASK_NAME[] = "ntp_sync";
const int NTP_SYNC_TASK_STACK_SIZE = 4096;
const int NTP_SYNC_TASK_PRIORITY = 1;

// Settings commit
const char SETTINGS_COMMIT_TASK_NAME[] = "settings_commit";
const int SETTINGS_COMMIT_TASK_STACK_SIZE = 4096;
const int SETTINGS_COMMIT_TASK_PRIORITY = 1;

// Public API task
const char HTTPS_CLIENT_TASK_NAME[] = "https_client";
const int HTTPS_CLIENT_TASK_TASK_STACK_SIZE = 2 * 4096;
const int HTTPS_CLIENT_TASK_TASK_PRIORITY = 2;

//
// Public API
//
const char PUB_API_SEARCH_URL[] = "https://api.weatherxm.com/api/v1/network/search?query=%s&exact=true&exclude=places";
const char PUB_API_REWARDS_URL[] = "https://api.weatherxm.com/api/v1/devices/%s/rewards";
const char PUB_API_DEV_INFO_URL[] = "https://api.weatherxm.com/api/v1/cells/%s/devices/%s";

const int PUB_API_STATUS_OK = 200;
const int PUB_API_JSON_DOC_SIZE = 2 * 1024;
const int PUB_API_ADDRESS_CHAR_SIZE = 128;
const int PUB_API_DEV_ID_CHAR_SIZE = 64;
const int PUB_API_CELL_INDEX_CHAR_SIZE = 32;
const char PUB_API_POL_REASON_NO_LOCATION_DATA_STR[] = "NO_LOCATION_DATA";
const char PUB_API_POL_REASON_NOT_VERIF_STR[] = "LOCATION_NOT_VERIFIED ";

//
// Https client
//
const int HTTPS_CLIENT_RESPONSE_SIZE = 2048;
const int HTTPS_CLIENT_URL_SIZE = 512;

//
// GPS
//


/** Number of decimal places needed for lat and lon when expressed in deg
 * 4 decimals offer ~10m accuracy */
const int GPS_DEG_DEC = 5;

//
// H3
//

/* Cell resolution for h3 map */
const int H3_CELL_RESOLUTION = 7;
const int H3_INDEX_INITIALIZATION_VAL = 0;


//
// Weather station
//

/** GPS request period interval */
const uint32_t WS_GPS_REQUEST_INTERVAL_SEC = 86400u; // a day

/** Low reception threshold */
const uint8_t WS_LOW_RECEPTION_THRESHOLD = 20;

/** GPS expiration  */
const uint32_t WS_GPS_EXPIRATION_SEC = WS_GPS_REQUEST_INTERVAL_SEC + 3600u; // 25h

/** GPS request maximum continuous retries before cooldown  */
const uint8_t WS_GPS_REQ_MAX_CONT_RETRIES = 3;

/** GPS request cooldown duration*/
const uint32_t WS_GPS_REQ_COOLDOWN_SEC = 8 * 3600; // request gps again in 8 hours

/** If no packet received for this amount, WS is considered lost/disconnected/out of range */
const int WS_LOST_TIMEOUT_MS = 300000; //5 minutes

/** WS1001 scan dwell windows: fast initial pass, then deep pass */
const uint32_t WS1001_SCAN_FAST_DWELL_MS = 60000;  // 60s (initial scan)
const uint32_t WS1001_SCAN_DEEP_DWELL_MS = 300000; // 300s / 5 minutes (deep scan)

const u32 WS_FREQ_HZ_86835 = (u32)86835e4;
const u32 WS_FREQ_HZ_868 = (u32)868e6;
const u32 WS_FREQ_HZ_915 = (u32)915e6;
const u32 WS_FREQ_HZ_923 = (u32)923e6;
const u32 WS_FREQ_HZ_AU915 = (u32)915e6;
const u32 WS_FREQ_HZ_IN865 = (u32)865e6;

/** Sniffer freq titles as displayed on UI */
#define WS_FREQ_TITLE_EU868 "EU 868 MHz"
#define WS_FREQ_SYMBOL_EU868 "868"
#define WS_FREQ_TITLE_US915 "US 915 MHz"
#define WS_FREQ_SYMBOL_US915 "915"
#define WS_FREQ_TITLE_AS923 "AS 923 MHz"
#define WS_FREQ_SYMBOL_AS923 "923"
#define WS_FREQ_TITLE_AU915 "AU 915 MHz"
#define WS_FREQ_SYMBOL_AU915 "916"
#define WS_FREQ_TITLE_IN865 "IN 865 MHz"
#define WS_FREQ_SYMBOL_IN865 "865"

/** Available sniffer frequencies */
const WsFreqDesc SNIFFER_FREQUENCIES[] = {
	{ WS_FREQ_SYMBOL_EU868, WS_FREQ_TITLE_EU868, WS_FREQ_REGION_EU868, WS_FREQ_HZ_868 },
	{ WS_FREQ_SYMBOL_US915, WS_FREQ_TITLE_US915, WS_FREQ_REGION_US915, WS_FREQ_HZ_915 },
	{ WS_FREQ_SYMBOL_AS923, WS_FREQ_TITLE_AS923, WS_FREQ_REGION_AS923, WS_FREQ_HZ_923 },
	{ WS_FREQ_SYMBOL_AU915, WS_FREQ_TITLE_AU915, WS_FREQ_REGION_AU915, WS_FREQ_HZ_AU915 },
	{ WS_FREQ_SYMBOL_IN865, WS_FREQ_TITLE_IN865, WS_FREQ_REGION_IN865, WS_FREQ_HZ_IN865 },
};

/** Sniffer freq titles formatted for dropdown */
#define FREQ_DROPDOWN_ITEMS \
	WS_FREQ_TITLE_EU868 "\n" \
	WS_FREQ_TITLE_US915 "\n" \
	WS_FREQ_TITLE_AS923 "\n" \
	WS_FREQ_TITLE_AU915 "\n" \
	WS_FREQ_TITLE_IN865


//
// WiFi
//
/* Times to attempt reconnect, before aborting for WIFI_CONNECT_RETRY_INT_LONG_MS time */
const int WIFI_RECONNECT_ATTEMPTS = 5;
/* Time to wait before trying to reconnect */
const int WIFI_CONNECT_RETRY_INT_SHORT_MS = 3000;
/* Time to wait after max reconnect attempts reached before starting to try again */
const int WIFI_CONNECT_RETRY_INT_LONG_MS = 25000;
/** Time to wait in order to get IP after STA is connected **/
const u32 WIFI_GET_IP_WAIT_MS = 15000;
/** Time to wait before checking internet connectivity again **/
const u32 WIFI_CHECK_INTERNET_EVERY_MS = 120000;
/** Consecutive failed internet checks before actively refreshing the connection. */
const uint8_t WIFI_INTERNET_FAILURES_BEFORE_RECOVERY = 3;
/** Increasing delay between connection refreshes during a sustained outage. */
const u32 WIFI_INTERNET_RECOVERY_BACKOFF_MS[] = {
	0,
	2 * 60 * 1000,
	5 * 60 * 1000,
	10 * 60 * 1000,
	30 * 60 * 1000
};
/** Start fully restarting the Wi-Fi driver at this recovery attempt. */
const uint8_t WIFI_INTERNET_DRIVER_RESTART_AFTER_ATTEMPT = 2;
/** Sustained outage duration before one final, guarded device restart. */
const u32 WIFI_INTERNET_REBOOT_AFTER_MS = 60 * 60 * 1000;
/** Require several lower-impact recoveries before restarting the device. */
const uint8_t WIFI_INTERNET_MIN_RECOVERIES_BEFORE_REBOOT = 4;
/** Maximum time to wait for a requested Wi-Fi driver stop. */
const u32 WIFI_DRIVER_STOP_WAIT_MS = 5000;

//
// Misc
//
/** Timestamp to check against for validity */
const u32 TSTAMP_VALID_CHECK = 1630281600;

/** Read pressure sensor interval */
const u32 PRESSURE_UPDATE_INT_SEC = 60;

//
// UI
//

/** Time after which WS data that is drawn on LCD is considered old,
 * and is removed from display */
const u32 UI_WS_DATA_EXPIRE_SEC = WS_LOST_TIMEOUT_MS/1000;

/** Time to wait until saving the active on screen tab index to NVS */
const u32 UI_LAST_TAB_SAVE_SEC = 5;

//
// WS Data buffer
//
/** Decoded packets to hold in receive buffer. Should at least fit packets
 * for 10mins of sniffing (16s/packet). */
const int WS_DATA_RECEIVE_BUFF_ITEMS = 40;

/** Aggregated packets buffer size */
const int WS_AGGREGATE_BUFF_SIZE_ITEMS = 100;

/** Period over which precipitation rate live decay will be zero (45 min) */
const int WS_DATA_PRECIP_RATE_DECAY_TIMEOUT_SEC = 2700;

/** Weather station public key length */
const size_t WS_PUB_KEY_LEN = 64;

/** Weather station serial number length */
const size_t WS_SN_LEN = 10;


//
// OTA
//
/** Chunk size to request from TB. Must not be larger than ESP MQTT buffer */
const int OTA_CHUNK_SIZE = 4096;

/** OTA attribute request body. Specifies which attributes will be returned by server.
 * No spaces between key names or thingsboard will cry */
const char OTA_TB_ATTRIBUTE_REQ_JSON[] = "{\"sharedKeys\":\"fw_version,fw_size,fw_checksum,fw_checksum_algorithm\"}";

/** OTA fw data request topic */
const char OTA_TB_FW_REQ_TOPIC_FMT[] = "v2/fw/request/%d/chunk/%d";

/** OTA fw data receive topic */
const char OTA_TB_FW_RECV_TOPIC[] = "v2/fw/response/#";

/** OTA fw specific chunk receive topic */
const char OTA_TB_FW_RECV_CHUNK_TOPIC[] = "v2/fw/response/%d/chunk/%d";

/** OTA fw specific chunk receive topic */
const uint8_t OTA_TB_FW_CHUNK_MAX_RETRIES = 3;

/** OTA fw data wait topic */
const char OTA_TB_FW_WAIT_TOPIC[] = "v2/fw/response";

/** Sent during OTA process as telemetry to update TB on current status */
const char OTA_TB_UPDATE_STATE_JSON_FMT[] = "{\"current_fw_title\": \"%s\", \"current_fw_version\": \"%s\", \"target_fw_version\": \"%s\", \"fw_state\": \"%s\"}";

/** Sent to TB on error as telemetry */
const char OTA_TB_FAILED_ERROR_JSON_FMT[] = "{\"target_fw_version\": \"%s\", \"fw_state\": \"FAILED\", \"fw_error\": \"%s\"}";

/** OTA failure telemetry with integrity-check context, so a failed update can be
 * diagnosed from ThingsBoard without a serial console. */
const char OTA_TB_FAILED_DETAILS_JSON_FMT[] = "{\"target_fw_version\": \"%s\", \"fw_state\": \"FAILED\", \"fw_error\": \"%s\", \"ota_chunk\": %d, \"ota_bytes_written\": %d, \"ota_bytes_expected\": %d, \"ota_checksum_expected\": \"%s\", \"ota_checksum_flash\": \"%s\", \"ota_verdict\": \"%s\"}";

/** Buffer used to read the OTA partition back from flash for post-download
 * verification. Allocated in internal RAM because the ESP32-S3 hardware SHA
 * crypto DMA cannot coherently read external PSRAM. */
const int OTA_VERIFY_READ_BUF_SIZE = 4096;

//
// Remote settings
//
/** Shared attr key name: Debug mode enabled */
const char RS_KEY_DEBUG_VERBOSITY[] = "rs_dbg_v";

/** Shared attr key name: Station frequency */
const char RS_KEY_WS_FREQ_NAME[] = "rs_ws_freq_name";

const char RS_VALUE_WS_FREQ_EU868[] = "EU868";
const char RS_VALUE_WS_FREQ_US915[] = "US915";
const char RS_VALUE_WS_FREQ_AS923[] = "AS923";
const char RS_VALUE_WS_FREQ_AU915[] = "AU915";
const char RS_VALUE_WS_FREQ_IN865[] = "IN865";

/** Shared attr key name: Station model */
const char RS_KEY_WS_MODEL[] = "rs_ws_model";

const char RS_VALUE_WS_MODEL_WS1001[] = "WS1001";
const char RS_VALUE_WS_MODEL_WS1300[] = "WS1300";

/** Shared attr key name: Station public key */
const char RS_KEY_WS_PUBKEY[] = "rs_ws_pubkey";

/** Shared attr key name: Station serial number */
const char RS_KEY_WS_SN[] = "rs_ws_sn";

/** Shared attr key name: Station ID (hex or numeric string) */
const char RS_KEY_WS_ID[] = "rs_ws_id";

/** Shared attr key name: Force remote scan (true/false) */
const char RS_KEY_SCAN[] = "rs_scan";

/** Shared attr key name: Force remote reboot (timestamp, nonce counter, or bool) */
const char RS_KEY_REBOOT[] = "rs_reboot";

/** Shared attr key name: Last rewards mechanism run TS */
const char RS_KEY_RM_RUN_TS[] = "rm_run_ts";

/** Shared attr key name: Device name */
const char RS_KEY_DEV_NAME[] = "name";

/** Remote settings shared attributes request body. Only requested keys are returned. */
const char RS_TB_ATTRIBUTE_REQ_JSON[] = "{\"clientKeys\":\"ws_fo_id,ws_id\",\"sharedKeys\":\"rs_dbg_v,rs_ws_freq_name,rs_ws_model,rs_ws_pubkey,rs_ws_sn,rs_ws_id,rs_scan,rs_reboot,name,rm_run_ts\"}";

/** Remote settings attributes request for unconfigured / wiped NV fallback: queries client ws_fo_id as well */
const char RS_TB_ATTRIBUTE_REQ_FALLBACK_JSON[] = "{\"clientKeys\":\"ws_fo_id,ws_id\",\"sharedKeys\":\"rs_dbg_v,rs_ws_freq_name,rs_ws_model,rs_ws_pubkey,rs_ws_sn,rs_ws_id,rs_scan,rs_reboot,name,rm_run_ts\"}";

//
// Logs
//
/** Max number of log entries in ring buff */
const int LOG_BUFFER_ITEM_COUNT = 100;

/** Char length of text logs (inc \0) */
const int LOG_MSG_LEN = 250;

//
// FW restore mode
// Boot mode from where user can rollback fw to factory version (switch to other partition)
// Mode is entered when user taps logo in splash screen X times

/** Time to show logo on boot screen */
const int BOOT_LOGO_DISPLAY_TIME_MS = 3000;

/** Number of times user needs to tap logo to be prompted for fw switch */
const int FW_SWITCH_LOGO_CLICK_THRESHOLD = 3;

/** Hard limit for max time allowed displaying the logo (failsafe) */
const int FW_SWITCH_FAILSAFE_TIMEOUT_MS = 20000;

//
// Settings
//
const char NVS_SETTINGS_NAMESPACE[] = "settings";
const char NVS_SETTINGS_KEY_FREQ_ID[] = "freq_name";
const char NVS_SETTINGS_KEY_TEMP_UNIT[] = "temp_unit";
const char NVS_SETTINGS_KEY_TZ_OFFSET[] = "tz_offset";
const char NVS_SETTINGS_KEY_WIND_SPEED_UNIT[] = "wind_speed_unit";
const char NVS_SETTINGS_KEY_RAIN_UNIT[] = "rain_unit";
const char NVS_SETTINGS_KEY_PRESSURE_UNIT[] = "pressure_unit";
const char NVS_SETTINGS_KEY_IDLE_BACKLIGHT_LEVEL[] = "bkl_fade_lvl";
const char NVS_SETTINGS_KEY_SCREEN_ROTATION[] = "scr_rot";
const char NVS_SETTINGS_KEY_MQTT_BROKER[] = "dbg_mqtt_broker";
const char NVS_SETTINGS_KEY_MQTT_DISABLED[] = "mqtt_disabled";
const char NVS_SETTINGS_KEY_CLOUD_PAIRING_ENABLED[] = "cloud_pair_en";
const char NVS_SETTINGS_KEY_PAIR_MODE[] = "pair_mode";
const char NVS_SETTINGS_KEY_PAIR_SOURCE[] = "pair_source";
const char NVS_SETTINGS_KEY_LOCAL_PAIR_SYNC_PENDING[] = "lp_sync_pend";
const char NVS_SETTINGS_DEBUG_VERBOSITY[] = "debug_verbosity";
const char NVS_SETTINGS_LAST_TAB[] = "last_tab";
const char NVS_SETTINGS_WS_ID[] = "ws_id";
const char NVS_SETTINGS_WS_TYPE[] = "ws_type";
const char NVS_SETTINGS_WS_PUBKEY[] = "ws_pubkey";
const char NVS_SETTINGS_WS_SN[] = "ws_sn";
const char NVS_SETTINGS_KEY_CLOUD_WS_ID[] = "c_ws_id";
const char NVS_SETTINGS_KEY_CLOUD_WS_VALID[] = "c_ws_valid";
const char NVS_SETTINGS_KEY_CLOUD_WS_TYPE[] = "c_ws_type";
const char NVS_SETTINGS_KEY_CLOUD_WS_SN[] = "c_ws_sn";
const char NVS_SETTINGS_KEY_CLOUD_WS_PUBKEY[] = "c_ws_pubkey";
const char NVS_SETTINGS_KEY_MESH_ENV_EN[] = "mesh_env_en";
const char NVS_SETTINGS_KEY_MESH_ENV_INT[] = "mesh_env_int";
const char NVS_SETTINGS_KEY_MESH_ENV_HOP[] = "mesh_env_hop";
const char NVS_SETTINGS_KEY_MESH_TX_PWR_HIGH[] = "m_tx_pwr_hi";
const char NVS_SETTINGS_KEY_MESH_PRESET[] = "m_preset";
const char NVS_SETTINGS_KEY_MESH_FREQ[] = "m_freq";
const char NVS_SETTINGS_KEY_MESH_BW[] = "m_bw";
const char NVS_SETTINGS_KEY_MESH_SF[] = "m_sf";
const char NVS_SETTINGS_KEY_MESH_CR[] = "m_cr";
const char NVS_SETTINGS_KEY_MESH_TX_PWR[] = "m_tx_pwr";
const char NVS_SETTINGS_KEY_MESH_CUSTOM_NAME_EN[] = "m_cname_en";
const char NVS_SETTINGS_KEY_MESH_CUSTOM_NAME[] = "m_cname";
const char NVS_SETTINGS_KEY_MESH_CUSTOM_SHORT_NAME[] = "m_cshort";
const char NVS_SETTINGS_KEY_MESH_FWD_EN[] = "mesh_fwd_en";
const char NVS_SETTINGS_KEY_MESHCORE_GW_EN[] = "mc_gw_en";
const char NVS_SETTINGS_KEY_MESHCORE_GW_INT[] = "mc_gw_int";
const char NVS_SETTINGS_KEY_MESHCORE_GW_PWR[] = "mc_gw_pwr";
const char NVS_SETTINGS_KEY_MESHCORE_GW_PROF[] = "mc_gw_prof";
const char NVS_SETTINGS_KEY_MESHCORE_GW_FREQ[] = "mc_gw_freq";
const char NVS_SETTINGS_KEY_MESHCORE_GW_BW[] = "mc_gw_bw";
const char NVS_SETTINGS_KEY_MESHCORE_GW_SF[] = "mc_gw_sf";
const char NVS_SETTINGS_KEY_MESHCORE_GW_CR[] = "mc_gw_cr";
const char NVS_SETTINGS_KEY_MESHCORE_GW_TX_PWR[] = "mc_gw_tx_pwr";
const char NVS_SETTINGS_KEY_GW_RADIO_MODE[] = "gw_radio_mode";
const char NVS_SETTINGS_KEY_WX_POLICY[] = "wx_policy";
const char NVS_SETTINGS_KEY_REBOOT_ID[] = "reboot_id";
const char NVS_SETTINGS_KEY_SCAN_ID[] = "scan_id";
const char NVS_SETTINGS_KEY_OWNERSHIP_CONF[] = "ws_own_conf";
const char NVS_SETTINGS_KEY_OWNERSHIP_ID[] = "ws_own_id";
const char NVS_SETTINGS_KEY_OWNERSHIP_TS[] = "ws_own_ts";

//
// Backlight settings
//

// Weather station type titles
#define WS_TYPE_TITLE_WS1001	"WS1001"
#define WS_TYPE_TITLE_WS1300	"WS1300"

/** Weather station type titles formatted for dropdown */
#define WS_TYPE_DROPDOWN_ITEMS \
	WS_TYPE_TITLE_WS1001 "\n" \
	WS_TYPE_TITLE_WS1300

/** List of available weather station types */
const WsTypeDesc WEATHER_STATION_TYPES[] = {
	{ WS_TYPE_TITLE_WS1001, WS_TYPE_1001 },
	{ WS_TYPE_TITLE_WS1300, WS_TYPE_1300 },
};

// Backlight level titles
#define BACKLIGHT_LEVEL_TITLE_0  "1%"
#define BACKLIGHT_LEVEL_TITLE_25 "25%"
#define BACKLIGHT_LEVEL_TITLE_50 "50%"
#define BACKLIGHT_LEVEL_TITLE_75 "75%"
#define BACKLIGHT_LEVEL_TITLE_100 "100%"

/** List of available idle backlight levels */
const BacklightLevelDesc IDLE_BACKLIGHT_LEVELS[] = {
	{ BACKLIGHT_LEVEL_TITLE_0, BACKLIGHT_LEVEL_0 },
	{ BACKLIGHT_LEVEL_TITLE_25, BACKLIGHT_LEVEL_25 },
	{ BACKLIGHT_LEVEL_TITLE_50, BACKLIGHT_LEVEL_50 },
	{ BACKLIGHT_LEVEL_TITLE_75, BACKLIGHT_LEVEL_75 },
	{ BACKLIGHT_LEVEL_TITLE_100, BACKLIGHT_LEVEL_100 }
};

/** Idle backlight level titles formatted for dropdown */
#define IDLE_BACKLIGHT_LEVEL_DROPDOWN_ITEMS \
	BACKLIGHT_LEVEL_TITLE_0 "\n" \
	BACKLIGHT_LEVEL_TITLE_25 "\n" \
	BACKLIGHT_LEVEL_TITLE_50 "\n" \
	BACKLIGHT_LEVEL_TITLE_75 "\n" \
	BACKLIGHT_LEVEL_TITLE_100

//
// Rotation settings
//

#define SCREEN_ROTATION_TITLE_0  "Normal"
#define SCREEN_ROTATION_TITLE_180 "Flipped"

/** List of available screen rotations */
const ScreenRotationDesc SCREEN_ROTATION_OPTIONS[] = {
	{ SCREEN_ROTATION_TITLE_0, SCR_ROT_DEGREES_0 },
	{ SCREEN_ROTATION_TITLE_180, SCR_ROT_DEGREES_180 }
};

/** Screen rotation titles formatted for dropdown */
#define SCREEN_ROTATION_DROPDOWN_ITEMS \
	SCREEN_ROTATION_TITLE_0 "\n" \
	SCREEN_ROTATION_TITLE_180

//
// Units
//
/** Temperature unit titles/symbols as displayed on UI */
#define TEMP_UNIT_SYMBOL_C "C"
#define TEMP_UNIT_TITLE_C "Celsius (°C)"
#define TEMP_UNIT_SYMBOL_F "F"
#define TEMP_UNIT_TITLE_F "Fahrenheit (°F)"

/** List of available wind temperature units */
const TempUnitDesc TEMP_UNITS[] = {
	{TEMP_UNIT_SYMBOL_C, TEMP_UNIT_TITLE_C, TEMP_UNIT_C},
	{TEMP_UNIT_SYMBOL_F, TEMP_UNIT_TITLE_F, TEMP_UNIT_F}
};

/** Temp unit titles formatted for dropdown */
#define TEMP_UNITS_DROPDOWN_ITEMS \
	TEMP_UNIT_TITLE_C "\n" \
	TEMP_UNIT_TITLE_F

/** Wind speed unit titles/symbols as displayed on UI */
#define WIND_SPEED_UNIT_TITLE_M_S "Meters per second (m/s)"
#define WIND_SPEED_UNIT_SYMBOL_M_S "m/s"
#define WIND_SPEED_UNIT_TITLE_KM_H "Kilometers per hour (km/h)"
#define WIND_SPEED_UNIT_SYMBOL_KM_H "km/h"
#define WIND_SPEED_UNIT_TITLE_MI_H "Miles per hour (mi/h)"
#define WIND_SPEED_UNIT_SYMBOL_MI_H "mi/h"
#define WIND_SPEED_UNIT_TITLE_FT_S "Feet per second (ft/s)"
#define WIND_SPEED_UNIT_SYMBOL_FT_S "ft/s"
#define WIND_SPEED_UNIT_TITLE_BEAUFORT "Beaufort (bft)"
#define WIND_SPEED_UNIT_SYMBOL_BEAUFORT "bft"
#define WIND_SPEED_UNIT_TITLE_KNOTS "Knots (kn)"
#define WIND_SPEED_UNIT_SYMBOL_KNOTS "kn"

// const char WIND_DIRECTION_CARDINAL_NAMES[8][3] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};

const char WIND_DIRECTION_CARDINAL_NAMES[8][3] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};

/** List of available wind speed units */
const WindSpeedUnitDesc WIND_SPEED_UNITS[] = {
	{WIND_SPEED_UNIT_SYMBOL_M_S, WIND_SPEED_UNIT_TITLE_M_S, WIND_SPEED_UNIT_M_S},
	{WIND_SPEED_UNIT_SYMBOL_KM_H, WIND_SPEED_UNIT_TITLE_KM_H, WIND_SPEED_UNIT_KM_H},
	{WIND_SPEED_UNIT_SYMBOL_MI_H, WIND_SPEED_UNIT_TITLE_MI_H, WIND_SPEED_UNIT_MI_H},
	{WIND_SPEED_UNIT_SYMBOL_FT_S, WIND_SPEED_UNIT_TITLE_FT_S, WIND_SPEED_UNIT_FT_S},
	{WIND_SPEED_UNIT_SYMBOL_BEAUFORT, WIND_SPEED_UNIT_TITLE_BEAUFORT, WIND_SPEED_UNIT_BEAUFORT},
	{WIND_SPEED_UNIT_SYMBOL_KNOTS, WIND_SPEED_UNIT_TITLE_KNOTS, WIND_SPEED_UNIT_KNOTS},
};

/** Wind speed unit titles formatted for dropdown */
#define WIND_SPEED_UNITS_DROPDOWN_ITEMS \
	WIND_SPEED_UNIT_TITLE_M_S "\n" \
	WIND_SPEED_UNIT_TITLE_KM_H "\n" \
	WIND_SPEED_UNIT_TITLE_MI_H "\n" \
	WIND_SPEED_UNIT_TITLE_FT_S "\n" \
	WIND_SPEED_UNIT_TITLE_BEAUFORT "\n" \
	WIND_SPEED_UNIT_TITLE_KNOTS

/** Precipitation unit titles/symbols as displayed on UI */
#define PRECIP_UNIT_TITLE_MM "Millimeters (mm)"
#define PRECIP_UNIT_SYMBOL_MM "mm"
#define PRECIP_UNIT_TITLE_IN "Inches (in)"
#define PRECIP_UNIT_SYMBOL_IN "in"

/** List of available Precipitation units */
const PrecipUnitDesc PRECIP_UNITS[] = {
	{PRECIP_UNIT_SYMBOL_MM, PRECIP_UNIT_TITLE_MM, PRECIP_UNIT_MM},
	{PRECIP_UNIT_SYMBOL_IN, PRECIP_UNIT_TITLE_IN, PRECIP_UNIT_IN}
};

/** Precipitation unit titles formatted for dropdown */
#define PRECIP_UNITS_DROPDOWN_ITEMS \
	PRECIP_UNIT_TITLE_MM "\n" \
	PRECIP_UNIT_TITLE_IN

/** Pressure unit titles/symbols as displayed on UI */
#define PRESSURE_UNIT_TITLE_HPA "Hectopascal (hPa)"
#define PRESSURE_UNIT_SYMBOL_HPA "hPa"
#define PRESSURE_UNIT_TITLE_MM_HG "Millimetres of mercury (mmHg)"
#define PRESSURE_UNIT_SYMBOL_MM_HG "mmHg"
#define PRESSURE_UNIT_TITLE_IN_HG "Inches of mercury (inHg)"
#define PRESSURE_UNIT_SYMBOL_IN_HG "inHg"

/** List of available pressure units */
const PressureUnitDesc PRESSURE_UNITS[] = {
	{PRESSURE_UNIT_SYMBOL_HPA, PRESSURE_UNIT_TITLE_HPA, PRESSURE_UNIT_HPA},
	{PRESSURE_UNIT_SYMBOL_MM_HG, PRESSURE_UNIT_TITLE_MM_HG, PRESSURE_UNIT_MM_HG},
	{PRESSURE_UNIT_SYMBOL_IN_HG, PRESSURE_UNIT_TITLE_IN_HG, PRESSURE_UNIT_IN_HG}
};

/** Pressure unit titles formatted for dropdown */
#define PRESSURE_UNITS_DROPDOWN_ITEMS \
	PRESSURE_UNIT_TITLE_HPA "\n" \
	PRESSURE_UNIT_TITLE_MM_HG "\n" \
	PRESSURE_UNIT_TITLE_IN_HG

// Speed unit conversions from m/s
const float UNIT_M_S_TO_KM_H = 3.6;
const float UNIT_M_S_TO_MI_H = 2.23693629;
const float UNIT_M_S_TO_FT_S = 3.2808399;
const float UNIT_M_S_TO_KNOTS = 1.94384449;
const float UNIT_MM_TO_IN = 0.0393700787;
const float UNIT_HPA_TO_MMHG = 0.75006157584566;
const float UNIT_HPA_TO_INHG = 0.029529983071445;

//
// Client attributes
//
/** Current fw version */
const char TB_ATTR_GW_FW_VERSION[] = "gw_fw_version";

/** Bootloader version/compile-date */
const char TB_ATTR_GW_BOOTLOADER_VERSION[] = "gw_bootloader_version";

/** Device S/N */
const char TB_ATTR_GW_SERIAL_NUM[] = "gw_serial_number";

/** Device MAC address */
const char TB_ATTR_GW_MAC_ADDR[] = "gw_mac_address";

/** eFuse block 3 data */
const char TB_ATTR_EFUSE_BLK3[] = "efuse_blk3";

/** Current system unix time */
const char TB_ATTR_GW_SYSTEM_TIME[] = "gw_sys_time";

/** System tick in mS */
const char TB_ATTR_GW_SYSTEM_TICK_MS[] = "gw_tick_ms";

/** WiFi RSSI **/
const char TB_ATTR_GW_WIFI_RSSI[] = "gw_wifi_rssi";

/** WsHandler freq. friendly name */
const char TB_ATTR_GW_FREQ_NAME[] = "gw_freq_name";

/** Weather station ID currently listening to */
const char TB_ATTR_WS_ID[] = "ws_fo_id";

/** Weather station current reception */
const char TB_ATTR_WS_RECEPTION[] = "ws_reception";

/** Weather station current status  */
const char TB_ATTR_WS_STATUS[] = "ws_status";

/** Weather station current status duration  */
const char TB_ATTR_WS_STATUS_DUR[] = "ws_status_dur";

/** Current pairing mode (off/cloud/local/scan) */
const char TB_ATTR_PAIR_MODE[] = "gw_pair_mode";

/** Weather station firmware version warning (set if non-production version < 0x41 or > 0x42 detected) */
const char TB_ATTR_WS_FW_WARN[] = "ws_fw_warn";

/** Weather station last error (set on invalid/unverified packet, "OK" when valid) */
const char TB_ATTR_WS_LAST_ERR[] = "ws_last_err";

/** Weather station sensor health status ("OK" or "BAD_VALUES") */
const char TB_ATTR_WS_SENSOR_ERR[] = "ws_sensor_err";

/** Weather station GPS lifecycle status ("IDLE", "REQUESTED", "FIXED", "FAILED") */
const char TB_ATTR_WS_GPS_STATUS[] = "ws_gps_status";

/** Weather station ACK loss status (boolean flag) */
const char TB_ATTR_WS_ACK_LOSS[] = "ws_ack_loss";

/** Weather station cumulative lost packets counter */
const char TB_ATTR_WS_LOST_PACKETS[] = "ws_lost_packets";

//
// Locally paired station identity payload keys. Retained for an explicit future
// user-authorized adoption flow; SCAN never publishes these automatically or
// mutates the independent rs_ws_* cloud assignment.
//
const char TB_ATTR_LOCAL_WS_ID[] = "local_ws_id";
const char TB_ATTR_LOCAL_WS_MODEL[] = "local_ws_model";
const char TB_ATTR_LOCAL_WS_SN[] = "local_ws_sn";
const char TB_ATTR_LOCAL_WS_PUBKEY[] = "local_ws_pubkey";
const char TB_ATTR_LOCAL_PAIR_TS[] = "local_pair_ts";

//
// IPFS
//
/** Tries on IPFS requests before aborting */
const int IPFS_REQ_TRIES = 3;

/** Timeout for IPFS requests */
const int IPFS_REQ_TIMEOUT_MS = 15000;

//
// Debug
//
/** Intervals at which to print diagnostics */
const int DIAG_TASK_PRINT_INT_SEC = 10;

//
// Test mode
//

// Colors for labels
#define TEST_MODE_COLOR_PASS LV_COLOR_MAKE(0x00, 0x80, 0x00)
#define TEST_MODE_COLOR_FAIL LV_COLOR_MAKE(0xFF, 0x00, 0x20)

// Colors for LEDs
const u32 TEST_MODE_LEDS_COLOR_PASS = 0x00FF00;
const u32 TEST_MODE_LEDS_COLOR_FAIL = 0xFF0000;
const int TEST_MODE_LEDS_BRIGHTNESS = 50;

const char TEST_MODE_PASS_TEXT[] = "PASS";
const char TEST_MODE_FAIL_TEXT[] = "FAIL";

/** Time to wait before aborting and failing all parallel run tasks (total) */
const int TEST_MODE_PARALLEL_TESTS_TIMEOUT_MS = 570000;
/** Time to wait before aborting seq. run tasks (total) */
const int TEST_MODE_SEQ_TESTS_TIMEOUT_MS = 30000;

/** Wifi to connect to before running tests */
#define TEST_MODE_WIFI_SSID "WXM"
#define TEST_MODE_WIFI_PASS "12341234"

/** Wifi connection attempt timeout */
const int TEST_MODE_WIFI_TIMEOUT_MS = 10000;

// Test mode: WS test
/** Number of valid packets to receive to pass test */
const int TEST_MODE_WS_PACKETS_TO_RECEIVE = 2;

// Test mode: GPS test
/** Number of min satellites to fix to pass */
const int TEST_MODE_GPS_MIN_SATELLITES = 3;

/** Time of continuous GPS fix to pass test */
const int TEST_MODE_GPS_SIGNAL_MIN_CONT_FIX_SEC = 20;

// Test mode: Iot platform
/** Times to try all IOT platform test operations */
const int TEST_MODE_IOT_PLATFORM_TRIES = 3;



//
// UI
//
/** Default msgbox width */
const int UI_DEFAULT_MSGBOX_W = 350;

// Colors
#define COLOR_PRIMARY LV_COLOR_MAKE(0x00, 0x59, 0x9C)
#define COLOR_POS LV_COLOR_MAKE(0x1B, 0xB5, 0x52)
#define COLOR_NEG LV_COLOR_MAKE(0xD0, 0x2E, 0x2C)

//
// ATCA
//

#endif
