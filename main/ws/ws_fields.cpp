#include "include/ws_fields.h"
#include <math.h>
#include "utils.h"

namespace WsFields
{

static const char TAG[] = "WsFields";

/******************************************************************************
 * @brief Initialize field
 * @param field Field to be initialized
 * @param field_desc Field descriptor
 * @retval ERROR_BAD_PARAMS if parameters are bad
 * @retval OK
 ******************************************************************************/
WsCommon::RetRes field_init(Field* field, const FieldDesc* field_desc)
{
	if(field == NULL || field_desc == NULL) return WsCommon::RetRes::ERROR_BAD_PARAMS;
	field->info = field_desc;
	field->decode_status = WsCommon::RetRes::NONE;
	switch(field->info->data_type)
	{
		case WsFields::ValueType::BOOL:
		case WsFields::ValueType::U8:
		case WsFields::ValueType::U16:
		case WsFields::ValueType::U32:
			field->value.val_u32 = 0U;
			break;
		case WsFields::ValueType::FLOAT:
			field->value.val_float = 0.0f;
			break;
		case WsFields::ValueType::INT:
			field->value.val_int = 0;
			break;
		default:
			break;
	}
	return WsCommon::RetRes::OK;
}

/******************************************************************************
 * @brief Initialize field pack
 * @param field_pack Field pack to be initialized
 * @retval ERROR_BAD_PARAMS if parameters are bad
 * @retval OK
 ******************************************************************************/
WsCommon::RetRes field_pack_init(FieldPack* field_pack)
{
	if(field_pack == NULL) return WsCommon::RetRes::ERROR_BAD_PARAMS;
	field_pack->agg_count = 0;
	field_pack->bad_values_detected = false;
	field_pack->field_count = 0;
	field_pack->timespan = 0;
	field_pack->ts = 0;
	field_pack->packer_name[0] = '\0';
	return WsCommon::RetRes::OK;
}

/******************************************************************************
 * @brief Add field to pack
 * @param field_pack Field pack to add the new field
 * @param field Field to add to pack
 * @retval ERROR_BAD_PARAMS if parameters are bad
 * @retval ERROR_NOT_SUPPORTED if field already exitsts in pack
 * @retval OK
 ******************************************************************************/
WsCommon::RetRes add_field_to_pack(FieldPack* field_pack, Field* field)
{
	if(field_pack == NULL || field == NULL) return WsCommon::RetRes::ERROR_BAD_PARAMS;

	if(field_pack->field_count >= MAX_FIELDS_IN_PACK)
	{
		ESP_LOGE(TAG, "add_field_to_pack: Field <%s> add error. Pack is full", field->info->name);
		return WsCommon::RetRes::ERROR;
	}
	if(field_exist_in_pack(field_pack, field->info->type, NULL)
		|| field_exist_in_pack(field_pack, field->info->name, NULL))
	{
		// Multilpe fields of the same type not supported in same pack
		ESP_LOGW(TAG, "add_field_to_pack: Field <%s> already in pack", field->info->name);
		return WsCommon::RetRes::ERROR_NOT_SUPPORTED;
	}

	memcpy(&field_pack->fields[field_pack->field_count], field, sizeof(Field));
	field_pack->field_count++;
	if(field->info != NULL && field->info->cat == FieldCatType::WEATHER && field->decode_status == WsCommon::RetRes::ERROR_BAD_RESULT)
	{
		field_pack->bad_values_detected = true;
	}
	return WsCommon::RetRes::OK;
}

/******************************************************************************
 * @brief Check if field exist in pack
 * @param field_pack Field pack to add the new field.
 * @param type Field type
 * @param index Index the field was found. Can be NULL
 * @retval true if field exist in pack
 ******************************************************************************/
bool field_exist_in_pack(FieldPack* field_pack, FieldType type, uint8_t* index)
{
	if(type == FieldType::NONE) return false;
	for(int i = 0; i < field_pack->field_count; i++)
	{
		if(field_pack->fields[i].info->type == type)
		{
			if(index != NULL)
			{
				*index = i;
			}			
			return true;
		}
	}
	return false;
}

/******************************************************************************
 * @brief Check if field exist in pack
 * @param field_pack Field pack to add the new field.
 * @param name Field name
 * @param index Index the field was found. Can be NULL
 * @retval true if field exist in pack
 ******************************************************************************/
bool field_exist_in_pack(FieldPack* field_pack, const char* name, uint8_t* index)
{
	for(int i = 0; i < field_pack->field_count; i++)
	{
		if(strcmp(name, field_pack->fields[i].info->name) == 0)
		{
			if(index != NULL)
			{
				*index = i;
			}		
			return true;
		}
	}
	return false;
}

/******************************************************************************
 * @brief Compare field value
 * @param base_field Base field
 * @param compare_to_field Field to be compared to
 * @retval GREATER base_field val is greater than compare_to_field val
 * @retval LESS base_field val is less than compare_to_field val
 * @retval EQUAL base_field val is equal to compare_to_field val
 ******************************************************************************/
WsCommon::CmpRes field_value_compare(Field* base_field, Field* compare_to_field)
{
	WsCommon::CmpRes ret_val = WsCommon::CmpRes::NONE;

	if(base_field == NULL || compare_to_field == NULL)
	{
		return ret_val;
	}

	if(base_field->info->data_type != compare_to_field->info->data_type)
	{
		return ret_val;
	}
	int cmp = 0;
	switch(base_field->info->data_type)
	{
		case WsFields::ValueType::BOOL:
		case WsFields::ValueType::U8:
		case WsFields::ValueType::U16:
		case WsFields::ValueType::U32:
			cmp = memcmp(&base_field->value, &compare_to_field->value, sizeof(uint32_t));
			if(cmp > 0)
			{
				ret_val = WsCommon::CmpRes::GREATER;
			}
			else if(cmp < 0)
			{
				ret_val =  WsCommon::CmpRes::LESS;
			}
			else ret_val =  WsCommon::CmpRes::EQUAL;
			break;
		case WsFields::ValueType::FLOAT:
			if(base_field->value.val_float > compare_to_field->value.val_float)
			{
				ret_val = WsCommon::CmpRes::GREATER;
			}
			else if(base_field->value.val_float < compare_to_field->value.val_float)
			{
				ret_val = WsCommon::CmpRes::LESS;
			}
			else ret_val = WsCommon::CmpRes::EQUAL;
			break;
		case WsFields::ValueType::INT:
			if(base_field->value.val_int > compare_to_field->value.val_int)
			{
				ret_val = WsCommon::CmpRes::GREATER;
			}
			else if(base_field->value.val_int < compare_to_field->value.val_int)
			{
				ret_val = WsCommon::CmpRes::LESS;
			}
			else ret_val = WsCommon::CmpRes::EQUAL;
			break;
		default:
			break;
	}
	return ret_val;
}

/******************************************************************************
 * @brief Add addend_field value to base_field value
 * @param base_field Base field
 * @param addend_field Field to be added
 * @retval ERROR_BAD_PARAMS if parameters are bad
 * @retval OK
 ******************************************************************************/
WsCommon::RetRes field_value_addition(Field* base_field, Field* addend_field)
{
	if(base_field == NULL || addend_field == NULL)
	{
		return WsCommon::RetRes::ERROR_BAD_PARAMS;
	}

	if(base_field->info->data_type != addend_field->info->data_type)
	{
		return WsCommon::RetRes::ERROR_BAD_PARAMS;
	}

	switch(base_field->info->data_type)
	{
		case WsFields::ValueType::BOOL:
			base_field->value.val_bool = base_field->value.val_bool || addend_field->value.val_bool;
			break;
		case WsFields::ValueType::U8:
		case WsFields::ValueType::U16:
		case WsFields::ValueType::U32:
			base_field->value.val_u32 += addend_field->value.val_u32;
			break;
		case WsFields::ValueType::FLOAT:
			base_field->value.val_float += addend_field->value.val_float;
			break;
		case WsFields::ValueType::INT:
			base_field->value.val_int += addend_field->value.val_int;
			break;
		default:
			break;
	}
	return WsCommon::RetRes::OK;
}

/******************************************************************************
 * @brief Divide field value by unsigned integer
 * @param field Base field
 * @param divisor Unsigned integer divisor
 * @retval ERROR_BAD_PARAMS if parameters are bad
 * @retval OK
 ******************************************************************************/
WsCommon::RetRes field_value_div_uint(Field* field, uint16_t divisor)
{
	if(field == NULL || divisor == 0)
	{
		return WsCommon::RetRes::ERROR_BAD_PARAMS;
	}
	switch(field->info->data_type)
	{
		case WsFields::ValueType::BOOL:
			// Do nothing for booleans
			break;
		case WsFields::ValueType::U8:
		case WsFields::ValueType::U16:
		case WsFields::ValueType::U32:
			field->value.val_u32 /= divisor;
			break;
		case WsFields::ValueType::FLOAT:
			field->value.val_float /= (float) divisor;
			break;
		case WsFields::ValueType::INT:
			field->value.val_int /= divisor;
			break;
		default:
			break;
	}
	return WsCommon::RetRes::OK;
}

/******************************************************************************
 * @brief Aggregate an array of packs into one pack
 * @param field_pack_buff Array of field packs
 * @param field_pack_buff_len Array of field packs length
 * @param field_pack_agg Field pack to hold the aggregation
 * @retval ERROR_BAD_PARAMS if parameters are bad
 * @retval OK
 ******************************************************************************/
WsCommon::RetRes field_packs_aggregation(FieldPack* field_pack_buff, uint16_t field_pack_buff_len, FieldPack* field_pack_agg)
{
	if(field_pack_buff_len == 0) return WsCommon::RetRes::ERROR_BAD_PARAMS;
	if(field_pack_buff == NULL || field_pack_agg == NULL) return WsCommon::RetRes::ERROR_BAD_PARAMS;

	// TODO: ATTENTION! If buffer has field packs with different field_count,
	// they will be aggregated according to &field_pack_buff[0] field_count.
	// So if for example there are 3 packs in buff with field_counts of 7, 10 and 12,
	// only 7outof10 and 7outof12 will be aggregated from 2nd and 3rd pack.
	// If there are 3 packs in buff with field_counts of 12, 10 and 7,
	// 2 fields will be aggregated only fromn 1st pack and 3 fields only from 1st and 2nd pack.
	WsFields::FieldPack* ref_field_pack = &field_pack_buff[0];

	for(int field_idx = 0; field_idx < ref_field_pack->field_count; field_idx++)
	{
		WsFields::Field aggregated_field;
		field_init(&aggregated_field, ref_field_pack->fields[field_idx].info);
		aggregated_field.decode_status = WsCommon::RetRes::OK;

		float temp_float_1 = 0.0f, temp_float_2 = 0.0f; // Messy but wind_dir is the only custom aggregation atm
		for(int pack_buff_i = 0; pack_buff_i < field_pack_buff_len; pack_buff_i++)
		{
			WsFields::Field* cur_pack_field = &field_pack_buff[pack_buff_i].fields[field_idx];

			if(cur_pack_field->decode_status == WsCommon::RetRes::NONE)
			{
				// This will occur if packs of diff field count added in buffer
				continue;
			}

			if(cur_pack_field->decode_status != WsCommon::RetRes::OK)
			{
				// We will not support aggregation of most fields.
				// If there is at least one decoding error we skip the field
				ESP_LOGE(TAG, "field_packs_aggregation: Field <%s> has bad falues. Excluded from pack.", cur_pack_field->info->name);
				aggregated_field.decode_status = WsCommon::RetRes::ERROR_BAD_RESULT;
				break;
			}

			if(aggregated_field.info->type != cur_pack_field->info->type)
			{
				// We will not support randomly packed fields.
				// All fields from a certain station should be packed in same order.
				ESP_LOGE(TAG, "field_packs_aggregation: Pack field order is messed up");
				return WsCommon::RetRes::ERROR;
			}

			switch(cur_pack_field->info->agg_type)
			{
				case WsFields::FieldAggType::NONE:
					// Justy overwrite it every time
					aggregated_field.value = cur_pack_field->value;
					break;
				case WsFields::FieldAggType::AVG:
					WsFields::field_value_addition(&aggregated_field, cur_pack_field);
					break;
				case WsFields::FieldAggType::MAX:
					if(pack_buff_i == 0) aggregated_field.value = cur_pack_field->value;
					else
					{
						WsCommon::CmpRes cmp = WsFields::field_value_compare(cur_pack_field, &aggregated_field);
						if(cmp == WsCommon::CmpRes::GREATER) aggregated_field.value = cur_pack_field->value;
					}
					break;
				case WsFields::FieldAggType::MIN:
					if(pack_buff_i == 0) aggregated_field.value = cur_pack_field->value;
					else
					{
						WsCommon::CmpRes cmp = WsFields::field_value_compare(cur_pack_field, &aggregated_field);
						if(cmp == WsCommon::CmpRes::LESS) aggregated_field.value = cur_pack_field->value;
					}
					break;
				case WsFields::FieldAggType::CUSTOM:
					// Add custom aggregations here.
					if(cur_pack_field->info->type == WsFields::FieldType::WEATHER_WIND_DIR)
					{
						temp_float_1 += sin(Utils::deg_to_rad(cur_pack_field->value.val_u16));
						temp_float_2 += cos(Utils::deg_to_rad(cur_pack_field->value.val_u16));
					}
					break;
				default:
					break;
			}
		}
		if(aggregated_field.info->agg_type == WsFields::FieldAggType::AVG)
		{
			WsFields::field_value_div_uint(&aggregated_field, field_pack_buff_len);
		}
		else if(aggregated_field.info->agg_type == WsFields::FieldAggType::CUSTOM)
		{
			//extra stuff for custom agg. Should there be callbacks for custom agg fields?
			if(aggregated_field.info->type == WsFields::FieldType::WEATHER_WIND_DIR)
			{
				float wind_dir_avg = atan2(temp_float_1 / field_pack_buff_len, temp_float_2 / field_pack_buff_len);
				wind_dir_avg = Utils::rad_to_deg(wind_dir_avg);
				if(wind_dir_avg < 0) wind_dir_avg += 360;
				aggregated_field.value.val_u16 = (uint16_t) wind_dir_avg;
			}			
		}
		
		//add field
		field_pack_agg->agg_count = field_pack_buff_len;
		field_pack_agg->ts = ref_field_pack->ts;
		if(WS_DATA_AGGREGATE_TS_ROUND == WsDataAggrTsRound::CEIL)
		{
			field_pack_agg->ts = field_pack_buff[field_pack_buff_len - 1].ts;
		}
		
		if(field_pack_buff[field_pack_buff_len - 1].ts < ref_field_pack->ts)
		{
			field_pack_agg->timespan = 0;
		}
		else
		{
			field_pack_agg->timespan = field_pack_buff[field_pack_buff_len - 1].ts - ref_field_pack->ts;
		}
		memcpy(field_pack_agg->packer_name, ref_field_pack->packer_name, sizeof(field_pack_agg->packer_name));
		WsCommon::RetRes ret = WsFields::add_field_to_pack(field_pack_agg, &aggregated_field);
		if(ret != WsCommon::RetRes::OK)
		{
			ESP_LOGE(TAG, "field_packs_aggregation: Add Field<%s> to pack failed", aggregated_field.info->name);
			WsCommon::print_ret_val(ret);
		}
	}

	return WsCommon::RetRes::OK;
}

/******************************************************************************
 * @brief Print field paramaters
 * @param field Field to print
 * @param print_agg_type if true, aggregation type of the field is printed too
 ******************************************************************************/
void print_field(Field* field, bool print_agg_type)
{
	printf("%s, ", field->info->name);
	if(field->info->agg_type != WsFields::FieldAggType::NONE && print_agg_type)
	{
		printf("AGG_TP: ");
		switch(field->info->agg_type)
		{
			case WsFields::FieldAggType::MAX:
				printf("MAX, ");
				break;
			case WsFields::FieldAggType::MIN:
				printf("MIN, ");
				break;
			case WsFields::FieldAggType::AVG:
				printf("AVG, ");
				break;
			case WsFields::FieldAggType::CUSTOM:
				printf("CUSTOM, ");
				break;
			default:
				break;
		}
	}	
	printf("Val: ");

	if(field->decode_status == WsCommon::RetRes::ERROR_BAD_RESULT)
	{
		printf("Bad Result\n");
		return;
	}
	if(field->decode_status != WsCommon::RetRes::OK)
	{
		printf("Unavailable\n");
		return;
	}
		
	switch(field->info->data_type)
	{
		
		case WsFields::ValueType::BOOL:
			printf("%s", field->value.val_bool ? "True" : "False");
			break;
		case WsFields::ValueType::FLOAT:
			printf("%.*f", field->info->decimals, field->value.val_float);
			break;
		case WsFields::ValueType::INT:
			printf("%d", field->value.val_int);
			break;
		case WsFields::ValueType::U16:
			printf("%u", field->value.val_u16);
			break;
		case WsFields::ValueType::U32:
			printf("%lu", field->value.val_u32);
			break;
		case WsFields::ValueType::U8:
			printf("%d", field->value.val_u8);
			break;
		default:
			printf("Unhandled ValueType");
			break;
	}
	printf("\n");
}

/******************************************************************************
 * @brief Print field pack paramaters
 * @param field Field pack to print
 ******************************************************************************/
void print_field_pack(FieldPack* field_pack)
{
	printf("---------- %s Field Pack ----------\n", field_pack->packer_name);
	printf("Timestamp: %lu\n", field_pack->ts);
	printf("Timespan: %u\n", field_pack->timespan);
	printf("Aggregation Count: %d\n", field_pack->agg_count);
	printf("Field Count: %u\n", field_pack->field_count);
	printf("Bad Values Detected: %s\n", field_pack->bad_values_detected ? "True" : "False");
	printf("-------------- Fields -----------------\n");
	for(int i = 0; i < field_pack->field_count; i++)
	{
		print_field(&field_pack->fields[i], field_pack->agg_count > 1 ? true : false);
	}
	printf("--------------------------------------\n");
}

}