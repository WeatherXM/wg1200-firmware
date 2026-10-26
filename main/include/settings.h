#ifndef SETTINGS_H
#define SETTINGS_H

#include "struct.h"
#include "nvs.h"
#include "log.h"
#include "ws/include/ws_handler.h"
#include "ws/include/ws_base.h"
#include "ws/include/adaptive_publish.h"

namespace Settings
{

	enum class Category : u32
	{
		NONE = 0x00000000,
		UNITS = 0x00000001,
		WS = 0x00000010,
		DEBUG = 0x00000100,
		SYSTEM = 0x00001000
	};

	enum class GatewayRadioMode : uint8_t
	{
		WXM_GATEWAY = 0,
		MESHCORE_GATEWAY = 1
	};

	enum class Edition : uint8_t
	{
		WXM = 0,
		OPEN = 1
	};

	inline Edition get_edition()
	{
#if defined(CONFIG_GATEWAY_EDITION_OPEN)
		return Edition::OPEN;
#else
		return Edition::WXM;
#endif
	}

	inline bool is_open_edition()
	{
		return get_edition() == Edition::OPEN;
	}
	
	struct Data
	{
		/** Frequency region */
		WsFreqRegion freq_region;

		/** Temperature unit */
		TempUnit temp_unit;

		/** Timezone UTC offset */
		int tz_offset;

		/** Wind speed unit */
		WindSpeedUnit wind_speed_unit;

		/** Precipitation unit */
		PrecipUnit precip_unit;

		/** Pressure unit */
		PressureUnit pressure_unit;

		/** Idle backlight level */
		BacklightLevel idle_backlight_level;

		/** Screen rotation */
		ScreenRotation scr_rot;

        /** MQTT broker to use. Ignored in release builds. */
        MqttBroker::Id mqtt_broker_id;

        /** Whether MQTT client is disabled */
        bool mqtt_disabled;

        /** Whether Cloud pairing override is enabled.
         * Kept in sync with pair_mode (true only when PAIR_MODE_CLOUD) */
        bool cloud_pairing_enabled;

        /** Pairing mode (OFF/CLOUD/LOCAL/SCAN) */
        PairMode pair_mode;

        /** Station identity selected for the radio. OFF preserves this value. */
        PairSource pair_source;

        /** Retained NVS migration field. New firmware never auto-adopts a local
         * profile into the independent cloud assignment. */
        bool local_pair_sync_pending;

		/** Debug mode verbosity */
		Log::Level debug_verbosity;
		/** Last on screen tab */
		uint32_t last_tab;

		/** Weather station ID */
		int64_t ws_id;

		/** Weather station type */
		WsType ws_type;

		/** Weather station pub_key */
		u8 ws_pubkey[WS_PUB_KEY_LEN];

		/** Weather station pub_key */
		u8 ws_sn[WS_SN_LEN];

		/** Cloud-configured station ID (from rs_ws_sn / rs_ws_id shared attributes) */
		int64_t cloud_ws_id;

		/** Cloud-configured station serial number */
		u8 cloud_ws_sn[WS_SN_LEN];

		/** Cloud-configured station public key */
		u8 cloud_ws_pubkey[WS_PUB_KEY_LEN];

		/** Cloud-configured station type */
		WsType cloud_ws_type;

		/** True if valid cloud station attributes have been received */
		bool cloud_ws_valid;

		/** Device name as seen on explorer */
		char dev_name[64];

		/** Whether Meshtastic environmental telemetry send is enabled */
		bool mesh_env_telemetry_enabled;

		/** Meshtastic environmental telemetry send interval in seconds */
		uint32_t mesh_env_telemetry_interval_sec;

		/** Meshtastic environmental telemetry hop limit (1-5) */
		uint8_t mesh_env_telemetry_hop_limit;

		/** Whether Meshtastic high transmit power (22 dBm EU / 14 dBm other) is enabled instead of low power (2 dBm) */
		bool mesh_tx_power_high;

		/** Meshtastic modem preset (0=LongFast, 1=LongSlow, ..., 255=Custom) */
		uint8_t mesh_modem_preset;

		/** Meshtastic custom frequency in Hz (0 = auto) */
		uint32_t mesh_freq_hz;

		/** Meshtastic custom bandwidth in kHz (0 = auto, 125, 250, 500) */
		uint16_t mesh_bw_khz;

		/** Meshtastic custom spreading factor (0 = auto, 7-12) */
		uint8_t mesh_sf;

		/** Meshtastic custom coding rate (0 = auto, 5=4/5..8=4/8) */
		uint8_t mesh_cr;

		/** Meshtastic explicit TX power in dBm (0 = auto / follow mesh_tx_power_high, 2-22 dBm) */
		int8_t mesh_tx_power_dbm;

