#ifndef BACKLIGHT_BRIDGE_H
#define BACKLIGHT_BRIDGE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Keep these values stable: they are exposed in diagnostics and ThingsBoard
 * attributes so field logs can identify why a dark display was illuminated.
 */
typedef enum
{
	BACKLIGHT_WAKE_BOOT = 0,
	BACKLIGHT_WAKE_PHYSICAL_BUTTON,
	BACKLIGHT_WAKE_TOUCH,
	BACKLIGHT_WAKE_OTA,
	BACKLIGHT_WAKE_SETTING_CHANGE,
	BACKLIGHT_WAKE_PROGRAMMATIC,
} backlight_wake_reason_t;

/**
 * C bridge used by lv_port.c. All application backlight changes must pass
 * through this function so the logical state cannot diverge from the hardware.
 */
void backlight_set_level_percent_c(uint8_t percent, backlight_wake_reason_t reason);

#ifdef __cplusplus
}
#endif

#endif
