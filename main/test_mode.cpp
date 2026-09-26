/******************************************************************************
* Self test mode
* Tests various functions for proper operation. Test mode requires a WiFi
* connection, on start tries to connect to an AP with predefined credentials.
* Test result is submitted to TB.
* Used by factory to test devices after production.
******************************************************************************/
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl/lvgl.h"
#include "esp_log.h"
#include "math.h"
#include "mbedtls/base64.h"
#include "esp_wifi.h"
#include "ArduinoJson.h"
#include "ui/include/ui_common.h"
#include "ui/resources/ui_resources.h"
#include "ui/include/ui_types.h"
#include "global_events.h"
#include "test_mode.h"
#include "utils.h"
#include "const.h"
#include "wifi.h"
#include "tb.h"
#include "global_state.h"
#ifndef CONFIG_SCREENLESS
#include "bmp3xx.h"
#endif
#include "pins.h"
#include "ws/include/ws_handler.h"
#include "ws/include/ws_data.h"
// #include "gps.h"

/******************************************************************************
* Local Types
******************************************************************************/
namespace TestMode
{
/**
 * Test id
*/
enum Test
{
	TEST_NONE = -1,
	TEST_ANY = -1,
	TEST_IOT_CLOUD = 1,
	TEST_IPFS,
	TEST_WS,
	TEST_GPS,
	TEST_PRESSURE,
	TEST_DS_STORAGE
};

/** Test result */
enum TestResult
{
	TEST_RESULT_NONE = 0,
	TEST_RESULT_PASS,
	TEST_RESULT_FAIL
};

/** Test run order */
enum TestRunOrder
{
	TEST_ORDER_SEQ,
	TEST_ORDER_PARALLEL
};

/** Data for a single test */
struct TestData
{
	const Test test_id;
	const char *name;
	TestResult result;

	/** Test task function */
	void (*const task) (void *param);

	/** Test should run sequentially or in parallel */
	TestRunOrder order;

	TaskHandle_t task_handle;

	char result_details[100];

	/** Test name label */
	lv_obj_t *lbl_name;
	/** Test status label */
	lv_obj_t *lbl_status;
	/** Test running spinner */
	lv_obj_t *spinner;
};

/** Softkey descriptor */
struct SoftKeyDesc
{
	/** Label object */
	lv_obj_t *lbl;

	/** on click callback */
	void (*click_cb)();
};

/******************************************************************************
* Local functions
******************************************************************************/
void start();
void end();
void cleanup();
void test_task(void *param);
void set_test_status(Test test, const char *status);
void set_test_result(Test test, TestResult res);
void set_test_result_details(Test test, const char *details);
TestData *get_test_data(Test test);
void setup_test_ui();
bool connect_wifi();
void tests_failed();
void tests_passed();
void show_details_msgbox();
void elapsed_timer_task(void *param);
void notify_test_finished(TestData *test_data);
bool wait_test_finished(Test test_id, uint32_t timeout_ms, TestData **data_out = NULL);
RetResult build_test_json(DynamicJsonDocument *json_doc);
void build_details_string(char *buff, int buff_len, bool include_platform = true);

// Tests
void test_ws_task(void *param);
void test_gps_task(void *param);
void test_pressure_task(void *param);
void test_ds_storage_task(void *param);
void test_platform_task(void *param);

/******************************************************************************
* Local vars
******************************************************************************/
const char TAG[] = "Test mode";

/** Main UI container - All elements except title, softkeys are drawn here */
lv_obj_t *_main_container = NULL;

/** Buttons */
UiTypes::ButtonComponent _btn_retry;
UiTypes::ButtonComponent _btn_details;
UiTypes::ButtonComponent _btn_exit;

/** Test task */
TaskHandle_t _test_task_handle = NULL;

/** Update elapsed timer task */
TaskHandle_t _elapsed_timer_task_handle = NULL;

// Text styles
lv_style_t _test_name_style;
lv_style_t _test_status_style;
lv_style_t _test_pass_style;
lv_style_t _test_fail_style;
lv_style_t _softkey_style;

/** Label: Test result (large fail/pass) */
lv_obj_t *_lbl_final_result = NULL;
/** Box: Container for final result label */
lv_obj_t *_box_final_result = NULL;

/** Elapsed timer */
lv_obj_t *_lbl_timer = NULL;

/** WiFi icon*/
lv_obj_t *_icon_wifi = NULL;

/** Test started TS MS*/
int64_t _test_started_ts_ms = 0;

/** Test descriptors array */
TestData TESTS[] = {
	{
		Test::TEST_WS,
		"Weather Station",
		TestResult::TEST_RESULT_NONE,
		test_ws_task,
		TEST_ORDER_PARALLEL
	},
	{
		Test::TEST_GPS,
		"GPS",
		TestResult::TEST_RESULT_NONE,
		test_gps_task,
		TEST_ORDER_PARALLEL
	},
	{
		Test::TEST_PRESSURE,
		"Pressure",
		TestResult::TEST_RESULT_NONE,
		test_pressure_task,
		TEST_ORDER_PARALLEL
	},
	{
		Test::TEST_DS_STORAGE,
		"DS Storage",
		TestResult::TEST_RESULT_NONE,
		test_ds_storage_task,
		TEST_ORDER_PARALLEL
	},
	{
		Test::TEST_IOT_CLOUD,
		"IOT Cloud",
		TestResult::TEST_RESULT_NONE,
		test_platform_task,
		TEST_ORDER_SEQ
	}
};

/** Notifies task of finished test */
QueueHandle_t _test_finished_queue = NULL;

/** Mutex for test data */
SemaphoreHandle_t _sem_test_data = NULL;

/******************************************************************************
* Constants
******************************************************************************/
/** Total number of tests */
const int TEST_COUNT = sizeof(TESTS) / sizeof(TESTS[0]);

/** Total number of softkeys */
const int SOFTKEY_COUNT = 3;

// ============================================================================

/******************************************************************************
 * Handle APP events
 ******************************************************************************/
void event_handler(void *arg, esp_event_base_t base, int32_t id, void *event_data)
{
	if(NULL == event_data)
	{
		return;
	}
	else if(id == (int32_t)GlobalEvents::EventId::WIFI_STATE_CHANGED)
	{
		UiCommon::lock();
		if(*((GlobalEvents::WifiStateChangedEvent *)event_data) == GlobalEvents::WifiStateChangedEvent::CONNECTED)
		{
			lv_img_set_src(_icon_wifi, &UiResources::icon_wifi_ok_full);
			lv_obj_set_style_img_recolor(_icon_wifi, UiThemeWxm::get_color_pos(), LV_PART_MAIN);
		}
		else if(*((GlobalEvents::WifiStateChangedEvent *)event_data) ==
				GlobalEvents::WifiStateChangedEvent::CONNECTED_NO_INTERNET)
		{
			lv_img_set_src(_icon_wifi, &UiResources::icon_wifi_warning);
			lv_obj_set_style_img_recolor(_icon_wifi, UiThemeWxm::get_color_warn(), LV_PART_MAIN);
		}
		else
		{
			lv_img_set_src(_icon_wifi, &UiResources::icon_wifi_error);
			lv_obj_set_style_img_recolor(_icon_wifi, UiThemeWxm::get_color_neg(), LV_PART_MAIN);

		}
		UiCommon::unlock();
	}
}

/******************************************************************************
* Enter test mode
******************************************************************************/
void enter()
{
	ESP_LOGW(TAG, "Entering diagnostics mode");

	//
	// System init
	//
	UiCommon::lock();
	lv_obj_clean(lv_scr_act());
	UiCommon::unlock();

	// Finished tests queue
	_test_finished_queue = xQueueCreate(3, sizeof(TestData*));
	if(_test_finished_queue == NULL)
	{
		ESP_LOGE(TAG, "Could not create queue.");
	}

	// Test data mutex
	_sem_test_data = xSemaphoreCreateMutex();
	if(_sem_test_data == NULL)
	{
		ESP_LOGE(TAG, "Could not create mutex.");
	}

	//
	// Styles
	//
	// Test name label
	lv_style_init(&_test_name_style);
	lv_style_set_text_font(&_test_name_style, &lv_font_montserrat_24);
	lv_style_set_text_color(&_test_name_style, UI_TEXT_COLOR_LIGHT);

	// Test status label default style
	lv_style_init(&_test_status_style);
	lv_style_set_text_font(&_test_status_style, &lv_font_montserrat_24);
	lv_style_set_text_color(&_test_status_style, UI_TEXT_COLOR_LIGHT);

	// Test PASS label
	lv_style_init(&_test_pass_style);
	lv_style_set_text_font(&_test_pass_style, &lv_font_montserrat_24);
	lv_style_set_text_color(&_test_pass_style, TEST_MODE_COLOR_PASS);

	// Test FAIL label
	lv_style_init(&_test_fail_style);
	lv_style_set_text_font(&_test_fail_style, &lv_font_montserrat_24);
	lv_style_set_text_color(&_test_fail_style,  TEST_MODE_COLOR_FAIL);

	// Softkey label
	lv_style_init(&_softkey_style);
	lv_style_set_text_font(&_softkey_style, &lv_font_montserrat_24);
	lv_style_set_text_color(&_softkey_style, UI_TEXT_COLOR_LIGHT);

	//
	// Page title
	//
	UiCommon::lock();

	lv_obj_t *scr = lv_scr_act();	

	lv_obj_t *lbl_title = lv_label_create(scr);
	lv_label_set_text_static(lbl_title, "Diagnostics");
	lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_16, 0);
	lv_obj_set_style_text_align(lbl_title, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_color(lbl_title, UI_TEXT_COLOR_LIGHT, 0);
	lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, 0, 10);

