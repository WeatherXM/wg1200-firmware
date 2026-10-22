#ifndef UI_TYPES_H
#define UI_TYPES_H

#include "ui_common.h"
#include "ui_theme_wxm.h"

namespace UiTypes
{

enum class WeatherIconType
{
	NONE,
	CLEAR_DAY,
	CLEAR_NIGHT,
	OVERCAST_DAY,
	OVERCAST_NIGHT,
	PARTLY_CLOUDY_DAY,
	PARTLY_CLOUDY_NIGHT,
	SLEET,
	RAIN,
	TS_RAIN,
	DRIZZLE,
	SNOW
};

template <class T> class ComponentBase
{
  public:
	virtual T &create(lv_obj_t *parent)
	{
		return (T &)*this;
	}

	T &align(lv_align_t align_type, lv_coord_t x_ofs, lv_coord_t y_ofs)
	{
		lv_obj_align(_self_lv_obj, align_type, x_ofs, y_ofs);
		_align_to_lv_obj = _parent_lv_obj;
		_align_type = align_type;
		_align_x_ofs = x_ofs;
		_align_y_ofs = y_ofs;
		realign_children();
		return (T &)*this;
	}

	T &align_to(lv_obj_t *obj, lv_align_t align_type, lv_coord_t x_ofs, lv_coord_t y_ofs)
	{
		lv_obj_align_to(_self_lv_obj, obj, align_type, x_ofs, y_ofs);
		_align_to_lv_obj = obj;
		_align_type = align_type;
		_align_x_ofs = x_ofs;
		_align_y_ofs = y_ofs;
		realign_children();
		return (T &)*this;
	}

	T &style(lv_style_t *style)
	{
		lv_obj_remove_style_all(_self_lv_obj);
		lv_obj_add_style(_self_lv_obj, style, LV_PART_MAIN);
		return (T &)*this;
	}

	T &size(lv_coord_t width, lv_coord_t height)
	{
		lv_obj_set_size(_self_lv_obj, width, height);
		realign_children();
		return (T &)*this;
	}

	T &inner_padding(int padding)
	{
		lv_obj_set_style_pad_all(_self_lv_obj, padding, LV_PART_MAIN);
		return (T &)*this;
	}

	T &outer_padding(int padding)
	{
		lv_obj_set_style_outline_pad(_self_lv_obj, padding, LV_PART_ANY);
		return (T &)*this;
	}

	T &clickable(bool flag)
	{
		if(flag)
		{
			lv_obj_add_flag(ComponentBase<T>::_self_lv_obj, LV_OBJ_FLAG_CLICKABLE);
		}
		else
		{
			lv_obj_clear_flag(ComponentBase<T>::_self_lv_obj, LV_OBJ_FLAG_CLICKABLE);
		}
		return (T &)*this;
	}

	T &hidden(bool flag)
	{
		if(flag)
		{
			lv_obj_add_flag(ComponentBase<T>::_self_lv_obj, LV_OBJ_FLAG_HIDDEN);
		}
		else
		{
			lv_obj_clear_flag(ComponentBase<T>::_self_lv_obj, LV_OBJ_FLAG_HIDDEN);
		}
		return (T &)*this;
	}

	T &realign()
	{
		//lv_obj_realign(_self_lv_obj);
		// lv_obj_refr_pos(_self_lv_obj);
		if(_align_to_lv_obj == NULL)
		{
			return (T &)*this;
		}

		if(_align_to_lv_obj == _parent_lv_obj)
		{
			lv_obj_align(_self_lv_obj, _align_type, _align_x_ofs, _align_y_ofs);
		}
		else
		{
			lv_obj_align_to(_self_lv_obj, _align_to_lv_obj, _align_type, _align_x_ofs, _align_y_ofs);
		}
		realign_children();
		return (T &)*this;
	}

	T &radius(lv_coord_t radius)
	{
		lv_obj_set_style_radius(_self_lv_obj, radius, LV_PART_MAIN);
		lv_obj_set_style_clip_corner(_self_lv_obj, true, 0);
		return (T &)*this;
	}

	T &border_color(lv_color_t color)
	{
		lv_obj_set_style_border_color(_self_lv_obj, color, LV_PART_MAIN);
		return (T &)*this;
	}

	T &border_width(int width)
	{
		lv_obj_set_style_border_width(_self_lv_obj, width, LV_PART_MAIN);
		return (T &)*this;
	}

	T &set_cb(lv_event_cb_t event_cb, void *user_data)
	{
		lv_obj_add_event_cb(_self_lv_obj, event_cb, LV_EVENT_ALL, user_data);
		return (T &)*this;
	}

	T &color_bg(lv_color_t bg_color)
	{
		lv_obj_set_style_bg_color(ComponentBase<T>::_self_lv_obj, bg_color, 0);
		lv_obj_set_style_bg_opa(ComponentBase<T>::_self_lv_obj, LV_OPA_100, 0);
		return (T &)*this;
	}

	lv_obj_t *self_lv_obj()
	{
		return _self_lv_obj;
	}

	void del_self()
	{
		lv_obj_del(_self_lv_obj);
		_self_lv_obj = NULL;
	}

  protected:
	virtual T &realign_children()
	{
		return (T &)*this;
	}

	lv_obj_t *_align_to_lv_obj = NULL;
	lv_align_t _align_type;
	lv_coord_t _align_x_ofs;
	lv_coord_t _align_y_ofs;
	lv_obj_t *_parent_lv_obj;
	lv_obj_t *_self_lv_obj = NULL;
};

class LabelComponent : public ComponentBase<LabelComponent>
{
  public:
	LabelComponent &create(lv_obj_t *parent);
	LabelComponent &color(lv_color_t color);
	LabelComponent &max_chars(uint8_t max_chars);
	LabelComponent &text(const char *value);
	LabelComponent &font(const lv_font_t *value);
	LabelComponent &text_align(lv_text_align_t align);

