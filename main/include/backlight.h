#ifndef BACKLIGHT_H
#define BACKLIGHT_H

#include "const.h"
#include "settings.h"
#include "backlight_bridge.h"
#ifndef CONFIG_SCREENLESS
#include "bsp_lcd.h"
#endif

namespace Backlight
{

void init();
bool is_screen_on();
void turn_screen_on(backlight_wake_reason_t reason = BACKLIGHT_WAKE_PROGRAMMATIC);
void turn_screen_off();
void set_backlight_level(BacklightLevel level, backlight_wake_reason_t reason = BACKLIGHT_WAKE_SETTING_CHANGE);
void set_backlight_percent(uint8_t percent, backlight_wake_reason_t reason);
const char *wake_reason_to_str(backlight_wake_reason_t reason);
const char *get_last_wake_reason();
uint32_t get_wake_count();
const BacklightLevelDesc *get_idle_backlight_level_desc(char *name, BacklightLevel level);

}

#endif