	// Elapsed timer label
	_lbl_timer = lv_label_create(scr);
	lv_label_set_text_static(_lbl_timer, "0:00");
	lv_obj_set_style_text_color(_lbl_timer, UI_TEXT_COLOR_LIGHT, 0);
	lv_obj_set_style_text_font(_lbl_timer, &lv_font_montserrat_16, 0);
	lv_obj_align(_lbl_timer, LV_ALIGN_TOP_LEFT, 20, 10);

	_icon_wifi = lv_img_create(scr);
	lv_img_set_src(_icon_wifi, &UiResources::icon_wifi_error);
	lv_obj_set_style_img_recolor_opa(_icon_wifi, LV_OPA_COVER, 0);
	lv_obj_set_style_img_recolor(_icon_wifi, UiThemeWxm::get_color_neg(), 0);
	lv_obj_align(_icon_wifi, LV_ALIGN_TOP_RIGHT, -17, 5);

	// &UiResources::icon_wifi_error

	//
	// Set up main container
	//

	_main_container = lv_obj_create(scr);
	lv_obj_set_size(_main_container, 440, 344);
	// lv_obj_set_click(_main_container, false);
	lv_obj_set_style_bg_color(_main_container, UI_COLOR_PRIMARY_DARK, 0);
	lv_obj_set_style_bg_opa(_main_container, LV_OPA_100, 0);
	lv_obj_set_style_border_width(_main_container, 0, 0);
	lv_obj_set_style_radius(_main_container, UI_ITEM_RADIUS, 0);
	lv_obj_align(_main_container, LV_ALIGN_TOP_MID, 0, 40);

	_btn_details.create(scr)
		.title("Details")
		.size(100, 50)
		.hidden(true)
		.align(LV_ALIGN_BOTTOM_LEFT, 20, -20)
		.set_cb([](lv_event_t *event)
		{
			lv_event_code_t  event_code = lv_event_get_code(event);
			if(event_code != LV_EVENT_CLICKED)
			{
				return;
			}
			show_details_msgbox();
		}, NULL);

	_btn_retry.create(scr)
		.title("Retry")
		.size(100, 50)
		.hidden(true)
		.align(LV_ALIGN_BOTTOM_MID, 0, -20)
		.set_cb([](lv_event_t *event)
		{
			lv_event_code_t  event_code = lv_event_get_code(event);
			if(event_code != LV_EVENT_CLICKED)
			{
				return;
			}
			start();
		}, NULL);

