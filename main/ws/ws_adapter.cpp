#include "ws/include/ws_adapter.h"

namespace WsAdapter
{

/******************************************************************************
 * @brief Get value name from an array of adapters
 * @param adapter_array Adapter array
 * @param adapter_array_count Adapter array length
 * @param field_name Name of field
 * @return Pointer to value name
 ******************************************************************************/
const char* get_value_name(const AdapterValueName** adapter_array, uint8_t  adapter_array_count, const char* field_name)
{
	if(adapter_array == NULL) return NULL;

	for(int i = 0; i < adapter_array_count; i++)
	{
		if(adapter_array[i]->ws_field_info == NULL) continue;
		if(strcmp(field_name, adapter_array[i]->ws_field_info->name) == 0)
		{
			return adapter_array[i]->value_name;
		}
	}
	return NULL;
}

/******************************************************************************
 * @brief Get value name from an array of adapters
 * @param adapter_array Adapter array
 * @param adapter_array_count Adapter array length
 * @param field_type Type of field
 * @return Pointer to value name
 ******************************************************************************/
const char* get_value_name(const AdapterValueName** adapter_array, uint8_t  adapter_array_count, WsFields::FieldType field_type)
{
	if(adapter_array == NULL) return NULL;
	for(int i = 0; i < adapter_array_count; i++)
	{
		if(adapter_array[i]->ws_field_info == NULL) continue;
		if(field_type == adapter_array[i]->ws_field_info->type)
		{
			return adapter_array[i]->value_name;
		}
	}
	return NULL;
}

}