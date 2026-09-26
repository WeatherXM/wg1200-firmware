#ifndef WS_FIELDS_H
#define WS_FIELDS_H

#include "ws_common.h"
#include "string.h"

namespace WsFields
{

const uint16_t FIELD_CAT_TYPE_WEATHER_CODE = 0x1000;
const uint16_t FIELD_CAT_TYPE_META_CODE = 0x2000;
const uint16_t FIELD_CAT_TYPE_GPS_CODE = 0x3000;
const uint8_t MAX_FIELDS_IN_PACK = 22;

/******************************************************************************
 * @brief Field category type
 ******************************************************************************/
enum class FieldCatType 
{
	NONE = 0,
	WEATHER = FIELD_CAT_TYPE_WEATHER_CODE,
	META = FIELD_CAT_TYPE_META_CODE,
	GPS = FIELD_CAT_TYPE_GPS_CODE
};

/******************************************************************************
 * @brief Most common fields.
 * Any extra field, either from weather stations
 * or device sensor (GPS, Pressure, Precip Rate),
 * will be defined only by unique name, and FieldType::NONE.
 ******************************************************************************/
enum class FieldType
{
	NONE = 0,
	WEATHER_TEMP = FIELD_CAT_TYPE_WEATHER_CODE,
	WEATHER_HUM,
	WEATHER_WIND_DIR,
	WEATHER_WIND_SPEED,
	WEATHER_WIND_GUST,
	WEATHER_ILLUMINANCE,
	WEATHER_SOLAR_RAD,
	WEATHER_UV_INDEX,
	WEATHER_PRECIP_CML,
	WEATHER_PRECIP_RATE,
	WEATHER_PRESSURE,
	WEATHER_TS,

	META_ID = FIELD_CAT_TYPE_META_CODE,
	META_PCK_COUNT,
	META_TX_POWER,
	META_BAT_MV,
	META_BAT_LOW,
	META_RSSI,
	META_FW_VER,
	META_TX_INTERVAL,	