	_btn_exit.create(scr)
		.title("Exit")
		.size(100, 50)
		.align(LV_ALIGN_BOTTOM_RIGHT, -20, -20)
		.set_cb([](lv_event_t *event)
		{
			lv_event_code_t  event_code = lv_event_get_code(event);
			if(event_code != LV_EVENT_CLICKED)
			{
				return;
			}
			Utils::safe_restart(Utils::RestartReason::TEST_MODE_EXIT);
		}, NULL);

	UiCommon::unlock();

	//
	// Initialize components as in regular boot
	//
	// Pressure::init();
	// Gps::init();
	WsHandler::init();
	// FoSniffer::set_radio_freq(FoSnifferFreq::SNIFFER_FREQ_868);
	// FoSniffer::stop_capture();
	// Wifi::init();
	Tb::init();
	GlobalEvents::subscribe(GlobalEvents::EventId::ANY_EVENT, event_handler, NULL);
	start();
}

/******************************************************************************
* Reset and start tests
******************************************************************************/
void start()
{
	cleanup();
	_test_started_ts_ms = Utils::get_tick_ms();
	// Create main test task
	if(xTaskCreatePinnedToCore(test_task, "test_task", 16384, NULL, 2, &_test_task_handle, 1) != pdPASS)
	{
		ESP_LOGE(TAG, "Could not create test task");
		return;
	}
}

/******************************************************************************
* Testing ended
******************************************************************************/
void end()
{
	ESP_LOGI(TAG, "Test end");
	// Delete elapsed timer
	_test_started_ts_ms = 0;
	if(_elapsed_timer_task_handle != NULL)
	{
		vTaskDelete(_elapsed_timer_task_handle);
		_elapsed_timer_task_handle = NULL;
	}
}

/******************************************************************************
* Clean up before starting tests
******************************************************************************/
void cleanup()
{
	ESP_LOGI(TAG, "Cleaning up");
	lv_obj_set_style_bg_color(_main_container, UI_COLOR_PRIMARY_DARK, 0);
	lv_obj_set_style_border_width(_main_container, 0, 0);
	_btn_retry.hidden(true);
	_btn_details.hidden(true);

	// Init data
	for (size_t i = 0; i < TEST_COUNT; i++)
	{
		TestData *data = &TESTS[i];

		data->result = TEST_RESULT_NONE;
		data->result_details[0] = '\0';
		data->task_handle = NULL;
	}
}

/******************************************************************************
* Connect to a WiFi network with preset credentials
* @return false on failure to connect
******************************************************************************/
bool connect_wifi()
{
	bool ret;

	if(!Wifi::is_connected())
	{
		char wifi_ssid[Utils::WIFI_SSID_STRING_SIZE] = "";
		char wifi_pass[Utils::WIFI_PASSWORD_STRING_SIZE] = "";
		bool is_wifi_default = false;
		if(Utils::wifi_manager_get_creds(wifi_ssid, wifi_pass) != RET_OK)
		{
			ESP_LOGE(TAG, "wifi_manager_get_creds failed");
		}
		ESP_LOGW(TAG, "SSID: %s", wifi_ssid); // delete
		ESP_LOGW(TAG, "PASS: %s", wifi_pass); // delete
		if(!strlen(wifi_ssid) && !strlen(wifi_pass))
		{
			is_wifi_default = true;
			ESP_LOGW(TAG, "Using default ssid/pass for tests");
			strncpy(wifi_ssid, TEST_MODE_WIFI_SSID, sizeof(wifi_ssid));
			strncpy(wifi_pass, TEST_MODE_WIFI_PASS, sizeof(wifi_pass));
		}
		//
		// Set up UI
		//
		UiCommon::lock();
		lv_obj_clean(_main_container);

		lv_obj_t *lbl_desc = lv_label_create(_main_container);		
		lv_obj_set_style_text_font(lbl_desc, &lv_font_montserrat_20, 0);
		lv_obj_set_style_text_color(lbl_desc, UI_TEXT_COLOR_LIGHT, 0);
		lv_label_set_text_fmt(lbl_desc, 
			"Connecting to WIFI network: \n"
			"SSID:  %s \n"
			"Password:  %s", wifi_ssid, is_wifi_default ? wifi_pass : "********");
		lv_obj_align(lbl_desc, LV_ALIGN_TOP_LEFT, 20, 20);

		// Progress label
		lv_obj_t *lbl_prog = lv_label_create(_main_container);
		lv_obj_set_style_text_font(lbl_prog, &lv_font_montserrat_20, 0);
		lv_obj_align_to(lbl_prog, lbl_desc, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 10);

		UiCommon::unlock();

		Wifi::connect(wifi_ssid, wifi_pass, true);
		char prog[10] = "";
		int64_t start_tick = Utils::get_tick_ms();

		while(1)
		{
			if(strlen(prog) == sizeof(prog) - 1)
			{
				prog[0] = '\0';
			}
			strcat(prog, ".");

			UiCommon::lock();
			lv_label_set_text_static(lbl_prog, prog);
			UiCommon::unlock();
			Utils::wait_ms(100);

			if(Wifi::is_connected() || Utils::get_tick_ms() - start_tick > TEST_MODE_WIFI_TIMEOUT_MS)
			{
				break;
			}
		}

		if(Wifi::is_connected())
		{
			UiCommon::lock();
			lv_label_set_text_static(lbl_prog, "Connected!");
			lv_obj_add_style(lbl_prog, &_test_pass_style, 0);
			UiCommon::unlock();

			// Wait a little so user sees the message
			Utils::wait_ms(1000);

			return true;
		}
		else
		{
			UiCommon::lock();
			if(Wifi::get_status() == Wifi::WifiStatus::CONNECTED_NO_IP)
			{
				lv_label_set_text_static(lbl_prog, "Connected to WiFi, no IP obtained!");
			}
			else if(Wifi::get_status() == Wifi::WifiStatus::CONNECTED_NO_INTERNET)
			{
				lv_label_set_text_static(lbl_prog, "Connected to WiFi, no Internet!");
			}
			else
			{
				lv_label_set_text_static(lbl_prog, "Could not connect to WiFi!");
			}
			lv_obj_add_style(lbl_prog, &_test_fail_style, 0);
			UiCommon::unlock();

			return false;
		}
	}

	// Already connected
	return true;
}

