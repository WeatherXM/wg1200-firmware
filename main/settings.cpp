/******************************************************************************
* @brief Device configuration provided by UI or via remote config.
* Settings are stored in NVS in independent key/vals instead of a single
* structure, this way backwards compatibility is preserved when new settings
* are added in newer versions.
* On load() all keys are loaded into a Settings::Data struct. If a key doesn't
* exist yet, a default value is loaded from struct DEFAULT_SETTINGS.
* When a value is set with any of the setter functions, it is set in the
* _current_settings struct. To persist these settings commit() must be called.
* Settings returned by getters must be valid/sanitized.
******************************************************************************/
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_attr.h"
#include "string.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "app_config.h"
#include "const.h"
#include "common.h"
#include "settings.h"
#include "units.h"
#include "log.h"
#include "utils.h"
#include "tb.h"
#include "env.h"
#include "backlight.h"
#include "ws/include/ws_handler.h"
#include "ws/include/ws_base.h"
#include "global_events.h"

namespace Settings
{
	//
	// Local vars
	//
	const char TAG[] = "Settings";

	/** Current settings loaded from NVS on init. */
	Data _current_settings = {};

	/** Shadow copy of settings currently synced to NVS, used for delta-only commits. */
	static Data _nvs_synced_settings = {};
	static bool _nvs_synced_valid = false;

	/** Retain active LCD tab index in RTC_NOINIT_ATTR SRAM across warm reboots/OTA/USB flashes
	 *  so browsing tabs on the LCD never wears NVS flash. */
	static constexpr uint32_t RTC_LAST_TAB_MAGIC = 0x4C544142U; // "LTAB"
	struct RtcLastTabCache
	{
		uint32_t magic;
		uint32_t last_tab;
		uint32_t inv_last_tab;
	};
	static RTC_NOINIT_ATTR RtcLastTabCache _rtc_last_tab_cache;

	/** Commit task handle */
	static TaskHandle_t _commit_task_handle = NULL;

	static SemaphoreHandle_t _commit_done_sem = NULL;
	static SemaphoreHandle_t _commit_mutex = NULL;
	static RetResult _last_commit_result = RET_ERROR;

	GlobalEvents::SettingsChangedEvent _changed_categories = 0;

	static bool profile_bytes_ready(const u8 *buff, size_t len)
	{
		bool all_zero = true;
		bool all_ff = true;
		for(size_t i = 0; i < len; i++)
		{
			all_zero = all_zero && (buff[i] == 0x00);
			all_ff = all_ff && (buff[i] == 0xFF);
		}
		return !all_zero && !all_ff;
	}

	static bool profile_id_matches_sn(int64_t id, const u8 *sn)
	{
		if(id < 0 || id > UINT32_MAX || sn == nullptr)
		{
			return false;
		}
		uint32_t sn_id = ((uint32_t)sn[0] << 24) |
						 ((uint32_t)sn[1] << 16) |
						 ((uint32_t)sn[2] << 8) |
						 (uint32_t)sn[3];
		return (uint32_t)id == sn_id;
	}

	//
	// Local functions
	//
	void commit_task(void *param);
	RetResult commit_safe();

	/******************************************************************************
	* Init
	******************************************************************************/
	RetResult init()
	{
		_commit_done_sem = xSemaphoreCreateBinary();
		_commit_mutex = xSemaphoreCreateMutex();

		// Create commit task
		BaseType_t ret = xTaskCreatePinnedToCore(&commit_task, SETTINGS_COMMIT_TASK_NAME, SETTINGS_COMMIT_TASK_STACK_SIZE,		
			NULL, SETTINGS_COMMIT_TASK_PRIORITY, &_commit_task_handle, 1);	

		if(_commit_task_handle == NULL)
		{
			ESP_LOGE(TAG, "Could not create settings commit task.");
			return RET_ERROR;
		}

		if(load() == RET_OK)
			Log::log(Log::Code::SETTINGS_LOADED);
		else	
			Log::log(Log::Code::SETTINGS_LOAD_FAILED);

		return RET_OK;
	}

