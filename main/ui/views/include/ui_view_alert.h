#ifndef UI_VIEW_ALERT_H
#define UI_VIEW_ALERT_H

#include "ui/include/ui_common.h"

namespace UiViewAlert
{

class View : public UiTypes::ContainerBase<View>
{
public:
	enum class AlertType
	{
		NONE = 0,
		INFO,
		WARNING,
		ERROR
	};

	View &create(lv_obj_t *parent);
	View &type(AlertType type);
	View &icon(const void *src_icon);
	View &title(const char *title);
	View &msg(const char *msg);
	View &action_btn(const char *label, void (*cb)(void));
	View &show();
	View &hide();
	bool is_shown();
	View &show_every_s(uint32_t seconds);
	uint32_t get_last_shown_ts();
	void set_last_shown_ts(uint32_t ts);
	uint32_t get_show_every_s();
	AlertType get_type();
	lv_color_t get_color();
	bool is_time_to_show();

protected:
	View &realign_children();

private:
	AlertType _type = AlertType::NONE;
	uint32_t _last_time_shown_ts = 0;
	uint32_t _show_every_s = 0;
	UiTypes::IconComponent _icon;
	UiTypes::LabelComponent _lbl_title;
	UiTypes::LabelComponent _lbl_msg;
	UiTypes::ButtonComponent _button_x;
	UiTypes::ButtonComponent _button_action;
	void (*_action_cb)(void) = nullptr;
	lv_color_t _color;
};

} // namespace UiViewAlert

#endif