/******************************************************************************
* Clear UI and set up elements
******************************************************************************/
void setup_test_ui()
{
	//
	// Set up UI
	//
	UiCommon::lock();

	lv_obj_t *scr = lv_scr_act();
	lv_obj_clean(_main_container);

	int prev_y = 30;
	for (int i = 0; i < TEST_COUNT; i++)
	{
		TestData *test = &TESTS[i];

		// Test name label
		lv_obj_t *lbl_name = lv_label_create(_main_container);
		// lv_label_set_static_text(lbl_name, test->name);
		lv_label_set_text_fmt(lbl_name, "%s - ", test->name);
		lv_obj_align(lbl_name, LV_ALIGN_TOP_LEFT, 40, prev_y);
		lv_obj_add_style(lbl_name, &_test_name_style, 0);
		test->lbl_name = lbl_name;

		// Test status label
		lv_obj_t *lbl_status = lv_label_create(_main_container);
		lv_label_set_text_static(lbl_status, LV_SYMBOL_PAUSE);
		lv_obj_align_to(lbl_status, lbl_name, LV_ALIGN_OUT_RIGHT_MID, 0, 2);
		lv_obj_add_style(lbl_name, &_test_status_style, 0);
		test->lbl_status = lbl_status;

		test->spinner = lv_spinner_create(_main_container, 1000, 80);
		lv_obj_add_flag(test->spinner, LV_OBJ_FLAG_HIDDEN);
		lv_obj_set_size(test->spinner, 25, 25);
		lv_obj_align_to(test->spinner, lbl_name, LV_ALIGN_OUT_RIGHT_MID, 0, 2);
		// lv_obj_set_style_bg_color(test->spinner, UI_COLOR_BG_DARK);		
		lv_obj_set_style_arc_color(test->spinner, UI_COLOR_BG_DARK, LV_PART_MAIN);
		lv_obj_set_style_arc_rounded(test->spinner, true, LV_PART_INDICATOR);
		lv_obj_set_style_arc_color(test->spinner, UI_COLOR_SECONDARY_DARK, LV_PART_INDICATOR);
		lv_obj_set_style_arc_width(test->spinner, 4, LV_PART_MAIN);
		lv_obj_set_style_arc_width(test->spinner, 4, LV_PART_INDICATOR);

		prev_y += 50;
	}

	// Box container for final OK/Fail label
	_box_final_result = lv_obj_create(_main_container);
	lv_obj_add_flag(_box_final_result, LV_OBJ_FLAG_HIDDEN);
	lv_obj_set_size(_box_final_result, 100, 100);
	lv_obj_set_style_border_width(_box_final_result, 0, 0);
	lv_obj_set_style_radius(_box_final_result, UI_ITEM_RADIUS, 0);
	// lv_obj_align(_box_final_result, scr, LV_ALIGN_IN_RIGHT_MID, 0, 0);
	// lv_obj_align_x(_box_final_result, _main_container, LV_ALIGN_RIGHT_MID, -26);
	// lv_obj_align_mid_y(_box_final_result, scr, LV_ALIGN_RIGHT_MID, 0);
	lv_obj_align(_box_final_result, LV_ALIGN_RIGHT_MID, -40, 0);
	lv_obj_set_style_bg_color(_box_final_result, TEST_MODE_COLOR_PASS, 0);

	// Final OK/FAIL
	_lbl_final_result = lv_label_create(_box_final_result);
	// Style same as pass but with large font
	lv_obj_set_style_text_color(_lbl_final_result, UI_TEXT_COLOR_LIGHT, 0);
	lv_obj_set_style_text_font(_lbl_final_result, &lv_font_montserrat_40, 0);	
	lv_obj_align(_lbl_final_result, LV_ALIGN_CENTER, 0, 0);

	UiCommon::unlock();
}

/******************************************************************************
* Build string of all test details to use in "details" msgbox and to submit
* to platform
* @param buff Output buffer
* @param buff_size Output buffer size
******************************************************************************/
void build_details_string(char *buff, int buff_len, bool include_platform)
{
	// Build details text
	char *pos = buff;
	// strcat(pos, "Failed:\n");
	for (size_t i = 0; i < TEST_COUNT; i++)
	{
		TestData *data = &TESTS[i];

		if(data->test_id == TEST_IOT_CLOUD && !include_platform)
			continue;
		if(i == TEST_COUNT - 1)
		{
			snprintf(pos, buff_len - (pos - buff), "%s (%s): %s", data->name, data->result == TEST_RESULT_PASS ? "Pass" : "Fail",
			data->result_details);
		}
		else
		{
			snprintf(pos, buff_len - (pos - buff), "%s (%s): %s\n", data->name, data->result == TEST_RESULT_PASS ? "Pass" : "Fail",
			data->result_details);
		}
		

		pos += strlen(pos);

		// Overflow
		if(pos >= buff + buff_len)
			break;
	}
}

/******************************************************************************
* Show task details messagebox after tasks have finished and user clicked details
******************************************************************************/
void show_details_msgbox()
{
	static lv_obj_t *msgbox = NULL;
	// Already shown
	if(msgbox != NULL)
		return;

	char details[1024] = "";
	build_details_string(details, sizeof(details));

	UiCommon::lock();
	static const char *btns[] = {"Close", ""};
	msgbox = lv_msgbox_create(NULL,
							"Details",
							details							
							, btns, false);
	lv_obj_set_style_text_font(msgbox, &lv_font_montserrat_16, 0);

	lv_obj_set_width(msgbox, UI_DEFAULT_MSGBOX_W);
	lv_obj_add_event_cb(msgbox, [](lv_event_t *event) {
		lv_obj_t *obj = lv_event_get_current_target(event);
		auto active_btn = lv_msgbox_get_active_btn(obj);

		if(active_btn == 0)
		{
			lv_msgbox_close_async(obj);
			msgbox = NULL;
		}
	}, LV_EVENT_VALUE_CHANGED, NULL);
	lv_obj_align(msgbox, LV_ALIGN_CENTER, 0, 0);
	UiCommon::unlock();
}