	/******************************************************************************
	* Load current settings from NVS to struct
	******************************************************************************/
	RetResult load()
	{
		// Load default settings
		memcpy(&_current_settings, &DEFAULT_SETTINGS, sizeof(_current_settings));

		nvs_handle_t nvs_handle = NULL;
		esp_err_t esp_ret = NULL;

		//
		// Load values one by one from NVS
		//
		// Opening namespace fails and an error is returned, however settings now
		// are set to default so default values will be returned by get functions
		esp_ret = nvs_open(NVS_SETTINGS_NAMESPACE, NVS_READWRITE, &nvs_handle);
		ASSERT_ESP_RET(esp_ret, "Could not open NVS");

		bool needs_commit = false;

		// Freq name
		esp_ret = nvs_get_u32(nvs_handle, NVS_SETTINGS_KEY_FREQ_ID, (u32*)&_current_settings.freq_region);
		ESP_ERROR_CHECK_WITHOUT_ABORT(esp_ret);
		if (esp_ret == ESP_ERR_NVS_NOT_FOUND) needs_commit = true;

		const auto *freq_desc = WsHandler::get_freq_desc(nullptr, _current_settings.freq_region);
		// Check if valid, if not use default
		if(freq_desc == nullptr)
		{
			ESP_LOGE(TAG, "Invalid frequency, using default.");
			_current_settings.freq_region = DEFAULT_SETTINGS.freq_region;
			needs_commit = true;
		}

		// Temperature unit
		esp_ret = nvs_get_u32(nvs_handle, NVS_SETTINGS_KEY_TEMP_UNIT, (u32*)&_current_settings.temp_unit);
		ESP_ERROR_CHECK_WITHOUT_ABORT(esp_ret);
		if (esp_ret == ESP_ERR_NVS_NOT_FOUND) needs_commit = true;

		// Timezone offset
		esp_ret = nvs_get_u32(nvs_handle, NVS_SETTINGS_KEY_TZ_OFFSET, (u32*)&_current_settings.tz_offset);
		ESP_ERROR_CHECK_WITHOUT_ABORT(esp_ret);
		if (esp_ret == ESP_ERR_NVS_NOT_FOUND) needs_commit = true;

		// Wind speed unit
		esp_ret = nvs_get_u32(nvs_handle, NVS_SETTINGS_KEY_WIND_SPEED_UNIT, (u32*)&_current_settings.wind_speed_unit);
		ESP_ERROR_CHECK_WITHOUT_ABORT(esp_ret);
		if (esp_ret == ESP_ERR_NVS_NOT_FOUND) needs_commit = true;

		// Rain unit
		esp_ret = nvs_get_u32(nvs_handle, NVS_SETTINGS_KEY_RAIN_UNIT, (u32*)&_current_settings.precip_unit);
		ESP_ERROR_CHECK_WITHOUT_ABORT(esp_ret);
		if (esp_ret == ESP_ERR_NVS_NOT_FOUND) needs_commit = true;

		// Pressure unit
		esp_ret = nvs_get_u32(nvs_handle, NVS_SETTINGS_KEY_PRESSURE_UNIT, (u32*)&_current_settings.pressure_unit);
		ESP_ERROR_CHECK_WITHOUT_ABORT(esp_ret);
		if (esp_ret == ESP_ERR_NVS_NOT_FOUND) needs_commit = true;

		// Idle backlight level
		esp_ret = nvs_get_u32(nvs_handle, NVS_SETTINGS_KEY_IDLE_BACKLIGHT_LEVEL, (u32*)&_current_settings.idle_backlight_level);
		ESP_ERROR_CHECK_WITHOUT_ABORT(esp_ret);
		if (esp_ret == ESP_ERR_NVS_NOT_FOUND) needs_commit = true;

		// Screen rotation
		esp_ret = nvs_get_u32(nvs_handle, NVS_SETTINGS_KEY_SCREEN_ROTATION, (u32*)&_current_settings.scr_rot);
		ESP_ERROR_CHECK_WITHOUT_ABORT(esp_ret);
		if (esp_ret == ESP_ERR_NVS_NOT_FOUND) needs_commit = true;

        // Debug build MQTT host
        esp_ret = nvs_get_u32(nvs_handle, NVS_SETTINGS_KEY_MQTT_BROKER, (u32*)&_current_settings.mqtt_broker_id);
		ESP_ERROR_CHECK_WITHOUT_ABORT(esp_ret);
		if (esp_ret == ESP_ERR_NVS_NOT_FOUND) needs_commit = true;

		// MQTT Disabled
		u8 mqtt_disabled_u8 = 0;
		esp_ret = nvs_get_u8(nvs_handle, NVS_SETTINGS_KEY_MQTT_DISABLED, &mqtt_disabled_u8);
		ESP_ERROR_CHECK_WITHOUT_ABORT(esp_ret);
		if (esp_ret == ESP_OK) {
			_current_settings.mqtt_disabled = (mqtt_disabled_u8 != 0);
		} else if (esp_ret == ESP_ERR_NVS_NOT_FOUND) {
			needs_commit = true;
		}
		if (!is_open_edition()) {
			_current_settings.mqtt_disabled = false;
		}

		// Cloud Pairing Enabled
		u8 cloud_pairing_enabled_u8 = 1;
		esp_ret = nvs_get_u8(nvs_handle, NVS_SETTINGS_KEY_CLOUD_PAIRING_ENABLED, &cloud_pairing_enabled_u8);
		ESP_ERROR_CHECK_WITHOUT_ABORT(esp_ret);
		if (esp_ret == ESP_OK) {
			_current_settings.cloud_pairing_enabled = (cloud_pairing_enabled_u8 != 0);
		} else if (esp_ret == ESP_ERR_NVS_NOT_FOUND) {
			_current_settings.cloud_pairing_enabled = true;
			needs_commit = true;
		}
		
		// Debug enabled
		esp_ret = nvs_get_u32(nvs_handle, NVS_SETTINGS_DEBUG_VERBOSITY, (u32*)&_current_settings.debug_verbosity);
		ESP_ERROR_CHECK_WITHOUT_ABORT(esp_ret);
		if (esp_ret == ESP_ERR_NVS_NOT_FOUND) needs_commit = true;

		// Last on screen tab (retained in RTC_NOINIT_ATTR across warm resets; optional NVS fallback)
		if (_rtc_last_tab_cache.magic == RTC_LAST_TAB_MAGIC &&
			_rtc_last_tab_cache.last_tab == ~_rtc_last_tab_cache.inv_last_tab) {
			_current_settings.last_tab = _rtc_last_tab_cache.last_tab;
		} else {
			u32 nvs_last_tab = _current_settings.last_tab;
			if (nvs_get_u32(nvs_handle, NVS_SETTINGS_LAST_TAB, &nvs_last_tab) == ESP_OK) {
				_current_settings.last_tab = nvs_last_tab;
			}
			_rtc_last_tab_cache.magic = RTC_LAST_TAB_MAGIC;
			_rtc_last_tab_cache.last_tab = _current_settings.last_tab;
			_rtc_last_tab_cache.inv_last_tab = ~_current_settings.last_tab;
		}

		// Weather station ID
		esp_ret = nvs_get_i64(nvs_handle, NVS_SETTINGS_WS_ID, (int64_t*)&_current_settings.ws_id);
		ESP_ERROR_CHECK_WITHOUT_ABORT(esp_ret);
		if (esp_ret == ESP_ERR_NVS_NOT_FOUND) needs_commit = true;

		// Weather station type
		esp_ret = nvs_get_u32(nvs_handle, NVS_SETTINGS_WS_TYPE, (u32*)&_current_settings.ws_type);
		ESP_ERROR_CHECK_WITHOUT_ABORT(esp_ret);
		if (esp_ret == ESP_ERR_NVS_NOT_FOUND) needs_commit = true;

		// Weather station public key
		size_t blob_size = sizeof(_current_settings.ws_pubkey);
		esp_ret = nvs_get_blob(nvs_handle, NVS_SETTINGS_WS_PUBKEY, _current_settings.ws_pubkey, &blob_size);
		ESP_ERROR_CHECK_WITHOUT_ABORT(esp_ret);
		if(esp_ret == ESP_OK) assert(sizeof(_current_settings.ws_pubkey) == blob_size);
		else if (esp_ret == ESP_ERR_NVS_NOT_FOUND) needs_commit = true;

		// Weather station serial number
		blob_size = sizeof(_current_settings.ws_sn);
		esp_ret = nvs_get_blob(nvs_handle, NVS_SETTINGS_WS_SN, _current_settings.ws_sn, &blob_size);
		ESP_ERROR_CHECK_WITHOUT_ABORT(esp_ret);
		if(esp_ret == ESP_OK) assert(sizeof(_current_settings.ws_sn) == blob_size);
		else if (esp_ret == ESP_ERR_NVS_NOT_FOUND) needs_commit = true;

		// Pairing mode. Migrate from legacy flags when the key does not exist
		// yet: cloud enabled -> CLOUD, paired -> LOCAL, else SCAN (offline-first:
		// an unprovisioned gateway must be able to find its station without cloud)
		u8 pair_mode_u8 = (u8)PairMode::PAIR_MODE_CLOUD;
		esp_ret = nvs_get_u8(nvs_handle, NVS_SETTINGS_KEY_PAIR_MODE, &pair_mode_u8);
		ESP_ERROR_CHECK_WITHOUT_ABORT(esp_ret);
		if (esp_ret == ESP_OK) {
			_current_settings.pair_mode = (PairMode)pair_mode_u8;
		} else if (esp_ret == ESP_ERR_NVS_NOT_FOUND) {
			if (_current_settings.cloud_pairing_enabled) {
				_current_settings.pair_mode = PairMode::PAIR_MODE_CLOUD;
			} else if (_current_settings.ws_id >= 0) {
				_current_settings.pair_mode = PairMode::PAIR_MODE_LOCAL;
			} else {
				_current_settings.pair_mode = PairMode::PAIR_MODE_SCAN;
			}
			ESP_LOGW(TAG, "Migrated legacy pairing flags to pair_mode=%d", (int)_current_settings.pair_mode);
			needs_commit = true;
		}

		u8 pair_source_u8 = (u8)PairSource::PAIR_SOURCE_CLOUD;
		esp_ret = nvs_get_u8(nvs_handle, NVS_SETTINGS_KEY_PAIR_SOURCE, &pair_source_u8);
		if(esp_ret == ESP_OK &&
		   (pair_source_u8 == (u8)PairSource::PAIR_SOURCE_CLOUD ||
		    pair_source_u8 == (u8)PairSource::PAIR_SOURCE_LOCAL))
		{
			_current_settings.pair_source = (PairSource)pair_source_u8;
		}
		else
		{
			_current_settings.pair_source =
				(_current_settings.pair_mode == PairMode::PAIR_MODE_CLOUD)
					? PairSource::PAIR_SOURCE_CLOUD
					: PairSource::PAIR_SOURCE_LOCAL;
			needs_commit = true;
		}

		// Local pair cloud-sync pending flag
		u8 lp_sync_pend_u8 = 0;
		esp_ret = nvs_get_u8(nvs_handle, NVS_SETTINGS_KEY_LOCAL_PAIR_SYNC_PENDING, &lp_sync_pend_u8);
		ESP_ERROR_CHECK_WITHOUT_ABORT(esp_ret);
		if (esp_ret == ESP_OK) {
			_current_settings.local_pair_sync_pending = (lp_sync_pend_u8 != 0);
		} else if (esp_ret == ESP_ERR_NVS_NOT_FOUND) {
			_current_settings.local_pair_sync_pending = false;
			needs_commit = true;
		}
		if(_current_settings.local_pair_sync_pending)
		{
			// Older firmware could ask a server-side rule to replace rs_ws_*
			// after a local scan. Profiles are independent now; silently
			// rewriting a cloud assignment is unsafe and non-reversible.
			_current_settings.local_pair_sync_pending = false;
			needs_commit = true;
			ESP_LOGW(TAG, "Cleared obsolete local-pair cloud adoption request");
		}

		// Cloud station attributes
		esp_ret = nvs_get_i64(nvs_handle, NVS_SETTINGS_KEY_CLOUD_WS_ID, (int64_t*)&_current_settings.cloud_ws_id);
		if (esp_ret == ESP_ERR_NVS_NOT_FOUND) needs_commit = true;

		u8 cloud_valid_u8 = 0;
		esp_ret = nvs_get_u8(nvs_handle, NVS_SETTINGS_KEY_CLOUD_WS_VALID, &cloud_valid_u8);
		if (esp_ret == ESP_OK) {
			_current_settings.cloud_ws_valid = (cloud_valid_u8 != 0);
		} else if (esp_ret == ESP_ERR_NVS_NOT_FOUND) {
			_current_settings.cloud_ws_valid = false;
			needs_commit = true;
		}

		bool cloud_type_missing = false;
		esp_ret = nvs_get_u32(nvs_handle, NVS_SETTINGS_KEY_CLOUD_WS_TYPE,
							 (u32*)&_current_settings.cloud_ws_type);
		if(esp_ret == ESP_ERR_NVS_NOT_FOUND)
		{
			cloud_type_missing = true;
			needs_commit = true;
		}

		bool cloud_sn_missing = false;
		blob_size = sizeof(_current_settings.cloud_ws_sn);
		esp_ret = nvs_get_blob(nvs_handle, NVS_SETTINGS_KEY_CLOUD_WS_SN,
							  _current_settings.cloud_ws_sn, &blob_size);
		if(esp_ret == ESP_OK)
		{
			if(blob_size != sizeof(_current_settings.cloud_ws_sn))
			{
				memset(_current_settings.cloud_ws_sn, 0, sizeof(_current_settings.cloud_ws_sn));
				cloud_sn_missing = true;
				needs_commit = true;
			}
		}
		else if(esp_ret == ESP_ERR_NVS_NOT_FOUND)
		{
			cloud_sn_missing = true;
			needs_commit = true;
		}

		bool cloud_pubkey_missing = false;
		blob_size = sizeof(_current_settings.cloud_ws_pubkey);
		esp_ret = nvs_get_blob(nvs_handle, NVS_SETTINGS_KEY_CLOUD_WS_PUBKEY,
							  _current_settings.cloud_ws_pubkey, &blob_size);
		if(esp_ret == ESP_OK)
		{
			if(blob_size != sizeof(_current_settings.cloud_ws_pubkey))
			{
				memset(_current_settings.cloud_ws_pubkey, 0, sizeof(_current_settings.cloud_ws_pubkey));
				cloud_pubkey_missing = true;
				needs_commit = true;
			}
		}
		else if(esp_ret == ESP_ERR_NVS_NOT_FOUND)
		{
			cloud_pubkey_missing = true;
			needs_commit = true;
		}

		// Migration from releases that persisted only c_ws_id/c_ws_valid while
		// keeping the active cloud credentials in ws_*.
		if(_current_settings.cloud_ws_valid &&
		   _current_settings.cloud_ws_id == _current_settings.ws_id)
		{
			if(cloud_type_missing)
			{
				_current_settings.cloud_ws_type = _current_settings.ws_type;
			}
			if(cloud_sn_missing)
			{
				memcpy(_current_settings.cloud_ws_sn, _current_settings.ws_sn,
					   sizeof(_current_settings.cloud_ws_sn));
			}
			if(cloud_pubkey_missing)
			{
				memcpy(_current_settings.cloud_ws_pubkey, _current_settings.ws_pubkey,
					   sizeof(_current_settings.cloud_ws_pubkey));
			}
		}

		// Migration from v0.6.1 / legacy firmware where cloud_ws_* keys did not exist,
		// but a weather station was already paired locally in ws_*.
		if(!_current_settings.cloud_ws_valid && _current_settings.cloud_ws_id < 0 && _current_settings.ws_id >= 0)
		{
			_current_settings.cloud_ws_id = _current_settings.ws_id;
			_current_settings.cloud_ws_type = _current_settings.ws_type != WsType::WS_TYPE_NONE ? _current_settings.ws_type : WsType::WS_TYPE_1001;
			memcpy(_current_settings.cloud_ws_sn, _current_settings.ws_sn, sizeof(_current_settings.cloud_ws_sn));
			memcpy(_current_settings.cloud_ws_pubkey, _current_settings.ws_pubkey, sizeof(_current_settings.cloud_ws_pubkey));
			_current_settings.cloud_ws_valid = true;
			_current_settings.pair_mode = PairMode::PAIR_MODE_CLOUD;
			_current_settings.pair_source = PairSource::PAIR_SOURCE_CLOUD;
			_current_settings.cloud_pairing_enabled = true;
			needs_commit = true;
			ESP_LOGI(TAG, "Migrated legacy paired station ws_id=0x%08llX (type %u) to cloud configuration",
					 (unsigned long long)_current_settings.ws_id, (unsigned int)_current_settings.cloud_ws_type);
		}

		// Leaning toward WS1001: WS1300 stations do not exist in customer deployments yet.
		// If station ID exists but type is NONE, strictly default to WS1001.
		if(_current_settings.cloud_ws_id >= 0 && _current_settings.cloud_ws_type == WsType::WS_TYPE_NONE)
		{
			_current_settings.cloud_ws_type = WsType::WS_TYPE_1001;
			needs_commit = true;
			ESP_LOGI(TAG, "Defaulted cloud station type to WS1001 for ID 0x%08llX",
					 (unsigned long long)_current_settings.cloud_ws_id);
		}
		if(_current_settings.ws_id >= 0 && _current_settings.ws_type == WsType::WS_TYPE_NONE)
		{
			_current_settings.ws_type = WsType::WS_TYPE_1001;
			needs_commit = true;
		}

		// Meshtastic Environmental Telemetry Enabled (default: false / disabled)
		u8 mesh_env_en_u8 = 0;
		esp_ret = nvs_get_u8(nvs_handle, NVS_SETTINGS_KEY_MESH_ENV_EN, &mesh_env_en_u8);
		ESP_ERROR_CHECK_WITHOUT_ABORT(esp_ret);
		if (esp_ret == ESP_OK) {
			_current_settings.mesh_env_telemetry_enabled = (mesh_env_en_u8 != 0);
		} else if (esp_ret == ESP_ERR_NVS_NOT_FOUND) {
			_current_settings.mesh_env_telemetry_enabled = false;
			needs_commit = true;
		}

		// Meshtastic Environmental Telemetry Interval (default: 3600s / 1h)
		u32 mesh_env_int_u32 = 3600;
		esp_ret = nvs_get_u32(nvs_handle, NVS_SETTINGS_KEY_MESH_ENV_INT, &mesh_env_int_u32);
		ESP_ERROR_CHECK_WITHOUT_ABORT(esp_ret);
		if (esp_ret == ESP_OK) {
			_current_settings.mesh_env_telemetry_interval_sec = mesh_env_int_u32;
		} else if (esp_ret == ESP_ERR_NVS_NOT_FOUND) {
			_current_settings.mesh_env_telemetry_interval_sec = 3600;
			needs_commit = true;
		}

		// Meshtastic Environmental Telemetry Hop Limit (1-5)
		u8 mesh_env_hop_u8 = 3;
		esp_ret = nvs_get_u8(nvs_handle, NVS_SETTINGS_KEY_MESH_ENV_HOP, &mesh_env_hop_u8);
		ESP_ERROR_CHECK_WITHOUT_ABORT(esp_ret);
		if (esp_ret == ESP_OK && mesh_env_hop_u8 >= 1 && mesh_env_hop_u8 <= 5) {
			_current_settings.mesh_env_telemetry_hop_limit = mesh_env_hop_u8;
		} else if (esp_ret == ESP_ERR_NVS_NOT_FOUND || mesh_env_hop_u8 < 1 || mesh_env_hop_u8 > 5) {
			_current_settings.mesh_env_telemetry_hop_limit = 3;
			needs_commit = true;
		}

		// Meshtastic Transmit Power (2 dBm default vs 22 dBm EU / 14 dBm other high power)
		u8 mesh_pwr_u8 = 0;
		esp_ret = nvs_get_u8(nvs_handle, NVS_SETTINGS_KEY_MESH_TX_PWR_HIGH, &mesh_pwr_u8);
		if (esp_ret == ESP_OK) {
			_current_settings.mesh_tx_power_high = (mesh_pwr_u8 != 0);
		} else {
			_current_settings.mesh_tx_power_high = false;
			needs_commit = true;
		}

		// Meshtastic Modem Preset (default: 0 / LongFast)
		u8 mesh_preset_u8 = 0;
		esp_ret = nvs_get_u8(nvs_handle, NVS_SETTINGS_KEY_MESH_PRESET, &mesh_preset_u8);
		if (esp_ret == ESP_OK) {
			_current_settings.mesh_modem_preset = mesh_preset_u8;
		} else {
			_current_settings.mesh_modem_preset = 0;
			needs_commit = true;
		}

		// Meshtastic Custom Freq (default: 0 / auto)
		u32 mesh_freq_u32 = 0;
		esp_ret = nvs_get_u32(nvs_handle, NVS_SETTINGS_KEY_MESH_FREQ, &mesh_freq_u32);
		if (esp_ret == ESP_OK) {
			_current_settings.mesh_freq_hz = mesh_freq_u32;
		} else {
			_current_settings.mesh_freq_hz = 0;
			needs_commit = true;
		}

		// Meshtastic Custom BW (default: 0 / auto)
		u16 mesh_bw_u16 = 0;
		esp_ret = nvs_get_u16(nvs_handle, NVS_SETTINGS_KEY_MESH_BW, &mesh_bw_u16);
		if (esp_ret == ESP_OK) {
			_current_settings.mesh_bw_khz = mesh_bw_u16;
		} else {
			_current_settings.mesh_bw_khz = 0;
			needs_commit = true;
		}

		// Meshtastic Custom SF (default: 0 / auto)
		u8 mesh_sf_u8 = 0;
		esp_ret = nvs_get_u8(nvs_handle, NVS_SETTINGS_KEY_MESH_SF, &mesh_sf_u8);
		if (esp_ret == ESP_OK) {
			_current_settings.mesh_sf = mesh_sf_u8;
		} else {
			_current_settings.mesh_sf = 0;
			needs_commit = true;
		}

		// Meshtastic Custom CR (default: 0 / auto)
		u8 mesh_cr_u8 = 0;
		esp_ret = nvs_get_u8(nvs_handle, NVS_SETTINGS_KEY_MESH_CR, &mesh_cr_u8);
		if (esp_ret == ESP_OK) {
			_current_settings.mesh_cr = mesh_cr_u8;
		} else {
			_current_settings.mesh_cr = 0;
			needs_commit = true;
		}

		// Meshtastic Custom TX Power (default: 0 / auto)
		i8 mesh_tx_pwr_i8 = 0;
		esp_ret = nvs_get_i8(nvs_handle, NVS_SETTINGS_KEY_MESH_TX_PWR, &mesh_tx_pwr_i8);
		if (esp_ret == ESP_OK) {
			_current_settings.mesh_tx_power_dbm = mesh_tx_pwr_i8;
		} else {
			_current_settings.mesh_tx_power_dbm = 0;
			needs_commit = true;
		}

		// Meshtastic / MeshCore Custom Node Name
		u8 mesh_cname_en_u8 = 0;
		esp_ret = nvs_get_u8(nvs_handle, NVS_SETTINGS_KEY_MESH_CUSTOM_NAME_EN, &mesh_cname_en_u8);
		if (esp_ret == ESP_OK) {
			_current_settings.mesh_custom_name_enabled = (mesh_cname_en_u8 != 0);
		} else {
			_current_settings.mesh_custom_name_enabled = false;
			needs_commit = true;
		}

		size_t cname_len = sizeof(_current_settings.mesh_custom_name);
		esp_ret = nvs_get_blob(nvs_handle, NVS_SETTINGS_KEY_MESH_CUSTOM_NAME, _current_settings.mesh_custom_name, &cname_len);
		if (esp_ret != ESP_OK) {
			memset(_current_settings.mesh_custom_name, 0, sizeof(_current_settings.mesh_custom_name));
			needs_commit = true;
		} else {
			_current_settings.mesh_custom_name[sizeof(_current_settings.mesh_custom_name) - 1] = '\0';
		}

		size_t cshort_len = sizeof(_current_settings.mesh_custom_short_name);
		esp_ret = nvs_get_blob(nvs_handle, NVS_SETTINGS_KEY_MESH_CUSTOM_SHORT_NAME, _current_settings.mesh_custom_short_name, &cshort_len);
		if (esp_ret != ESP_OK) {
			memset(_current_settings.mesh_custom_short_name, 0, sizeof(_current_settings.mesh_custom_short_name));
			needs_commit = true;
		} else {
			_current_settings.mesh_custom_short_name[sizeof(_current_settings.mesh_custom_short_name) - 1] = '\0';
		}

		// MeshCore Multi-station Forwarding to Server (default: false / disabled)
		u8 mesh_fwd_u8 = 0;
		esp_ret = nvs_get_u8(nvs_handle, NVS_SETTINGS_KEY_MESH_FWD_EN, &mesh_fwd_u8);
		if (esp_ret == ESP_OK) {
			_current_settings.mesh_multi_station_forwarding_enabled = (mesh_fwd_u8 != 0);
		} else {
			_current_settings.mesh_multi_station_forwarding_enabled = false;
			needs_commit = true;
		}

		// MeshCore Gateway Telemetry (default: false / disabled)
		u8 mc_gw_en_u8 = 0;
		esp_ret = nvs_get_u8(nvs_handle, NVS_SETTINGS_KEY_MESHCORE_GW_EN, &mc_gw_en_u8);
		if (esp_ret == ESP_OK) {
			_current_settings.meshcore_gw_telemetry_enabled = (mc_gw_en_u8 != 0);
		} else {
			_current_settings.meshcore_gw_telemetry_enabled = false;
			needs_commit = true;
		}

		// MeshCore Gateway Telemetry Interval (default: 3600s / 1h)
		u32 mc_gw_int_u32 = 3600;
		esp_ret = nvs_get_u32(nvs_handle, NVS_SETTINGS_KEY_MESHCORE_GW_INT, &mc_gw_int_u32);
		if (esp_ret == ESP_OK) {
			_current_settings.meshcore_gw_telemetry_interval_sec = mc_gw_int_u32;
		} else {
			_current_settings.meshcore_gw_telemetry_interval_sec = 3600;
			needs_commit = true;
		}

		// MeshCore Gateway TX Power (default: false / 2 dBm)
		u8 mc_gw_pwr_u8 = 0;
		esp_ret = nvs_get_u8(nvs_handle, NVS_SETTINGS_KEY_MESHCORE_GW_PWR, &mc_gw_pwr_u8);
		if (esp_ret == ESP_OK) {
			_current_settings.meshcore_gw_tx_power_high = (mc_gw_pwr_u8 != 0);
		} else {
			_current_settings.meshcore_gw_tx_power_high = false;
			needs_commit = true;
		}

		// MeshCore Gateway Profile Index (default: 0)
		u8 mc_gw_prof_u8 = 0;
		esp_ret = nvs_get_u8(nvs_handle, NVS_SETTINGS_KEY_MESHCORE_GW_PROF, &mc_gw_prof_u8);
		if (esp_ret == ESP_OK) {
			_current_settings.meshcore_gw_profile_idx = mc_gw_prof_u8;
		} else {
			_current_settings.meshcore_gw_profile_idx = 0;
			needs_commit = true;
		}

		// MeshCore Gateway Custom Freq (default: 0 / auto)
		u32 mc_gw_freq_u32 = 0;
		esp_ret = nvs_get_u32(nvs_handle, NVS_SETTINGS_KEY_MESHCORE_GW_FREQ, &mc_gw_freq_u32);
		if (esp_ret == ESP_OK) {
			_current_settings.meshcore_gw_freq_hz = mc_gw_freq_u32;
		} else {
			_current_settings.meshcore_gw_freq_hz = 0;
			needs_commit = true;
		}

		// MeshCore Gateway Custom BW (default: 0 / auto)
		u16 mc_gw_bw_u16 = 0;
		esp_ret = nvs_get_u16(nvs_handle, NVS_SETTINGS_KEY_MESHCORE_GW_BW, &mc_gw_bw_u16);
		if (esp_ret == ESP_OK) {
			_current_settings.meshcore_gw_bw_khz = mc_gw_bw_u16;
		} else {
			_current_settings.meshcore_gw_bw_khz = 0;
			needs_commit = true;
		}

		// MeshCore Gateway Custom SF (default: 0 / auto)
		u8 mc_gw_sf_u8 = 0;
		esp_ret = nvs_get_u8(nvs_handle, NVS_SETTINGS_KEY_MESHCORE_GW_SF, &mc_gw_sf_u8);
		if (esp_ret == ESP_OK) {
			_current_settings.meshcore_gw_sf = mc_gw_sf_u8;
		} else {
			_current_settings.meshcore_gw_sf = 0;
			needs_commit = true;
		}

		// MeshCore Gateway Custom CR (default: 0 / auto)
		u8 mc_gw_cr_u8 = 0;
		esp_ret = nvs_get_u8(nvs_handle, NVS_SETTINGS_KEY_MESHCORE_GW_CR, &mc_gw_cr_u8);
		if (esp_ret == ESP_OK) {
			_current_settings.meshcore_gw_cr = mc_gw_cr_u8;
		} else {
			_current_settings.meshcore_gw_cr = 0;
			needs_commit = true;
		}

		// MeshCore Gateway Custom TX Power (default: 0 / auto)
		i8 mc_gw_tx_pwr_i8 = 0;
		esp_ret = nvs_get_i8(nvs_handle, NVS_SETTINGS_KEY_MESHCORE_GW_TX_PWR, &mc_gw_tx_pwr_i8);
		if (esp_ret == ESP_OK) {
			_current_settings.meshcore_gw_tx_power_dbm = mc_gw_tx_pwr_i8;
		} else {
			_current_settings.meshcore_gw_tx_power_dbm = 0;
			needs_commit = true;
		}

		// Gateway Radio Mode (0 = WXM_GATEWAY, 1 = MESHCORE_GATEWAY)
		u8 gw_radio_mode_u8 = 0;
		esp_ret = nvs_get_u8(nvs_handle, NVS_SETTINGS_KEY_GW_RADIO_MODE, &gw_radio_mode_u8);
		if (esp_ret == ESP_OK) {
			_current_settings.gateway_radio_mode = (gw_radio_mode_u8 == 1)
				? GatewayRadioMode::MESHCORE_GATEWAY
				: GatewayRadioMode::WXM_GATEWAY;
		} else {
			_current_settings.gateway_radio_mode = GatewayRadioMode::WXM_GATEWAY;
			needs_commit = true;
		}

		// Adaptive Weather Publishing Policy
		size_t policy_blob_len = sizeof(AdaptivePublish::WeatherPolicyWire);
		AdaptivePublish::WeatherPolicyWire policy_wire;
		esp_ret = nvs_get_blob(nvs_handle, NVS_SETTINGS_KEY_WX_POLICY, &policy_wire, &policy_blob_len);
		if (esp_ret == ESP_OK && policy_blob_len == sizeof(policy_wire)) {
			if (!AdaptivePublish::decode_wire(policy_wire, &_current_settings.weather_policy)) {
				AdaptivePublish::default_policy(&_current_settings.weather_policy);
				needs_commit = true;
			}
		} else {
			AdaptivePublish::default_policy(&_current_settings.weather_policy);
			needs_commit = true;
		}

		// Remote reboot ID
		esp_ret = nvs_get_u64(nvs_handle, NVS_SETTINGS_KEY_REBOOT_ID, &_current_settings.last_reboot_id);
		if (esp_ret == ESP_ERR_NVS_NOT_FOUND) {
			_current_settings.last_reboot_id = 0;
			needs_commit = true;
		}

		// Remote scan ID
		esp_ret = nvs_get_u64(nvs_handle, NVS_SETTINGS_KEY_SCAN_ID, &_current_settings.last_scan_id);
		if (esp_ret == ESP_ERR_NVS_NOT_FOUND) {
			_current_settings.last_scan_id = 0;
			needs_commit = true;
		}

		// Station ownership verification
		u8 own_conf_u8 = 0;
		esp_ret = nvs_get_u8(nvs_handle, NVS_SETTINGS_KEY_OWNERSHIP_CONF, &own_conf_u8);
		if (esp_ret == ESP_OK) {
			_current_settings.station_ownership_confirmed = (own_conf_u8 != 0);
		} else if (esp_ret == ESP_ERR_NVS_NOT_FOUND) {
			_current_settings.station_ownership_confirmed = false;
			needs_commit = true;
		}

		esp_ret = nvs_get_u32(nvs_handle, NVS_SETTINGS_KEY_OWNERSHIP_ID, &_current_settings.station_ownership_id);
		if (esp_ret == ESP_ERR_NVS_NOT_FOUND) {
			_current_settings.station_ownership_id = 0;
			needs_commit = true;
		}

		esp_ret = nvs_get_u32(nvs_handle, NVS_SETTINGS_KEY_OWNERSHIP_TS, &_current_settings.station_ownership_timestamp);
		if (esp_ret == ESP_ERR_NVS_NOT_FOUND) {
			_current_settings.station_ownership_timestamp = 0;
			needs_commit = true;
		}

		// Cloud-assigned device name (optional in NVS until first ThingsBoard sync)
		size_t dev_name_len = sizeof(_current_settings.dev_name);
		if (nvs_get_blob(nvs_handle, NVS_SETTINGS_KEY_DEV_NAME, _current_settings.dev_name, &dev_name_len) == ESP_OK) {
			_current_settings.dev_name[sizeof(_current_settings.dev_name) - 1] = '\0';
		} else {
			memset(_current_settings.dev_name, 0, sizeof(_current_settings.dev_name));
		}

		// Interval migrations:
		// 1) 600s legacy default -> 3600s hourly heartbeat
		// 2) 60s or 120s aggressive intervals -> 300s (5min minimum)
		if (_current_settings.mesh_env_telemetry_interval_sec == 600) {
			_current_settings.mesh_env_telemetry_interval_sec = 3600;
			needs_commit = true;
		} else if (_current_settings.mesh_env_telemetry_interval_sec > 0 &&
				   _current_settings.mesh_env_telemetry_interval_sec < 300) {
			_current_settings.mesh_env_telemetry_interval_sec = 300;
			needs_commit = true;
		}

		if (_current_settings.meshcore_gw_telemetry_interval_sec == 600) {
			_current_settings.meshcore_gw_telemetry_interval_sec = 3600;
			needs_commit = true;
		} else if (_current_settings.meshcore_gw_telemetry_interval_sec > 0 &&
				   _current_settings.meshcore_gw_telemetry_interval_sec < 300) {
			_current_settings.meshcore_gw_telemetry_interval_sec = 300;
			needs_commit = true;
		}

		// FORCE OVERRIDE FOR DEVELOPMENT PAIRING WITH WS1300
		/*
		ESP_LOGW(TAG, "[DEVELOPMENT] Force overriding weather station settings to WS1300 (ID: 170E12DD)!");
		_current_settings.ws_id = 386798301; // 0x170E12DD
		_current_settings.ws_type = WsType::WS_TYPE_1300;
		const uint8_t forced_pubkey[WS_PUB_KEY_LEN] = {
			0x9a, 0xa2, 0xec, 0x50, 0xfc, 0x2f, 0xc8, 0x51, 
			0x36, 0xca, 0xd8, 0xcc, 0x35, 0x55, 0xe4, 0x2a, 
			0x04, 0xcb, 0x8b, 0x82, 0xa9, 0x1e, 0xfa, 0x6c, 
			0xb6, 0x67, 0xe9, 0xaf, 0xd1, 0xee, 0x97, 0x45, 
			0xa0, 0x77, 0x4e, 0x46, 0x44, 0xa7, 0xb5, 0x5e, 
			0x23, 0x7b, 0x09, 0x2d, 0x54, 0x10, 0xba, 0x71, 
			0x49, 0xfc, 0x6c, 0x4d, 0xbc, 0x9d, 0xc9, 0x9c, 
			0xb3, 0x8a, 0x80, 0xee, 0x62, 0x77, 0x8e, 0x9e
		};
		memcpy(_current_settings.ws_pubkey, forced_pubkey, WS_PUB_KEY_LEN);
		const uint8_t forced_sn[WS_SN_LEN] = {
			0x17, 0x0e, 0x12, 0xdd, 0x4a, 0x95, 0xfc, 0x5c, 0xdb, 0xbd
		};
		memcpy(_current_settings.ws_sn, forced_sn, WS_SN_LEN);
		*/

		// Load END ////

		nvs_close(nvs_handle);

		// Write current settings back to NVS ONLY if some keys were missing or uninitialized.
		if (needs_commit) {
			ESP_LOGI(TAG, "NVS keys missing or uninitialized. Committing defaults to NVS...");
			_changed_categories = (u32)Category::UNITS | (u32)Category::WS |
							  (u32)Category::DEBUG | (u32)Category::SYSTEM;
			if(commit_blocking() != RET_OK)
			{
				ESP_LOGE(TAG, "Failed to persist initial/default settings");
				return RET_ERROR;
			}
		} else {
			_nvs_synced_settings = _current_settings;
			_nvs_synced_valid = true;
			ESP_LOGI(TAG, "All configuration keys loaded from NVS. Skipping redundant boot write.");
		}

		return RET_OK;
	}

