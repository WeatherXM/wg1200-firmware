#ifndef WS_ADAPTER_H
#define WS_ADAPTER_H

#include "ws/include/ws_fields.h"

namespace WsAdapter
{

/******************************************************************************
 * @brief Value name adapter. Bind WsFields::FieldDesc to a name
 ******************************************************************************/
struct AdapterValueName
{
	const WsFields::FieldDesc* ws_field_info;
	const char* value_name;
};
const char* get_value_name(const AdapterValueName** adapter_array, uint8_t  adapter_array_count, const char* field_name);
const char* get_value_name(const AdapterValueName** adapter_array, uint8_t  adapter_array_count, WsFields::FieldType field_type);

}

#endif