  private:
	uint8_t _max_chars;
};

class IconComponent : public ComponentBase<IconComponent>
{
  public:
	IconComponent &create(lv_obj_t *parent);
	IconComponent &source(const void *src_img);
	IconComponent &color(lv_color_t color);
};

class StatusIconComponent : public ComponentBase<StatusIconComponent>
{
  public:
	enum class StatusType
	{
		NONE = 0,
		POS,
		WARN,
		NEG
	};

	StatusIconComponent &create(lv_obj_t *parent);
	StatusIconComponent &status_config(StatusIconComponent::StatusType status, const void *src_img, lv_color_t color);
	StatusIconComponent &status(StatusIconComponent::StatusType status);

  private:
	struct StatusConfig
	{
		const void *src;
		lv_color_t color;
	};
	StatusConfig _config_pos;
	StatusConfig _config_warn;
	StatusConfig _config_neg;
	StatusConfig _config_default;
};

class TabViewComponent : public ComponentBase<TabViewComponent>
{
  public:
	TabViewComponent &create(lv_obj_t *parent);
	TabViewComponent &add_tab(lv_obj_t **tab, const char *name);
	TabViewComponent &sel_tab(int tab_index);
	int get_active_tab_index();
};

template <class T> class ContainerBase : public ComponentBase<T>
{
  public:
	T &layout(uint32_t layout)
	{
		lv_obj_set_layout(ComponentBase<T>::_self_lv_obj, layout);
		return (T &)*this;
	}
};

class ContainerComponent : public ContainerBase<ContainerComponent>
{
  public:
	ContainerComponent &create(lv_obj_t *parent);
};


enum class SimpleTileType
{
	NONE,
	WIND,
	PRECIP_RATE,
	PRECIP_DAILY,
	HUMIDITY,
	UV_INDEX,
	IRRADIANCE,
	DEW_POINT,
	PRESSURE,
	ANY
};

class SimpleTile : public UiTypes::ContainerBase<SimpleTile>
{
  public:
	virtual SimpleTile &create(lv_obj_t *parent);
	SimpleTile &set_type(SimpleTileType tile_type);
	SimpleTileType get_type();
	SimpleTile &title(const char *title);
	SimpleTile &title_value(char *value);
	SimpleTile &value(char *value);
	SimpleTile &units(char *units);
	SimpleTile &title_font(const lv_font_t *font);
	SimpleTile &title_align(lv_align_t align, lv_coord_t x_ofs, lv_coord_t y_ofs);
	SimpleTile &units_font(const lv_font_t *font);
	SimpleTile &units_align(lv_align_t align, lv_coord_t x_ofs, lv_coord_t y_ofs);
	static void update_per_type(SimpleTile* array[],
								uint16_t array_length,
								UiTypes::SimpleTileType type,
								char *title_value,
								char *value,
								char *units);
	using ContainerBase::size;

  protected:
	UiTypes::LabelComponent _lbl_title;
	UiTypes::LabelComponent _lbl_title_value;
	UiTypes::LabelComponent _lbl_value;
	UiTypes::LabelComponent _lbl_units;
	SimpleTileType _tile_type;
	SimpleTile &realign_children();
	void realign_middle_row();
};

class TempBigTile : public UiTypes::ContainerBase<TempBigTile>
{
  public:
	TempBigTile &create(lv_obj_t *parent);
	TempBigTile &feels_like(char *feels_like);
	TempBigTile &temp(char *value);
	TempBigTile &dew(char *dew);
	TempBigTile &icon(UiTypes::WeatherIconType icon_type);