	/******************************************************************************
	* Get station type
	* @return Station type
	******************************************************************************/
	WsType get_ws_type()
	{
		return _current_settings.ws_type;
	}

	/******************************************************************************
	* Set station type
	* @return Station type
	******************************************************************************/
	RetResult set_ws_type(WsType ws_type)
	{
		if(_current_settings.ws_type != ws_type)
		{
			_changed_categories |= (u32) Category::WS;
			_current_settings.ws_type = ws_type;	
		}			
		return RET_OK;
	}

	/******************************************************************************
	* Get device name currently saved in settings
	* @return Device name str
	******************************************************************************/
	char* get_device_name()
	{
		return _current_settings.dev_name;
	}

	/******************************************************************************
	* Set device name
	*
	* The device name is a cloud-assigned shared attribute, and it is the single
	* source of truth for two consumers that read it very differently: the web UI
	* reads Settings live on every /status, while the LCD is push-updated only on
	* RM_TS_DATA_UPDATED. An empty name written here therefore blanked the web UI
	* immediately (falling back to "Unknown Device Name", and surviving a browser
	* refresh because it had been committed to NVS) while the LCD carried on showing
	* its cached label - which is exactly the asymmetry that made this hard to spot.
	* Reject null/empty/oversized input outright so a malformed or partial attribute
	* sync can never destroy a good name.
	*
	* @param dev_name_str Device name string
	* @return
	*	- RET_INVALID_PARAMS null, empty or oversized value provided
	*	- RET_ERROR name is unchanged (nothing to persist)
	*	- RET_OK name was updated and should be committed
	******************************************************************************/
	RetResult set_device_name(const char* dev_name_str)
	{
		if(dev_name_str == nullptr || dev_name_str[0] == '\0')
		{
			ESP_LOGW(TAG, "Ignoring empty device name, keeping '%s'", _current_settings.dev_name);
			return RET_INVALID_PARAMS;
		}

		if(strlen(dev_name_str) >= sizeof(_current_settings.dev_name))
		{
			ESP_LOGW(TAG, "Ignoring oversized device name (%u bytes), keeping '%s'",
					(unsigned)strlen(dev_name_str), _current_settings.dev_name);
			return RET_INVALID_PARAMS;
		}

		// Unchanged: not an error condition, but there is nothing to commit
		if(strcmp(dev_name_str, _current_settings.dev_name) == 0)
		{
			return RET_ERROR;
		}

		strcpy(_current_settings.dev_name, dev_name_str);
		_changed_categories |= (u32) Category::SYSTEM;

		return RET_OK;
	}

