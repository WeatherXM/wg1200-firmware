// MUST be defined before ANY include. mbedTLS 4.x (IDF 6.x) hides the SHA-256
// context internals unless this is set, and the mbedTLS headers are pulled in
// transitively (tb.h -> mqtt_client.h -> esp_tls.h) long before the explicit
// include below - by which point their include guards are already set and
// defining it later has no effect. credentials.cpp does the same on line 1.
#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "cJSON.h"
#include "time.h"
#include "esp_ota_ops.h"
#include "esp_flash_partitions.h"
#include "esp_partition.h"
#include "math.h"
#include "const.h"
#include "app_config.h"
#include "submit.h"
#include "utils.h"
#include "tb.h"
#include "ws/include/ws_data.h"
#include "ws/include/ws_handler.h"
#include "ws/include/remote_observation_queue.h"
#include "tb_json.h"
#include "wifi.h"
#include "datetime_rtc.h"
#include "env.h"
#include "log.h"
#include "global_events.h"
#include "soft_watchdog.h"
#include "boot_guard.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <limits.h>

// mbedTLS 4.x (IDF 6.x) moved the SHA-256 declarations behind private headers.
// Mirrors the pattern already used in credentials.cpp so this builds on both.
// See the MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS define at the top of this file.
extern "C" {
#if __has_include("mbedtls/private/sha256.h")
#include "mbedtls/private/sha256.h"
#else
#include "mbedtls/sha256.h"
#endif
}

