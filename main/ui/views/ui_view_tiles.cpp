#include "ui/views/include/ui_view_tiles.h"
#include "ui/include/ui_common.h"

namespace UiViewTiles
{

/******************************************************************************
 * @brief Update temperature value.
 * @param temp Temperature float.
 * @param units Units text.
 ******************************************************************************/
void View::update_temp(char *temp)
{
	_tile_temp.temp(temp);
}

/******************************************************************************
 * @brief Update feels like value.
 * @param feels_like Feels like float.
 * @param units Units text.
 ******************************************************************************/
void View::update_feels_like(char *temp)
{
	_tile_temp.feels_like(temp);
}

/******************************************************************************
 * @brief Update dew point value.
 * @param dew Dew point text.
 ******************************************************************************/
void View::update_dew(char *dew)
{
	_tile_temp.dew(dew);
}

/******************************************************************************
 * @brief Update weather icon.
 * @param icon Icon resource.
 ******************************************************************************/
void View::update_icon(UiTypes::WeatherIconType icon)
{
	_tile_temp.icon(icon);
}

UiTypes::SimpleTile** View::get_simple_tiles_array()
{
	return &_simple_tiles[0];
}

uint16_t View::get_simple_tiles_array_len()
{
	return NUM_OF_SIMPLE_TILES;
}

View &View::realign_children()
{
	_tile_top_right.realign();
	_tile_bottom_left.realign();
	_tile_bottom_middle.realign();
	_tile_bottom_right.realign();
	_tile_right_middle.realign();
	_tile_temp.realign();
	return *this;
}

View &View::create(lv_obj_t *parent)
{
	_parent_lv_obj = parent;
	_self_lv_obj = lv_obj_create(_parent_lv_obj);
	lv_obj_remove_style_all(_self_lv_obj);
	lv_obj_add_style(_self_lv_obj, UiThemeWxm::get_style_obj_basic(), LV_PART_ANY);
	lv_obj_set_style_pad_all(_self_lv_obj, 0, LV_PART_MAIN);

	_tile_top_right.create(_self_lv_obj)
		.size(UI_SIMPLE_TILE_WIDTH, UI_SIMPLE_TILE_HEIGHT)
		.set_type(UiTypes::SimpleTileType::PRECIP_RATE)
		.clickable(false)
		.align(LV_ALIGN_TOP_RIGHT, -UI_ITEM_GAP, 0);

	_tile_right_middle.create(_self_lv_obj)
		.size(UI_SIMPLE_TILE_WIDTH, UI_SIMPLE_TILE_HEIGHT)
		.set_type(UiTypes::SimpleTileType::PRESSURE)
		.clickable(false)
		.align_to(_tile_top_right.self_lv_obj(), LV_ALIGN_OUT_BOTTOM_MID, 0, UI_ITEM_GAP);

	_tile_bottom_right.create(_self_lv_obj)
		.size(UI_SIMPLE_TILE_WIDTH, UI_SIMPLE_TILE_HEIGHT)
		.set_type(UiTypes::SimpleTileType::WIND)
		.clickable(false)
		.align(LV_ALIGN_BOTTOM_RIGHT, -UI_ITEM_GAP, -UI_ITEM_GAP);	

	_tile_bottom_middle.create(_self_lv_obj)
		.size(UI_SIMPLE_TILE_WIDTH, UI_SIMPLE_TILE_HEIGHT)
		.set_type(UiTypes::SimpleTileType::IRRADIANCE)
		.clickable(false)
		.align(LV_ALIGN_BOTTOM_MID, 0, -UI_ITEM_GAP);

	_tile_bottom_left.create(_self_lv_obj)
		.size(UI_SIMPLE_TILE_WIDTH, UI_SIMPLE_TILE_HEIGHT)
		.set_type(UiTypes::SimpleTileType::HUMIDITY)
		.clickable(false)
		.align(LV_ALIGN_BOTTOM_LEFT, UI_ITEM_GAP, -UI_ITEM_GAP);	

	_tile_temp.create(_self_lv_obj)
		.size(310, 294)
		.clickable(false)
		.align(LV_ALIGN_TOP_LEFT, UI_ITEM_GAP, 0);

	return *this;
}

} // namespace UiViewTiles