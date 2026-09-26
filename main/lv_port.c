#ifndef CONFIG_SCREENLESS

#include "esp_heap_caps.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_cache.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lvgl.h"
#include "lv_port.h"
#include "bsp_board.h"
#include "bsp_lcd.h"
#include "indev/indev.h"
#include "sdkconfig.h"
#include "backlight_bridge.h"


#define LV_PORT_BUFFER_HEIGHT           (brd->LCD_HEIGHT)
#define LV_PORT_BUFFER_MALLOC           (MALLOC_CAP_SPIRAM)
// #define LV_PORT_BUFFER_HEIGHT           (100)
// #define LV_PORT_BUFFER_MALLOC           (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)

#define LV_PORT_TASK_DELAY_MS           (5)


static char *TAG = "lvgl_port";
static lv_disp_drv_t disp_drv;
static lv_indev_t *indev_touchpad = NULL;
static lv_indev_t *indev_button = NULL;
static SemaphoreHandle_t lvgl_mutex = NULL;

static TaskHandle_t lvgl_task_handle;
static const char LVGL_TASK_NAME[] = "lvgl";
static EXT_RAM_BSS_ATTR StackType_t _task_stack[4096 * 4] = {};
static StaticTask_t _task_buff;

#ifndef CONFIG_LCD_TASK_PRIORITY
#define CONFIG_LCD_TASK_PRIORITY    5
#endif

static void lv_port_disp_init(void);
static void lv_port_indev_init(void);
static bool lv_port_flush_ready(void *arg);
static bool lv_port_flush_is_last(void);
static IRAM_ATTR void touchpad_read(lv_indev_drv_t *indev_drv, lv_indev_data_t *data);
static void disp_flush(lv_disp_drv_t *disp_drv, const lv_area_t *area, lv_color_t *color_p);
static esp_err_t lv_port_tick_init(void);
static void lvgl_task(void *args);
static void lv_port_direct_mode_copy(void);

void lv_port_init(void)
{
    lv_init();
    lv_port_disp_init();
    lv_port_indev_init();
    lv_port_tick_init();

    lvgl_mutex = xSemaphoreCreateMutex();

    lvgl_task_handle = xTaskCreateStaticPinnedToCore(lvgl_task, LVGL_TASK_NAME, sizeof(_task_stack), NULL, CONFIG_LCD_TASK_PRIORITY, _task_stack, &_task_buff, 1);
}

void lv_port_sem_take(void)
{
    TaskHandle_t task = xTaskGetCurrentTaskHandle();
    if (lvgl_task_handle != task) {
        xSemaphoreTake(lvgl_mutex, portMAX_DELAY);
    }
}

void lv_port_sem_give(void)
{
    TaskHandle_t task = xTaskGetCurrentTaskHandle();
    if (lvgl_task_handle != task) {
        xSemaphoreGive(lvgl_mutex);
    }
}

/**
 * @brief Tell LVGL that LCD flush done.
 *
 * @return true Call `portYIELD_FROM_ISR()` after esp-lcd ISR return.
 * @return false Do nothing after esp-lcd ISR return.v
 */
static bool lv_port_flush_ready(void *arg)
{
    /* Inform the graphics library that you are ready with the flushing */
    lv_disp_flush_ready(&disp_drv);

    /* portYIELD_FROM_ISR (true) or not (false). */
    return false;
}

static bool lv_port_flush_is_last(void)
{
    return lv_disp_flush_is_last(&disp_drv);
}

/**
 * @brief LCD flush function callback for LVGL.
 *
 * @param disp_drv
 * @param area
 * @param color_p
 */
static void disp_flush(lv_disp_drv_t *disp_drv, const lv_area_t *area, lv_color_t *color_p)
{
    (void)disp_drv;

    /*The most simple case (but also the slowest) to put all pixels to the screen one-by-one*/
    bsp_lcd_flush(area->x1, area->y1, area->x2 + 1, area->y2 + 1, (uint8_t *) color_p);
}