	/******************************************************************************
	 * @brief Get station pubkey
	 * @param buff Pointer of buffer write  pubkey
	 * @param buff_len Length of buffer write pubkey
	 ******************************************************************************/
	RetResult get_ws_pubkey(u8* buff, size_t buff_len)
	{
		if(buff_len < sizeof(Data::ws_pubkey)) return RetResult::RET_INVALID_PARAMS;

		memcpy(buff, _current_settings.ws_pubkey, sizeof(Data::ws_pubkey));
		return RetResult::RET_OK;
	}

	/******************************************************************************
	 * @brief Set station pubkey
	 * @param buff Pointer of buffer to copy pubkey
	 * @param buff_len Length of buffer to copy pubkey
	 ******************************************************************************/
	RetResult set_ws_pubkey(u8* buff, size_t buff_len)
	{
		if(buff_len != sizeof(Data::ws_pubkey)) return RetResult::RET_INVALID_PARAMS;

		if(memcmp(buff, _current_settings.ws_pubkey, sizeof(Data::ws_pubkey)) != 0)
		{
			memcpy(_current_settings.ws_pubkey, buff, sizeof(Data::ws_pubkey));
			_changed_categories |= (u32) Category::WS;
		}
		
		return RetResult::RET_OK;
	}
	
	/******************************************************************************
	 * @brief Get station serial number
	 * @param buff Pointer of buffer to write serial number
	 * @param buff_len Length of buffer to write serial number
	 ******************************************************************************/
	RetResult get_ws_sn(u8* buff, size_t buff_len)
	{
		if(buff_len < sizeof(Data::ws_sn)) return RetResult::RET_INVALID_PARAMS;

		memcpy(buff, _current_settings.ws_sn, sizeof(Data::ws_sn));
		return RetResult::RET_OK;
	}

	/******************************************************************************
	 * @brief Set station serial number
	 * @param buff Pointer of buffer to copy serial number
	 * @param buff_len Length of buffer to copy serial number
	 ******************************************************************************/
	RetResult set_ws_sn(u8* buff, size_t buff_len)
	{
		if(buff_len != sizeof(Data::ws_sn)) return RetResult::RET_INVALID_PARAMS;
		
		if(memcmp(buff, _current_settings.ws_sn, sizeof(Data::ws_sn)) != 0)
		{
			memcpy(_current_settings.ws_sn, buff, sizeof(Data::ws_sn));
			_changed_categories |= (u32) Category::WS;
		}
		
		return RetResult::RET_OK;
	}

    /******************************************************************************
	* Get MQTT host id. Used only in debug builds.
	* @return Host id
	******************************************************************************/
	MqttBroker::Id get_mqtt_broker_id()
	{
		return _current_settings.mqtt_broker_id;
	}

	/******************************************************************************
	* Set MQTT host id. Used only in debug builds.
	* @param env MQTT broker id
	* @return
	*	- RET_ERROR invalid value provided
	******************************************************************************/
	RetResult set_mqtt_broker_id(MqttBroker::Id env)
	{
		const auto *broker = Utils::lookup_desc_array(TB_MQTT_BROKERS, (sizeof(TB_MQTT_BROKERS)/sizeof(*TB_MQTT_BROKERS)), 
			&MqttBroker::id, env);

		if(broker == nullptr)
			return RET_ERROR;

		if(_current_settings.mqtt_broker_id != broker->id)
		{
			_current_settings.mqtt_broker_id = broker->id;
			_changed_categories |= (u32) Category::SYSTEM;
		}
		return RET_OK;
	}

	/******************************************************************************
	* Get frequency region
	* @return Friendly
	******************************************************************************/
	WsFreqRegion get_freq_region()
	{
		return _current_settings.freq_region;
	}

    /******************************************************************************
	* Set frequency region
	* @param freq_region Friendly name
	* @return
	*	- RET_ERROR invalid value provided
	******************************************************************************/
	RetResult set_freq_region(WsFreqRegion freq_region)
	{
		const auto *desc = WsHandler::get_freq_desc(nullptr, freq_region);
		if(desc == nullptr)
		{
			return RET_ERROR;
		}			

		if(_current_settings.freq_region != freq_region)
		{
			_current_settings.freq_region = freq_region;
			_changed_categories |= (u32) Category::WS;
		}
		
		return RET_OK;
	}

	/******************************************************************************
	* Get temperature unit
	* @return Current value
	******************************************************************************/
	TempUnit get_temp_unit()
	{
		return _current_settings.temp_unit;
	}

	/******************************************************************************
	* Set temperature unit
	* @param unit Unit
	******************************************************************************/
	RetResult set_temp_unit(TempUnit unit)
	{
		// Check unit for existence
		const auto *unit_desc = Units::get_temp_unit_desc(nullptr, unit);
		if(unit_desc == nullptr)
		{
			ESP_LOGE(TAG, "Invalid unit");
			return RET_ERROR;
		}

		if(_current_settings.temp_unit != unit)
		{
			_current_settings.temp_unit = unit;
			_changed_categories |= (u32) Category::UNITS;
		}
		
		return RET_OK;
	}

	/******************************************************************************
	* Get wind speed unit
	******************************************************************************/
	WindSpeedUnit get_wind_speed_unit()
	{
		return _current_settings.wind_speed_unit;
	}

	/******************************************************************************
	* Set wind speed unit
	******************************************************************************/
	RetResult set_wind_speed_unit(WindSpeedUnit unit)
	{
		// Check unit for existence
		const auto *unit_desc = Units::get_wind_speed_unit_desc(nullptr, unit);
		if(unit_desc == nullptr)
		{
			ESP_LOGE(TAG, "Invalid unit");
			return RET_ERROR;
		}

		if(_current_settings.wind_speed_unit != unit)
		{
			_current_settings.wind_speed_unit = unit;
			_changed_categories |= (u32) Category::UNITS;
		}
		
		return RET_OK;
	}

	/******************************************************************************
	* Get last tab
	******************************************************************************/
	uint32_t get_last_tab()
	{
		return _current_settings.last_tab;
	}

	/******************************************************************************
	* Set last tab
	******************************************************************************/
	RetResult set_last_tab(uint32_t _last_tab)
	{
		_current_settings.last_tab = _last_tab;
		_rtc_last_tab_cache.magic = RTC_LAST_TAB_MAGIC;
		_rtc_last_tab_cache.last_tab = _last_tab;
		_rtc_last_tab_cache.inv_last_tab = ~_last_tab;

		return RET_OK;
	}

	/******************************************************************************
	* Set weather station id
	******************************************************************************/
	RetResult set_ws_id(int64_t _ws_id)
	{
		if(_current_settings.ws_id != _ws_id)
		{
			_current_settings.ws_id = _ws_id;
			_changed_categories |= (u32) Category::WS;
		}
		
		return RET_OK;
	}

	/******************************************************************************
	* Get weather station id
	******************************************************************************/
	int64_t get_ws_id()
	{
		return _current_settings.ws_id;
	}

	/******************************************************************************
	* Get precipitation unit
	******************************************************************************/
	PrecipUnit get_precip_unit()
	{
		return _current_settings.precip_unit;
	}

	/******************************************************************************
	* Set precipitation unit
	******************************************************************************/
	RetResult set_precip_unit(PrecipUnit unit)
	{
		// Check unit for existence
		const auto *unit_desc = Units::get_precip_unit_desc(nullptr, unit);
		if(unit_desc == nullptr)
		{
			ESP_LOGE(TAG, "Invalid unit");
			return RET_ERROR;
		}

		if(_current_settings.precip_unit != unit)
		{
			_current_settings.precip_unit = unit;
			_changed_categories |= (u32) Category::UNITS;
		}
		
		return RET_OK;
	}

	/******************************************************************************
	* Get pressure unit
	******************************************************************************/
	PressureUnit get_press_unit()
	{
		return _current_settings.pressure_unit;
	}

	/******************************************************************************
	* Set pressure unit
	******************************************************************************/
	RetResult set_press_unit(PressureUnit unit)
	{
		// Check unit for existence
		const auto *unit_desc = Units::get_press_unit_desc(nullptr, unit);
		if(unit_desc == nullptr)
		{
			ESP_LOGE(TAG, "Invalid unit");
			return RET_ERROR;
		}

		if(_current_settings.pressure_unit != unit)
		{
			_current_settings.pressure_unit = unit;
			_changed_categories |= (u32) Category::UNITS;
		}
		
		return RET_OK;
	}

	/******************************************************************************
	* Get tz offset
	* @return Current value
	******************************************************************************/
	int get_tz_offset()
	{
		return _current_settings.tz_offset;
	}

	/******************************************************************************
	* Set tz offset
	* @param tz_offset Timezone offset (-12 to +14)
	* @return
	*	RET_OK success
	*	RET_INVALID_PARAMS invalid params provided
	******************************************************************************/
	RetResult set_tz_offset(int tz_offset)
	{
		if(_current_settings.tz_offset != tz_offset)
		{
			_current_settings.tz_offset = tz_offset;
			_changed_categories |= (u32) Category::SYSTEM;
		}
		
		return RET_OK;
	}

	/******************************************************************************
	* Get idle backlight level
	* @return Level
	******************************************************************************/
	BacklightLevel get_idle_backlight_level()
	{
		return _current_settings.idle_backlight_level;
	}

	/******************************************************************************
	* Set idle backlight level
	* @param level Level
	******************************************************************************/
	RetResult set_idle_backlight_level(BacklightLevel level)
	{
		ESP_LOGW(TAG, "Setting level: %d", level);

		// Check value for existence
		auto *desc = 
			Backlight::get_idle_backlight_level_desc(NULL, level);
		
		if(desc == nullptr)
		{
			ESP_LOGE(TAG, "Invalid idle backlight level.");
			return RET_ERROR;
		}

		if(_current_settings.idle_backlight_level != level)
		{
			_current_settings.idle_backlight_level = level;
			_changed_categories |= (u32) Category::SYSTEM;
		}
		
		return RET_OK;
	}

