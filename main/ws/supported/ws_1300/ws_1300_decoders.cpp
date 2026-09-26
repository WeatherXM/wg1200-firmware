#include "include/ws_1300_decoders.h"
#include "include/ws_1300_helpers.h"
#include "utils.h"
#include <cmath>

namespace Ws1300Decoders
{

/******************************************************************************
 * @brief Get sensors struct from packet
 * @param pkt Packet received
 * @retval NULL if length is wrong
 ******************************************************************************/
Ws1300::ByteBuffer::StaSensors* get_sensors_pkt(WsCommon::Packet* pkt)
{
	if(pkt->pl_len != sizeof(Ws1300::ByteBuffer::StaSensors))
	{
		return NULL;
	}
	return (Ws1300::ByteBuffer::StaSensors*) pkt->pl;
}

/******************************************************************************
 * @brief Get header struct from packet
 * @param pkt Packet received
 * @retval NULL if length is wrong
 ******************************************************************************/
Ws1300::ByteBuffer::StaHeader* get_header_pkt(WsCommon::Packet* pkt)
{
	if(pkt->pl_len < sizeof(Ws1300::ByteBuffer::StaHeader))
	{
		return NULL;
	}
	return (Ws1300::ByteBuffer::StaHeader*) pkt->pl;
}

/******************************************************************************
 * @brief Get gnss struct from packet
 * @param pkt Packet received
 * @retval NULL if length is wrong
 ******************************************************************************/
Ws1300::ByteBuffer::StaGnss* get_gnss_pkt(WsCommon::Packet* pkt)
{
	if(pkt->pl_len != sizeof(Ws1300::ByteBuffer::StaGnss))
	{
		return NULL;
	}
	return (Ws1300::ByteBuffer::StaGnss*) pkt->pl;
}

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
	Ws1300::ByteBuffer::StaSensors* sensors_pkt = get_sensors_pkt(pkt);
	if(sensors_pkt == NULL)
	{
		return WsCommon::RetRes::ERROR_OUT_OF_BOUNDS;
	}
	i16 temp = Ws1300::ByteBuffer::be_to_le(sensors_pkt->temp);
	if(temp == 0x7FFF)
	{
		return WsCommon::RetRes::ERROR_BAD_RESULT;
	}
	*val = temp / 10.0f;
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
	Ws1300::ByteBuffer::StaSensors* sensors_pkt = get_sensors_pkt(pkt);
	if(sensors_pkt == NULL)
	{
		return WsCommon::RetRes::ERROR_OUT_OF_BOUNDS;
	}
	if(sensors_pkt->hum == 0xFF)
	{
		return WsCommon::RetRes::ERROR_BAD_RESULT;
	}
	*val = (float) sensors_pkt->hum;
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
	Ws1300::ByteBuffer::StaSensors* sensors_pkt = get_sensors_pkt(pkt);
	if(sensors_pkt == NULL)
	{
		return WsCommon::RetRes::ERROR_OUT_OF_BOUNDS;
	}
	u16 wind_dir = Ws1300::ByteBuffer::be_to_le(sensors_pkt->wind_dir);
	if(wind_dir == 0xFFFF)
	{
		return WsCommon::RetRes::ERROR_BAD_RESULT;
	}
	*val = wind_dir;
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
	Ws1300::ByteBuffer::StaSensors* sensors_pkt = get_sensors_pkt(pkt);
	if(sensors_pkt == NULL)
	{
		return WsCommon::RetRes::ERROR_OUT_OF_BOUNDS;
	}
	u16 wind_speed = Ws1300::ByteBuffer::be_to_le(sensors_pkt->wind_speed);
	if(wind_speed == 0xFFFF)
	{
		return WsCommon::RetRes::ERROR_BAD_RESULT;
	}
	*val = wind_speed / 10.0f;
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
	Ws1300::ByteBuffer::StaSensors* sensors_pkt = get_sensors_pkt(pkt);
	if(sensors_pkt == NULL)
	{
		return WsCommon::RetRes::ERROR_OUT_OF_BOUNDS;
	}
	u16 wind_gust = Ws1300::ByteBuffer::be_to_le(sensors_pkt->wind_gust);
	if(wind_gust == 0xFFFF)
	{
		return WsCommon::RetRes::ERROR_BAD_RESULT;
	}
	*val = wind_gust / 10.0f;
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
	Ws1300::ByteBuffer::StaSensors* sensors_pkt = get_sensors_pkt(pkt);
	if(sensors_pkt == NULL)
	{
		return WsCommon::RetRes::ERROR_OUT_OF_BOUNDS;
	}
	u24 illum = Ws1300::ByteBuffer::be_to_le(sensors_pkt->illum);
	u24 illum_error_val = {0xff, 0xff, 0xff};
	if(memcmp(illum.data, illum_error_val.data, sizeof(u24)) == 0)
	{
		return WsCommon::RetRes::ERROR_BAD_RESULT;
	}
	memset(val, 0x00, sizeof(uint32_t));
	memcpy(val, illum.data, sizeof(illum.data));
	return WsCommon::RetRes::OK;
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
	Ws1300::ByteBuffer::StaSensors* sensors_pkt = get_sensors_pkt(pkt);
	if(sensors_pkt == NULL)
	{
		return WsCommon::RetRes::ERROR_OUT_OF_BOUNDS;
	}
	if(sensors_pkt->uv_index == 0xFF)
	{
		return WsCommon::RetRes::ERROR_BAD_RESULT;
	}
	*val = sensors_pkt->uv_index;
	return WsCommon::RetRes::OK;
}