static void button_read(lv_indev_drv_t *indev_drv, lv_indev_data_t *data)
{
    // static uint8_t prev_btn_id = 0;
    static uint32_t last_key = 0;

    /* Read touch point(s) via touch IC */
    indev_data_t indev_data;
    if (ESP_OK != indev_get_major_value(&indev_data)) {
        ESP_LOGE(TAG, "Failed read input device value");
        return;
    }

    /*Get the pressed button's ID*/
    if (indev_data.btn_val & 0x02) {
        last_key = LV_KEY_ENTER;
        data->state = LV_INDEV_STATE_PRESSED;
        ESP_LOGD(TAG, "ok");
    } else if (indev_data.btn_val & 0x04) {
        data->state = LV_INDEV_STATE_PRESSED;
        last_key = LV_KEY_PREV;
        ESP_LOGD(TAG, "prev");
    } else if (indev_data.btn_val & 0x01) {
        data->state = LV_INDEV_STATE_PRESSED;
        last_key = LV_KEY_NEXT;
        ESP_LOGD(TAG, "next");
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
    data->key = last_key;
}

/**
 * @brief Read touchpad data.
 *
 * @param indev_drv
 * @param data
 * @return IRAM_ATTR
 */
static IRAM_ATTR void touchpad_read(lv_indev_drv_t *indev_drv, lv_indev_data_t *data)
{
    uint8_t tp_num = 0;
    static uint16_t x = 0, y = 0, btn_val = 0;
    /* Read touch point(s) via touch IC */
    indev_data_t indev_data;
    if (ESP_OK != indev_get_major_value(&indev_data)) {
        return;
    }
    /* FT series touch IC might return 0xff before first touch. */
    if (indev_data.pressed) {
        data->state = LV_INDEV_STATE_PR;
        data->point.x = CONFIG_LCD_EVB_SCREEN_WIDTH - indev_data.x;
        data->point.y = CONFIG_LCD_EVB_SCREEN_HEIGHT - indev_data.y;
        x = data->point.x;
        y = data->point.y;

        // indicator_display_sleep_restart();
    } else {
        data->state = LV_INDEV_STATE_REL;
        data->point.x = x;
        data->point.y = y;
    }
    //ESP_LOGI(TAG, "Touch (%u) : [%3u, %3u] - 0x%02X", indev_data.pressed, data->point.x, data->point.y, indev_data.btn_val);
}

/**
 * @brief Initialize display driver for LVGL.
 *
 */
static void lv_port_disp_init(void)
{
    static lv_disp_draw_buf_t disp_buf; // contains internal graphic buffer(s) called draw buffer(s)
    const board_res_desc_t *brd = bsp_board_get_description();

    void *buf1 = NULL;
    void *buf2 = NULL;
    int buffer_size;
#if CONFIG_LCD_AVOID_TEAR
    buffer_size = brd->LCD_WIDTH * brd->LCD_HEIGHT;
    bsp_lcd_get_frame_buffer(&buf1, &buf2);
#else
    buffer_size = brd->LCD_WIDTH * LV_PORT_BUFFER_HEIGHT;
    buf1 = heap_caps_malloc(buffer_size * sizeof(lv_color_t), LV_PORT_BUFFER_MALLOC);
    assert(buf1);
#endif
    // initialize LVGL draw buffers
    lv_disp_draw_buf_init(&disp_buf, buf1, buf2, buffer_size);

    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = brd->LCD_WIDTH;
    disp_drv.ver_res = brd->LCD_HEIGHT;
    disp_drv.flush_cb = disp_flush;
    disp_drv.draw_buf = &disp_buf;
#if CONFIG_LCD_LVGL_FULL_REFRESH
    disp_drv.full_refresh = 1;
#elif CONFIG_LCD_LVGL_DIRECT_MODE
    disp_drv.direct_mode = 1;
#endif

    /* Use lcd_trans_done_cb to inform the graphics library that flush already done */
    bsp_lcd_set_cb(lv_port_flush_ready, NULL);

#if CONFIG_LCD_LVGL_DIRECT_MODE
    bsp_lcd_flush_is_last_register(lv_port_flush_is_last);
    bsp_lcd_direct_mode_register(lv_port_direct_mode_copy);
#endif

    lv_disp_drv_register(&disp_drv);
}

/**
 * @brief Initialize input device for LVGL.
 *
 * @return esp_err_t
 */
static void lv_port_indev_init(void)
{
    /**
     * Here you will find example implementation of input devices supported by LittelvGL:
     *  - Touchpad
     *  - Mouse (with cursor support)
     *  - Keypad (supports GUI usage only with key)
     *  - Encoder (supports GUI usage only with: left, right, push)
     *  - Button (external buttons to press points on the screen)
     *
     *  The `..._read()` function are only examples.
     *  You should shape them according to your hardware
     */
    static lv_indev_drv_t indev_drv_tp;
    static lv_indev_drv_t indev_drv_btn;

    /* Initialize your touchpad if you have */
    const board_res_desc_t *brd = bsp_board_get_description();
    if (brd->BSP_INDEV_IS_TP) {
        ESP_LOGI(TAG, "Add TP input device to LVGL");
        lv_indev_drv_init(&indev_drv_tp);
        indev_drv_tp.type = LV_INDEV_TYPE_POINTER;
        indev_drv_tp.read_cb = touchpad_read;
        indev_touchpad = lv_indev_drv_register(&indev_drv_tp);

    } else {
        ESP_LOGI(TAG, "Add KEYPAD input device to LVGL");
        lv_indev_drv_init(&indev_drv_btn);
        indev_drv_btn.type = LV_INDEV_TYPE_KEYPAD;
        indev_drv_btn.read_cb = button_read;
        indev_button = lv_indev_drv_register(&indev_drv_btn);
    }

#if CONFIG_LV_PORT_SHOW_MOUSE_CURSOR
    LV_IMG_DECLARE(mouse_cursor_icon)
    lv_obj_t *cursor_obj = lv_img_create(lv_scr_act());  /*Create an image object for the cursor */
    lv_img_set_src(cursor_obj, &mouse_cursor_icon);           /*Set the image source*/
    lv_indev_set_cursor(indev_touchpad, cursor_obj);             /*Connect the image  object to the driver*/
#endif
}

/**
 * @brief Task to generate ticks for LVGL.
 *
 * @param pvParam Not used.
 */
static void lv_tick_inc_cb(void *data)
{
    uint32_t tick_inc_period_ms = *((uint32_t *) data);

    lv_tick_inc(tick_inc_period_ms);
}

/**
 * @brief Create tick task for LVGL.
 *
 * @return esp_err_t
 */
static esp_err_t lv_port_tick_init(void)
{
    static const uint32_t tick_inc_period_ms = 2;
    const esp_timer_create_args_t periodic_timer_args = {
        .callback = lv_tick_inc_cb,
        .name = "timer_lv_tick_inc",     /* name is optional, but may help identify the timer when debugging */
        .arg = &tick_inc_period_ms,
        .dispatch_method = ESP_TIMER_TASK,
        .skip_unhandled_events = true,
    };

    esp_timer_handle_t periodic_timer;
    ESP_ERROR_CHECK(esp_timer_create(&periodic_timer_args, &periodic_timer));

    /* The timer has been created but is not running yet. Start the timer now */
    ESP_ERROR_CHECK(esp_timer_start_periodic(periodic_timer, tick_inc_period_ms * 1000));

    return ESP_OK;
}

#if CONFIG_LCD_LVGL_DIRECT_MODE
/**
 * @brief Copy dirty area from last frame to current frame.
 * @note  It's useful only when enable lvgl direct-mode, and should be called behind the end of flushing in lvgl.
 *
 */
static void lv_port_direct_mode_copy(void)
{
    lv_disp_t *disp_refr = _lv_refr_get_disp_refreshing();

    uint8_t *buf_act = disp_refr->driver->draw_buf->buf_act;
    uint8_t *buf1 = disp_refr->driver->draw_buf->buf1;
    uint8_t *buf2 = disp_refr->driver->draw_buf->buf2;
    int h_res = disp_refr->driver->hor_res;
    int v_res = disp_refr->driver->ver_res;

    uint8_t *fb_from = buf_act;
    uint8_t *fb_to = (fb_from == buf1) ? buf2 : buf1;

    int32_t i;
    lv_coord_t x_start, x_end, y_start, y_end;
    uint32_t copy_bytes_per_line;
    uint32_t bytes_to_flush;
    uint32_t bytes_per_line = h_res * 2;
    uint8_t *from, *to;
    uint8_t *flush_ptr;
    // ESP_LOGI(TAG, "----- Area list ----");
    for(i = 0; i < disp_refr->inv_p; i++) {
        /*Refresh the unjoined areas*/
        if(disp_refr->inv_area_joined[i] == 0) {
            x_start = disp_refr->inv_areas[i].x1;
            x_end = disp_refr->inv_areas[i].x2 + 1;
            y_start = disp_refr->inv_areas[i].y1;
            y_end = disp_refr->inv_areas[i].y2 + 1;

            copy_bytes_per_line= (x_end - x_start) * 2;
            from = fb_from + (y_start * h_res + x_start) * 2;
            to = fb_to + (y_start * h_res + x_start) * 2;
            for (int y = y_start; y < y_end; y++) {
                memcpy(to, from, copy_bytes_per_line);
                from += bytes_per_line;
                to += bytes_per_line;
            }
            bytes_to_flush = (y_end - y_start) * bytes_per_line;
            flush_ptr = fb_to + y_start * bytes_per_line;

            ESP_ERROR_CHECK(esp_cache_msync(flush_ptr, bytes_to_flush,
                                            ESP_CACHE_MSYNC_FLAG_DIR_C2M));
        }
    }
}
#endif

static volatile bool g_lvgl_paused = false;
static lv_obj_t *s_ota_overlay = NULL;

void lv_port_pause(void)
{
    g_lvgl_paused = true;
}

void lv_port_resume(void)
{
    if (g_lvgl_paused) {
        if (s_ota_overlay) {
            lv_obj_del(s_ota_overlay);
            s_ota_overlay = NULL;
        }
        backlight_set_level_percent_c(100, BACKLIGHT_WAKE_OTA);
        g_lvgl_paused = false;
        lv_port_sem_give();
    }
}

void lv_port_show_ota_screen(void)
{
    lv_port_sem_take();

    if (!s_ota_overlay) {
        lv_obj_t *scr = lv_scr_act();
        s_ota_overlay = lv_obj_create(scr);
        lv_obj_set_size(s_ota_overlay, LV_HOR_RES, LV_VER_RES);
        lv_obj_align(s_ota_overlay, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_bg_color(s_ota_overlay, lv_color_hex(0x0f172a), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s_ota_overlay, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(s_ota_overlay, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(s_ota_overlay, 0, LV_PART_MAIN);

        lv_obj_t *lbl_title = lv_label_create(s_ota_overlay);
        lv_label_set_text(lbl_title, "FIRMWARE UPDATE");
        lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(lbl_title, lv_color_hex(0xf59e0b), 0);
        lv_obj_align(lbl_title, LV_ALIGN_CENTER, 0, -25);

        lv_obj_t *lbl_msg = lv_label_create(s_ota_overlay);
        lv_label_set_text(lbl_msg, "OTA Flashing in Progress...\nDo NOT turn off power.");
        lv_obj_set_style_text_font(lbl_msg, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(lbl_msg, lv_color_hex(0xf8fafc), 0);
        lv_obj_set_style_text_align(lbl_msg, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(lbl_msg, LV_ALIGN_CENTER, 0, 25);
    }

    // Force LVGL to render frame immediately before pausing
    lv_refr_now(NULL);
    vTaskDelay(pdMS_TO_TICKS(150));

    // Keep backlight ON so the FIRMWARE UPDATE warning remains visible on screen
    backlight_set_level_percent_c(100, BACKLIGHT_WAKE_OTA);

    g_lvgl_paused = true;
    // NOTE: Keep lvgl_mutex held so background tasks calling UiCommon::lock() block safely during OTA
}

/**
 * @brief Task to draw and flush for LVGL.
 *
 * @param args Not used.
 */
static void lvgl_task(void *args)
{
	uint32_t ms_to_next_call = 0;
	while (1) {
		if (g_lvgl_paused) {
			vTaskDelay(pdMS_TO_TICKS(100));
			continue;
		}

		size_t free_heap = esp_get_free_heap_size();
		size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

		// Critically low heap limit: 64KB heap / 32KB PSRAM.
		// If below this, yield the CPU and print a throttled warning to allow the system to recover.
		if (free_heap < 64 * 1024 || free_psram < 32 * 1024) {
			static uint32_t last_warn_time = 0;
			uint32_t now = pdTICKS_TO_MS(xTaskGetTickCount());
			if (now - last_warn_time > 5000) {
				ESP_LOGW(TAG, "Heap critically low (Free: %d KB, PSRAM: %d KB). Suspending LVGL task handler.",
						 (int)(free_heap / 1024), (int)(free_psram / 1024));
				last_warn_time = now;
			}
			vTaskDelay(pdMS_TO_TICKS(100));
			continue;
		}

		xSemaphoreTake(lvgl_mutex, portMAX_DELAY);
        ms_to_next_call = lv_task_handler();
        xSemaphoreGive(lvgl_mutex);

		// Wait for minimum portTICK_PERIOD_MS, because ms_to_next_call can return 0
		if(ms_to_next_call <= portTICK_PERIOD_MS)
		{
			// According to FreeRTOS doc, to ensure min X time delay we need (Xtime + 1tick)
			vTaskDelay(pdMS_TO_TICKS(portTICK_PERIOD_MS) + 1);
		}
		else{
			vTaskDelay(pdMS_TO_TICKS(ms_to_next_call));
		}
	}

	/* A task should NEVER return */
	vTaskDelete(NULL);
}

#else // CONFIG_SCREENLESS

#include "lv_port.h"

void lv_port_init(void) {}
void lv_port_sem_take(void) {}
void lv_port_sem_give(void) {}
void lv_port_pause(void) {}
void lv_port_resume(void) {}
void lv_port_show_ota_screen(void) {}

#endif // CONFIG_SCREENLESS
