#include "include/ws_1001.h"
#include "include/ws_1001_helpers.h"

namespace Ws1001
{

static const char TAG[] = "Ws1001";

/******************************************************************************
 * @brief Get gps parameters from packet
 * @param gw_ctx Gateway context
 * @param pkt_received Received packet
 * @param pkt_response Response packet
 * @param sta_ctx Status metadata used for response generation and logging
 * @return Validation and Verification result
 ******************************************************************************/
WsCommon::RetRes packet_handler(WsBase::GwCtx* gw_ctx, WsCommon::Packet* pkt_received, 
	WsCommon::Packet* pkt_response, WsBase::StaCtx* sta_ctx)
{
	WsCommon::RetRes ret = Ws1001Helpers::packet_validate(pkt_received);
	if(ret != WsCommon::RetRes::OK)
	{
		return ret;
	}

	uint32_t pkt_id;
	ret = Ws1001Decoders::decode_meta_id(pkt_received, &pkt_id);
	if(ret != WsCommon::RetRes::OK)
	{
		return ret;
	}

	if(gw_ctx->is_paired && gw_ctx->paired_info.id != pkt_id)
	{
		return WsCommon::RetRes::ERROR_BAD_ID;
	}

	Ws1001Decoders::decode_meta_id(pkt_received, &sta_ctx->pair_data.id);
	sta_ctx->pair_permitted = true;

	ret = Ws1001Helpers::packet_verify(pkt_received);
	if(ret != WsCommon::RetRes::OK)
	{
		return ret;
	}

	bool request_reboot = false;

	// ============== Generate ACK Response ========================
	pkt_response->pl_len = sizeof(Ws1001::AckPacket);
	Ws1001Helpers::calc_sta_status(pkt_received, sta_ctx);
	if(Ws1001Helpers::gen_ack_base(pkt_received, sta_ctx->req_tx_power, 
		gw_ctx->gps_needed && sta_ctx->gps_req_permitted, Ws1001::BROADCAST_INTERVAL_SEC, request_reboot, pkt_response->pl))
	{
		Ws1001Helpers::gen_ack_sig(pkt_received, pkt_response,
								&pkt_response->pl[pkt_response->pl_len], Ws1001::SIGNATURE_LEN);
		pkt_response->pl_len = sizeof(Ws1001::AckPacket) + Ws1001::SIGNATURE_LEN;
	}

	return WsCommon::RetRes::OK;
}

/******************************************************************************
 * @brief Get gps parameters from packet
 * @param pkt_received Received packet
 * @param field_pack Field pack to add sta_ctx and weather fields of the station
 * @param gps_data GPS data to add GPS fields of the station
 * @return Decoding result
 ******************************************************************************/
WsCommon::RetRes packet_decoder(WsCommon::Packet* pkt_received,	WsFields::FieldPack* field_pack, WsGps::GpsData* gps_data)
{
	for(int i = 0; i < Ws1001::FIELDS_COUNT; i++)
	{
		WsFields::Field common_field;
		WsFields::field_init(&common_field, Ws1001::FIELDS[i]);
		switch(Ws1001::FIELDS[i]->type)
		{
			case WsFields::FieldType::WEATHER_TEMP:
				common_field.decode_status =  Ws1001Decoders::decode_weather_temp(pkt_received, &common_field.value.val_float);
				break;
			case WsFields::FieldType::WEATHER_HUM:
				common_field.decode_status =  Ws1001Decoders::decode_weather_hum(pkt_received, &common_field.value.val_float);
				break;
			case WsFields::FieldType::WEATHER_WIND_DIR:
				common_field.decode_status =  Ws1001Decoders::decode_weather_wind_dir(pkt_received, &common_field.value.val_u16);
				break;
			case WsFields::FieldType::WEATHER_WIND_SPEED:
				common_field.decode_status =  Ws1001Decoders::decode_weather_wind_speed(pkt_received, &common_field.value.val_float);
				break;
			case WsFields::FieldType::WEATHER_WIND_GUST:
				common_field.decode_status =  Ws1001Decoders::decode_weather_wind_gust(pkt_received, &common_field.value.val_float);
				break;
			case WsFields::FieldType::WEATHER_ILLUMINANCE:
				common_field.decode_status =  Ws1001Decoders::decode_weather_illuminance(pkt_received, &common_field.value.val_u32);
				break;
			case WsFields::FieldType::WEATHER_SOLAR_RAD:
				common_field.decode_status =  Ws1001Decoders::decode_weather_solar_rad(pkt_received, &common_field.value.val_float);
				break;
			case WsFields::FieldType::WEATHER_UV_INDEX:
				common_field.decode_status =  Ws1001Decoders::decode_weather_uv_index(pkt_received, &common_field.value.val_u8);
				break;
			case WsFields::FieldType::WEATHER_PRECIP_CML:
				common_field.decode_status =  Ws1001Decoders::decode_weather_precip(pkt_received, &common_field.value.val_float);
				break;
			case WsFields::FieldType::META_ID:
				common_field.decode_status =  Ws1001Decoders::decode_meta_id(pkt_received, &common_field.value.val_u32);
				break;
			case WsFields::FieldType::META_PCK_COUNT:
				common_field.decode_status =  Ws1001Decoders::decode_meta_pkt_count(pkt_received, &common_field.value.val_u32);
				break;
			case WsFields::FieldType::META_TX_POWER:
				common_field.decode_status =  Ws1001Decoders::decode_meta_tx_power(pkt_received, &common_field.value.val_u16);
				break;
			case WsFields::FieldType::META_BAT_MV:
				common_field.decode_status =  Ws1001Decoders::decode_meta_bat_mv(pkt_received, &common_field.value.val_u16);
				break;
			case WsFields::FieldType::META_BAT_LOW:
				common_field.decode_status =  Ws1001Decoders::decode_meta_bat_low(pkt_received, &common_field.value.val_bool);
				break;
			case WsFields::FieldType::META_RSSI:
				common_field.decode_status =  Ws1001Decoders::decode_meta_rssi(pkt_received, &common_field.value.val_int);
				break;
			case WsFields::FieldType::META_FW_VER:
				common_field.decode_status =  Ws1001Decoders::decode_meta_fw_ver(pkt_received, &common_field.value.val_u8);
				break;
			case WsFields::FieldType::META_TX_INTERVAL:
				common_field.decode_status =  Ws1001Decoders::decode_meta_tx_interval(pkt_received, &common_field.value.val_u16);
				break;
			default:
				break;
		}
		WsCommon::RetRes ret = WsFields::add_field_to_pack(field_pack, &common_field);
		if(ret != WsCommon::RetRes::OK)
		{
			ESP_LOGE(TAG, "add_field_to_pack failed. Field: %s", common_field.info->name);
			WsCommon::print_ret_val(ret);
		}
	}

	if(Ws1001Helpers::get_packet_type(pkt_received) != Ws1001::PacketType::WEATHER_GPS_SIGNED)
	{
		return WsCommon::RetRes::OK;
	}

	float lat, lon, speed_kn, dir;
	uint32_t ts;
	Ws1001Helpers::get_gps_parameters(pkt_received, &lat, &lon, &gps_data->valid, &ts, &speed_kn, &dir);
	if(gps_data->valid)
	{
		gps_data->decoding_status = WsGps::DecodingStatus::DECODED;
		gps_data->decoding_type = WsGps::DecodingType::DIRECT;

		WsFields::Field gps_field_lat;
		WsFields::field_init(&gps_field_lat, &WsGps::FIELD_INFO_GPS_LAT);
		gps_field_lat.decode_status = WsFields::set_field_val(&gps_field_lat, lat);
		WsCommon::RetRes ret = WsFields::add_field_to_pack(&gps_data->field_pack, &gps_field_lat);
		if(ret != WsCommon::RetRes::OK)
		{
			ESP_LOGE(TAG, "add_field_to_pack failed. Field: %s", gps_field_lat.info->name);
			WsCommon::print_ret_val(ret);
		}

		WsFields::Field gps_field_lon;
		WsFields::field_init(&gps_field_lon, &WsGps::FIELD_INFO_GPS_LON);
		gps_field_lon.decode_status = WsFields::set_field_val(&gps_field_lon, lon);
		ret = WsFields::add_field_to_pack(&gps_data->field_pack, &gps_field_lon);
		if(ret != WsCommon::RetRes::OK)
		{
			ESP_LOGE(TAG, "add_field_to_pack failed. Field: %s", gps_field_lon.info->name);
			WsCommon::print_ret_val(ret);
		}

		WsFields::Field gps_field_ts;
		WsFields::field_init(&gps_field_ts, &WsGps::FIELD_INFO_GPS_TS);
		gps_field_ts.decode_status = WsFields::set_field_val(&gps_field_ts, ts);
		ret = WsFields::add_field_to_pack(&gps_data->field_pack, &gps_field_ts);
		if(ret != WsCommon::RetRes::OK)
		{
			ESP_LOGE(TAG, "add_field_to_pack failed. Field: %s", gps_field_ts.info->name);
			WsCommon::print_ret_val(ret);
		}

		WsFields::Field gps_field_speed;
		WsFields::field_init(&gps_field_speed, &WsGps::FIELD_INFO_GPS_SPEED);
		gps_field_speed.decode_status = WsFields::set_field_val(&gps_field_speed, speed_kn);
		ret = WsFields::add_field_to_pack(&gps_data->field_pack, &gps_field_speed);
		if(ret != WsCommon::RetRes::OK)
		{
			ESP_LOGE(TAG, "add_field_to_pack failed. Field: %s", gps_field_speed.info->name);
			WsCommon::print_ret_val(ret);
		}

		WsFields::Field gps_field_dir;
		WsFields::field_init(&gps_field_dir, &WsGps::FIELD_INFO_GPS_DIR);
		gps_field_dir.decode_status = WsFields::set_field_val(&gps_field_dir, dir);
		ret = WsFields::add_field_to_pack(&gps_data->field_pack, &gps_field_dir);
		if(ret != WsCommon::RetRes::OK)
		{
			ESP_LOGE(TAG, "add_field_to_pack failed. Field: %s", gps_field_dir.info->name);
			WsCommon::print_ret_val(ret);
		}

	}
	return WsCommon::RetRes::OK;
}

WsCommon::RetRes freq_resolver(u32 region_base_freq_hz, const WsBase::PairData* pair_data, u32* resolved_freq_hz)
{
	*resolved_freq_hz = region_base_freq_hz;
	return WsCommon::RetRes::OK;
}

}