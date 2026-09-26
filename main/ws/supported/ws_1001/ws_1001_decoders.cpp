#include "include/ws_1001_decoders.h"
#include "include/ws_1001_helpers.h"
#include "utils.h"

namespace Ws1001Decoders
{

/******************************************************************************
 * @brief Decode temperature
 * @param pkt Packet received
 * @param val Decoded value pointer
 * @retval ERROR_OUT_OF_BOUNDS if packet is too short to decode value
 * @retval ERROR_BAD_RESULT if value reports error code
 * @retval OK
 ******************************************************************************/
WsCommon::RetRes decode_weather_temp(WsCommon::Packet* pkt, float* val)
{
	if(pkt->pl_len <= 15) return WsCommon::RetRes::ERROR_OUT_OF_BOUNDS;

	uint16_t temp_raw = pkt->pl[14] << 8 | pkt->pl[15];
	temp_raw = temp_raw >> 4;
	float temp_f;
	if (temp_raw == 0x07FF)
	{
		return WsCommon::RetRes::ERROR_BAD_RESULT;
	}
	else if ((temp_raw & 0x0800) != 0)
	{
		temp_f = (float)((int16_t)(temp_raw | 0xF000) / 10);
	}
	else
	{
		temp_f = temp_raw / 10.0;
	}

	// Why the F is temp in F???
	// Hide it before people find out
	*val = (temp_f - 32.0f) * (5.0f / 9.0f);
	return WsCommon::RetRes::OK;
}

/******************************************************************************
 * @brief Decode humidity
 * @param pkt Packet received
 * @param val Decoded value pointer
 * @retval ERROR_OUT_OF_BOUNDS if packet is too short to decode value
 * @retval ERROR_BAD_RESULT if value reports error code
 * @retval OK
 ******************************************************************************/
WsCommon::RetRes decode_weather_hum(WsCommon::Packet* pkt, float* val)
{
	if(pkt->pl_len <= 16) return WsCommon::RetRes::ERROR_OUT_OF_BOUNDS;

	uint16_t hum_raw = pkt->pl[15] << 8 | pkt->pl[16];
	hum_raw &= 0x03FF;
	if (hum_raw == 0x03FF)
	{
		return WsCommon::RetRes::ERROR_BAD_RESULT;
	}
	else
	{
		*val = hum_raw / 10.0f;
	}
	return WsCommon::RetRes::OK;
}

/******************************************************************************
 * @brief Decode wind direction
 * @param pkt Packet received
 * @param val Decoded value pointer
 * @retval ERROR_OUT_OF_BOUNDS if packet is too short to decode value
 * @retval ERROR_BAD_RESULT if value reports error code
 * @retval OK
 ******************************************************************************/
WsCommon::RetRes decode_weather_wind_dir(WsCommon::Packet* pkt, uint16_t* val)
{
	if(pkt->pl_len <= 20) return WsCommon::RetRes::ERROR_OUT_OF_BOUNDS;

	uint16_t wind_dir_raw = pkt->pl[19] | ((pkt->pl[20] & 0x80) << 1);
	if (wind_dir_raw == 0x01FF)
	{
		return WsCommon::RetRes::ERROR_BAD_RESULT;
	}
	else
	{
		*val = wind_dir_raw;
	}
	return WsCommon::RetRes::OK;
}

/******************************************************************************
 * @brief Decode wind speed
 * @param pkt Packet received
 * @param val Decoded value pointer
 * @retval ERROR_OUT_OF_BOUNDS if packet is too short to decode value
 * @retval ERROR_BAD_RESULT if value reports error code
 * @retval OK
 ******************************************************************************/
WsCommon::RetRes decode_weather_wind_speed(WsCommon::Packet* pkt, float* val)
{
	if(pkt->pl_len <= 17) return WsCommon::RetRes::ERROR_OUT_OF_BOUNDS;

	uint16_t wind_speed_raw = pkt->pl[17];
	wind_speed_raw = ((pkt->pl[15] & 0x08) << 5) | wind_speed_raw;

	if (wind_speed_raw == 0x01FF)
	{
		return WsCommon::RetRes::ERROR_BAD_RESULT;
	}
	else
	{
		uint8_t fw_ver = pkt->pl[1];
		*val = wind_speed_raw / (fw_ver < 0x40 ? 2.f : 10.f);
	}
	return WsCommon::RetRes::OK;
}

/******************************************************************************
 * @brief Decode wind gust
 * @param pkt Packet received
 * @param val Decoded value pointer
 * @retval ERROR_OUT_OF_BOUNDS if packet is too short to decode value
 * @retval ERROR_BAD_RESULT if value reports error code
 * @retval OK
 ******************************************************************************/
WsCommon::RetRes decode_weather_wind_gust(WsCommon::Packet* pkt, float* val)
{
	if(pkt->pl_len <= 18) return WsCommon::RetRes::ERROR_OUT_OF_BOUNDS;

	uint16_t wind_gust_raw = pkt->pl[18];
	wind_gust_raw = ((pkt->pl[15] & 0x04) << 6) | wind_gust_raw;

	if (wind_gust_raw == 0x01FF)
	{
		return WsCommon::RetRes::ERROR_BAD_RESULT;
	}
	else
	{
		uint8_t fw_ver = pkt->pl[1];
		*val = wind_gust_raw / (fw_ver < 0x40 ? 2.f : 10.f);
	}
	return WsCommon::RetRes::OK;
}

/******************************************************************************
 * @brief Decode illuminance
 * @param pkt Packet received
 * @param val Decoded value pointer
 * @retval ERROR_OUT_OF_BOUNDS if packet is too short to decode value
 * @retval ERROR_BAD_RESULT if value reports error code
 * @retval OK
 ******************************************************************************/
WsCommon::RetRes decode_weather_illuminance(WsCommon::Packet* pkt, uint32_t* val)
{
	if(pkt->pl_len <= 21) return WsCommon::RetRes::ERROR_OUT_OF_BOUNDS;

	uint16_t illum_raw = (pkt->pl[20] & 0x7F) << 8 | pkt->pl[21];
	if (illum_raw == 0x7FFF)
	{
		return WsCommon::RetRes::ERROR_BAD_RESULT;
	}
	else
	{
		*val = illum_raw * 10;
	}

	return WsCommon::RetRes::OK;
}

/******************************************************************************
 * @brief Decode solar radiation
 * @param pkt Packet received
 * @param val Decoded value pointer
 * @retval ERROR_OUT_OF_BOUNDS if packet is too short to decode value
 * @retval ERROR_BAD_RESULT if value reports error code
 * @retval OK
 ******************************************************************************/
WsCommon::RetRes decode_weather_solar_rad(WsCommon::Packet* pkt, float* val)
{
	uint32_t illum;
	WsCommon::RetRes ret = decode_weather_illuminance(pkt, &illum);
	*val = illum * 0.007892f;
	return ret;
}

/******************************************************************************
 * @brief Decode UV Index
 * @param pkt Packet received
 * @param val Decoded value pointer
 * @retval ERROR_OUT_OF_BOUNDS if packet is too short to decode value
 * @retval ERROR_BAD_RESULT if value reports error code
 * @retval OK
 ******************************************************************************/
WsCommon::RetRes decode_weather_uv_index(WsCommon::Packet* pkt, uint8_t* val)
{
	if(pkt->pl_len <= 22) return WsCommon::RetRes::ERROR_OUT_OF_BOUNDS;
	*val = (uint8_t) ((float)pkt->pl[22]) * 0.1f;
	return WsCommon::RetRes::OK;
}

/******************************************************************************
 * @brief Decode accumulated precipitation
 * @param pkt Packet received
 * @param val Decoded value pointer
 * @retval ERROR_OUT_OF_BOUNDS if packet is too short to decode value
 * @retval ERROR_BAD_RESULT if value reports error code
 * @retval OK
 ******************************************************************************/
WsCommon::RetRes decode_weather_precip(WsCommon::Packet* pkt, float* val)
{
	if(pkt->pl_len <= 24) return WsCommon::RetRes::ERROR_OUT_OF_BOUNDS;
	*val = pkt->pl[23] << 8 | pkt->pl[24];
	*val *= 0.254;
	return WsCommon::RetRes::OK;
}

/******************************************************************************
 * @brief Decode station ID
 * @param pkt Packet received
 * @param val Decoded value pointer
 * @retval ERROR_OUT_OF_BOUNDS if packet is too short to decode value
 * @retval ERROR_BAD_RESULT if value reports error code
 * @retval OK
 ******************************************************************************/
WsCommon::RetRes decode_meta_id(WsCommon::Packet* pkt, uint32_t* val)
{
	const int PACKET_WS_ID_INDEX = 3;
	if(pkt->pl_len <= PACKET_WS_ID_INDEX + 3) return WsCommon::RetRes::ERROR_OUT_OF_BOUNDS;

	*val = (pkt->pl[PACKET_WS_ID_INDEX] << 24 | 
			pkt->pl[PACKET_WS_ID_INDEX+1] << 16 | 
			pkt->pl[PACKET_WS_ID_INDEX+2] << 8 | 
			pkt->pl[PACKET_WS_ID_INDEX+3]);
	return WsCommon::RetRes::OK;
}

/******************************************************************************
 * @brief Decode station packet count
 * @param pkt Packet received
 * @param val Decoded value pointer
 * @retval ERROR_OUT_OF_BOUNDS if packet is too short to decode value
 * @retval ERROR_BAD_RESULT if value reports error code
 * @retval OK
 ******************************************************************************/
WsCommon::RetRes decode_meta_pkt_count(WsCommon::Packet* pkt, uint32_t* val)
{
	const int PACKET_PACKET_COUNT_INDEX = 7;
	if(pkt->pl_len <= PACKET_PACKET_COUNT_INDEX + 1) return WsCommon::RetRes::ERROR_OUT_OF_BOUNDS;
	*val = pkt->pl[PACKET_PACKET_COUNT_INDEX] << 8 | pkt->pl[PACKET_PACKET_COUNT_INDEX+1];
	return WsCommon::RetRes::OK;
}

/******************************************************************************
 * @brief Decode station TX Power
 * @param pkt Packet received
 * @param val Decoded value pointer
 * @retval ERROR_OUT_OF_BOUNDS if packet is too short to decode value
 * @retval ERROR_BAD_RESULT if value reports error code
 * @retval OK
 ******************************************************************************/
WsCommon::RetRes decode_meta_tx_power(WsCommon::Packet* pkt, uint16_t* val)
{
	const int PACKET_POWER_INDEX = 9;
	if(pkt->pl_len <= PACKET_POWER_INDEX) return WsCommon::RetRes::ERROR_OUT_OF_BOUNDS;
	uint8_t fw_ver = pkt->pl[1];
	if (fw_ver >= 0x40)
	{
		*val = pkt->pl[PACKET_POWER_INDEX] & 0x3F;
	}
	else
	{
		*val = pkt->pl[PACKET_POWER_INDEX] & 0x7F;
	}
	return WsCommon::RetRes::OK;
}

/******************************************************************************
 * @brief Decode station battery level millivolts
 * @param pkt Packet received
 * @param val Decoded value pointer
 * @retval ERROR_OUT_OF_BOUNDS if packet is too short to decode value
 * @retval ERROR_BAD_RESULT if value reports error code
 * @retval OK
 ******************************************************************************/
WsCommon::RetRes decode_meta_bat_mv(WsCommon::Packet* pkt, uint16_t* val)
{
	const int PACKET_BATT_INDEX = 11;
	if(pkt->pl_len <= PACKET_BATT_INDEX) return WsCommon::RetRes::ERROR_OUT_OF_BOUNDS;
	*val = (pkt->pl[PACKET_BATT_INDEX] * 0.02) * 1000;
	return WsCommon::RetRes::OK;
}

/******************************************************************************
 * @brief Decode station battery level boolen True:Low, False:Good
 * @param pkt Packet received
 * @param val Decoded value pointer
 * @retval ERROR_OUT_OF_BOUNDS if packet is too short to decode value
 * @retval ERROR_BAD_RESULT if value reports error code
 * @retval OK
 ******************************************************************************/
WsCommon::RetRes decode_meta_bat_low(WsCommon::Packet* pkt, bool* val)
{
	const int LOW_BATT_THR = 2600;
	uint16_t mV;
	decode_meta_bat_mv(pkt, &mV);
	if(mV < LOW_BATT_THR)
	{
		*val = true;
	}
	else
	{
		*val = false;
	}	
	return WsCommon::RetRes::OK;
}

/******************************************************************************
 * @brief Decode packet RSSI. This is the RSSI that station listens the gateway
 * @param pkt Packet received
 * @param val Decoded value pointer
 * @retval ERROR_OUT_OF_BOUNDS if packet is too short to decode value
 * @retval ERROR_BAD_RESULT if value reports error code
 * @retval OK
 ******************************************************************************/
WsCommon::RetRes decode_meta_rssi(WsCommon::Packet* pkt, int* val)
{
	const int PACKET_RSSI_INDEX = 10;
	if(pkt->pl_len <= PACKET_RSSI_INDEX) return WsCommon::RetRes::ERROR_OUT_OF_BOUNDS;
	*val = pkt->pl[PACKET_RSSI_INDEX] - 137;
	return WsCommon::RetRes::OK;
}

/******************************************************************************
 * @brief Decode station firmware version
 * @param pkt Packet received
 * @param val Decoded value pointer
 * @retval ERROR_OUT_OF_BOUNDS if packet is too short to decode value
 * @retval ERROR_BAD_RESULT if value reports error code
 * @retval OK
 ******************************************************************************/
WsCommon::RetRes decode_meta_fw_ver(WsCommon::Packet* pkt, uint8_t* val)
{
	const int PACKET_FW_VERSION_INDEX = 1;
	if(pkt->pl_len <= PACKET_FW_VERSION_INDEX) return WsCommon::RetRes::ERROR_OUT_OF_BOUNDS;
	*val = pkt->pl[PACKET_FW_VERSION_INDEX];
	return WsCommon::RetRes::OK;
}

/******************************************************************************
 * @brief Decode station value depicting the station is in gps fixing state/busy
 * @param pkt Packet received
 * @param val Decoded value pointer
 * @retval ERROR_OUT_OF_BOUNDS if packet is too short to decode value
 * @retval ERROR_BAD_RESULT if value reports error code
 * @retval OK
 ******************************************************************************/
WsCommon::RetRes decode_meta_gps_fixing(WsCommon::Packet* pkt, bool* val)
{
	const int PACKET_GPS_FIXING_INDEX = 9;
	if(pkt->pl_len <= PACKET_GPS_FIXING_INDEX) return WsCommon::RetRes::ERROR_OUT_OF_BOUNDS;
	*val = (pkt->pl[PACKET_GPS_FIXING_INDEX] & 0x80) >> 7;
	return WsCommon::RetRes::OK;
}

/******************************************************************************
 * @brief Decode tx interval
 * @param pkt Packet received
 * @param val Decoded value pointer
 * @retval ERROR_OUT_OF_BOUNDS if packet is too short to decode value
 * @retval ERROR_BAD_RESULT if value reports error code
 * @retval OK
 ******************************************************************************/
WsCommon::RetRes decode_meta_tx_interval(WsCommon::Packet* pkt, uint16_t* val)
{
	const int PACKET_CUR_INTERVAL_INDEX = 13;
	if(pkt->pl_len <= PACKET_CUR_INTERVAL_INDEX) return WsCommon::RetRes::ERROR_OUT_OF_BOUNDS;
	*val = pkt->pl[PACKET_CUR_INTERVAL_INDEX] * 10;
	return WsCommon::RetRes::OK;
}

}