/******************************************************************************
* Test procedure failed (1 or more tests failed)
******************************************************************************/
void tests_failed()
{
	ESP_LOGE(TAG, "Tests failed!");

	UiCommon::lock();
	// Display final result
	lv_label_set_text_static(_lbl_final_result, LV_SYMBOL_CLOSE);
	lv_obj_set_style_text_color(_lbl_final_result, lv_color_hex(0x3D1A25), 0);
	// lv_obj_realign(_lbl_final_result);
	lv_obj_set_style_bg_color(_box_final_result, TEST_MODE_COLOR_FAIL,  0);

	lv_obj_clear_flag(_box_final_result, LV_OBJ_FLAG_HIDDEN);

	lv_obj_set_style_bg_color(_main_container, lv_color_hex(0x3D1A25), 0);
	lv_obj_set_style_border_width(_main_container, 2, 0);
	lv_obj_set_style_border_color(_main_container, TEST_MODE_COLOR_FAIL, 0);

	_btn_retry.hidden(false);
	_btn_details.hidden(false);

	UiCommon::unlock();
}

/******************************************************************************
* All tests passed
******************************************************************************/
void tests_passed()
{
	ESP_LOGI(TAG, "All tests passed!");
	UiCommon::lock();
	// Display final result
	lv_label_set_text_static(_lbl_final_result, LV_SYMBOL_OK);
	lv_obj_set_y(_lbl_final_result, -3);
	lv_obj_set_style_text_color(_lbl_final_result, lv_color_hex(0x1a3d24), 0);
	// lv_obj_realign(_lbl_final_result);
	lv_obj_set_style_bg_color(_box_final_result, TEST_MODE_COLOR_PASS, 0);

	lv_obj_clear_flag(_box_final_result, LV_OBJ_FLAG_HIDDEN);

	lv_obj_set_style_bg_color(_main_container, lv_color_hex(0x1a3d24), 0);
	lv_obj_set_style_border_width(_main_container, 2, 0);
	lv_obj_set_style_border_color(_main_container, TEST_MODE_COLOR_PASS, 0);

	UiCommon::unlock();
	

	// Show softkeys
	// set_softkey(2, "Restart", [](){
	// 	esp_restart();
	// });

	// set_softkey(0, "Details", show_details_msgbox);
}

/******************************************************************************
* Set test status label to any text
* @param test Test id
* @param status Text to set. Will be copied.
******************************************************************************/
void set_test_status(Test test, const char *status)
{
	const TestData *data = get_test_data(test);
	if(data == nullptr) return;

	if(data->lbl_status != NULL)
	{
		UiCommon::lock();
		lv_label_set_text(data->lbl_status, status);
		UiCommon::unlock();
	}
}

/******************************************************************************
* Set test status label to fail/pass text
* @param test Test id
* @param res Fail/Pass
******************************************************************************/
void set_test_result(Test test, TestResult res)
{
	TestData *data = get_test_data(test);
	if(data == nullptr) return;

	if(data->lbl_status != NULL)
	{
		UiCommon::lock();
		lv_obj_add_flag(data->spinner, LV_OBJ_FLAG_HIDDEN);
		lv_obj_clear_flag(data->lbl_status, LV_OBJ_FLAG_HIDDEN);
		if(res == TEST_RESULT_PASS)
		{
			lv_label_set_text(data->lbl_status, LV_SYMBOL_OK);
			lv_obj_align_to(data->lbl_status, data->lbl_name, LV_ALIGN_OUT_RIGHT_MID, 0, -3);
			lv_obj_add_style(data->lbl_status, &_test_pass_style, 0);
		}
		else if(res == TEST_RESULT_FAIL)
		{
			lv_label_set_text(data->lbl_status, LV_SYMBOL_CLOSE);
			lv_obj_align_to(data->lbl_status, data->lbl_name, LV_ALIGN_OUT_RIGHT_MID, 0, -3);
			lv_obj_add_style(data->lbl_status, &_test_fail_style, 0);
		}
		UiCommon::unlock();

		data->result = res;
	}
}

/******************************************************************************
* Set test result details to show when user clicks details after test finish.
* @param test Test id
* @param details Text to set. Will be copied.
******************************************************************************/
void set_test_result_details(Test test, const char *details)
{
	TestData *data = get_test_data(test);
	if(data == nullptr) return;

	xSemaphoreTake(_sem_test_data, portMAX_DELAY);
	strncpy(data->result_details, details, sizeof(TestData::result_details));
	xSemaphoreGive(_sem_test_data);
}

/******************************************************************************
* Get test data struct
******************************************************************************/
TestData *get_test_data(Test test)
{
	for (size_t i = 0; i < TEST_COUNT; i++)
	{
		if(TESTS[i].test_id == test)
		{
			return &TESTS[i];
		}
	}

	return nullptr;
}

/******************************************************************************
* Wait until specified test finished or timed out
* @param test_id 	Test to wait on. TEST_ANY to return on any type
* @param timeout_ms Time to wait before aborting
* @return
*	- true 	Test finished
*	- false	Timeout
******************************************************************************/
bool wait_test_finished(Test test_id, uint32_t timeout_ms, TestData **data_out)
{
	int64_t wait_start_tick = Utils::get_tick_ms();

	int32_t wait_ms = timeout_ms;

	while(1)
	{
		TestData *data = NULL;
		if(xQueueReceive(_test_finished_queue, &data, pdMS_TO_TICKS(wait_ms)) == pdTRUE)
		{
			if(data->test_id == test_id || test_id == TEST_ANY)
			{
				if(data_out != NULL)
				{
					*data_out = data;
				}
				return true;
			}

			// Calc for next wait round
			wait_ms -= Utils::get_tick_ms() - wait_start_tick;

			if(wait_ms < 0) wait_ms = 0;
		}
		else
		{
			return false;
		}
	}
}

