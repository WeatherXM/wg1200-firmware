// #include "ui/views/include/ui_view_bsod.h"
// #include "ui/include/ui_common.h"

// namespace UiViewBsod
// {

// void create(char *msg)
// {
//     UiCommon::lock();

//     // BSOD screen
// 	lv_obj_t *main_scr = lv_scr_act();
//     lv_obj_clean(main_scr);

// 	// Style: White, 20
// 	static lv_style_t medium_text;
// 	lv_style_init(&medium_text);
// 	lv_style_set_text_font(&medium_text, &lv_font_montserrat_20);
// 	lv_style_set_text_color(&medium_text, LV_COLOR_WHITE);

// 	// Style: White, 16
// 	static lv_style_t small_text;
// 	lv_style_init(&small_text);
// 	lv_style_set_text_font(&small_text, &lv_font_montserrat_16);
// 	lv_style_set_text_color(&small_text, LV_COLOR_WHITE);

// 	// Sad face
// 	lv_obj_t *lbl_sad = lv_label_create(main_scr);
// 	lv_label_set_text_static(lbl_sad, ":-(");
// 	lv_label_set_align(lbl_sad, LV_LABEL_ALIGN_CENTER);
// 	lv_obj_align(lbl_sad, LV_ALIGN_TOP_LEFT, 30, 25);

// 	// White, large
// 	static lv_style_t face_style;
// 	lv_style_init(&face_style);
// 	lv_style_set_text_font(&face_style, &lv_font_montserrat_48);
// 	lv_style_set_text_color(&face_style, LV_COLOR_WHITE);
// 	lv_obj_add_style(lbl_sad, LV_PART_MAIN, &face_style);

// 	// Something went wrong
// 	lv_obj_t *lbl_wrong = lv_label_create(main_scr, NULL);
// 	lv_label_set_text_static(lbl_wrong, "Someone's under\nthe weather");
// 	lv_label_set_align(lbl_wrong, LV_LABEL_ALIGN_LEFT);
// 	lv_obj_align(lbl_wrong, lbl_sad, LV_ALIGN_OUT_RIGHT_MID, 35, 0);
// 	lv_obj_add_style(lbl_wrong, LV_PART_MAIN, &medium_text);

// 	// A problem has been detected
// 	lv_obj_t *lbl_descr = lv_label_create(main_scr, NULL);
// 	lv_label_set_text_static(lbl_descr, "A problem has been detected");
// 	lv_label_set_align(lbl_descr, LV_LABEL_ALIGN_CENTER);
// 	lv_obj_align(lbl_descr, main_scr, LV_ALIGN_CENTER, 0, 0);
// 	lv_obj_add_style(lbl_descr, LV_PART_MAIN, &small_text);

// 	// Msg
// 	if(msg != NULL)
// 	{
// 		lv_obj_t *lbl_msg = lv_label_create(main_scr, NULL);
// 		lv_label_set_long_mode(lbl_msg, LV_LABEL_LONG_BREAK);
// 		lv_obj_set_size(lbl_msg, 250, 10);
// 		lv_label_set_text(lbl_msg, msg);
// 		lv_label_set_align(lbl_msg, LV_LABEL_ALIGN_LEFT);
// 		lv_obj_align(lbl_msg, lbl_descr, LV_ALIGN_IN_TOP_LEFT, 0, 50);
// 		lv_obj_add_style(lbl_msg, LV_OBJ_PART_MAIN, &small_text);
// 	}

// 	// Set bkg color last
// 	static lv_style_t bg_style;
// 	lv_style_init(&bg_style);

// 	lv_style_set_bg_color(&bg_style, LV_STATE_DEFAULT, lv_color_hex(0x82));  // Original BSOD
// 	// lv_style_set_bg_color(&bg_style, LV_STATE_DEFAULT, lv_color_hex(0x79D8));   // Zoomer BSOD
// 	lv_obj_add_style(main_scr, LV_OBJ_PART_MAIN, &bg_style);

//     UiCommon::unlock();
// }

// }