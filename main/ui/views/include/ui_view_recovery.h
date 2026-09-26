#ifndef UI_VIEW_RECOVERY_H
#define UI_VIEW_RECOVERY_H

#include "ui/include/ui_common.h"

namespace UiViewRecovery
{

class View : public UiTypes::ContainerBase<View>
{
  public:
	View &create(lv_obj_t *parent);
	UiTypes::ButtonComponent _btn_restore_fw;
	UiTypes::ButtonComponent _btn_enter_diagnostics;
	UiTypes::ButtonComponent _btn_exit;

  protected:
	View &realign_children();

  private:
	UiTypes::ContainerComponent _cont_bg;
	UiTypes::LabelComponent _lbl_title;
	UiTypes::ContainerComponent _cont_line;
	
};

} // namespace UiViewRecovery

#endif