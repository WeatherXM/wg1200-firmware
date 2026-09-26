#include "esp_err.h"
#include "esp_log.h"
#include "mbedtls/base64.h"
#include "wifi.h"
#include "tb_json.h"
#include "ws/include/ws_data.h"
#include "ws/include/ws_handler.h"
#include "const.h"
#include "app_config.h"
#include "utils.h"
#include "datetime_rtc.h"
#ifndef CONFIG_SCREENLESS
#include "backlight.h"
#endif
#include "credentials.h"
#include "settings.h"
#include "log.h"


namespace TBJson
{
//
// Local vars
//
const char TAG[] = "TBJson";

/******************************************************************************
* Create JSON from FO aggregate packet
* @param agg Source aggregate data packet
* @param json_doc Target JSON document
******************************************************************************/
RetResult from_ws_aggregate(WsFields::FieldPack *agg_pack, StaticJsonDocument<TB_JSON_DOC_SIZE> *json_doc)
{
	JsonArray root_array = json_doc->template to<JsonArray>();
	JsonObject json_entry = root_array.createNestedObject();

	json_entry[TB_KEY_TS] = (long long)agg_pack->ts * 1000;
	JsonObject values = json_entry.createNestedObject(TB_KEY_VALUES);
	values[TB_KEY_WS_SAMPLES] = agg_pack->agg_count;

	for(int i = 0; i < agg_pack->field_count; i++)
	{
		// Ignore cell_id or other gps related data
		if(agg_pack->fields[i].info->cat != WsFields::FieldCatType::WEATHER &&
			agg_pack->fields[i].info->cat != WsFields::FieldCatType::META)
		{
			continue;
		}

		const char* field_name = agg_pack->fields[i].info->name;
		const char* value_name = NULL;
		if (WsHandler::get_cur_station() != NULL)
		{
			value_name = WsAdapter::get_value_name(WsHandler::get_cur_station()->ADAPTER_TB,
				WsHandler::get_cur_station()->ADAPTER_TB_VALUES_COUNT, field_name);
		}

		// Search other adapters if field is not covered by station tb adapter
		if(value_name == NULL)
		{
			value_name = WsAdapter::get_value_name(WsData::ADAPTER_TB, WsData::ADAPTER_TB_VALUES_COUNT, field_name);
		}
		
		if(value_name != NULL)
		{
			switch(agg_pack->fields[i].info->data_type)
			{
				case WsFields::ValueType::BOOL:
					if(agg_pack->fields[i].info->type == WsFields::FieldType::META_BAT_LOW)
					{
						if(agg_pack->fields[i].value.val_bool)
						{
							values[value_name] = agg_pack->fields[i].value.val_bool;
						}
					}
					else values[value_name] = agg_pack->fields[i].value.val_bool;					
					break;
				case WsFields::ValueType::U8:
				case WsFields::ValueType::U16:
				case WsFields::ValueType::U32:
					if (agg_pack->fields[i].info->type == WsFields::FieldType::META_ID) {
						char hex_id[32];
						snprintf(hex_id, sizeof(hex_id), "0x%08lX", (unsigned long)agg_pack->fields[i].value.val_u32);
						values[value_name] = hex_id;
					} else if (agg_pack->fields[i].info->type == WsFields::FieldType::META_FW_VER) {
						uint8_t fw_ver = agg_pack->fields[i].value.val_u8;
						uint8_t minor = fw_ver >> 5;
						uint8_t patch = fw_ver & 0x1F;
						char ver_str[32];
						snprintf(ver_str, sizeof(ver_str), "1.%u.%u", minor, patch);
						values[value_name] = ver_str;
					} else {
						values[value_name] = agg_pack->fields[i].value.val_u32;
					}
					break;
				case WsFields::ValueType::FLOAT:
					values[value_name] = Utils::round_num(agg_pack->fields[i].value.val_float, 
															agg_pack->fields[i].info->decimals);
					break;
				case WsFields::ValueType::INT:
					values[value_name] = agg_pack->fields[i].value.val_int;
					break;
				default:
					break;
			}
		}
		else
		{
			ESP_LOGW(TAG, "from_ws_aggregate, value_name not found, Field: %s", field_name);		
		}	
	}
	return RET_OK;
}

/******************************************************************************
* Build packet structure for signing from a weather data packet.
* @param agg Weather data packet
* @param buff Output buffer
* @param buff_size Output buffer size
******************************************************************************/
RetResult signed_ws_packet_structure(WsFields::FieldPack *agg_pack, char *buff, size_t buff_size)
{
	StaticJsonDocument<TB_JSON_DOC_SIZE> signed_packet_struct_doc;

	signed_packet_struct_doc[SPS_KEY_FW_VER] = APP_VERSION;
	signed_packet_struct_doc[SPS_KEY_WS_MODEL] = agg_pack->packer_name;
	signed_packet_struct_doc[SPS_KEY_TS] = (long long)agg_pack->ts * 1000;
	signed_packet_struct_doc[SPS_KEY_WS_SAMPLES] = agg_pack->agg_count;
	
	float temp;
	if(WsFields::get_field_val_from_pack(agg_pack, WsFields::FieldType::WEATHER_TEMP, NULL, &temp) == WsCommon::RetRes::OK)
		signed_packet_struct_doc[SPS_KEY_WS_TEMP] = Utils::round_num(temp, 2);

	float hum;
	if(WsFields::get_field_val_from_pack(agg_pack, WsFields::FieldType::WEATHER_HUM, NULL, &hum) == WsCommon::RetRes::OK)
		signed_packet_struct_doc[SPS_KEY_WS_HUM] = Utils::round_num(hum, 0);

	float precip_cml;
	if(WsFields::get_field_val_from_pack(agg_pack, WsFields::FieldType::WEATHER_PRECIP_CML, NULL, &precip_cml) == WsCommon::RetRes::OK)
		signed_packet_struct_doc[SPS_KEY_WS_PRECIP_CML] = Utils::round_num(precip_cml, 2);

	float precip_rate;
	if(WsFields::get_field_val_from_pack(agg_pack, WsFields::FieldType::WEATHER_PRECIP_RATE, NULL, &precip_rate) == WsCommon::RetRes::OK)
		signed_packet_struct_doc[SPS_KEY_WS_PRECIP_RATE] = precip_rate;

	float wind_speed;
	if(WsFields::get_field_val_from_pack(agg_pack, WsFields::FieldType::WEATHER_WIND_SPEED, NULL, &wind_speed) == WsCommon::RetRes::OK)
		signed_packet_struct_doc[SPS_KEY_WS_W_SPEED] = Utils::round_num(wind_speed, 2);

	float wind_gust;
	if(WsFields::get_field_val_from_pack(agg_pack, WsFields::FieldType::WEATHER_WIND_GUST, NULL, &wind_gust) == WsCommon::RetRes::OK)
		signed_packet_struct_doc[SPS_KEY_WS_W_GUST] = Utils::round_num(wind_gust, 2);

	uint16_t wind_dir;
	if(WsFields::get_field_val_from_pack(agg_pack, WsFields::FieldType::WEATHER_WIND_DIR, NULL, &wind_dir) == WsCommon::RetRes::OK)
		signed_packet_struct_doc[SPS_KEY_WS_W_DIR] = wind_dir;

	uint8_t uv_index;
	if(WsFields::get_field_val_from_pack(agg_pack, WsFields::FieldType::WEATHER_UV_INDEX, NULL, &uv_index) == WsCommon::RetRes::OK)
		signed_packet_struct_doc[SPS_KEY_WS_UV_INDEX] = uv_index;

	float illlum;
	if(WsFields::get_field_val_from_pack(agg_pack, WsFields::FieldType::WEATHER_ILLUMINANCE, NULL, &illlum) == WsCommon::RetRes::OK)
		signed_packet_struct_doc[SPS_KEY_WS_LIGHT] = Utils::round_num(illlum, 0);

	float irrad;
	if(WsFields::get_field_val_from_pack(agg_pack, WsFields::FieldType::WEATHER_SOLAR_RAD, NULL, &irrad) == WsCommon::RetRes::OK)
		signed_packet_struct_doc[SPS_KEY_WS_SOL_RAD] = Utils::round_num(irrad, 1);

	uint32_t cell_id_pt1;
	uint32_t cell_id_pt2;
	if(WsFields::get_field_val_from_pack<uint32_t>(agg_pack, WsFields::FieldType::NONE, WsGps::FIELD_INFO_GPS_CELL_ID_PT1.name, &cell_id_pt1) == WsCommon::RetRes::OK
		&& WsFields::get_field_val_from_pack(agg_pack, WsFields::FieldType::NONE, WsGps::FIELD_INFO_GPS_CELL_ID_PT2.name, &cell_id_pt2) == WsCommon::RetRes::OK)
	{
		// Cell_id unpacking, cell_id_pt1 = cell_id[0:3], cell_id_pt2 = cell_id[4:7]
		uint64_t cell_id;
		uint8_t* cell_id_byte_ptr = (uint8_t*) &cell_id;
		memcpy(cell_id_byte_ptr, &cell_id_pt1, (sizeof(cell_id)/2));
		memcpy(&cell_id_byte_ptr[(sizeof(cell_id)/2)], &cell_id_pt2, (sizeof(cell_id)/2));
		
		char cell_id_str_buff[17] = {};
		WsGps::get_cell_id_str(cell_id, cell_id_str_buff, sizeof(cell_id_str_buff));
		signed_packet_struct_doc[SPS_KEY_WS_CELL_ID] = cell_id_str_buff;
	}
	else
	{
		ESP_LOGW(TAG, "signed_ws_packet_structure: cell_id not in gps data field_pack");
	}

	// char json_buff[1024] = {}; // debug
	// serializeJsonPretty(signed_packet_struct_doc, json_buff, sizeof(json_buff)); // debug
	// printf("Signed Struct: %s\n", json_buff); // debug

	serializeJson(signed_packet_struct_doc, buff, buff_size);

	return RET_OK;
}

/******************************************************************************
* Build packet structure for signing from a GPS data packet
* @param data GPS data packet
* @param buff Output buffer
* @param buff_size Output buffer size
******************************************************************************/
RetResult signed_gps_packet_structure(WsGps::GpsData* gps_data, char *buff, size_t buff_size)
{
	StaticJsonDocument<TB_JSON_DOC_SIZE> signed_packet_struct_doc;

	signed_packet_struct_doc[SPS_KEY_FW_VER] = APP_VERSION;
	signed_packet_struct_doc[SPS_KEY_WS_MODEL] = gps_data->field_pack.packer_name;
	// TODO: Which TS?
	signed_packet_struct_doc[SPS_KEY_TS] = (long long)gps_data->field_pack.ts * 1000;

	float lat;
	if(WsFields::get_field_val_from_pack(&gps_data->field_pack, WsFields::FieldType::GPS_LAT, NULL, &lat) == WsCommon::RetRes::OK)
		signed_packet_struct_doc[SPS_KEY_GPS_LAT] = Utils::round_num(lat , GPS_DEG_DEC);

	float lon;
	if(WsFields::get_field_val_from_pack(&gps_data->field_pack, WsFields::FieldType::GPS_LON, NULL, &lon) == WsCommon::RetRes::OK)
		signed_packet_struct_doc[SPS_KEY_GPS_LON] = Utils::round_num(lon , GPS_DEG_DEC);

	// char json_buff[1024] = {}; // debug
	// serializeJsonPretty(signed_packet_struct_doc, json_buff, sizeof(json_buff)); // debug
	// printf("Signed Struct: %s\n", json_buff); // debug

	serializeJson(signed_packet_struct_doc, buff, buff_size);

	return RET_OK;
}

/******************************************************************************
* Sign the provided buffer and append both the signature and original packet
* as base64, as telemetry values to the provided JsonDoc
* The provided JsonDoc is expected to contain a single TB telemetry packet
* structure. ie. [ { "values": {...} }]
* @param input Input buffer of the JSON to be signed and appended as telemetry
* @param json_doc Output, target JSON document
* @param key_prefix Prefix to use for keys
******************************************************************************/
RetResult append_telemetry_signature(char *input, StaticJsonDocument<TB_JSON_DOC_SIZE> *json_doc, const char *key_prefix)
{
	int mbed_ret = 0;
	size_t b64_res_len = 0;

	//
	// Build base64 of input packet
	//

	// Max buffer size required to hold full packet + redundant space for padding + null
	// TODO: Buffer can be large, check for possible stack overflow
	char b64_buff[ (TB_JSON_DOC_SIZE / 3) * 4 + 4 + 1 ] = "";

	mbed_ret = mbedtls_base64_encode((uint8_t*)b64_buff, sizeof(b64_buff), &b64_res_len, (uint8_t*)input, strlen(input));
	if(mbed_ret != 0)
	{
		ESP_LOGE(TAG, "Could not base64 encode data, error: %d", mbed_ret);
		return RET_ERROR;
	}

	//
	// Sign base64 of packet
	//
	ESP_LOGW(TAG, "Size of b64 buff: %d, result; %d, input: %d", strlen(b64_buff), b64_res_len, strlen(input));
	uint8_t sig_buff[DEVICE_SIG_BUFF_SIZE];
	RetResult ret = Utils::device_sign((uint8_t*)b64_buff, strlen(b64_buff), sig_buff, sizeof(sig_buff));
	if(ret != RET_OK)
	{
		ESP_LOGE(TAG, "Could not sign telemetry packet, error: %d", ret);
		return RET_ERROR;
	}

	// Convert signature to base64
	char sig_b64[(DEVICE_SIG_BUFF_SIZE / 3) * 4 + 4 + 1] = "";
	b64_res_len = 0;

	mbed_ret = mbedtls_base64_encode((uint8_t*)sig_b64, sizeof(sig_b64), &b64_res_len, (uint8_t*)sig_buff, sizeof(sig_buff));
	if(mbed_ret != 0)
	{
		ESP_LOGE(TAG, "Could not base64 encode sig, error: %d", mbed_ret);
		return RET_ERROR;
	}

	//
	// Add fields to provided JSON doc
	//
	// Build key names
	char key_packet_b64[20] = "", key_signature[20] = "";
	snprintf(key_packet_b64, sizeof(key_packet_b64), "%s%s", key_prefix, TB_KEY_PACKET_BASE64_POSTFIX);
	snprintf(key_signature, sizeof(key_signature), "%s%s", key_prefix, TB_KEY_PACKET_SIGNATURE_POSTFIX);

	JsonObject obj = json_doc->getElement(0);

	// Add to JSON
	obj["values"][key_packet_b64] = b64_buff;
	obj["values"][key_signature] = sig_b64;

	return RET_OK;
}

/******************************************************************************
* Add IPFS hash key to an already build TB packet
* @param JSON doc already containing TB telemetry structure
* @param hash IPFS file hash to add
******************************************************************************/
RetResult add_ipfs_hash_to_values(StaticJsonDocument<TB_JSON_DOC_SIZE> *json_doc, char *hash)
{
	json_doc->getElement(0)[TB_KEY_VALUES][TB_KEY_IPFS_HASH] = hash;

	return RET_OK;
}

/******************************************************************************
* Create JSON from GPS data packet
* @param agg Source aggregate data packet
* @param json_doc Target JSON document
******************************************************************************/
RetResult from_gps_data(WsGps::GpsData* gps_data, StaticJsonDocument<TB_JSON_DOC_SIZE> *json_doc)
{
	JsonArray root_array = json_doc->template to<JsonArray>();
	JsonObject json_entry = root_array.createNestedObject();

	json_entry[TB_KEY_TS] = (long long)gps_data->field_pack.ts * 1000;
	JsonObject values = json_entry.createNestedObject(TB_KEY_VALUES);

	values[TB_KEY_FIRMWARE_VERSION] = APP_VERSION;
	values[TB_KEY_WEATHER_STATION_MODEL] = gps_data->field_pack.packer_name;

	for(int i = 0; i < gps_data->field_pack.field_count; i++)
	{
		const char* field_name = gps_data->field_pack.fields[i].info->name;
		const char* value_name = WsAdapter::get_value_name(WsGps::ADAPTER_TB,
			WsGps::ADAPTER_TB_VALUES_COUNT, field_name);
		
		if(value_name !=  NULL)
		{
			switch(gps_data->field_pack.fields[i].info->data_type)
			{
				case WsFields::ValueType::BOOL:
					values[value_name] = gps_data->field_pack.fields[i].value.val_bool;					
					break;
				case WsFields::ValueType::U8:
				case WsFields::ValueType::U16:
				case WsFields::ValueType::U32:
					values[value_name] = gps_data->field_pack.fields[i].value.val_u32;
					break;
				case WsFields::ValueType::FLOAT:
					values[value_name] = Utils::round_num(gps_data->field_pack.fields[i].value.val_float, 
															gps_data->field_pack.fields[i].info->decimals);
					break;
				case WsFields::ValueType::INT:
					values[value_name] = gps_data->field_pack.fields[i].value.val_int;
					break;
				default:
					break;
			}
		}
		else
		{
			ESP_LOGE(TAG, "from_gps_data, value_name not found");
		}	
	}

	return RET_OK;
}

/******************************************************************************
* Build one-time submit attributes
* @param json_doc Target JSON document
******************************************************************************/
RetResult build_one_time_attr(StaticJsonDocument<TB_JSON_DOC_SIZE> *json_doc)
{
	ESP_LOGI(TAG, "Building one time attributes");
	JsonObject root_obj = json_doc->template to<JsonObject>();

	// Current FW version
	root_obj[TB_ATTR_GW_FW_VERSION] = APP_VERSION;

	// Standard ThingsBoard OTA current version reporting
	root_obj["current_fw_version"] = APP_VERSION;
	root_obj["current_fw_title"] = "WG1200 Miner";
	root_obj["fw_state"] = "UPDATED";

	// Legacy version key compatibility
	root_obj["fw_v"] = APP_VERSION;

	// Bootloader version
	root_obj[TB_ATTR_GW_BOOTLOADER_VERSION] = Utils::get_bootloader_version();
	root_obj["gw_bootloader_sha256"] = Utils::get_bootloader_sha256();

	const Utils::BootDiagnostics &boot_diag = Utils::get_boot_diagnostics();
	root_obj["gw_reset_reason"] = (int)boot_diag.reset_reason;
	root_obj["gw_raw_reset_cpu0"] = boot_diag.raw_reset_reason_cpu0;
	root_obj["gw_raw_reset_cpu1"] = boot_diag.raw_reset_reason_cpu1;
	root_obj["gw_controlled_restart_reason"] =
		Utils::restart_reason_to_str(boot_diag.controlled_restart_reason);
	root_obj["gw_bootloader_factory_reset"] = boot_diag.bootloader_factory_reset;
	root_obj["gw_running_partition"] = boot_diag.running_partition;
	root_obj["gw_gpio10_at_app_entry"] = boot_diag.gpio10_level_at_app_entry;
	root_obj["gw_gpio38_at_app_entry"] = boot_diag.gpio38_level_at_app_entry;
	root_obj["gw_nvs_init_initial"] = (int)boot_diag.nvs_initial_result;
	root_obj["gw_nvs_init_final"] = (int)boot_diag.nvs_final_result;
	root_obj["gw_nvs_used_entries"] = (unsigned)boot_diag.nvs_used_entries;
	root_obj["gw_nvs_free_entries"] = (unsigned)boot_diag.nvs_free_entries;
	root_obj["gw_nvs_total_entries"] = (unsigned)boot_diag.nvs_total_entries;
	root_obj["gw_nvs_namespaces"] = (unsigned)boot_diag.nvs_namespace_count;

	// Device serial number
	root_obj[TB_ATTR_GW_SERIAL_NUM] = Credentials::get_serial_str();

	// Mac addr
	char buff[30] = "";
	if(Utils::get_mac_addr_str(buff, sizeof(buff)) == RET_OK)
	{
		root_obj[TB_ATTR_GW_MAC_ADDR] = buff;
	}

	// eFuse Block 3
	// char blk3_buff[(EFUSE_BLOCK_SIZE/8) * 2 + 1] = "";
	// if(Utils::get_efuse_blk3(blk3_buff, sizeof(blk3_buff)) == RET_OK)
	// {
	// 	root_obj[TB_ATTR_EFUSE_BLK3] = blk3_buff;
	// }

	return RET_OK;
}

/******************************************************************************
* Build client attributes request from current settings
* @param json_doc Target JSON document
******************************************************************************/
RetResult build_client_attr(StaticJsonDocument<TB_JSON_DOC_SIZE> *json_doc)
{
	JsonObject root_obj = json_doc->template to<JsonObject>();

	//
	// Current status
	//
	char buff[30] = "";

	// System time
	if(DatetimeRtc::cur_time_valid())
	{
		snprintf(buff, sizeof(buff), "%" PRId64, DatetimeRtc::get_tstamp());
		root_obj[TB_ATTR_GW_SYSTEM_TIME] = buff;
	}
	

	// System tick
	snprintf(buff, sizeof(buff), "%llu", Utils::get_tick_ms());
	root_obj[TB_ATTR_GW_SYSTEM_TICK_MS] = buff;

	// Wifi RSSI
	snprintf(buff, sizeof(buff), "%d", Wifi::get_rssi());
	root_obj[TB_ATTR_GW_WIFI_RSSI] = buff;

	//
	// Current settings
	//
	// Current WS freq. friendly name
	snprintf(buff, sizeof(buff), "%lu", (u32) Settings::get_freq_region());
	root_obj[TB_ATTR_GW_FREQ_NAME] = buff;

	// Current WS weather station ID
	uint32_t paired_id = WsHandler::get_paired_id();
	if (paired_id == 0 && Settings::cloud_ws_profile_ready())
	{
		paired_id = (uint32_t)Settings::get_cloud_ws_id();
	}
	else if (paired_id == 0 && Settings::local_ws_profile_ready())
	{
		paired_id = (uint32_t)Settings::get_ws_id();
	}

	if (paired_id > 0 && paired_id != UINT32_MAX)
	{
		snprintf(buff, sizeof(buff), "0x%08lX", (unsigned long)paired_id);
		root_obj[TB_ATTR_WS_ID] = buff;
	}

	// Current WS reception
	snprintf(buff, sizeof(buff), "%d", WsHandler::get_packet_reception());
	root_obj[TB_ATTR_WS_RECEPTION] = buff;

	// Current WS Status
	snprintf(buff, sizeof(buff), "%d", (int) WsHandler::get_activity_status());
	root_obj[TB_ATTR_WS_STATUS] = buff;

	// Current WS Status Duration
	snprintf(buff, sizeof(buff), "%lu", WsHandler::get_activity_status_duration_sec());
	root_obj[TB_ATTR_WS_STATUS_DUR] = buff;

	// Current pairing mode
	root_obj[TB_ATTR_PAIR_MODE] = Utils::pair_mode_to_str(Settings::get_pair_mode());

	// WS Station RSSI and SNR when debug level is DEBUG
	if (Settings::get_debug_mode_verbosity() == Log::Level::DEBUG)
	{
		int16_t st_rssi = 0;
		int8_t st_snr = 0;
		if (WsHandler::get_station_signal(&st_rssi, &st_snr))
		{
			snprintf(buff, sizeof(buff), "%d", (int)st_rssi);
			root_obj["ws_rssi"] = buff;
			snprintf(buff, sizeof(buff), "%d", (int)st_snr);
			root_obj["ws_snr"] = buff;
		}
	}

	// WS Station firmware warning attribute
	const char *fw_warn = WsHandler::get_station_fw_warning();
	root_obj[TB_ATTR_WS_FW_WARN] = (fw_warn != nullptr && fw_warn[0] != '\0') ? fw_warn : "OK";

	// WS Station last packet error attribute
	const char *last_err = WsHandler::get_station_last_err();
	root_obj[TB_ATTR_WS_LAST_ERR] = (last_err != nullptr && last_err[0] != '\0') ? last_err : "OK";

	// WS Station sensor error attribute
	const char *sensor_err = WsHandler::get_station_sensor_err();
	root_obj[TB_ATTR_WS_SENSOR_ERR] = (sensor_err != nullptr && sensor_err[0] != '\0') ? sensor_err : "OK";

	// WS Station GPS lifecycle status attribute
	const char *gps_status = WsHandler::get_station_gps_status();
	root_obj[TB_ATTR_WS_GPS_STATUS] = (gps_status != nullptr && gps_status[0] != '\0') ? gps_status : "IDLE";

	// WS Station link ACK loss and cumulative lost packets
	root_obj[TB_ATTR_WS_ACK_LOSS] = WsHandler::get_station_ack_loss();
	root_obj[TB_ATTR_WS_LOST_PACKETS] = WsHandler::get_station_lost_packets();

#ifndef CONFIG_SCREENLESS
	root_obj["gw_backlight_on"] = Backlight::is_screen_on();
	root_obj["gw_backlight_last_wake"] = Backlight::get_last_wake_reason();
	root_obj["gw_backlight_wake_count"] = Backlight::get_wake_count();
#endif

	return RET_OK;
}

/******************************************************************************
* Build client attributes describing the locally paired station.
* Retained for an explicit, user-authorized adoption flow; SCAN does not call it.
* @param json_doc Target JSON document
******************************************************************************/
RetResult build_local_pair_attr(StaticJsonDocument<TB_JSON_DOC_SIZE> *json_doc)
{
	JsonObject root_obj = json_doc->template to<JsonObject>();

	// Bootloader version
	root_obj[TB_ATTR_GW_BOOTLOADER_VERSION] = Utils::get_bootloader_version();

	char buff[2 * sizeof(Settings::Data::ws_pubkey) + 1] = "";

	snprintf(buff, sizeof(buff), "0x%08lX", (unsigned long)WsHandler::get_paired_id());
	root_obj[TB_ATTR_LOCAL_WS_ID] = buff;

	WsType ws_type = Settings::get_ws_type();
	root_obj[TB_ATTR_LOCAL_WS_MODEL] = (ws_type == WsType::WS_TYPE_1300) ? WS_TYPE_TITLE_WS1300 : WS_TYPE_TITLE_WS1001;

	u8 ws_sn[sizeof(Settings::Data::ws_sn)];
	Settings::get_ws_sn(ws_sn, sizeof(ws_sn));
	Utils::buff_to_hex_str(ws_sn, sizeof(ws_sn), buff, sizeof(buff));
	root_obj[TB_ATTR_LOCAL_WS_SN] = buff;

	u8 ws_pubkey[sizeof(Settings::Data::ws_pubkey)];
	Settings::get_ws_pubkey(ws_pubkey, sizeof(ws_pubkey));
	Utils::buff_to_hex_str(ws_pubkey, sizeof(ws_pubkey), buff, sizeof(buff));
	root_obj[TB_ATTR_LOCAL_WS_PUBKEY] = buff;

	if(DatetimeRtc::cur_time_valid())
	{
		snprintf(buff, sizeof(buff), "%" PRId64, DatetimeRtc::get_tstamp());
		root_obj[TB_ATTR_LOCAL_PAIR_TS] = buff;
	}

	return RET_OK;
}

/******************************************************************************
* Create JSON from log entry and append to document.
* Caller is responsible for clearing doc
* @param entry Source entry
* @param json_doc Target JSON document
******************************************************************************/
RetResult append_from_log_entry(const Log::Entry *entry, StaticJsonDocument<TB_JSON_DOC_SIZE> *json_doc)
{
	JsonArray root_array = json_doc->as<JsonArray>();
	// JsonArray root_array = json_doc->getOrAddElement(0);
	JsonObject json_entry = root_array.createNestedObject();

	json_entry[TB_KEY_TS] = (long long)entry->tstamp * 1000;
	JsonObject values = json_entry.createNestedObject(TB_KEY_VALUES);

	if(entry->type == Log::Entry::LOG_TYPE_CODE)
	{
		//ESP_LOGI(TAG, "LOG_TYPE_CODE");
		values["log_code"] = (u32)entry->code;
		values["log_meta1"] = entry->meta1;
		values["log_meta2"] = entry->meta2;
	}
	else if(entry->type == Log::Entry::LOG_TYPE_MSG)
	{
		// If not cast to char* ArdJson decides to use UTF escape sequences \0000 for some reason
		//ESP_LOGI(TAG, "LOG_TYPE_MSG");
		values["log_msg"] = (char*)entry->msg;
	}
	else
	{
		ESP_LOGE(TAG, "Wrong LOG_TYPE");
		return RET_ERROR;
	}

	return RET_OK;
}

}
