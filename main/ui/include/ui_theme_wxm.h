#ifndef UI_THEME_WXM_H
#define UI_THEME_WXM_H

#include "lvgl/lvgl.h"
#include "lvgl/src/lv_conf_internal.h"

namespace UiThemeWxm
{

/**
 * Initialize the theme
 * @param disp pointer to display to attach the theme
 * @return a pointer to reference this theme later
 */
lv_theme_t * lv_theme_basic_init(lv_disp_t * disp);

/**
* Check if the theme is initialized
* @return true if default theme is initialized, false otherwise
*/
bool lv_theme_basic_is_inited(void);

lv_style_t *get_style_heading_1();
lv_style_t *get_style_heading_2();
lv_style_t *get_style_heading_3();
lv_style_t *get_style_heading_4();
lv_style_t *get_style_heading_5();
lv_color_t get_color_pos();
lv_color_t get_color_warn();
lv_color_t get_color_neg();
lv_style_t *get_style_obj_basic();

} // namespace UiThemeWxm

#endif