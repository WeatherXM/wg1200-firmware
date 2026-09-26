#ifndef UI_VIEW_WIFI_CONFIG_H
#define UI_VIEW_WIFI_CONFIG_H

#include "ui/include/ui_common.h"

namespace UiViewWifiConfig
{

class View : public UiTypes::ContainerBase<View>
{
  public:
	View &create(lv_obj_t *parent);
	void update_status_text(char *text, lv_color_t color);
	UiTypes::ButtonComponent _btn_exit;

  protected:
	View &realign_children();

  private:
	UiTypes::ContainerComponent _cont_bg;
	UiTypes::LabelComponent _lbl_title;
	UiTypes::ContainerComponent _cont_line;
	UiTypes::LabelComponent _lbl_instr_1;
	UiTypes::LabelComponent _lbl_ap_name;
	UiTypes::LabelComponent _lbl_instr_2;
	UiTypes::LabelComponent _lbl_ip;
	UiTypes::LabelComponent _lbl_status;
	UiTypes::SpinnerComponent _spinner_status;
	
};

} // namespace UiViewWifiConfig

#endif