/******************************************************************************
* Notify task of finished test
******************************************************************************/
void notify_test_finished(TestData *test_data)
{
	xQueueSend(_test_finished_queue, &test_data, 0);
}

/******************************************************************************
* Main test task
* Handles running all tests
******************************************************************************/
void test_task(void *param)
{
	// Connect wifi
	if(!connect_wifi())
	{
		Wifi::disconnect();
		_btn_retry.hidden(false);
		vTaskDelete(NULL);
	}

	// Set up test ui
	setup_test_ui();

	// Create timer task if doesn't exist
	if(_elapsed_timer_task_handle == NULL)
	{
		xTaskCreatePinnedToCore(elapsed_timer_task, "elapsed", 4096, NULL, 2, &_elapsed_timer_task_handle, 1);
	}

	//
	// Create test tasks for all parallel tests
	//
	int parallel_test_count = 0;
	for (size_t i = 0; i < TEST_COUNT; i++)
	{
		TestData *data = &TESTS[i];
		// if(data->test_id == TEST_IOT_CLOUD || data->test_id == TEST_PRESSURE)
		// 	continue;

		if(data->task != NULL && data->order == TEST_ORDER_PARALLEL)
		{
			parallel_test_count++;
			Utils::wait_ms(300);
			UiCommon::lock();
			lv_obj_add_flag(data->lbl_status, LV_OBJ_FLAG_HIDDEN);
			lv_obj_clear_flag(data->spinner, LV_OBJ_FLAG_HIDDEN);
			UiCommon::unlock();
			if(xTaskCreatePinnedToCore(data->task, data->name, 4096, NULL, 2, &data->task_handle, 1) != pdPASS)
			{
				ESP_LOGW(TAG, "Could not create test task: %s", data->name);
			}
		}
	}

	//
	// Wait on queue for status updates
	//
	int64_t start_tick = Utils::get_tick_ms();
	int64_t time_left_ms = TEST_MODE_PARALLEL_TESTS_TIMEOUT_MS;

	// Test left to finish - excluding platform test, pressure test
	bool timed_out = false;

	while(1)
	{
		TestData *data = NULL;

		time_left_ms -= Utils::get_tick_ms() - start_tick;

		if(wait_test_finished(TEST_ANY, time_left_ms, &data) == true)
		{
			// Done?
			parallel_test_count--;

			if(parallel_test_count == 0)
				break;
		}
		else
		{
			timed_out = true;
			break;
		}

		Utils::wait_ms(10);
	}

	ESP_LOGI(TAG, "Finished parallel tests.");

	// Timeout
	// All parallel tests that have not finished, are marked failed
	if(timed_out)
	{
		// FoSniffer::stop_capture();
		// Gps::disable();
		ESP_LOGE(TAG, "Parallel tests timed out");

		for (size_t i = 0; i < TEST_COUNT; i++)
		{
			TestData *data = &TESTS[i];

			if(data->result != TEST_RESULT_PASS && data->order == TEST_ORDER_PARALLEL)
			{
				set_test_result(data->test_id, TEST_RESULT_FAIL);

				if(!strlen(data->result_details))
				{
					set_test_result_details(data->test_id, "Timeout");
				}

				if(data->task_handle != NULL)
					vTaskDelete(data->task_handle);
			}
		}
	}

	//
	// Run sequential tests
	//
	time_left_ms = TEST_MODE_SEQ_TESTS_TIMEOUT_MS;
	start_tick = Utils::get_tick_ms();
	timed_out = false;

	for (size_t i = 0; i < TEST_COUNT; i++)
	{
		TestData *data = &TESTS[i];
		if(data->task == NULL || data->order != TEST_ORDER_SEQ)
		{
			continue;
		}
			
		UiCommon::lock();
		lv_obj_add_flag(data->lbl_status, LV_OBJ_FLAG_HIDDEN);
		lv_obj_clear_flag(data->spinner, LV_OBJ_FLAG_HIDDEN);
		UiCommon::unlock();
		if(xTaskCreatePinnedToCore(data->task, data->name, 4096 * 3, NULL, 2, &data->task_handle, 1) != pdPASS)
		{
			ESP_LOGW(TAG, "Could not create test task: %s", data->name);
			continue;
		}

		time_left_ms -= Utils::get_tick_ms() - start_tick;
		if(wait_test_finished(data->test_id, time_left_ms) != true)
		{
			timed_out = true;
			break;
		}
	}

	// Timeout
	// All tests that have not finished, are marked failed
	if(timed_out)
	{
		for (size_t i = 0; i < TEST_COUNT; i++)
		{
			TestData *data = &TESTS[i];
			if(data->result != TEST_RESULT_PASS)
			{
				set_test_result(data->test_id, TEST_RESULT_FAIL);

				if(!strlen(data->result_details))
				{
					set_test_result_details(data->test_id, "Timeout");
				}

				if(data->task_handle != NULL)
					vTaskDelete(data->task_handle);
			}
		}
	}

	ESP_LOGI(TAG, "Finished seq. tests.");

	//
	// Check results
	//
	bool overall_pass = true;
	for (size_t i = 0; i < TEST_COUNT; i++)
	{
		TestData *data = &TESTS[i];

		if(data->result != TEST_RESULT_PASS)
		{
			overall_pass = false;
			break;
		}
	}

	// All passed
	if(overall_pass)
	{
		tests_passed();
	}
	else
	{

		tests_failed();
	}

	end();

	vTaskDelete(NULL);
}

/******************************************************************************
* Update elapsed timer
******************************************************************************/
void elapsed_timer_task(void *param)
{
	int64_t _start_tick = Utils::get_tick_ms();
	char time[20] = "0:00";

	while(1)
	{
		int d = (int)((Utils::get_tick_ms() - _start_tick) / 1000);

		int min = d / 60;
		d -= min * 60;

		snprintf(time, sizeof(time), "%1d:%02d", min, d);

		UiCommon::lock();
		if(_lbl_timer != NULL)
		{
			lv_label_set_text_static(_lbl_timer, time);
			// lv_obj_realign(_lbl_timer);
		}
		UiCommon::unlock();

		Utils::wait_ms(1000);
	}

	vTaskDelete(NULL);
}

