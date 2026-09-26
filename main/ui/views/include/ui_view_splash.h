#ifndef UI_VIEW_SPLASH_H
#define UI_VIEW_SPLASH_H

#include "ui/include/ui_common.h"

namespace UiViewSplash
{

class View : public UiTypes::ContainerBase<View>
{
  public:
	View &create(lv_obj_t *parent);
	void set_enter_recovery(bool flag);
	bool get_enter_recovery();
	void disable_click();

  protected:
	View &realign_children();

  private:
	UiTypes::IconComponent _icon_wxm;
	UiTypes::LabelComponent _lbl_model_ver;
	bool _enter_recovery = false;	
	
};

} // namespace UiViewWifiConfig

#endif