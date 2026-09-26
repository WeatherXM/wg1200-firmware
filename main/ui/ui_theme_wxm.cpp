
/*********************
 *      INCLUDES
 *********************/
#include "lvgl/lvgl.h" /*To see all the widgets*/

#include "lvgl/src/misc/lv_gc.h"
#include "ui/include/ui_theme_wxm.h"
#include "ui/resources/ui_resources.h"

namespace UiThemeWxm
{

/*********************
 *      DEFINES
 *********************/
#define COLOR_SCR     UI_COLOR_BG_DARK
#define COLOR_WHITE   lv_color_white()
#define COLOR_LIGHT   lv_palette_lighten(LV_PALETTE_GREY, 2)
#define COLOR_DARK    lv_palette_main(LV_PALETTE_GREY)
#define COLOR_DIM     lv_palette_darken(LV_PALETTE_GREY, 2)
#define SCROLLBAR_WIDTH     2

/**********************
 *      TYPEDEFS
 **********************/
typedef struct {
	lv_style_t scr;
	lv_style_t transp;
	lv_style_t white;
	lv_style_t light;
	lv_style_t dark;
	lv_style_t dim;
	lv_style_t scrollbar;
	lv_style_t msgbox;
	lv_style_t msgbox_items;
	lv_style_t msgbox_btn_matrix;
	lv_style_t msgbox_btn;
#if LV_USE_ARC || LV_USE_COLORWHEEL
	lv_style_t arc_line;
	lv_style_t arc_knob;
#endif
#if LV_USE_TEXTAREA
	lv_style_t ta_cursor;
#endif
} my_theme_styles_t;


/**********************
 *  STATIC PROTOTYPES
 **********************/
static void style_init_reset(lv_style_t * style);
static void theme_apply(lv_theme_t * th, lv_obj_t * obj);

/**********************
 *  STATIC VARIABLES
 **********************/
static my_theme_styles_t * styles;
static lv_theme_t theme;
static bool inited;

static lv_style_t _style_heading_1;
static lv_style_t _style_heading_2;
static lv_style_t _style_heading_3;
static lv_style_t _style_heading_4;
static lv_style_t _style_heading_5;
lv_style_t _style_obj_basic;

/**********************
 *      MACROS
 **********************/

/**********************
 *   STATIC FUNCTIONS
 **********************/

static void custom_styles_init(void)
{
	// TODO: Dark/light on init or dynamic?
	// TODO: Color set

	lv_style_init(&_style_heading_1);
	lv_style_set_text_font(&_style_heading_1, &UiResources::ui_font_montserrat_42_bold);
	lv_style_set_text_color(&_style_heading_1, lv_color_make(255, 255, 255));

	lv_style_init(&_style_heading_2);
	lv_style_set_text_font(&_style_heading_2, &UiResources::ui_font_montserrat_34_bold);
	lv_style_set_text_color(&_style_heading_2, lv_color_make(255, 255, 255));

	lv_style_init(&_style_heading_3);
	lv_style_set_text_font(&_style_heading_3, &lv_font_montserrat_20);
	lv_style_set_text_color(&_style_heading_3, lv_color_make(255, 255, 255));

	lv_style_init(&_style_heading_4);
	lv_style_set_text_font(&_style_heading_4, &lv_font_montserrat_18);
	lv_style_set_text_color(&_style_heading_4, lv_color_make(255, 255, 255));

	lv_style_init(&_style_heading_5);
	lv_style_set_text_font(&_style_heading_5, &lv_font_montserrat_16);
	lv_style_set_text_color(&_style_heading_5, lv_color_make(255, 255, 255));

	lv_style_reset(&_style_obj_basic);
}

static void style_init(void)
{
	style_init_reset(&styles->scrollbar);
	lv_style_set_bg_opa(&styles->scrollbar, LV_OPA_COVER);
	lv_style_set_bg_color(&styles->scrollbar, COLOR_DARK);
	lv_style_set_width(&styles->scrollbar,  SCROLLBAR_WIDTH);

	style_init_reset(&styles->scr);
	lv_style_set_bg_opa(&styles->scr, LV_OPA_COVER);
	lv_style_set_bg_color(&styles->scr, COLOR_SCR);
	lv_style_set_text_color(&styles->scr, COLOR_DIM);


	style_init_reset(&styles->transp);
	lv_style_set_bg_opa(&styles->transp, LV_OPA_TRANSP);

	style_init_reset(&styles->msgbox);
	lv_style_set_radius(&styles->msgbox, UI_ITEM_RADIUS);
	lv_style_set_bg_color(&styles->msgbox, UI_COLOR_BG_DARK);
	lv_style_set_text_font(&styles->msgbox, &lv_font_montserrat_20);
	lv_style_set_text_color(&styles->msgbox, lv_color_white());
	lv_style_set_text_align(&styles->msgbox, LV_TEXT_ALIGN_CENTER);
	lv_style_set_bg_opa(&styles->msgbox, LV_OPA_COVER);
	lv_style_set_border_width(&styles->msgbox, 1);
	lv_style_set_border_color(&styles->msgbox, UI_COLOR_SECONDARY_DARK);
	lv_style_set_shadow_width(&styles->msgbox, 30);
	lv_style_set_shadow_spread(&styles->msgbox, 0);
	lv_style_set_shadow_color(&styles->msgbox, UI_COLOR_SECONDARY_DARK);
	lv_style_set_shadow_opa(&styles->msgbox, LV_OPA_50);
	lv_style_set_max_width(&styles->msgbox, 350);
	lv_style_set_pad_row(&styles->msgbox, 20);
	lv_style_set_pad_column(&styles->msgbox, 20);
	lv_style_set_align(&styles->msgbox, LV_ALIGN_CENTER);
	lv_style_set_pad_all(&styles->msgbox, 30);

	style_init_reset(&styles->msgbox_items);
	lv_style_set_align(&styles->msgbox_items, LV_ALIGN_CENTER);

	style_init_reset(&styles->msgbox_btn_matrix);
	lv_style_set_pad_column(&styles->msgbox_btn_matrix, 20);

	style_init_reset(&styles->msgbox_btn);
	lv_style_set_pad_all(&styles->msgbox_btn, 20);
	lv_style_set_radius(&styles->msgbox_btn, UI_ITEM_RADIUS);
	// lv_style_set_bg_color(&styles->msgbox_btn, UI_COLOR_PRIMARY_DARK);
	lv_style_set_bg_color(&styles->msgbox_btn, UI_COLOR_SECONDARY_DARK);
	lv_style_set_text_font(&styles->msgbox_btn, &lv_font_montserrat_18);
	// lv_style_set_text_color(&styles->msgbox_btn, lv_color_white());
	lv_style_set_text_color(&styles->msgbox_btn, lv_color_black());
	lv_style_set_text_align(&styles->msgbox_btn, LV_TEXT_ALIGN_CENTER);
	lv_style_set_bg_opa(&styles->msgbox_btn, LV_OPA_COVER);
	// lv_style_set_border_width(&styles->msgbox_btn, 1);
	// lv_style_set_border_color(&styles->msgbox_btn, UI_COLOR_SECONDARY_DARK);

	style_init_reset(&styles->white);
	lv_style_set_bg_opa(&styles->white, LV_OPA_COVER);
	lv_style_set_bg_color(&styles->white, COLOR_WHITE);
	lv_style_set_line_width(&styles->white, 1);
	lv_style_set_line_color(&styles->white, COLOR_WHITE);
	lv_style_set_arc_width(&styles->white, 2);
	lv_style_set_arc_color(&styles->white, COLOR_WHITE);

	style_init_reset(&styles->light);
	lv_style_set_bg_opa(&styles->light, LV_OPA_COVER);	
	lv_style_set_bg_color(&styles->light, COLOR_LIGHT);
	lv_style_set_line_width(&styles->light, 1);
	lv_style_set_line_color(&styles->light, COLOR_LIGHT);
	lv_style_set_arc_width(&styles->light, 2);
	lv_style_set_arc_color(&styles->light, COLOR_LIGHT);

	style_init_reset(&styles->dark);
	lv_style_set_bg_opa(&styles->dark, LV_OPA_COVER);
	lv_style_set_bg_color(&styles->dark, COLOR_DARK);
	lv_style_set_line_width(&styles->dark, 1);
	lv_style_set_line_color(&styles->dark, COLOR_DARK);
	lv_style_set_arc_width(&styles->dark, 2);
	lv_style_set_arc_color(&styles->dark, COLOR_DARK);
	lv_style_set_text_align(&styles->dark, LV_TEXT_ALIGN_CENTER);

	style_init_reset(&styles->dim);
	lv_style_set_bg_opa(&styles->dim, LV_OPA_COVER);
	lv_style_set_bg_color(&styles->dim, COLOR_DIM);
	lv_style_set_line_width(&styles->dim, 1);
	lv_style_set_line_color(&styles->dim, COLOR_DIM);
	lv_style_set_arc_width(&styles->dim, 2);
	lv_style_set_arc_color(&styles->dim, COLOR_DIM);

#if LV_USE_ARC || LV_USE_COLORWHEEL
	style_init_reset(&styles->arc_line);
	lv_style_set_arc_width(&styles->arc_line, 6);
	style_init_reset(&styles->arc_knob);
	lv_style_set_pad_all(&styles->arc_knob, 5);
#endif

#if LV_USE_TEXTAREA
	style_init_reset(&styles->ta_cursor);
	lv_style_set_border_side(&styles->ta_cursor, LV_BORDER_SIDE_LEFT);
	lv_style_set_border_color(&styles->ta_cursor, COLOR_DIM);
	lv_style_set_border_width(&styles->ta_cursor, 2);
	lv_style_set_bg_opa(&styles->ta_cursor, LV_OPA_TRANSP);
	lv_style_set_anim_time(&styles->ta_cursor, 500);
#endif

	custom_styles_init();
}


/**********************
 *   GLOBAL FUNCTIONS
 **********************/

lv_style_t *get_style_heading_1()
{
	// lv_style_set_text_color(&_style_heading_1, LV_STATE_DEFAULT, heading_color);
	return &_style_heading_1;
}

lv_style_t *get_style_heading_2()
{
	// lv_style_set_text_color(&_style_heading_2, LV_STATE_DEFAULT, heading_color);
	return &_style_heading_2;
}

lv_style_t *get_style_heading_3()
{
	// lv_style_set_text_color(&_style_heading_3, LV_STATE_DEFAULT, heading_color);
	return &_style_heading_3;
}

lv_style_t *get_style_heading_4()
{
	// lv_style_set_text_color(&_style_heading_4, LV_STATE_DEFAULT, heading_color);
	return &_style_heading_4;
}

lv_style_t *get_style_heading_5()
{
	// lv_style_set_text_color(&_style_heading_5, LV_STATE_DEFAULT, heading_color);
	return &_style_heading_5;
}

lv_style_t *get_style_obj_basic()
{
	return &_style_obj_basic;
}

lv_color_t get_color_pos()
{
	return lv_color_make(0, 230, 118);
}

lv_color_t get_color_warn()
{
	return lv_color_make(255, 171, 73);
}

lv_color_t get_color_neg()
{
	return lv_color_make(255, 23, 68);
}

bool lv_theme_basic_is_inited(void)
{
	return  LV_GC_ROOT(_lv_theme_basic_styles) == NULL ? false : true;
}

lv_theme_t * lv_theme_basic_init(lv_disp_t * disp)
{

	/*This trick is required only to avoid the garbage collection of
	 *styles' data if LVGL is used in a binding (e.g. Micropython)
	 *In a general case styles could be in simple `static lv_style_t my_style...` variables*/
	if(!lv_theme_basic_is_inited()) {
		inited = false;
		LV_GC_ROOT(_lv_theme_basic_styles) = lv_mem_alloc(sizeof(my_theme_styles_t));
		styles = (my_theme_styles_t *)LV_GC_ROOT(_lv_theme_basic_styles);
	}

	theme.disp = disp;
	theme.font_small = LV_FONT_DEFAULT;
	theme.font_normal = LV_FONT_DEFAULT;
	theme.font_large = LV_FONT_DEFAULT;
	theme.apply_cb = theme_apply;

	style_init();

	if(disp == NULL || lv_disp_get_theme(disp) == &theme) {
		lv_obj_report_style_change(NULL);
	}

	inited = true;

	return (lv_theme_t *)&theme;
}


static void theme_apply(lv_theme_t * th, lv_obj_t * obj)
{
	LV_UNUSED(th);

	if(lv_obj_get_parent(obj) == NULL) {
		lv_obj_add_style(obj, &styles->scr, 0);
		lv_obj_add_style(obj, &styles->scrollbar, LV_PART_SCROLLBAR);
		return;
	}

	if(lv_obj_check_type(obj, &lv_obj_class)) {
#if LV_USE_TABVIEW
		lv_obj_t * parent = lv_obj_get_parent(obj);
		/*Tabview content area*/
		if(lv_obj_check_type(parent, &lv_tabview_class)) {
			lv_obj_add_style(obj, &styles->scr, 0);
			return;
		}
		/*Tabview pages*/
		else if(lv_obj_check_type(lv_obj_get_parent(parent), &lv_tabview_class)) {
			lv_obj_add_style(obj, &styles->scr, 0);
			lv_obj_add_style(obj, &styles->scrollbar, LV_PART_SCROLLBAR);
			return;
		}
#endif

#if LV_USE_WIN
		/*Header*/
		if(lv_obj_get_index(obj) == 0 && lv_obj_check_type(lv_obj_get_parent(obj), &lv_win_class)) {
			lv_obj_add_style(obj, &styles->light, 0);
			return;
		}
		/*Content*/
		else if(lv_obj_get_index(obj) == 1 && lv_obj_check_type(lv_obj_get_parent(obj), &lv_win_class)) {
			lv_obj_add_style(obj, &styles->light, 0);
			lv_obj_add_style(obj, &styles->scrollbar, LV_PART_SCROLLBAR);
			return;
		}
#endif
		lv_obj_add_style(obj, &styles->white, 0);
		lv_obj_add_style(obj, &styles->scrollbar, LV_PART_SCROLLBAR);
	}
#if LV_USE_BTN
	else if(lv_obj_check_type(obj, &lv_btn_class)) {
		lv_obj_add_style(obj, &styles->dark, 0);
	}
#endif

// #if LV_USE_LABEL
// 	else if(lv_obj_check_type(obj, &lv_label_class)) {
// 		lv_obj_add_style(obj, &styles->light, 0);
// 	}
// #endif

#if LV_USE_BTNMATRIX
	else if(lv_obj_check_type(obj, &lv_btnmatrix_class)) {
#if LV_USE_MSGBOX
		if(lv_obj_check_type(lv_obj_get_parent(obj), &lv_msgbox_class)) {
			lv_obj_add_style(obj, &styles->msgbox_btn_matrix, LV_PART_MAIN);
			lv_obj_add_style(obj, &styles->msgbox_btn, LV_PART_ITEMS);
			// lv_obj_set_style_bg_color(obj, lv_color_lighten(UI_COLOR_PRIMARY_DARK, 20), LV_PART_ITEMS | LV_STATE_PRESSED);
			lv_obj_set_style_bg_color(obj, lv_color_darken(UI_COLOR_SECONDARY_DARK, 50), LV_PART_ITEMS | LV_STATE_PRESSED);
			return;
		}
#endif
#if LV_USE_TABVIEW
		if(lv_obj_check_type(lv_obj_get_parent(obj), &lv_tabview_class)) {
			lv_obj_add_style(obj, &styles->light, LV_PART_ITEMS);
			return;
		}
#endif
		lv_obj_add_style(obj, &styles->white, 0);
		lv_obj_add_style(obj, &styles->light, LV_PART_ITEMS);
	}
#endif

#if LV_USE_BAR
	else if(lv_obj_check_type(obj, &lv_bar_class)) {
		lv_obj_add_style(obj, &styles->light, 0);
		lv_obj_add_style(obj, &styles->dark, LV_PART_INDICATOR);
	}
#endif

#if LV_USE_SLIDER
	else if(lv_obj_check_type(obj, &lv_slider_class)) {
		lv_obj_add_style(obj, &styles->light, 0);
		lv_obj_add_style(obj, &styles->dark, LV_PART_INDICATOR);
		lv_obj_add_style(obj, &styles->dim, LV_PART_KNOB);
	}
#endif

#if LV_USE_TABLE
	else if(lv_obj_check_type(obj, &lv_table_class)) {
		lv_obj_add_style(obj, &styles->scrollbar, LV_PART_SCROLLBAR);
		lv_obj_add_style(obj, &styles->light, LV_PART_ITEMS);
	}
#endif

#if LV_USE_CHECKBOX
	else if(lv_obj_check_type(obj, &lv_checkbox_class)) {
		lv_obj_add_style(obj, &styles->light, LV_PART_INDICATOR);
		lv_obj_add_style(obj, &styles->dark, LV_PART_INDICATOR | LV_STATE_CHECKED);
	}
#endif

#if LV_USE_SWITCH
	else if(lv_obj_check_type(obj, &lv_switch_class)) {
		lv_obj_add_style(obj, &styles->light, 0);
		lv_obj_add_style(obj, &styles->dim, LV_PART_KNOB);
	}
#endif

#if LV_USE_CHART
	else if(lv_obj_check_type(obj, &lv_chart_class)) {
		lv_obj_add_style(obj, &styles->white, 0);
		lv_obj_add_style(obj, &styles->scrollbar, LV_PART_SCROLLBAR);
		lv_obj_add_style(obj, &styles->light, LV_PART_ITEMS);
		lv_obj_add_style(obj, &styles->dark, LV_PART_TICKS);
		lv_obj_add_style(obj, &styles->dark, LV_PART_CURSOR);
	}
#endif

#if LV_USE_ROLLER
	else if(lv_obj_check_type(obj, &lv_roller_class)) {
		lv_obj_add_style(obj, &styles->light, 0);
		lv_obj_add_style(obj, &styles->dark, LV_PART_SELECTED);
	}
#endif

#if LV_USE_DROPDOWN
	else if(lv_obj_check_type(obj, &lv_dropdown_class)) {
		lv_obj_add_style(obj, &styles->white, 0);
	}
	else if(lv_obj_check_type(obj, &lv_dropdownlist_class)) {
		lv_obj_add_style(obj, &styles->white, 0);
		lv_obj_add_style(obj, &styles->scrollbar, LV_PART_SCROLLBAR);
		lv_obj_add_style(obj, &styles->light, LV_PART_SELECTED);
		lv_obj_add_style(obj, &styles->dark, LV_PART_SELECTED | LV_STATE_CHECKED);
	}
#endif

#if LV_USE_ARC
	else if(lv_obj_check_type(obj, &lv_arc_class)) {
		lv_obj_add_style(obj, &styles->light, 0);
		lv_obj_add_style(obj, &styles->transp, 0);
		lv_obj_add_style(obj, &styles->arc_line, 0);
		lv_obj_add_style(obj, &styles->dark, LV_PART_INDICATOR);
		lv_obj_add_style(obj, &styles->arc_line, LV_PART_INDICATOR);
		lv_obj_add_style(obj, &styles->dim, LV_PART_KNOB);
		lv_obj_add_style(obj, &styles->arc_knob, LV_PART_KNOB);
	}
#endif

#if LV_USE_SPINNER
	else if(lv_obj_check_type(obj, &lv_spinner_class)) {
		lv_obj_add_style(obj, &styles->light, 0);
		lv_obj_add_style(obj, &styles->transp, 0);
		lv_obj_add_style(obj, &styles->arc_line, 0);
		lv_obj_add_style(obj, &styles->dark, LV_PART_INDICATOR);
		lv_obj_add_style(obj, &styles->arc_line, LV_PART_INDICATOR);
	}
#endif

#if LV_USE_COLORWHEEL
	else if(lv_obj_check_type(obj, &lv_colorwheel_class)) {
		lv_obj_add_style(obj, &styles->light, 0);
		lv_obj_add_style(obj, &styles->transp, 0);
		lv_obj_add_style(obj, &styles->arc_line, 0);
		lv_obj_add_style(obj, &styles->dim, LV_PART_KNOB);
		lv_obj_add_style(obj, &styles->arc_knob, LV_PART_KNOB);
	}
#endif

#if LV_USE_METER
	else if(lv_obj_check_type(obj, &lv_meter_class)) {
		lv_obj_add_style(obj, &styles->light, 0);
	}
#endif

#if LV_USE_TEXTAREA
	else if(lv_obj_check_type(obj, &lv_textarea_class)) {
		lv_obj_add_style(obj, &styles->white, 0);
		lv_obj_add_style(obj, &styles->scrollbar, LV_PART_SCROLLBAR);
		lv_obj_add_style(obj, &styles->ta_cursor, LV_PART_CURSOR | LV_STATE_FOCUSED);
	}
#endif

#if LV_USE_CALENDAR
	else if(lv_obj_check_type(obj, &lv_calendar_class)) {
		lv_obj_add_style(obj, &styles->light, 0);
	}
#endif

#if LV_USE_KEYBOARD
	else if(lv_obj_check_type(obj, &lv_keyboard_class)) {
		lv_obj_add_style(obj, &styles->scr, 0);
		lv_obj_add_style(obj, &styles->white, LV_PART_ITEMS);
		lv_obj_add_style(obj, &styles->light, LV_PART_ITEMS | LV_STATE_CHECKED);
	}
#endif
#if LV_USE_LIST
	else if(lv_obj_check_type(obj, &lv_list_class)) {
		lv_obj_add_style(obj, &styles->light, 0);
		lv_obj_add_style(obj, &styles->scrollbar, LV_PART_SCROLLBAR);
		return;
	}
	else if(lv_obj_check_type(obj, &lv_list_text_class)) {

	}
	else if(lv_obj_check_type(obj, &lv_list_btn_class)) {
		lv_obj_add_style(obj, &styles->dark, 0);

	}
#endif
#if LV_USE_MSGBOX
	else if(lv_obj_check_type(obj, &lv_msgbox_class)) {
		lv_obj_add_style(obj, &styles->msgbox, LV_PART_MAIN);
		lv_obj_add_style(obj, &styles->msgbox_items, LV_PART_ITEMS);
		lv_obj_set_flex_align(obj, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		return;
	}
#endif
#if LV_USE_SPINBOX
	else if(lv_obj_check_type(obj, &lv_spinbox_class)) {
		lv_obj_add_style(obj, &styles->light, 0);
		lv_obj_add_style(obj, &styles->dark, LV_PART_CURSOR);
	}
#endif
#if LV_USE_TILEVIEW
	else if(lv_obj_check_type(obj, &lv_tileview_class)) {
		lv_obj_add_style(obj, &styles->scr, 0);
		lv_obj_add_style(obj, &styles->scrollbar, LV_PART_SCROLLBAR);
	}
	else if(lv_obj_check_type(obj, &lv_tileview_tile_class)) {
		lv_obj_add_style(obj, &styles->scrollbar, LV_PART_SCROLLBAR);
	}
#endif

#if LV_USE_COLORWHEEL
	else if(lv_obj_check_type(obj, &lv_colorwheel_class)) {
		lv_obj_add_style(obj, &styles->light, 0);
		lv_obj_add_style(obj, &styles->light, LV_PART_KNOB);
	}
#endif

#if LV_USE_LED
	else if(lv_obj_check_type(obj, &lv_led_class)) {
		lv_obj_add_style(obj, &styles->light, 0);
	}
#endif
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

static void style_init_reset(lv_style_t * style)
{
	if(inited) {
		lv_style_reset(style);
	}
	else {
		lv_style_init(style);
	}
}

}
