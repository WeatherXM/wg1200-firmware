#include "ui/views/include/ui_view_statusbar.h"
#include "ui/resources/ui_resources.h"

namespace UiViewStatusBar
{

View &View::create(lv_obj_t *parent)
{
	_parent_lv_obj = parent;
	_self_lv_obj = lv_obj_create(_parent_lv_obj);
	lv_obj_remove_style_all(_self_lv_obj);
	lv_obj_add_style(_self_lv_obj, UiThemeWxm::get_style_obj_basic(), LV_PART_ANY);

	_lbl_datetime.create(_self_lv_obj)
		.style(UiThemeWxm::get_style_heading_5())
		.color(UI_TEXT_COLOR_LIGHT)
		.text("")
		.align(LV_ALIGN_LEFT_MID, 9, 0);
	_icon_nav_index_0.create(_self_lv_obj)
		.source(&UiResources::icon_nav_bullet_bg)
		.align(LV_ALIGN_CENTER,
			   -(_nav_bullet_width + _nav_bullet_gap + _nav_bullet_width / 2 + _nav_bullet_gap / 2) - 1, 0);
	_icon_nav_index_1.create(_self_lv_obj)
		.source(&UiResources::icon_nav_bullet_bg)
		.align(LV_ALIGN_CENTER, -(_nav_bullet_width / 2 + _nav_bullet_gap / 2) - 1, 0);
	_icon_nav_index_2.create(_self_lv_obj)
		.source(&UiResources::icon_nav_bullet_bg)
		.align(LV_ALIGN_CENTER, (_nav_bullet_width / 2 + _nav_bullet_gap / 2), 0);
	_icon_nav_index_3.create(_self_lv_obj)
		.source(&UiResources::icon_nav_bullet_bg)
		.align(LV_ALIGN_CENTER, (_nav_bullet_width + _nav_bullet_gap + _nav_bullet_width / 2 + _nav_bullet_gap / 2), 0);


	_icon_ws.create(_self_lv_obj)
		.status_config(UiTypes::StatusIconComponent::StatusType::NONE, &UiResources::icon_ws_error,
					   UiThemeWxm::get_color_neg())
		.status_config(UiTypes::StatusIconComponent::StatusType::POS, &UiResources::icon_ws_ok,
					   UiThemeWxm::get_color_pos())
		.status_config(UiTypes::StatusIconComponent::StatusType::NEG, &UiResources::icon_ws_error,
					   UiThemeWxm::get_color_neg())
		.status_config(UiTypes::StatusIconComponent::StatusType::WARN, &UiResources::icon_ws_warning,
					   UiThemeWxm::get_color_warn())
		.status(UiTypes::StatusIconComponent::StatusType::NONE)
		.align(LV_ALIGN_RIGHT_MID, -(83), 0);
	_icon_tb.create(_self_lv_obj)
		.status_config(UiTypes::StatusIconComponent::StatusType::NONE, &UiResources::icon_tb_ok,
					   UiThemeWxm::get_color_neg())
		.status_config(UiTypes::StatusIconComponent::StatusType::POS, &UiResources::icon_tb_ok,
					   UiThemeWxm::get_color_pos())
		.status_config(UiTypes::StatusIconComponent::StatusType::NEG, &UiResources::icon_tb_ok,
					   UiThemeWxm::get_color_neg())
		.status(UiTypes::StatusIconComponent::StatusType::NONE)
		.align(LV_ALIGN_RIGHT_MID, -(55), 0);
	_icon_gps.create(_self_lv_obj)
		.status_config(UiTypes::StatusIconComponent::StatusType::NONE, &UiResources::icon_gps_ok,
					   UiThemeWxm::get_color_neg())
		.status_config(UiTypes::StatusIconComponent::StatusType::POS, &UiResources::icon_gps_ok,
					   UiThemeWxm::get_color_pos())
		.status_config(UiTypes::StatusIconComponent::StatusType::NEG, &UiResources::icon_gps_ok,
					   UiThemeWxm::get_color_neg())
		.status(UiTypes::StatusIconComponent::StatusType::NONE)
		.align(LV_ALIGN_RIGHT_MID, -(31), 0);
	_icon_wifi.create(_self_lv_obj)
		.status_config(UiTypes::StatusIconComponent::StatusType::NONE, &UiResources::icon_wifi_error,
					   UiThemeWxm::get_color_neg())
		.status_config(UiTypes::StatusIconComponent::StatusType::POS, &UiResources::icon_wifi_ok_full,
					   UiThemeWxm::get_color_pos())
		.status_config(UiTypes::StatusIconComponent::StatusType::WARN, &UiResources::icon_wifi_warning,
					   UiThemeWxm::get_color_warn())
		.status_config(UiTypes::StatusIconComponent::StatusType::NEG, &UiResources::icon_wifi_error,
					   UiThemeWxm::get_color_neg())
		.status(UiTypes::StatusIconComponent::StatusType::NONE)
		.align(LV_ALIGN_RIGHT_MID, -(7), 0);

	update_nav_index(1);
	return *this;
}

View &View::realign_children()
{
	_lbl_datetime.realign();
	_icon_nav_index_0.realign();
	_icon_nav_index_1.realign();
	_icon_nav_index_2.realign();
	// _icon_nav_index_3.realign();
	_icon_ws.realign();
	_icon_tb.realign();
	_icon_gps.realign();
	_icon_wifi.realign();
	return *this;
}

void View::update_datetime(char *txt)
{
	_lbl_datetime.text(txt);
}

void View::update_nav_index(uint8_t index)
{
	_icon_nav_index_0.source(&UiResources::icon_nav_bullet_bg);
	_icon_nav_index_1.source(&UiResources::icon_nav_bullet_bg);
	_icon_nav_index_2.source(&UiResources::icon_nav_bullet_bg);
	_icon_nav_index_3.source(&UiResources::icon_nav_bullet_bg);
	_icon_nav_index_0.color(UI_COLOR_PRIMARY_DARK);
	_icon_nav_index_1.color(UI_COLOR_PRIMARY_DARK);
	_icon_nav_index_2.color(UI_COLOR_PRIMARY_DARK);
	_icon_nav_index_3.color(UI_COLOR_PRIMARY_DARK);
	switch(index)
	{
	case 0:
		_icon_nav_index_0.source(&UiResources::icon_nav_bullet);
		_icon_nav_index_0.color(UI_COLOR_SECONDARY_DARK);
		break;
	case 1:
		_icon_nav_index_1.source(&UiResources::icon_nav_bullet);
		_icon_nav_index_1.color(UI_COLOR_SECONDARY_DARK);
		break;
	case 2:
		_icon_nav_index_2.source(&UiResources::icon_nav_bullet);
		_icon_nav_index_2.color(UI_COLOR_SECONDARY_DARK);
		break;
	case 3:
		_icon_nav_index_3.source(&UiResources::icon_nav_bullet);
		_icon_nav_index_3.color(UI_COLOR_SECONDARY_DARK);
		break;
	default:
		_icon_nav_index_1.source(&UiResources::icon_nav_bullet);
		_icon_nav_index_1.color(UI_COLOR_SECONDARY_DARK);
		break;
	}
}

void View::update_icon_status_ws(UiTypes::StatusIconComponent::StatusType status)
{
	_icon_ws.status(status);
}

void View::update_icon_status_tb(UiTypes::StatusIconComponent::StatusType status)
{
	_icon_tb.status(status);
}

void View::update_icon_status_gps(UiTypes::StatusIconComponent::StatusType status)
{
	_icon_gps.status(status);
}

void View::update_icon_status_wifi(UiTypes::StatusIconComponent::StatusType status, WiFiSignalType signal_type)
{
	_icon_wifi.status(status);
	if(UiTypes::StatusIconComponent::StatusType::POS == status)
	{
		if(WiFiSignalType::MEDIUM == signal_type)
		{
			// TODO:
			// _icon_wifi.status_config(UiTypes::StatusIconComponent::StatusType::POS,
			// img).status(UiTypes::StatusIconComponent::StatusType::POS);
		}
		else if(WiFiSignalType::BAD == signal_type)
		{
			// TODO:
			// _icon_wifi.status_config(UiTypes::StatusIconComponent::StatusType::POS,
			// img).status(UiTypes::StatusIconComponent::StatusType::POS);
		}
	}
}

} // namespace UiViewStatusBar