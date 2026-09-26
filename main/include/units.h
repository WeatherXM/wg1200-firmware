#ifndef UNITS_H
#define UNITS_H

#include "struct.h"

namespace Units
{
	float convert_temp(float temp_c, TempUnit target_unit);
	const TempUnitDesc *get_temp_unit_desc(char *name, TempUnit unit);

	float convert_wind_speed(float speed_m_s, WindSpeedUnit target_unit);
	const WindSpeedUnitDesc *get_wind_speed_unit_desc(char *name, WindSpeedUnit unit);

	float convert_precip(float rain_mm, PrecipUnit target_unit);
	const PrecipUnitDesc *get_precip_unit_desc(char *name, PrecipUnit unit);

	float convert_press(float press_hpa, PressureUnit target_unit);
	const PressureUnitDesc *get_press_unit_desc(char *name, PressureUnit unit);

	float calculate_dew(float humidity, float temp);
	float calculate_feels_like(float temp, float wind_spd, float humidity);

	int m_s_to_bft(float speed_m_s);
	float celsius_to_f(float celsius);
	const char * wind_dir_to_cardinal(uint16_t wind_dir);
}

#endif
