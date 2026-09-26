#ifndef UI_VIEW_DETAILED_H
#define UI_VIEW_DETAILED_H

#include "ui/include/ui_common.h"

namespace UiViewDetailed
{

class View : public UiTypes::ContainerBase<View>
{
  public:
	View &create(lv_obj_t *parent);
	void update_temp(char *temp);
	void update_feels_like(char *feels_like);
	void update_wind_dual(char *wind_spd_gust);
	void update_wind_dual_units(char *units);
	void update_wind_dual_dir(char *wind_dir);
	void update_wind_chart(float speed, float gust);
	void update_temp_chart_temp(float temp);
	void update_temp_chart_feels_like(float feels_like);
	void update_temp_min_max(const char *min_max);
	void update_wind_peak_gust(const char *peak_gust);
	UiTypes::SimpleTile** get_simple_tiles_array();
	uint16_t get_simple_tiles_array_len();
	
  protected:
	View &realign_children();

  private:
	UiTypes::SimpleTile _tile_top_right;	
	UiTypes::SimpleTile _tile_left_middle;
	UiTypes::SimpleTile _tile_bottom_right;
	UiTypes::SimpleTile _tile_bottom_middle;
	UiTypes::SimpleTile _tile_bottom_left;

	static const int NUM_OF_SIMPLE_TILES = 5;
	UiTypes::SimpleTile *_simple_tiles[NUM_OF_SIMPLE_TILES] = {&_tile_top_right, 
													&_tile_left_middle, 
													&_tile_bottom_right,
													&_tile_bottom_middle,
													&_tile_bottom_left
													};
	UiTypes::WindDualTile _tile_wind;
	UiTypes::TempDualTile _tile_temp;
};

} // namespace UiViewDetailed

#endif