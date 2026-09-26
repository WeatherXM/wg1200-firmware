#include "ui/views/include/ui_view_sniffer.h"
#include <ctime>
#include "settings.h"
#include "units.h"
#include "wifi.h"
#include "ws/include/ws_data.h"
#include "ws/include/ws_handler.h"


namespace UiViewSniffer
{

View *_view_context = NULL;

const int UI_SNIFFER_STATUSBAR_HEIGHT = 30;
const int UI_SNIFFER_TABVIEW_HEIGHT = 450;

View &View::create(lv_obj_t *parent)
{
	_last_dew = 0.0f;
	strcpy(_last_dew_units, "C");
	if(NULL != _view_context)
	{
		return *this;
	}
	_parent_lv_obj = parent;
	lv_obj_t *old_scr = lv_scr_act();
	_self_lv_obj = lv_obj_create(NULL);
	lv_scr_load(_self_lv_obj);
	if(old_scr != NULL && old_scr != _self_lv_obj)
	{
		lv_obj_del_async(old_scr);
	}
	lv_obj_set_scrollbar_mode(_self_lv_obj, LV_SCROLLBAR_MODE_OFF);
	_tabview_main.create(_self_lv_obj)
		.size(UI_SCREEN_WIDTH, UI_SNIFFER_TABVIEW_HEIGHT)
		.align(LV_ALIGN_BOTTOM_MID, 0, 0);
	_view_statusbar.create(_self_lv_obj)
		.size(UI_SCREEN_WIDTH, UI_SNIFFER_STATUSBAR_HEIGHT)
		.align(LV_ALIGN_TOP_MID, 0, 0);

	_tabview_main.add_tab(&_tab_settings, _STR_SETTINGS_TAB);
	lv_obj_set_scrollbar_mode(_tab_settings, LV_SCROLLBAR_MODE_OFF);
	lv_obj_set_style_pad_all(_tab_settings, 0, LV_PART_MAIN);

	_tabview_main.add_tab(&_tab_tiles, _STR_TILES_TAB);
	lv_obj_set_scrollbar_mode(_tab_tiles, LV_SCROLLBAR_MODE_OFF);
	lv_obj_set_style_pad_all(_tab_tiles, 0, LV_PART_MAIN);

	_tabview_main.add_tab(&_tab_detailed, _STR_DETAILED_TAB);
	lv_obj_set_scrollbar_mode(_tab_detailed, LV_SCROLLBAR_MODE_OFF);
	lv_obj_set_style_pad_all(_tab_detailed, 0, LV_PART_MAIN);

	_tabview_main.add_tab(&_tab_crypto, _STR_CRYPTO_TAB);
	lv_obj_set_scrollbar_mode(_tab_crypto, LV_SCROLLBAR_MODE_OFF);
	lv_obj_set_style_pad_all(_tab_crypto, 0, LV_PART_MAIN);

	_tabview_main.sel_tab(1);

	_tabview_main.set_cb([](lv_event_t *event) {
		lv_event_code_t  event_code = lv_event_get_code(event);		
		if(event_code == LV_EVENT_VALUE_CHANGED)
		{
			UiViewSniffer::View *view = (UiViewSniffer::View *) lv_event_get_user_data(event);
			view->_view_statusbar.update_nav_index(lv_tabview_get_tab_act(view->_tabview_main.self_lv_obj()));
			lv_event_send(view->self_lv_obj(), LV_EVENT_VALUE_CHANGED, NULL);
		}
	}, this); 

	_view_tiles.create(_tab_tiles)
		.size(UI_SCREEN_WIDTH, UI_SNIFFER_TABVIEW_HEIGHT)
		.clickable(false)
		.align(LV_ALIGN_BOTTOM_MID, 0, 0);
	_view_detailed.create(_tab_detailed)
		.size(UI_SCREEN_WIDTH, UI_SNIFFER_TABVIEW_HEIGHT)
		.clickable(false)
		.align(LV_ALIGN_BOTTOM_MID, 0, 0);
	_view_settings.create(_tab_settings)
		.size(UI_SCREEN_WIDTH, 1364)
		// .clickable(false)
		.align(LV_ALIGN_TOP_MID, 0, 0);
	_view_crypto.create(_tab_crypto)
		.size(UI_SCREEN_WIDTH, UI_SNIFFER_TABVIEW_HEIGHT)
		.clickable(false)
		.align(LV_ALIGN_TOP_MID, 0, 0);

	_view_context = this;

	_layer_popups = lv_obj_create(_self_lv_obj); // Keep it safe. That's the top-most layer of the sniffer view!
	lv_obj_remove_style_all(_layer_popups);
	lv_obj_set_size(_layer_popups, UI_SCREEN_WIDTH, UI_SCREEN_HEIGHT);
	lv_obj_align(_layer_popups, LV_ALIGN_CENTER, 0, 0);	
	lv_obj_set_style_bg_opa(_layer_popups, LV_OPA_TRANSP, LV_PART_MAIN);
	lv_obj_set_style_border_opa(_layer_popups, LV_OPA_TRANSP, LV_PART_MAIN);
	lv_obj_move_foreground(_layer_popups);

	set_background_touch(true);

	return *this;
}

void View::set_background_touch(bool flag)
{
	if(flag)
	{
		lv_obj_clear_flag(get_popups_layer(), LV_OBJ_FLAG_CLICKABLE);		
	}
	else
	{
		lv_obj_add_flag(get_popups_layer(), LV_OBJ_FLAG_CLICKABLE);
	}    
}

void View::update_temp(float temp, char *units)
{
	char buff[10];
	snprintf(buff, sizeof(buff), "%.1f°%s", temp, units);
	_view_detailed.update_temp(buff);
	_view_detailed.update_temp_chart_temp(temp);
	_view_tiles.update_temp(buff);
	_view_crypto.update_temp(temp, units);
}

void View::update_feels_like(float feels_like, char *units)
{
	char buff[10];
	snprintf(buff, sizeof(buff), "%.0f°%s", feels_like, units);
	_view_detailed.update_feels_like(buff);
	_view_detailed.update_temp_chart_feels_like(feels_like);
	_view_tiles.update_feels_like(buff);
	_view_crypto.update_feels_like(feels_like, units);
}

void View::update_icon(UiTypes::WeatherIconType icon)
{
	_view_tiles.update_icon(icon);
}

void View::update_precip(float daily, float rate)
{
	char daily_buff[10];
	if(daily == 0.0f)
	{
		snprintf(daily_buff, sizeof(daily_buff), "0.0");
	}
	else if(daily > 99.9f)
	{
		snprintf(daily_buff, sizeof(daily_buff), "%.0f", daily);
	}
	else
	{
		snprintf(daily_buff, sizeof(daily_buff), "%.1f", daily);
	}

	char rate_buff[10];
	if(rate == 0.0f)
	{
		snprintf(rate_buff, sizeof(rate_buff), "0.0");
	}
	else if(rate > 99.9f)
	{
		snprintf(rate_buff, sizeof(rate_buff), "%.0f", rate);
	}
	else
	{
		snprintf(rate_buff, sizeof(rate_buff), "%.1f", rate);
	}

	// Screen 2 (Tiles view with big temperature icon): Shows live Rain Rate
	UiTypes::SimpleTile::update_per_type(_view_tiles.get_simple_tiles_array(), _view_tiles.get_simple_tiles_array_len(), 
		UiTypes::SimpleTileType::PRECIP_RATE, (char *)UI_SIMPLE_TILE_PRECIP_RATE_TITLE, rate_buff, NULL);

	// Screen 3 (Detailed view with charts): Shows cumulative Daily Rain
	UiTypes::SimpleTile::update_per_type(_view_detailed.get_simple_tiles_array(), _view_detailed.get_simple_tiles_array_len(), 
		UiTypes::SimpleTileType::PRECIP_DAILY, NULL, daily_buff, NULL);

	_view_crypto.update_precip(daily, rate);
}

void View::update_precip_rate(float rate)
{
	update_precip(rate, rate);
}

void View::update_precip_units(char *units)
{
	char rate_units[16];
	snprintf(rate_units, sizeof(rate_units), "%s/h", units);

	// Screen 2 (Tiles): mm/h or in/h
	UiTypes::SimpleTile::update_per_type(_view_tiles.get_simple_tiles_array(), _view_tiles.get_simple_tiles_array_len(), 
		UiTypes::SimpleTileType::PRECIP_RATE, NULL, NULL, rate_units);

	// Screen 3 (Detailed): Daily Rain (mm) or Daily Rain (in)
	char daily_title[32];
	snprintf(daily_title, sizeof(daily_title), "Daily Rain (%s)", units);
	UiTypes::SimpleTile::update_per_type(_view_detailed.get_simple_tiles_array(), _view_detailed.get_simple_tiles_array_len(), 
		UiTypes::SimpleTileType::PRECIP_DAILY, daily_title, NULL, NULL);

	_view_crypto.update_precip_units(units);
}

void View::update_daily_extremes(const WsData::DailyExtremes &extremes)
{
	char buf[32];

	// 1. Temperature min/max on TempDualTile
	if(!isnan(extremes.temp_min) && !isnan(extremes.temp_max))
	{
		float t_min = Units::convert_temp(extremes.temp_min, Settings::get_temp_unit());
		float t_max = Units::convert_temp(extremes.temp_max, Settings::get_temp_unit());
		snprintf(buf, sizeof(buf), LV_SYMBOL_DOWN " %.1f°  " LV_SYMBOL_UP " %.1f°", t_min, t_max);
		_view_detailed.update_temp_min_max(buf);
	}
	else
	{
		_view_detailed.update_temp_min_max("");
	}

	// 2. Wind peak gust on WindDualTile
	if(!isnan(extremes.wind_gust_max))
	{
		float g_max = Units::convert_wind_speed(extremes.wind_gust_max, Settings::get_wind_speed_unit());
		const WindSpeedUnitDesc *wind_desc = Units::get_wind_speed_unit_desc(nullptr, Settings::get_wind_speed_unit());
		snprintf(buf, sizeof(buf), "Max " LV_SYMBOL_UP " %.*f %s", g_max == 0.0f ? 0 : 1, g_max, wind_desc ? wind_desc->symbol : "");
		_view_detailed.update_wind_peak_gust(buf);
	}
	else
	{
		_view_detailed.update_wind_peak_gust("");
	}

	// 3. Pressure min/max on SimpleTile PRESSURE
	if(!isnan(extremes.pressure_min) && !isnan(extremes.pressure_max))
	{
		float p_min = Units::convert_press(extremes.pressure_min, Settings::get_press_unit());
		float p_max = Units::convert_press(extremes.pressure_max, Settings::get_press_unit());
		snprintf(buf, sizeof(buf), LV_SYMBOL_DOWN " %.1f  " LV_SYMBOL_UP " %.1f", p_min, p_max);
		UiTypes::SimpleTile::update_per_type(_view_detailed.get_simple_tiles_array(), _view_detailed.get_simple_tiles_array_len(), 
			UiTypes::SimpleTileType::PRESSURE, NULL, NULL, buf);
	}

	// 4. Humidity min/max on SimpleTile HUMIDITY
	if(!isnan(extremes.hum_min) && !isnan(extremes.hum_max))
	{
		snprintf(buf, sizeof(buf), LV_SYMBOL_DOWN " %.0f%%  " LV_SYMBOL_UP " %.0f%%", extremes.hum_min, extremes.hum_max);
		UiTypes::SimpleTile::update_per_type(_view_detailed.get_simple_tiles_array(), _view_detailed.get_simple_tiles_array_len(), 
			UiTypes::SimpleTileType::HUMIDITY, NULL, NULL, buf);
	}

	// 5. Daily Rain peak rate on SimpleTile PRECIP_DAILY
	if(!isnan(extremes.precip_rate_max))
	{
		float r_max = Units::convert_precip(extremes.precip_rate_max, Settings::get_precip_unit());
		const PrecipUnitDesc *p_desc = Units::get_precip_unit_desc(nullptr, Settings::get_precip_unit());
		snprintf(buf, sizeof(buf), "Max " LV_SYMBOL_UP " %.1f%s/h", r_max, p_desc ? p_desc->symbol : "");
		UiTypes::SimpleTile::update_per_type(_view_detailed.get_simple_tiles_array(), _view_detailed.get_simple_tiles_array_len(), 
			UiTypes::SimpleTileType::PRECIP_DAILY, NULL, NULL, buf);
	}

	// 6. UV Index peak on SimpleTile UV_INDEX
	if(extremes.uv_max > 0 || extremes.has_data)
	{
		snprintf(buf, sizeof(buf), "Max " LV_SYMBOL_UP " %u", extremes.uv_max);
		UiTypes::SimpleTile::update_per_type(_view_detailed.get_simple_tiles_array(), _view_detailed.get_simple_tiles_array_len(), 
			UiTypes::SimpleTileType::UV_INDEX, NULL, NULL, buf);
	}

	// 7. Solar irradiance peak on SimpleTile IRRADIANCE
	if(!isnan(extremes.solar_rad_max))
	{
		snprintf(buf, sizeof(buf), "Max " LV_SYMBOL_UP " %.0f", extremes.solar_rad_max);
		UiTypes::SimpleTile::update_per_type(_view_detailed.get_simple_tiles_array(), _view_detailed.get_simple_tiles_array_len(), 
			UiTypes::SimpleTileType::IRRADIANCE, NULL, NULL, buf);
	}
}

void View::update_wind_units(char *units)
{
	UiTypes::SimpleTile::update_per_type(_view_tiles.get_simple_tiles_array(), _view_tiles.get_simple_tiles_array_len(), 
		UiTypes::SimpleTileType::WIND, NULL, NULL, units);
	UiTypes::SimpleTile::update_per_type(_view_detailed.get_simple_tiles_array(), _view_detailed.get_simple_tiles_array_len(), 
		UiTypes::SimpleTileType::WIND, NULL, NULL, units);
	_view_detailed.update_wind_dual_units(units);
	_view_crypto.update_wind_units(units);
}

void View::update_wind_dir(uint16_t wind_dir)
{
	char buff[10];

	int dir_index = ((int)(wind_dir + 22.5f) % 360) / 45;
	char dir[][3] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
	if(dir_index > sizeof(dir) / sizeof(dir[0]) - 1)
	{
		dir_index = -1;
	}
	snprintf(buff, sizeof(buff), "%s", dir_index < 0 ? "-" : dir[dir_index]);
	UiTypes::SimpleTile::update_per_type(_view_tiles.get_simple_tiles_array(), _view_tiles.get_simple_tiles_array_len(), 
		UiTypes::SimpleTileType::WIND, buff, NULL, NULL);
	UiTypes::SimpleTile::update_per_type(_view_detailed.get_simple_tiles_array(), _view_detailed.get_simple_tiles_array_len(), 
		UiTypes::SimpleTileType::WIND, buff, NULL, NULL);
	_view_detailed.update_wind_dual_dir(buff);
}

void View::update_wind_speed_and_gust(float wind_spd, float wind_gust)
{
	char buff[15];
	int wind_spd_decimals = wind_spd == 0.0f ? 0 : 1;
	int wind_gust_decimals = wind_gust == 0.0f ? 0 : 1;
	snprintf(buff, sizeof(buff), "%.*f \\ %.*f", wind_spd_decimals, wind_spd, wind_gust_decimals, wind_gust);
	_view_detailed.update_wind_dual(buff);
	_view_detailed.update_wind_chart(wind_spd, wind_gust);

	snprintf(buff, sizeof(buff), "%.0f", wind_spd);
	UiTypes::SimpleTile::update_per_type(_view_tiles.get_simple_tiles_array(), _view_tiles.get_simple_tiles_array_len(), 
		UiTypes::SimpleTileType::WIND, NULL, buff, NULL);
	UiTypes::SimpleTile::update_per_type(_view_detailed.get_simple_tiles_array(), _view_detailed.get_simple_tiles_array_len(), 
		UiTypes::SimpleTileType::WIND, NULL, buff, NULL);
	_view_crypto.update_wind_spd(wind_spd);
}

void View::update_humidity(float hum)
{
	char buff[10];
	snprintf(buff, sizeof(buff), "%.0f", hum);
	UiTypes::SimpleTile::update_per_type(_view_tiles.get_simple_tiles_array(), _view_tiles.get_simple_tiles_array_len(), 
		UiTypes::SimpleTileType::HUMIDITY, NULL, buff, NULL);
	UiTypes::SimpleTile::update_per_type(_view_detailed.get_simple_tiles_array(), _view_detailed.get_simple_tiles_array_len(), 
		UiTypes::SimpleTileType::HUMIDITY, NULL, buff, NULL);
	_view_crypto.update_hum(hum);
}

void View::update_dew(float dew)
{
	_last_dew = dew;
	char buff[10];
	snprintf(buff, sizeof(buff), "%.1f", dew);
	UiTypes::SimpleTile::update_per_type(_view_tiles.get_simple_tiles_array(), _view_tiles.get_simple_tiles_array_len(), 
		UiTypes::SimpleTileType::DEW_POINT, NULL, buff, NULL);
	UiTypes::SimpleTile::update_per_type(_view_detailed.get_simple_tiles_array(), _view_detailed.get_simple_tiles_array_len(), 
		UiTypes::SimpleTileType::DEW_POINT, NULL, buff, NULL);
	_view_crypto.update_dew(dew);

	char detailed_buff[16];
	snprintf(detailed_buff, sizeof(detailed_buff), "%.1f°%s", _last_dew, _last_dew_units);
	_view_tiles.update_dew(detailed_buff);
}

void View::update_dew_units(char *units)
{
	if (units != NULL) {
		strncpy(_last_dew_units, units, sizeof(_last_dew_units) - 1);
		_last_dew_units[sizeof(_last_dew_units) - 1] = '\0';
	}
	UiTypes::SimpleTile::update_per_type(_view_tiles.get_simple_tiles_array(), _view_tiles.get_simple_tiles_array_len(), 
		UiTypes::SimpleTileType::DEW_POINT, NULL, NULL, units);
	UiTypes::SimpleTile::update_per_type(_view_detailed.get_simple_tiles_array(), _view_detailed.get_simple_tiles_array_len(), 
		UiTypes::SimpleTileType::DEW_POINT, NULL, NULL, units);
	_view_crypto.update_dew_units(units);

	char detailed_buff[16];
	snprintf(detailed_buff, sizeof(detailed_buff), "%.1f°%s", _last_dew, _last_dew_units);
	_view_tiles.update_dew(detailed_buff);
}

void View::update_pressure(float press)
{
	char buff[10];
	snprintf(buff, sizeof(buff), "%.1f", press);
	if(press < 0.0f)
	{
		snprintf(buff, sizeof(buff), UI_VALUES_DEFAULT);
	}
	UiTypes::SimpleTile::update_per_type(_view_tiles.get_simple_tiles_array(), _view_tiles.get_simple_tiles_array_len(), 
		UiTypes::SimpleTileType::PRESSURE, NULL, buff, NULL);
	UiTypes::SimpleTile::update_per_type(_view_detailed.get_simple_tiles_array(), _view_detailed.get_simple_tiles_array_len(), 
		UiTypes::SimpleTileType::PRESSURE, NULL, buff, NULL);
}

void View::update_pressure_units(char *units)
{
	// Screen 2 (Tiles): units in bottom row
	UiTypes::SimpleTile::update_per_type(_view_tiles.get_simple_tiles_array(), _view_tiles.get_simple_tiles_array_len(), 
		UiTypes::SimpleTileType::PRESSURE, NULL, NULL, units);

	// Screen 3 (Detailed): units in title, bottom row is min/max
	char press_title[32];
	snprintf(press_title, sizeof(press_title), "Pressure (%s)", units);
	UiTypes::SimpleTile::update_per_type(_view_detailed.get_simple_tiles_array(), _view_detailed.get_simple_tiles_array_len(), 
		UiTypes::SimpleTileType::PRESSURE, press_title, NULL, NULL);
}

void View::update_irradiance(float irr)
{
	char buff[10];
	snprintf(buff, sizeof(buff), "%.0f", irr);
	UiTypes::SimpleTile::update_per_type(_view_tiles.get_simple_tiles_array(), _view_tiles.get_simple_tiles_array_len(), 
		UiTypes::SimpleTileType::IRRADIANCE, NULL, buff, NULL);
	UiTypes::SimpleTile::update_per_type(_view_detailed.get_simple_tiles_array(), _view_detailed.get_simple_tiles_array_len(), 
		UiTypes::SimpleTileType::IRRADIANCE, NULL, buff, NULL);
}

void View::update_uv(uint8_t uv)
{
	char buff[10];
	snprintf(buff, sizeof(buff), "%d", uv);
	UiTypes::SimpleTile::update_per_type(_view_tiles.get_simple_tiles_array(), _view_tiles.get_simple_tiles_array_len(), 
		UiTypes::SimpleTileType::UV_INDEX, NULL, buff, NULL);
	UiTypes::SimpleTile::update_per_type(_view_detailed.get_simple_tiles_array(), _view_detailed.get_simple_tiles_array_len(), 
		UiTypes::SimpleTileType::UV_INDEX, NULL, buff, NULL);
}

void View::empty_weather_content()
{
	_view_detailed.update_wind_dual("--\\--");
	_view_detailed.update_wind_dual_dir(UI_VALUES_DEFAULT);
	_view_detailed.update_temp(UI_VALUES_DEFAULT);
	_view_detailed.update_feels_like(UI_VALUES_DEFAULT);
	_view_detailed.update_temp_min_max("");
	_view_detailed.update_wind_peak_gust("");
	_view_tiles.update_temp(UI_VALUES_DEFAULT);
	_view_tiles.update_feels_like(UI_VALUES_DEFAULT);
	_view_crypto.empty_weather_content();
	empty_ws_status();

	UiTypes::SimpleTile::update_per_type(_view_tiles.get_simple_tiles_array(), _view_tiles.get_simple_tiles_array_len(), 
		UiTypes::SimpleTileType::ANY, NULL, (char *)UI_VALUES_DEFAULT, NULL);
	UiTypes::SimpleTile::update_per_type(_view_detailed.get_simple_tiles_array(), _view_detailed.get_simple_tiles_array_len(), 
		UiTypes::SimpleTileType::ANY, NULL, (char *)UI_VALUES_DEFAULT, (char *)UI_VALUES_DEFAULT);
	UiTypes::SimpleTile::update_per_type(_view_tiles.get_simple_tiles_array(), _view_tiles.get_simple_tiles_array_len(), 
		UiTypes::SimpleTileType::WIND, NULL, (char *)UI_VALUES_DEFAULT, NULL);
	UiTypes::SimpleTile::update_per_type(_view_detailed.get_simple_tiles_array(), _view_detailed.get_simple_tiles_array_len(), 
		UiTypes::SimpleTileType::WIND, NULL, (char *)UI_VALUES_DEFAULT, NULL);
}

void View::set_wifi_info(char *ssid, int rssi)
{
	char buff[80];
	char *ip = Wifi::get_ip_str();
	if(strlen(ssid) > 15)
	{
		snprintf(buff, sizeof(buff), "SSID: %.15s..., RSSI: %d, IP: %s", ssid, rssi, ip);
	}
	else
	{
		snprintf(buff, sizeof(buff), "SSID: %s, RSSI: %d, IP: %s", ssid, rssi, ip);
	}
	_view_settings._info_wifi.set_info(buff);
	_view_settings.set_about_title(ip);
}

void View::empty_wifi_info()
{
	_view_settings._info_wifi.set_info("SSID: --, RSSI: --, IP: --");
	_view_settings.set_about_title(nullptr);
}

void View::set_gps_info(float lan, float lon, char * time_fmt)
{
	char buff[60];
	snprintf(buff, sizeof(buff), "Lat: %.4f, Lon: %.4f, TS: %s", lan, lon, time_fmt);
	_view_settings._info_gps.set_info(buff);
}

void View::empty_gps_info()
{
	_view_settings._info_gps.set_info("Lat: --, Lon: --, TS: --");
}

void View::set_act_tab(uint16_t tab_index)
{
	// Manually selecting a tab do not trigger the tab_changed event. So we have to set it in both places.
	_view_statusbar.update_nav_index(tab_index);
	_tabview_main.sel_tab(tab_index);
}

void View::set_fw_ver_info(char *fw_ver)
{
	char buff[40];
	snprintf(buff, sizeof(buff), "v%s", fw_ver);
	_view_settings._info_fw_ver.set_info(buff);
}

void View::set_serial_info(char *serial)
{
	_view_settings._info_sn.set_info(serial);
}

void View::set_device_name_info(const char *device_name)
{
	_view_settings._info_dev_name.set_info((char *)device_name);
}

void View::set_ws_pair_info(bool is_paired, uint8_t* sn, size_t sn_len)
{
	if(sn_len == 0 || sn_len > 10 || sn == nullptr) return;

	if(is_paired)
	{
		char buff[50];
		char sn_hex_chars[21];
		for(int i = 0; i < 10; i++)
		{
			snprintf(&sn_hex_chars[2 * i], sizeof(sn_hex_chars), "%02X", sn[i]);
		}
		const char *source = WsHandler::get_active_pair_source() == PairSource::PAIR_SOURCE_CLOUD
			? "Cloud" : "Local";
		snprintf(buff, sizeof(buff), "%s target S/N: %s", source, sn_hex_chars);
			_view_settings._button_unpair.clickable(false);
		_view_settings._button_unpair.set_title(buff);
	}
	else
	{
		_view_settings._button_unpair.set_title("No station target");
		_view_settings._button_unpair.clickable(false);
	}
}

void View::set_ws_pair_info(bool is_paired, uint32_t id)
{
	if(is_paired)
	{
		char buff[40];
		bool is_cloud =
			WsHandler::get_active_pair_source() == PairSource::PAIR_SOURCE_CLOUD;
		snprintf(buff, sizeof(buff), "%s target ID: %08X",
			is_cloud ? "Cloud" : "Local", id);
			_view_settings._button_unpair.clickable(!is_cloud);
		_view_settings._button_unpair.set_title(buff);
	}
	else
	{
		_view_settings._button_unpair.set_title("No station target");
		_view_settings._button_unpair.clickable(false);
	}
}

void View::enable_ws_info(bool freq_is_selectable)
{
	_view_settings._dd_ws_type.hidden(false);
	_view_settings._dd_ws_freq.hidden(false).enable_top_separator();
	if(freq_is_selectable)
	{
		_view_settings._dd_ws_freq.clickable(true);
	}
	_view_settings._info_ws_status.hidden(false).enable_top_separator();
	_view_settings._button_unpair.hidden(false).enable_top_separator();

	if(_view_settings._lbl_ws_sync_section_title.self_lv_obj() != NULL)
	{
		_view_settings._lbl_ws_sync_section_title.del_self();
	}
	if(_view_settings._icon_ws_sync_section_icon.self_lv_obj() != NULL)
	{
		_view_settings._icon_ws_sync_section_icon.del_self();
	}
	if(_view_settings._lbl_ws_sync_section_msg.self_lv_obj() != NULL)
	{
		_view_settings._lbl_ws_sync_section_msg.del_self();
	}
	if(_view_settings._cont_ws_sync_section.self_lv_obj() != NULL)
	{
		_view_settings._cont_ws_sync_section.del_self();
	}
}

void View::set_ws_status(bool is_battery_low, uint8_t reception_perc)
{
	char buff[50];
	snprintf(buff, sizeof(buff), "Battery Level: %s, Reception: %d%%", is_battery_low ? "Low" : "Good", reception_perc);
	_view_settings._info_ws_status.set_info(buff);
}

void View::set_station_radio_info(const char *frequency, const char *power,
	int16_t rssi, int8_t snr, bool signal_available)
{
	if(signal_available)
	{
		snprintf(_station_radio_info, sizeof(_station_radio_info),
			"%s / %s Power / %d RSSI / %d SNR",
			frequency != NULL ? frequency : "--",
			power != NULL ? power : "--", rssi, snr);
	}
	else
	{
		snprintf(_station_radio_info, sizeof(_station_radio_info), "%s / %s Power",
			frequency != NULL ? frequency : "--", power != NULL ? power : "--");
	}
	_view_settings._dd_ws_freq.set_title("Frequency");
	_view_settings._dd_ws_freq.set_display_text(_station_radio_info);
}

void View::empty_ws_status()
{
	_view_settings._info_ws_status.set_info("Battery Level: --, Reception: --");
	_view_settings._dd_ws_freq.set_title("Frequency");
	_view_settings._dd_ws_freq.clear_display_text();
}

lv_obj_t * View::get_popups_layer()
{
	return _layer_popups;
}

View &View::realign_children()
{
	_tabview_main.realign();
	_view_statusbar.realign();
	_view_tiles.realign();
	_view_detailed.realign();
	_view_crypto.realign();
	_view_settings.realign();
	return *this;
}

} // namespace UiViewSniffer
