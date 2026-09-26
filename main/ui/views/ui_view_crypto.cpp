#include "ui/views/include/ui_view_crypto.h"
#include <esp_log.h>
#include "ui/include/ui_common.h"
#include "ui/resources/ui_resources.h"
#include "settings.h"


namespace UiViewCrypto
{

/******************************************************************************
 * @brief Create component.
 * @param parent Parent LVGL object.
 * @return Component object.
 ******************************************************************************/
View &View::create(lv_obj_t *parent)
{
    _parent_lv_obj = parent;
	_self_lv_obj = lv_obj_create(_parent_lv_obj);
	lv_obj_remove_style_all(_self_lv_obj);
	lv_obj_add_style(_self_lv_obj, UiThemeWxm::get_style_obj_basic(), LV_PART_ANY);

	_cont_weather_summary.create(_self_lv_obj)
		.color_bg(UI_COLOR_PRIMARY_DARK)
		.radius(UI_ITEM_RADIUS)
		.size(166, 438)
		.align(LV_ALIGN_TOP_LEFT, UI_ITEM_GAP, UI_ITEM_GAP);

	_lbl_temp.create(_self_lv_obj)
		.font(&UiResources::ui_font_montserrat_42_bold)
		.color(UI_TEXT_COLOR_LIGHT)
		.text_align(LV_TEXT_ALIGN_CENTER)
		.text(UI_VALUES_DEFAULT)
		.align_to(_cont_weather_summary.self_lv_obj(), LV_ALIGN_TOP_MID, 0, 44);

	_lbl_feels_like_title.create(_self_lv_obj)
		.font(&lv_font_montserrat_18)
		.color(UI_TEXT_COLOR_LIGHT)
		.text_align(LV_TEXT_ALIGN_CENTER)
		.text("Feels Like")
		.align_to(_lbl_temp.self_lv_obj(), LV_ALIGN_OUT_BOTTOM_MID, 0, 8);

	_lbl_feels_like_value.create(_self_lv_obj)
		.font(&UiResources::ui_font_montserrat_34_bold)
		.color(UI_TEXT_COLOR_LIGHT)
		.text_align(LV_TEXT_ALIGN_CENTER)
		.text(UI_VALUES_DEFAULT)
		.align_to(_lbl_feels_like_title.self_lv_obj(), LV_ALIGN_OUT_BOTTOM_MID, 0, 0);
	
	static lv_point_t line_points[] = { {0, 0}, {150, 0} };
	lv_obj_t *line = lv_line_create(_self_lv_obj);
	lv_line_set_points(line, line_points, 2);
	lv_obj_set_style_line_width(line, 1, LV_PART_MAIN);
	lv_obj_set_style_line_color(line, lv_color_hex(0x4E5166), LV_PART_MAIN);
	lv_obj_align_to(line, _lbl_feels_like_value.self_lv_obj(), LV_ALIGN_OUT_BOTTOM_MID, 0, 16);

	_icon_hum.create(_self_lv_obj)
		.source(&UiResources::icon_hum)
		.align_to(line, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 16);
	_lbl_hum_value.create(_self_lv_obj)
		.font(&UiResources::ui_font_montserrat_34_bold)
		.color(UI_TEXT_COLOR_LIGHT)
		.text_align(LV_TEXT_ALIGN_LEFT)
		.text(UI_VALUES_DEFAULT)
		.align_to(_icon_hum.self_lv_obj(), LV_ALIGN_OUT_RIGHT_MID, 5, 0);
	_lbl_hum_units.create(_self_lv_obj)
		.font(&lv_font_montserrat_18)
		.color(UI_TEXT_COLOR_LIGHT)
		.text_align(LV_TEXT_ALIGN_LEFT)
		.text("%")
		.align_to(_lbl_hum_value.self_lv_obj(), LV_ALIGN_OUT_RIGHT_BOTTOM, 0, -3);

	_icon_wind.create(_self_lv_obj)
		.source(&UiResources::icon_wind)
		.align_to(_icon_hum.self_lv_obj(), LV_ALIGN_OUT_BOTTOM_MID, 0, 26);
	_lbl_wind_value.create(_self_lv_obj)
		.font(&UiResources::ui_font_montserrat_34_bold)
		.color(UI_TEXT_COLOR_LIGHT)
		.text_align(LV_TEXT_ALIGN_LEFT)
		.text(UI_VALUES_DEFAULT)
		.align_to(_icon_wind.self_lv_obj(), LV_ALIGN_OUT_RIGHT_MID, 5, 0);
	_lbl_wind_units.create(_self_lv_obj)
		.font(&lv_font_montserrat_18)
		.color(UI_TEXT_COLOR_LIGHT)
		.text_align(LV_TEXT_ALIGN_LEFT)
		.text(UI_VALUES_DEFAULT)
		.align_to(_lbl_wind_value.self_lv_obj(), LV_ALIGN_OUT_RIGHT_BOTTOM, 0, -3);

	_icon_precip.create(_self_lv_obj)
		.source(&UiResources::icon_precip)
		.align_to(_icon_wind.self_lv_obj(), LV_ALIGN_OUT_BOTTOM_MID, 0, 26);
	_lbl_precip_value.create(_self_lv_obj)
		.font(&UiResources::ui_font_montserrat_34_bold)
		.color(UI_TEXT_COLOR_LIGHT)
		.text_align(LV_TEXT_ALIGN_LEFT)
		.text(UI_VALUES_DEFAULT)
		.align_to(_icon_precip.self_lv_obj(), LV_ALIGN_OUT_RIGHT_MID, 5, 0);
	_lbl_precip_units.create(_self_lv_obj)
		.font(&lv_font_montserrat_18)
		.color(UI_TEXT_COLOR_LIGHT)
		.text_align(LV_TEXT_ALIGN_LEFT)
		.text(UI_VALUES_DEFAULT)
		.align_to(_lbl_precip_value.self_lv_obj(), LV_ALIGN_OUT_RIGHT_BOTTOM, 0, -3);

	_icon_dew.create(_self_lv_obj)
		.source(&UiResources::icon_dew)
		.align_to(_icon_precip.self_lv_obj(), LV_ALIGN_OUT_BOTTOM_MID, 0, 26);
	_lbl_dew_value.create(_self_lv_obj)
		.font(&UiResources::ui_font_montserrat_34_bold)
		.color(UI_TEXT_COLOR_LIGHT)
		.text_align(LV_TEXT_ALIGN_LEFT)
		.text(UI_VALUES_DEFAULT)
		.align_to(_icon_dew.self_lv_obj(), LV_ALIGN_OUT_RIGHT_MID, 5, 0);
	_lbl_dew_units.create(_self_lv_obj)
		.font(&lv_font_montserrat_18)
		.color(UI_TEXT_COLOR_LIGHT)
		.text_align(LV_TEXT_ALIGN_LEFT)
		.text(UI_VALUES_DEFAULT)
		.align_to(_lbl_dew_value.self_lv_obj(), LV_ALIGN_OUT_RIGHT_BOTTOM, 0, -3);

	_cont_rewards_summary.create(_self_lv_obj)
		.color_bg(UI_COLOR_PRIMARY_DARK)
		.radius(UI_ITEM_RADIUS)
		.size(296, 438)
		.align(LV_ALIGN_TOP_RIGHT, -UI_ITEM_GAP, UI_ITEM_GAP);
	
	_cont_qod.create(_cont_rewards_summary.self_lv_obj())
		.color_bg(UI_QOD_TILE_COLOR_NA)
		.radius(UI_ITEM_RADIUS)
		.size(136, 134)
		.align(LV_ALIGN_TOP_LEFT, 8, 8);
	_lbl_qod_title.create(_self_lv_obj)
		.font(&lv_font_montserrat_18)
		.color(UI_TEXT_COLOR_LIGHT)
		.text_align(LV_TEXT_ALIGN_CENTER)
		.text(UI_QOD_TILE_TITLE)
		.align_to(_cont_qod.self_lv_obj(), LV_ALIGN_TOP_MID, 0, 12);
	_lbl_qod_val.create(_self_lv_obj)
		.font(&UiResources::ui_font_montserrat_34_bold)
		.color(UI_TEXT_COLOR_LIGHT)
		.text_align(LV_TEXT_ALIGN_CENTER)
		.text(UI_VALUES_DEFAULT)
		.align_to(_lbl_qod_title.self_lv_obj(), LV_ALIGN_OUT_BOTTOM_MID, 0, 18);
	_lbl_qod_units.create(_self_lv_obj)
		.font(&lv_font_montserrat_18)
		.color(UI_TEXT_COLOR_LIGHT)
		.text_align(LV_TEXT_ALIGN_CENTER)
		.text("")
		.align_to(_lbl_qod_val.self_lv_obj(), LV_ALIGN_OUT_BOTTOM_MID, 0, 6)
		.hidden(true);
	
	_cont_pol.create(_cont_rewards_summary.self_lv_obj())
		.color_bg(UI_POL_TILE_COLOR_NA)
		.radius(UI_ITEM_RADIUS)
		.size(136, 134)
		.align(LV_ALIGN_TOP_RIGHT, -8, 8);
	_lbl_pol_title.create(_self_lv_obj)
		.font(&lv_font_montserrat_18)
		.color(UI_TEXT_COLOR_LIGHT)
		.text_align(LV_TEXT_ALIGN_CENTER)
		.text(UI_POL_TILE_TITLE_TEXT_NO_DATA)
		.align_to(_cont_pol.self_lv_obj(), LV_ALIGN_TOP_MID, 0, 12);
	_lbl_pol_address.create(_self_lv_obj)
		.font(&lv_font_montserrat_14)
		.color(UI_TEXT_COLOR_LIGHT)
		.text_align(LV_TEXT_ALIGN_CENTER)
		.text("N/A")
		.align_to(_cont_pol.self_lv_obj(), LV_ALIGN_CENTER, 0, 18);
	lv_label_set_long_mode(_lbl_pol_address.self_lv_obj(), LV_LABEL_LONG_WRAP);
	lv_obj_set_width(_lbl_pol_address.self_lv_obj(), 115);

	_cont_last_reward.create(_cont_rewards_summary.self_lv_obj())
		.color_bg(UI_REW_LAST_TILE_COLOR)
		.radius(UI_ITEM_RADIUS)
		.size(280, 88)
		.align(LV_ALIGN_TOP_MID, 0, 148);
	_lbl_last_reward_title.create(_self_lv_obj)
		.font(&lv_font_montserrat_18)
		.color(UI_TEXT_COLOR_LIGHT)
		.text_align(LV_TEXT_ALIGN_CENTER)
		.text(UI_REW_LAST_TILE_TITLE_TEXT)
		.align_to(_cont_last_reward.self_lv_obj(), LV_ALIGN_TOP_MID, 0, 24);
	_lbl_last_reward_val.create(_self_lv_obj)
		.font(&UiResources::ui_font_montserrat_34_bold)
		.color(UI_TEXT_COLOR_LIGHT)
		.text_align(LV_TEXT_ALIGN_CENTER)
		.text(UI_VALUES_DEFAULT)
		.align_to(_lbl_last_reward_title.self_lv_obj(), LV_ALIGN_OUT_BOTTOM_MID, 0, 4);
	_icon_rocket.create(_self_lv_obj)
		.source(&UiResources::icon_rocket)
		.align_to(_lbl_last_reward_title.self_lv_obj(), LV_ALIGN_OUT_BOTTOM_MID, 0, 0)
		.hidden(true);
	_lbl_last_reward_units.create(_self_lv_obj)
		.font(&lv_font_montserrat_18)
		.color(UI_REW_TOTAL_TILE_TOKEN_TEXT_COLOR)
		.text_align(LV_TEXT_ALIGN_LEFT)
		.text(UI_REW_TOTAL_TILE_TOKEN_TEXT)
		.align_to(_lbl_last_reward_val.self_lv_obj(), LV_ALIGN_OUT_RIGHT_BOTTOM, 10, -3);
	_lbl_last_reward_val.align_to(_lbl_last_reward_title.self_lv_obj(), 
		LV_ALIGN_OUT_BOTTOM_MID, -lv_obj_get_width(_lbl_last_reward_units.self_lv_obj())/2 - 5, 4);
	_lbl_last_reward_units.realign();

	_cont_total_rewards.create(_cont_rewards_summary.self_lv_obj())
		.color_bg(UI_REW_LAST_TILE_COLOR)
		.radius(UI_ITEM_RADIUS)
		.size(280, 88)
		.align(LV_ALIGN_TOP_MID, 0, 244);
	_lbl_total_rewards_title.create(_self_lv_obj)
		.font(&lv_font_montserrat_18)
		.color(UI_TEXT_COLOR_LIGHT)
		.text_align(LV_TEXT_ALIGN_CENTER)
		.text(UI_REW_TOTAL_TILE_TITLE_TEXT)
		.align_to(_cont_total_rewards.self_lv_obj(), LV_ALIGN_TOP_MID, 0, 24);
	_lbl_total_rewards_val.create(_self_lv_obj)
		.font(&UiResources::ui_font_montserrat_24_bold)
		.color(UI_TEXT_COLOR_LIGHT)
		.text_align(LV_TEXT_ALIGN_CENTER)
		.text(UI_VALUES_DEFAULT)
		.align_to(_lbl_total_rewards_title.self_lv_obj(), LV_ALIGN_OUT_BOTTOM_MID, 0, 4);
	_lbl_total_rewards_units.create(_self_lv_obj)
		.font(&lv_font_montserrat_18)
		.color(UI_REW_TOTAL_TILE_TOKEN_TEXT_COLOR)
		.text_align(LV_TEXT_ALIGN_LEFT)
		.text(UI_REW_TOTAL_TILE_TOKEN_TEXT)
		.align_to(_lbl_total_rewards_val.self_lv_obj(), LV_ALIGN_OUT_RIGHT_BOTTOM, 10, -1);
	_lbl_total_rewards_val.align_to(_lbl_total_rewards_title.self_lv_obj(), 
		LV_ALIGN_OUT_BOTTOM_MID, -lv_obj_get_width(_lbl_total_rewards_units.self_lv_obj())/2 - 5, 4);
	_lbl_total_rewards_units.realign();

	_cont_device_name.create(_cont_rewards_summary.self_lv_obj())
		.color_bg(UI_REW_LAST_TILE_COLOR)
		.radius(UI_ITEM_RADIUS)
		.size(280, 88)
		.align(LV_ALIGN_TOP_MID, 0, 340);
	_lbl_device_name_title.create(_self_lv_obj)
		.font(&lv_font_montserrat_18)
		.color(UI_TEXT_COLOR_LIGHT)
		.text_align(LV_TEXT_ALIGN_CENTER)
		.text("Device Name")
		.align_to(_cont_device_name.self_lv_obj(), LV_ALIGN_TOP_MID, 0, 24);

	_lbl_device_name_val.create(_self_lv_obj)
		.color(UI_TEXT_COLOR_LIGHT)
		.text_align(LV_TEXT_ALIGN_CENTER);

	update_device_name(Settings::get_device_name());
	
	return *this;
}

/******************************************************************************
 * @brief Get timestamp of last rewards update
 * @return Timestamp
 ******************************************************************************/
uint32_t View::get_last_rewards_update_ts()
{
	return _last_rewards_update_ts;
}

/******************************************************************************
 * @brief Set timestamp of last rewards update
 * @param ts Timestamp
 ******************************************************************************/
void View::set_last_rewards_update_ts(uint32_t ts)
{
	_last_rewards_update_ts = ts;
}

/******************************************************************************
 * @brief Get timestamp of last device info update
 * @return Timestamp
 ******************************************************************************/
uint32_t View::get_last_dev_info_update_ts()
{
	return _last_dev_info_update_ts;
}

/******************************************************************************
 * @brief Set timestamp of last device info update
 * @param ts Timestamp
 ******************************************************************************/
void View::set_last_dev_info_update_ts(uint32_t ts)
{
	_last_dev_info_update_ts = ts;
}

/******************************************************************************
 * @brief Realign children components.
 * @return Component object.
 ******************************************************************************/
View &View::realign_children()
{
	_cont_weather_summary.realign();	
	_lbl_temp.realign();
	_lbl_feels_like_title.realign();
	_lbl_feels_like_value.realign();
	_icon_hum.realign();
	_lbl_hum_value.realign();
	_lbl_hum_units.realign();
	_icon_wind.realign();
	_lbl_wind_value.realign();
	_lbl_wind_units.realign();
	_icon_precip.realign();
	_lbl_precip_value.realign();
	_lbl_precip_units.realign();
	_icon_dew.realign();
	_lbl_dew_value.realign();
	_lbl_dew_units.realign();

	_cont_rewards_summary.realign();
	_cont_qod.realign();
	_lbl_qod_title.realign();
	_lbl_qod_val.realign();
	_lbl_qod_units.realign();
	_cont_pol.realign();
	_lbl_pol_title.realign();
	_lbl_pol_address.realign();
	_cont_last_reward.realign();
	_lbl_last_reward_title.realign();
	_lbl_last_reward_val.realign();
	_lbl_last_reward_units.realign();
	_icon_rocket.realign();
	_cont_total_rewards.realign();
	_lbl_total_rewards_title.realign();
	_lbl_total_rewards_val.realign();
	_lbl_total_rewards_units.realign();
	_cont_device_name.realign();
	_lbl_device_name_title.realign();
	_lbl_device_name_val.realign();
	return *this;
}

/******************************************************************************
 * @brief Set pending state
 * @param state Enable/Disable
 ******************************************************************************/
void View::set_pending_state(bool state)
{
	enum class State
	{
		NO_DATA,
		PENDING,
		OK
	};
	static State saved_state = State::NO_DATA;

	if(state)
	{
		if(saved_state == State::OK || saved_state == State::PENDING)
		{
			return;
		}
		_lbl_qod_val.hidden(true);
		_lbl_qod_units.hidden(true);
		_lbl_qod_title.text(UI_QOD_TILE_TEXT_PENDING).align_to(_cont_qod.self_lv_obj(), LV_ALIGN_CENTER, 0, 0);
		_cont_qod.color_bg(UI_QOD_TILE_COLOR_NA);

		_lbl_pol_title.text(UI_POL_TILE_TITLE_TEXT_PENDING);
		_cont_pol.color_bg(UI_QOD_TILE_COLOR_NA);

		_lbl_last_reward_val.hidden(true);
		_lbl_last_reward_units.hidden(true);
		_lbl_last_reward_title.text(UI_REW_LAST_TILE_TEXT_PENDING);
		_icon_rocket.hidden(false);

		saved_state = State::PENDING;
	}
	else
	{
		if(saved_state == State::OK)
		{
			return;
		}

		_lbl_qod_val.hidden(false);
		_lbl_qod_units.hidden(true);
		_lbl_qod_title.text(UI_QOD_TILE_TITLE).align_to(_cont_qod.self_lv_obj(), LV_ALIGN_TOP_MID, 0, 12);

		_lbl_pol_title.text(UI_POL_TILE_TITLE_TEXT_NO_DATA);

		_lbl_last_reward_val.hidden(false);
		_lbl_last_reward_units.hidden(false);
		_lbl_last_reward_title.text(UI_REW_LAST_TILE_TITLE_TEXT);
		_icon_rocket.hidden(true);

		saved_state = State::OK;
	}
}

/******************************************************************************
 * @brief Update temperature value.
 * @param temp Temperature float.
 * @param units Units in text.
 ******************************************************************************/
void View::update_temp(float temp, char *units)
{
	char buff[10];
	snprintf(buff, sizeof(buff), "%.1f°%s", temp, units);
	_lbl_temp.text(buff);
}

/******************************************************************************
 * @brief Update "feels like" temperature value.
 * @param feels_like "Feels like" temperature float.
 * @param units Units in text.
 ******************************************************************************/
void View::update_feels_like(float feels_like, char *units)
{
	char buff[10];
	snprintf(buff, sizeof(buff), "%.0f°%s", feels_like, units);
	_lbl_feels_like_value.text(buff);
}

/******************************************************************************
 * @brief Update humidity value.
 * @param hum Humidity float value.
 * @return Component object.
 ******************************************************************************/
void View::update_hum(float hum)
{
	char buff[10];
	snprintf(buff, sizeof(buff), "%.0f", hum);
	_lbl_hum_value.text(buff);
	_lbl_hum_units.realign();
}

/******************************************************************************
 * @brief Update wind speed value.
 * @param wind_spd Wind speed float value.
 ******************************************************************************/
void View::update_wind_spd(float wind_spd)
{
	if(wind_spd > 99.0f)
	{
		wind_spd = 99.0f;
	}
	char buff[10];
	snprintf(buff, sizeof(buff), "%.0f", wind_spd);
	_lbl_wind_value.text(buff);
	_lbl_wind_units.realign();
}
/******************************************************************************
 * @brief Update wind speed units.
 * @param units Units of wind speed.
 ******************************************************************************/
void View::update_wind_units(char *units)
{
	_lbl_wind_units.text(units);
}

/******************************************************************************
 * @brief Update precipitation daily accumulation and rate.
 * @param daily Daily cumulative precipitation.
 * @param rate Precipitation rate.
 ******************************************************************************/
void View::update_precip(float daily, float rate)
{
	char buff[10];
	if(daily == 0.0f)
	{
		snprintf(buff, sizeof(buff), "0.0");
	}
	else if(daily > 99.9f)
	{
		snprintf(buff, sizeof(buff), "%.0f", daily);
	}
	else
	{
		snprintf(buff, sizeof(buff), "%.1f", daily);
	}
	_lbl_precip_value.text(buff);
	_lbl_precip_units.realign();
}

/******************************************************************************
 * @brief Update precipitation rate value.
 * @param rate Precipitation rate.
 ******************************************************************************/
void View::update_precip_rate(float rate)
{
	update_precip(rate, rate);
}

/******************************************************************************
 * @brief Update precipitation units.
 * @param units Units of precipitation.
 ******************************************************************************/
void View::update_precip_units(char *units)
{
	char buff[10];
	snprintf(buff, sizeof(buff), "%s", units);
	_lbl_precip_units.text(buff);
}

/******************************************************************************
 * @brief Update dew point.
 * @param dew Dew point.
 ******************************************************************************/
void View::update_dew(float dew)
{
	char buff[10];
	snprintf(buff, sizeof(buff), "%.1f", dew);
	_lbl_dew_value.text(buff);
	_lbl_dew_units.realign();
}

/******************************************************************************
 * @brief Update precipitation units.
 * @param units Units of precipitation.
 * @return Component object.
 ******************************************************************************/
void View::update_dew_units(char *units)
{
	char buff[10];
	snprintf(buff, sizeof(buff), "°%s", units);
	_lbl_dew_units.text(buff);
}

/******************************************************************************
 * @brief Update connection status.
 * @param status Connection status text.
 * @param color Color value for the connection status icon.
 * @return Component object.
 ******************************************************************************/
void View::update_con_status(char *status , lv_color_t *color)
{

}

/******************************************************************************
 * @brief Update quality of data (QOD) display.
 * @param qod_title Title of the QOD display.
 * @param qod The QOD value to be displayed.
 * @param color_hex Background color in hexadecimal format (e.g., 0x24263A).
 * @param unit Unit of the QOD value.
 * @return Component object.
 ******************************************************************************/
void View::update_qod(uint8_t qod_score)
{	
	_cont_qod.color_bg(UI_QOD_TILE_COLOR_OK);
	if(qod_score < UI_QOD_TILE_COLOR_ERR_LIMIT)
	{
		_cont_qod.color_bg(UI_QOD_TILE_COLOR_ERR);
	}
	else if(qod_score < UI_QOD_TILE_COLOR_WRN_LIMIT)
	{
		_cont_qod.color_bg(UI_QOD_TILE_COLOR_WRN);
	}
	char buff[6];
	snprintf(buff, sizeof(buff), "%d", qod_score);
	_lbl_qod_val.text(buff);
	_lbl_qod_units.text("");
}

/******************************************************************************
 * @brief Update proof of location (POL) display.
 * @param pol_title Title of the POL display.
 * @param address The POL value to be displayed.
 * @param color_hex Background color in hexadecimal format (e.g., 0x24263A).
 * @return Component object.
 ******************************************************************************/
void View::update_pol(const char *pol_title, char *address)
{	
	_cont_pol.color_bg(UI_POL_TILE_COLOR_OK);
	if(strcmp(pol_title, UI_POL_TILE_TITLE_TEXT_NOT_VER) == 0)
	{
		_cont_pol.color_bg(UI_POL_TILE_COLOR_WRN);
	}
	else if(strcmp(pol_title, UI_POL_TILE_TITLE_TEXT_NO_LOC) == 0)
	{
		_cont_pol.color_bg(UI_POL_TILE_COLOR_ERR);
	}

	size_t address_length = strlen(address);

	if(address_length > UI_POL_TILE_TITLE_TEXT_MAX_ADDR_CHARS
		&& address_length > 3)
	{
		address[UI_POL_TILE_TITLE_TEXT_MAX_ADDR_CHARS] = '\0';
		address[UI_POL_TILE_TITLE_TEXT_MAX_ADDR_CHARS - 1] = '.';
		address[UI_POL_TILE_TITLE_TEXT_MAX_ADDR_CHARS - 2] = '.';
		address[UI_POL_TILE_TITLE_TEXT_MAX_ADDR_CHARS - 3] = '.';
	}
	_lbl_pol_address.text(address);
	_lbl_pol_title.text(pol_title);
}

/******************************************************************************
 * @brief Update reward display.
 * @param total Total rewards value.
 * @param last Last reward value.
 * @param last_title Title for the last reward.
 * @param unit Unit of the reward
 * @return Component object.
 ******************************************************************************/
void View::update_rewards(float total, float last)
{	
	char buff[12];	

	snprintf(buff, sizeof(buff), "%.2f", total);
	_lbl_total_rewards_val.text(buff);
	_lbl_total_rewards_val.align_to(_lbl_total_rewards_title.self_lv_obj(), 
		LV_ALIGN_OUT_BOTTOM_MID, -lv_obj_get_width(_lbl_total_rewards_units.self_lv_obj())/2 - 5, 4);
	_lbl_total_rewards_units.realign();

	snprintf(buff, sizeof(buff), "+%.2f", last);
	_lbl_last_reward_val.text(buff);
	_lbl_last_reward_val.align_to(_lbl_last_reward_title.self_lv_obj(), 
		LV_ALIGN_OUT_BOTTOM_MID, -lv_obj_get_width(_lbl_last_reward_units.self_lv_obj())/2 - 5, 4);
	_lbl_last_reward_units.realign();
}

void View::update_device_name(const char *name)
{
	if (name != NULL && strlen(name) > 0)
	{
		size_t len = strlen(name);
		if (len <= 16)
		{
			_lbl_device_name_val.font(&UiResources::ui_font_montserrat_34_bold);
		}
		else if (len <= 20)
		{
			_lbl_device_name_val.font(&UiResources::ui_font_montserrat_24_bold);
		}
		else if (len <= 26)
		{
			_lbl_device_name_val.font(&UiResources::ui_font_montserrat_20_bold);
		}
		else
		{
			_lbl_device_name_val.font(&lv_font_montserrat_16);
		}
		_lbl_device_name_val.text((char *)name);
	}
	else
	{
		_lbl_device_name_val.font(&UiResources::ui_font_montserrat_34_bold);
		_lbl_device_name_val.text("--");
	}
	_lbl_device_name_val.align_to(_lbl_device_name_title.self_lv_obj(), LV_ALIGN_OUT_BOTTOM_MID, 0, 4);
}

/******************************************************************************
 * @brief Update last update display.
 * @param date The date of the last reward update.
 * @return Component object.
 ******************************************************************************/
void View::set_last_update(char *date)
{
	char buff[40];
	if (date != NULL && strlen(date) > 0)
	{
		snprintf(buff, sizeof(buff), "%s (%s)", UI_REW_TOTAL_TILE_TITLE_TEXT, date);
	}
	else
	{
		snprintf(buff, sizeof(buff), "%s", UI_REW_TOTAL_TILE_TITLE_TEXT);
	}
	_lbl_total_rewards_title.text(buff);
	_lbl_total_rewards_title.realign();
}

/******************************************************************************
 * @brief Empty the overview screen content display.
 * @return Component object.
 ******************************************************************************/
void View::empty_weather_content()
{
	_lbl_temp.text(UI_VALUES_DEFAULT);
	_lbl_feels_like_value.text(UI_VALUES_DEFAULT);
	_lbl_hum_value.text(UI_VALUES_DEFAULT);
	_lbl_wind_value.text(UI_VALUES_DEFAULT);
	_lbl_precip_value.text(UI_VALUES_DEFAULT);
	_lbl_dew_value.text(UI_VALUES_DEFAULT);
	this->realign();
}

} // namespace UiViewCrypto
