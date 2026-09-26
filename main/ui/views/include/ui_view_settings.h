#ifndef UI_VIEW_SETTINGS_H
#define UI_VIEW_SETTINGS_H

#include "ui/include/ui_common.h"

namespace UiViewSettings
{

class View : public UiTypes::ContainerBase<View>
{
  public:
	View &create(lv_obj_t *parent);
	void set_about_title(const char *ip);
	// System section
	UiTypes::SettingsSectionComponent _section_system;
	UiTypes::SettingsButtonComponent _button_about;
	UiTypes::SettingsButtonComponent _button_wifi_config;
	UiTypes::SettingsButtonComponent _button_claim;
	UiTypes::SettingsDropDownComponent _dd_timezone;
	UiTypes::SettingsDropDownComponent _dd_screen_dim;

	// Weather Station section
	UiTypes::SettingsSectionComponent _section_weather_station;
	UiTypes::SettingsDropDownComponent _dd_ws_type;
	UiTypes::SettingsDropDownComponent _dd_ws_freq;
	UiTypes::SettingsInfoComponent _info_ws_status;
	UiTypes::SettingsButtonComponent _button_unpair;
	UiTypes::LabelComponent _lbl_ws_sync_section_title;
	UiTypes::IconComponent _icon_ws_sync_section_icon;
	UiTypes::LabelComponent _lbl_ws_sync_section_msg;
	UiTypes::ContainerComponent _cont_ws_sync_section;

	// Units section
	UiTypes::SettingsSectionComponent _section_units;
	UiTypes::SettingsDropDownComponent _dd_units_temperature;
	UiTypes::SettingsDropDownComponent _dd_units_precipitation;
	UiTypes::SettingsDropDownComponent _dd_units_wind_speed;
	UiTypes::SettingsDropDownComponent _dd_units_pressure;
	

	// About section
	UiTypes::SettingsSectionComponent _section_about;
	UiTypes::SettingsInfoComponent _info_sn;
	UiTypes::SettingsInfoComponent _info_dev_name;
	UiTypes::SettingsInfoComponent _info_fw_ver;
	UiTypes::SettingsInfoComponent _info_gps;
	UiTypes::SettingsInfoComponent _info_wifi;
	UiTypes::IconComponent _icon_wxm;
	
  protected:
	View &realign_children();
	char _last_about_ip[20] = {};
	
};

} // namespace UiViewSettingsNew

#endif