/******************************************************************************
* Build test report JSON to publish uring Iot platform test
******************************************************************************/
RetResult build_test_json(DynamicJsonDocument *json_doc)
{
	JsonObject root_object = json_doc->template to<JsonObject>();

	char details[1024] = "";
	build_details_string(details, sizeof(details), false);

	// Get overall success
	bool success = true;
	for (size_t i = 0; i < TEST_COUNT; i++)
	{
		TestData *data = &TESTS[i];
		if(data->test_id == TEST_IOT_CLOUD)
		{
			// If you're here, platform test was already successful.
			continue;
		}

		if(data->result != TEST_RESULT_PASS)
			success = false;
	}

	root_object["test_details"] = details;
	root_object["test_result"] = success;

	return RET_OK;
}

// ============================================================================
// TEST TASKS
// ============================================================================

/******************************************************************************
* Test FineOffset reception
* Success: Receive X valid packets from weather station within a timewindow
******************************************************************************/
void test_ws_task(void *param)
{
	// TODO: Update to use SX127x driver
	const char TEST_TAG[] = "Test - FO";
	ESP_LOGI(TAG, "Running test: FO Weather station");

	TestData *data = get_test_data(TEST_WS);

	const int FO_BROADCAST_WAIT_PERIOD_MS = 17000;

	// Message in case of timeout
	set_test_result_details(data->test_id, "Packets not received.");
	
	// FoSniffer::start_capture();

	int packets_left = TEST_MODE_WS_PACKETS_TO_RECEIVE;
	WsFields::FieldPack field_pack = {};
	ESP_LOGI(TEST_TAG, "Waiting for packets.");
	while(packets_left)
	{		
		// We need to end the waiting before the fo test task gets deleted by the TEST_MODE_PARALLEL_TESTS_TIMEOUT_MS
		if(Utils::get_tick_ms() - _test_started_ts_ms > TEST_MODE_PARALLEL_TESTS_TIMEOUT_MS - (FO_BROADCAST_WAIT_PERIOD_MS*TEST_MODE_WS_PACKETS_TO_RECEIVE))
		{
			break;
		}
		if(GlobalEvents::wait(GlobalEvents::EventId::WS_DATA_RECEIVED, &field_pack, sizeof(WsFields::FieldPack), pdMS_TO_TICKS(FO_BROADCAST_WAIT_PERIOD_MS)) == RET_OK)
		{
			// ESP_LOGI(TEST_TAG, "Packet received from WS ID: 0x%02x", decoded.metadata.sensor_id);
			packets_left--;
		}		
	}

	// FoSniffer::stop_capture();
	if(packets_left == 0)
	{
		char details[20] = "";
		snprintf(details, sizeof(details), "ID: 0x%02X", WsHandler::get_paired_id());
		set_test_result_details(data->test_id, details);
		set_test_result(data->test_id, TEST_RESULT_PASS);
	}	
	notify_test_finished(data);
	vTaskDelete(NULL);
}

/******************************************************************************
* Test GPS reception
* Success: Receive GPS location within a timewindow
******************************************************************************/
void test_gps_task(void *param)
{
	const char TEST_TAG[] = "Test - GPS";
	ESP_LOGI(TEST_TAG, "Running test: GPS");
	TestData *data = get_test_data(TEST_GPS);
	bool success = false;

	// Message in case of timeout
	set_test_result_details(data->test_id, "Test TODO");

	// Gps::enable();

	// int cont_fix_time_ms = 0;
	// int64_t start_ms = Utils::get_tick_ms();
	// int64_t fix_start_ms = -1;
	// while(1)
	// {
	// 	Gps::GpsData gps_data = {};
	// 	if(Gps::get_last_data(&gps_data) != RET_ERROR)
	// 	{
	// 		// No fix
	// 		if(gps_data.sats < TEST_MODE_GPS_MIN_SATELLITES || isnan(gps_data.lat) || isnan(gps_data.lon))
	// 		{
	// 			fix_start_ms = -1;
	// 		}
	// 		else
	// 		{
	// 			// Got fix
	// 			if(fix_start_ms == -1)
	// 			{
	// 				ESP_LOGI(TAG, "Got GPS Fix - Sats %d", gps_data.sats);
	// 				fix_start_ms = Utils::get_tick_ms();
	// 			}

	// 			// Fix stayed long enough?
	// 			if(Utils::get_tick_ms() - fix_start_ms > TEST_MODE_GPS_SIGNAL_MIN_CONT_FIX_SEC * 1000)
	// 			{
	// 				// Success
	// 				success = true;

	// 				char details[100] = "";
	// 				snprintf(details, sizeof(details), "Sats: %d - Lat/Lon: %.4f / %.4f", gps_data.sats, gps_data.lat, gps_data.lon);
	// 				set_test_result_details(data->test_id, details);

	// 				break;
	// 			}
	// 		}
	// 	}
	// 	Utils::wait_ms(50);
	// }

	set_test_result(data->test_id, success ? TEST_RESULT_PASS : TEST_RESULT_FAIL);
	notify_test_finished(data);

	vTaskDelete(NULL);
}

