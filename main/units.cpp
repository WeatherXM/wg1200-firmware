#include "units.h"
#include "const.h"
#include "math.h"
#include "string.h"

namespace Units
{

/******************************************************************************
 * Convert m/s to BFT scale
 ******************************************************************************/
int m_s_to_bft(float speed_m_s)
{
	// Map [BFT index] : min speed (m/s) for this index
	static const float BFT_SCALE[] = {0, 0.5, 1.6, 3.4, 5.5, 8, 10.8, 13.9, 17.2, 20.8, 24.5, 28.5, 32.7};

	int bft = 0;
	for(int i = sizeof(BFT_SCALE) / sizeof(BFT_SCALE[0]) - 1; i >= 0; i--)
	{
		if(speed_m_s >= BFT_SCALE[i])
		{
			bft = i;
			break;
		}
	}

	return bft;
}

/******************************************************************************
 * Convert temperature from temp_c to target_unit
 ******************************************************************************/
float convert_temp(float temp_c, TempUnit target_unit)
{
	float converted = 0.0f;
	const TempUnitDesc *descr = get_temp_unit_desc(nullptr, target_unit);
	if(descr == nullptr)
	{
		return 0.0f;
	}

	switch(target_unit)
	{
	case TempUnit::TEMP_UNIT_C:
		converted = temp_c;
		break;
	case TempUnit::TEMP_UNIT_F:
		converted = celsius_to_f(temp_c);
		break;
	default:
		converted = 0.0f;
		break;
	}

	return converted;
}

/******************************************************************************
 * Get temp unit descriptor by id, title or symbol
 ******************************************************************************/
const TempUnitDesc *get_temp_unit_desc(char *title_or_symbol, TempUnit unit)
{
	// Only title/symbol or unit must be set
	if(!((title_or_symbol == nullptr) ^ (unit == TempUnit::TEMP_UNIT_NONE)))
	{
		return nullptr;
	}

	const int LAST_INDEX = sizeof(TEMP_UNITS) / sizeof(TEMP_UNITS[0]);
	int found_index = -1;

	if(title_or_symbol != nullptr)
	{
		for(size_t i = 0; i < LAST_INDEX; i++)
		{
			if(strcmp(title_or_symbol, TEMP_UNITS[i].symbol) == 0 || strcmp(title_or_symbol, TEMP_UNITS[i].title) == 0)
			{
				found_index = i;
				break;
			}
		}
	}
	else
	{
		for(size_t i = 0; i < LAST_INDEX; i++)
		{
			if(TEMP_UNITS[i].unit == unit)
			{
				found_index = i;
				break;
			}
		}
	}

	return found_index >= 0 ? &TEMP_UNITS[found_index] : nullptr;
}

/******************************************************************************
 * Convert wind speed from speed_m_s to target_unit
 ******************************************************************************/
float convert_wind_speed(float speed_m_s, WindSpeedUnit target_unit)
{
	float converted = 0.0f;

	const WindSpeedUnitDesc *descr = get_wind_speed_unit_desc(nullptr, target_unit);
	if(descr == nullptr)
	{
		return 0.0f;
	}

	switch(target_unit)
	{
	case WindSpeedUnit::WIND_SPEED_UNIT_M_S:
		converted = speed_m_s;
		break;
	case WindSpeedUnit::WIND_SPEED_UNIT_KM_H:
		converted = speed_m_s * UNIT_M_S_TO_KM_H;
		break;
	case WindSpeedUnit::WIND_SPEED_UNIT_MI_H:
		converted = speed_m_s * UNIT_M_S_TO_MI_H;
		break;
	case WindSpeedUnit::WIND_SPEED_UNIT_FT_S:
		converted = speed_m_s * UNIT_M_S_TO_FT_S;
		break;
	case WindSpeedUnit::WIND_SPEED_UNIT_BEAUFORT:
		converted = m_s_to_bft(speed_m_s);
		break;
	case WindSpeedUnit::WIND_SPEED_UNIT_KNOTS:
		converted = speed_m_s * UNIT_M_S_TO_KNOTS;
		break;
	default:
		converted = 0.0f;
		break;
	}

	return converted;
}

/******************************************************************************
 * Get wind speed descriptor for a unit by id, title or symbol
 ******************************************************************************/
const WindSpeedUnitDesc *get_wind_speed_unit_desc(char *title_or_symbol, WindSpeedUnit unit)
{
	// Only title/symbol or unit must be set
	if(!((title_or_symbol == nullptr) ^ (unit == WindSpeedUnit::WIND_SPEED_UNIT_NONE)))
	{
		return nullptr;
	}

	const int LAST_INDEX = sizeof(WIND_SPEED_UNITS) / sizeof(WIND_SPEED_UNITS[0]);
	int found_index = -1;

	if(title_or_symbol != nullptr)
	{
		for(size_t i = 0; i < LAST_INDEX; i++)
		{
			if(strcmp(title_or_symbol, WIND_SPEED_UNITS[i].symbol) == 0 || strcmp(title_or_symbol, WIND_SPEED_UNITS[i].title) == 0)
			{
				found_index = i;
				break;
			}
		}
	}
	else
	{
		for(size_t i = 0; i < LAST_INDEX; i++)
		{
			if(WIND_SPEED_UNITS[i].unit == unit)
			{
				found_index = i;
				break;
			}
		}
	}

	return found_index >= 0 ? &WIND_SPEED_UNITS[found_index] : nullptr;
}

/******************************************************************************
 * Convert precipitation from precip_mm to target_unit
 ******************************************************************************/
float convert_precip(float precip_mm, PrecipUnit target_unit)
{
	float converted = 0.0f;

	const PrecipUnitDesc *unit_desc = get_precip_unit_desc(nullptr, target_unit);
	if(unit_desc == nullptr)
	{
		return 0.0f;
	}

	switch(target_unit)
	{
	case PrecipUnit::PRECIP_UNIT_MM:
		converted = precip_mm;
		break;
	case PrecipUnit::PRECIP_UNIT_IN:
		converted = precip_mm * UNIT_MM_TO_IN;
		break;
	default:
		converted = 0.0f;
		break;
	}

	return converted;
}

/******************************************************************************
 * Get precipitation unit descriptor by id, title or symbol
 ******************************************************************************/
const PrecipUnitDesc *get_precip_unit_desc(char *title_or_symbol, PrecipUnit unit)
{
	// Only title/symbol or unit must be set
	if(!((title_or_symbol == nullptr) ^ (unit == PrecipUnit::PRECIP_UNIT_NONE)))
	{
		return nullptr;
	}

	const int LAST_INDEX = sizeof(PRECIP_UNITS) / sizeof(PRECIP_UNITS[0]);
	int found_index = -1;

	if(title_or_symbol != nullptr)
	{
		for(size_t i = 0; i < LAST_INDEX; i++)
		{
			if(strcmp(title_or_symbol, PRECIP_UNITS[i].symbol) == 0)
			{
				found_index = i;
				break;
			}
			if(strcmp(title_or_symbol, PRECIP_UNITS[i].title) == 0)
			{
				found_index = i;
				break;
			}
		}
	}
	else
	{
		for(size_t i = 0; i < LAST_INDEX; i++)
		{
			if(PRECIP_UNITS[i].unit == unit)
			{
				found_index = i;
				break;
			}
		}
	}

	return found_index >= 0 ? &PRECIP_UNITS[found_index] : nullptr;
}

/******************************************************************************
 * Convert pressure from press_hpa to target_unit
 ******************************************************************************/
float convert_press(float press_hpa, PressureUnit target_unit)
{
	float converted = 0.0f;

	const PressureUnitDesc *unit_desc = get_press_unit_desc(nullptr, target_unit);
	if(unit_desc == nullptr)
	{
		return 0.0f;
	}

	switch(target_unit)
	{
	case PressureUnit::PRESSURE_UNIT_HPA:
		converted = press_hpa;
		break;
	case PressureUnit::PRESSURE_UNIT_MM_HG:
		converted = press_hpa * UNIT_HPA_TO_MMHG;
		break;
	case PressureUnit::PRESSURE_UNIT_IN_HG:
		converted = press_hpa * UNIT_HPA_TO_INHG;
		break;
	default:
		converted = 0.0f;
		break;
	}

	return converted;
}

/******************************************************************************
 * Calculate dew point temp in Celsius
 ******************************************************************************/
float calculate_dew(float humidity, float temp)
{
	float a = 17.625;
	float b = 243.04;
	float c = log(humidity / 100) + (a * temp) / (b + temp);
	return (b * c) / (a - c);
}

/******************************************************************************
 * Calculate feels like temp in Celsius
 ******************************************************************************/
float calculate_feels_like(float temp, float wind_spd, float humidity)
{
	if(temp <= 10.0)
	{
		/**
		 * Wind speed is in m/s but we need it at km/h on this formula
		 */
		wind_spd *= UNIT_M_S_TO_KM_H;
		float windChill =
			13.12 + ((0.6215 * temp) - (11.37 * pow(wind_spd, 0.16))) + (0.3965 * temp * pow(wind_spd, 0.16));
		if(windChill <= temp)
		{
			return windChill;
		}
		else
		{
			return temp;
		}
	}
	else if(temp > 26.7)
	{
		return -8.785 + (1.611 * temp) + (2.339 * humidity) - (0.146 * temp * humidity) - (0.01231 * pow(temp, 2)) -
				(0.01642 * pow(humidity, 2)) + (0.002212 * pow(temp, 2) * humidity) +
				(0.0007255 * temp * pow(humidity, 2)) - (0.000003582 * pow(temp, 2) * pow(humidity, 2));
	}
	return temp;
}

/******************************************************************************
 * Get pressure unit descriptor by id, title or name
 ******************************************************************************/
const PressureUnitDesc *get_press_unit_desc(char *title_or_symbol, PressureUnit unit)
{
	// Only title/symbol or unit must be set
	if(!((title_or_symbol == nullptr) ^ (unit == PressureUnit::PRESSURE_UNIT_NONE)))
	{
		return nullptr;
	}

	const int LAST_INDEX = sizeof(PRESSURE_UNITS) / sizeof(PRESSURE_UNITS[0]);
	int found_index = -1;

	if(title_or_symbol != nullptr)
	{
		for(size_t i = 0; i < LAST_INDEX; i++)
		{
			if(strcmp(title_or_symbol, PRESSURE_UNITS[i].symbol) == 0)
			{
				found_index = i;
				break;
			}
			if(strcmp(title_or_symbol, PRESSURE_UNITS[i].title) == 0)
			{
				found_index = i;
				break;
			}
		}
	}
	else
	{
		for(size_t i = 0; i < LAST_INDEX; i++)
		{
			if(PRESSURE_UNITS[i].unit == unit)
			{
				found_index = i;
				break;
			}
		}
	}

	return found_index >= 0 ? &PRESSURE_UNITS[found_index] : nullptr;
}

/******************************************************************************
 * Convert celsius to fahrenheit
 * @param celsius Temp in celsius
 ******************************************************************************/
float celsius_to_f(float celsius)
{
	return (celsius / 5 * 9) + 32;
}

/******************************************************************************
 * Convert wind direction degrees to cardinal string
 * @param wind_dir Direction in degrees
 ******************************************************************************/
const char * wind_dir_to_cardinal(uint16_t wind_dir)
{
	int dir_index = ((int)(wind_dir + 22.5f) % 360) / 45;
	if(dir_index > sizeof(WIND_DIRECTION_CARDINAL_NAMES) / sizeof(WIND_DIRECTION_CARDINAL_NAMES[0]) - 1)
	{
		dir_index = -1;
	}
	if(dir_index < 0)
	{
		return "-";
	}
	return &WIND_DIRECTION_CARDINAL_NAMES[dir_index][0];
}

} // namespace Units