  protected:
	UiTypes::LabelComponent _lbl_temp;
	UiTypes::LabelComponent _lbl_feels_like;
	UiTypes::LabelComponent _lbl_feels_like_title;
	UiTypes::LabelComponent _lbl_dew;
	UiTypes::LabelComponent _lbl_dew_title;
	UiTypes::IconComponent _icon_weather_icon;
	TempBigTile &realign_children();
};

class TempDualTile : public UiTypes::ContainerBase<TempDualTile>
{
  public:
	TempDualTile &create(lv_obj_t *parent);
	TempDualTile &feels_like(char *feels_like);
	TempDualTile &temp(char *value);
	TempDualTile &min_max(const char *min_max);
	TempDualTile &add_temp_data(float temp);
	TempDualTile &add_feels_like_data(float feels_like);

  protected:
	UiTypes::LabelComponent _lbl_temp;
	UiTypes::LabelComponent _lbl_feels_like;
	UiTypes::LabelComponent _lbl_feels_like_title;
	UiTypes::LabelComponent _lbl_min_max;
	UiTypes::LabelComponent _lbl_chart_max;
	UiTypes::LabelComponent _lbl_chart_mid;
	UiTypes::LabelComponent _lbl_chart_min;

	lv_obj_t *_chart;
	lv_chart_series_t *_ser_temp;
	lv_chart_series_t *_ser_feels_like;
	lv_timer_t *_timer;

	float _last_temp;
	float _last_feels_like;
	bool _temp_received;
	bool _feels_like_received;

	float _temp_history[120];
	float _feels_like_history[120];
	bool _has_history;
	bool _is_fahrenheit;

	void tick_chart();
	static void timer_cb(lv_timer_t *timer);

	TempDualTile &realign_children();
};

class WindDualTile : public UiTypes::ContainerBase<WindDualTile>
{
  public:
	WindDualTile &create(lv_obj_t *parent);
	WindDualTile &direction(const char *direction);
	WindDualTile &peak_gust(const char *peak_gust);
	WindDualTile &speed_and_gust(const char *speed_and_gust);
	WindDualTile &units(const char *units);
	WindDualTile &add_chart_data(float speed, float gust);
	void tick_chart();
	static void timer_cb(lv_timer_t *timer);

  protected:
	UiTypes::LabelComponent _lbl_title_wind;
	UiTypes::LabelComponent _lbl_title_separator;
	UiTypes::LabelComponent _lbl_title_gust;
	UiTypes::LabelComponent _lbl_direction_title;
	UiTypes::LabelComponent _lbl_direction;
	UiTypes::LabelComponent _lbl_peak_gust;
	UiTypes::LabelComponent _lbl_speed_and_gust;
	UiTypes::LabelComponent _lbl_units;
	WindDualTile &realign_children();

