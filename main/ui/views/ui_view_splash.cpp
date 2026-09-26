#include "ui/views/include/ui_view_splash.h"
#include "ui/include/ui_common.h"
#include "ui/resources/ui_resources.h"
#include "app_config.h"

namespace UiViewSplash
{

View &View::create(lv_obj_t *parent)
{
	_parent_lv_obj = parent;
	lv_obj_t *old_scr = lv_scr_act();
	_self_lv_obj = lv_obj_create(NULL);
	lv_scr_load(_self_lv_obj);
	if(old_scr != NULL && old_scr != _self_lv_obj)
	{
		lv_obj_del_async(old_scr);
	}
	lv_obj_set_scrollbar_mode(_self_lv_obj, LV_SCROLLBAR_MODE_OFF);
	lv_obj_add_event_cb(_self_lv_obj, [](lv_event_t *event)
	{
		static int click_count = 0;
		click_count++;
		// printf("Splash click count: %d", click_count);
		if(click_count >= 5)
		{
			UiCommon::lock();
			((UiViewSplash::View*)lv_event_get_user_data(event))->set_enter_recovery(true);
			UiCommon::unlock();
		}
	}, LV_EVENT_CLICKED, this);

	_icon_wxm.create(_self_lv_obj)
		.source(&UiResources::icon_splash_logo)
		.clickable(false)
		.align(LV_ALIGN_CENTER, 0, 0);

	char buff[60];
	snprintf(buff, sizeof(buff), "WG1200 v%s", APP_VERSION);
	_lbl_model_ver.create(_self_lv_obj)
		.font(&lv_font_montserrat_20)
		.color(lv_color_darken(UI_TEXT_COLOR_LIGHT, LV_OPA_60))
		.text(buff)
		.clickable(false)
		.align(LV_ALIGN_BOTTOM_RIGHT, -15, -15);

	return *this;
}

bool View::get_enter_recovery()
{
	return _enter_recovery;
}

void View::set_enter_recovery(bool flag)
{
	_enter_recovery = flag;
	if(_enter_recovery)
	{
		_lbl_model_ver.text("Entering recovery...");
	}
}

void View::disable_click()
{
	lv_obj_clear_flag(_self_lv_obj, LV_OBJ_FLAG_CLICKABLE);
}

View &View::realign_children()
{
	// _cont_bg.realign();	
	_icon_wxm.realign();
	_lbl_model_ver.realign();
	return *this;
}

} // namespace UiViewSplash