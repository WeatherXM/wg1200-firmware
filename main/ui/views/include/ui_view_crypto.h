#ifndef UI_VIEW_CRYPTO_H
#define UI_VIEW_CRYPTO_H

#include "ui/include/ui_common.h"

namespace UiViewCrypto
{

class View : public UiTypes::ContainerBase<View>
{
public:
	View &create(lv_obj_t *parent);
	void update_temp(float temp, char *units);
	void update_feels_like(float feels_like, char *units);
	void update_qod(uint8_t qod_score);
	void update_pol(const char *pol_title, char *pol);
	void update_hum(float hum);
	void update_wind_spd(float wind_spd);
	void update_wind_units(char *units);
	void update_precip(float daily, float rate);
	void update_precip_rate(float rate);
	void update_precip_units(char *units);
	void update_dew(float dew);
	void update_dew_units(char *units);
	void update_con_status(char *status, lv_color_t *color);
	void update_rewards(float total, float last);
	void update_device_name(const char *name);
	void set_last_update(char *date);
	void set_pending_state(bool state);
	uint32_t get_last_rewards_update_ts();
	void set_last_rewards_update_ts(uint32_t ts);
	uint32_t get_last_dev_info_update_ts();
	void set_last_dev_info_update_ts(uint32_t ts);
	void empty_weather_content();
protected:
	View &realign_children();

private:
	UiTypes::ContainerComponent _cont_weather_summary;	
	UiTypes::LabelComponent _lbl_temp;
	UiTypes::LabelComponent _lbl_feels_like_title;
	UiTypes::LabelComponent _lbl_feels_like_value;
	UiTypes::IconComponent _icon_hum;
	UiTypes::LabelComponent _lbl_hum_value;
	UiTypes::LabelComponent _lbl_hum_units;
	UiTypes::IconComponent _icon_wind;
	UiTypes::LabelComponent _lbl_wind_value;
	UiTypes::LabelComponent _lbl_wind_units;
	UiTypes::IconComponent _icon_precip;
	UiTypes::LabelComponent _lbl_precip_value;
	UiTypes::LabelComponent _lbl_precip_units;
	UiTypes::IconComponent _icon_dew;
	UiTypes::LabelComponent _lbl_dew_value;
	UiTypes::LabelComponent _lbl_dew_units;

	UiTypes::ContainerComponent _cont_rewards_summary;
	UiTypes::ContainerComponent _cont_qod;
	UiTypes::LabelComponent _lbl_qod_title;
	UiTypes::LabelComponent _lbl_qod_val;
	UiTypes::LabelComponent _lbl_qod_units;
	UiTypes::ContainerComponent _cont_pol;
	UiTypes::LabelComponent _lbl_pol_title;
	UiTypes::LabelComponent _lbl_pol_address;
	UiTypes::ContainerComponent _cont_last_reward;
	UiTypes::LabelComponent _lbl_last_reward_title;
	UiTypes::LabelComponent _lbl_last_reward_val;
	UiTypes::LabelComponent _lbl_last_reward_units;
	UiTypes::IconComponent _icon_rocket;
	UiTypes::ContainerComponent _cont_total_rewards;
	UiTypes::LabelComponent _lbl_total_rewards_title;
	UiTypes::LabelComponent _lbl_total_rewards_val;
	UiTypes::LabelComponent _lbl_total_rewards_units;

	UiTypes::ContainerComponent _cont_device_name;
	UiTypes::LabelComponent _lbl_device_name_title;
	UiTypes::LabelComponent _lbl_device_name_val;

	uint32_t _last_rewards_update_ts = 0;
	uint32_t _last_dev_info_update_ts = 0;
};

} // namespace UiViewCrypto

#endif

