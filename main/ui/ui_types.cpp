#include "ui/include/ui_types.h"
#include "ui/resources/ui_resources.h"

namespace UiTypes
{

LabelComponent& LabelComponent::create(lv_obj_t *parent)
{
	_parent_lv_obj = parent;
	_self_lv_obj = lv_label_create(_parent_lv_obj);
	return *this;
}

LabelComponent& LabelComponent::color(lv_color_t color)
{    
	lv_obj_set_style_text_color(_self_lv_obj, color, 0);
	lv_obj_set_style_text_opa(_self_lv_obj, LV_OPA_100, 0);
	return *this;
}

LabelComponent& LabelComponent::max_chars(uint8_t max_chars)
{
	_max_chars = max_chars;
	return *this;
}

LabelComponent& LabelComponent::text(const char *value)
{
	// TODO: max char implementation
	// if(strlen(value) <= _max_chars)
	// {
	// 	lv_label_set_text(_self_lv_obj, UI_VALUES_MAX_ERROR);
	// }
	// else
	// {
	// 	lv_label_set_text(_self_lv_obj, value);
	// }    
	lv_label_set_text(_self_lv_obj, value);
	realign();
	return *this;
}

LabelComponent& LabelComponent::font(const lv_font_t *value)
{
	lv_obj_set_style_text_font(_self_lv_obj, value, 0);
	return *this;
}

LabelComponent& LabelComponent::text_align(lv_text_align_t align)
{
	lv_obj_set_style_text_align(_self_lv_obj, align, LV_PART_MAIN);
	return *this;
}

IconComponent& IconComponent::create(lv_obj_t *parent)
{
	_parent_lv_obj = parent;
	_self_lv_obj = lv_img_create(_parent_lv_obj);
	return *this;
}

IconComponent& IconComponent::source(const void *src_img)
{
	lv_img_set_src(_self_lv_obj, src_img);
	return *this;
}

IconComponent& IconComponent::color(lv_color_t color)
{
	lv_obj_set_style_img_recolor(_self_lv_obj, color, 0);
	lv_obj_set_style_img_recolor_opa(_self_lv_obj, LV_OPA_COVER, 0);
	return *this;
}

StatusIconComponent& StatusIconComponent::create(lv_obj_t *parent)
{
	_parent_lv_obj = parent;
	_self_lv_obj = lv_img_create(_parent_lv_obj);
	return *this;
}

StatusIconComponent& StatusIconComponent::status_config(StatusIconComponent::StatusType status, const void *src_img, lv_color_t color)
{
	switch(status)
	{
		case StatusIconComponent::StatusType::NONE:
			_config_default.src = src_img;
			_config_default.color = color;
			break;
		case StatusIconComponent::StatusType::POS:
			_config_pos.src = src_img;
			_config_pos.color = color;
			break;
		case StatusIconComponent::StatusType::WARN:
			_config_warn.src = src_img;
			_config_warn.color = color;
			break;
		case StatusIconComponent::StatusType::NEG:
			_config_neg.src = src_img;
			_config_neg.color = color;
			break;
		default:
			_config_default.src = src_img;
			_config_default.color = color;
			break;
	}
	return *this;
}

StatusIconComponent& StatusIconComponent::status(StatusIconComponent::StatusType status)
{
	lv_obj_set_style_img_recolor_opa(_self_lv_obj, LV_OPA_COVER, 0);
	switch(status)
	{
		case StatusIconComponent::StatusType::NONE:
			if(NULL == _config_default.src)
			{
				break;
			}
			lv_img_set_src(_self_lv_obj, _config_default.src);
			lv_obj_set_style_img_recolor(_self_lv_obj, _config_default.color, 0);
			break;
		case StatusIconComponent::StatusType::POS:
			if(NULL == _config_pos.src)
			{
				break;
			}
			lv_img_set_src(_self_lv_obj, _config_pos.src);
			lv_obj_set_style_img_recolor(_self_lv_obj, _config_pos.color, 0);
			break;
		case StatusIconComponent::StatusType::WARN:
			if(NULL == _config_warn.src)
			{
				break;
			}
			lv_img_set_src(_self_lv_obj, _config_warn.src);
			lv_obj_set_style_img_recolor(_self_lv_obj, _config_warn.color, 0);
			break;
		case StatusIconComponent::StatusType::NEG:
			if(NULL == _config_neg.src)
			{
				break;
			}
			lv_img_set_src(_self_lv_obj, _config_neg.src);
			lv_obj_set_style_img_recolor(_self_lv_obj,  _config_neg.color, 0);
			break;
		default:
			if(NULL == _config_default.src)
			{
				break;
			}
			lv_img_set_src(_self_lv_obj, _config_default.src);
			lv_obj_set_style_img_recolor(_self_lv_obj,  _config_default.color, 0);
			break;
	}
	return *this;
}

TabViewComponent& TabViewComponent::create(lv_obj_t *parent)
{
	_parent_lv_obj = parent;
	_self_lv_obj = lv_tabview_create(_parent_lv_obj, LV_DIR_TOP, 0);
	// lv_tabview_set_btns_pos(_self_lv_obj, LV_TABVIEW_TAB_POS_NONE);
	// lv_btnmatrix_set_btn_ctrl_all(lv_tabview_get_tab_btns(_self_lv_obj), LV_BTNMATRIX_CTRL_HIDDEN);
	// lv_obj_set_style_local_pad_left(_self_lv_obj, LV_PARt, LV_STATE_DEFAULT, 0);
	// lv_obj_set_style_local_pad_right(_self_lv_obj, LV_TABVIEW_PART_BG_SCROLLABLE, LV_STATE_DEFAULT, 0);
	return *this;
}

TabViewComponent& TabViewComponent::add_tab(lv_obj_t **tab, const char *name)
{
	*tab = lv_tabview_add_tab(_self_lv_obj, name);
	return *this;
}

TabViewComponent& TabViewComponent::sel_tab(int tab_index)
{
	lv_tabview_set_act(_self_lv_obj, tab_index, LV_ANIM_OFF);
	return *this;
}

int TabViewComponent::get_active_tab_index()
{
	return lv_tabview_get_tab_act(_self_lv_obj);
}

ContainerComponent& ContainerComponent::create(lv_obj_t *parent)
{
	_parent_lv_obj = parent;
	_self_lv_obj = lv_obj_create(_parent_lv_obj);
	lv_obj_remove_style_all(_self_lv_obj);
	return *this;
}

SimpleTile& SimpleTile::create(lv_obj_t *parent)
{
	_parent_lv_obj = parent;
	_self_lv_obj = lv_obj_create(_parent_lv_obj);
	lv_obj_remove_style_all(_self_lv_obj);
	radius(UI_SIMPLE_TILE_RADIUS);
	_lbl_title.create(_self_lv_obj).style(UiThemeWxm::get_style_heading_4()).align(LV_ALIGN_TOP_MID, 0, 25);
	_lbl_value.create(_self_lv_obj).style(UiThemeWxm::get_style_heading_1()).align(LV_ALIGN_CENTER, 0, 0);
	_lbl_units.create(_self_lv_obj).style(UiThemeWxm::get_style_heading_4()).align(LV_ALIGN_BOTTOM_MID, 0, -25);
	return *this;
}

SimpleTile& SimpleTile::set_type(SimpleTileType tile_type)
{
	// TODO: delete title/value/units for dynamic change
	_tile_type = tile_type;
	switch(_tile_type)
	{
		case SimpleTileType::NONE:
			break;
		case SimpleTileType::DEW_POINT:
			color_bg(UI_SIMPLE_TILE_DEW_COLOR);
			_lbl_title.color(UI_SIMPLE_TILE_DEW_TEXT_COLOR).text(UI_SIMPLE_TILE_DEW_TITLE);
			_lbl_value.color(UI_SIMPLE_TILE_DEW_TEXT_COLOR).text(UI_VALUES_DEFAULT);
			_lbl_units.color(UI_SIMPLE_TILE_DEW_TEXT_COLOR).text(UI_VALUES_DEFAULT);
			break;        
		case SimpleTileType::IRRADIANCE:            
			color_bg(UI_SIMPLE_TILE_IRR_COLOR);
			_lbl_title.color(UI_SIMPLE_TILE_IRR_TEXT_COLOR).text(UI_SIMPLE_TILE_IRR_TITLE);
			_lbl_value.color(UI_SIMPLE_TILE_IRR_TEXT_COLOR).text(UI_VALUES_DEFAULT);
			_lbl_units.color(UI_SIMPLE_TILE_IRR_TEXT_COLOR).text("w/m2");
			break;
		case SimpleTileType::HUMIDITY:
			color_bg(UI_SIMPLE_TILE_HUM_COLOR);
			_lbl_title.color(UI_SIMPLE_TILE_HUM_TEXT_COLOR).text(UI_SIMPLE_TILE_HUM_TITLE);
			_lbl_value.color(UI_SIMPLE_TILE_HUM_TEXT_COLOR).text(UI_VALUES_DEFAULT);
			_lbl_units.color(UI_SIMPLE_TILE_HUM_TEXT_COLOR).text("%");
			break;
		case SimpleTileType::PRECIP_RATE:
			color_bg(UI_SIMPLE_TILE_PRECIP_COLOR);
			_lbl_title.color(UI_SIMPLE_TILE_PRECIP_TEXT_COLOR).text(UI_SIMPLE_TILE_PRECIP_RATE_TITLE);
			_lbl_value.color(UI_SIMPLE_TILE_PRECIP_TEXT_COLOR).text(UI_VALUES_DEFAULT);
			_lbl_units.color(UI_SIMPLE_TILE_PRECIP_TEXT_COLOR).text("mm/h");
			break;
		case SimpleTileType::PRECIP_DAILY:
			color_bg(UI_SIMPLE_TILE_PRECIP_COLOR);
			_lbl_title.color(UI_SIMPLE_TILE_PRECIP_TEXT_COLOR).text(UI_SIMPLE_TILE_PRECIP_DAILY_TITLE);
			_lbl_value.color(UI_SIMPLE_TILE_PRECIP_TEXT_COLOR).text(UI_VALUES_DEFAULT);
			_lbl_units.color(UI_SIMPLE_TILE_PRECIP_TEXT_COLOR).text("mm");
			break;
		case SimpleTileType::PRESSURE:
			color_bg(UI_SIMPLE_TILE_PRESSURE_COLOR);
			_lbl_title.color(UI_SIMPLE_TILE_PRESSURE_TEXT_COLOR).text(UI_SIMPLE_TILE_PRESSURE_TITLE);
			_lbl_value.color(UI_SIMPLE_TILE_PRESSURE_TEXT_COLOR).text(UI_VALUES_DEFAULT);
			_lbl_units.color(UI_SIMPLE_TILE_PRESSURE_TEXT_COLOR).text(UI_VALUES_DEFAULT);
			break;
		case SimpleTileType::UV_INDEX:
			color_bg(UI_SIMPLE_TILE_UV_COLOR);
			_lbl_title.color(UI_SIMPLE_TILE_UV_TEXT_COLOR).text(UI_SIMPLE_TILE_UV_TITLE);
			_lbl_value.color(UI_SIMPLE_TILE_UV_TEXT_COLOR).text(UI_VALUES_DEFAULT);
			_lbl_units.color(UI_SIMPLE_TILE_UV_TEXT_COLOR).text("index");
			break;
		case SimpleTileType::WIND:
			color_bg(UI_SIMPLE_TILE_WIND_COLOR);
			_lbl_title.color(UI_SIMPLE_TILE_WIND_TEXT_COLOR)
				.text(UI_SIMPLE_TILE_WIND_TITLE);
			_lbl_title_value.create(_self_lv_obj)
				.style(UiThemeWxm::get_style_heading_2())
				.color(UI_SIMPLE_TILE_WIND_TEXT_COLOR)
				.text(UI_VALUES_DEFAULT);
			_lbl_value.color(UI_SIMPLE_TILE_WIND_TEXT_COLOR)
				.text(UI_VALUES_DEFAULT);
			_lbl_units.color(UI_SIMPLE_TILE_WIND_TEXT_COLOR)
				.text(UI_VALUES_DEFAULT);
			realign_middle_row();
			break;
		default:
			break;
	}
	return *this;
}

SimpleTileType SimpleTile::get_type()
{
	return _tile_type;
}

SimpleTile& SimpleTile::title(const char *title)
{
	_lbl_title.text(title);
	return *this;
}

SimpleTile& SimpleTile::value(char *value)
{
	if(strlen(value) > 6)
	{
		return *this;
	}
	_lbl_value.text(value);
	if (_tile_type == SimpleTileType::WIND)
	{
		realign_middle_row();
	}
	return *this;
}

SimpleTile& SimpleTile::title_value(char *value)
{
	_lbl_title_value.text(value);
	if (_tile_type == SimpleTileType::WIND)
	{
		realign_middle_row();
		_lbl_title.align(LV_ALIGN_TOP_MID, 0, 25);
		_lbl_title.realign();
	}
	else
	{
		_lbl_title_value.realign();
		int title_and_value_width = lv_obj_get_width(_lbl_title.self_lv_obj()) + 3 + lv_obj_get_width(_lbl_title_value.self_lv_obj());
		_lbl_title.align(LV_ALIGN_TOP_MID, -(title_and_value_width/2 - lv_obj_get_width(_lbl_title.self_lv_obj())/2), 25);
		_lbl_title_value.realign();
	}
	return *this;
}

void SimpleTile::update_per_type(SimpleTile* array[],
								uint16_t array_length,
								UiTypes::SimpleTileType type,
								char *title_value,
								char *value, 
								char *units)
{
	for(uint8_t i = 0; i < array_length; i++)
	{
		if(array[i]->get_type() == type || type == UiTypes::SimpleTileType::ANY)
		{
			if(title_value != NULL)
			{
				if (type == UiTypes::SimpleTileType::WIND)
				{
					array[i]->title_value(title_value);
				}
				else
				{
					array[i]->title(title_value);
				}
			}
			if(value != NULL)
			{
				array[i]->value(value);
			}
			if(units != NULL)
			{
				array[i]->units(units);
			}
		}
	}
}

SimpleTile& SimpleTile::units(char *units)
{
	_lbl_units.text(units);
	return *this;
}

SimpleTile& SimpleTile::title_font(const lv_font_t *font)
{
	lv_obj_set_style_text_font(_lbl_title.self_lv_obj(), font, 0);
	return *this;
}

SimpleTile& SimpleTile::title_align(lv_align_t align, lv_coord_t x_ofs, lv_coord_t y_ofs)
{
	_lbl_title.align(align, x_ofs, y_ofs);
	return *this;
}

SimpleTile& SimpleTile::units_font(const lv_font_t *font)
{
	lv_obj_set_style_text_font(_lbl_units.self_lv_obj(), font, 0);
	return *this;
}

SimpleTile& SimpleTile::units_align(lv_align_t align, lv_coord_t x_ofs, lv_coord_t y_ofs)
{
	_lbl_units.align(align, x_ofs, y_ofs);
	return *this;
}

void SimpleTile::realign_middle_row()
{
	if (_tile_type == SimpleTileType::WIND)
	{
		_lbl_value.realign();
		_lbl_title_value.realign();

		int w_val = lv_obj_get_width(_lbl_value.self_lv_obj());
		int w_dir = lv_obj_get_width(_lbl_title_value.self_lv_obj());

		int total_width = w_val + 6 + w_dir;
		int x_offset = -(total_width / 2 - w_val / 2);

		_lbl_value.align(LV_ALIGN_CENTER, x_offset, 0);
		_lbl_value.realign();

		_lbl_title_value.align_to(_lbl_value.self_lv_obj(), LV_ALIGN_OUT_RIGHT_BOTTOM, 6, -3);
		_lbl_title_value.realign();
	}
}

SimpleTile& SimpleTile::realign_children()
{
	_lbl_title.realign();
	_lbl_title_value.realign();
	_lbl_value.realign();
	_lbl_units.realign();
	if (_tile_type == SimpleTileType::WIND)
	{
		realign_middle_row();
		_lbl_title.align(LV_ALIGN_TOP_MID, 0, 25);
		_lbl_title.realign();
	}
	return *this;
}

TempBigTile& TempBigTile::create(lv_obj_t *parent)
{
	_parent_lv_obj = parent;
	_self_lv_obj = lv_obj_create(_parent_lv_obj);
	lv_obj_remove_style_all(_self_lv_obj);
	radius(UI_SIMPLE_TILE_RADIUS);
	color_bg(UI_COLOR_PRIMARY_DARK);

	_lbl_temp.create(_self_lv_obj)
		.style(UiThemeWxm::get_style_heading_1())
		.color(UI_TEXT_COLOR_LIGHT)
		.text(UI_VALUES_DEFAULT)
		.align(LV_ALIGN_TOP_MID, 0, 155);

	_lbl_feels_like_title.create(_self_lv_obj)
		.style(UiThemeWxm::get_style_heading_3())
		.color(lv_color_hex(0xAF52DE)) // Violet (Feels Like)
		.text("Feels Like")
		.align(LV_ALIGN_BOTTOM_MID, -20, -50);

	_lbl_feels_like.create(_self_lv_obj)
		.style(UiThemeWxm::get_style_heading_2())
		.color(lv_color_hex(0xAF52DE)) // Violet (Feels Like)
		.text(UI_VALUES_DEFAULT)
		.align_to(_lbl_feels_like_title.self_lv_obj(), LV_ALIGN_OUT_RIGHT_BOTTOM, 6, 2);

	_lbl_dew_title.create(_self_lv_obj)
		.style(UiThemeWxm::get_style_heading_3())
		.color(lv_color_hex(0x64D2FF)) // Sky Blue (Dew Point)
		.text("Dew Point")
		.align(LV_ALIGN_BOTTOM_MID, -20, -12);

	_lbl_dew.create(_self_lv_obj)
		.style(UiThemeWxm::get_style_heading_2())
		.color(lv_color_hex(0x64D2FF)) // Sky Blue (Dew Point)
		.text(UI_VALUES_DEFAULT)
		.align_to(_lbl_dew_title.self_lv_obj(), LV_ALIGN_OUT_RIGHT_BOTTOM, 6, 2);

	_icon_weather_icon.create(_self_lv_obj).source(&UiResources::icon_weather_temp).align(LV_ALIGN_TOP_MID, 0, 37);
	return *this;
}

TempBigTile& TempBigTile::feels_like(char *feels_like)
{
	_lbl_feels_like.text(feels_like);
	_lbl_feels_like.realign();
	int title_and_value_width = lv_obj_get_width(_lbl_feels_like_title.self_lv_obj()) + 3 + lv_obj_get_width(_lbl_feels_like.self_lv_obj());
	_lbl_feels_like_title.align(LV_ALIGN_BOTTOM_MID, -(title_and_value_width/2 - lv_obj_get_width(_lbl_feels_like_title.self_lv_obj())/2), -50);
	_lbl_feels_like.realign();
	
	return *this;
}

TempBigTile& TempBigTile::temp(char *value)
{
	_lbl_temp.text(value);
	return *this;
}

TempBigTile& TempBigTile::dew(char *dew_val)
{
	_lbl_dew.text(dew_val);
	_lbl_dew.realign();
	int title_and_value_width = lv_obj_get_width(_lbl_dew_title.self_lv_obj()) + 3 + lv_obj_get_width(_lbl_dew.self_lv_obj());
	_lbl_dew_title.align(LV_ALIGN_BOTTOM_MID, -(title_and_value_width/2 - lv_obj_get_width(_lbl_dew_title.self_lv_obj())/2), -12);
	_lbl_dew.realign();
	return *this;
}

TempBigTile& TempBigTile::icon(UiTypes::WeatherIconType icon_type)
{
	return *this;
}

TempBigTile& TempBigTile::realign_children()
{
	_lbl_temp.realign();
	_lbl_feels_like_title.realign();
	_lbl_feels_like.realign();
	_lbl_dew_title.realign();
	_lbl_dew.realign();
	return *this;
}


static inline lv_coord_t to_chart_val(float val)
{
	if (val == (float)LV_CHART_POINT_NONE)
	{
		return LV_CHART_POINT_NONE;
	}
	return (lv_coord_t)(val * 100.0f);
}

TempDualTile& TempDualTile::create(lv_obj_t *parent)
{
	_chart = NULL;
	_ser_temp = NULL;
	_ser_feels_like = NULL;
	_timer = NULL;
	_last_temp = 0.0f;
	_last_feels_like = 0.0f;
	_temp_received = false;
	_feels_like_received = false;
	_has_history = false;
	_is_fahrenheit = false;
	memset(_temp_history, 0, sizeof(_temp_history));
	memset(_feels_like_history, 0, sizeof(_feels_like_history));

	_parent_lv_obj = parent;
	_self_lv_obj = lv_obj_create(_parent_lv_obj);
	lv_obj_remove_style_all(_self_lv_obj);
	radius(UI_SIMPLE_TILE_RADIUS);
	color_bg(UI_COLOR_PRIMARY_DARK);

	// 1. Create the background scrolling chart
	_chart = lv_chart_create(_self_lv_obj);
	lv_obj_remove_style_all(_chart);
	lv_obj_set_size(_chart, 280, 134);
	lv_obj_align(_chart, LV_ALIGN_CENTER, 0, 0);
	lv_chart_set_type(_chart, LV_CHART_TYPE_LINE);
	lv_chart_set_div_line_count(_chart, 0, 0); // Hide grid lines
	lv_chart_set_point_count(_chart, 120); // 120 points * 20s = 40 mins history
	
	// Style the chart as transparent background and borders
	lv_obj_set_style_bg_opa(_chart, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_opa(_chart, LV_OPA_TRANSP, 0);

	// Hide indicator points (the square black boxes)
	lv_obj_set_style_size(_chart, 0, LV_PART_INDICATOR);

	// Style lines to look professional and clean
	lv_obj_set_style_line_width(_chart, 2, LV_PART_ITEMS);
	lv_obj_set_style_line_opa(_chart, LV_OPA_80, LV_PART_ITEMS);

	// Add series (Actual Temp in Red/Coral, Feels Like in Violet/Lavender)
	_ser_temp = lv_chart_add_series(_chart, lv_color_hex(0xFF453A), LV_CHART_AXIS_PRIMARY_Y);
	_ser_feels_like = lv_chart_add_series(_chart, lv_color_hex(0xAF52DE), LV_CHART_AXIS_PRIMARY_Y);

	// Pre-fill chart with NONE initially to start clean on the right
	for (int i = 0; i < 120; i++) {
		lv_chart_set_next_value(_chart, _ser_temp, LV_CHART_POINT_NONE);
		lv_chart_set_next_value(_chart, _ser_feels_like, LV_CHART_POINT_NONE);
	}
	lv_chart_refresh(_chart);

	_has_history = false;
	memset(_temp_history, 0, sizeof(_temp_history));
	memset(_feels_like_history, 0, sizeof(_feels_like_history));

	// 2. Create Min/Mid/Max range labels (increased legibility size 14)
	_lbl_chart_max.create(_self_lv_obj)
		.font(&UiResources::ui_font_montserrat_14)
		.color(UI_TEXT_COLOR_LIGHT)
		.text("30") // default for Celsius max
		.align(LV_ALIGN_TOP_LEFT, 8, 8);
	lv_obj_set_style_text_opa(_lbl_chart_max.self_lv_obj(), LV_OPA_60, 0);

	_lbl_chart_mid.create(_self_lv_obj)
		.font(&UiResources::ui_font_montserrat_14)
		.color(UI_TEXT_COLOR_LIGHT)
		.text("22") // default for Celsius mid
		.align(LV_ALIGN_LEFT_MID, 8, -6);
	lv_obj_set_style_text_opa(_lbl_chart_mid.self_lv_obj(), LV_OPA_60, 0);

	_lbl_chart_min.create(_self_lv_obj)
		.font(&UiResources::ui_font_montserrat_14)
		.color(UI_TEXT_COLOR_LIGHT)
		.text("15") // default for Celsius min
		.align(LV_ALIGN_BOTTOM_LEFT, 8, -8);
	lv_obj_set_style_text_opa(_lbl_chart_min.self_lv_obj(), LV_OPA_60, 0);

	// 3. Create foreground label components on top of chart background (Color Coded!)
	_lbl_temp.create(_self_lv_obj)
		.font(&UiResources::ui_font_montserrat_42_bold)
		.color(lv_color_hex(0xFF453A)) // Coral Red
		.text(UI_VALUES_DEFAULT)
		.align(LV_ALIGN_TOP_MID, 0, 18);

	_lbl_feels_like_title.create(_self_lv_obj)
		.font(&lv_font_montserrat_14)
		.color(lv_color_hex(0xAF52DE)) // Violet (Feels Like)
		.text("Feels Like")
		.align(LV_ALIGN_BOTTOM_LEFT, 24, -12);

	_lbl_feels_like.create(_self_lv_obj)
		.font(&lv_font_montserrat_14)
		.color(lv_color_hex(0xAF52DE)) // Violet (Feels Like)
		.text(UI_VALUES_DEFAULT)
		.align_to(_lbl_feels_like_title.self_lv_obj(), LV_ALIGN_OUT_RIGHT_BOTTOM, 6, 0);

	_lbl_min_max.create(_self_lv_obj)
		.font(&lv_font_montserrat_14)
		.color(UI_TEXT_COLOR_LIGHT)
		.text("")
		.align(LV_ALIGN_BOTTOM_RIGHT, -24, -12);

	// 4. Create 20-second active background timer
	_timer = lv_timer_create(TempDualTile::timer_cb, 20000, this);

	return *this;
}

TempDualTile& TempDualTile::feels_like(char *feels_like)
{
	_lbl_feels_like.text(feels_like);
	lv_obj_align_to(_lbl_feels_like.self_lv_obj(), _lbl_feels_like_title.self_lv_obj(), LV_ALIGN_OUT_RIGHT_BOTTOM, 6, 0);
	return *this;
}

TempDualTile& TempDualTile::min_max(const char *min_max)
{
	_lbl_min_max.text(min_max);
	_lbl_min_max.realign();
	return *this;
}

TempDualTile& TempDualTile::temp(char *value)
{
	_lbl_temp.text(value);
	_lbl_temp.realign();
	
	if (_chart != NULL && value != NULL)
	{
		bool new_is_f = (strstr(value, "F") != NULL);
		if (_has_history && new_is_f != _is_fahrenheit)
		{
			float min_val = 9999.0f;
			float max_val = -9999.0f;
			bool valid_points = false;
			for (int i = 0; i < 120; i++)
			{
				if (_temp_history[i] != (float)LV_CHART_POINT_NONE && _temp_history[i] > -100.0f && _temp_history[i] < 150.0f)
				{
					_temp_history[i] = new_is_f ? (_temp_history[i] * 1.8f + 32.0f) : ((_temp_history[i] - 32.0f) / 1.8f);
					lv_chart_set_value_by_id(_chart, _ser_temp, i, to_chart_val(_temp_history[i]));
					if (_temp_history[i] < min_val) min_val = _temp_history[i];
					if (_temp_history[i] > max_val) max_val = _temp_history[i];
					valid_points = true;
				}
				if (_feels_like_history[i] != (float)LV_CHART_POINT_NONE && _feels_like_history[i] > -100.0f && _feels_like_history[i] < 150.0f)
				{
					_feels_like_history[i] = new_is_f ? (_feels_like_history[i] * 1.8f + 32.0f) : ((_feels_like_history[i] - 32.0f) / 1.8f);
					lv_chart_set_value_by_id(_chart, _ser_feels_like, i, to_chart_val(_feels_like_history[i]));
					if (_feels_like_history[i] < min_val) min_val = _feels_like_history[i];
					if (_feels_like_history[i] > max_val) max_val = _feels_like_history[i];
					valid_points = true;
				}
			}
			_is_fahrenheit = new_is_f;
			if (valid_points)
			{
				float span = max_val - min_val;
				float span_adjusted = (span < 1.0f) ? 1.0f : span;
				float chart_min = max_val - span_adjusted;
				float chart_max = max_val + span_adjusted;
				lv_chart_set_range(_chart, LV_CHART_AXIS_PRIMARY_Y, (lv_coord_t)(chart_min * 100.0f), (lv_coord_t)(chart_max * 100.0f));
				char lbl_buf[16];
				snprintf(lbl_buf, sizeof(lbl_buf), "%.0f", chart_max);
				_lbl_chart_max.text(lbl_buf);
				_lbl_chart_max.realign();
				snprintf(lbl_buf, sizeof(lbl_buf), "%.0f", (chart_min + chart_max) / 2.0f);
				_lbl_chart_mid.text(lbl_buf);
				_lbl_chart_mid.realign();
				snprintf(lbl_buf, sizeof(lbl_buf), "%.0f", chart_min);
				_lbl_chart_min.text(lbl_buf);
				_lbl_chart_min.realign();
			}
			lv_chart_refresh(_chart);
		}
		else if (!_has_history)
		{
			_is_fahrenheit = new_is_f;
			// adjust Y-axis scale dynamically based on selected temperature unit
			if (new_is_f) {
				lv_chart_set_range(_chart, LV_CHART_AXIS_PRIMARY_Y, 5000, 9000);
				_lbl_chart_max.text("90");
				_lbl_chart_mid.text("70");
				_lbl_chart_min.text("50");
			} else {
				// default to Celsius
				lv_chart_set_range(_chart, LV_CHART_AXIS_PRIMARY_Y, 1500, 3000);
				_lbl_chart_max.text("30");
				_lbl_chart_mid.text("22");
				_lbl_chart_min.text("15");
			}
			_lbl_chart_max.realign();
			_lbl_chart_mid.realign();
			_lbl_chart_min.realign();
		}
	}
	return *this;
}

TempDualTile& TempDualTile::add_temp_data(float temp)
{
	_last_temp = temp;
	_temp_received = true;
	return *this;
}

TempDualTile& TempDualTile::add_feels_like_data(float feels_like)
{
	_last_feels_like = feels_like;
	_feels_like_received = true;
	return *this;
}

void TempDualTile::tick_chart()
{
	if (_chart != NULL)
	{
		if (_temp_received || _feels_like_received)
		{
			// If we don't have history yet, initialize all points to this first value
			if (!_has_history)
			{
				float initial_temp = _last_temp;
				float initial_feels = _last_feels_like;
				if (!_temp_received) initial_temp = initial_feels;
				if (!_feels_like_received) initial_feels = initial_temp;
				
				for (int i = 0; i < 120; i++)
				{
					_temp_history[i] = initial_temp;
					_feels_like_history[i] = initial_feels;
					lv_chart_set_value_by_id(_chart, _ser_temp, i, to_chart_val(initial_temp));
					lv_chart_set_value_by_id(_chart, _ser_feels_like, i, to_chart_val(initial_feels));
				}
				_has_history = true;
			}
			else
			{
				// Shift history array
				for (int i = 0; i < 119; i++)
				{
					_temp_history[i] = _temp_history[i+1];
					_feels_like_history[i] = _feels_like_history[i+1];
				}
				_temp_history[119] = _last_temp;
				_feels_like_history[119] = _last_feels_like;

				// Add to LVGL chart (shifts left automatically)
				lv_chart_set_next_value(_chart, _ser_temp, to_chart_val(_last_temp));
				lv_chart_set_next_value(_chart, _ser_feels_like, to_chart_val(_last_feels_like));
			}

			// Calculate dynamic Y-axis range based on active history points
			float min_val = 9999.0f;
			float max_val = -9999.0f;
			bool valid_points = false;

			for (int i = 0; i < 120; i++)
			{
				if (_temp_history[i] != (float)LV_CHART_POINT_NONE && _temp_history[i] > -100.0f && _temp_history[i] < 150.0f)
				{
					if (_temp_history[i] < min_val) min_val = _temp_history[i];
					if (_temp_history[i] > max_val) max_val = _temp_history[i];
					valid_points = true;
				}
				if (_feels_like_history[i] != (float)LV_CHART_POINT_NONE && _feels_like_history[i] > -100.0f && _feels_like_history[i] < 150.0f)
				{
					if (_feels_like_history[i] < min_val) min_val = _feels_like_history[i];
					if (_feels_like_history[i] > max_val) max_val = _feels_like_history[i];
					valid_points = true;
				}
			}

			if (!valid_points)
			{
				min_val = 15.0f;
				max_val = 30.0f;
			}

			// Symmetrical Y-axis range centering the actual temp (max_val) perfectly at 50% midpoint
			// and placing the feels-like (min_val) at the absolute bottom (0% to 5% height) to align with the bottom of the text
			float span = max_val - min_val;
			float span_adjusted = (span < 1.0f) ? 1.0f : span;
			
			float chart_min = max_val - span_adjusted;
			float chart_max = max_val + span_adjusted;

			// Set chart range (highly precise with 100x scaling)
			lv_chart_set_range(_chart, LV_CHART_AXIS_PRIMARY_Y, (lv_coord_t)(chart_min * 100.0f), (lv_coord_t)(chart_max * 100.0f));

			// Format range labels (Max, Mid, Min)
			char lbl_buf[16];
			
			// 1. Max label (top of Normal Temp scale)
			snprintf(lbl_buf, sizeof(lbl_buf), "%.0f", chart_max);
			_lbl_chart_max.text(lbl_buf);
			_lbl_chart_max.realign();

			// 2. Mid label (bottom of Normal Temp / top of Feels Like scale - also the actual temp)
			float mid_val = (chart_min + chart_max) / 2.0f;
			snprintf(lbl_buf, sizeof(lbl_buf), "%.0f", mid_val);
			_lbl_chart_mid.text(lbl_buf);
			_lbl_chart_mid.realign();

			// 3. Min label (bottom of Feels Like scale)
			snprintf(lbl_buf, sizeof(lbl_buf), "%.0f", chart_min);
			_lbl_chart_min.text(lbl_buf);
			_lbl_chart_min.realign();

			_temp_received = false;
			_feels_like_received = false;
		}
		else
		{
			// Shift history with NONE values
			for (int i = 0; i < 119; i++)
			{
				_temp_history[i] = _temp_history[i+1];
				_feels_like_history[i] = _feels_like_history[i+1];
			}
			_temp_history[119] = (float)LV_CHART_POINT_NONE;
			_feels_like_history[119] = (float)LV_CHART_POINT_NONE;

			lv_chart_set_next_value(_chart, _ser_temp, LV_CHART_POINT_NONE);
			lv_chart_set_next_value(_chart, _ser_feels_like, LV_CHART_POINT_NONE);
		}
		lv_chart_refresh(_chart);
	}
}

void TempDualTile::timer_cb(lv_timer_t *timer)
{
	if (timer != NULL && timer->user_data != NULL)
	{
		TempDualTile *tile = (TempDualTile *)timer->user_data;
		tile->tick_chart();
	}
}

TempDualTile& TempDualTile::realign_children()
{
	_lbl_temp.realign();
	_lbl_feels_like_title.realign();
	lv_obj_align_to(_lbl_feels_like.self_lv_obj(), _lbl_feels_like_title.self_lv_obj(), LV_ALIGN_OUT_RIGHT_BOTTOM, 6, 0);
	_lbl_min_max.realign();
	_lbl_chart_max.realign();
	_lbl_chart_mid.realign();
	_lbl_chart_min.realign();
	if (_chart != NULL) {
		lv_obj_align(_chart, LV_ALIGN_CENTER, 0, 0);
	}
	return *this;
}

WindDualTile& WindDualTile::create(lv_obj_t *parent)
{
	_chart = NULL;
	_ser_speed = NULL;
	_ser_gust = NULL;
	_timer = NULL;
	_last_speed = 0.0f;
	_last_gust = 0.0f;
	_packet_received = false;
	_has_history = false;
	memset(_speed_history, 0, sizeof(_speed_history));
	memset(_gust_history, 0, sizeof(_gust_history));
	strcpy(_last_units, "m/s");

	_parent_lv_obj = parent;
	_self_lv_obj = lv_obj_create(_parent_lv_obj);
	lv_obj_remove_style_all(_self_lv_obj);
	radius(UI_SIMPLE_TILE_RADIUS);
	color_bg(UI_SIMPLE_TILE_WIND_COLOR);

	// 1. Create the chart first so it acts as background watermark and is drawn behind labels
	_chart = lv_chart_create(_self_lv_obj);
	lv_obj_remove_style_all(_chart);
	lv_obj_set_size(_chart, 280, 134);
	lv_obj_align(_chart, LV_ALIGN_CENTER, 0, 0);
	lv_chart_set_type(_chart, LV_CHART_TYPE_LINE);
	lv_chart_set_div_line_count(_chart, 0, 0); // Hide grid lines
	lv_chart_set_point_count(_chart, 120);    // 120 points (40 mins history)

	// Style the chart as transparent background and borders
	lv_obj_set_style_bg_opa(_chart, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_opa(_chart, LV_OPA_TRANSP, 0);

	// Hide indicator points (the square black boxes)
	lv_obj_set_style_size(_chart, 0, LV_PART_INDICATOR);

	// Style lines to look professional and clean
	lv_obj_set_style_line_width(_chart, 2, LV_PART_ITEMS);

	// Speed series: Glowing Neon Cyan
	_ser_speed = lv_chart_add_series(_chart, lv_color_hex(0x00E1FF), LV_CHART_AXIS_PRIMARY_Y);
	// Gust series: Soft Orange/Amber
	_ser_gust = lv_chart_add_series(_chart, lv_color_hex(0xFF9F00), LV_CHART_AXIS_PRIMARY_Y);

	// Pre-fill chart with zeros to keep start drawing tidy
	for (int i = 0; i < 120; i++) {
		lv_chart_set_next_value(_chart, _ser_speed, 0);
		lv_chart_set_next_value(_chart, _ser_gust, 0);
	}
	lv_chart_refresh(_chart);

	// 2. Create Min/Mid/Max range labels at Left
	_lbl_chart_max.create(_self_lv_obj)
		.font(&UiResources::ui_font_montserrat_14)
		.color(UI_SIMPLE_TILE_WIND_TEXT_COLOR)
		.text("15") // default for m/s
		.align(LV_ALIGN_TOP_LEFT, 8, 8);
	lv_obj_set_style_text_opa(_lbl_chart_max.self_lv_obj(), LV_OPA_60, 0); // elegant transparency

	_lbl_chart_mid.create(_self_lv_obj)
		.font(&UiResources::ui_font_montserrat_14)
		.color(UI_SIMPLE_TILE_WIND_TEXT_COLOR)
		.text("7")
		.align(LV_ALIGN_LEFT_MID, 8, -6);
	lv_obj_set_style_text_opa(_lbl_chart_mid.self_lv_obj(), LV_OPA_60, 0); // elegant transparency

	_lbl_chart_min.create(_self_lv_obj)
		.font(&UiResources::ui_font_montserrat_14)
		.color(UI_SIMPLE_TILE_WIND_TEXT_COLOR)
		.text("0")
		.align(LV_ALIGN_BOTTOM_LEFT, 8, -8);
	lv_obj_set_style_text_opa(_lbl_chart_min.self_lv_obj(), LV_OPA_60, 0); // elegant transparency

	// 3. Create foreground label components on top of chart background
	_lbl_title_separator.create(_self_lv_obj)
		.font(&lv_font_montserrat_18)
		.color(UI_SIMPLE_TILE_WIND_TEXT_COLOR)
		.text("\\")
		.align(LV_ALIGN_TOP_MID, 0, 12);

	_lbl_title_wind.create(_self_lv_obj)
		.font(&lv_font_montserrat_18)
		.color(lv_color_hex(0x00E1FF)) // glowing cyan speed color
		.text("Wind")
		.align_to(_lbl_title_separator.self_lv_obj(), LV_ALIGN_OUT_LEFT_MID, -4, 0);

	_lbl_title_gust.create(_self_lv_obj)
		.font(&lv_font_montserrat_18)
		.color(lv_color_hex(0xFF9F00)) // warning orange gust color
		.text("Gust")
		.align_to(_lbl_title_separator.self_lv_obj(), LV_ALIGN_OUT_RIGHT_MID, 4, 0);

	_lbl_direction_title.create(_self_lv_obj)
		.font(&lv_font_montserrat_14)
		.color(UI_SIMPLE_TILE_WIND_TEXT_COLOR)
		.text("Dir")
		.align(LV_ALIGN_BOTTOM_LEFT, 24, -12);

	_lbl_direction.create(_self_lv_obj)
		.font(&lv_font_montserrat_14)
		.color(UI_SIMPLE_TILE_WIND_TEXT_COLOR)
		.text("-")
		.align_to(_lbl_direction_title.self_lv_obj(), LV_ALIGN_OUT_RIGHT_BOTTOM, 6, 0);

	_lbl_peak_gust.create(_self_lv_obj)
		.font(&lv_font_montserrat_14)
		.color(UI_SIMPLE_TILE_WIND_TEXT_COLOR)
		.text("")
		.align(LV_ALIGN_BOTTOM_RIGHT, -24, -12);

	_lbl_speed_and_gust.create(_self_lv_obj)
		.style(UiThemeWxm::get_style_heading_1())
		.color(UI_SIMPLE_TILE_WIND_TEXT_COLOR)
		.text("--\\--")
		.align(LV_ALIGN_CENTER, -15, 0);

	_lbl_units.create(_self_lv_obj)
		.font(&UiResources::ui_font_montserrat_14) // keep units font small
		.color(UI_SIMPLE_TILE_WIND_TEXT_COLOR)
		.text(UI_VALUES_DEFAULT)
		.align_to(_lbl_speed_and_gust.self_lv_obj(), LV_ALIGN_OUT_RIGHT_BOTTOM, 6, -8);

	// 4. Create the 20-second active timer
	_timer = lv_timer_create(WindDualTile::timer_cb, 20000, this);

	return *this;
}

WindDualTile& WindDualTile::direction(const char *direction)
{
	_lbl_direction.text(direction);
	lv_obj_align_to(_lbl_direction.self_lv_obj(), _lbl_direction_title.self_lv_obj(), LV_ALIGN_OUT_RIGHT_BOTTOM, 6, 0);
	return *this;
}

WindDualTile& WindDualTile::peak_gust(const char *peak_gust)
{
	_lbl_peak_gust.text(peak_gust);
	_lbl_peak_gust.realign();
	return *this;
}

WindDualTile& WindDualTile::speed_and_gust(const char *speed_and_gust)
{
	_lbl_speed_and_gust.text(speed_and_gust);
	_lbl_speed_and_gust.realign();
	_lbl_units.realign();
	return *this;
}

static float wind_unit_factor_from_ms(const char *u)
{
	if (u == NULL) return 1.0f;
	if (strcmp(u, "km/h") == 0) return 3.6f;
	if (strcmp(u, "mph") == 0 || strcmp(u, "mi/h") == 0) return 2.23694f;
	if (strcmp(u, "knots") == 0 || strcmp(u, "kts") == 0 || strcmp(u, "kn") == 0) return 1.94384f;
	if (strcmp(u, "ft/s") == 0) return 3.28084f;
	return 1.0f;
}

WindDualTile& WindDualTile::units(const char *units)
{
	_lbl_units.text(units);
	if (_chart != NULL && units != NULL)
	{
		if (_has_history && strcmp(_last_units, units) != 0)
		{
			float old_f = wind_unit_factor_from_ms(_last_units);
			float new_f = wind_unit_factor_from_ms(units);
			if (old_f > 0.0f && new_f > 0.0f && strcmp(_last_units, "bft") != 0 && strcmp(units, "bft") != 0)
			{
				float ratio = new_f / old_f;
				float min_val = 9999.0f;
				float max_val = -9999.0f;
				bool valid_points = false;
				for (int i = 0; i < 120; i++)
				{
					if (_speed_history[i] != (float)LV_CHART_POINT_NONE && _speed_history[i] >= 0.0f && _speed_history[i] < 200.0f)
					{
						_speed_history[i] *= ratio;
						lv_chart_set_value_by_id(_chart, _ser_speed, i, (lv_coord_t)(_speed_history[i] * 10.0f));
						if (_speed_history[i] < min_val) min_val = _speed_history[i];
						if (_speed_history[i] > max_val) max_val = _speed_history[i];
						valid_points = true;
					}
					if (_gust_history[i] != (float)LV_CHART_POINT_NONE && _gust_history[i] >= 0.0f && _gust_history[i] < 200.0f)
					{
						_gust_history[i] *= ratio;
						lv_chart_set_value_by_id(_chart, _ser_gust, i, (lv_coord_t)(_gust_history[i] * 10.0f));
						if (_gust_history[i] < min_val) min_val = _gust_history[i];
						if (_gust_history[i] > max_val) max_val = _gust_history[i];
						valid_points = true;
					}
				}
				if (valid_points)
				{
					float chart_max = max_val * 1.10f;
					float chart_min = min_val * 0.90f;
					if (chart_min < 0.0f) chart_min = 0.0f;
					float min_span = 2.0f;
					if (strcmp(units, "km/h") == 0) min_span = 10.0f;
					else if (strcmp(units, "mph") == 0 || strcmp(units, "mi/h") == 0) min_span = 8.0f;
					else if (strcmp(units, "knots") == 0 || strcmp(units, "kts") == 0 || strcmp(units, "kn") == 0) min_span = 6.0f;
					else if (strcmp(units, "ft/s") == 0) min_span = 8.0f;
					else if (strcmp(units, "bft") == 0) min_span = 3.0f;
					if ((chart_max - chart_min) < min_span) chart_max = chart_min + min_span;
					lv_chart_set_range(_chart, LV_CHART_AXIS_PRIMARY_Y, (lv_coord_t)(chart_min * 10.0f), (lv_coord_t)(chart_max * 10.0f));
					char lbl_buf[16];
					snprintf(lbl_buf, sizeof(lbl_buf), "%.0f", chart_max);
					_lbl_chart_max.text(lbl_buf);
					_lbl_chart_max.realign();
					snprintf(lbl_buf, sizeof(lbl_buf), "%.0f", (chart_min + chart_max) / 2.0f);
					_lbl_chart_mid.text(lbl_buf);
					_lbl_chart_mid.realign();
					snprintf(lbl_buf, sizeof(lbl_buf), "%.0f", chart_min);
					_lbl_chart_min.text(lbl_buf);
					_lbl_chart_min.realign();
				}
				lv_chart_refresh(_chart);
			}
		}

		strncpy(_last_units, units, sizeof(_last_units) - 1);
		_last_units[sizeof(_last_units) - 1] = '\0';

		if (!_has_history)
		{
			int max_y = 15; // default for m/s
			if (strcmp(units, "km/h") == 0) max_y = 60;
			else if (strcmp(units, "mph") == 0 || strcmp(units, "mi/h") == 0) max_y = 40;
			else if (strcmp(units, "knots") == 0 || strcmp(units, "kts") == 0 || strcmp(units, "kn") == 0) max_y = 35;
			else if (strcmp(units, "ft/s") == 0) max_y = 50;
			else if (strcmp(units, "bft") == 0) max_y = 12;
			else if (strcmp(units, "m/s") == 0) max_y = 15;

			lv_chart_set_range(_chart, LV_CHART_AXIS_PRIMARY_Y, 0, (lv_coord_t)(max_y * 10.0f));

			char max_buff[16];
			snprintf(max_buff, sizeof(max_buff), "%d", max_y);
			_lbl_chart_max.text(max_buff);
			_lbl_chart_min.text("0");

			char mid_buff[16];
			snprintf(mid_buff, sizeof(mid_buff), "%d", max_y / 2);
			_lbl_chart_mid.text(mid_buff);

			_lbl_chart_max.realign();
			_lbl_chart_mid.realign();
			_lbl_chart_min.realign();
		}
	}
	_lbl_speed_and_gust.realign();
	_lbl_units.realign();
	return *this;
}

WindDualTile& WindDualTile::add_chart_data(float speed, float gust)
{
	_last_speed = speed;
	_last_gust = gust;
	_packet_received = true;
	return *this;
}

void WindDualTile::tick_chart()
{
	if (_chart != NULL)
	{
		if (_packet_received)
		{
			if (!_has_history)
			{
				float initial_speed = _last_speed;
				float initial_gust = _last_gust;
				for (int i = 0; i < 120; i++)
				{
					_speed_history[i] = initial_speed;
					_gust_history[i] = initial_gust;
					lv_chart_set_value_by_id(_chart, _ser_speed, i, (lv_coord_t)(initial_speed * 10.0f));
					lv_chart_set_value_by_id(_chart, _ser_gust, i, (lv_coord_t)(initial_gust * 10.0f));
				}
				_has_history = true;
			}
			else
			{
				for (int i = 0; i < 119; i++)
				{
					_speed_history[i] = _speed_history[i+1];
					_gust_history[i] = _gust_history[i+1];
				}
				_speed_history[119] = _last_speed;
				_gust_history[119] = _last_gust;

				lv_chart_set_next_value(_chart, _ser_speed, (lv_coord_t)(_last_speed * 10.0f));
				lv_chart_set_next_value(_chart, _ser_gust, (lv_coord_t)(_last_gust * 10.0f));
			}
			_packet_received = false;
		}
		else
		{
			if (_has_history)
			{
				for (int i = 0; i < 119; i++)
				{
					_speed_history[i] = _speed_history[i+1];
					_gust_history[i] = _gust_history[i+1];
				}
				_speed_history[119] = (float)LV_CHART_POINT_NONE;
				_gust_history[119] = (float)LV_CHART_POINT_NONE;
			}
			lv_chart_set_next_value(_chart, _ser_speed, LV_CHART_POINT_NONE);
			lv_chart_set_next_value(_chart, _ser_gust, LV_CHART_POINT_NONE);
		}

		if (_has_history)
		{
			float min_val = 9999.0f;
			float max_val = -9999.0f;
			bool valid_points = false;

			for (int i = 0; i < 120; i++)
			{
				if (_speed_history[i] != (float)LV_CHART_POINT_NONE && _speed_history[i] >= 0.0f && _speed_history[i] < 200.0f)
				{
					if (_speed_history[i] < min_val) min_val = _speed_history[i];
					if (_speed_history[i] > max_val) max_val = _speed_history[i];
					valid_points = true;
				}
				if (_gust_history[i] != (float)LV_CHART_POINT_NONE && _gust_history[i] >= 0.0f && _gust_history[i] < 200.0f)
				{
					if (_gust_history[i] < min_val) min_val = _gust_history[i];
					if (_gust_history[i] > max_val) max_val = _gust_history[i];
					valid_points = true;
				}
			}

			if (valid_points)
			{
				float chart_max = max_val * 1.10f;
				float chart_min = min_val * 0.90f;
				if (chart_min < 0.0f) chart_min = 0.0f;

				float min_span = 2.0f; // default for m/s
				if (strcmp(_last_units, "km/h") == 0) min_span = 10.0f;
				else if (strcmp(_last_units, "mph") == 0 || strcmp(_last_units, "mi/h") == 0) min_span = 8.0f;
				else if (strcmp(_last_units, "knots") == 0 || strcmp(_last_units, "kts") == 0 || strcmp(_last_units, "kn") == 0) min_span = 6.0f;
				else if (strcmp(_last_units, "ft/s") == 0) min_span = 8.0f;
				else if (strcmp(_last_units, "bft") == 0) min_span = 3.0f;

				if ((chart_max - chart_min) < min_span)
				{
					chart_max = chart_min + min_span;
				}

				lv_chart_set_range(_chart, LV_CHART_AXIS_PRIMARY_Y, (lv_coord_t)(chart_min * 10.0f), (lv_coord_t)(chart_max * 10.0f));

				char lbl_buf[16];
				snprintf(lbl_buf, sizeof(lbl_buf), "%.0f", chart_max);
				_lbl_chart_max.text(lbl_buf);
				_lbl_chart_max.realign();

				float mid_val = (chart_min + chart_max) / 2.0f;
				snprintf(lbl_buf, sizeof(lbl_buf), "%.0f", mid_val);
				_lbl_chart_mid.text(lbl_buf);
				_lbl_chart_mid.realign();

				snprintf(lbl_buf, sizeof(lbl_buf), "%.0f", chart_min);
				_lbl_chart_min.text(lbl_buf);
				_lbl_chart_min.realign();
			}
		}
		lv_chart_refresh(_chart);
	}
}

void WindDualTile::timer_cb(lv_timer_t *timer)
{
	if (timer != NULL && timer->user_data != NULL)
	{
		WindDualTile *tile = (WindDualTile *)timer->user_data;
		tile->tick_chart();
	}
}

WindDualTile& WindDualTile::realign_children()
{
	_lbl_title_separator.realign();
	_lbl_title_wind.realign();
	_lbl_title_gust.realign();
	_lbl_direction_title.realign();
	lv_obj_align_to(_lbl_direction.self_lv_obj(), _lbl_direction_title.self_lv_obj(), LV_ALIGN_OUT_RIGHT_BOTTOM, 6, 0);
	_lbl_peak_gust.realign();
	_lbl_speed_and_gust.realign();
	_lbl_units.realign();
	_lbl_chart_max.realign();
	_lbl_chart_mid.realign();
	_lbl_chart_min.realign();
	if (_chart != NULL) {
		lv_obj_align(_chart, LV_ALIGN_CENTER, 0, 0);
	}
	return *this;
}

SettingsSectionComponent& SettingsSectionComponent::create(lv_obj_t *parent)
{
	_parent_lv_obj = parent;
	_self_lv_obj = lv_obj_create(parent);
	_lbl_info.create(_self_lv_obj)
		.font(&lv_font_montserrat_14)
		.align(LV_ALIGN_TOP_LEFT, 20, 10)
		.color(lv_color_white());
	radius(UI_ITEM_RADIUS);
	color_bg(UI_COLOR_PRIMARY_DARK);
	return *this;
}

SettingsSectionComponent& SettingsSectionComponent::set_title(const char *title)
{
	_lbl_info.text(title);
	return *this;
}

SettingsSectionComponent& SettingsSectionComponent::realign_children()
{
	_lbl_info.realign();
	return *this;
}

SettingsButtonComponent& SettingsButtonComponent::create(lv_obj_t *parent)
{
	SettingsItemBase::create(parent);
	return *this;
}

SettingsInfoComponent& SettingsInfoComponent::create(lv_obj_t *parent)
{
	SettingsItemBase::TITLE_Y_OFF = -12;
	SettingsItemBase::create(parent);
	clickable(false);
	_lbl_info.create(_self_lv_obj)
		.align(LV_ALIGN_LEFT_MID, TITLE_X_OFF, -TITLE_Y_OFF)
		.color(UI_COLOR_SECONDARY_DARK);
	return *this;
}

SettingsInfoComponent& SettingsInfoComponent::set_info(char *info)
{
	_lbl_info.text(info);
	return *this;
}

SettingsInfoComponent& SettingsInfoComponent::realign_children()
{
	SettingsItemBase::realign_children();
	_lbl_info.realign();
	return *this;
}

SettingsDropDownComponent& SettingsDropDownComponent::create(lv_obj_t *parent)
{
	SettingsItemBase::TITLE_Y_OFF = -10;
	SettingsItemBase::create(parent);

	_lv_dropdown = lv_dropdown_create(_self_lv_obj);	
	lv_obj_clear_flag(_lv_dropdown, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_remove_style_all(_lv_dropdown);
	lv_obj_set_style_pad_all(_lv_dropdown, 0, LV_PART_MAIN);
	lv_obj_set_style_pad_top(_lv_dropdown, 35, LV_PART_MAIN);
	// lv_obj_set_style_pad_left(_lv_dropdown, 15, LV_PART_MAIN);
	lv_dropdown_set_symbol(_lv_dropdown, "");
	lv_obj_set_style_text_font(_lv_dropdown, &lv_font_montserrat_14, 0);
	lv_obj_set_style_text_color(_lv_dropdown, UI_COLOR_SECONDARY_DARK, 0);

	lv_obj_t *list = lv_dropdown_get_list(_lv_dropdown);
	lv_obj_remove_style_all(list);
	lv_obj_set_style_max_height(list, 3*51, LV_PART_MAIN);
	lv_obj_set_style_pad_all(list, 15, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_clip_corner(list, true, LV_PART_MAIN);
	lv_obj_set_style_radius(list, UI_ITEM_RADIUS, LV_PART_MAIN);	
	// lv_obj_set_style_outline_width(list, 10, LV_PART_MAIN);
	lv_obj_set_style_bg_color(list, lv_color_lighten(lv_color_hex(0x1D1E2C), 20), LV_PART_SELECTED);
	lv_obj_set_style_bg_color(list, lv_color_hex(0x353752), LV_PART_SELECTED | LV_STATE_CHECKED);
	lv_obj_set_style_bg_color(list, lv_color_hex(0x1D1E2C), LV_PART_MAIN);
	lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
	lv_obj_set_style_bg_color(list, UI_COLOR_SECONDARY_DARK, LV_PART_SCROLLBAR);
	lv_obj_set_style_bg_opa(list, LV_OPA_COVER, LV_PART_SCROLLBAR);
	lv_obj_set_style_width(list, UI_ITEM_RADIUS, LV_PART_SCROLLBAR);	
	lv_obj_set_style_radius(list, UI_ITEM_RADIUS, LV_PART_SCROLLBAR);
	lv_obj_set_style_pad_top(list, 3, LV_PART_SCROLLBAR);
	lv_obj_set_style_pad_bottom(list, 3, LV_PART_SCROLLBAR);
	lv_obj_set_style_pad_right(list, 3, LV_PART_SCROLLBAR);
	lv_obj_set_style_bg_opa(list, LV_OPA_COVER, LV_PART_SELECTED);
	lv_obj_set_style_bg_opa(list, LV_OPA_COVER, LV_PART_SELECTED | LV_STATE_CHECKED);
	lv_obj_set_style_bg_opa(list, LV_OPA_COVER, LV_PART_MAIN);
	lv_obj_set_style_bg_opa(list, LV_OPA_COVER, LV_PART_SCROLLBAR);
	lv_obj_set_style_text_font(list, &lv_font_montserrat_18, LV_PART_MAIN);
	lv_obj_set_style_text_color(list, lv_color_white(), LV_PART_MAIN);
	lv_obj_set_style_text_line_space(list, 30, LV_PART_MAIN);
	// lv_obj_set_style_shadow_color(list, lv_color_black(), LV_PART_MAIN);
	// lv_obj_set_style_shadow_opa(list, 10, LV_PART_MAIN);
	// lv_obj_set_style_shadow_spread(list, 10, LV_PART_MAIN);
	lv_obj_set_size(_lv_dropdown, 368, 62);	
	lv_obj_align(_lv_dropdown, LV_ALIGN_LEFT_MID, TITLE_X_OFF, 0);
	lv_obj_add_event_cb(_self_lv_obj, [](lv_event_t *event) {
		lv_event_code_t  event_code = lv_event_get_code(event);		
		lv_obj_t *dd_obj =  (lv_obj_t*) lv_event_get_user_data(event);
		lv_obj_t *list = lv_dropdown_get_list(dd_obj);
		lv_obj_t *item_obj =  (lv_obj_t*) lv_event_get_current_target(event);
		static bool closed_by_press = false;
		static bool clicked = false;
		if(event_code == LV_EVENT_CLICKED)
		{
			clicked = true;
			if(lv_dropdown_is_open(dd_obj))
			{
				lv_dropdown_close(dd_obj);
			}
			else
			{
				if(!closed_by_press)
				{				
					lv_area_t item_coords;
					lv_obj_get_coords(item_obj, &item_coords);
					if(item_coords.y1 + (item_coords.y2 - item_coords.y1)/2 > UI_SCREEN_HEIGHT/2)
					{
						lv_dropdown_set_dir(dd_obj, LV_DIR_TOP);
					}
					else
					{
						lv_dropdown_set_dir(dd_obj, LV_DIR_BOTTOM);
					}
					lv_dropdown_open(dd_obj);
					if(lv_dropdown_get_dir(dd_obj) == LV_DIR_TOP)
					{
						lv_obj_set_y(list, lv_obj_get_y(list) + 54);
					}
					else if (lv_dropdown_get_dir(dd_obj) == LV_DIR_BOTTOM)
					{
						lv_obj_set_y(list, lv_obj_get_y(list) - 54);
					}
				}
				closed_by_press = false;
			}
		}
		else if(event_code == LV_EVENT_DEFOCUSED)
		{
			if(lv_dropdown_is_open(dd_obj))
			{
				lv_dropdown_close(dd_obj);
			}
		}
		else if(event_code == LV_EVENT_PRESSED)
		{
			if(lv_dropdown_is_open(dd_obj))
			{
				lv_dropdown_close(dd_obj);
				closed_by_press = true;
			}
		}
		else if(event_code == LV_EVENT_RELEASED)
		{
			if(!clicked)
			{
				// drag happened
				closed_by_press = false;
			}
			clicked = false;
		}
	}, LV_EVENT_ALL, _lv_dropdown);

	return *this;
}

SettingsDropDownComponent& SettingsDropDownComponent::set_options(const char *options)
{
	lv_dropdown_set_options(_lv_dropdown, options);
	return *this;
}

SettingsDropDownComponent& SettingsDropDownComponent::set_display_text(const char *text)
{
	lv_dropdown_set_text(_lv_dropdown, text);
	return *this;
}

SettingsDropDownComponent& SettingsDropDownComponent::clear_display_text()
{
	lv_dropdown_set_text(_lv_dropdown, nullptr);
	return *this;
}

int SettingsDropDownComponent::get_item_index(const char *item_text)
{
	if(_lv_dropdown == nullptr || item_text == nullptr)
		return -1;

	const char *options = lv_dropdown_get_options(_lv_dropdown);

	if(strlen(options) < 1)
		return -1;

	char *item_pos = nullptr;
	if((item_pos = strstr(options, item_text)) == nullptr)
		return -1;

	// Next must be either end of string, or start of next item,
	// otherwise it is a partial find
	char *next = item_pos + strlen(item_text);
	if(*next != '\0' && *next != '\n')
		return -1;

	// First item
	if(item_pos == options)
		return 0;

	int index = 0;
	const char *cur_pos = options;
	while(1)
	{
		char *nl = strchr(cur_pos, '\n');

		if(nl != nullptr && nl < item_pos)
			index++;
		else
			break;

		cur_pos = nl + 1;
	}

	return index;
}

void SettingsDropDownComponent::set_selected_text(const char *item_text)
{
	if(_lv_dropdown == nullptr || item_text == nullptr)
	{
		return;
	}

	int index = get_item_index(item_text);
	if(index < 0)
	{
		return;
	}
	

	lv_dropdown_set_selected(_lv_dropdown, index);
}

SettingsDropDownComponent& SettingsDropDownComponent::set_selected(uint16_t sel_opt)
{
	lv_dropdown_set_selected(_lv_dropdown, sel_opt);
	return *this;
}

SettingsDropDownComponent& SettingsDropDownComponent::set_cb(lv_event_cb_t event_cb, void *user_data)
{
	lv_obj_add_event_cb(_lv_dropdown, event_cb, LV_EVENT_ALL, user_data);
	return *this;
}

SettingsDropDownComponent& SettingsDropDownComponent::realign_children()
{
	SettingsItemBase::realign_children();
	return *this;
}

ButtonComponent& ButtonComponent::create(lv_obj_t *parent)
{
	_parent_lv_obj = parent;
	_self_lv_obj = lv_btn_create(parent);
	lv_obj_remove_style_all(_self_lv_obj);
	_lbl_title.create(_self_lv_obj).font(&lv_font_montserrat_18).color(UI_COLOR_PRIMARY_DARK).align(LV_ALIGN_CENTER, 0, 0);
	color_bg(lv_color_hex(0xB8C6FF)).color_bg_clicked(lv_color_hex(0x8a94bf)).border_width(0).border_color(lv_color_hex(0xB8C6FF)).radius(6);
	// lv_obj_set_drag_parent(_self_lv_obj, true);
	return *this;
}

ButtonComponent& ButtonComponent::title(char *feels_like)
{
	_lbl_title.text(feels_like);
	return *this;
}

ButtonComponent& ButtonComponent::color_title(lv_color_t color)
{
	_lbl_title.color(color);
	return *this;
}

ButtonComponent& ButtonComponent::color_bg_clicked(lv_color_t color)
{
	lv_obj_set_style_bg_color(_self_lv_obj, color, LV_PART_MAIN | LV_STATE_PRESSED);
	lv_obj_set_style_bg_img_recolor_opa(_self_lv_obj, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_PRESSED);
	return *this;
}

ButtonComponent& ButtonComponent::icon(const void *icon_src)
{
	if(NULL != _lbl_title.self_lv_obj())
	{
		_lbl_title.del_self();
	}
	lv_obj_remove_style_all(_self_lv_obj);
	_icon.create(_self_lv_obj).source(icon_src).align(LV_ALIGN_CENTER, 0, 0);
	return *this;
}

ButtonComponent& ButtonComponent::realign_children()
{
	if(NULL != _lbl_title.self_lv_obj())
	{
		_lbl_title.realign();
	}
	if(NULL != _icon.self_lv_obj())
	{
		_icon.realign();
	}
	return *this;
}

QrCodeComponent& QrCodeComponent::create(lv_obj_t *parent)
{
	_parent_lv_obj = parent;
	_self_lv_obj = lv_qrcode_create(_parent_lv_obj, 146, lv_color_hex(0xFEFBFF), UI_COLOR_BG_DARK);
	return *this;
}

QrCodeComponent& QrCodeComponent::data(char *data, size_t data_length)
{
	lv_qrcode_update(_self_lv_obj, data, data_length);
	return *this;
}

QrCodeComponent& QrCodeComponent::realign_children()
{
	return *this;
}

SpinnerComponent& SpinnerComponent::create(lv_obj_t *parent)
{
	_parent_lv_obj = parent;
	_self_lv_obj = lv_spinner_create(_parent_lv_obj, 1000, 80);
	lv_obj_set_style_arc_rounded(_self_lv_obj, true, LV_PART_INDICATOR);
	return *this;
}

SpinnerComponent& SpinnerComponent::color_arc_bg(lv_color_t color)
{
	lv_obj_set_style_arc_color(_self_lv_obj, color, LV_PART_MAIN);
	return *this;
}

SpinnerComponent& SpinnerComponent::color_arc_line(lv_color_t color)
{
	lv_obj_set_style_arc_color(_self_lv_obj, color, LV_PART_INDICATOR);
	return *this;
}

SpinnerComponent& SpinnerComponent::realign_children()
{
	return *this;
}

}
