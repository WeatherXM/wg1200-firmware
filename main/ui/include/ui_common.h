#ifndef UI_COMMON_H
#define UI_COMMON_H

#include "lvgl/lvgl.h"
#include "stdint.h"
#include "ui_const.h"
#include "ui_types.h"
#include "ui_theme_wxm.h"
#include <stdio.h>

namespace UiCommon
{
void lock();
void unlock();

uint32_t get_millis_count();
uint32_t get_seconds_count();

} // namespace UiCommon

#endif