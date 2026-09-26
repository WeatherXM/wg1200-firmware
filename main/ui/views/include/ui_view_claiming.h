#ifndef UI_VIEW_CLAIMING_H
#define UI_VIEW_CLAIMING_H

#include "ui/include/ui_common.h"

namespace UiViewClaiming
{

class View : public UiTypes::ContainerBase<View>
{
public:
	
	View &create(lv_obj_t *parent);
	bool is_open();
	void set_max_fail_streak(int max_fail_streak);
	void claim_enable_success(char *sn, size_t length, uint32_t code);
	void claim_enable_failed();
	void claim_enable_timeout();
	UiTypes::ButtonComponent _btn_close;
	
protected:
	View &realign_children();

private:
	UiTypes::LabelComponent _lbl_title;
	UiTypes::LabelComponent _lbl_claim_instr;
	UiTypes::LabelComponent _lbl_fail_instr;
	UiTypes::IconComponent _icon_fail;
	UiTypes::QrCodeComponent _qr_sn;
	UiTypes::LabelComponent _lbl_key;
	UiTypes::LabelComponent _lbl_sn;
	UiTypes::SpinnerComponent _spinner_wait;

	int _fail_streak_count = 0;
	int _max_fail_streak = 5;
};

} // namespace UiViewClaiming

#endif