#include "ui/views/include/ui_view_settings.h"
#include "const.h"
#include "ui/resources/ui_resources.h"
namespace UiViewSettings
{

const int SECTION_WIDTH = UI_SCREEN_WIDTH - 2*UI_ITEM_GAP;
const int ITEM_HEIGHT = 62;

static const char *timezone_opts = "-12:00 GMT\n-11:00 GMT\n-10:00 GMT\n-9:00 GMT\n-8:00 GMT\n-7:00 GMT\n-6:00 "
								   "GMT\n-5:00 GMT\n-4:00 GMT\n-3:00 GMT\n-2:00 GMT\n-1:00 GMT\n+0:00 GMT\n"
								   "+1:00 GMT\n+2:00 GMT\n+3:00 GMT\n+4:00 GMT\n+5:00 GMT\n+6:00 GMT\n+7:00 GMT\n+8:00 "
								   "GMT\n+9:00 GMT\n+10:00 GMT\n+11:00 GMT\n+12:00 GMT\n+13:00 GMT\n+14:00 GMT";

View &View::create(lv_obj_t *parent)
{
	_parent_lv_obj = parent;
	_self_lv_obj = lv_obj_create(_parent_lv_obj);
	lv_obj_remove_style_all(_self_lv_obj);
	lv_obj_add_style(_self_lv_obj, UiThemeWxm::get_style_obj_basic(), LV_PART_ANY);

	// System section
	_section_system.create(_self_lv_obj)
		.size(SECTION_WIDTH, (5 * 62 + 30))
		.align(LV_ALIGN_TOP_MID, 0, 0)
		.set_title("System");	
	_button_about.create(_section_system.self_lv_obj())
		.size(SECTION_WIDTH, ITEM_HEIGHT)		
		.set_title("About")
		.set_icon_source(&UiResources::icon_settings_about)
		.set_cb([](lv_event_t *event) {
			if(lv_event_get_code(event) != LV_EVENT_CLICKED)
			{
				return;
			}
			lv_obj_t *tabview =  (lv_obj_t*) lv_event_get_user_data(event);
			lv_obj_scroll_by(tabview, 0, -3*(4 * 62 + 30 + 6) - 62, LV_ANIM_ON);
		}, parent)
		.align(LV_ALIGN_TOP_MID, 0, 30);
	lv_label_set_long_mode(_button_about.title_lbl().self_lv_obj(), LV_LABEL_LONG_WRAP);
	lv_obj_set_width(_button_about.title_lbl().self_lv_obj(), SECTION_WIDTH - 70 - 10);
	_last_about_ip[0] = '\0';
	_button_wifi_config.create(_section_system.self_lv_obj())
		.size(SECTION_WIDTH, ITEM_HEIGHT)
		.set_title("Configure WiFi")
		.set_icon_source(&UiResources::icon_settings_wifi)
		.enable_top_separator()
		.align_to(_button_about.self_lv_obj(), LV_ALIGN_OUT_BOTTOM_MID, 0, 0);
	_button_claim.create(_section_system.self_lv_obj())
		.size(SECTION_WIDTH, ITEM_HEIGHT)
		.set_title("Claim Device")
		.set_icon_source(&UiResources::icon_settings_claim)
		.enable_top_separator()
		.align_to(_button_wifi_config.self_lv_obj(), LV_ALIGN_OUT_BOTTOM_MID, 0, 0);
	_dd_timezone.create(_section_system.self_lv_obj())
		.size(SECTION_WIDTH, ITEM_HEIGHT)
		.set_title("Time Zone")
		.set_icon_source(&UiResources::icon_settings_timezone)
		.set_options(timezone_opts)
		.enable_top_separator()
		.align_to(_button_claim.self_lv_obj(), LV_ALIGN_OUT_BOTTOM_MID, 0, 0);	
	_dd_screen_dim.create(_section_system.self_lv_obj())
		.size(SECTION_WIDTH, ITEM_HEIGHT)
		.set_title("Screen Brightness")
		.set_icon_source(&UiResources::icon_settings_screen_dim)
		.set_options(IDLE_BACKLIGHT_LEVEL_DROPDOWN_ITEMS)
		.enable_top_separator()
		.align_to(_dd_timezone.self_lv_obj(), LV_ALIGN_OUT_BOTTOM_MID, 0, 0);

	// Weather Station section
	_section_weather_station.create(_self_lv_obj)
		.size(SECTION_WIDTH, (4 * 62 + 30))
		.align_to(_section_system.self_lv_obj(), LV_ALIGN_OUT_BOTTOM_MID, 0, 6)
		.set_title("Weather Station");
	_dd_ws_type.create(_section_weather_station.self_lv_obj())
		.size(SECTION_WIDTH, ITEM_HEIGHT)
		.set_title("Type")
		.set_icon_source(&UiResources::icon_settings_ws_type)
		.set_options(WS_TYPE_DROPDOWN_ITEMS)
		.align(LV_ALIGN_TOP_MID, 0, 30)
		.hidden(true)
		.clickable(false);
	_dd_ws_freq.create(_section_weather_station.self_lv_obj())
		.size(SECTION_WIDTH, ITEM_HEIGHT)
		.set_title("Frequency")
		.set_icon_source(&UiResources::icon_settings_frequency)
		.set_options(FREQ_DROPDOWN_ITEMS)
		// .enable_top_separator()
		.hidden(true)
		.clickable(false)
		.align_to(_dd_ws_type.self_lv_obj(), LV_ALIGN_OUT_BOTTOM_MID, 0, 0);
	_info_ws_status.create(_section_weather_station.self_lv_obj())
		.size(SECTION_WIDTH, ITEM_HEIGHT)
		.set_title("Status")
		.set_icon_source(&UiResources::icon_settings_ws_status)
		.set_info("Battery Level: --, Reception: --")
		// .enable_top_separator()
		.hidden(true)
		.align_to(_dd_ws_freq.self_lv_obj(), LV_ALIGN_OUT_BOTTOM_MID, 0, 0);
	_button_unpair.create(_section_weather_station.self_lv_obj())
		.size(SECTION_WIDTH, ITEM_HEIGHT)
		.set_title("Not paired")
		.clickable(false)
		.set_icon_source(&UiResources::icon_settings_unpair)
		.hidden(true)
		// .enable_top_separator()
		.align_to(_info_ws_status.self_lv_obj(), LV_ALIGN_OUT_BOTTOM_MID, 0, 0);
	_cont_ws_sync_section.create(_section_weather_station.self_lv_obj())
		.size(300, 170)
		.color_bg(UI_COLOR_PRIMARY_DARK)
		// .border_color(UI_COLOR_SECONDARY_DARK)
		// .border_width(2)
		.radius(9)
		.align(LV_ALIGN_CENTER, 0, 0);
	_lbl_ws_sync_section_title.create(_cont_ws_sync_section.self_lv_obj())
		.style(UiThemeWxm::get_style_heading_4())
		.text_align(LV_TEXT_ALIGN_CENTER)
		.text("Settings not synced!")
		.align(LV_ALIGN_TOP_MID, 0, 15);
	_icon_ws_sync_section_icon.create(_cont_ws_sync_section.self_lv_obj())		
		.source(&UiResources::icon_settings_ws_sync_wrn)
		.align(LV_ALIGN_CENTER, 0, -5);
	_lbl_ws_sync_section_msg.create(_cont_ws_sync_section.self_lv_obj())
		.font(&lv_font_montserrat_14)
		.color(UI_COLOR_SECONDARY_DARK)
		.text_align(LV_TEXT_ALIGN_CENTER)
		.text("Connect to the internet and wait for the settings ti sync.")
		.align(LV_ALIGN_BOTTOM_MID, 0, -15);

	// Units section
	_section_units.create(_self_lv_obj)
		.size(SECTION_WIDTH, (4 * 62 + 30))
		.align_to(_section_weather_station.self_lv_obj(), LV_ALIGN_OUT_BOTTOM_MID, 0, 6)
		.set_title("Units");
	_dd_units_temperature.create(_section_units.self_lv_obj())
		.size(SECTION_WIDTH, ITEM_HEIGHT)
		.set_title("Temperature")
		.set_icon_source(&UiResources::icon_settings_temp)
		.set_options(TEMP_UNITS_DROPDOWN_ITEMS)
		.align(LV_ALIGN_TOP_MID, 0, 30);
	_dd_units_precipitation.create(_section_units.self_lv_obj())
		.size(SECTION_WIDTH, ITEM_HEIGHT)
		.set_title("Precipitation")
		.set_icon_source(&UiResources::icon_settings_precip)
		.set_options(PRECIP_UNITS_DROPDOWN_ITEMS)
		.enable_top_separator()
		.align_to(_dd_units_temperature.self_lv_obj(), LV_ALIGN_OUT_BOTTOM_MID, 0, 0);
	_dd_units_wind_speed.create(_section_units.self_lv_obj())
		.size(SECTION_WIDTH, ITEM_HEIGHT)
		.set_title("Wind Speed")
		.set_icon_source(&UiResources::icon_settings_wind)
		.set_options(WIND_SPEED_UNITS_DROPDOWN_ITEMS)
		.enable_top_separator()
		.align_to(_dd_units_precipitation.self_lv_obj(), LV_ALIGN_OUT_BOTTOM_MID, 0, 0);
	_dd_units_pressure.create(_section_units.self_lv_obj())
		.size(SECTION_WIDTH, ITEM_HEIGHT)
		.set_title("Pressure")
		.set_icon_source(&UiResources::icon_settings_pressure)
		.set_options(PRESSURE_UNITS_DROPDOWN_ITEMS)
		.enable_top_separator()
		.align_to(_dd_units_wind_speed.self_lv_obj(), LV_ALIGN_OUT_BOTTOM_MID, 0, 0);

	// About section
	_section_about.create(_self_lv_obj)
		.size(SECTION_WIDTH, 506)
		.align_to(_section_units.self_lv_obj(), LV_ALIGN_OUT_BOTTOM_MID, 0, 6)
		.set_title("About");
	_info_sn.create(_section_about.self_lv_obj())
		.size(SECTION_WIDTH, ITEM_HEIGHT)
		.set_title("Serial Number")
		.set_icon_source(&UiResources::icon_settings_serial)
		.set_info("--")
		.align(LV_ALIGN_TOP_MID, 0, 30);
	_info_dev_name.create(_section_about.self_lv_obj())
		.size(SECTION_WIDTH, ITEM_HEIGHT)
		.set_title("Device Name")
		.set_icon_source(&UiResources::icon_settings_about)
		.set_info("--")
		.enable_top_separator()
		.align_to(_info_sn.self_lv_obj(), LV_ALIGN_OUT_BOTTOM_MID, 0, 0);
	_info_fw_ver.create(_section_about.self_lv_obj())
		.size(SECTION_WIDTH, ITEM_HEIGHT)
		.set_title("Firmware Version")
		.set_icon_source(&UiResources::icon_settings_fw)
		.set_info("--")
		.enable_top_separator()
		.align_to(_info_dev_name.self_lv_obj(), LV_ALIGN_OUT_BOTTOM_MID, 0, 0);
	_info_gps.create(_section_about.self_lv_obj())
		.size(SECTION_WIDTH, ITEM_HEIGHT)
		.set_title("GPS Info")
		.set_icon_source(&UiResources::icon_settings_gps)
		.set_info("Lat: --, Lon: --, TS: --")
		.enable_top_separator()
		.align_to(_info_fw_ver.self_lv_obj(), LV_ALIGN_OUT_BOTTOM_MID, 0, 0);
	_info_wifi.create(_section_about.self_lv_obj())
		.size(SECTION_WIDTH, ITEM_HEIGHT)
		.set_title("WiFi Info")
		.set_icon_source(&UiResources::icon_settings_wifi_info)
		.set_info("SSID: --, RSSI: --")
		.enable_top_separator()
		.align_to(_info_gps.self_lv_obj(), LV_ALIGN_OUT_BOTTOM_MID, 0, 0);
	_icon_wxm.create(_self_lv_obj)
		.source(&UiResources::icon_settings_wxm)
		.align_to(_section_about.self_lv_obj(), LV_ALIGN_BOTTOM_MID, 0, -30);


	return *this;
}

View &View::realign_children()
{
	_section_system.realign();
	_button_about.realign();
	_button_wifi_config.realign();
	_dd_timezone.realign();
	_dd_screen_dim.realign();

	_section_weather_station.realign();
	_dd_ws_type.realign();
	_dd_ws_freq.realign();
	_info_ws_status.realign();
	_button_unpair.realign();
	_cont_ws_sync_section.realign();
	_lbl_ws_sync_section_title.realign();
	_icon_ws_sync_section_icon.realign();
	_lbl_ws_sync_section_msg.realign();

	_section_units.realign();
	_dd_units_temperature.realign();
	_dd_units_precipitation.realign();
	_dd_units_wind_speed.realign();
	_dd_units_pressure.realign();

	_section_about.realign();
	_info_sn.realign();
	_info_dev_name.realign();
	_info_fw_ver.realign();
	_info_gps.realign();
	_info_wifi.realign();
	_icon_wxm.realign();

	return *this;
}

void View::set_about_title(const char *ip)
{
	if(ip != nullptr && strlen(ip) > 0 && strcmp(ip, "0.0.0.0") != 0 && strcmp(ip, "--") != 0)
	{
		if(strcmp(_last_about_ip, ip) != 0)
		{
			strncpy(_last_about_ip, ip, sizeof(_last_about_ip) - 1);
			_last_about_ip[sizeof(_last_about_ip) - 1] = '\0';
			char buff[64];
			snprintf(buff, sizeof(buff), "Web UI: %s", ip);
			_button_about.set_title_font(&lv_font_montserrat_18);
			_button_about.set_title(buff);
		}
	}
	else
	{
		if(_last_about_ip[0] != '\0')
		{
			_last_about_ip[0] = '\0';
			_button_about.set_title_font(&lv_font_montserrat_18);
			_button_about.set_title("About");
		}
	}
}


} // namespace UiViewSettings