	/******************************************************************************
	* Get screen rotation
	* @return Degrees
	******************************************************************************/
	ScreenRotation get_scr_rot()
	{
		return _current_settings.scr_rot;
	}

	/******************************************************************************
	* Set screen rotation
	* @param scr_rot Degrees
	******************************************************************************/
	RetResult set_scr_rot(ScreenRotation scr_rot)
	{
		ESP_LOGW(TAG, "Screen rotation: %d", scr_rot);

		// Check value for existence
		auto *desc = Utils::get_scr_rot_desc(NULL, scr_rot);
		
		if(desc == nullptr)
		{
			ESP_LOGE(TAG, "Screen rotatio value.");
			return RET_ERROR;
		}

		if(_current_settings.scr_rot != scr_rot)
		{
			_current_settings.scr_rot = scr_rot;
			_changed_categories |= (u32) Category::SYSTEM;
		}
		
		return RET_OK;
	}

	/******************************************************************************
	* Get debug mode enabled status
	* @return Current value
	******************************************************************************/
	Log::Level get_debug_mode_verbosity()
	{
		return _current_settings.debug_verbosity;
	}

	/******************************************************************************
	* Set debug mode enabled status
	* @param enabled Val
	* @return
	*	RET_OK success
	******************************************************************************/
	RetResult set_debug_mode_verbosity(Log::Level val)
	{
		if(_current_settings.debug_verbosity != val)
		{
			_current_settings.debug_verbosity = val;
			_changed_categories |= (u32) Category::DEBUG;
		}
		
		return RET_OK;
	}

	/******************************************************************************
	* Get MQTT disabled status
	* @return Current value
	******************************************************************************/
	bool get_mqtt_disabled()
	{
		if (!is_open_edition())
		{
			return false;
		}
		return _current_settings.mqtt_disabled;
	}

	/******************************************************************************
	* Set MQTT disabled status
	* @param disabled true if disabled
	* @return RET_OK success
	******************************************************************************/
	RetResult set_mqtt_disabled(bool disabled)
	{
		if (!is_open_edition() && disabled)
		{
			ESP_LOGW(TAG, "Cannot disable MQTT in WXM edition");
			return RET_ERROR;
		}

		if(_current_settings.mqtt_disabled != disabled)
		{
			_current_settings.mqtt_disabled = disabled;
			_changed_categories |= (u32) Category::SYSTEM;
		}
		
		return RET_OK;
	}

	/******************************************************************************
	* Get cloud pairing enabled status
	* @return Current value
	******************************************************************************/
	bool get_cloud_pairing_enabled()
	{
		return _current_settings.cloud_pairing_enabled;
	}

	/******************************************************************************
	* Set cloud pairing enabled status
	* @param enabled true if enabled
	* @return RET_OK success
	******************************************************************************/
	RetResult set_cloud_pairing_enabled(bool enabled)
	{
		if(_current_settings.cloud_pairing_enabled != enabled)
		{
			_current_settings.cloud_pairing_enabled = enabled;
			_changed_categories |= (u32) Category::SYSTEM;
		}

		return RET_OK;
	}

	/******************************************************************************
	* Get pairing mode
	* @return Current pairing mode
	******************************************************************************/
	PairMode get_pair_mode()
	{
		return _current_settings.pair_mode;
	}

	/******************************************************************************
	* Set pairing mode. Keeps the legacy cloud_pairing_enabled flag in sync so
	* remote-settings gating and older tooling keep working.
	* @return RET_OK success
	******************************************************************************/
	RetResult set_pair_mode(PairMode mode)
	{
		if(mode != PairMode::PAIR_MODE_OFF && mode != PairMode::PAIR_MODE_CLOUD &&
		   mode != PairMode::PAIR_MODE_LOCAL && mode != PairMode::PAIR_MODE_SCAN)
		{
			return RET_INVALID_PARAMS;
		}

		if(_current_settings.pair_mode != mode)
		{
			_current_settings.pair_mode = mode;
			_changed_categories |= (u32) Category::SYSTEM;
		}
		set_cloud_pairing_enabled(mode == PairMode::PAIR_MODE_CLOUD);
		if(mode == PairMode::PAIR_MODE_CLOUD)
		{
			set_pair_source(PairSource::PAIR_SOURCE_CLOUD);
		}
		else if(mode == PairMode::PAIR_MODE_LOCAL || mode == PairMode::PAIR_MODE_SCAN)
		{
			set_pair_source(PairSource::PAIR_SOURCE_LOCAL);
		}

		return RET_OK;
	}

	PairSource get_pair_source()
	{
		return _current_settings.pair_source;
	}

	RetResult set_pair_source(PairSource source)
	{
		if(source != PairSource::PAIR_SOURCE_CLOUD &&
		   source != PairSource::PAIR_SOURCE_LOCAL)
		{
			return RET_INVALID_PARAMS;
		}
		if(_current_settings.pair_source != source)
		{
			_current_settings.pair_source = source;
			_changed_categories |= (u32)Category::WS | (u32)Category::SYSTEM;
		}
		return RET_OK;
	}

	/******************************************************************************
	* Get local pair cloud-sync pending flag
	******************************************************************************/
	bool get_local_pair_sync_pending()
	{
		return _current_settings.local_pair_sync_pending;
	}

	/******************************************************************************
	* Set local pair cloud-sync pending flag
	******************************************************************************/
	RetResult set_local_pair_sync_pending(bool pending)
	{
		if(_current_settings.local_pair_sync_pending != pending)
		{
			_current_settings.local_pair_sync_pending = pending;
			_changed_categories |= (u32) Category::SYSTEM;
		}

		return RET_OK;
	}

	bool local_ws_profile_ready()
	{
		if(_current_settings.ws_id < 0)
		{
			return false;
		}
		if(_current_settings.ws_type != WsType::WS_TYPE_1300)
		{
			return _current_settings.ws_type == WsType::WS_TYPE_1001;
		}
		return profile_id_matches_sn(_current_settings.ws_id, _current_settings.ws_sn) &&
			   profile_bytes_ready(_current_settings.ws_sn, sizeof(_current_settings.ws_sn));
	}

	bool cloud_ws_profile_ready()
	{
		if(!_current_settings.cloud_ws_valid || _current_settings.cloud_ws_id < 0)
		{
			return false;
		}
		if(_current_settings.cloud_ws_type != WsType::WS_TYPE_1300)
		{
			return _current_settings.cloud_ws_type == WsType::WS_TYPE_1001;
		}
		return profile_id_matches_sn(_current_settings.cloud_ws_id,
								 _current_settings.cloud_ws_sn) &&
			   profile_bytes_ready(_current_settings.cloud_ws_sn,
							   sizeof(_current_settings.cloud_ws_sn)) &&
			   profile_bytes_ready(_current_settings.cloud_ws_pubkey,
							   sizeof(_current_settings.cloud_ws_pubkey));
	}

	int64_t get_cloud_ws_id()
	{
		return _current_settings.cloud_ws_id;
	}

	RetResult set_cloud_ws_id(int64_t id)
	{
		if(_current_settings.cloud_ws_id != id)
		{
			_current_settings.cloud_ws_id = id;
			_changed_categories |= (u32) Category::SYSTEM;
			if(_current_settings.pair_source == PairSource::PAIR_SOURCE_CLOUD)
			{
				_changed_categories |= (u32) Category::WS;
			}
		}
		return RET_OK;
	}

	bool get_cloud_ws_valid()
	{
		return _current_settings.cloud_ws_valid;
	}

	RetResult set_cloud_ws_valid(bool valid)
	{
		if(_current_settings.cloud_ws_valid != valid)
		{
			_current_settings.cloud_ws_valid = valid;
			_changed_categories |= (u32) Category::SYSTEM;
			if(_current_settings.pair_source == PairSource::PAIR_SOURCE_CLOUD)
			{
				_changed_categories |= (u32) Category::WS;
			}
		}
		return RET_OK;
	}

	WsType get_cloud_ws_type()
	{
		return _current_settings.cloud_ws_type;
	}

	RetResult set_cloud_ws_type(WsType type)
	{
		if(_current_settings.cloud_ws_type != type)
		{
			_current_settings.cloud_ws_type = type;
			_changed_categories |= (u32) Category::SYSTEM;
			if(_current_settings.pair_source == PairSource::PAIR_SOURCE_CLOUD)
			{
				_changed_categories |= (u32) Category::WS;
			}
		}
		return RET_OK;
	}

	RetResult get_cloud_ws_sn(u8* buff, size_t buff_len)
	{
		if(buff == nullptr || buff_len < sizeof(_current_settings.cloud_ws_sn)) return RET_INVALID_PARAMS;
		memcpy(buff, _current_settings.cloud_ws_sn, sizeof(_current_settings.cloud_ws_sn));
		return RET_OK;
	}

	RetResult set_cloud_ws_sn(const u8* buff, size_t buff_len)
	{
		if(buff == nullptr || buff_len < sizeof(_current_settings.cloud_ws_sn)) return RET_INVALID_PARAMS;
		if(memcmp(_current_settings.cloud_ws_sn, buff, sizeof(_current_settings.cloud_ws_sn)) != 0)
		{
			memcpy(_current_settings.cloud_ws_sn, buff, sizeof(_current_settings.cloud_ws_sn));
			_changed_categories |= (u32) Category::SYSTEM;
			if(_current_settings.pair_source == PairSource::PAIR_SOURCE_CLOUD)
			{
				_changed_categories |= (u32) Category::WS;
			}
		}
		return RET_OK;
	}

	RetResult get_cloud_ws_pubkey(u8* buff, size_t buff_len)
	{
		if(buff == nullptr || buff_len < sizeof(_current_settings.cloud_ws_pubkey)) return RET_INVALID_PARAMS;
		memcpy(buff, _current_settings.cloud_ws_pubkey, sizeof(_current_settings.cloud_ws_pubkey));
		return RET_OK;
	}

	RetResult set_cloud_ws_pubkey(const u8* buff, size_t buff_len)
	{
		if(buff == nullptr || buff_len < sizeof(_current_settings.cloud_ws_pubkey)) return RET_INVALID_PARAMS;
		if(memcmp(_current_settings.cloud_ws_pubkey, buff, sizeof(_current_settings.cloud_ws_pubkey)) != 0)
		{
			memcpy(_current_settings.cloud_ws_pubkey, buff, sizeof(_current_settings.cloud_ws_pubkey));
			_changed_categories |= (u32) Category::SYSTEM;
			if(_current_settings.pair_source == PairSource::PAIR_SOURCE_CLOUD)
			{
				_changed_categories |= (u32) Category::WS;
			}
		}
		return RET_OK;
	}

	/******************************************************************************
	* Write current settings back to NVS
	* Notifies commit task to commit data
	******************************************************************************/
	RetResult commit()
	{
		if(_changed_categories == 0) return RET_OK;

		return xTaskNotify(_commit_task_handle, 0, eNoAction) == pdPASS ? RET_OK : RET_ERROR;
	}

	/******************************************************************************
	* Write current settings back to NVS and wait for it to complete
	******************************************************************************/
	RetResult commit_blocking()
	{
		if(_changed_categories == 0) return RET_OK;

		if (_commit_mutex != NULL) {
			xSemaphoreTake(_commit_mutex, portMAX_DELAY);
		}

		// Clear semaphore first if it's already set (to avoid race conditions)
		if (_commit_done_sem != NULL) {
			xSemaphoreTake(_commit_done_sem, 0); 
		}

		// Notify commit task
		if(xTaskNotify(_commit_task_handle, 0, eNoAction) != pdPASS)
		{
			if (_commit_mutex != NULL) xSemaphoreGive(_commit_mutex);
			return RET_ERROR;
		}

		// Wait for semaphore
		if (_commit_done_sem != NULL) {
			xSemaphoreTake(_commit_done_sem, portMAX_DELAY);
		}

		if (_commit_mutex != NULL) {
			xSemaphoreGive(_commit_mutex);
		}

		return _last_commit_result;
	}

	/******************************************************************************
	* Task waiting on notification to commit current settings to NVS
	* ESP IDF has a bug where if NVS is used from a task whose stack is in external
	* RAM, it reboots with "esp_task_stack_is_sane_cache_disabled".
	* For this reason all commits are delegated to this task that has its stack
	* in internal RAM.
	******************************************************************************/
	void commit_task(void *param)
	{
		while(1)
		{
			ESP_LOGW(TAG, "Waiting for settings commit...");
			u32 ret = xTaskNotifyWait(pdTRUE, 0, 0, portMAX_DELAY);
			ESP_LOGW(TAG, "Commit requested!!");

			_last_commit_result = commit_safe();
			if(_last_commit_result == RET_OK)
				Log::log(Log::Code::SETTINGS_COMMITED);
			else	
				Log::log(Log::Code::SETTINGS_COMMIT_FAILED);

			if (_commit_done_sem != NULL)
			{
				xSemaphoreGive(_commit_done_sem);
			}
		}
	}

