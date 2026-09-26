#ifndef UI_VIEW_SNIFFER_H
#define UI_VIEW_SNIFFER_H

#include "ui/include/ui_common.h"
#include "ui/views/include/ui_view_crypto.h"
#include "ui/views/include/ui_view_detailed.h"
#include "ui/views/include/ui_view_settings.h"
#include "ui/views/include/ui_view_settings.h"
#include "ui/views/include/ui_view_statusbar.h"
#include "ui/views/include/ui_view_tiles.h"

namespace WsData {
struct DailyExtremes;
}

namespace UiViewSniffer
{

class View : public UiTypes::ContainerBase<View>
{
  public:
	View &create(lv_obj_t *parent);
	void update_temp(float temp, char *units);
	void update_feels_like(float feels_like, char *units);
	void update_icon(UiTypes::WeatherIconType icon);
	void update_precip(float daily, float rate);
	void update_precip_rate(float rate);
	void update_precip_units(char *units);
	void update_daily_extremes(const WsData::DailyExtremes &extremes);
	void update_wind_units(char *units);
	void update_wind_dir(uint16_t wind_dir);
	void update_wind_speed_and_gust(float wind_spd, float wind_gust);
	void update_humidity(float hum);
	void update_dew(float dew);
	void update_dew_units(char *units);
	void update_pressure(float press);
	void update_pressure_units(char *units);
	void update_irradiance(float irr);
	void update_uv(uint8_t uv);
	void empty_weather_content();

	void set_ws_pair_info(bool is_paired, uint32_t id);
	void set_ws_pair_info(bool is_paired, uint8_t* sn, size_t sn_len);
	void enable_ws_info(bool freq_is_selectable);
	bool is_ws_info_enabled();
	void set_ws_status(bool is_battery_low, uint8_t reception_perc);
	void set_station_radio_info(const char *frequency, const char *power,
		int16_t rssi, int8_t snr, bool signal_available);
	void empty_ws_status();

	void set_wifi_info(char *ssid, int rssi);
	void empty_wifi_info();

	void set_gps_info(float lan, float lon, char * time_fmt);
	void empty_gps_info();

	void set_fw_ver_info(char *fw_ver);

	void set_serial_info(char *serial);

	void set_device_name_info(const char *device_name);

	void set_act_tab(uint16_t tab_index);
	lv_obj_t * get_popups_layer();
	void set_background_touch(bool flag);

	UiViewTiles::View _view_tiles;
	UiViewDetailed::View _view_detailed;
	UiViewCrypto::View _view_crypto;
	UiViewSettings::View _view_settings;
	UiViewStatusBar::View _view_statusbar;
	UiTypes::TabViewComponent _tabview_main;	

  protected:
	View &realign_children();

  private:		
	const char *_STR_SETTINGS_TAB = "TAB_SETTINGS";
	const char *_STR_TILES_TAB = "TAB_TILES";
	const char *_STR_DETAILED_TAB = "TAB_DETAILED";
	const char *_STR_CRYPTO_TAB = "TAB_CRYPTO";
	lv_obj_t *_tab_tiles;
	lv_obj_t *_tab_detailed;
	lv_obj_t *_tab_crypto;
	lv_obj_t *_tab_settings;

	lv_obj_t *_layer_popups;
	float _last_dew;
	char _last_dew_units[8];
	char _station_radio_info[80] = {};
};

} // namespace UiViewSniffer

#endif
