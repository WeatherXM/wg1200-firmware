#include "ui/views/include/ui_view_recovery.h"
#include "ui/include/ui_common.h"

namespace UiViewRecovery
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

	_cont_bg.create(_self_lv_obj)
		.radius(UI_ITEM_RADIUS)
		.size(UI_SCREEN_WIDTH - 40, UI_SCREEN_HEIGHT - 40)
		.color_bg(UI_COLOR_PRIMARY_DARK)
		.align(LV_ALIGN_CENTER, 0, 0);	

	_lbl_title.create(_self_lv_obj)
		.font(&lv_font_montserrat_20)
		.color(UI_TEXT_COLOR_LIGHT)
		.text("Recovery")
		.align(LV_ALIGN_TOP_MID, 0, 40);

	_cont_line.create(_self_lv_obj)
		.size(UI_SCREEN_WIDTH - 2*40, 2)
		.color_bg(UI_COLOR_BG_DARK)
		.align(LV_ALIGN_TOP_MID, 0, 80);
	
	_btn_restore_fw.create(_self_lv_obj)
		.title("Restore factory firmware")
		.color_bg(UI_COLOR_BG_DARK)
		.color_bg_clicked(UI_COLOR_PRIMARY_DARK)
		.color_title(UI_TEXT_COLOR_LIGHT)
		.border_color(UI_COLOR_SECONDARY_DARK)
		.border_width(1)
		.size(300, 60)
		.align(LV_ALIGN_TOP_MID, 0, 130)
		.clickable(true);

	_btn_enter_diagnostics.create(_self_lv_obj)
		.title("Enter diagnostics mode")
		.color_bg(UI_COLOR_BG_DARK)
		.color_bg_clicked(UI_COLOR_PRIMARY_DARK)
		.color_title(UI_TEXT_COLOR_LIGHT)
		.border_color(UI_COLOR_SECONDARY_DARK)
		.border_width(1)
		.size(300, 60)
		.align(LV_ALIGN_TOP_MID, 0, 210)
		.clickable(true);

	_btn_exit.create(_self_lv_obj)
		.title("Exit")
		.size(100, 45)
		.align(LV_ALIGN_BOTTOM_RIGHT, -40, -40)
		.clickable(true);

	return *this;
}


View &View::realign_children()
{
	_cont_bg.realign();
	_lbl_title.realign();
	_cont_line.realign();
	_btn_restore_fw.realign();
	_btn_enter_diagnostics.realign();
	_btn_exit.realign();
	
	return *this;
}

} // namespace UiViewWifiConfig