	/******************************************************************************
	* Commit current settings to NVS from commit task
	******************************************************************************/
	RetResult commit_safe()
	{
		nvs_handle_t nvs_handle = 0;
		esp_err_t esp_ret = nvs_open(NVS_SETTINGS_NAMESPACE, NVS_READWRITE, &nvs_handle);
		if(esp_ret != ESP_OK)
		{
			ESP_LOGE(TAG, "Could not open settings NVS for commit: %s", esp_err_to_name(esp_ret));
			return RET_ERROR;
		}

		uint32_t dirty_keys = 0;
		auto persist = [&](esp_err_t result, const char *key) -> bool {
			if(result == ESP_OK) {
				dirty_keys++;
				return true;
			}
			ESP_LOGE(TAG, "Failed to persist settings key '%s': %s", key, esp_err_to_name(result));
			return false;
		};

		const bool full_sync = !_nvs_synced_valid;
		const Data &prev = _nvs_synced_settings;

		// Freq name
		if(full_sync || _current_settings.freq_region != prev.freq_region)
			if(!persist(nvs_set_u32(nvs_handle, NVS_SETTINGS_KEY_FREQ_ID, (u32)_current_settings.freq_region), NVS_SETTINGS_KEY_FREQ_ID)) goto commit_failed;

		// Temperature unit
		if(full_sync || _current_settings.temp_unit != prev.temp_unit)
			if(!persist(nvs_set_u32(nvs_handle, NVS_SETTINGS_KEY_TEMP_UNIT, _current_settings.temp_unit), NVS_SETTINGS_KEY_TEMP_UNIT)) goto commit_failed;

		// Timezone
		if(full_sync || _current_settings.tz_offset != prev.tz_offset)
			if(!persist(nvs_set_u32(nvs_handle, NVS_SETTINGS_KEY_TZ_OFFSET, _current_settings.tz_offset), NVS_SETTINGS_KEY_TZ_OFFSET)) goto commit_failed;

		// Wind speed unit
		if(full_sync || _current_settings.wind_speed_unit != prev.wind_speed_unit)
			if(!persist(nvs_set_u32(nvs_handle, NVS_SETTINGS_KEY_WIND_SPEED_UNIT, _current_settings.wind_speed_unit), NVS_SETTINGS_KEY_WIND_SPEED_UNIT)) goto commit_failed;

		// Rain unit
		if(full_sync || _current_settings.precip_unit != prev.precip_unit)
			if(!persist(nvs_set_u32(nvs_handle, NVS_SETTINGS_KEY_RAIN_UNIT, _current_settings.precip_unit), NVS_SETTINGS_KEY_RAIN_UNIT)) goto commit_failed;

		// Pressure unit
		if(full_sync || _current_settings.pressure_unit != prev.pressure_unit)
			if(!persist(nvs_set_u32(nvs_handle, NVS_SETTINGS_KEY_PRESSURE_UNIT, _current_settings.pressure_unit), NVS_SETTINGS_KEY_PRESSURE_UNIT)) goto commit_failed;

		// Idle backlight level
		if(full_sync || _current_settings.idle_backlight_level != prev.idle_backlight_level)
			if(!persist(nvs_set_u32(nvs_handle, NVS_SETTINGS_KEY_IDLE_BACKLIGHT_LEVEL, _current_settings.idle_backlight_level), NVS_SETTINGS_KEY_IDLE_BACKLIGHT_LEVEL)) goto commit_failed;

		// Screen rotation
		if(full_sync || _current_settings.scr_rot != prev.scr_rot)
			if(!persist(nvs_set_u32(nvs_handle, NVS_SETTINGS_KEY_SCREEN_ROTATION, _current_settings.scr_rot), NVS_SETTINGS_KEY_SCREEN_ROTATION)) goto commit_failed;

		// Debug build MQTT broker
		if(full_sync || _current_settings.mqtt_broker_id != prev.mqtt_broker_id)
			if(!persist(nvs_set_u32(nvs_handle, NVS_SETTINGS_KEY_MQTT_BROKER, _current_settings.mqtt_broker_id), NVS_SETTINGS_KEY_MQTT_BROKER)) goto commit_failed;

		// MQTT Disabled
		if(full_sync || _current_settings.mqtt_disabled != prev.mqtt_disabled)
			if(!persist(nvs_set_u8(nvs_handle, NVS_SETTINGS_KEY_MQTT_DISABLED, _current_settings.mqtt_disabled ? 1 : 0), NVS_SETTINGS_KEY_MQTT_DISABLED)) goto commit_failed;

		// Cloud Pairing Enabled
		if(full_sync || _current_settings.cloud_pairing_enabled != prev.cloud_pairing_enabled)
			if(!persist(nvs_set_u8(nvs_handle, NVS_SETTINGS_KEY_CLOUD_PAIRING_ENABLED, _current_settings.cloud_pairing_enabled ? 1 : 0), NVS_SETTINGS_KEY_CLOUD_PAIRING_ENABLED)) goto commit_failed;

		// Pairing mode
		if(full_sync || _current_settings.pair_mode != prev.pair_mode)
			if(!persist(nvs_set_u8(nvs_handle, NVS_SETTINGS_KEY_PAIR_MODE, (u8)_current_settings.pair_mode), NVS_SETTINGS_KEY_PAIR_MODE)) goto commit_failed;
		if(full_sync || _current_settings.pair_source != prev.pair_source)
			if(!persist(nvs_set_u8(nvs_handle, NVS_SETTINGS_KEY_PAIR_SOURCE, (u8)_current_settings.pair_source), NVS_SETTINGS_KEY_PAIR_SOURCE)) goto commit_failed;

		// Local pair cloud-sync pending flag
		if(full_sync || _current_settings.local_pair_sync_pending != prev.local_pair_sync_pending)
			if(!persist(nvs_set_u8(nvs_handle, NVS_SETTINGS_KEY_LOCAL_PAIR_SYNC_PENDING, _current_settings.local_pair_sync_pending ? 1 : 0), NVS_SETTINGS_KEY_LOCAL_PAIR_SYNC_PENDING)) goto commit_failed;
		
		// Debug mode enabled
		if(full_sync || _current_settings.debug_verbosity != prev.debug_verbosity)
			if(!persist(nvs_set_u32(nvs_handle, NVS_SETTINGS_DEBUG_VERBOSITY, (u32)_current_settings.debug_verbosity), NVS_SETTINGS_DEBUG_VERBOSITY)) goto commit_failed;

		// Note: last_tab is retained in RTC_NOINIT_ATTR SRAM (_rtc_last_tab_cache) so LCD tab switches never wear NVS flash.

		// Weather station ID
		if(full_sync || _current_settings.ws_id != prev.ws_id)
			if(!persist(nvs_set_i64(nvs_handle, NVS_SETTINGS_WS_ID, _current_settings.ws_id), NVS_SETTINGS_WS_ID)) goto commit_failed;

		// Weather station type
		if(full_sync || _current_settings.ws_type != prev.ws_type)
			if(!persist(nvs_set_u32(nvs_handle, NVS_SETTINGS_WS_TYPE, (u32)_current_settings.ws_type), NVS_SETTINGS_WS_TYPE)) goto commit_failed;

		// Weather station public key
		if(full_sync || memcmp(_current_settings.ws_pubkey, prev.ws_pubkey, sizeof(_current_settings.ws_pubkey)) != 0)
			if(!persist(nvs_set_blob(nvs_handle, NVS_SETTINGS_WS_PUBKEY, _current_settings.ws_pubkey, sizeof(_current_settings.ws_pubkey)), NVS_SETTINGS_WS_PUBKEY)) goto commit_failed;

		// Weather station serial number
		if(full_sync || memcmp(_current_settings.ws_sn, prev.ws_sn, sizeof(_current_settings.ws_sn)) != 0)
			if(!persist(nvs_set_blob(nvs_handle, NVS_SETTINGS_WS_SN, _current_settings.ws_sn, sizeof(_current_settings.ws_sn)), NVS_SETTINGS_WS_SN)) goto commit_failed;

		// Cloud station attributes
		if(full_sync || _current_settings.cloud_ws_id != prev.cloud_ws_id)
			if(!persist(nvs_set_i64(nvs_handle, NVS_SETTINGS_KEY_CLOUD_WS_ID, _current_settings.cloud_ws_id), NVS_SETTINGS_KEY_CLOUD_WS_ID)) goto commit_failed;
		if(full_sync || _current_settings.cloud_ws_valid != prev.cloud_ws_valid)
			if(!persist(nvs_set_u8(nvs_handle, NVS_SETTINGS_KEY_CLOUD_WS_VALID, _current_settings.cloud_ws_valid ? 1 : 0), NVS_SETTINGS_KEY_CLOUD_WS_VALID)) goto commit_failed;
		if(full_sync || _current_settings.cloud_ws_type != prev.cloud_ws_type)
			if(!persist(nvs_set_u32(nvs_handle, NVS_SETTINGS_KEY_CLOUD_WS_TYPE, (u32)_current_settings.cloud_ws_type), NVS_SETTINGS_KEY_CLOUD_WS_TYPE)) goto commit_failed;
		if(full_sync || memcmp(_current_settings.cloud_ws_sn, prev.cloud_ws_sn, sizeof(_current_settings.cloud_ws_sn)) != 0)
			if(!persist(nvs_set_blob(nvs_handle, NVS_SETTINGS_KEY_CLOUD_WS_SN, _current_settings.cloud_ws_sn, sizeof(_current_settings.cloud_ws_sn)), NVS_SETTINGS_KEY_CLOUD_WS_SN)) goto commit_failed;
		if(full_sync || memcmp(_current_settings.cloud_ws_pubkey, prev.cloud_ws_pubkey, sizeof(_current_settings.cloud_ws_pubkey)) != 0)
			if(!persist(nvs_set_blob(nvs_handle, NVS_SETTINGS_KEY_CLOUD_WS_PUBKEY, _current_settings.cloud_ws_pubkey, sizeof(_current_settings.cloud_ws_pubkey)), NVS_SETTINGS_KEY_CLOUD_WS_PUBKEY)) goto commit_failed;

		// Meshtastic Environmental Telemetry
		if(full_sync || _current_settings.mesh_env_telemetry_enabled != prev.mesh_env_telemetry_enabled)
			if(!persist(nvs_set_u8(nvs_handle, NVS_SETTINGS_KEY_MESH_ENV_EN, _current_settings.mesh_env_telemetry_enabled ? 1 : 0), NVS_SETTINGS_KEY_MESH_ENV_EN)) goto commit_failed;
		if(full_sync || _current_settings.mesh_env_telemetry_interval_sec != prev.mesh_env_telemetry_interval_sec)
			if(!persist(nvs_set_u32(nvs_handle, NVS_SETTINGS_KEY_MESH_ENV_INT, _current_settings.mesh_env_telemetry_interval_sec), NVS_SETTINGS_KEY_MESH_ENV_INT)) goto commit_failed;
		if(full_sync || _current_settings.mesh_env_telemetry_hop_limit != prev.mesh_env_telemetry_hop_limit)
			if(!persist(nvs_set_u8(nvs_handle, NVS_SETTINGS_KEY_MESH_ENV_HOP, _current_settings.mesh_env_telemetry_hop_limit), NVS_SETTINGS_KEY_MESH_ENV_HOP)) goto commit_failed;
		if(full_sync || _current_settings.mesh_tx_power_high != prev.mesh_tx_power_high)
			if(!persist(nvs_set_u8(nvs_handle, NVS_SETTINGS_KEY_MESH_TX_PWR_HIGH, _current_settings.mesh_tx_power_high ? 1 : 0), NVS_SETTINGS_KEY_MESH_TX_PWR_HIGH)) goto commit_failed;
		if(full_sync || _current_settings.mesh_modem_preset != prev.mesh_modem_preset)
			if(!persist(nvs_set_u8(nvs_handle, NVS_SETTINGS_KEY_MESH_PRESET, _current_settings.mesh_modem_preset), NVS_SETTINGS_KEY_MESH_PRESET)) goto commit_failed;
		if(full_sync || _current_settings.mesh_freq_hz != prev.mesh_freq_hz)
			if(!persist(nvs_set_u32(nvs_handle, NVS_SETTINGS_KEY_MESH_FREQ, _current_settings.mesh_freq_hz), NVS_SETTINGS_KEY_MESH_FREQ)) goto commit_failed;
		if(full_sync || _current_settings.mesh_bw_khz != prev.mesh_bw_khz)
			if(!persist(nvs_set_u16(nvs_handle, NVS_SETTINGS_KEY_MESH_BW, _current_settings.mesh_bw_khz), NVS_SETTINGS_KEY_MESH_BW)) goto commit_failed;
		if(full_sync || _current_settings.mesh_sf != prev.mesh_sf)
			if(!persist(nvs_set_u8(nvs_handle, NVS_SETTINGS_KEY_MESH_SF, _current_settings.mesh_sf), NVS_SETTINGS_KEY_MESH_SF)) goto commit_failed;
		if(full_sync || _current_settings.mesh_cr != prev.mesh_cr)
			if(!persist(nvs_set_u8(nvs_handle, NVS_SETTINGS_KEY_MESH_CR, _current_settings.mesh_cr), NVS_SETTINGS_KEY_MESH_CR)) goto commit_failed;
		if(full_sync || _current_settings.mesh_tx_power_dbm != prev.mesh_tx_power_dbm)
			if(!persist(nvs_set_i8(nvs_handle, NVS_SETTINGS_KEY_MESH_TX_PWR, _current_settings.mesh_tx_power_dbm), NVS_SETTINGS_KEY_MESH_TX_PWR)) goto commit_failed;
		if(full_sync || _current_settings.mesh_custom_name_enabled != prev.mesh_custom_name_enabled)
			if(!persist(nvs_set_u8(nvs_handle, NVS_SETTINGS_KEY_MESH_CUSTOM_NAME_EN, _current_settings.mesh_custom_name_enabled ? 1 : 0), NVS_SETTINGS_KEY_MESH_CUSTOM_NAME_EN)) goto commit_failed;
		if(full_sync || memcmp(_current_settings.mesh_custom_name, prev.mesh_custom_name, sizeof(_current_settings.mesh_custom_name)) != 0)
			if(!persist(nvs_set_blob(nvs_handle, NVS_SETTINGS_KEY_MESH_CUSTOM_NAME, _current_settings.mesh_custom_name, sizeof(_current_settings.mesh_custom_name)), NVS_SETTINGS_KEY_MESH_CUSTOM_NAME)) goto commit_failed;
		if(full_sync || memcmp(_current_settings.mesh_custom_short_name, prev.mesh_custom_short_name, sizeof(_current_settings.mesh_custom_short_name)) != 0)
			if(!persist(nvs_set_blob(nvs_handle, NVS_SETTINGS_KEY_MESH_CUSTOM_SHORT_NAME, _current_settings.mesh_custom_short_name, sizeof(_current_settings.mesh_custom_short_name)), NVS_SETTINGS_KEY_MESH_CUSTOM_SHORT_NAME)) goto commit_failed;

		if(full_sync || _current_settings.mesh_multi_station_forwarding_enabled != prev.mesh_multi_station_forwarding_enabled)
			if(!persist(nvs_set_u8(nvs_handle, NVS_SETTINGS_KEY_MESH_FWD_EN, _current_settings.mesh_multi_station_forwarding_enabled ? 1 : 0), NVS_SETTINGS_KEY_MESH_FWD_EN)) goto commit_failed;
		if(full_sync || _current_settings.meshcore_gw_telemetry_enabled != prev.meshcore_gw_telemetry_enabled)
			if(!persist(nvs_set_u8(nvs_handle, NVS_SETTINGS_KEY_MESHCORE_GW_EN, _current_settings.meshcore_gw_telemetry_enabled ? 1 : 0), NVS_SETTINGS_KEY_MESHCORE_GW_EN)) goto commit_failed;
		if(full_sync || _current_settings.meshcore_gw_telemetry_interval_sec != prev.meshcore_gw_telemetry_interval_sec)
			if(!persist(nvs_set_u32(nvs_handle, NVS_SETTINGS_KEY_MESHCORE_GW_INT, _current_settings.meshcore_gw_telemetry_interval_sec), NVS_SETTINGS_KEY_MESHCORE_GW_INT)) goto commit_failed;
		if(full_sync || _current_settings.meshcore_gw_tx_power_high != prev.meshcore_gw_tx_power_high)
			if(!persist(nvs_set_u8(nvs_handle, NVS_SETTINGS_KEY_MESHCORE_GW_PWR, _current_settings.meshcore_gw_tx_power_high ? 1 : 0), NVS_SETTINGS_KEY_MESHCORE_GW_PWR)) goto commit_failed;
		if(full_sync || _current_settings.meshcore_gw_profile_idx != prev.meshcore_gw_profile_idx)
			if(!persist(nvs_set_u8(nvs_handle, NVS_SETTINGS_KEY_MESHCORE_GW_PROF, _current_settings.meshcore_gw_profile_idx), NVS_SETTINGS_KEY_MESHCORE_GW_PROF)) goto commit_failed;
		if(full_sync || _current_settings.meshcore_gw_freq_hz != prev.meshcore_gw_freq_hz)
			if(!persist(nvs_set_u32(nvs_handle, NVS_SETTINGS_KEY_MESHCORE_GW_FREQ, _current_settings.meshcore_gw_freq_hz), NVS_SETTINGS_KEY_MESHCORE_GW_FREQ)) goto commit_failed;
		if(full_sync || _current_settings.meshcore_gw_bw_khz != prev.meshcore_gw_bw_khz)
			if(!persist(nvs_set_u16(nvs_handle, NVS_SETTINGS_KEY_MESHCORE_GW_BW, _current_settings.meshcore_gw_bw_khz), NVS_SETTINGS_KEY_MESHCORE_GW_BW)) goto commit_failed;
		if(full_sync || _current_settings.meshcore_gw_sf != prev.meshcore_gw_sf)
			if(!persist(nvs_set_u8(nvs_handle, NVS_SETTINGS_KEY_MESHCORE_GW_SF, _current_settings.meshcore_gw_sf), NVS_SETTINGS_KEY_MESHCORE_GW_SF)) goto commit_failed;
		if(full_sync || _current_settings.meshcore_gw_cr != prev.meshcore_gw_cr)
			if(!persist(nvs_set_u8(nvs_handle, NVS_SETTINGS_KEY_MESHCORE_GW_CR, _current_settings.meshcore_gw_cr), NVS_SETTINGS_KEY_MESHCORE_GW_CR)) goto commit_failed;
		if(full_sync || _current_settings.meshcore_gw_tx_power_dbm != prev.meshcore_gw_tx_power_dbm)
			if(!persist(nvs_set_i8(nvs_handle, NVS_SETTINGS_KEY_MESHCORE_GW_TX_PWR, _current_settings.meshcore_gw_tx_power_dbm), NVS_SETTINGS_KEY_MESHCORE_GW_TX_PWR)) goto commit_failed;
		if(full_sync || _current_settings.gateway_radio_mode != prev.gateway_radio_mode)
			if(!persist(nvs_set_u8(nvs_handle, NVS_SETTINGS_KEY_GW_RADIO_MODE, (_current_settings.gateway_radio_mode == GatewayRadioMode::MESHCORE_GATEWAY) ? 1 : 0), NVS_SETTINGS_KEY_GW_RADIO_MODE)) goto commit_failed;

		// Adaptive Weather Publishing Policy
		if(full_sync || memcmp(&_current_settings.weather_policy, &prev.weather_policy, sizeof(_current_settings.weather_policy)) != 0)
		{
			AdaptivePublish::WeatherPolicyWire policy_wire;
			AdaptivePublish::encode_wire(_current_settings.weather_policy, &policy_wire);
			if(!persist(nvs_set_blob(nvs_handle, NVS_SETTINGS_KEY_WX_POLICY, &policy_wire, sizeof(policy_wire)), NVS_SETTINGS_KEY_WX_POLICY)) goto commit_failed;
		}

		// Remote reboot ID
		if(full_sync || _current_settings.last_reboot_id != prev.last_reboot_id)
			if(!persist(nvs_set_u64(nvs_handle, NVS_SETTINGS_KEY_REBOOT_ID, _current_settings.last_reboot_id), NVS_SETTINGS_KEY_REBOOT_ID)) goto commit_failed;

		// Remote scan ID
		if(full_sync || _current_settings.last_scan_id != prev.last_scan_id)
			if(!persist(nvs_set_u64(nvs_handle, NVS_SETTINGS_KEY_SCAN_ID, _current_settings.last_scan_id), NVS_SETTINGS_KEY_SCAN_ID)) goto commit_failed;

		// Station ownership verification
		if(full_sync || _current_settings.station_ownership_confirmed != prev.station_ownership_confirmed)
			if(!persist(nvs_set_u8(nvs_handle, NVS_SETTINGS_KEY_OWNERSHIP_CONF, _current_settings.station_ownership_confirmed ? 1 : 0), NVS_SETTINGS_KEY_OWNERSHIP_CONF)) goto commit_failed;
		if(full_sync || _current_settings.station_ownership_id != prev.station_ownership_id)
			if(!persist(nvs_set_u32(nvs_handle, NVS_SETTINGS_KEY_OWNERSHIP_ID, _current_settings.station_ownership_id), NVS_SETTINGS_KEY_OWNERSHIP_ID)) goto commit_failed;
		if(full_sync || _current_settings.station_ownership_timestamp != prev.station_ownership_timestamp)
			if(!persist(nvs_set_u32(nvs_handle, NVS_SETTINGS_KEY_OWNERSHIP_TS, _current_settings.station_ownership_timestamp), NVS_SETTINGS_KEY_OWNERSHIP_TS)) goto commit_failed;

		// Cloud-assigned device name
		if((full_sync && _current_settings.dev_name[0] != '\0') ||
		   memcmp(_current_settings.dev_name, prev.dev_name, sizeof(_current_settings.dev_name)) != 0)
			if(!persist(nvs_set_blob(nvs_handle, NVS_SETTINGS_KEY_DEV_NAME, _current_settings.dev_name, sizeof(_current_settings.dev_name)), NVS_SETTINGS_KEY_DEV_NAME)) goto commit_failed;

		// Commit only when at least one key changed
		if(dirty_keys > 0)
		{
			esp_ret = nvs_commit(nvs_handle);
			if(!persist(esp_ret, "commit")) goto commit_failed;
		}

		nvs_close(nvs_handle);

		_nvs_synced_settings = _current_settings;
		_nvs_synced_valid = true;

		if(_changed_categories != 0)
		{
			GlobalEvents::publish(GlobalEvents::EventId::SETTINGS_CHANGED,
										&_changed_categories, sizeof(_changed_categories));
		}
		
		_changed_categories = 0;

		return RET_OK;

	commit_failed:
		nvs_close(nvs_handle);
		return RET_ERROR;
	}

