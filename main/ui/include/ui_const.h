#ifndef UI_CONST_H
#define UI_CONST_H

#include "ui_common.h"

// TODO: Some need to be implemented in the theme

#define UI_SCREEN_WIDTH 480
#define UI_SCREEN_HEIGHT 480

#define UI_ITEM_GAP 6
#define UI_ITEM_RADIUS 9

#define UI_VALUES_DEFAULT "--"
#define UI_VALUES_MAX_ERROR "..."
#define UI_TEXT_COLOR_DARK lv_color_hex(0x050505)
#define UI_TEXT_COLOR_LIGHT lv_color_hex(0xFEFBFF)

#define UI_SIMPLE_TILE_RADIUS UI_ITEM_RADIUS

#define UI_SIMPLE_TILE_WIDTH 152
#define UI_SIMPLE_TILE_HEIGHT 144

#define UI_SIMPLE_TILE_HUM_COLOR lv_color_hex(0x75B2A1)//ui_color_make_hex(0x75B2A1)
#define UI_SIMPLE_TILE_HUM_TEXT_COLOR UI_TEXT_COLOR_DARK
#define UI_SIMPLE_TILE_HUM_TITLE "Humidity"

#define UI_SIMPLE_TILE_IRR_COLOR lv_color_hex(0xFFCE49)
#define UI_SIMPLE_TILE_IRR_TEXT_COLOR UI_TEXT_COLOR_DARK
#define UI_SIMPLE_TILE_IRR_TITLE "Irradiance"

#define UI_SIMPLE_TILE_PRECIP_COLOR lv_color_hex(0x265EB4)
#define UI_SIMPLE_TILE_PRECIP_TEXT_COLOR UI_TEXT_COLOR_LIGHT
#define UI_SIMPLE_TILE_PRECIP_TITLE "Precipitation"
#define UI_SIMPLE_TILE_PRECIP_RATE_TITLE "Rain Rate"
#define UI_SIMPLE_TILE_PRECIP_DAILY_TITLE "Daily Rain"

#define UI_SIMPLE_TILE_WIND_COLOR lv_color_hex(0xEAEBF9)
#define UI_SIMPLE_TILE_WIND_TEXT_COLOR UI_TEXT_COLOR_DARK
#define UI_SIMPLE_TILE_WIND_TITLE "Wind"

#define UI_SIMPLE_TILE_PRESSURE_COLOR lv_color_hex(0x8D458A)
#define UI_SIMPLE_TILE_PRESSURE_TEXT_COLOR UI_TEXT_COLOR_LIGHT
#define UI_SIMPLE_TILE_PRESSURE_TITLE "Pressure"

#define UI_SIMPLE_TILE_UV_COLOR lv_color_hex(0xB55454)
#define UI_SIMPLE_TILE_UV_TEXT_COLOR UI_TEXT_COLOR_LIGHT
#define UI_SIMPLE_TILE_UV_TITLE "UV"

#define UI_SIMPLE_TILE_DEW_COLOR lv_color_hex(0x4785A5)
#define UI_SIMPLE_TILE_DEW_TEXT_COLOR UI_TEXT_COLOR_DARK
#define UI_SIMPLE_TILE_DEW_TITLE "Dew Point"

#define UI_COLOR_BG_DARK lv_color_hex(0x1D1E2C)
#define UI_COLOR_PRIMARY_DARK lv_color_hex(0x26283C)
#define UI_COLOR_SECONDARY_DARK lv_color_hex(0xB8C6FF)

#define UI_QOD_TILE_COLOR_NA lv_color_hex(0x5D5E67)
#define UI_QOD_TILE_COLOR_WRN lv_color_hex(0x46381B)
#define UI_QOD_TILE_COLOR_WRN_LIMIT 80
#define UI_QOD_TILE_COLOR_ERR lv_color_hex(0x4B2F36)
#define UI_QOD_TILE_COLOR_ERR_LIMIT 20
#define UI_QOD_TILE_COLOR_OK lv_color_hex(0x00572D)
#define UI_QOD_TILE_TEXT_COLOR UI_TEXT_COLOR_LIGHT
#define UI_QOD_TILE_TITLE "Quality of\nData Score (%)"
#define UI_QOD_TILE_TEXT_PENDING "Your station\nhas started\ntransmitting\nits initial data."

#define UI_POL_TILE_COLOR_NA lv_color_hex(0x5D5E67)
#define UI_POL_TILE_COLOR_WRN lv_color_hex(0x46381B)
#define UI_POL_TILE_COLOR_ERR lv_color_hex(0x4B2F36)
#define UI_POL_TILE_COLOR_OK lv_color_hex(0x00572D)
#define UI_POL_TILE_TEXT_COLOR UI_TEXT_COLOR_LIGHT
#define UI_POL_TILE_TITLE_TEXT_OK "Location\nVerified"
#define UI_POL_TILE_TITLE_TEXT_PENDING "Pending\nVerification"
#define UI_POL_TILE_TITLE_TEXT_NO_DATA "Location\nVerification"
#define UI_POL_TILE_TITLE_TEXT_NOT_VER "Location not\nVerified"
#define UI_POL_TILE_TITLE_TEXT_NO_LOC "No Location\nData"
#define UI_POL_TILE_TITLE_TEXT_MAX_ADDR_CHARS 35

#define UI_REW_LAST_TILE_COLOR lv_color_hex(0x2F324C)
#define UI_REW_LAST_TILE_TEXT_COLOR UI_TEXT_COLOR_LIGHT
#define UI_REW_LAST_TILE_TITLE_TEXT "Last Reward"
#define UI_REW_LAST_TILE_TEXT_PENDING "Rewards Coming Soon!"

#define UI_REW_TOTAL_TILE_COLOR lv_color_hex(0x2F324C)
#define UI_REW_TOTAL_TILE_TEXT_COLOR UI_TEXT_COLOR_LIGHT
#define UI_REW_TOTAL_TILE_TITLE_TEXT "Total Rewards"
#define UI_REW_TOTAL_TILE_TOKEN_TEXT_COLOR lv_color_hex(0x797A8A) //FEFBFF
#define UI_REW_TOTAL_TILE_TOKEN_TEXT "$WXM"


#endif