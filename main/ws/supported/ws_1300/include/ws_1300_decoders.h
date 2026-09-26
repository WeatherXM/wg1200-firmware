#ifndef WS_1300_DECODERS_H
#define WS_1300_DECODERS_H

#include "ws/include/ws_common.h"

namespace Ws1300Decoders
{

WsCommon::RetRes decode_weather_temp(WsCommon::Packet* pkt, float* val);
WsCommon::RetRes decode_weather_hum(WsCommon::Packet* pkt, float* val);
WsCommon::RetRes decode_weather_wind_dir(WsCommon::Packet* pkt, uint16_t* val);
WsCommon::RetRes decode_weather_wind_speed(WsCommon::Packet* pkt, float* val);
WsCommon::RetRes decode_weather_wind_gust(WsCommon::Packet* pkt, float* val);
WsCommon::RetRes decode_weather_illuminance(WsCommon::Packet* pkt, uint32_t* val);
WsCommon::RetRes decode_weather_uv_index(WsCommon::Packet* pkt, uint8_t* val);
WsCommon::RetRes decode_weather_precip_cml(WsCommon::Packet* pkt, float* val);
WsCommon::RetRes decode_weather_precip_rate(WsCommon::Packet* pkt, float* val);
WsCommon::RetRes decode_weather_pressure(WsCommon::Packet* pkt, float* val);
WsCommon::RetRes decode_weather_ts(WsCommon::Packet* pkt, uint32_t* val);
WsCommon::RetRes decode_weather_solar_rad(WsCommon::Packet* pkt, float* val);

WsCommon::RetRes decode_meta_id(WsCommon::Packet* pkt, uint32_t* val);
WsCommon::RetRes decode_meta_pkt_count(WsCommon::Packet* pkt, uint32_t* val);
WsCommon::RetRes decode_meta_tx_power(WsCommon::Packet* pkt, uint16_t* val);
WsCommon::RetRes decode_meta_bat_mv(WsCommon::Packet* pkt, uint16_t* val);
WsCommon::RetRes decode_meta_bat_low(WsCommon::Packet* pkt, bool* val);
WsCommon::RetRes decode_meta_rssi(WsCommon::Packet* pkt, int* val);
WsCommon::RetRes decode_meta_fw_ver(WsCommon::Packet* pkt, uint8_t* val);
WsCommon::RetRes decode_meta_gps_fixing(WsCommon::Packet* pkt, bool* val);
WsCommon::RetRes decode_meta_tx_interval(WsCommon::Packet* pkt, uint16_t* val);

WsCommon::RetRes decode_gnss_fix_ok(WsCommon::Packet* pkt, bool* val);
WsCommon::RetRes decode_gnss_lat(WsCommon::Packet* pkt, float* val);
WsCommon::RetRes decode_gnss_lon(WsCommon::Packet* pkt, float* val);
WsCommon::RetRes decode_gnss_sats(WsCommon::Packet* pkt, uint8_t* val);
WsCommon::RetRes decode_gnss_ts(WsCommon::Packet* pkt, uint32_t* val);

}

#endif