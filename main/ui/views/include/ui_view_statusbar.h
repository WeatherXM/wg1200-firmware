#ifndef UI_VIEW_STATUSBAR_H
#define UI_VIEW_STATUSBAR_H

#include "ui/include/ui_common.h"

namespace UiViewStatusBar
{

enum class WiFiSignalType
{
	NONE,
	GOOD,
	MEDIUM,
	BAD
};

class View : public UiTypes::ContainerBase<View>
{
  public:
  public:
	View &create(lv_obj_t *parent);
	void update_nav_index(uint8_t index);
	void update_datetime(char *txt);	
	void update_icon_status_ws(UiTypes::StatusIconComponent::StatusType status);
	void update_icon_status_tb(UiTypes::StatusIconComponent::StatusType status);
	void update_icon_status_gps(UiTypes::StatusIconComponent::StatusType status);
	void update_icon_status_wifi(UiTypes::StatusIconComponent::StatusType status, WiFiSignalType signal_type);

  protected:
	View &realign_children();

  private:
	
	UiTypes::LabelComponent _lbl_datetime;
	UiTypes::IconComponent _icon_nav_index_0;
	UiTypes::IconComponent _icon_nav_index_1;
	UiTypes::IconComponent _icon_nav_index_2;
	UiTypes::IconComponent _icon_nav_index_3;
	UiTypes::StatusIconComponent _icon_ws;
	UiTypes::StatusIconComponent _icon_tb;
	UiTypes::StatusIconComponent _icon_gps;
	UiTypes::StatusIconComponent _icon_wifi;

	const uint16_t _nav_bullet_width = 15;
	const uint16_t _nav_bullet_gap = 4;
	const uint16_t _icon_width = 30;
	const uint16_t _icon_gap = 8;
	const uint16_t _left_offset = 6;
	const uint16_t _right_offset = 4;
};

} // namespace UiViewStatusBar

#endif