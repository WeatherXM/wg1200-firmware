#include "ui/views/include/ui_view_alert.h"
#include "ui/resources/ui_resources.h"
#include <time.h>

namespace UiViewAlert
{

uint8_t _alerts_shown = 0;

/******************************************************************************
 * @brief Create component.
 * @param alert Alert view.
 * @param show True/False.
 ******************************************************************************/
void set_alert_state(UiViewAlert::View* alert, bool show)
{
	// When multiple alerts are shown, we need to track how many there are on screen.
	// When the last one is closed, we enable background touch again.
	lv_obj_t *popup_layer = lv_obj_get_parent(alert->self_lv_obj());
	if(show)
	{
		_alerts_shown++;

		lv_obj_add_flag(popup_layer, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_clear_flag(alert->self_lv_obj(), LV_OBJ_FLAG_HIDDEN);
	}
	else
	{
		if(_alerts_shown > 0)
		{
			_alerts_shown--;
		}
		if(_alerts_shown == 0)
		{
			lv_obj_clear_flag(popup_layer, LV_OBJ_FLAG_CLICKABLE);
		}
		lv_obj_add_flag(alert->self_lv_obj(), LV_OBJ_FLAG_HIDDEN);
	}
}

/******************************************************************************
 * @brief Create component.
 * @param parent Parent LVGL object.
 * @return Component object.
 ******************************************************************************/
View &View::create(lv_obj_t *parent)
{
	_parent_lv_obj = parent;
	_self_lv_obj = lv_obj_create(_parent_lv_obj);
	lv_obj_remove_style_all(_self_lv_obj);
	lv_obj_add_flag(_self_lv_obj, LV_OBJ_FLAG_HIDDEN);
	lv_obj_clear_flag(_self_lv_obj, LV_OBJ_FLAG_CLICKABLE);
	size(370, 260).border_width(3).radius(UI_ITEM_RADIUS).align(LV_ALIGN_CENTER, 0, 12);	
	color_bg(lv_color_hex(0x3D3125)).border_color(lv_color_hex(0xFFAB49));
	_icon.create(_self_lv_obj).align(LV_ALIGN_TOP_MID, 0, 35);
	_lbl_title
		.create(_self_lv_obj)
		// .text(_title)		
		.style(UiThemeWxm::get_style_heading_2())
		.text_align(LV_TEXT_ALIGN_CENTER)
		.color(lv_color_hex(0xFEFBFF))
		.align(LV_ALIGN_TOP_MID, 0, 105);
	_lbl_msg
		.create(_self_lv_obj)
		// .text(_msg)		
		.style(UiThemeWxm::get_style_heading_5())
		.text_align(LV_TEXT_ALIGN_CENTER)
		.color(lv_color_hex(0xFEFBFF))
		.align_to(_lbl_title.self_lv_obj(), LV_ALIGN_OUT_BOTTOM_MID, 0, 7);
	lv_label_set_long_mode(_lbl_msg.self_lv_obj(), LV_LABEL_LONG_WRAP);
	lv_obj_set_width(_lbl_msg.self_lv_obj(), 340);
	_button_x.create(_self_lv_obj)		
		.icon(&UiResources::icon_alert_close)
		.size(70, 70)
		// .border_width(1)
		// .border_color(lv_color_white())
		.set_cb([](lv_event_t *event) {
			lv_event_code_t  event_code = lv_event_get_code(event);		
			UiViewAlert::View *alert_obj =  (UiViewAlert::View*) lv_event_get_user_data(event);
			if(event_code != LV_EVENT_CLICKED)
			{
				return;
			}
			alert_obj->set_last_shown_ts(UiCommon::get_seconds_count());
			set_alert_state(alert_obj, false);
		}, this)
		.align(LV_ALIGN_TOP_RIGHT, 0, 0);
	_button_action.create(_self_lv_obj)
		.size(165, 40)
		.radius(8)
		.color_bg(lv_color_hex(0x0D47A1))
		.color_bg_clicked(lv_color_hex(0x1565C0))
		.border_width(1)
		.border_color(lv_color_hex(0x64B5F6))
		.color_title(lv_color_hex(0xFEFBFF))
		.title((char *)"Mount SD")
		.set_cb([](lv_event_t *event) {
			if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
				return;
			}
			UiViewAlert::View *alert_obj = (UiViewAlert::View *)lv_event_get_user_data(event);
			set_alert_state(alert_obj, false);
			if (alert_obj->_action_cb != nullptr) {
				alert_obj->_action_cb();
			}
		}, this)
		.align(LV_ALIGN_BOTTOM_MID, 0, -14);
	lv_obj_add_flag(_button_action.self_lv_obj(), LV_OBJ_FLAG_HIDDEN);
	return *this;
}

/******************************************************************************
 * @brief Select type of the alert component.
 * @param type Type of the alert component. NONE, INFO, WARNING, ERROR.
 * @return Component object.
 ******************************************************************************/
View &View::type(AlertType type)
{
	_type = type;
	switch(_type)
	{
	case AlertType::INFO:
		color_bg(lv_color_hex(0x31364A)).border_color(UI_COLOR_SECONDARY_DARK);
		_color = lv_color_hex(0x31364A);
		break;
	case AlertType::WARNING:
		color_bg(lv_color_hex(0x3D3125)).border_color(lv_color_hex(0xFFAB49));
		_icon.color(lv_color_hex(0xFFAB49));
		_color = lv_color_hex(0x3D3125);
		break;
	case AlertType::ERROR:
		color_bg(lv_color_hex(0x3D1A25)).border_color(lv_color_hex(0xFF1744));
		_icon.color(lv_color_hex(0xFF1744));
		_color = lv_color_hex(0x3D1A25);
		break;
	default:
		color_bg(lv_color_hex(0x3D3125)).border_color(lv_color_hex(0xFFAB49));
		_icon.color(lv_color_hex(0xFFAB49));
		_color = lv_color_hex(0x3D3125);
		break;
	}
	return *this;
}

/******************************************************************************
 * @brief Set icon source of the alert component.
 * @param src_icon Pointer to icon resource.
 * @return Component object.
 ******************************************************************************/
View &View::icon(const void *src_icon)
{
	_icon.source(src_icon);
	_icon.realign();
	_lbl_title.realign();
	return *this;
}

/******************************************************************************
 * @brief Set title of the alert component.
 * @param title Title of the alert component.
 * @return Component object.
 ******************************************************************************/
View &View::title(const char *title)
{
	_lbl_title.text(title);
	_lbl_title.realign();
	return *this;
}

/******************************************************************************
 * @brief Set message of the alert component.
 * @param msg message of the alert component.
 * @return Component object.
 ******************************************************************************/
View &View::msg(const char *msg)
{
	_lbl_msg.text(msg);
	_lbl_msg.text_align(LV_TEXT_ALIGN_CENTER);
	_lbl_msg.realign();
	return *this;
}

View &View::action_btn(const char *label, void (*cb)(void))
{
	if (label != nullptr && label[0] != '\0' && cb != nullptr) {
		_action_cb = cb;
		_button_action.title(const_cast<char *>(label));
		lv_obj_clear_flag(_button_action.self_lv_obj(), LV_OBJ_FLAG_HIDDEN);
		_icon.align(LV_ALIGN_TOP_MID, 0, 22);
		_lbl_title.align(LV_ALIGN_TOP_MID, 0, 86);
		_lbl_msg.align_to(_lbl_title.self_lv_obj(), LV_ALIGN_OUT_BOTTOM_MID, 0, 6);
	} else {
		_action_cb = nullptr;
		lv_obj_add_flag(_button_action.self_lv_obj(), LV_OBJ_FLAG_HIDDEN);
		_icon.align(LV_ALIGN_TOP_MID, 0, 35);
		_lbl_title.align(LV_ALIGN_TOP_MID, 0, 105);
		_lbl_msg.align_to(_lbl_title.self_lv_obj(), LV_ALIGN_OUT_BOTTOM_MID, 0, 7);
	}
	return *this;
}

/******************************************************************************
 * @brief Show alert component.
 * @return Component object.
 ******************************************************************************/
View &View::show()
{
	if(is_shown())
	{
		return *this;
	}
	set_alert_state(this, true);
	return *this;
}

/******************************************************************************
 * @brief Hide alert component.
 * @return Component object.
 ******************************************************************************/
View &View::hide()
{
	if(is_shown())
	{
		set_alert_state(this, false);
	}
	return *this;
}

/******************************************************************************
 * @brief Check if alert component is shown.
 * @retval True.
 * @retval False.
 ******************************************************************************/
bool View::is_shown()
{
	if(lv_obj_has_flag(_self_lv_obj, LV_OBJ_FLAG_HIDDEN))
	{
		return false;
	}
	return true;
}

/******************************************************************************
 * @brief Set in how many seconds the alert should show again.
 * @param seconds Time in seconds.
 * @return Component object.
 ******************************************************************************/
View &View::show_every_s(uint32_t seconds)
{
	_show_every_s = seconds;
	return *this;
}

/******************************************************************************
 * @brief Get the timestamp tha the alert was last shown.
 * @return Timestamp in milliseconds.
 ******************************************************************************/
uint32_t View::get_last_shown_ts()
{
	return _last_time_shown_ts;
}

void View::set_last_shown_ts(uint32_t ts)
{
	_last_time_shown_ts = ts;
}

/******************************************************************************
 * @brief Get in how many seconds the alert should show again.
 * @return Time in seconds.
 ******************************************************************************/
uint32_t View::get_show_every_s()
{
	return _show_every_s;
}

bool View::is_time_to_show()
{
	if(get_last_shown_ts() == 0)
	{
		return true;
	}
	if(UiCommon::get_seconds_count() > get_last_shown_ts() + get_show_every_s())
	{
		return true;
	}
	return false;
}

/******************************************************************************
 * @brief Get the type of the alert component.
 * @return Type of the alert component. NONE, INFO, WARNING, ERROR.
 ******************************************************************************/
View::AlertType View::get_type()
{
	return _type;
}

/******************************************************************************
 * @brief Get color of the alert component.
 * @return LVGL color type.
 ******************************************************************************/
lv_color_t View::get_color()
{
	return _color;
}

/******************************************************************************
 * @brief Realign children components.
 * @return Component object.
 ******************************************************************************/
View &View::realign_children()
{
	return *this;
}

} // namespace UiViewAlert