	GPS_LAT = FIELD_CAT_TYPE_GPS_CODE,
	GPS_LON,
	GPS_VALID,
	GPS_TS,
	GPS_SPEED,
	GPS_DIR,
	GPS_SATS
};

/******************************************************************************
 * @brief Field value type
 ******************************************************************************/
enum class ValueType
{
	NONE = 0,
	FLOAT,
	U8,
	U16,
	U32,
	INT,
	BOOL
};

/******************************************************************************
 * @brief Field value union
 ******************************************************************************/
union FieldValue
{
	float val_float;
	int val_int;
	uint8_t val_u8;
	uint16_t val_u16;
	uint32_t val_u32 = 0U;
	bool val_bool;
};

/******************************************************************************
 * @brief Field aggregation type used when field is passed to field_packs_aggregation
 ******************************************************************************/
enum class FieldAggType
{
	NONE = 0,
	MAX,
	MIN,
	AVG,
	CUSTOM
};

/******************************************************************************
 * @brief Field descriptor
 ******************************************************************************/
struct FieldDesc
{
	const char* name = "";
	const ValueType data_type = ValueType::NONE;
	const FieldType type = FieldType::NONE;
	const FieldCatType cat = FieldCatType::NONE;
	FieldAggType agg_type = FieldAggType::NONE;
	const uint8_t decimals = 0;
};

/******************************************************************************
 * @brief Field struct used by station and GPS to pass values dynamically
 ******************************************************************************/
struct Field
{
	const FieldDesc* info;
	FieldValue value;
	WsCommon::RetRes decode_status = WsCommon::RetRes::NONE;
};

/******************************************************************************
 * @brief Pack containing fields and metadata about them
 ******************************************************************************/
struct FieldPack
{
	uint32_t ts = 0;
	uint16_t timespan = 0;
	uint16_t field_count = 0;
	uint8_t agg_count = 0;
	bool bad_values_detected = false;
	char packer_name[7];
	Field fields[MAX_FIELDS_IN_PACK];
};

/** Rounding if aggregation timestamp */
enum class WsDataAggrTsRound
{
	NONE = 0,
	CEIL,
	FLOOR
};

const WsDataAggrTsRound WS_DATA_AGGREGATE_TS_ROUND = WsDataAggrTsRound::CEIL;

const FieldDesc FIELD_INFO_WEATHER_TEMP = {.name = "Temperature", .data_type = ValueType::FLOAT, 
.type = FieldType::WEATHER_TEMP, .cat = FieldCatType::WEATHER, .agg_type = FieldAggType::AVG, .decimals = 2};
const FieldDesc FIELD_INFO_WEATHER_HUM = {.name = "Humidity", .data_type = ValueType::FLOAT, 
.type = FieldType::WEATHER_HUM, .cat = FieldCatType::WEATHER, .agg_type = FieldAggType::AVG};
const FieldDesc FIELD_INFO_WEATHER_WIND_DIR = {.name = "Wind Direction", .data_type = ValueType::U16, 
.type = FieldType::WEATHER_WIND_DIR, .cat = FieldCatType::WEATHER, .agg_type = FieldAggType::CUSTOM};
const FieldDesc FIELD_INFO_WEATHER_WIND_SPEED = {.name = "Wind Speed", .data_type = ValueType::FLOAT, 
.type = FieldType::WEATHER_WIND_SPEED, .cat = FieldCatType::WEATHER, .agg_type = FieldAggType::AVG, .decimals = 2};
const FieldDesc FIELD_INFO_WEATHER_WIND_GUST = {.name = "Wind Gust", .data_type = ValueType::FLOAT, 
.type = FieldType::WEATHER_WIND_GUST, .cat = FieldCatType::WEATHER, .agg_type = FieldAggType::MAX, .decimals = 2};
const FieldDesc FIELD_INFO_WEATHER_ILLUMINANCE = {.name = "Illuminance", .data_type = ValueType::U32, 
.type = FieldType::WEATHER_ILLUMINANCE, .cat = FieldCatType::WEATHER, .agg_type = FieldAggType::AVG};
const FieldDesc FIELD_INFO_WEATHER_SOLAR_RAD = {.name = "Solar Radiation", .data_type = ValueType::FLOAT, 
.type = FieldType::WEATHER_SOLAR_RAD, .cat = FieldCatType::WEATHER, .agg_type = FieldAggType::AVG, .decimals = 1};
const FieldDesc FIELD_INFO_WEATHER_UV_INDEX = {.name = "UV Index", .data_type = ValueType::U8, 
.type = FieldType::WEATHER_UV_INDEX, .cat = FieldCatType::WEATHER, .agg_type = FieldAggType::AVG};
const FieldDesc FIELD_INFO_WEATHER_PRECIP_CML = {.name = "Precipitation", .data_type = ValueType::FLOAT, 
.type = FieldType::WEATHER_PRECIP_CML, .cat = FieldCatType::WEATHER, .agg_type = FieldAggType::MAX, .decimals = 2};
const FieldDesc FIELD_INFO_WEATHER_PRECIP_RATE = {.name = "Precipitation Rate", .data_type = ValueType::FLOAT, 
.type = FieldType::WEATHER_PRECIP_RATE, .cat = FieldCatType::WEATHER, .agg_type = FieldAggType::MAX, .decimals = 2};
const FieldDesc FIELD_INFO_WEATHER_PRESSURE = {.name = "Pressure", .data_type = ValueType::FLOAT, 
.type = FieldType::WEATHER_PRESSURE, .cat = FieldCatType::WEATHER, .agg_type = FieldAggType::AVG, .decimals = 2};
const FieldDesc FIELD_INFO_WEATHER_TS = {.name = "Weather Timestamp", .data_type = ValueType::U32, 
.type = FieldType::WEATHER_TS, .cat = FieldCatType::WEATHER, .agg_type = FieldAggType::MAX};

const FieldDesc FIELD_INFO_META_ID = {.name = "Station ID", .data_type = ValueType::U32, 
.type = FieldType::META_ID, .cat = FieldCatType::META, .agg_type = FieldAggType::NONE};
const FieldDesc FIELD_INFO_META_PCK_COUNT = {.name = "Packet Count", .data_type = ValueType::U32, 
.type = FieldType::META_PCK_COUNT, .cat = FieldCatType::META, .agg_type = FieldAggType::MAX};
const FieldDesc FIELD_INFO_META_TX_POWER = {.name = "TX Power", .data_type = ValueType::U16, 
.type = FieldType::META_TX_POWER, .cat = FieldCatType::META, .agg_type = FieldAggType::MAX};
const FieldDesc FIELD_INFO_META_BAT_MV = {.name = "Battery mV", .data_type = ValueType::U16, 
.type = FieldType::META_BAT_MV, .cat = FieldCatType::META, .agg_type = FieldAggType::MIN};
const FieldDesc FIELD_INFO_META_BAT_LOW = {.name = "Battery Low", .data_type = ValueType::BOOL, 
.type = FieldType::META_BAT_LOW, .cat = FieldCatType::META, .agg_type = FieldAggType::MAX};
const FieldDesc FIELD_INFO_META_RSSI = {.name = "RSSI", .data_type = ValueType::INT, 
.type = FieldType::META_RSSI, .cat = FieldCatType::META, .agg_type = FieldAggType::MIN};
const FieldDesc FIELD_INFO_META_FW_VER = {.name = "Firmware Version", .data_type = ValueType::U8, 
.type = FieldType::META_FW_VER, .cat = FieldCatType::META, .agg_type = FieldAggType::NONE};
const FieldDesc FIELD_INFO_META_TX_INTERVAL = {.name = "TX Interval", .data_type = ValueType::U16, 
.type = FieldType::META_TX_INTERVAL, .cat = FieldCatType::META, .agg_type = FieldAggType::MAX};


/******************************************************************************
 * @brief Take a reference to a field value
 * @param field Field to reference its value
 * @param val Type pointer to field value
 * @retval ERROR_NOT_SUPPORTED if type is not implemented by field
 * @retval ERROR_BAD_PARAMS if parameteres are wrong
 * @retval OK
 ******************************************************************************/
template <typename T>
WsCommon::RetRes ref_field_val(WsFields::Field* field, T** val)
{
	if(field == NULL || val == NULL) return WsCommon::RetRes::ERROR_BAD_PARAMS;
	if(field->info == NULL) return WsCommon::RetRes::ERROR_BAD_PARAMS;

	switch (field->info->data_type)
	{
		case WsFields::ValueType::FLOAT:
			*val = reinterpret_cast<T*> (&field->value.val_float);
			break;
		case WsFields::ValueType::U8:
			*val = reinterpret_cast<T*> (&field->value.val_u8);
			break;
		case WsFields::ValueType::U16:
			*val = reinterpret_cast<T*> (&field->value.val_u16);
			break;
		case WsFields::ValueType::U32:
			*val = reinterpret_cast<T*> (&field->value.val_u32);
			break;
		case WsFields::ValueType::INT:
			*val = reinterpret_cast<T*> (&field->value.val_int);
			break;
		case WsFields::ValueType::BOOL:
			*val = reinterpret_cast<T*> (&field->value.val_bool);
			break;
		default:
			return WsCommon::RetRes::ERROR_NOT_SUPPORTED;
			break;
	}
	return WsCommon::RetRes::OK;
}

/******************************************************************************
 * @brief Get field value based on data type
 * @param field Field to reference its value
 * @param val Type pointer to field value
 * @retval ERROR_NOT_SUPPORTED if type is not implemented by field
 * @retval ERROR_BAD_PARAMS if parameteres are wrong
 * @retval ERROR_BAD_RESULT if field's decode_status is wrong
 * @retval OK
 ******************************************************************************/
template <typename T>
WsCommon::RetRes get_field_val(WsFields::Field* field, T* val)
{
	if(field == NULL || val == NULL) return WsCommon::RetRes::ERROR_BAD_PARAMS;
	if(field->decode_status != WsCommon::RetRes::OK) return field->decode_status;

	T* val_pt = NULL;
	WsCommon::RetRes ret = ref_field_val<T>(field, &val_pt);
	if(ret != WsCommon::RetRes::OK)
	{
		return ret;
	}
	if(val_pt != NULL)
	{
		*val = *val_pt;
	}
	return WsCommon::RetRes::OK;
}

/******************************************************************************
 * @brief Set field value based on data type
 * @param field Field to reference its value
 * @param val Type pointer to field value
 * @retval ERROR_NOT_SUPPORTED if type is not implemented by field
 * @retval ERROR_BAD_PARAMS if parameteres are wrong
 * @retval OK
 ******************************************************************************/
template <typename T>
WsCommon::RetRes set_field_val(WsFields::Field* field, T val)
{
	if(field == NULL) return WsCommon::RetRes::ERROR_BAD_PARAMS;

	T* val_pt = NULL;
	WsCommon::RetRes ret = ref_field_val<T>(field, &val_pt);
	if(ret != WsCommon::RetRes::OK)
	{
		return ret;
	}
	*val_pt = val;
	return WsCommon::RetRes::OK;
}

/******************************************************************************
 * @brief Get field value from pack. Provide either type or name.
 * @param field_pack Field pack
 * @param type Field type
 * @param name Field name
 * @param val Returned value
 * @retval ERROR_NOT_SUPPORTED if type is not implemented by field
 * @retval ERROR_BAD_PARAMS if parameteres are wrong
 * @retval ERROR_NOT_FOUND if field is not in pack
 * @retval OK
 ******************************************************************************/
template <typename T>
WsCommon::RetRes get_field_val_from_pack(FieldPack* field_pack, FieldType type, const char* name, T* val)
{
	if(type == FieldType::NONE && name == NULL) return WsCommon::RetRes::ERROR_BAD_PARAMS;
	if(type != FieldType::NONE && name != NULL) return WsCommon::RetRes::ERROR_BAD_PARAMS;

	for(int i = 0; i < field_pack->field_count; i++)
	{
		if(field_pack->fields[i].info == NULL) continue;

		if(name != NULL)
		{
			if(strcmp(name, field_pack->fields[i].info->name) == 0)
			{
				return get_field_val<T>(&field_pack->fields[i], val);
			}
		}
		else if(type != FieldType::NONE)
		{
			if(type == field_pack->fields[i].info->type)
			{
				return get_field_val<T>(&field_pack->fields[i], val);
			}
		}
	}
	return WsCommon::RetRes::ERROR_NOT_FOUND;
}

WsCommon::RetRes add_field_to_pack(FieldPack* field_pack, Field* field);
bool field_exist_in_pack(FieldPack* field_pack, FieldType type, uint8_t* index);
bool field_exist_in_pack(FieldPack* field_pack, const char* name, uint8_t* index);
void print_field_pack(FieldPack* field_pack);
void print_field(Field* field, bool print_agg_type);
WsCommon::CmpRes field_value_compare(Field* base_field, Field* compare_to_field);
WsCommon::RetRes field_value_addition(Field* base_field, Field* addend_field);
WsCommon::RetRes field_value_div_uint(Field* field, uint16_t divisor);
WsCommon::RetRes field_init(Field* field, const FieldDesc* field_desc);
WsCommon::RetRes field_packs_aggregation(FieldPack* field_pack_buff, uint16_t field_pack_buff_len, FieldPack* field_pack_agg);
WsCommon::RetRes field_pack_init(FieldPack* field);

}

#endif