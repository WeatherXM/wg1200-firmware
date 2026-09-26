#ifndef WS_GPS_H
#define WS_GPS_H

#include "ws_fields.h"
#include "h3api.h"
#include "ws_adapter.h"
#include "const.h"

namespace WsGps
{

const uint16_t MAX_GPS_RAW_BYTES_LEN = 100;
const uint16_t MAX_FIELDS_IN_GPS_PACK = 8;

/******************************************************************************
 * @brief Field decoding status. AWAITING status means packets need to be decoded externally
 ******************************************************************************/
enum class DecodingStatus
{
	NONE = 0,
	DECODED,
	AWAITING,
	FAILED
};

/******************************************************************************
 * @brief Field decoding type. INDIRECT type is decoded externally
 ******************************************************************************/
enum class DecodingType
{
	NONE = 0,
	DIRECT,
	INDIRECT
};

/******************************************************************************
 * @brief Struct holding raw data to be decoded
 ******************************************************************************/
struct RawData
{
	uint8_t buff[MAX_GPS_RAW_BYTES_LEN];
	uint16_t buff_len;
};

/******************************************************************************
 * @brief Struct holding gps data fields, raw data and metadata about them
 ******************************************************************************/
struct GpsData
{
	DecodingStatus decoding_status = DecodingStatus::NONE;
	DecodingType decoding_type = DecodingType::NONE;
	bool valid = false;
	RawData raw_data;
	WsFields::FieldPack field_pack = {};
};

const WsFields::FieldDesc FIELD_INFO_GPS_LAT = {.name = "GPS Latitude", .data_type = WsFields::ValueType::FLOAT, 
	.type = WsFields::FieldType::GPS_LAT, .cat = WsFields::FieldCatType::GPS, .agg_type = WsFields::FieldAggType::NONE, .decimals = GPS_DEG_DEC};
const WsFields::FieldDesc FIELD_INFO_GPS_LON = {.name = "GPS Longitude", .data_type = WsFields::ValueType::FLOAT, 
	.type = WsFields::FieldType::GPS_LON, .cat = WsFields::FieldCatType::GPS, .agg_type = WsFields::FieldAggType::NONE, .decimals = GPS_DEG_DEC};
const WsFields::FieldDesc FIELD_INFO_GPS_TS = {.name = "GPS Timestamp", .data_type = WsFields::ValueType::U32, 
	.type = WsFields::FieldType::GPS_TS, .cat = WsFields::FieldCatType::GPS, .agg_type = WsFields::FieldAggType::NONE};
const WsFields::FieldDesc FIELD_INFO_GPS_SPEED = {.name = "GPS Speed", .data_type = WsFields::ValueType::FLOAT, 
	.type = WsFields::FieldType::GPS_SPEED, .cat = WsFields::FieldCatType::GPS, .agg_type = WsFields::FieldAggType::NONE, .decimals = 1};
const WsFields::FieldDesc FIELD_INFO_GPS_DIR = {.name = "GPS Direction", .data_type = WsFields::ValueType::FLOAT, 
	.type = WsFields::FieldType::GPS_DIR, .cat = WsFields::FieldCatType::GPS, .agg_type = WsFields::FieldAggType::NONE, .decimals = 1};
const WsFields::FieldDesc FIELD_INFO_GPS_SATS = {.name = "GPS Satellites", .data_type = WsFields::ValueType::U8, 
	.type = WsFields::FieldType::GPS_SATS, .cat = WsFields::FieldCatType::GPS, .agg_type = WsFields::FieldAggType::NONE};
const WsFields::FieldDesc FIELD_INFO_GPS_CELL_ID_PT1 = {.name = "GPS Cell ID Pt1", .data_type = WsFields::ValueType::U32, 
	.type = WsFields::FieldType::NONE, .cat = WsFields::FieldCatType::GPS, .agg_type = WsFields::FieldAggType::NONE};
const WsFields::FieldDesc FIELD_INFO_GPS_CELL_ID_PT2 = {.name = "GPS Cell ID Pt2", .data_type = WsFields::ValueType::U32, 
	.type = WsFields::FieldType::NONE, .cat = WsFields::FieldCatType::GPS, .agg_type = WsFields::FieldAggType::NONE};


const WsAdapter::AdapterValueName ADAPTER_TB_GPS_LAT = 
	{&FIELD_INFO_GPS_LAT, TB_KEY_GPS_LAT};
const WsAdapter::AdapterValueName ADAPTER_TB_GPS_LON = 
	{&FIELD_INFO_GPS_LON, TB_KEY_GPS_LON};
const WsAdapter::AdapterValueName ADAPTER_TB_GPS_TS = 
	{&FIELD_INFO_GPS_TS, TB_KEY_GPS_TS};
const WsAdapter::AdapterValueName ADAPTER_TB_GPS_SPEED = 
	{&FIELD_INFO_GPS_SPEED, TB_KEY_GPS_SPEED};
const WsAdapter::AdapterValueName ADAPTER_TB_GPS_DIR = 
	{&FIELD_INFO_GPS_DIR, TB_KEY_GPS_DIR};
const WsAdapter::AdapterValueName ADAPTER_TB_GPS_SATS = 
	{&FIELD_INFO_GPS_SATS, TB_KEY_GPS_SATS};

static const WsAdapter::AdapterValueName* ADAPTER_TB[] =
{
	&ADAPTER_TB_GPS_LAT,
	&ADAPTER_TB_GPS_LON,
	&ADAPTER_TB_GPS_TS,
	&ADAPTER_TB_GPS_SPEED,
	&ADAPTER_TB_GPS_DIR,
	&ADAPTER_TB_GPS_SATS,
};

static const uint8_t ADAPTER_TB_VALUES_COUNT = sizeof(ADAPTER_TB)/sizeof(WsAdapter::AdapterValueName*);

void init();
void print_gps_data(GpsData* field_pack);
void push_gps_data(GpsData *data);
bool pop_gps_data(GpsData *data);
void get_last_gps_data(GpsData *data);
void set_last_gps_data(GpsData *data);
bool gps_data_awaiting_decode();
bool gps_data_has_required_fields(GpsData *gps_data);
bool gps_data_is_usable(GpsData *gps_data);
uint64_t calc_h3_index();
void get_cell_id_str(uint64_t cell_id, char* cell_id_str_buff, uint16_t cell_id_str_buff_len);
bool gps_data_expired(WsGps::GpsData *gps_data);

}

#endif
