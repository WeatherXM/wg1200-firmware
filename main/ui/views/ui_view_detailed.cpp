#include "ui/views/include/ui_view_detailed.h"

namespace UiViewDetailed
{

View &View::create(lv_obj_t *parent)
{
	_parent_lv_obj = parent;
	_self_lv_obj = lv_obj_create(_parent_lv_obj);
	lv_obj_remove_style_all(_self_lv_obj);
	lv_obj_add_style(_self_lv_obj, UiThemeWxm::get_style_obj_basic(), LV_PART_ANY);

	_tile_temp.create(_self_lv_obj)
		.size(2*UI_SIMPLE_TILE_WIDTH + UI_ITEM_GAP, UI_SIMPLE_TILE_HEIGHT)
		.clickable(false)
		.align(LV_ALIGN_TOP_LEFT, UI_ITEM_GAP, 0);

	_tile_top_right.create(_self_lv_obj)
		.size(UI_SIMPLE_TILE_WIDTH, UI_SIMPLE_TILE_HEIGHT)
		.set_type(UiTypes::SimpleTileType::PRECIP_DAILY)
		.title("Daily Rain (mm)")
		.title_font(&lv_font_montserrat_14)
		.title_align(LV_ALIGN_TOP_MID, 0, 14)
		.units_font(&lv_font_montserrat_14)
		.units_align(LV_ALIGN_BOTTOM_MID, 0, -14)
		.clickable(false)
		.align(LV_ALIGN_TOP_RIGHT, -UI_ITEM_GAP, 0);

	_tile_left_middle.create(_self_lv_obj)
		.size(UI_SIMPLE_TILE_WIDTH, UI_SIMPLE_TILE_HEIGHT)
		.set_type(UiTypes::SimpleTileType::PRESSURE)
		.title("Pressure (hPa)")
		.title_font(&lv_font_montserrat_14)
		.title_align(LV_ALIGN_TOP_MID, 0, 14)
		.units_font(&lv_font_montserrat_14)
		.units_align(LV_ALIGN_BOTTOM_MID, 0, -14)
		.clickable(false)
		.align_to(_tile_temp.self_lv_obj(), LV_ALIGN_OUT_BOTTOM_LEFT, 0, UI_ITEM_GAP);

	_tile_wind.create(_self_lv_obj)
		.size(2*UI_SIMPLE_TILE_WIDTH + UI_ITEM_GAP, UI_SIMPLE_TILE_HEIGHT)
		.clickable(false)
		.direction(UI_VALUES_DEFAULT)
		.align_to(_tile_top_right.self_lv_obj(), LV_ALIGN_OUT_BOTTOM_RIGHT, 0, UI_ITEM_GAP);

	_tile_bottom_right.create(_self_lv_obj)
		.size(UI_SIMPLE_TILE_WIDTH, UI_SIMPLE_TILE_HEIGHT)
		.set_type(UiTypes::SimpleTileType::IRRADIANCE)
		.title("Solar (W/m2)")
		.title_font(&lv_font_montserrat_14)
		.title_align(LV_ALIGN_TOP_MID, 0, 14)
		.units_font(&lv_font_montserrat_14)
		.units_align(LV_ALIGN_BOTTOM_MID, 0, -14)
		.clickable(false)
		.align_to(_tile_wind.self_lv_obj(), LV_ALIGN_OUT_BOTTOM_RIGHT, 0, UI_ITEM_GAP);

	_tile_bottom_middle.create(_self_lv_obj)
		.size(UI_SIMPLE_TILE_WIDTH, UI_SIMPLE_TILE_HEIGHT)
		.set_type(UiTypes::SimpleTileType::HUMIDITY)
		.title("Humidity (%)")
		.title_font(&lv_font_montserrat_14)
		.title_align(LV_ALIGN_TOP_MID, 0, 14)
		.units_font(&lv_font_montserrat_14)
		.units_align(LV_ALIGN_BOTTOM_MID, 0, -14)
		.clickable(false)
		.align_to(_tile_wind.self_lv_obj(), LV_ALIGN_OUT_BOTTOM_LEFT, 0, UI_ITEM_GAP);

	_tile_bottom_left.create(_self_lv_obj)
		.size(UI_SIMPLE_TILE_WIDTH, UI_SIMPLE_TILE_HEIGHT)
		.set_type(UiTypes::SimpleTileType::UV_INDEX)
		.title("UV Index")
		.title_font(&lv_font_montserrat_14)
		.title_align(LV_ALIGN_TOP_MID, 0, 14)
		.units_font(&lv_font_montserrat_14)
		.units_align(LV_ALIGN_BOTTOM_MID, 0, -14)
		.clickable(false)
		.align_to(_tile_left_middle.self_lv_obj(), LV_ALIGN_OUT_BOTTOM_LEFT, 0, UI_ITEM_GAP);

	return *this;
}

View &View::realign_children()
{
	_tile_top_right.realign();
	_tile_left_middle.realign();
	_tile_bottom_right.realign();
	_tile_bottom_middle.realign();
	_tile_bottom_left.realign();
	_tile_wind.realign();
	_tile_temp.realign();

	return *this;
}

void View::update_temp(char *temp)
{
	_tile_temp.temp(temp);
}
void View::update_feels_like(char *feels_like)
{
	_tile_temp.feels_like(feels_like);
}

void View::update_wind_dual(char *wind_spd_gust)
{
	_tile_wind.speed_and_gust(wind_spd_gust);
}

void View::update_wind_dual_units(char *units)
{
	_tile_wind.units(units);
}

void View::update_wind_dual_dir(char *wind_dir)
{
	_tile_wind.direction(wind_dir);
}

void View::update_wind_chart(float speed, float gust)
{
	_tile_wind.add_chart_data(speed, gust);
}

void View::update_temp_chart_temp(float temp)
{
	_tile_temp.add_temp_data(temp);
}

void View::update_temp_chart_feels_like(float feels_like)
{
	_tile_temp.add_feels_like_data(feels_like);
}

void View::update_temp_min_max(const char *min_max)
{
	_tile_temp.min_max(min_max);
}

void View::update_wind_peak_gust(const char *peak_gust)
{
	_tile_wind.peak_gust(peak_gust);
}

UiTypes::SimpleTile** View::get_simple_tiles_array()
{
	return &_simple_tiles[0];
}

uint16_t View::get_simple_tiles_array_len()
{
	return NUM_OF_SIMPLE_TILES;
}

} // namespace UiViewDetailed