#ifndef WS_DATA_H
#define WS_DATA_H

#include <cmath>
#include "common.h"
#include "struct.h"
#include "ring_buff.h"
#include "ws/include/ws_base.h"

namespace WsData
{

const WsAdapter::AdapterValueName ADAPTER_TB_WEATHER_PRECIP_RATE= 
	{&WsFields::FIELD_INFO_WEATHER_PRECIP_RATE, TB_KEY_WS_PRECIP_RATE};
const WsAdapter::AdapterValueName ADAPTER_TB_WEATHER_PRESSURE = 
	{&WsFields::FIELD_INFO_WEATHER_PRESSURE, TB_KEY_WS_PRESSURE};

static const WsAdapter::AdapterValueName* ADAPTER_TB[] =
{
	&ADAPTER_TB_WEATHER_PRECIP_RATE,
	&ADAPTER_TB_WEATHER_PRESSURE,
};

static const uint8_t ADAPTER_TB_VALUES_COUNT = sizeof(ADAPTER_TB)/sizeof(WsAdapter::AdapterValueName*);

//
// Functions
//
WsCommon::RetRes init();
WsCommon::RetRes add(WsFields::FieldPack* field_pack);
int get_recv_count();
int get_aggregate_count();
WsCommon::RetRes pop_aggregate_item(WsFields::FieldPack* field_pack);
WsCommon::RetRes get_last_packet(WsFields::FieldPack* field_pack);

WsCommon::RetRes clear_recv();
WsCommon::RetRes aggregate_now();
WsCommon::RetRes clear_aggregates();

WsCommon::RetRes update_precip_rate_params(WsFields::FieldPack* field_pack);
float get_precip_rate();

float get_daily_precip();
void update_daily_precip(float cml_val);
void reset_daily_precip();

struct DailyExtremes {
	float temp_min = NAN;
	float temp_max = NAN;
	float pressure_min = NAN;
	float pressure_max = NAN;
	float hum_min = NAN;
	float hum_max = NAN;
	float wind_gust_max = NAN;
	float precip_rate_max = NAN;
	uint8_t uv_max = 0;
	float solar_rad_max = NAN;
	bool has_data = false;
};

void update_daily_extremes(const WsFields::FieldPack* field_pack);
DailyExtremes get_daily_extremes();
void reset_daily_extremes();

}

#endif
