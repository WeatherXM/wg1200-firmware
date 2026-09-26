#include "ui/views/include/ui_view_wifi_config.h"
#include "ui/include/ui_common.h"

namespace UiViewWifiConfig
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
		.text("WiFi Configuration")
		.align(LV_ALIGN_TOP_MID, 0, 40);

	_cont_line.create(_self_lv_obj)
		.size(UI_SCREEN_WIDTH - 2*40, 2)
		.color_bg(UI_COLOR_BG_DARK)
		.align(LV_ALIGN_TOP_MID, 0, 80);
	
	_lbl_instr_1.create(_self_lv_obj)
		.font(&lv_font_montserrat_18)
		.color(UI_COLOR_SECONDARY_DARK)
		.text("To configure WiFi:\n\n1. Connect to WiFi SSID named: ")
		.text_align(LV_TEXT_ALIGN_LEFT)
		.align(LV_ALIGN_TOP_LEFT, 40, 100);

	_lbl_ap_name.create(_self_lv_obj)
		.font(&lv_font_montserrat_24)
		.color(lv_color_darken(UI_TEXT_COLOR_LIGHT, LV_OPA_10))
		.text(CONFIG_DEFAULT_AP_SSID)
		.align(LV_ALIGN_TOP_MID, 0, 190);

	_lbl_instr_2.create(_self_lv_obj)
		.font(&lv_font_montserrat_18)
		.color(UI_COLOR_SECONDARY_DARK)
		.text("2. Open your browser and navigate to:")
		.text_align(LV_TEXT_ALIGN_LEFT)
		.align(LV_ALIGN_TOP_LEFT, 40, 240);

	_lbl_ip.create(_self_lv_obj)
		.font(&lv_font_montserrat_24)
		.color(lv_color_darken(UI_TEXT_COLOR_LIGHT, LV_OPA_10))
		.text(CONFIG_DEFAULT_AP_IP)
		.align(LV_ALIGN_TOP_MID, 0, 290);

	_lbl_status.create(_self_lv_obj)
		.font(&lv_font_montserrat_18)
		.color(UI_TEXT_COLOR_LIGHT)
		.text("")
		.align(LV_ALIGN_TOP_MID, 0, 330);

	_spinner_status.create(_self_lv_obj)
		.size(50, 50)
		.color_arc_bg(UI_COLOR_BG_DARK)
		.color_arc_line(UI_COLOR_SECONDARY_DARK)
		.align(LV_ALIGN_TOP_MID, 0, 340);

	_btn_exit.create(_self_lv_obj)
		.title("Exit")
		.size(100, 45)
		.align(LV_ALIGN_BOTTOM_RIGHT, -40, -40)
		.clickable(true);

	return *this;
}

void View::update_status_text(char *text, lv_color_t color)
{
	_lbl_status.text(text).color(color);
	_spinner_status.hidden(true);
}

View &View::realign_children()
{
	_lbl_title.realign();
	_cont_line.realign();
	_lbl_instr_1.realign();
	_lbl_ap_name.realign();
	_lbl_instr_2.realign();
	_lbl_ip.realign();
	_lbl_status.realign();
	_btn_exit.realign();
	_cont_bg.realign();
	return *this;
}

} // namespace UiViewWifiConfig