namespace Submit
{

///////////////////////////////////////////////////////////////////////////////
// Local vars
///////////////////////////////////////////////////////////////////////////////
/** Logger tag */
static const char TAG[] = "Submit";

/** Submission timer */
esp_timer_handle_t _timer_handle = NULL;

/** Submit data task handle */
static TaskHandle_t _submit_task_handle = NULL;

/** Submit autostart enabled */
bool _autostart_en = false;

/** One-time client attributes have been submitted */
bool _one_time_attr_submitted = false;

/** MQTT response buffer */
EXT_RAM_BSS_ATTR Tb::MqttResponse _mqtt_resp = {};

/** Task stack */
StackType_t _task_stack[SUBMIT_TASK_STACK_SIZE] = {};

/** Task buffer */
StaticTask_t _task_buff;

/** Soft Watchdog handle */
SoftWatchdog::Handle _swd_handle;

/** Shared OTA mutex to prevent concurrent Cloud OTA and Web OTA writes */
static StaticSemaphore_t _ota_mutex_buff;
static SemaphoreHandle_t _ota_mutex = NULL;

/** WXM cert to use with HTTPS */
extern const uint8_t _wxm_pem_start[] asm("_binary_wxm_pem_start");
extern const uint8_t wxm_pem_end[] asm("_binary_wxm_pem_end");

///////////////////////////////////////////////////////////////////////////////
// Local functions
///////////////////////////////////////////////////////////////////////////////
void submit_task(void *param);
void submit_ws_data();
void submit_remote_ws_data();
RetResult submit_gps_data();
RetResult submit_client_attr();;
void submit_logs();
RetResult handle_remote_settings();
RetResult handle_ota();
RetResult ota_cleanup();
RetResult publish_ota_failed(const char *error, const char *target_fw_version);
RetResult publish_ota_failed_details(const char *error, const char *target_fw_version,
	int chunk_index, int bytes_written, int bytes_expected,
	const char *checksum_expected, const char *checksum_flash, const char *verdict);
RetResult publish_update_ota_state(const char *state, const char *target_fw_version = NULL);
bool parse_sha256_hex(const char *hex, uint8_t digest[32]);
void sha256_to_hex(const uint8_t digest[32], char hex[65]);
bool sha256_of_partition(const esp_partition_t *partition, int length, uint8_t digest[32]);
bool parse_cloud_station_serial(const char *value, uint8_t serial[WS_SN_LEN], uint32_t *station_id);
bool parse_cloud_station_id(JsonVariantConst value, uint32_t *station_id);
// RetResult ipfs_add(char *filename, char *data, IPFSClient::IPFSFile *file);

bool parse_cloud_station_serial(const char *value, uint8_t serial[WS_SN_LEN], uint32_t *station_id)
{
	if(value == nullptr || serial == nullptr || station_id == nullptr)
	{
		return false;
	}
	const char *hex = value;
	if(strncmp(hex, "0x", 2) == 0 || strncmp(hex, "0X", 2) == 0)
	{
		hex += 2;
	}
	if(strlen(hex) != WS_SN_LEN * 2 ||
	   Utils::hex_str_to_byte_array(hex, serial, WS_SN_LEN) != RetResult::RET_OK)
	{
		return false;
	}
	*station_id = ((uint32_t)serial[0] << 24) |
				  ((uint32_t)serial[1] << 16) |
				  ((uint32_t)serial[2] << 8) |
				  (uint32_t)serial[3];
	return true;
}

bool parse_cloud_station_id(JsonVariantConst value, uint32_t *station_id)
{
	if(station_id == nullptr)
	{
		return false;
	}
	if(value.is<uint32_t>() || value.is<uint64_t>() || value.is<int>())
	{
		uint64_t parsed = value.as<uint64_t>();
		if(parsed > UINT32_MAX)
		{
			return false;
		}
		*station_id = (uint32_t)parsed;
		return true;
	}
	if(!value.is<const char*>())
	{
		return false;
	}

	const char *text = value.as<const char*>();
	if(text == nullptr || *text == '\0')
	{
		return false;
	}
	const char *digits = text;
	int base = 10;
	if(strncmp(digits, "0x", 2) == 0 || strncmp(digits, "0X", 2) == 0)
	{
		digits += 2;
		base = 16;
	}
	else if(strlen(digits) == 8)
	{
		// An eight-character station ID is the wire-format hexadecimal ID.
		base = 16;
	}

	errno = 0;
	char *end = nullptr;
	unsigned long long parsed = strtoull(digits, &end, base);
	if(errno != 0 || end == digits || *end != '\0' || parsed > UINT32_MAX)
	{
		return false;
	}
	*station_id = (uint32_t)parsed;
	return true;
}

/******************************************************************************
* Initialize
******************************************************************************/
RetResult init()
{
	ESP_LOGI(TAG, "Initializing sumbmission handler.");
	esp_err_t esp_ret;

	// Create timer that will run the data submit routine on specified intervals
	const esp_timer_create_args_t timer_args = {
		.callback = &start,
		.dispatch_method = ESP_TIMER_TASK,		
		.name = "submit",
		.skip_unhandled_events = true
	};

	if((esp_ret = esp_timer_create(&timer_args, &_timer_handle)) != ESP_OK)
	{
		ESP_LOGE(TAG, "Could not create submit timer: %x", esp_ret);
	}

	if(_ota_mutex == NULL)
	{
		_ota_mutex = xSemaphoreCreateMutexStatic(&_ota_mutex_buff);
	}

	SoftWatchdog::init(&_swd_handle, SUBMIT_TASK_SWD_TIMEOUT_MS);

	return RET_OK;
}

bool try_lock_ota()
{
	if(_ota_mutex == NULL)
	{
		return true;
	}
	return xSemaphoreTake(_ota_mutex, 0) == pdTRUE;
}

void unlock_ota()
{
	if(_ota_mutex != NULL)
	{
		xSemaphoreGive(_ota_mutex);
	}
}

/******************************************************************************
* Handles all data submission
* Runs on timer expiration
******************************************************************************/
void start(void *param)
{
	ESP_LOGW(TAG, "Submitting data");

	// Task already running
	if(_submit_task_handle != NULL)
	{
		return;
	}
	
    // Create the task using static creation API
    _submit_task_handle = xTaskCreateStaticPinnedToCore(submit_task, "submit", SUBMIT_TASK_STACK_SIZE, NULL, SUBMIT_TASK_PRIORITY, _task_stack, &_task_buff, 1);
	if(_submit_task_handle == NULL)
	{
		ESP_LOGE(TAG, "Could not create submit task");
	}

	// Create task with stack in internal memory
	// ota operations fail when run from a task that has its stack on ext mem.
	// BaseType_t ret = xTaskCreatePinnedToCore(&submit_task, "submit", SUBMIT_TASK_STACK_SIZE,
	// 	NULL, SUBMIT_TASK_PRIORITY, &_submit_task_handle, 1);
	// if(ret != pdPASS)
	// {
	// 	ESP_LOGE(TAG, "Could not create submit task. Error: %d", ret);
	// }

	// _submit_task_handle = xTaskCreateStatic(&submit_task, SUBMIT_TASK_NAME, sizeof(_task_stack), NULL,
	// 	SUBMIT_TASK_PRIORITY, _task_stack, &_task_buff);
	// if(_submit_task_handle == NULL)
	// {
	// 	ESP_LOGE(TAG, "Could not create submit task.");
	// }
}

/******************************************************************************
* Enable/Disable auto data submission by enabling/disalbing the timer
* Will not interrupt a submission in progress
******************************************************************************/
RetResult enable_autostart()
{
	//
	// TODO: Check if already timer enabled
	//
	ESP_LOGI(TAG, "Enabling auto data submission.");

	_autostart_en = true;

	set_timer();

	return RET_OK;
}

/******************************************************************************
* Enable/Disable auto data submission by enabling/disalbing the timer
* Will not interrupt a submission in progress
******************************************************************************/
RetResult disable_autostart()
{
	ESP_LOGI(TAG, "Disabling auto data submission.");

	_autostart_en = false;

	return esp_timer_stop(_timer_handle) == ESP_OK ? RET_OK : RET_ERROR;
}

/******************************************************************************
* Set timer if not already running, single shot.
******************************************************************************/
RetResult set_timer()
{
	if(esp_timer_get_next_alarm() == 0)
	{
		ESP_LOGE(TAG, "Timer already running.");
		return RET_INVALID_STATE;
	}

	esp_err_t esp_ret;
	if((esp_ret = esp_timer_start_once(_timer_handle, SUBMIT_TELEMETRY_INTERVAL_US)) != ESP_OK)
	{
		ESP_LOGE(TAG, "Could not start submit timer: 0x%X", esp_ret);
		return RET_ERROR;
	}

	return RET_OK;
}

/******************************************************************************
* Determine whether an observation from a given source should be uploaded to TB
******************************************************************************/
bool shouldUpload(const WsCommon::ObservationEnvelope &obs)
{
	if(obs.source == WsCommon::ObservationSource::DIRECT_LORA)
	{
		if(WsHandler::get_pair_state() != WsHandler::PairState::PAIRED)
		{
			return false;
		}
		if(Settings::get_pair_mode() == PairMode::PAIR_MODE_OFF)
		{
			return false;
		}
		return true;
	}
	else if(obs.source == WsCommon::ObservationSource::MESHCORE_REMOTE)
	{
		return obs.station_id != 0 && Settings::get_mesh_multi_station_forwarding_enabled();
	}
	return false;
}

/******************************************************************************
* Submit weather station data to TB and IPFS
******************************************************************************/
void submit_ws_data()
{
	// Packets since last signed packet submitted
	static uint16_t non_signed_packet_count = 0;

	WsCommon::ObservationEnvelope local_obs = {
		.source = WsCommon::ObservationSource::DIRECT_LORA,
		.station_id = 0,
		.timestamp = 0,
		.is_signed = false
	};

	// Unpaired gateways never uplink local weather (sniffed data may mix stations).
	// Buffers are also flushed on unpair; this guards the race in between.
	// OFF is monitor-only: keep receiving and decoding so the packet log and the
	// dashboard stay live, but publish nothing upstream.
	if(!shouldUpload(local_obs))
	{
		if(WsHandler::get_pair_state() != WsHandler::PairState::PAIRED)
		{
			ESP_LOGI(TAG, "Not paired: skipping local WS data submission.");
		}
		else if(Settings::get_pair_mode() == PairMode::PAIR_MODE_OFF)
		{
			ESP_LOGI(TAG, "Pair mode OFF (monitor only): skipping local WS data submission.");
			WsData::clear_aggregates();
		}
		return;
	}

	int agg_items_count = WsData::get_aggregate_count();
	int cur_index = 1;

	if(agg_items_count < 1)
	{
		ESP_LOGW(TAG, "No WS data to submit.");
		return;
	}

	while(cur_index <= agg_items_count)
	{
		SoftWatchdog::feed(&_swd_handle);
		ESP_LOGI(TAG, "Submitting WS item %d/%d\n", cur_index, agg_items_count);

		WsFields::FieldPack agg_pack = {};
		StaticJsonDocument<TB_JSON_DOC_SIZE> json_doc;
		char json_buff[TB_JSON_BUFF_SIZE] = "";

		if(WsData::pop_aggregate_item(&agg_pack) != WsCommon::RetRes::OK)
		{
			ESP_LOGE(TAG, "Could not pop WS item %d/%d\n", cur_index, agg_items_count);
			Log::logw(Log::Code::TB_SUBMIT_ITEM_FAILED);
			break;
		}

		// Build TB json
		if(TBJson::from_ws_aggregate(&agg_pack, &json_doc) != RET_OK)
		{
			ESP_LOGE(TAG, "Could not build JSON for WS item %d/%d\n", cur_index, agg_items_count);
			continue;
		}
		serializeJson(json_doc, json_buff, sizeof(json_buff));

		
		// Sign packet and add to telemetry
		
		// Check if it is time for a signed packet to be submitted
		if(non_signed_packet_count >= TB_PACKET_SIGNING_INTERVAL)
		{
			char signed_buff[TB_JSON_BUFF_SIZE] = {};
			TBJson::signed_ws_packet_structure(&agg_pack, signed_buff, sizeof(signed_buff));

			if(TBJson::append_telemetry_signature(signed_buff, &json_doc, TB_KEY_WEATHER_DATA_SIGNATURE_PREFIX) == RET_OK)
			{
				non_signed_packet_count = 0;
			}
			else
			{
				ESP_LOGE(TAG, "Could not sign packet.");
			}
		}

		//
		// Submit to IPFS and get hash
		//
		// if(IPFS_SUBMIT_ENABLED)
		// {
		// 	IPFSClient::IPFSFile ipfs_file = {};
		// 	char filename[16] = "";
		// 	snprintf(filename, sizeof(filename), "%d.txt", agg.packet.tstamp);

		// 
		// 	if(ipfs_add(filename, json_buff, &ipfs_file) == RET_OK)
		// 	{
		// 		// Add CID to JSON doc
		// 		TBJson::add_ipfs_hash_to_values(&json_doc, ipfs_file.cid);
		// 	}
		// 	
		// }

		//
		// Submit packet
		//
		if(PRINT_TB_JSON_ON_SUBMIT)
		{
			serializeJsonPretty(json_doc, json_buff, sizeof(json_buff));
			printf("Submitting WS data to TB: %s\n", json_buff);
		}

		// Serialize doc now that it has been updated with IPFS CID and signature
		serializeJson(json_doc, json_buff, sizeof(json_buff));
		if(Tb::publish_telemetry(json_buff) != RET_OK)
		{
			ESP_LOGE(TAG, "Could not publish telemetry item %d/%d\n", cur_index, agg_items_count);
			Log::logw(Log::Code::TB_SUBMIT_ITEM_FAILED);
		}

		non_signed_packet_count++;
		cur_index++;
	}
}

/******************************************************************************
* Submit forwarded remote weather station data received over MeshCore
******************************************************************************/
void submit_remote_ws_data()
{
	if(!Settings::get_mesh_multi_station_forwarding_enabled())
	{
		RemoteObservationQueue::clear();
		return;
	}

	int count = RemoteObservationQueue::get_count();
	if(count <= 0) return;

	ESP_LOGI(TAG, "Submitting %d remote MeshCore station observations\n", count);

	while(count-- > 0)
	{
		SoftWatchdog::feed(&_swd_handle);
		RemoteObservationQueue::RemoteObservation robs = {};
		if(RemoteObservationQueue::pop(&robs) != WsCommon::RetRes::OK)
		{
			break;
		}

		WsCommon::ObservationEnvelope env = {
			.source = WsCommon::ObservationSource::MESHCORE_REMOTE,
			.station_id = robs.station_id,
			.timestamp = robs.timestamp,
			.is_signed = robs.is_signed
		};

		if(!shouldUpload(env))
		{
			continue;
		}

		StaticJsonDocument<TB_JSON_DOC_SIZE> json_doc;
		char json_buff[TB_JSON_BUFF_SIZE] = "";

		if(TBJson::from_ws_aggregate(&robs.field_pack, &json_doc) != RET_OK)
		{
			ESP_LOGE(TAG, "Could not build JSON for remote observation 0x%08X\n", (unsigned int)robs.station_id);
			continue;
		}

		// Annotate forwarded metadata
		json_doc["relayed_from_sta"] = robs.station_id;
		json_doc["relayed_rssi"] = robs.rssi;
		json_doc["relayed_snr"] = robs.snr;

		serializeJson(json_doc, json_buff, sizeof(json_buff));
		if(Tb::publish_telemetry(json_buff) != RET_OK)
		{
			ESP_LOGE(TAG, "Could not publish remote telemetry for station 0x%08X\n", (unsigned int)robs.station_id);
			Log::logw(Log::Code::TB_SUBMIT_ITEM_FAILED);
		}
		else
		{
			ESP_LOGI(TAG, "Forwarded remote telemetry for station 0x%08X (ts=%u)\n",
					 (unsigned int)robs.station_id, (unsigned int)robs.timestamp);
		}
	}
}

/******************************************************************************
* Submit telemetry as file to IPFS
* @param filename Filename
* @param data File contents
* @param file IPFSFile structure to receive result
* @return
*	RET_OK File added to IPFS successfully.
******************************************************************************/
// RetResult ipfs_add(char *filename, char *data, IPFSClient::IPFSFile *file)
// {
// 	if(file == NULL || data == NULL || file == NULL)
// 	{
// 		return RET_ERROR;
// 	}

// 	RetResult ret = RET_ERROR;

// 	IPFSClient ipfs_client;
// 	char ipfs_buff[1000] = "";
// 	ipfs_client.set_addr(IPFS_REST_ADDR);
// 	ipfs_client.set_basic_auth_creds_base64(IPFS_BASIC_AUTH_CREDS_BASE64);
// 	ipfs_client.set_buffer(ipfs_buff, sizeof(ipfs_buff));

// 	esp_tls_cfg_t tls_cfg = {
// 		.cacert_buf = (const unsigned char *)_wxm_pem_start,
// 		.cacert_bytes = (unsigned int)(wxm_pem_end - _wxm_pem_start),
// 		.use_secure_element = true,
// 		.timeout_ms = IPFS_REQ_TIMEOUT_MS,
// 	};

// 	ipfs_client.set_tls_cfg(&tls_cfg);

// 	// Try multiple times before aborting
// 	// Due to AWS-SDK ATECC thread-unsafety bug/feature, verifying cert might sometimes fail
// 	// the first time.
// 	int tries = IPFS_REQ_TRIES;
// 	while(tries--)
// 	{
// 		if(!ipfs_client.is_connected())
// 		{
// 			ESP_LOGW(TAG, "IPFS Not connected, connecting...");
// 			if(ipfs_client.connect() != IPFSClient::IPFS_CLIENT_OK && tries)
// 			{
// 				ESP_LOGW(TAG, "Could not open IPFS connection. Retrying...");
// 			}
// 		}
// 		else
// 		{
// 			break;
// 		}
// 	}

// 	if(ipfs_client.is_connected())
// 	{
// 		// Filename used anywhere?
// 		if(ipfs_client.add(file, filename, data) != IPFSClient::IPFS_CLIENT_OK)
// 		{
// 			ESP_LOGE(TAG, "Could not submit to IPFS.");
// 			ret = RET_ERROR;
// 		}
// 		else
// 		{
// 			ESP_LOGI(TAG, "Submitted to IPFS: %s %s (%d bytes)", file->name, file->cid, file->size);;

// 			ret = RET_OK;
// 		}
// 	}
// 	else
// 	{
// 		ESP_LOGE(TAG, "Failed to open connection to IPFS, aborting IPFS submission.");

// 		ret = RET_ERROR;
// 	}

// 	ipfs_client.disconnect();

// 	return ret;
// }

/******************************************************************************
* Submit GPS data to TB
******************************************************************************/
RetResult submit_gps_data()
{
	// Packets since last signed packet submitted
	static uint16_t non_signed_packet_count = 0;

	// Monitor-only mode uplinks nothing, station position included. Still pop the
	// queued fix so the buffer cannot grow unbounded while OFF is selected.
	if(Settings::get_pair_mode() == PairMode::PAIR_MODE_OFF)
	{
		WsGps::GpsData discard = {};
		while(WsGps::pop_gps_data(&discard)) {}
		ESP_LOGI(TAG, "Pair mode OFF (monitor only): skipping GPS submission.");
		return RET_OK;
	}

	WsGps::GpsData gps_pack = {};
	if(!WsGps::pop_gps_data(&gps_pack))
	{
		return RET_ERROR;
	}

	StaticJsonDocument<TB_JSON_DOC_SIZE> json_doc;
	char json_buff[TB_JSON_BUFF_SIZE] = "";

	// Build TB json
	if(TBJson::from_gps_data(&gps_pack, &json_doc) != RET_OK)
	{
		ESP_LOGE(TAG, "Could not build JSON for GPS data.");
		return RET_ERROR;
	}
	serializeJson(json_doc, json_buff, sizeof(json_buff));

	//
	// Sign packet and add to telemetry
	//
	// Check if it is time for a signed ws packet to be submitted
	if(non_signed_packet_count >= TB_PACKET_SIGNING_INTERVAL)
	{
		char signed_buff[TB_JSON_BUFF_SIZE] = {};
		TBJson::signed_gps_packet_structure(&gps_pack, signed_buff, sizeof(signed_buff));

		if(TBJson::append_telemetry_signature(signed_buff, &json_doc, TB_KEY_GPS_SIGNATURE_PREFIX) == RET_OK)
		{
			non_signed_packet_count = 0;
		}
		else
		{
			ESP_LOGE(TAG, "Could not sign packet.");
		}
	}

	//
	// Submit packet
	//
	if(PRINT_TB_JSON_ON_SUBMIT)
	{
		serializeJsonPretty(json_doc, json_buff, sizeof(json_buff));
		printf("Submitting GPS data to TB: %s\n", json_buff);
	}

	serializeJson(json_doc, json_buff, sizeof(json_buff));
	if(Tb::publish_telemetry(json_buff) != RET_OK)
	{
		ESP_LOGE(TAG, "Could not publish GPS telemetry JSON.");
	}
	non_signed_packet_count++;

	ESP_LOGI(TAG, "Done submitting GPS data\n");

	return RET_OK;
}

/******************************************************************************
* Publish update on current OTA state as client attributes and telemetry
******************************************************************************/
RetResult publish_update_ota_state(const char *state, const char *target_fw_version)
{
	char json[200] = "";

	snprintf(json, sizeof(json), OTA_TB_UPDATE_STATE_JSON_FMT, "WG1200 Miner", APP_VERSION, target_fw_version, state);

	printf("Pub OTA state %s\n", json);

	Tb::publish(TB_ATTRIBUTE_PUBLISH_TOPIC, json);
	return Tb::publish_telemetry(json);
}

/******************************************************************************
* Publish failed message to TB as client attributes and telemetry
******************************************************************************/
/******************************************************************************
* Publish OTA failure with integrity-check context
******************************************************************************/
RetResult publish_ota_failed_details(const char *error, const char *target_fw_version,
	int chunk_index, int bytes_written, int bytes_expected,
	const char *checksum_expected, const char *checksum_flash, const char *verdict)
{
	char json[640] = "";

	snprintf(json, sizeof(json), OTA_TB_FAILED_DETAILS_JSON_FMT,
		target_fw_version, error, chunk_index, bytes_written, bytes_expected,
		checksum_expected, checksum_flash, verdict);

	return Tb::publish_telemetry(json);
}

/******************************************************************************
* Parse a ThingsBoard SHA-256 hex string into 32 raw bytes
******************************************************************************/
bool parse_sha256_hex(const char *hex, uint8_t digest[32])
{
	if(hex == NULL || strlen(hex) != 64)
	{
		return false;
	}

	for(size_t i = 0; i < 32; i++)
	{
		char high = hex[2 * i];
		char low = hex[2 * i + 1];
		int high_val = high >= '0' && high <= '9' ? high - '0' :
			high >= 'a' && high <= 'f' ? high - 'a' + 10 :
			high >= 'A' && high <= 'F' ? high - 'A' + 10 : -1;
		int low_val = low >= '0' && low <= '9' ? low - '0' :
			low >= 'a' && low <= 'f' ? low - 'a' + 10 :
			low >= 'A' && low <= 'F' ? low - 'A' + 10 : -1;

		if(high_val < 0 || low_val < 0)
		{
			return false;
		}

		digest[i] = (uint8_t)((high_val << 4) | low_val);
	}

	return true;
}

/******************************************************************************
* Format a 32 byte digest as lowercase hex
******************************************************************************/
void sha256_to_hex(const uint8_t digest[32], char hex[65])
{
	static const char HEX[] = "0123456789abcdef";
	for(size_t i = 0; i < 32; i++)
	{
		hex[2 * i] = HEX[digest[i] >> 4];
		hex[2 * i + 1] = HEX[digest[i] & 0x0F];
	}
	hex[64] = '\0';
}

/******************************************************************************
* Read the first <length> bytes of an OTA partition back from flash and hash
* them in a single pass.
*
* Deliberately hashes the flash read-back rather than digesting chunks as they
* stream in. A digest accumulated during the download must keep one SHA-256
* context open for the whole transfer, competing with TLS and the DS peripheral
* for the shared hardware SHA engine; a disturbed save/restore there corrupts
* the digest even when every flashed byte is correct. Hashing after the fact
* uses one short-lived context and additionally verifies what actually landed in
* flash, catching write errors a RAM-side digest cannot see.
*
* Reads into an internal-RAM buffer: the hardware SHA DMA cannot coherently read
* external PSRAM.
*
* NOTE: valid only while flash encryption is disabled. With encryption enabled,
* esp_partition_read on an app partition would not return the plaintext that was
* hashed by the server.
******************************************************************************/
bool sha256_of_partition(const esp_partition_t *partition, int length, uint8_t digest[32])
{
	if(partition == NULL || length <= 0 || length > (int)partition->size)
	{
		return false;
	}

	uint8_t *read_buf = (uint8_t *)heap_caps_malloc(OTA_VERIFY_READ_BUF_SIZE,
		MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
	if(read_buf == NULL)
	{
		ESP_LOGE(TAG, "Could not allocate OTA verify read buffer");
		return false;
	}

	mbedtls_sha256_context ctx;
	mbedtls_sha256_init(&ctx);
	bool ok = mbedtls_sha256_starts(&ctx, 0) == 0;

	int offset = 0;
	int chunk_num = 0;
	while(ok && offset < length)
	{
		SoftWatchdog::feed(&_swd_handle);
		if((++chunk_num & 0x0F) == 0)
		{
			vTaskDelay(1);
		}

		int read_len = length - offset;
		if(read_len > OTA_VERIFY_READ_BUF_SIZE)
		{
			read_len = OTA_VERIFY_READ_BUF_SIZE;
		}

		if(esp_partition_read(partition, offset, read_buf, read_len) != ESP_OK)
		{
			ESP_LOGE(TAG, "OTA verify: flash read failed at offset %d", offset);
			ok = false;
			break;
		}

		if(mbedtls_sha256_update(&ctx, read_buf, read_len) != 0)
		{
			ESP_LOGE(TAG, "OTA verify: SHA-256 update failed at offset %d", offset);
			ok = false;
			break;
		}

		offset += read_len;
	}

	if(ok)
	{
		ok = mbedtls_sha256_finish(&ctx, digest) == 0;
	}

	mbedtls_sha256_free(&ctx);
	heap_caps_free(read_buf);

	return ok;
}

RetResult publish_ota_failed(const char *error, const char *target_fw_version)
{
	char json[200] = "";

	snprintf(json, sizeof(json), OTA_TB_FAILED_ERROR_JSON_FMT, target_fw_version, error);

	Tb::publish(TB_ATTRIBUTE_PUBLISH_TOPIC, json);
	return Tb::publish_telemetry(json);
}

static uint32_t s_consecutive_ota_failures = 0;
static constexpr uint32_t MAX_CONSECUTIVE_OTA_FAILURES = 5;

static void record_ota_failure(const char *reason)
{
	s_consecutive_ota_failures++;
	ESP_LOGW(TAG, "OTA attempt failed (%s). Consecutive failures: %lu/%lu",
			 reason != nullptr ? reason : "error",
			 (unsigned long)s_consecutive_ota_failures,
			 (unsigned long)MAX_CONSECUTIVE_OTA_FAILURES);
	if(s_consecutive_ota_failures >= MAX_CONSECUTIVE_OTA_FAILURES)
	{
		ESP_LOGE(TAG, "Exceeded %lu consecutive OTA failures. Restarting gateway for clean recovery...",
				 (unsigned long)MAX_CONSECUTIVE_OTA_FAILURES);
		s_consecutive_ota_failures = 0;
		ota_cleanup();
		SoftWatchdog::stop(&_swd_handle);
		vTaskDelay(pdMS_TO_TICKS(1000));
		Utils::safe_restart(Utils::RestartReason::OTA_FAILED);
	}
}

/******************************************************************************
* Download and apply OTA update if available
* Get TB OTA attributes and if available fw is newer than current, update and
* reboot.
******************************************************************************/
RetResult handle_ota()
{
	static int64_t last_ota_check_ms = 0;
	if(last_ota_check_ms != 0 && (Utils::get_tick_ms() - last_ota_check_ms) / 1000 < SUBMIT_OTA_CHECK_INTERVAL_SEC)
	{
		return RET_OK;
	}

	RetResult ret = RET_ERROR;
	esp_err_t esp_ret = 0;

	// Subscribe to attribute response topic
	if(Tb::subscribe(TB_ATTRIBUTE_RESP_TOPIC, TB_MQTT_OTA_QOS) < 1)
	{
		return RET_ERROR;
	}

	// Publish OTA attributes request and wait
	if(Tb::publish(TB_ATTRIBUTE_REQ_TOPIC, (char*)OTA_TB_ATTRIBUTE_REQ_JSON) < 0)
	{
		return RET_ERROR;
	}

	ret = Tb::wait_response((char*)TB_ATTRIBUTE_WAIT_TOPIC, &_mqtt_resp, TB_MQTT_DEFAULT_WAIT_RESP_TIMEOUT_MS);
	if(ret != RET_OK)
	{
		ESP_LOGE(TAG, "Could not get OTA attributes.");
		return RET_ERROR;
	}

	// Parse JSON
	StaticJsonDocument<TB_JSON_DOC_SIZE> attr_doc;
	DeserializationError error = deserializeJson(attr_doc, _mqtt_resp.data, _mqtt_resp.data_len);
	JsonObject shared_attr = attr_doc.getMember(TB_KEY_SHARED_ATTRIBUTES);

	if(error != DeserializationError::Ok)
	{
		ESP_LOGE(TAG, "Could not parse OTA attr json: %s", error.c_str());
		return RET_ERROR;
	}

	// Mark check timestamp only after a valid attribute response is received
	last_ota_check_ms = Utils::get_tick_ms();

	// Check if all OTA attributes present
	if(!(shared_attr.containsKey(TB_KEY_OTA_FW_VERSION) && shared_attr.containsKey(TB_KEY_OTA_FW_SIZE) &&
		shared_attr.containsKey(TB_KEY_OTA_FW_CHECKSUM) && shared_attr.containsKey(TB_KEY_OTA_FW_CHECKSUM_ALGO)))
		{
			ESP_LOGI(TAG, "Device has no OTA keys, aborting.");
			return RET_ERROR;
		}

	// Copy to use it later, after OTA finishes.
	// Buffer was 10 bytes and filled with a plain strncpy(.., sizeof(buf)):
	// any attribute >= 10 chars (e.g. "0.7.7.7-g9b573c4") left version_str
	// WITHOUT a NUL terminator, so every later read of it overran the buffer.
	char version_str[32] = "";
	const char *version_attr = shared_attr[TB_KEY_OTA_FW_VERSION].as<const char *>();
	if(version_attr == NULL || strlen(version_attr) >= sizeof(version_str))
	{
		ESP_LOGE(TAG, "Invalid or oversized OTA version attribute.");
		return RET_ERROR;
	}
	strncpy(version_str, version_attr, sizeof(version_str) - 1);
	version_str[sizeof(version_str) - 1] = '\0';

	const int fw_size_bytes = shared_attr[TB_KEY_OTA_FW_SIZE].as<int>();

	// The checksum attributes were previously required to be present and then
	// never used, leaving the Secure Boot signature at esp_ota_end() as the only
	// integrity gate. That proves the image is authentic but not that it is the
	// image ThingsBoard intended to serve, and it reports nothing useful when it
	// fails. Parse them and verify against flash after the download.
	const char *fw_checksum_attr = shared_attr[TB_KEY_OTA_FW_CHECKSUM].as<const char *>();
	const char *fw_checksum_algo = shared_attr[TB_KEY_OTA_FW_CHECKSUM_ALGO].as<const char *>();
	uint8_t expected_sha256[32] = {};
	char expected_sha256_hex[65] = "";

	if(fw_checksum_algo == NULL ||
		(strcasecmp(fw_checksum_algo, "SHA256") != 0 &&
		strcasecmp(fw_checksum_algo, "SHA-256") != 0) ||
		!parse_sha256_hex(fw_checksum_attr, expected_sha256))
	{
		ESP_LOGE(TAG, "OTA requires a valid SHA-256 checksum (algo: %s).",
			fw_checksum_algo != NULL ? fw_checksum_algo : "none");
		return RET_ERROR;
	}
	sha256_to_hex(expected_sha256, expected_sha256_hex);

	ESP_LOGI(TAG, "---- OTA FW ATTRIBUTES ----");
	ESP_LOGI(TAG, "Version: %s", version_str);
	ESP_LOGI(TAG, "Size: %d", fw_size_bytes);
	ESP_LOGI(TAG, "SHA-256: %s", expected_sha256_hex);
	ESP_LOGI(TAG, "---------------------------");

	if(fw_size_bytes < 1)
	{
		ESP_LOGE(TAG, "Invalid file size %d", fw_size_bytes);
		return RET_ERROR;
	}

	// All attributes OK, check if version is newer
	int ver_cmp = Utils::version_compare((char*)version_str, (char*)APP_VERSION);
	if(ver_cmp == -1)
	{
		ESP_LOGI(TAG, "Online version is older, aborting OTA.");
		s_consecutive_ota_failures = 0;
		return RET_ERROR;
	}
	else if(ver_cmp == 0)
	{
		ESP_LOGI(TAG, "Version is current, aborting OTA.");
		s_consecutive_ota_failures = 0;
		return RET_ERROR;
	}
	else if(ver_cmp == -2)
	{
		ESP_LOGE(TAG, "Invalid version provided (%s), aborting OTA.", version_str);
		return RET_ERROR;
	}
	else if(ver_cmp == 1)
	{
		ESP_LOGI(TAG, "New OTA version online: %s (current: %s). Proceeding with update.", version_str, APP_VERSION);
	}

	// Prevent concurrent Cloud OTA and Web OTA writes to the same partition
	if(!try_lock_ota())
	{
		ESP_LOGW(TAG, "OTA already in progress (Web OTA active), deferring cloud OTA.");
		last_ota_check_ms = 0;
		return RET_ERROR;
	}
	struct OtaLockGuard {
		~OtaLockGuard() { unlock_ota(); }
	} ota_lock_guard;

	//
	// Download and write FW
	//
	// From this point on, TB telemetry updates are published
	char write_err[100] = "";
	publish_update_ota_state("DOWNLOADING", version_str);

	// Subscribe to data receive topic
	if (Tb::subscribe(OTA_TB_FW_RECV_TOPIC, TB_MQTT_OTA_QOS) < 1)
	{
		const char err[] = "Could not subscribe to FW data topic.";
		publish_ota_failed(err, version_str);
		ESP_LOGE(TAG, "%s", err);
		record_ota_failure(err);
		return RET_ERROR;
	}

	ESP_LOGI(TAG, "Getting update partition");
	const esp_partition_t *ota_partition = esp_ota_get_next_update_partition(NULL);
	if(ota_partition == NULL)
	{
		snprintf(write_err, sizeof(write_err), "Could not find OTA partition: %s", esp_err_to_name(esp_ret));
		publish_ota_failed(write_err, version_str);
		ESP_LOGE(TAG, "%s", write_err);
		record_ota_failure(write_err);
		return RET_ERROR;
	}

	// Reject an image that cannot physically fit before erasing anything
	if(fw_size_bytes > (int)ota_partition->size)
	{
		snprintf(write_err, sizeof(write_err), "FW size %d exceeds OTA slot %d",
			fw_size_bytes, (int)ota_partition->size);
		publish_ota_failed(write_err, version_str);
		ESP_LOGE(TAG, "%s", write_err);
		record_ota_failure(write_err);
		return RET_ERROR;
	}

	// Begin OTA with sequential writes so erase is performed incrementally
	// per sector during esp_ota_write() rather than a blocking upfront erase.
	esp_ota_handle_t ota_handle = 0;
	esp_ret = esp_ota_begin(ota_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
	if (esp_ret != ESP_OK)
	{
		snprintf(write_err, sizeof(write_err), "OTA begin failed: %s", esp_err_to_name(esp_ret));
		publish_ota_failed(write_err, version_str);
		ESP_LOGE(TAG, "%s", write_err);
		record_ota_failure(write_err);
		return RET_ERROR;
	}

	// Start download and write to partition
	int chunk_count = ceil((float)fw_size_bytes / OTA_CHUNK_SIZE);
	int bytes_written = 0;

	bool failed = false;

	int last_chunk_index = -1;

	for (int chunk_index = 0; chunk_index < chunk_count; chunk_index++)
	{
		SoftWatchdog::feed(&_swd_handle);
		last_chunk_index = chunk_index;
		uint8_t chunk_retries = 0;
		bool chunk_failed = false;

		// How many bytes this chunk must contain. Without this the loop accepted
		// a short chunk silently, producing a truncated image that only failed
		// much later at signature verification.
		int expected_chunk_len = fw_size_bytes - bytes_written;
		if(expected_chunk_len > OTA_CHUNK_SIZE)
		{
			expected_chunk_len = OTA_CHUNK_SIZE;
		}

		char chunk_rcv_topic[TB_MQTT_DEFAULT_TOPIC_LEN];
		snprintf(chunk_rcv_topic, sizeof(chunk_rcv_topic), OTA_TB_FW_RECV_CHUNK_TOPIC, chunk_index, chunk_index);
		do
		{
			SoftWatchdog::feed(&_swd_handle);
			chunk_retries++;
			chunk_failed = false;
			write_err[0] = '\0';

			// Request topic built in its own buffer rather than borrowing
			// _mqtt_resp.topic, which is the field the receive path matches on.
			// Fmt: request id, chunk id
			char req_topic[TB_MQTT_DEFAULT_TOPIC_LEN];
			snprintf(req_topic, sizeof(req_topic), OTA_TB_FW_REQ_TOPIC_FMT, chunk_index, chunk_index);

			// Convert chunk size to str to publish in body of req
			char chunk_size_str[10] = "";
			snprintf(chunk_size_str, sizeof(chunk_size_str), "%d", OTA_CHUNK_SIZE);

			// Arm the wait BEFORE publishing the request. Previously the wait
			// was armed inside wait_response(), i.e. after the request had
			// already gone out, so a reply that came back faster than this task
			// could arm arrived while the handler was IDLE and was discarded -
			// costing a full timeout plus a retry on every occurrence.
			if(Tb::start_wait_response(chunk_rcv_topic) != RET_OK)
			{
				snprintf(write_err, sizeof(write_err), "Arm wait failed, chunk %d/%d", chunk_index + 1, chunk_count);
				ESP_LOGE(TAG, "%s", write_err);
				chunk_failed = true;
			}
			else if(Tb::publish(req_topic, chunk_size_str, TB_MQTT_OTA_QOS) < 0)
			{
				Tb::cancel_wait_response();
				snprintf(write_err, sizeof(write_err), "Pub. chunk req. failed, chunk %d/%d", chunk_index + 1, chunk_count);
				ESP_LOGE(TAG, "%s", write_err);
				chunk_failed = true;
			}
			else if(Tb::await_response(&_mqtt_resp, TB_MQTT_DEFAULT_WAIT_RESP_TIMEOUT_MS) != RET_OK)
			{
				snprintf(write_err, sizeof(write_err), "Receive failed, chunk %d/%d", chunk_index + 1, chunk_count);
				ESP_LOGE(TAG, "%s", write_err);
				chunk_failed = true;
				if(!Tb::is_connected())
				{
					ESP_LOGE(TAG, "TB is not connected, aborting OTA");
					break;
				}
			}
			else if(_mqtt_resp.data_len != expected_chunk_len)
			{
				snprintf(write_err, sizeof(write_err),
					"Short chunk %d/%d: got %d expected %d",
					chunk_index + 1, chunk_count, _mqtt_resp.data_len, expected_chunk_len);
				ESP_LOGE(TAG, "%s", write_err);
				chunk_failed = true;
			}

			if(chunk_failed)
			{
				char reason[64];
				strncpy(reason, write_err, sizeof(reason) - 1);
				reason[sizeof(reason) - 1] = '\0';
				snprintf(write_err, sizeof(write_err), "Chunk %d retry %d: %s", chunk_index, chunk_retries, reason);
				ESP_LOGE(TAG, "%s", write_err);
				if(chunk_retries < OTA_TB_FW_CHUNK_MAX_RETRIES)
				{
					vTaskDelay(pdMS_TO_TICKS(1000));
				}
			}

		}
		while(chunk_retries < OTA_TB_FW_CHUNK_MAX_RETRIES && chunk_failed);

		if(chunk_failed)
		{
			failed = true;
			ESP_LOGE(TAG, "Critical failure at chunk %d ", chunk_index);
			break;
		}

		ESP_LOGI(TAG, "Writing %d bytes", _mqtt_resp.data_len);
		esp_ret = esp_ota_write(ota_handle, (const void *)_mqtt_resp.data, _mqtt_resp.data_len);
		if(esp_ret != ESP_OK)
		{
			snprintf(write_err, sizeof(write_err), "Write failed, chunk %d/%d: %s", chunk_index + 1, chunk_count, esp_err_to_name(esp_ret));
			ESP_LOGE(TAG, "%s", write_err);
			failed = true;
			break;
		}

		bytes_written += _mqtt_resp.data_len;

		// Percentage computed as (written * 100) / size. The previous form,
		// written / (size / 100), divides by zero for any fw_size_bytes < 100 -
		// a value that passes the "< 1" guard above.
		ESP_LOGI(TAG, "Written %5d / %5d KB (%d%%)", bytes_written / 1024,
			fw_size_bytes / 1024, (int)(((int64_t)bytes_written * 100) / fw_size_bytes));
	}

	if(failed)
	{
		// Release the OTA handle. Without this every failed download leaked the
		// handle and left the partition in a partially written state.
		esp_ota_abort(ota_handle);

		if(strlen(write_err) > 0)
		{
			publish_ota_failed_details(write_err, version_str, last_chunk_index,
				bytes_written, fw_size_bytes, expected_sha256_hex, "not-run",
				"download-failed");
		}
		ESP_LOGE(TAG, "OTA failed");
		record_ota_failure(strlen(write_err) > 0 ? write_err : "Download failed");
		return RET_ERROR;
	}

	if(bytes_written != fw_size_bytes)
	{
		snprintf(write_err, sizeof(write_err), "OTA length mismatch: wrote %d expected %d",
			bytes_written, fw_size_bytes);
		esp_ota_abort(ota_handle);
		publish_ota_failed_details(write_err, version_str, last_chunk_index,
			bytes_written, fw_size_bytes, expected_sha256_hex, "not-run",
			"length-mismatch");
		ESP_LOGE(TAG, "%s", write_err);
		record_ota_failure(write_err);
		return RET_ERROR;
	}

	//
	// Integrity check: re-read the image out of flash and hash it.
	// Runs before esp_ota_end() so a bad image is never finalized.
	//
	uint8_t flash_sha256[32] = {};
	char flash_sha256_hex[65] = "";

	SoftWatchdog::feed(&_swd_handle);
	if(!sha256_of_partition(ota_partition, fw_size_bytes, flash_sha256))
	{
		// Could not verify. Fail closed: with no bootloader rollback configured,
		// an unverified image is not worth the risk of committing.
		snprintf(write_err, sizeof(write_err), "OTA verification unavailable");
		esp_ota_abort(ota_handle);
		publish_ota_failed_details(write_err, version_str, last_chunk_index,
			bytes_written, fw_size_bytes, expected_sha256_hex, "unavailable",
			"verify-failed");
		ESP_LOGE(TAG, "%s", write_err);
		record_ota_failure(write_err);
		return RET_ERROR;
	}
	SoftWatchdog::feed(&_swd_handle);

	sha256_to_hex(flash_sha256, flash_sha256_hex);

	if(memcmp(flash_sha256, expected_sha256, sizeof(expected_sha256)) != 0)
	{
		snprintf(write_err, sizeof(write_err), "OTA SHA-256 mismatch");
		esp_ota_abort(ota_handle);
		publish_ota_failed_details(write_err, version_str, last_chunk_index,
			bytes_written, fw_size_bytes, expected_sha256_hex, flash_sha256_hex,
			"checksum-mismatch");
		ESP_LOGE(TAG, "%s (expected %s, flash %s)", write_err,
			expected_sha256_hex, flash_sha256_hex);
		record_ota_failure(write_err);
		return RET_ERROR;
	}

	ESP_LOGI(TAG, "OTA image verified against flash: %s", flash_sha256_hex);

	publish_update_ota_state("DOWNLOADED", version_str);

	// Finalize
	SoftWatchdog::feed(&_swd_handle);
	esp_ret  = esp_ota_end(ota_handle);
	SoftWatchdog::feed(&_swd_handle);
	if(esp_ret != ESP_OK)
	{
		snprintf(write_err, sizeof(write_err), "OTA end failed: %s", esp_err_to_name(esp_ret));
		ESP_LOGE(TAG, "%s", write_err);
		publish_ota_failed(write_err, version_str);
		record_ota_failure(write_err);
		return RET_ERROR;
	}

	// Check if running partition is same as ota partition (downloaded same binary)
	if(esp_partition_check_identity(esp_ota_get_running_partition(), ota_partition) == true)
	{
		snprintf(write_err, sizeof(write_err), "OTA fw is same as current.");
		publish_ota_failed(write_err, version_str);
		ESP_LOGE(TAG, "OTA firmware is same as current firmware.");
		return RET_ERROR;
	}

	// Set as boot partition
	SoftWatchdog::feed(&_swd_handle);
	esp_ret = esp_ota_set_boot_partition(ota_partition);
	if(esp_ret != ESP_OK)
	{
		snprintf(write_err, sizeof(write_err), "Could not set OTA as boot partition: %s", esp_err_to_name(esp_ret));
		ESP_LOGE(TAG, "%s", write_err);
		publish_ota_failed(write_err, version_str);
		record_ota_failure(write_err);
		return RET_ERROR;
	}
	else
	{
		publish_update_ota_state("UPDATED", version_str);
		s_consecutive_ota_failures = 0;

		// Stop soft watchdog before graceful restart (which blocks up to 7s)
		// so it cannot bark during shutdown and mark a false crash on first boot.
		SoftWatchdog::stop(&_swd_handle);
		Utils::restart(Utils::RestartReason::OTA_UPDATE);
	}

	return RET_OK;
}

/******************************************************************************
* Clean up
******************************************************************************/
RetResult ota_cleanup()
{
	Tb::unsubscribe(TB_ATTRIBUTE_RESP_TOPIC);
	Tb::unsubscribe(OTA_TB_FW_RECV_TOPIC);

	return RET_OK;
}

/******************************************************************************
* Submit client attributes.
* Submitted attributes consist of:
* - One-time attributes: Will be submitted once on every boot and contain data
*   such as current fw version
* - Current settings
******************************************************************************/
RetResult submit_client_attr()
{
	StaticJsonDocument<TB_JSON_DOC_SIZE> json_doc;
	char json_buff[TB_JSON_BUFF_SIZE] = "";

	//
	// Submit one-time attributes
	// Submitted on first submit, ignores client attr interval
	if(!_one_time_attr_submitted)
	{
		ESP_LOGI(TAG, "Submitting one-time attributes.");
		TBJson::build_one_time_attr(&json_doc);

		serializeJson(json_doc, json_buff, sizeof(json_buff));

		if(Tb::publish(TB_ATTRIBUTE_PUBLISH_TOPIC, json_buff) > 0)
		{
			ESP_LOGI(TAG, "One-time attributes submitted successfully.");
			_one_time_attr_submitted = true;
		}
	}

	//
	// Time to submit current settings/status as client attributes
	//
	static int64_t last_submit_ms = 0;
	if(!(last_submit_ms == 0 || (Utils::get_tick_ms() - last_submit_ms) / 1000 >= SUBMIT_CLIENT_ATTR_INTERVAL_SEC))
	{
		return RET_OK;
	}

	json_doc.clear();

	TBJson::build_client_attr(&json_doc);

	serializeJson(json_doc, json_buff, sizeof(json_buff));

	printf("%s\n", json_buff);

	if(Tb::publish(TB_ATTRIBUTE_PUBLISH_TOPIC, json_buff) > 0)
	{
		ESP_LOGI(TAG, "Client attributes submitted successfully.");
		last_submit_ms = Utils::get_tick_ms();
	}
	else
	{
		return RET_ERROR;
	}

	return RET_OK;
}

/******************************************************************************
* Handle remote settings
* Get config from TB shared attributes and apply
******************************************************************************/
RetResult handle_remote_settings()
{
	static int64_t last_rs_check_ms = 0;
	if(last_rs_check_ms != 0 && (Utils::get_tick_ms() - last_rs_check_ms) / 1000 < SUBMIT_REMOTE_SETTINGS_INTERVAL_SEC)
	{
		return RET_OK;
	}

	RetResult ret = RET_ERROR;

	ESP_LOGI(TAG, "Handling remote settings");

	ESP_LOGI(TAG, "Subscribing to attr topic");
	// Subscribe to attribute response topic
	if(Tb::subscribe(TB_ATTRIBUTE_RESP_TOPIC) < 1)
	{
		ESP_LOGE(TAG, "Could not subscribe to attribute resp topic.");
		return RET_ERROR;
	}

	bool nv_station_configured = Settings::cloud_ws_profile_ready() || Settings::local_ws_profile_ready();
	const char *req_json = nv_station_configured ? RS_TB_ATTRIBUTE_REQ_JSON : RS_TB_ATTRIBUTE_REQ_FALLBACK_JSON;

	ESP_LOGI(TAG, "Publishing (%s)", nv_station_configured ? "shared only" : "wiped NV fallback with clientKeys");
	// Publish OTA attributes request and wait
	if(Tb::publish(TB_ATTRIBUTE_REQ_TOPIC, (char*)req_json) < 0)
	{
		ESP_LOGE(TAG, "Publish attribute req.");
		Tb::unsubscribe(TB_ATTRIBUTE_RESP_TOPIC);
		return RET_ERROR;
	}

	ESP_LOGI(TAG, "Waiting response");
	ret = Tb::wait_response((char*)TB_ATTRIBUTE_WAIT_TOPIC, &_mqtt_resp, TB_MQTT_DEFAULT_WAIT_RESP_TIMEOUT_MS);
	if(ret != RET_OK)
	{
		ESP_LOGE(TAG, "Could not get remote settings attributes.");
		Tb::unsubscribe(TB_ATTRIBUTE_RESP_TOPIC);
		return RET_ERROR;
	}

	ESP_LOGI(TAG, "Got response");
	Tb::unsubscribe(TB_ATTRIBUTE_RESP_TOPIC);

	printf("Current settings:\n");
	Settings::print_cur_settings();

	printf("================================\n");
	printf("Remote Settings JSON\n");
	printf("================================\n");
	printf("%.*s", _mqtt_resp.data_len, _mqtt_resp.data);
	printf("\n");
	printf("================================\n");

	// Parse JSON
	StaticJsonDocument<TB_JSON_DOC_SIZE> attr_doc;
	DeserializationError error = deserializeJson(attr_doc, _mqtt_resp.data, _mqtt_resp.data_len);
	if(error != DeserializationError::Ok)
	{
		ESP_LOGE(TAG, "Could not parse RS attr json: %s", error.c_str());
		return RET_ERROR;
	}

	// Mark check timestamp only after a valid attribute response is received
	last_rs_check_ms = Utils::get_tick_ms();

	JsonObject shared_attr = attr_doc.containsKey(TB_KEY_SHARED_ATTRIBUTES) 
		? attr_doc[TB_KEY_SHARED_ATTRIBUTES].as<JsonObject>() 
		: (attr_doc.containsKey("shared") ? attr_doc["shared"].as<JsonObject>() : attr_doc.as<JsonObject>());

	JsonObject client_attr = attr_doc.containsKey("client")
		? attr_doc["client"].as<JsonObject>()
		: JsonObject();

	//
	// Handle remote settings
	//

	if (!Settings::get_cloud_pairing_enabled()) {
		ESP_LOGI(TAG, "Cloud pairing is not active; caching the assignment without changing the local profile.");
	}

	// Debug mode enabled
	if(shared_attr.containsKey(RS_KEY_DEBUG_VERBOSITY))
	{
		u32 debug_verb = (u32)shared_attr[RS_KEY_DEBUG_VERBOSITY];
		if(Settings::get_debug_mode_verbosity() != (Log::Level) debug_verb)
		{
			Settings::set_debug_mode_verbosity((Log::Level) debug_verb);
		}
	}
	else
	{
		Settings::set_debug_mode_verbosity(Log::Level::NONE);
		ESP_LOGW(TAG, "No %s key.", RS_KEY_DEBUG_VERBOSITY);
	}

	// Cloud station model. Cache it in every mode so CLOUD remains a reversible
	// selection after a local SCAN pairing.
	if(shared_attr.containsKey(RS_KEY_WS_MODEL) &&
	   shared_attr[RS_KEY_WS_MODEL].is<const char*>())
	{
		const char* ws_model_str = shared_attr[RS_KEY_WS_MODEL];
		if(strncmp(ws_model_str, RS_VALUE_WS_MODEL_WS1001, strlen(RS_VALUE_WS_MODEL_WS1001)) == 0)
		{
			Settings::set_cloud_ws_type(WsType::WS_TYPE_1001);
		}
		else if(strncmp(ws_model_str, RS_VALUE_WS_MODEL_WS1300, strlen(RS_VALUE_WS_MODEL_WS1300)) == 0)
		{
			Settings::set_cloud_ws_type(WsType::WS_TYPE_1300);
		}
		else
		{
			ESP_LOGE(TAG, "Failed to parse %s", RS_KEY_WS_MODEL);
		}
	}
	else
	{
		ESP_LOGW(TAG, "No %s key.", RS_KEY_WS_MODEL);
	}

	// Ws Freq
	if(shared_attr.containsKey(RS_KEY_WS_FREQ_NAME))
	{
		const char* ws_freq_str = shared_attr[RS_KEY_WS_FREQ_NAME];
		if(strncmp(ws_freq_str, RS_VALUE_WS_FREQ_EU868, strlen(RS_VALUE_WS_FREQ_EU868)) == 0)
		{
			Settings::set_freq_region(WsFreqRegion::WS_FREQ_REGION_EU868);
		}
		else if(strncmp(ws_freq_str, RS_VALUE_WS_FREQ_US915, strlen(RS_VALUE_WS_FREQ_US915)) == 0)
		{
			Settings::set_freq_region(WsFreqRegion::WS_FREQ_REGION_US915);
		}
		else if(strncmp(ws_freq_str, RS_VALUE_WS_FREQ_AS923, strlen(RS_VALUE_WS_FREQ_AS923)) == 0)
		{
			Settings::set_freq_region(WsFreqRegion::WS_FREQ_REGION_AS923);
		}
		else if(strncmp(ws_freq_str, RS_VALUE_WS_FREQ_AU915, strlen(RS_VALUE_WS_FREQ_AU915)) == 0)
		{
			Settings::set_freq_region(WsFreqRegion::WS_FREQ_REGION_AU915);
		}
		else if(strncmp(ws_freq_str, RS_VALUE_WS_FREQ_IN865, strlen(RS_VALUE_WS_FREQ_IN865)) == 0)
		{
			Settings::set_freq_region(WsFreqRegion::WS_FREQ_REGION_IN865);
		}
		else
		{
			ESP_LOGE(TAG, "Failed to parse %s (%s)", RS_KEY_WS_FREQ_NAME, ws_freq_str);
		}
	}
	else
	{
		ESP_LOGW(TAG, "No %s key.", RS_KEY_WS_FREQ_NAME);
	}

	// A WS1300 serial is ten bytes / twenty hexadecimal characters. Keep serial
	// keys and numeric-ID aliases separate: treating an arbitrary serial as a
	// decimal string used to turn "39FA23..." into decimal 39 (0x27).
	bool cloud_identity_parsed = false;
	const char* serial_keys[] = {RS_KEY_WS_SN, "ws_sn"};
	for(const char *key : serial_keys)
	{
		if(!shared_attr.containsKey(key))
		{
			continue;
		}
		uint8_t cloud_sn[WS_SN_LEN] = {};
		uint32_t cloud_id = 0;
		const char *serial_text = shared_attr[key].is<const char*>()
			? shared_attr[key].as<const char*>() : nullptr;
		if(parse_cloud_station_serial(serial_text, cloud_sn, &cloud_id))
		{
			Settings::set_cloud_ws_type(WsType::WS_TYPE_1300);
			Settings::set_cloud_ws_sn(cloud_sn, sizeof(cloud_sn));
			Settings::set_cloud_ws_id((int64_t)cloud_id);
			Settings::set_cloud_ws_valid(true);
			if(!Settings::local_ws_profile_ready())
			{
				Settings::set_pair_mode(PairMode::PAIR_MODE_CLOUD);
				Settings::set_pair_source(PairSource::PAIR_SOURCE_CLOUD);
				Settings::set_ws_type(WsType::WS_TYPE_1300);
				Settings::set_ws_sn(cloud_sn, sizeof(cloud_sn));
				Settings::set_ws_id((int64_t)cloud_id);
			}
			ESP_LOGI(TAG, "Parsed cloud station serial key %s: ID=0x%08X", key,
					 (unsigned int)cloud_id);
			cloud_identity_parsed = true;
			break;
		}
		ESP_LOGE(TAG, "Invalid cloud station serial in key %s (expected %u hex chars)",
				 key, (unsigned int)(WS_SN_LEN * 2));
	}

	if(!cloud_identity_parsed)
	{
		const char* id_keys[] = {RS_KEY_WS_ID, TB_ATTR_WS_ID, "ws_fo_id", "ws_id"};

		// 1. If NV was not configured with any station profile, OR cloud profile is not ready, check client attributes first
		if(!nv_station_configured || !Settings::cloud_ws_profile_ready())
		{
			for(const char *key : id_keys)
			{
				if(!client_attr.containsKey(key))
				{
					continue;
				}
				uint32_t cloud_id = 0;
				if(parse_cloud_station_id(client_attr[key], &cloud_id) && cloud_id > 0 && cloud_id != UINT32_MAX)
				{
					Settings::set_cloud_ws_type(WsType::WS_TYPE_1001);
					Settings::set_cloud_ws_id((int64_t)cloud_id);
					Settings::set_cloud_ws_valid(true);
					if(!Settings::local_ws_profile_ready())
					{
						Settings::set_pair_mode(PairMode::PAIR_MODE_CLOUD);
						Settings::set_pair_source(PairSource::PAIR_SOURCE_CLOUD);
						// Also mirror to local ws_* so offline fallback is ready
						Settings::set_ws_type(WsType::WS_TYPE_1001);
						Settings::set_ws_id((int64_t)cloud_id);
					}
					ESP_LOGI(TAG, "Restored WS1001 cloud pairing from client attribute key %s: 0x%08X", key,
							 (unsigned int)cloud_id);
					cloud_identity_parsed = true;
					break;
				}
			}
		}

		// 2. Check shared attributes for explicitly configured station ID
		if(!cloud_identity_parsed)
		{
			for(const char *key : id_keys)
			{
				if(!shared_attr.containsKey(key))
				{
					continue;
				}
				uint32_t cloud_id = 0;
				if(parse_cloud_station_id(shared_attr[key], &cloud_id) && cloud_id > 0 && cloud_id != UINT32_MAX)
				{
					if(Settings::get_cloud_ws_type() == WsType::WS_TYPE_NONE)
					{
						Settings::set_cloud_ws_type(WsType::WS_TYPE_1001);
					}
					Settings::set_cloud_ws_id((int64_t)cloud_id);
					Settings::set_cloud_ws_valid(true);
					if(!Settings::local_ws_profile_ready())
					{
						Settings::set_pair_mode(PairMode::PAIR_MODE_CLOUD);
						Settings::set_pair_source(PairSource::PAIR_SOURCE_CLOUD);
						Settings::set_ws_type(Settings::get_cloud_ws_type());
						Settings::set_ws_id((int64_t)cloud_id);
					}
					ESP_LOGI(TAG, "Parsed cloud station ID key %s: 0x%08X", key,
							 (unsigned int)cloud_id);
					cloud_identity_parsed = true;
					break;
				}
				ESP_LOGE(TAG, "Invalid cloud station ID in key %s", key);
			}
		}
	}

	if(!cloud_identity_parsed)
	{
		ESP_LOGW(TAG, "No valid cloud station identity received; preserving cached assignment");
	}
	else if(WsHandler::get_pair_state() != WsHandler::PairState::PAIRED &&
	        !WsHandler::is_scan_active() &&
	        WsHandler::get_fallback_phase() != WsHandler::AutoFallbackPhase::PHASE_MANUAL)
	{
		if((Settings::get_pair_mode() == PairMode::PAIR_MODE_CLOUD || !nv_station_configured) && Settings::cloud_ws_profile_ready())
		{
			ESP_LOGI(TAG, "Re-applying cloud pair mode for station ID 0x%08X", (unsigned int)Settings::get_cloud_ws_id());
			WsHandler::apply_pair_mode(PairMode::PAIR_MODE_CLOUD, (uint32_t)Settings::get_cloud_ws_id(), -1);
		}
		else if(Settings::get_pair_mode() == PairMode::PAIR_MODE_LOCAL && Settings::local_ws_profile_ready())
		{
			ESP_LOGI(TAG, "Re-applying local pair mode for station ID 0x%08X", (unsigned int)Settings::get_ws_id());
			WsHandler::apply_pair_mode(PairMode::PAIR_MODE_LOCAL, (uint32_t)Settings::get_ws_id(), -1);
		}
	}

	// Remote scan trigger via ThingsBoard shared attribute (rs_scan)
	if(shared_attr.containsKey(RS_KEY_SCAN))
	{
		uint64_t req_id = 0;
		bool armed = false;
		WsType scan_target_type = WsType::WS_TYPE_NONE;

		if(shared_attr[RS_KEY_SCAN].is<bool>())
		{
			if(shared_attr[RS_KEY_SCAN].as<bool>())
			{
				req_id = 1;
				armed = true;
			}
			else
			{
				// Operator reset attribute to false: clear stored ID so a future boolean true can trigger again
				Settings::set_last_scan_id(0);
				Settings::commit_blocking();
			}
		}
		else if(shared_attr[RS_KEY_SCAN].is<int>())
		{
			int val = shared_attr[RS_KEY_SCAN].as<int>();
			if(val == 1001)
			{
				req_id = 1;
				armed = true;
				scan_target_type = WsType::WS_TYPE_1001;
			}
			else if(val == 1300)
			{
				req_id = 1;
				armed = true;
				scan_target_type = WsType::WS_TYPE_1300;
			}
			else if(val > 0)
			{
				req_id = (uint64_t)val;
				armed = true;
			}
			else
			{
				Settings::set_last_scan_id(0);
				Settings::commit_blocking();
			}
		}
		else if(shared_attr[RS_KEY_SCAN].is<const char*>())
		{
			const char *v = shared_attr[RS_KEY_SCAN].as<const char*>();
			if(v != nullptr)
			{
				if(strcasecmp(v, "1001") == 0 || strcasecmp(v, "ws1001") == 0)
				{
					req_id = 1;
					armed = true;
					scan_target_type = WsType::WS_TYPE_1001;
				}
				else if(strcasecmp(v, "1300") == 0 || strcasecmp(v, "ws1300") == 0)
				{
					req_id = 1;
					armed = true;
					scan_target_type = WsType::WS_TYPE_1300;
				}
				else if(strcasecmp(v, "all") == 0 || strcasecmp(v, "combo") == 0)
				{
					req_id = 1;
					armed = true;
					scan_target_type = WsType::WS_TYPE_ALL;
				}
				else if(strcasecmp(v, "true") == 0 || strcmp(v, "1") == 0 || strcasecmp(v, "scan") == 0)
				{
					req_id = 1;
					armed = true;
				}
				else if(strcasecmp(v, "false") == 0 || strcmp(v, "0") == 0)
				{
					Settings::set_last_scan_id(0);
					Settings::commit_blocking();
				}
				else
				{
					req_id = strtoull(v, nullptr, 10);
					armed = (req_id > 0);
				}
			}
		}

		uint64_t last_id = Settings::get_last_scan_id();
		if(armed && req_id > last_id)
		{
			if(scan_target_type == WsType::WS_TYPE_NONE)
			{
				WsType conf_type = Settings::get_ws_type();
				scan_target_type = (conf_type == WsType::WS_TYPE_1300) ? WsType::WS_TYPE_1300
								 : ((conf_type == WsType::WS_TYPE_ALL) ? WsType::WS_TYPE_ALL
								 : WsType::WS_TYPE_1001);
			}
			ESP_LOGW(TAG, "Remote forced scan requested via ThingsBoard shared attribute %s (id: %llu > last: %llu, target profile: %d)",
					 RS_KEY_SCAN, (unsigned long long)req_id, (unsigned long long)last_id, (int)scan_target_type);
			Settings::set_last_scan_id(req_id);
			Settings::commit_blocking();
			WsHandler::apply_pair_mode(PairMode::PAIR_MODE_SCAN, 0, -1, scan_target_type);
		}
	}

	// Force remote reboot trigger via ThingsBoard shared attribute (rs_reboot)
	if(shared_attr.containsKey(RS_KEY_REBOOT))
	{
		uint64_t req_id = 0;
		bool armed = false;

		if(shared_attr[RS_KEY_REBOOT].is<uint64_t>())
		{
			req_id = shared_attr[RS_KEY_REBOOT].as<uint64_t>();
			armed = (req_id > 0);
		}
		else if(shared_attr[RS_KEY_REBOOT].is<int64_t>())
		{
			int64_t val = shared_attr[RS_KEY_REBOOT].as<int64_t>();
			if(val > 0)
			{
				req_id = (uint64_t)val;
				armed = true;
			}
		}
		else if(shared_attr[RS_KEY_REBOOT].is<int>())
		{
			int val = shared_attr[RS_KEY_REBOOT].as<int>();
			if(val > 0)
			{
				req_id = (uint64_t)val;
				armed = true;
			}
		}
		else if(shared_attr[RS_KEY_REBOOT].is<bool>())
		{
			bool val = shared_attr[RS_KEY_REBOOT].as<bool>();
			if(val)
			{
				req_id = 1;
				armed = true;
			}
			else
			{
				// Operator reset attribute to false: clear stored ID so a future boolean true can trigger again
				Settings::set_last_reboot_id(0);
				Settings::commit_blocking();
			}
		}
		else if(shared_attr[RS_KEY_REBOOT].is<const char*>())
		{
			const char *v = shared_attr[RS_KEY_REBOOT].as<const char*>();
			if(v != nullptr)
			{
				if(strcasecmp(v, "true") == 0)
				{
					req_id = 1;
					armed = true;
				}
				else if(strcasecmp(v, "false") == 0 || strcmp(v, "0") == 0)
				{
					Settings::set_last_reboot_id(0);
					Settings::commit_blocking();
				}
				else
				{
					req_id = strtoull(v, nullptr, 10);
					armed = (req_id > 0);
				}
			}
		}

		uint64_t last_id = Settings::get_last_reboot_id();
		if(armed && req_id > last_id)
		{
			ESP_LOGW(TAG, "Remote reboot requested via ThingsBoard shared attribute %s (id: %llu > last: %llu). Restarting device...",
					 RS_KEY_REBOOT, (unsigned long long)req_id, (unsigned long long)last_id);
			Settings::set_last_reboot_id(req_id);
			Settings::commit_blocking();
			SoftWatchdog::stop(&_swd_handle);
			vTaskDelay(pdMS_TO_TICKS(500));
			Utils::safe_restart(Utils::RestartReason::REMOTE_REBOOT);
		}
	}

	if(shared_attr.containsKey(RS_KEY_WS_PUBKEY))
	{
		const char* ws_pubkey_str = shared_attr[RS_KEY_WS_PUBKEY].is<const char*>()
			? shared_attr[RS_KEY_WS_PUBKEY].as<const char*>() : nullptr;
		uint8_t cloud_pubkey[WS_PUB_KEY_LEN] = {0};
		if(ws_pubkey_str != nullptr &&
		   Utils::hex_str_to_byte_array(ws_pubkey_str, cloud_pubkey,
									   sizeof(cloud_pubkey)) == RetResult::RET_OK)
		{
			Settings::set_cloud_ws_pubkey(cloud_pubkey, sizeof(cloud_pubkey));
		}
		else
		{
			ESP_LOGE(TAG, "Failed to parse %s", RS_KEY_WS_PUBKEY);
		}
	}

	// Device name
	// containsKey() only proves the key is present - it says nothing about the type.
	// A JSON null, number or object yields nullptr from as<const char*>(), which used
	// to go straight into strlen() inside set_device_name(). Check the type here and
	// let set_device_name() reject empty/oversized values, so a partial or malformed
	// attribute sync cannot wipe a good name out of NVS.
	if(shared_attr[RS_KEY_DEV_NAME].is<const char*>())
	{
		const char* dev_name_str = shared_attr[RS_KEY_DEV_NAME];
		if(Settings::set_device_name(dev_name_str) == RET_OK)
		{
			ESP_LOGI(TAG, "Device name updated to '%s'", dev_name_str);
		}
	}
	else if(shared_attr.containsKey(RS_KEY_DEV_NAME))
	{
		ESP_LOGW(TAG, "Key %s present but not a string, ignoring", RS_KEY_DEV_NAME);
	}
	else
	{
		ESP_LOGW(TAG, "No %s key", RS_KEY_DEV_NAME);
	}

	Settings::commit();

	GlobalEvents::RmDataUpdatedEvent rm_event;
	if(shared_attr.containsKey(RS_KEY_RM_RUN_TS))
	{
		uint64_t ts = shared_attr[RS_KEY_RM_RUN_TS].as<uint64_t>();
		if(ts > 0xFFFFFFFF)
		{
			ts /= 1000;
		}
		rm_event.run_ts = ts;
	}
	else
	{
		rm_event.run_ts = 0;
		ESP_LOGW(TAG, "No %s key", RS_KEY_RM_RUN_TS);
	}
	GlobalEvents::publish(GlobalEvents::EventId::RM_TS_DATA_UPDATED, &rm_event, sizeof(rm_event));

	return RET_OK;
}

/******************************************************************************
* Submit logs when debug mode enabled
******************************************************************************/
void submit_logs()
{
	if(Settings::get_debug_mode_verbosity() == Log::Level::NONE)
	{
		return;
	}

	ESP_LOGW(TAG, "Submitting logs");

	Log::Entry entry = {};

	StaticJsonDocument<TB_JSON_DOC_SIZE> json_doc;

	uint32_t entries_total = Log::get_count();

	while(Log::pop(&entry) == RET_OK && entries_total > 0)
	{
		SoftWatchdog::feed(&_swd_handle);
		json_doc.template to<JsonArray>();
		TBJson::append_from_log_entry((Log::Entry*)&entry, &json_doc);

		char json_buff[TB_JSON_BUFF_SIZE] = "";
		serializeJsonPretty(json_doc, json_buff, sizeof(json_buff));
		Tb::publish_telemetry(json_buff);

		json_doc.clear();

		printf("Submitting log: \n%s\n", json_buff);
		entries_total--;
	}
}

/******************************************************************************
* Submit task
******************************************************************************/
void submit_task(void *param)
{
	ESP_LOGW(TAG, "Submit task");

	// char msg[40] = "";
	// snprintf(msg, sizeof(msg), "Test log msg: %d", (int)(Utils::get_tick_ms() / 1000));
	// printf("Msg: %s", msg);
	// Log::log(msg);

	SoftWatchdog::start(&_swd_handle);

	// Pre-Submit Collision Guard: Check if we are near the station's LoRa packet arrival
	// or in the post-packet mesh cooldown window, and defer into the safe quiet window.
	uint32_t defer_ms = 0;
	if (WsHandler::is_in_station_packet_guard_window(&defer_ms) && defer_ms > 0)
	{
		ESP_LOGI(TAG, "Near station packet window, deferring submit by %u ms into quiet window", (unsigned)defer_ms);
		SoftWatchdog::feed(&_swd_handle);
		vTaskDelay(pdMS_TO_TICKS(defer_ms));
		SoftWatchdog::feed(&_swd_handle);
	}

	if(Tb::is_connected())
	{
		// Sync time expired
		// if(RTC::sync_expired())
		// {
		// 	ESP_LOGW(TAG, "RTC time expired, resyncing.");
		// 	RTC::sync_from_ntp();
		// }

		// Log WiFi RSSI
		Log::log(Log::Code::WIFI_RSSI, Wifi::get_rssi());
		SoftWatchdog::feed(&_swd_handle);

		// Handle OTA
		handle_ota();
		ota_cleanup();
		SoftWatchdog::feed(&_swd_handle);

		// Handle remote config
		handle_remote_settings();
		SoftWatchdog::feed(&_swd_handle);

		// Client attributes
		submit_client_attr();
		SoftWatchdog::feed(&_swd_handle);

		// Confirm boot healthy once connected to ThingsBoard and before shipping logs
		BootGuard::mark_healthy();

		// Submit logs
		submit_logs();
		SoftWatchdog::feed(&_swd_handle);

		// Submit weather station data
		submit_ws_data();
		SoftWatchdog::feed(&_swd_handle);

		// Submit forwarded remote weather station data
		submit_remote_ws_data();
		SoftWatchdog::feed(&_swd_handle);

		// Submit GPS data
		submit_gps_data();
		SoftWatchdog::feed(&_swd_handle);
	}
	else
	{
		ESP_LOGE(TAG, "TB not connected, submit failed.");
	}

	SoftWatchdog::stop(&_swd_handle);

	if(_autostart_en)
	{
		set_timer();
	}

	ESP_LOGI(TAG, "Submit task high watermark: %d", uxTaskGetStackHighWaterMark(NULL));

	_submit_task_handle = NULL;
	vTaskDelete(NULL);
}

}