		/** Whether Meshtastic / MeshCore custom node name is enabled */
		bool mesh_custom_name_enabled;

		/** Custom Meshtastic / MeshCore long name (max 23 characters) */
		char mesh_custom_name[32];

		/** Custom Meshtastic / MeshCore short name (max 4 characters) */
		char mesh_custom_short_name[8];

		/** Whether forwarding observations from non-paired stations received over MeshCore to server is enabled */
		bool mesh_multi_station_forwarding_enabled;

		/** Whether MeshCore Gateway telemetry send is enabled */
		bool meshcore_gw_telemetry_enabled;

		/** MeshCore Gateway telemetry send interval in seconds */
		uint32_t meshcore_gw_telemetry_interval_sec;

		/** Whether MeshCore Gateway high transmit power (22 dBm EU / 14 dBm other) is enabled */
		bool meshcore_gw_tx_power_high;

		/** MeshCore Gateway RF profile index (0=LongRange, 1=Medium, 2=Narrow, 3=Fast) */
		uint8_t meshcore_gw_profile_idx;

		/** MeshCore Gateway custom frequency in Hz (0 = auto) */
		uint32_t meshcore_gw_freq_hz;

		/** MeshCore Gateway custom bandwidth in kHz (0 = auto, 125, 250, 500) */
		uint16_t meshcore_gw_bw_khz;

		/** MeshCore Gateway custom spreading factor (0 = auto, 7-12) */
		uint8_t meshcore_gw_sf;

		/** MeshCore Gateway custom coding rate (0 = auto, 5=4/5..8=4/8) */
		uint8_t meshcore_gw_cr;

		/** MeshCore Gateway explicit TX power in dBm (0 = auto / follow meshcore_gw_tx_power_high, 2-22 dBm) */
		int8_t meshcore_gw_tx_power_dbm;

		/** Gateway Radio Operating Mode (0 = WXM_GATEWAY, 1 = MESHCORE_GATEWAY) */
		GatewayRadioMode gateway_radio_mode;

		/** Adaptive Weather Publishing Policy */
		AdaptivePublish::WeatherChangePolicy weather_policy;

		/** Last handled remote reboot ID / timestamp to avoid boot loops */
		uint64_t last_reboot_id;

		/** Last handled remote scan ID / timestamp to avoid boot loops */
		uint64_t last_scan_id;

		/** Station ownership confirmation status */
		bool station_ownership_confirmed;
		uint32_t station_ownership_id;
		uint32_t station_ownership_timestamp;
	};

	RetResult init();
	RetResult load();

	AdaptivePublish::WeatherChangePolicy get_weather_policy();
	RetResult set_weather_policy(const AdaptivePublish::WeatherChangePolicy &policy);

	GatewayRadioMode get_gateway_radio_mode();
	RetResult set_gateway_radio_mode(GatewayRadioMode mode);

	bool get_mesh_env_telemetry_enabled();
	RetResult set_mesh_env_telemetry_enabled(bool enabled);

	uint32_t get_mesh_env_telemetry_interval_sec();
	RetResult set_mesh_env_telemetry_interval_sec(uint32_t interval_sec);

	uint8_t get_mesh_env_telemetry_hop_limit();
	RetResult set_mesh_env_telemetry_hop_limit(uint8_t hop_limit);

	bool get_mesh_tx_power_high();
	RetResult set_mesh_tx_power_high(bool high);

	uint8_t get_mesh_modem_preset();
	RetResult set_mesh_modem_preset(uint8_t preset);

	uint32_t get_mesh_freq_hz();
	RetResult set_mesh_freq_hz(uint32_t freq_hz);

	uint16_t get_mesh_bw_khz();
	RetResult set_mesh_bw_khz(uint16_t bw_khz);

	uint8_t get_mesh_sf();
	RetResult set_mesh_sf(uint8_t sf);

	uint8_t get_mesh_cr();
	RetResult set_mesh_cr(uint8_t cr);

	int8_t get_mesh_tx_power_dbm();
	RetResult set_mesh_tx_power_dbm(int8_t power);

	bool get_mesh_custom_name_enabled();
	RetResult set_mesh_custom_name_enabled(bool enabled);

	const char* get_mesh_custom_name();
	RetResult set_mesh_custom_name(const char* name);

	const char* get_mesh_custom_short_name();
	RetResult set_mesh_custom_short_name(const char* short_name);

	bool get_mesh_multi_station_forwarding_enabled();
	RetResult set_mesh_multi_station_forwarding_enabled(bool enabled);

	bool get_meshcore_gw_telemetry_enabled();
	RetResult set_meshcore_gw_telemetry_enabled(bool enabled);

	uint32_t get_meshcore_gw_telemetry_interval_sec();
	RetResult set_meshcore_gw_telemetry_interval_sec(uint32_t interval_sec);

