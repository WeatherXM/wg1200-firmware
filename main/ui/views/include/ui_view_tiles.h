#ifndef UI_VIEW_TILES_H
#define UI_VIEW_TILES_H

#include "ui/include/ui_common.h"

namespace UiViewTiles
{

class View : public UiTypes::ContainerBase<View>
{
  public:
	View &create(lv_obj_t *parent);
	void update_temp(char *temp);
	void update_feels_like(char *feels_like);
	void update_dew(char *dew);
	void update_icon(UiTypes::WeatherIconType icon);
	UiTypes::SimpleTile** get_simple_tiles_array();
	uint16_t get_simple_tiles_array_len();

  protected:
	View &realign_children();

  private:
	UiTypes::SimpleTile _tile_bottom_middle;
	UiTypes::SimpleTile _tile_top_right;
	UiTypes::SimpleTile _tile_right_middle;
	UiTypes::SimpleTile _tile_bottom_right;
	UiTypes::SimpleTile _tile_bottom_left;
	static const int NUM_OF_SIMPLE_TILES = 5;
	UiTypes::SimpleTile *_simple_tiles[NUM_OF_SIMPLE_TILES] = {&_tile_bottom_middle, 
													&_tile_top_right, 
													&_tile_right_middle,
													&_tile_bottom_right,
													&_tile_bottom_left
													};
	UiTypes::TempBigTile _tile_temp;
	
};

} // namespace UiViewTiles

#endif