	/******************************************************************************
	* Print a settings data structure
	* @param data Struct to print
	******************************************************************************/
	void print_settings(Data *data)
	{
		printf("================================\n");
		printf("Settings\n");
		printf("================================\n");

		// Sniffer freq
		const auto *freq_desc = WsHandler::get_freq_desc(nullptr, data->freq_region);

		printf("Sniffer Freq (Hz): %d\n", freq_desc != nullptr ? freq_desc->freq_region : -1);

		// Sniffer freq
		printf("Temp unit: %d\n", data->temp_unit);

		// Wind speed unit
		printf("Wind speed unit: %d\n", data->wind_speed_unit);

		// Rain unit
		printf("Precipitation unit: %d\n", data->precip_unit);

		// Pressure unit
		printf("Pressure unit: %d\n", data->pressure_unit);

		// Sniffer freq
		printf("Timezone: UTC%+d\n", data->tz_offset);

		// Idle backlight level
		printf("Bkl idle level: %d\n", data->idle_backlight_level);

		// Screen rotation
		printf("Screen rotation: %d\n", data->scr_rot);

		// MQTT broker
		printf("MQTT broker ID: %d\n", data->mqtt_broker_id);;

		// Debug enabled
		printf("Debug verbosity: %d\n", (int)data->debug_verbosity);

		// Pairing mode
		printf("Pair mode: %d (0=OFF 1=CLOUD 2=LOCAL 3=SCAN), source: %d (1=CLOUD 2=LOCAL), local sync pending: %d\n",
			(int)data->pair_mode, (int)data->pair_source,
			(int)data->local_pair_sync_pending);

		// Weather station ID
		if(data->ws_id >=0)
		{
			printf("Local WS ID: %08X\n", (uint32_t)data->ws_id);
		}
		else
		{
			printf("Local WS ID: -1\n");
		}

		// Weather station type
		printf("WS Type: %d\n", (int)data->ws_type);

		// Weather station pub key
		printf("WS PubKey:\n");
		Utils::print_buff_hex(data->ws_pubkey, sizeof(data->ws_pubkey));

		// Weather station sn
		printf("WS SN:\n");
		Utils::print_buff_hex(data->ws_sn, sizeof(data->ws_sn));

		if(data->cloud_ws_id >= 0)
		{
			printf("Cloud WS ID: %08X\n", (uint32_t)data->cloud_ws_id);
		}
		else
		{
			printf("Cloud WS ID: -1\n");
		}
		printf("Cloud WS Type: %d, valid: %d\n",
			(int)data->cloud_ws_type, (int)data->cloud_ws_valid);
		printf("Cloud WS SN:\n");
		Utils::print_buff_hex(data->cloud_ws_sn, sizeof(data->cloud_ws_sn));

		// Device name
		printf("Device Name:%s\n", data->dev_name);

		printf("================================\n");
	}

	bool get_mesh_env_telemetry_enabled()
	{
		return _current_settings.mesh_env_telemetry_enabled;
	}

	RetResult set_mesh_env_telemetry_enabled(bool enabled)
	{
		if (_current_settings.mesh_env_telemetry_enabled != enabled) {
			_current_settings.mesh_env_telemetry_enabled = enabled;
			_changed_categories |= (u32)Category::SYSTEM;
			commit();
		}
		return RET_OK;
	}

	uint32_t get_mesh_env_telemetry_interval_sec()
	{
		return _current_settings.mesh_env_telemetry_interval_sec;
	}

	RetResult set_mesh_env_telemetry_interval_sec(uint32_t interval_sec)
	{
		// Sanitize interval (0 = disabled, or at least 300s / 5m)
		if (interval_sec > 0 && interval_sec < 300) {
			interval_sec = 300;
		}
		if (_current_settings.mesh_env_telemetry_interval_sec != interval_sec) {
			_current_settings.mesh_env_telemetry_interval_sec = interval_sec;
			_changed_categories |= (u32)Category::SYSTEM;
			commit();
		}
		return RET_OK;
	}

	uint8_t get_mesh_env_telemetry_hop_limit()
	{
		return _current_settings.mesh_env_telemetry_hop_limit;
	}

	RetResult set_mesh_env_telemetry_hop_limit(uint8_t hop_limit)
	{
		if (hop_limit < 1 || hop_limit > 5) {
			return RET_INVALID_PARAMS;
		}
		if (_current_settings.mesh_env_telemetry_hop_limit != hop_limit) {
			_current_settings.mesh_env_telemetry_hop_limit = hop_limit;
			_changed_categories |= (u32)Category::SYSTEM;
			commit();
		}
		return RET_OK;
	}