	bool get_meshcore_gw_tx_power_high();
	RetResult set_meshcore_gw_tx_power_high(bool high);

	uint8_t get_meshcore_gw_profile_idx();
	RetResult set_meshcore_gw_profile_idx(uint8_t profile_idx);

	uint32_t get_meshcore_gw_freq_hz();
	RetResult set_meshcore_gw_freq_hz(uint32_t freq_hz);

	uint16_t get_meshcore_gw_bw_khz();
	RetResult set_meshcore_gw_bw_khz(uint16_t bw_khz);

	uint8_t get_meshcore_gw_sf();
	RetResult set_meshcore_gw_sf(uint8_t sf);

	uint8_t get_meshcore_gw_cr();
	RetResult set_meshcore_gw_cr(uint8_t cr);

	int8_t get_meshcore_gw_tx_power_dbm();
	RetResult set_meshcore_gw_tx_power_dbm(int8_t power);

    RetResult set_mqtt_broker_id(MqttBroker::Id id);
    MqttBroker::Id get_mqtt_broker_id();

    bool get_mqtt_disabled();
    RetResult set_mqtt_disabled(bool disabled);

    bool get_cloud_pairing_enabled();
    RetResult set_cloud_pairing_enabled(bool enabled);

    /** Get/set pairing mode. Setting the mode also syncs cloud_pairing_enabled. */
    PairMode get_pair_mode();
    RetResult set_pair_mode(PairMode mode);

    PairSource get_pair_source();
    RetResult set_pair_source(PairSource source);

    bool get_local_pair_sync_pending();
    RetResult set_local_pair_sync_pending(bool pending);

	bool local_ws_profile_ready();
	bool cloud_ws_profile_ready();

	int64_t get_cloud_ws_id();
	RetResult set_cloud_ws_id(int64_t id);

	bool get_cloud_ws_valid();
	RetResult set_cloud_ws_valid(bool valid);

	WsType get_cloud_ws_type();
	RetResult set_cloud_ws_type(WsType type);

	RetResult get_cloud_ws_sn(u8* buff, size_t buff_len);
	RetResult set_cloud_ws_sn(const u8* buff, size_t buff_len);

	RetResult get_cloud_ws_pubkey(u8* buff, size_t buff_len);
	RetResult set_cloud_ws_pubkey(const u8* buff, size_t buff_len);

	/** Get/set frequency */
	WsFreqRegion get_freq_region();
	RetResult set_freq_region(WsFreqRegion freq_region);

	/** Get/set station type */
	WsType get_ws_type();
	RetResult set_ws_type(WsType ws_type);

	/** Get/set station pubkey */
	RetResult get_ws_pubkey(u8* buff, size_t buff_len);
	RetResult set_ws_pubkey(u8* buff, size_t buff_len);

	/** Get/set station sn */
	RetResult get_ws_sn(u8* buff, size_t buff_len);
	RetResult set_ws_sn(u8* buff, size_t buff_len);

	/** Get/set temperature unit */
	TempUnit get_temp_unit();
	RetResult set_temp_unit(TempUnit unit);

	char* get_device_name();
	RetResult set_device_name(const char* dev_name_str);

	RetResult set_last_tab(uint32_t _last_tab);
	uint32_t get_last_tab();

	RetResult set_ws_id(int64_t _ws_id);
	int64_t get_ws_id();

	WindSpeedUnit get_wind_speed_unit();
	RetResult set_wind_speed_unit(WindSpeedUnit unit);
	PrecipUnit get_precip_unit();
	RetResult set_precip_unit(PrecipUnit unit);
	PressureUnit get_press_unit();
	RetResult set_press_unit(PressureUnit unit);

	/** Get/set timezone offset */
	int get_tz_offset();
	RetResult set_tz_offset(int tz_offset);

	BacklightLevel get_idle_backlight_level();
	RetResult set_idle_backlight_level(BacklightLevel level);

	ScreenRotation get_scr_rot();
	RetResult set_scr_rot(ScreenRotation scr_rot);

	Log::Level get_debug_mode_verbosity();
	RetResult set_debug_mode_verbosity(Log::Level val);

	uint64_t get_last_reboot_id();
	RetResult set_last_reboot_id(uint64_t id);

	uint64_t get_last_scan_id();
	RetResult set_last_scan_id(uint64_t id);

	bool is_station_ownership_confirmed();
	uint32_t get_station_ownership_id();
	uint32_t get_station_ownership_timestamp();
	RetResult set_station_ownership_confirmed(bool confirmed, uint32_t station_id = 0, uint32_t timestamp = 0);

	RetResult copy_cloud_profile_to_local();
	RetResult clear_local_ws_profile();

	RetResult commit();
	RetResult commit_blocking();

	void print_settings(Data *data);
	void print_cur_settings();
}

#endif
