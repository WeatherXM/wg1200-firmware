#ifndef WS_1001_ADAPTER_TB_H
#define WS_1001_ADAPTER_TB_H

#include "ws/include/ws_adapter.h"
#include "const.h"

namespace Ws1001AdapterTb
{

const WsAdapter::AdapterValueName ADAPTER_TB_WEATHER_TEMP = 
	{&WsFields::FIELD_INFO_WEATHER_TEMP, TB_KEY_WS_TEMP};
const WsAdapter::AdapterValueName ADAPTER_TBWEATHER_HUM = 
	{&WsFields::FIELD_INFO_WEATHER_HUM, TB_KEY_WS_HUM};
const WsAdapter::AdapterValueName ADAPTER_TB_WEATHER_WIND_DIR = 
	{&WsFields::FIELD_INFO_WEATHER_WIND_DIR, TB_KEY_WS_W_DIR};
const WsAdapter::AdapterValueName ADAPTER_TB_WEATHER_WIND_SPEED = 
	{&WsFields::FIELD_INFO_WEATHER_WIND_SPEED, TB_KEY_WS_W_SPEED};
const WsAdapter::AdapterValueName ADAPTER_TB_WEATHER_WIND_GUST = 
	{&WsFields::FIELD_INFO_WEATHER_WIND_GUST, TB_KEY_WS_W_GUST};
const WsAdapter::AdapterValueName ADAPTER_TB_WEATHER_ILLUMINANCE = 
	{&WsFields::FIELD_INFO_WEATHER_ILLUMINANCE, TB_KEY_WS_LIGHT};
const WsAdapter::AdapterValueName ADAPTER_TB_WEATHER_SOLAR_RAD = 
	{&WsFields::FIELD_INFO_WEATHER_SOLAR_RAD, TB_KEY_WS_SOL_RAD};
const WsAdapter::AdapterValueName ADAPTER_TB_WEATHER_UV_INDEX = 
	{&WsFields::FIELD_INFO_WEATHER_UV_INDEX, TB_KEY_WS_UV_INDEX};
const WsAdapter::AdapterValueName ADAPTER_TB_WEATHER_PRECIP = 
	{&WsFields::FIELD_INFO_WEATHER_PRECIP_CML, TB_KEY_WS_PRECIP_CML};

const WsAdapter::AdapterValueName ADAPTER_TB_META_ID = 
	{&WsFields::FIELD_INFO_META_ID, TB_ATTR_WS_ID};
const WsAdapter::AdapterValueName ADAPTER_TB_META_PCK_COUNT = 
	{&WsFields::FIELD_INFO_META_PCK_COUNT, TB_KEY_WS_PKT_COUNT};
const WsAdapter::AdapterValueName ADAPTER_TB_META_TX_POWER = 
	{&WsFields::FIELD_INFO_META_TX_POWER, TB_KEY_WS_TX_POWER};
const WsAdapter::AdapterValueName ADAPTER_TB_META_BAT_MV = 
	{&WsFields::FIELD_INFO_META_BAT_MV, TB_KEY_WS_BAT_MV};
const WsAdapter::AdapterValueName ADAPTER_TB_META_BAT_LOW = 
	{&WsFields::FIELD_INFO_META_BAT_LOW, TB_KEY_WS_LOW_BAT};
const WsAdapter::AdapterValueName ADAPTER_TB_META_RSSI = 
	{&WsFields::FIELD_INFO_META_RSSI, TB_KEY_WS_RSSI};
const WsAdapter::AdapterValueName ADAPTER_TB_META_FW_VER = 
	{&WsFields::FIELD_INFO_META_FW_VER, TB_KEY_WS_FW_V};
const WsAdapter::AdapterValueName ADAPTER_TB_META_TX_INTERVAL = 
	{&WsFields::FIELD_INFO_META_TX_INTERVAL, TB_KEY_WS_CUR_INTERVAL};

}

#endif