/******************************************************************************
 * @brief Decode pressure
 * @param pkt Packet received
 * @param val Decoded value pointer
 * @retval ERROR_OUT_OF_BOUNDS if packet is too short to decode value
 * @retval ERROR_BAD_RESULT if value reports error code
 * @retval OK
 ******************************************************************************/
WsCommon::RetRes decode_weather_pressure(WsCommon::Packet* pkt, float* val)
{
	Ws1300::ByteBuffer::StaSensors* sensors_pkt = get_sensors_pkt(pkt);
	if(sensors_pkt == NULL)
	{
		return WsCommon::RetRes::ERROR_OUT_OF_BOUNDS;
	}
	u16 press = Ws1300::ByteBuffer::be_to_le(sensors_pkt->press);
	if(press == 0xFFFF)
	{
		return WsCommon::RetRes::ERROR_BAD_RESULT;
	}
	*val = press / 10.0f;
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
WsCommon::RetRes decode_weather_precip_cml(WsCommon::Packet* pkt, float* val)
{
	Ws1300::ByteBuffer::StaSensors* sensors_pkt = get_sensors_pkt(pkt);
	if(sensors_pkt == NULL)
	{
		return WsCommon::RetRes::ERROR_OUT_OF_BOUNDS;
	}
	float precip_cml = Ws1300::ByteBuffer::be_to_le(sensors_pkt->precip_cml);
	// The protocol's all-0xFF sentinel decodes to NaN. Comparing it with
	// another NaN never succeeds, so validate the decoded value itself.
	if(!std::isfinite(precip_cml))
	{
		return WsCommon::RetRes::ERROR_BAD_RESULT;
	}
	*val = precip_cml;
	return WsCommon::RetRes::OK;
}

/******************************************************************************
 * @brief Decode precipitation rate
 * @param pkt Packet received
 * @param val Decoded value pointer
 * @retval ERROR_OUT_OF_BOUNDS if packet is too short to decode value
 * @retval ERROR_BAD_RESULT if value reports error code
 * @retval OK
 ******************************************************************************/
WsCommon::RetRes decode_weather_precip_rate(WsCommon::Packet* pkt, float* val)
{
	Ws1300::ByteBuffer::StaSensors* sensors_pkt = get_sensors_pkt(pkt);
	if(sensors_pkt == NULL)
	{
		return WsCommon::RetRes::ERROR_OUT_OF_BOUNDS;
	}
	float precip_rate = Ws1300::ByteBuffer::be_to_le(sensors_pkt->precip_rate);
	if(precip_rate == 0xFFFF)
	{
		return WsCommon::RetRes::ERROR_BAD_RESULT;
	}
	*val = precip_rate / 10.0f;
	return WsCommon::RetRes::OK;
}

/******************************************************************************
 * @brief Decode weather packet timestamp
 * @param pkt Packet received
 * @param val Decoded value pointer
 * @retval ERROR_OUT_OF_BOUNDS if packet is too short to decode value
 * @retval ERROR_BAD_RESULT if value reports error code
 * @retval OK
 ******************************************************************************/
WsCommon::RetRes decode_weather_ts(WsCommon::Packet* pkt, uint32_t* val)
{
	Ws1300::ByteBuffer::StaSensors* sensors_pkt = get_sensors_pkt(pkt);
	if(sensors_pkt == NULL)
	{
		return WsCommon::RetRes::ERROR_OUT_OF_BOUNDS;
	}
	u32 ts = Ws1300::ByteBuffer::be_to_le(sensors_pkt->ts);
	// This one doesn't count as bad result so just set it to NONE
	if(ts == 0xFFFFFFFF)
	{
		return WsCommon::RetRes::NONE;
	}
	*val = ts;
	return WsCommon::RetRes::OK;
}

/******************************************************************************
 * @brief Compute solar radiation
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
	if(ret != WsCommon::RetRes::OK)
	{
		return ret;
	}

	const float LUX_TO_SOLAR_RADIATION_COEFF = 0.007892;

	*val = ((float) illum) * LUX_TO_SOLAR_RADIATION_COEFF;
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
	Ws1300::ByteBuffer::StaHeader* header_pkt = get_header_pkt(pkt);
	if(header_pkt == NULL)
	{
		return WsCommon::RetRes::ERROR_OUT_OF_BOUNDS;
	}
	if(header_pkt->tx_int == 0)
	{
		return WsCommon::RetRes::NONE;
	}
	*val = header_pkt->tx_int;
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
	Ws1300::ByteBuffer::StaHeader* header_pkt = get_header_pkt(pkt);
	if(header_pkt == NULL)
	{
		return WsCommon::RetRes::ERROR_OUT_OF_BOUNDS;
	}
	u32 station_id = Ws1300::ByteBuffer::be_to_le(header_pkt->station_id);
	*val = station_id;
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
	Ws1300::ByteBuffer::StaHeader* header_pkt = get_header_pkt(pkt);
	if(header_pkt == NULL)
	{
		return WsCommon::RetRes::ERROR_OUT_OF_BOUNDS;
	}
	u16 packet_id = Ws1300::ByteBuffer::be_to_le(header_pkt->packet_id);
	*val = packet_id;
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
	Ws1300::ByteBuffer::StaHeader* header_pkt = get_header_pkt(pkt);
	if(header_pkt == NULL)
	{
		return WsCommon::RetRes::ERROR_OUT_OF_BOUNDS;
	}
	*val = header_pkt->aux_byte_1.tx_power;
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
	Ws1300::ByteBuffer::StaHeader* header_pkt = get_header_pkt(pkt);
	if(header_pkt == NULL)
	{
		return WsCommon::RetRes::ERROR_OUT_OF_BOUNDS;
	}
	if(header_pkt->batt_mv == 0xFF) return WsCommon::RetRes::ERROR_NOT_SUPPORTED;
	*val = header_pkt->batt_mv * 20;
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
	Ws1300::ByteBuffer::StaHeader* header_pkt = get_header_pkt(pkt);
	if(header_pkt == NULL)
	{
		return WsCommon::RetRes::ERROR_OUT_OF_BOUNDS;
	}
	if(header_pkt->batt_mv == 0xFF || (!header_pkt->aux_byte_2.int_batt && !header_pkt->aux_byte_2.ext_batt))
	{
		*val = false;
		return WsCommon::RetRes::OK;
	}
	*val = header_pkt->aux_byte_2.low_batt;
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
	Ws1300::ByteBuffer::StaHeader* header_pkt = get_header_pkt(pkt);
	if(header_pkt == NULL)
	{
		return WsCommon::RetRes::ERROR_OUT_OF_BOUNDS;
	}
	*val = -header_pkt->last_resp_rssi; // TODO: Check sin
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
	Ws1300::ByteBuffer::StaHeader* header_pkt = get_header_pkt(pkt);
	if(header_pkt == NULL)
	{
		return WsCommon::RetRes::ERROR_OUT_OF_BOUNDS;
	}
	*val = header_pkt->fw_ver;
	return WsCommon::RetRes::OK;
}

/******************************************************************************
 * @brief Decode gnss validity
 * @param pkt Packet received
 * @param val Decoded value pointer
 * @retval ERROR_OUT_OF_BOUNDS if packet is too short to decode value
 * @retval ERROR_BAD_RESULT if value reports error code
 * @retval OK
 ******************************************************************************/
WsCommon::RetRes decode_gnss_fix_ok(WsCommon::Packet* pkt, bool* val)
{
	Ws1300::ByteBuffer::StaGnss* gnss_pkt = get_gnss_pkt(pkt);
	if(gnss_pkt == NULL)
	{
		return WsCommon::RetRes::ERROR_OUT_OF_BOUNDS;
	}
	*val = gnss_pkt->aux_byte.fix_ok;
	return WsCommon::RetRes::OK;
}

/******************************************************************************
 * @brief Decode gnss lat
 * @param pkt Packet received
 * @param val Decoded value pointer
 * @retval ERROR_OUT_OF_BOUNDS if packet is too short to decode value
 * @retval ERROR_BAD_RESULT if value reports error code
 * @retval OK
 ******************************************************************************/
WsCommon::RetRes decode_gnss_lat(WsCommon::Packet* pkt, float* val)
{
	Ws1300::ByteBuffer::StaGnss* gnss_pkt = get_gnss_pkt(pkt);
	if(gnss_pkt == NULL)
	{
		return WsCommon::RetRes::ERROR_OUT_OF_BOUNDS;
	}
	*val = Ws1300::ByteBuffer::be_to_le(gnss_pkt->lat) * 1e-7;
	return WsCommon::RetRes::OK;
}

/******************************************************************************
 * @brief Decode gnss lon
 * @param pkt Packet received
 * @param val Decoded value pointer
 * @retval ERROR_OUT_OF_BOUNDS if packet is too short to decode value
 * @retval ERROR_BAD_RESULT if value reports error code
 * @retval OK
 ******************************************************************************/
WsCommon::RetRes decode_gnss_lon(WsCommon::Packet* pkt, float* val)
{
	Ws1300::ByteBuffer::StaGnss* gnss_pkt = get_gnss_pkt(pkt);
	if(gnss_pkt == NULL)
	{
		return WsCommon::RetRes::ERROR_OUT_OF_BOUNDS;
	}
	*val = Ws1300::ByteBuffer::be_to_le(gnss_pkt->lon) * 1e-7;
	return WsCommon::RetRes::OK;
}

/******************************************************************************
 * @brief Decode gnss sats
 * @param pkt Packet received
 * @param val Decoded value pointer
 * @retval ERROR_OUT_OF_BOUNDS if packet is too short to decode value
 * @retval ERROR_BAD_RESULT if value reports error code
 * @retval OK
 ******************************************************************************/
WsCommon::RetRes decode_gnss_sats(WsCommon::Packet* pkt, uint8_t* val)
{
	Ws1300::ByteBuffer::StaGnss* gnss_pkt = get_gnss_pkt(pkt);
	if(gnss_pkt == NULL)
	{
		return WsCommon::RetRes::ERROR_OUT_OF_BOUNDS;
	}
	*val = gnss_pkt->sats;
	return WsCommon::RetRes::OK;
}

/******************************************************************************
 * @brief Decode gnss ts
 * @param pkt Packet received
 * @param val Decoded value pointer
 * @retval ERROR_OUT_OF_BOUNDS if packet is too short to decode value
 * @retval ERROR_BAD_RESULT if value reports error code
 * @retval OK
 ******************************************************************************/
WsCommon::RetRes decode_gnss_ts(WsCommon::Packet* pkt, uint32_t* val)
{
	Ws1300::ByteBuffer::StaGnss* gnss_pkt = get_gnss_pkt(pkt);
	if(gnss_pkt == NULL)
	{
		return WsCommon::RetRes::ERROR_OUT_OF_BOUNDS;
	}
	*val = Ws1300::ByteBuffer::be_to_le(gnss_pkt->ts);
	return WsCommon::RetRes::OK;
}

}