	bool get_mesh_tx_power_high()
	{
		return _current_settings.mesh_tx_power_high;
	}

	RetResult set_mesh_tx_power_high(bool high)
	{
		if (_current_settings.mesh_tx_power_high != high) {
			_current_settings.mesh_tx_power_high = high;
			_changed_categories |= (u32)Category::SYSTEM;
			commit();
		}
		return RET_OK;
	}

	bool get_mesh_multi_station_forwarding_enabled()
	{
		return _current_settings.mesh_multi_station_forwarding_enabled;
	}

	RetResult set_mesh_multi_station_forwarding_enabled(bool enabled)
	{
		if (_current_settings.mesh_multi_station_forwarding_enabled != enabled) {
			_current_settings.mesh_multi_station_forwarding_enabled = enabled;
			_changed_categories |= (u32)Category::SYSTEM;
			commit();
		}
		return RET_OK;
	}

	bool get_meshcore_gw_telemetry_enabled()
	{
		return _current_settings.meshcore_gw_telemetry_enabled;
	}

	RetResult set_meshcore_gw_telemetry_enabled(bool enabled)
	{
		if (_current_settings.meshcore_gw_telemetry_enabled != enabled) {
			_current_settings.meshcore_gw_telemetry_enabled = enabled;
			_changed_categories |= (u32)Category::SYSTEM;
			commit();
		}
		return RET_OK;
	}

	uint32_t get_meshcore_gw_telemetry_interval_sec()
	{
		return _current_settings.meshcore_gw_telemetry_interval_sec;
	}

	RetResult set_meshcore_gw_telemetry_interval_sec(uint32_t interval_sec)
	{
		// Sanitize interval (0 = disabled, or at least 300s / 5m)
		if (interval_sec > 0 && interval_sec < 300) {
			interval_sec = 300;
		}
		if (_current_settings.meshcore_gw_telemetry_interval_sec != interval_sec) {
			_current_settings.meshcore_gw_telemetry_interval_sec = interval_sec;
			_changed_categories |= (u32)Category::SYSTEM;
			commit();
		}
		return RET_OK;
	}

	bool get_meshcore_gw_tx_power_high()
	{
		return _current_settings.meshcore_gw_tx_power_high;
	}

	RetResult set_meshcore_gw_tx_power_high(bool high)
	{
		if (_current_settings.meshcore_gw_tx_power_high != high) {
			_current_settings.meshcore_gw_tx_power_high = high;
			_changed_categories |= (u32)Category::SYSTEM;
			commit();
		}
		return RET_OK;
	}

	uint8_t get_meshcore_gw_profile_idx()
	{
		return _current_settings.meshcore_gw_profile_idx;
	}

	RetResult set_meshcore_gw_profile_idx(uint8_t profile_idx)
	{
		if (_current_settings.meshcore_gw_profile_idx != profile_idx) {
			_current_settings.meshcore_gw_profile_idx = profile_idx;
			_changed_categories |= (u32)Category::SYSTEM;
			commit();
		}
		return RET_OK;
	}

	uint8_t get_mesh_modem_preset()
	{
		return _current_settings.mesh_modem_preset;
	}

	RetResult set_mesh_modem_preset(uint8_t preset)
	{
		if (_current_settings.mesh_modem_preset != preset) {
			_current_settings.mesh_modem_preset = preset;
			_changed_categories |= (u32)Category::SYSTEM;
			commit();
		}
		return RET_OK;
	}

	uint32_t get_mesh_freq_hz()
	{
		return _current_settings.mesh_freq_hz;
	}

	RetResult set_mesh_freq_hz(uint32_t freq_hz)
	{
		if (_current_settings.mesh_freq_hz != freq_hz) {
			_current_settings.mesh_freq_hz = freq_hz;
			_changed_categories |= (u32)Category::SYSTEM;
			commit();
		}
		return RET_OK;
	}

	uint16_t get_mesh_bw_khz()
	{
		return _current_settings.mesh_bw_khz;
	}

	RetResult set_mesh_bw_khz(uint16_t bw_khz)
	{
		if (_current_settings.mesh_bw_khz != bw_khz) {
			_current_settings.mesh_bw_khz = bw_khz;
			_changed_categories |= (u32)Category::SYSTEM;
			commit();
		}
		return RET_OK;
	}

	uint8_t get_mesh_sf()
	{
		return _current_settings.mesh_sf;
	}

	RetResult set_mesh_sf(uint8_t sf)
	{
		if (_current_settings.mesh_sf != sf) {
			_current_settings.mesh_sf = sf;
			_changed_categories |= (u32)Category::SYSTEM;
			commit();
		}
		return RET_OK;
	}

	uint8_t get_mesh_cr()
	{
		return _current_settings.mesh_cr;
	}

	RetResult set_mesh_cr(uint8_t cr)
	{
		if (_current_settings.mesh_cr != cr) {
			_current_settings.mesh_cr = cr;
			_changed_categories |= (u32)Category::SYSTEM;
			commit();
		}
		return RET_OK;
	}

	int8_t get_mesh_tx_power_dbm()
	{
		return _current_settings.mesh_tx_power_dbm;
	}

	RetResult set_mesh_tx_power_dbm(int8_t power)
	{
		if (_current_settings.mesh_tx_power_dbm != power) {
			_current_settings.mesh_tx_power_dbm = power;
			_changed_categories |= (u32)Category::SYSTEM;
			commit();
		}
		return RET_OK;
	}

	bool get_mesh_custom_name_enabled()
	{
		return _current_settings.mesh_custom_name_enabled;
	}

	RetResult set_mesh_custom_name_enabled(bool enabled)
	{
		if (_current_settings.mesh_custom_name_enabled != enabled) {
			_current_settings.mesh_custom_name_enabled = enabled;
			_changed_categories |= (u32)Category::SYSTEM;
			commit();
		}
		return RET_OK;
	}

	const char* get_mesh_custom_name()
	{
		return _current_settings.mesh_custom_name;
	}

	RetResult set_mesh_custom_name(const char* name)
	{
		if (name == nullptr) return RET_ERROR;
		char buf[sizeof(_current_settings.mesh_custom_name)] = {0};
		strncpy(buf, name, sizeof(buf) - 1);
		if (strcmp(_current_settings.mesh_custom_name, buf) != 0) {
			strncpy(_current_settings.mesh_custom_name, buf, sizeof(_current_settings.mesh_custom_name) - 1);
			_current_settings.mesh_custom_name[sizeof(_current_settings.mesh_custom_name) - 1] = '\0';
			_changed_categories |= (u32)Category::SYSTEM;
			commit();
		}
		return RET_OK;
	}

	const char* get_mesh_custom_short_name()
	{
		return _current_settings.mesh_custom_short_name;
	}

	RetResult set_mesh_custom_short_name(const char* short_name)
	{
		if (short_name == nullptr) return RET_ERROR;
		char buf[sizeof(_current_settings.mesh_custom_short_name)] = {0};
		strncpy(buf, short_name, sizeof(buf) - 1);
		if (strcmp(_current_settings.mesh_custom_short_name, buf) != 0) {
			strncpy(_current_settings.mesh_custom_short_name, buf, sizeof(_current_settings.mesh_custom_short_name) - 1);
			_current_settings.mesh_custom_short_name[sizeof(_current_settings.mesh_custom_short_name) - 1] = '\0';
			_changed_categories |= (u32)Category::SYSTEM;
			commit();
		}
		return RET_OK;
	}

	uint32_t get_meshcore_gw_freq_hz()
	{
		return _current_settings.meshcore_gw_freq_hz;
	}

	RetResult set_meshcore_gw_freq_hz(uint32_t freq_hz)
	{
		if (_current_settings.meshcore_gw_freq_hz != freq_hz) {
			_current_settings.meshcore_gw_freq_hz = freq_hz;
			_changed_categories |= (u32)Category::SYSTEM;
			commit();
		}
		return RET_OK;
	}

	uint16_t get_meshcore_gw_bw_khz()
	{
		return _current_settings.meshcore_gw_bw_khz;
	}

	RetResult set_meshcore_gw_bw_khz(uint16_t bw_khz)
	{
		if (_current_settings.meshcore_gw_bw_khz != bw_khz) {
			_current_settings.meshcore_gw_bw_khz = bw_khz;
			_changed_categories |= (u32)Category::SYSTEM;
			commit();
		}
		return RET_OK;
	}

	uint8_t get_meshcore_gw_sf()
	{
		return _current_settings.meshcore_gw_sf;
	}

	RetResult set_meshcore_gw_sf(uint8_t sf)
	{
		if (_current_settings.meshcore_gw_sf != sf) {
			_current_settings.meshcore_gw_sf = sf;
			_changed_categories |= (u32)Category::SYSTEM;
			commit();
		}
		return RET_OK;
	}

	uint8_t get_meshcore_gw_cr()
	{
		return _current_settings.meshcore_gw_cr;
	}

	RetResult set_meshcore_gw_cr(uint8_t cr)
	{
		if (_current_settings.meshcore_gw_cr != cr) {
			_current_settings.meshcore_gw_cr = cr;
			_changed_categories |= (u32)Category::SYSTEM;
			commit();
		}
		return RET_OK;
	}

	int8_t get_meshcore_gw_tx_power_dbm()
	{
		return _current_settings.meshcore_gw_tx_power_dbm;
	}

	RetResult set_meshcore_gw_tx_power_dbm(int8_t power)
	{
		if (_current_settings.meshcore_gw_tx_power_dbm != power) {
			_current_settings.meshcore_gw_tx_power_dbm = power;
			_changed_categories |= (u32)Category::SYSTEM;
			commit();
		}
		return RET_OK;
	}

	GatewayRadioMode get_gateway_radio_mode()
	{
		return _current_settings.gateway_radio_mode;
	}

	RetResult set_gateway_radio_mode(GatewayRadioMode mode)
	{
		if (_current_settings.gateway_radio_mode != mode) {
			_current_settings.gateway_radio_mode = mode;
			_changed_categories |= (u32)Category::SYSTEM;
			commit();
		}
		return RET_OK;
	}

	AdaptivePublish::WeatherChangePolicy get_weather_policy()
	{
		return _current_settings.weather_policy;
	}

	RetResult set_weather_policy(const AdaptivePublish::WeatherChangePolicy &policy)
	{
		if (memcmp(&_current_settings.weather_policy, &policy, sizeof(AdaptivePublish::WeatherChangePolicy)) != 0) {
			_current_settings.weather_policy = policy;
			_changed_categories |= (u32)Category::SYSTEM | (u32)Category::WS;
			WsHandler::g_ws1300_weather_policy_changed = true;
			commit();
		}
		return RET_OK;
	}

	uint64_t get_last_reboot_id()
	{
		return _current_settings.last_reboot_id;
	}

	RetResult set_last_reboot_id(uint64_t id)
	{
		if (_current_settings.last_reboot_id != id) {
			_current_settings.last_reboot_id = id;
			_changed_categories |= (u32)Category::SYSTEM;
			commit();
		}
		return RET_OK;
	}

	uint64_t get_last_scan_id()
	{
		return _current_settings.last_scan_id;
	}

	RetResult set_last_scan_id(uint64_t id)
	{
		if (_current_settings.last_scan_id != id) {
			_current_settings.last_scan_id = id;
			_changed_categories |= (u32)Category::SYSTEM;
			commit();
		}
		return RET_OK;
	}

	bool is_station_ownership_confirmed()
	{
		return _current_settings.station_ownership_confirmed;
	}

	uint32_t get_station_ownership_id()
	{
		return _current_settings.station_ownership_id;
	}

	uint32_t get_station_ownership_timestamp()
	{
		return _current_settings.station_ownership_timestamp;
	}

	RetResult set_station_ownership_confirmed(bool confirmed, uint32_t station_id, uint32_t timestamp)
	{
		bool changed = false;
		if (_current_settings.station_ownership_confirmed != confirmed) {
			_current_settings.station_ownership_confirmed = confirmed;
			changed = true;
		}
		if (station_id != 0 && _current_settings.station_ownership_id != station_id) {
			_current_settings.station_ownership_id = station_id;
			changed = true;
		}
		if (timestamp != 0 && _current_settings.station_ownership_timestamp != timestamp) {
			_current_settings.station_ownership_timestamp = timestamp;
			changed = true;
		}
		if (changed) {
			_changed_categories |= (u32)Category::SYSTEM;
			commit();
		}
		return RET_OK;
	}

	RetResult copy_cloud_profile_to_local()
	{
		if (!_current_settings.cloud_ws_valid || _current_settings.cloud_ws_id < 0) {
			return RET_INVALID_STATE;
		}
		_current_settings.ws_id = _current_settings.cloud_ws_id;
		_current_settings.ws_type = _current_settings.cloud_ws_type;
		memcpy(_current_settings.ws_sn, _current_settings.cloud_ws_sn, sizeof(_current_settings.ws_sn));
		memcpy(_current_settings.ws_pubkey, _current_settings.cloud_ws_pubkey, sizeof(_current_settings.ws_pubkey));
		_current_settings.pair_source = PairSource::PAIR_SOURCE_LOCAL;
		_current_settings.pair_mode = PairMode::PAIR_MODE_LOCAL;
		_changed_categories |= (u32)Category::SYSTEM;
		return commit();
	}

	/******************************************************************************
	* Print current settings data structure
	******************************************************************************/
	void print_cur_settings()
	{
		print_settings(&_current_settings);
	}
}
