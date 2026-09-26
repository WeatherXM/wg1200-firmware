#include "ui/views/include/ui_view_claiming.h"
#include "ui/resources/ui_resources.h"

namespace UiViewClaiming
{

View &View::create(lv_obj_t *parent)
{
	_parent_lv_obj = parent;
	_self_lv_obj = lv_obj_create(_parent_lv_obj);
	lv_obj_remove_style_all(_self_lv_obj);
	color_bg(UI_COLOR_BG_DARK);
	border_color(UI_COLOR_SECONDARY_DARK);
	border_width(1);
	radius(UI_ITEM_RADIUS);
	lv_obj_set_style_shadow_width(_self_lv_obj, 30, LV_PART_MAIN);
	lv_obj_set_style_shadow_spread(_self_lv_obj, 0, LV_PART_MAIN);
	lv_obj_set_style_shadow_color(_self_lv_obj, UI_COLOR_SECONDARY_DARK, LV_PART_MAIN);
	lv_obj_set_style_shadow_opa(_self_lv_obj, LV_OPA_50, LV_PART_MAIN);

	_fail_streak_count = 0;

	_lbl_title.create(_self_lv_obj)
		.font(&lv_font_montserrat_20)
		.color(UI_TEXT_COLOR_LIGHT)
		.text("Please Wait")
		.text_align(LV_TEXT_ALIGN_CENTER)
		.align(LV_ALIGN_TOP_MID, 0, 18);

	_spinner_wait.create(_self_lv_obj)
		.size(70, 70)
		.color_arc_bg(lv_color_hex(0x292931))
		.color_arc_line(UI_COLOR_SECONDARY_DARK)
		.align(LV_ALIGN_TOP_MID, 0, 136);

	_btn_close.create(_self_lv_obj)
		.title("Close")
		.size(344, 40)
		.align(LV_ALIGN_BOTTOM_MID, 0, -20);

	_icon_fail.create(_self_lv_obj)
		.source(&UiResources::icon_cloud_slash)
		.hidden(true)
		.align(LV_ALIGN_TOP_MID, 0, 75);

	_qr_sn.create(_self_lv_obj)
		.align(LV_ALIGN_TOP_MID, 0, 100)
		.hidden(true);
	
	_lbl_key.create(_self_lv_obj)
		.font(&lv_font_montserrat_20)
		.color(UI_TEXT_COLOR_LIGHT)
		.text_align(LV_TEXT_ALIGN_CENTER)
		.align_to(_qr_sn.self_lv_obj(), LV_ALIGN_OUT_BOTTOM_MID, 0, 10)
		.hidden(true);

	_lbl_sn.create(_self_lv_obj)
		.font(&lv_font_montserrat_20)
		.color(UI_TEXT_COLOR_LIGHT)
		.text_align(LV_TEXT_ALIGN_CENTER)
		.align_to(_lbl_key.self_lv_obj(), LV_ALIGN_OUT_BOTTOM_MID, 0, 6)
		.hidden(true);

	_lbl_claim_instr.create(_self_lv_obj)
		.font(&lv_font_montserrat_16)
		.color(UI_COLOR_SECONDARY_DARK)
		.text("Open your mobile app and add a new\n\
device by following the instructions.")
		.text_align(LV_TEXT_ALIGN_CENTER)	
		.align(LV_ALIGN_TOP_MID, 0, 50)
		.hidden(true);

	_lbl_fail_instr.create(_self_lv_obj)
		.font(&lv_font_montserrat_16)
		.color(UI_COLOR_SECONDARY_DARK)
		.text("Something is wrong with device\n\
cloud connectivity. Please close this \n\
window and make sure your device\n\
is connected to the cloud.")
		.text_align(LV_TEXT_ALIGN_CENTER)	
		.align(LV_ALIGN_TOP_MID, 0, 175)
		.hidden(true);

	return *this;
}

void View::set_max_fail_streak(int max_fail_streak)
{
	_max_fail_streak = max_fail_streak;
}

bool View::is_open()
{
	if(_self_lv_obj != NULL)
	{
		return true;
	}
	return false;
}

void View::claim_enable_success(char *sn, size_t length, uint32_t code)
{
	if(_fail_streak_count < _max_fail_streak)
	{
		_fail_streak_count = 0;
		_lbl_title.text("Claim this Device");
		_spinner_wait.hidden(true);
		_btn_close.title("Dismiss");

		char str_buff[40] = "";
		snprintf(str_buff, sizeof(str_buff), "S/N: %s", sn);
		_lbl_sn.text(str_buff).hidden(false).realign();

		snprintf(str_buff, sizeof(str_buff), "%06d", code);
		char qr_buff[50];
		snprintf(qr_buff, sizeof(qr_buff), "%s,%s", sn, str_buff);
		_qr_sn.data(qr_buff, strlen(qr_buff)).hidden(false);

		snprintf(str_buff, sizeof(str_buff), "KEY: %06d", code);
		_lbl_key.text(str_buff).hidden(false).realign();
		_lbl_claim_instr.hidden(false);
	}	
}

void View::claim_enable_failed()
{
	_fail_streak_count++;

	if(_fail_streak_count >= _max_fail_streak)
	{
		_lbl_title.text("Oops!");
		_spinner_wait.hidden(true);		
	}
	else
	{
		_lbl_title.text("Please Wait");
		_spinner_wait.hidden(false);
		_lbl_claim_instr.hidden(false);
		_lbl_fail_instr.hidden(true);
	}

	_lbl_claim_instr.hidden(false);
	_btn_close.title("Close");
	_qr_sn.hidden(true);
	_lbl_sn.hidden(true);
	_lbl_key.hidden(true);	
	_icon_fail.hidden(true);
}

void View::claim_enable_timeout()
{

}

View &View::realign_children()
{
	_lbl_title.realign();
	_lbl_claim_instr.realign();
	_lbl_fail_instr.realign();
	_icon_fail.realign();
	_lbl_key.realign();
	_spinner_wait.realign();
	_btn_close.realign();
	return *this;
}
} // namespace UiViewClaiming