  private:
	lv_obj_t *_chart;
	lv_chart_series_t *_ser_speed;
	lv_chart_series_t *_ser_gust;
	lv_timer_t *_timer;
	UiTypes::LabelComponent _lbl_chart_max;
	UiTypes::LabelComponent _lbl_chart_mid;
	UiTypes::LabelComponent _lbl_chart_min;
	float _last_speed;
	float _last_gust;
	bool _packet_received;
	char _last_units[16];
	float _speed_history[120];
	float _gust_history[120];
	bool _has_history;
};

template <class T> class SettingsItemBase : public ComponentBase<T>
{
	public:
	T &create(lv_obj_t *parent)
	{
		ComponentBase<T>::_self_lv_obj = lv_obj_create(parent);
		lv_obj_remove_style_all(ComponentBase<T>::_self_lv_obj);
		_icon.create(ComponentBase<T>::_self_lv_obj).align(LV_ALIGN_LEFT_MID, 18, 0);
		_lbl_title.create(ComponentBase<T>::_self_lv_obj)
			.style(UiThemeWxm::get_style_heading_4())
			.align_to(ComponentBase<T>::_self_lv_obj, LV_ALIGN_LEFT_MID, TITLE_X_OFF, TITLE_Y_OFF);
		ComponentBase<T>::clickable(true);

		lv_obj_set_style_bg_color(ComponentBase<T>::_self_lv_obj, lv_color_black(), LV_STATE_PRESSED);
		lv_obj_set_style_bg_opa(ComponentBase<T>::_self_lv_obj, LV_OPA_10, LV_STATE_PRESSED);

		lv_obj_set_style_bg_color(ComponentBase<T>::_self_lv_obj, lv_color_black(), LV_STATE_DEFAULT);
		lv_obj_set_style_bg_opa(ComponentBase<T>::_self_lv_obj, LV_OPA_0, LV_STATE_DEFAULT);

		_top_separator = lv_obj_create(ComponentBase<T>::_self_lv_obj);
		lv_obj_set_style_bg_color(_top_separator, UI_COLOR_BG_DARK, 0);
		lv_obj_set_size(_top_separator, lv_obj_get_width(ComponentBase<T>::_self_lv_obj) - 100, 2);
		lv_obj_align(_top_separator, LV_ALIGN_TOP_LEFT, TITLE_X_OFF, 0);
		lv_obj_add_flag(_top_separator, LV_OBJ_FLAG_HIDDEN);

		return (T &)*this;
	}

	T &enable_top_separator()
	{
		lv_obj_clear_flag(_top_separator, LV_OBJ_FLAG_HIDDEN);
		return (T &)*this;
	}

	T &set_title(const char *value)
	{
		_lbl_title.text(value);
		return (T &)*this;
	}

	T &set_title_font(const lv_font_t *font)
	{
		_lbl_title.font(font);
		_lbl_title.realign();
		return (T &)*this;
	}

	LabelComponent &title_lbl()
	{
		return _lbl_title;
	}

	T &set_icon_source(const void *src_img)
	{
		_icon.source(src_img);
		return (T &)*this;
	}

  protected:
	int TITLE_Y_OFF = 0;
	const int TITLE_X_OFF = 70;
	LabelComponent _lbl_title;
	IconComponent _icon;
	lv_obj_t *_top_separator;

	T &realign_children()
	{
		_lbl_title.realign();
		_icon.realign();
		lv_obj_set_size(_top_separator, lv_obj_get_width(ComponentBase<T>::_self_lv_obj) - 100, 2);
		lv_obj_align(_top_separator, LV_ALIGN_TOP_LEFT, TITLE_X_OFF, 0);
		return (T &)*this;
	}

};

class SettingsButtonComponent : public SettingsItemBase<SettingsButtonComponent>
{
  public:
	SettingsButtonComponent &create(lv_obj_t *parent);

};

class SettingsInfoComponent : public SettingsItemBase<SettingsInfoComponent>
{
  public:
	SettingsInfoComponent &create(lv_obj_t *parent);
	SettingsInfoComponent &set_info(char *info);

  private:
	LabelComponent _lbl_info;
	SettingsInfoComponent &realign_children();
};

class SettingsDropDownComponent : public SettingsItemBase<SettingsDropDownComponent>
{
  public:
	SettingsDropDownComponent &create(lv_obj_t *parent);
	SettingsDropDownComponent &set_options(const char *options);
	SettingsDropDownComponent &set_display_text(const char *text);
	SettingsDropDownComponent &clear_display_text();
	int get_item_index(const char *item_text);
	void set_selected_text(const char *item_text);
	SettingsDropDownComponent &set_selected(uint16_t sel_opt);
	SettingsDropDownComponent &set_cb(lv_event_cb_t event_cb, void *user_data);

  private:
	lv_obj_t * _lv_dropdown;
	SettingsDropDownComponent &realign_children();
};

class SettingsSectionComponent : public ComponentBase<SettingsSectionComponent>
{
  public:
	SettingsSectionComponent &create(lv_obj_t *parent);
	SettingsSectionComponent &set_title(const char *title);

  private:
	LabelComponent _lbl_info;
	SettingsSectionComponent &realign_children();
};

class ButtonComponent : public ComponentBase<ButtonComponent>
{
  public:
	ButtonComponent &create(lv_obj_t *parent);
	ButtonComponent &title(char *text);
	ButtonComponent &color_bg_clicked(lv_color_t color);
	ButtonComponent &color_title(lv_color_t color);
	ButtonComponent &icon(const void *icon_src);

  private:
	LabelComponent _lbl_title;
	IconComponent _icon;
	ButtonComponent &realign_children();
};

class QrCodeComponent : public ComponentBase<QrCodeComponent>
{
  public:
	QrCodeComponent &create(lv_obj_t *parent);
	QrCodeComponent &data(char *data, size_t data_length);

  private:
	QrCodeComponent &realign_children();
};

class SpinnerComponent : public ComponentBase<SpinnerComponent>
{
  public:
	SpinnerComponent &create(lv_obj_t *parent);
	SpinnerComponent& color_arc_bg(lv_color_t color);
	SpinnerComponent& color_arc_line(lv_color_t color);

  private:
	SpinnerComponent &realign_children();
};

} // namespace UiTypes

#endif