/******************************************************************************
* Test Pressure sensor
* Success: Read pressure sensor successfully
******************************************************************************/
void test_pressure_task(void *param)
{
	const char TEST_TAG[] = "Test - Pressure";
	ESP_LOGI(TEST_TAG, "Running test: Pressure sensor");
	TestData *data = get_test_data(TEST_PRESSURE);
	bool success = false;

	// Message in case of timeout
	set_test_result_details(data->test_id, "Read failed");

	// temp
	// set_test_result(data->test_id, TEST_RESULT_PASS);
	// notify_test_finished(data);
	// vTaskDelete(NULL);

	int tries = 5;

	int ret = ESP_FAIL;

	float pressure = NAN;
	float temperature = NAN;
	// Try multiple times
#ifndef CONFIG_SCREENLESS
	while(ret = bmp3xx_read_data(&pressure, &temperature) != ESP_OK && tries--)
	{
		ESP_LOGE(TEST_TAG, "Could not read pressure sensor");
		Utils::wait_ms(2250);
	}
#else
	ret = ESP_FAIL;
#endif

	if(ret != ESP_OK)
	{
		ESP_LOGE(TEST_TAG, "Pressure reading failed.");
		set_test_result_details(data->test_id, "Failed reading.");
	}
	else
	{
		pressure /= 100;
		ESP_LOGI(TEST_TAG, "Pressure: %.1f hPa", pressure);
		success = true;
	}

	Utils::wait_ms(2000);
	if(success)
	{
		char details[20] = "";
		snprintf(details, sizeof(details), "%.1f hPa", pressure);
		set_test_result_details(data->test_id, details);
		set_test_result(data->test_id, TEST_RESULT_PASS);
	}
	else
	{
		set_test_result(data->test_id, TEST_RESULT_FAIL);
	}
	notify_test_finished(data);
	vTaskDelete(NULL);
}

/******************************************************************************
* Test Secure element
* All operations will be retried multiple times due to known bug in AWS SDK
* (no thread safe i2c access)
* Success: Device public key obtained successfully
******************************************************************************/
void test_ds_storage_task(void *param)
{
	const char TEST_TAG[] = "Test - SE";
	ESP_LOGI(TEST_TAG, "Running test");

	TestData *data = get_test_data(TEST_DS_STORAGE);

	set_test_result_details(data->test_id, "Test TODO");
	set_test_result(data->test_id, TestResult::TEST_RESULT_FAIL);

	// // temp
	// // set_test_result(data->test_id, TEST_RESULT_PASS);
	// // notify_test_finished(data);
	// // vTaskDelete(NULL);

	// //
	// // Get public key
	// //
	// ESP_LOGI(TEST_TAG, "Getting public device key.");
	// uint8_t pubkey[65] = "";
	// ATCA_STATUS atca_ret = ATCA_TIMEOUT;
	// int tries = TEST_MODE_SE_TRIES;
	// atcab_init(&cfg_ateccx08a_i2c_default); // When TB disconnects deinitializes the attec so it fails in retry

	// while((atca_ret = atcab_get_pubkey(0, pubkey)) != ATCA_SUCCESS && tries--)
	// {
	// 	ESP_LOGE(TEST_TAG, "Could not get public key.");
	// 	if(tries)
	// 	{
	// 		ESP_LOGI(TEST_TAG, "Retrying");
	// 	}

	// 	Utils::wait_ms(1500);
	// }

	// if(atca_ret != ATCA_SUCCESS)
	// {
	// 	ESP_LOGE(TEST_TAG, "Could not get public key");
	// 	set_test_result(data->test_id, TestResult::TEST_RESULT_FAIL);
	// 	set_test_result_details(data->test_id, "Public key read error.");
	// }
	// else
	// {
	// 	uint8_t pubkey_base64[150] = "";
	// 	size_t res_len = 0;
	// 	mbedtls_base64_encode(pubkey_base64, sizeof(pubkey_base64), &res_len, pubkey, 64);

	// 	ESP_LOGI(TEST_TAG, "Got device public key: %s", pubkey_base64);

	// 	// Get serial num
	// 	RetResult ret = RET_ERROR;
	// 	char sn[ATCA_SERIAL_NUM_STRING_BUFF_SIZE] = "";
	// 	while((ret = Utils::get_serial_number_str(sn, sizeof(sn))) != ATCA_SUCCESS && tries--)
	// 	{
	// 		ESP_LOGE(TEST_TAG, "Could not get serial number.");
	// 		if(tries)
	// 		{
	// 			ESP_LOGI(TEST_TAG, "Retrying");
	// 		}

	// 		Utils::wait_ms(500);
	// 	}
	// 	Utils::wait_ms(2000);
	// 	if(ret != RET_OK)
	// 	{
	// 		ESP_LOGE(TEST_TAG, "Could not read serial number");
	// 		set_test_result(data->test_id, TestResult::TEST_RESULT_FAIL);
	// 		set_test_result_details(data->test_id, "SN read error.");
	// 	}
	// 	else
	// 	{
	// 		// Success details: SN
	// 		set_test_result_details(data->test_id, sn);
	// 		set_test_result(data->test_id, TestResult::TEST_RESULT_PASS);
	// 	}
	// }
	notify_test_finished(data);
	vTaskDelete(NULL);
}

/******************************************************************************
* Test Platform connectity
* Success: Successfully connected to MQTT and received attributes
******************************************************************************/
void test_platform_task(void *param)
{
	ESP_LOGI(TAG, "Running test: Platform connectivity");
	TestData *data = get_test_data(TEST_IOT_CLOUD);	
	Tb::stop();
	Tb::start();

	// Connect
	// Message in case of timeout
	set_test_result_details(data->test_id, "Cannot connect.");
	while(Tb::wait_connected() != RET_OK){}
	ESP_LOGI(TAG, "TB Connected");

	// Submit JSON with all test results
	DynamicJsonDocument json_doc(2048);
	char json_buff[TB_JSON_BUFF_SIZE] = "";

	set_test_result_details(data->test_id, "Success");
	build_test_json(&json_doc);
	serializeJsonPretty(json_doc, json_buff, sizeof(json_buff));

	printf("Test report JSON: \n%s\n", json_buff);

	set_test_result_details(data->test_id, "Cannot publish.");
	int tries = TEST_MODE_IOT_PLATFORM_TRIES;
	RetResult ret = RET_ERROR;

	while((ret = Tb::publish_telemetry(json_buff)) != RET_OK && tries--)
	{
		Utils::wait_ms(3000);
	}
	Utils::wait_ms(2000);
	if(ret == RET_OK)
	{
		set_test_result(data->test_id, TEST_RESULT_PASS);
		set_test_result_details(data->test_id, "Publish success.");
	}
	else
	{
		set_test_result(data->test_id, TEST_RESULT_FAIL);
		set_test_result_details(data->test_id, "Publish error.");
	}
	Tb::stop();

	notify_test_finished(data);
	vTaskDelete(NULL);
}
}
