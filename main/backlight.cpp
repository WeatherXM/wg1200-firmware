#include "backlight.h"
#include "esp_log.h"
#include "esp_timer.h"

namespace Backlight
{

static const char TAG[] = "Backlight";
static bool _is_screen_on = false;
static backlight_wake_reason_t _last_wake_reason = BACKLIGHT_WAKE_BOOT;
static uint32_t _wake_count = 0;

const char *wake_reason_to_str(backlight_wake_reason_t reason)
{
	switch(reason)
	{
		case BACKLIGHT_WAKE_BOOT:
			return "boot";
		case BACKLIGHT_WAKE_PHYSICAL_BUTTON:
			return "physical_button";
		case BACKLIGHT_WAKE_TOUCH:
			return "touch";
		case BACKLIGHT_WAKE_OTA:
			return "ota";
		case BACKLIGHT_WAKE_SETTING_CHANGE:
			return "setting_change";
		case BACKLIGHT_WAKE_PROGRAMMATIC:
		default:
			return "programmatic";
	}
}

static void record_wake(backlight_wake_reason_t reason)
{
	_last_wake_reason = reason;
	_wake_count++;
	ESP_LOGW(TAG, "Screen wake: reason=%s uptime_ms=%lld count=%lu",
			 wake_reason_to_str(reason),
			 (long long)(esp_timer_get_time() / 1000),
			 (unsigned long)_wake_count);
}

void init()
{
	_is_screen_on = false;
	set_backlight_level(Settings::get_idle_backlight_level(), BACKLIGHT_WAKE_BOOT);
}

void set_backlight_percent(uint8_t percent, backlight_wake_reason_t reason)
{
	if(percent > 100)
	{
		percent = 100;
	}

	const bool was_on = _is_screen_on;
#ifndef CONFIG_SCREENLESS
	bsp_lcd_set_backlight_level(percent);
#endif
	_is_screen_on = (percent != 0);
	if(!was_on && _is_screen_on)
	{
		record_wake(reason);
	}
}

void set_backlight_level(BacklightLevel level, backlight_wake_reason_t reason)
{
	set_backlight_percent((uint8_t)level, reason);
}

bool is_screen_on()
{
	return _is_screen_on;
}

void turn_screen_on(backlight_wake_reason_t reason)
{
	if(!_is_screen_on)
	{
		set_backlight_level(Settings::get_idle_backlight_level(), reason);
	}
}

void turn_screen_off()
{
	if(_is_screen_on)
	{
		set_backlight_percent(0, BACKLIGHT_WAKE_PROGRAMMATIC);
		ESP_LOGI(TAG, "Screen off: uptime_ms=%lld",
				 (long long)(esp_timer_get_time() / 1000));
	}
}

const char *get_last_wake_reason()
{
	return wake_reason_to_str(_last_wake_reason);
}

uint32_t get_wake_count()
{
	return _wake_count;
}

/******************************************************************************
* Get backlight level descriptor by title or level value
* Either title or level must be set, not both
* @param levels Level descriptor array to search
* @param title Level title as displayed in UI
* @param level Level value
* @return Ptr to backlight level descriptor in provided levels array
******************************************************************************/
const BacklightLevelDesc *get_idle_backlight_level_desc(char *title, BacklightLevel level)
{
	// Only title or level must be set
	if(!((title == nullptr) ^ (level == BacklightLevel::BACKLIGHT_LEVEL_NONE)))
	{
		return nullptr;
	}

	const int LAST_INDEX = sizeof(IDLE_BACKLIGHT_LEVELS) / sizeof(IDLE_BACKLIGHT_LEVELS[0]);
	int found_index = -1;

	if(title != nullptr)
	{
		for (int i = 0; i < LAST_INDEX; i++)
		{
			if(strcmp(title, IDLE_BACKLIGHT_LEVELS[i].title) == 0)
			{
				found_index = i;
				break;
			}
		}
	}
	else
	{
		for (int i = 0; i < LAST_INDEX; i++)
		{
			if(IDLE_BACKLIGHT_LEVELS[i].level == level)
			{
				found_index = i;
				break;
			}
		}
	}

	return found_index >= 0 ? &IDLE_BACKLIGHT_LEVELS[found_index] : nullptr;
}

}

extern "C" void backlight_set_level_percent_c(uint8_t percent, backlight_wake_reason_t reason)
{
	Backlight::set_backlight_percent(percent, reason);
}
