#include "esp_log.h"
#include "bsp_storage.h"
#include "weather_logger.h"
#ifndef CONFIG_SCREENLESS
#include "bsp_board.h"
#include "bsp_btn.h"
#endif
#ifndef CONFIG_SCREENLESS
#include "lv_port.h"
#include "ui/include/ui_theme_wxm.h"
#include "backlight.h"
#include "state_sniffer.h"
#include "recovery_mode.h"
#endif
#include "wifi.h"
#include "tb.h"
#include "utils.h"
#include "settings.h"
#include "global_events.h"
#include "global_state.h"
#include "nvs_flash.h"
#include "boot_guard.h"
#include "datetime_rtc.h"
#include "submit.h"
#include "wifi_config_mode.h"
#include "credentials.h"
#include "pressure.h"
#include "ws/include/ws_handler.h"
#include "ws/include/web_server.h"
#include "ws/include/mesh_telemetry.h"
#include "esp_littlefs.h"
#include "esp_partition.h"
#include "esp_system.h"

#ifdef CONFIG_SCREENLESS
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_sleep.h"
#ifdef CONFIG_CELLULAR_BACKUP
#include "cellular.h"
#endif
#endif

static const char TAG[] = "Main";

#ifdef CONFIG_SCREENLESS
#define XIAO_LED_PIN GPIO_NUM_21

enum LedStatus {
    LED_STATUS_BOOT,
    LED_STATUS_OK,
    LED_STATUS_ERROR,
    LED_STATUS_AP
};

static volatile LedStatus g_led_status = LED_STATUS_BOOT;

extern "C" void set_led_status_ap_mode() {
    g_led_status = LED_STATUS_AP;
}

void led_indicator_task(void *pvParameters) {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << XIAO_LED_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);
    
    // Active LOW LED: 1 is OFF, 0 is ON
    gpio_set_level(XIAO_LED_PIN, 1); 

    while (1) {
        if (g_led_status == LED_STATUS_ERROR) {
            // Rapid flashing for error: 100ms ON, 100ms OFF
            gpio_set_level(XIAO_LED_PIN, 0); // ON
            vTaskDelay(pdMS_TO_TICKS(100));
            gpio_set_level(XIAO_LED_PIN, 1); // OFF
            vTaskDelay(pdMS_TO_TICKS(100));
        } else if (g_led_status == LED_STATUS_AP) {
            // AP Mode: Fast blink (200ms ON, 200ms OFF) to clearly indicate AP configuration mode
            gpio_set_level(XIAO_LED_PIN, 0); // ON
            vTaskDelay(pdMS_TO_TICKS(200));
            gpio_set_level(XIAO_LED_PIN, 1); // OFF
            vTaskDelay(pdMS_TO_TICKS(200));
        } else {
            EventGroupHandle_t evg = GlobalState::get_event_group();
            if (evg != NULL) {
                EventBits_t bits = xEventGroupGetBits(evg);
                if (bits & GlobalState::EV_TB_CONNECTED_BIT) {
                    // Normal Connected Operation: Heartbeat dual-blink every 3 seconds
                    gpio_set_level(XIAO_LED_PIN, 0); // ON
                    vTaskDelay(pdMS_TO_TICKS(50));
                    gpio_set_level(XIAO_LED_PIN, 1); // OFF
                    vTaskDelay(pdMS_TO_TICKS(150));
                    gpio_set_level(XIAO_LED_PIN, 0); // ON
                    vTaskDelay(pdMS_TO_TICKS(50));
                    gpio_set_level(XIAO_LED_PIN, 1); // OFF
                    vTaskDelay(pdMS_TO_TICKS(2750));
                } else if (bits & GlobalState::EV_WIFI_CONNECTED_BIT) {
                    // WiFi connected, but ThingsBoard still connecting: Medium blink (500ms ON, 500ms OFF)
                    gpio_set_level(XIAO_LED_PIN, 0); // ON
                    vTaskDelay(pdMS_TO_TICKS(500));
                    gpio_set_level(XIAO_LED_PIN, 1); // OFF
                    vTaskDelay(pdMS_TO_TICKS(500));
                } else {
                    // Connecting WiFi / Booting: Slow blink (1sec ON, 1sec OFF)
                    gpio_set_level(XIAO_LED_PIN, 0); // ON
                    vTaskDelay(pdMS_TO_TICKS(1000));
                    gpio_set_level(XIAO_LED_PIN, 1); // OFF
                    vTaskDelay(pdMS_TO_TICKS(1000));
                }
            } else {
                // Booting/Pre-init: Slow blink (1sec ON, 1sec OFF)
                gpio_set_level(XIAO_LED_PIN, 0); // ON
                vTaskDelay(pdMS_TO_TICKS(1000));
                gpio_set_level(XIAO_LED_PIN, 1); // OFF
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        }
    }
}

#ifdef CONFIG_CELLULAR_BACKUP
void network_scheduler_task(void *pvParameters) {
    ESP_LOGI("NetworkScheduler", "Network scheduler task started.");
    
    // 1. Try to connect to WiFi
    Wifi::connect(NULL, NULL, true);
    
    // 2. Wait up to 15 seconds for WiFi connection
    uint32_t wait_start = Utils::get_tick_sec();
    bool wifi_connected = false;
    while (Utils::get_tick_sec() - wait_start < 15) {
        if (Wifi::is_connected()) {
            wifi_connected = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    
    if (wifi_connected) {
        ESP_LOGI("NetworkScheduler", "WiFi connected successfully. Using WiFi.");
    } else {
        ESP_LOGW("NetworkScheduler", "WiFi connection failed/timed out. Switching to Cellular PPP...");
        // Disconnect WiFi to free radio/resources
        Wifi::disconnect();
        vTaskDelay(pdMS_TO_TICKS(1000));
        
        // Start Cellular PPP
        if (Cellular::start_ppp() == ESP_OK) {
            ESP_LOGI("NetworkScheduler", "Cellular PPP connected successfully.");
        } else {
            ESP_LOGE("NetworkScheduler", "Failed to establish Cellular PPP connection.");
        }
    }
    
    vTaskDelete(NULL);
}
#endif

void sleep_scheduler_task(void *pvParameters) {
    ESP_LOGI("SleepScheduler", "Sleep scheduler task started.");
    
    uint32_t start_time = Utils::get_tick_sec();
    while (true) {
        uint32_t now = Utils::get_tick_sec();
        
        // If sniff mode is enabled, keep staying awake indefinitely
        if (WsHandler::g_sniff_mode_enabled) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        
        // If there was a web request in the last 60 seconds, keep staying awake
        if (WebServer::g_last_web_request_ts > 0 && (now - WebServer::g_last_web_request_ts) < 60) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        
        // If we haven't reached the initial 60 seconds of uptime, keep staying awake
        if ((now - start_time) < 60) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        
        break;
    }
    
    ESP_LOGI("SleepScheduler", "Preparing to enter deep sleep...");

#ifdef CONFIG_CELLULAR_BACKUP
    Cellular::power_off();
#endif
    
    // De-initialize UART / disconnect pin 1 so we can control it as GPIO
    uart_driver_delete(UART_NUM_1); // assuming UART_NUM_1 is KISS_UART
    
    // Configure pin 1 (RTS) as GPIO output and set it HIGH (deassert RTS to tell nRF52 to buffer)
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << GPIO_NUM_1),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);
    gpio_set_level(GPIO_NUM_1, 1); // RTS High (inactive/deasserted)
    
    // Enable hold on pin 1 so its high state persists through deep sleep
    gpio_hold_en(GPIO_NUM_1);
    gpio_deep_sleep_hold_en();
    
    // Configure deep sleep wake-up timer (5 minutes)
    ESP_LOGI("SleepScheduler", "Entering deep sleep for 5 minutes...");
    esp_sleep_enable_timer_wakeup(5 * 60 * 1000000ULL);
    esp_deep_sleep_start();
    
    vTaskDelete(NULL);
}
#endif

extern "C" {
    extern const uint8_t _binary_uPlot_iife_min_js_start[];
    extern const uint8_t _binary_uPlot_iife_min_js_end[];
    extern const uint8_t _binary_uPlot_min_css_start[];
    extern const uint8_t _binary_uPlot_min_css_end[];
}

static void auto_extract_files() {
    const struct {
        const char *filepath;
        const uint8_t *start;
        const uint8_t *end;
    } files[] = {
        {"/littlefs/uPlot.iife.min.js", _binary_uPlot_iife_min_js_start, _binary_uPlot_iife_min_js_end},
        {"/littlefs/uPlot.min.css", _binary_uPlot_min_css_start, _binary_uPlot_min_css_end}
    };

    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        size_t embedded_size = files[i].end - files[i].start;
        bool need_write = false;

        FILE *f = fopen(files[i].filepath, "r");
        if (f == NULL) {
            // File does not exist
            need_write = true;
        } else {
            // File exists, check size
            fseek(f, 0, SEEK_END);
            size_t size = ftell(f);
            fclose(f);
            if (size != embedded_size) {
                ESP_LOGW("MAIN", "File %s size mismatch (%u vs %u embedded). Re-writing.", files[i].filepath, (unsigned int)size, (unsigned int)embedded_size);
                need_write = true;
            }
        }

        if (need_write) {
            ESP_LOGI("MAIN", "Extracting %s from firmware flash...", files[i].filepath);
            FILE *out = fopen(files[i].filepath, "w");
            if (out == NULL) {
                ESP_LOGE("MAIN", "Failed to open %s for writing", files[i].filepath);
                continue;
            }
            size_t written = fwrite(files[i].start, 1, embedded_size, out);
            fclose(out);
            ESP_LOGI("MAIN", "Extracted %u bytes successfully to %s", (unsigned int)written, files[i].filepath);
        } else {
            ESP_LOGI("MAIN", "File %s already exists with correct size (%u bytes)", files[i].filepath, (unsigned int)embedded_size);
        }
    }
}

static bool init_littlefs() {
    ESP_LOGI("MAIN", "Initializing LittleFS filesystem...");
    esp_vfs_littlefs_conf_t conf = {
        .base_path = "/littlefs",
        .partition_label = "spiffs",
        .format_if_mount_failed = true,
        .dont_mount = false,
    };
    esp_err_t ret = esp_vfs_littlefs_register(&conf);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE("MAIN", "Failed to mount or format LittleFS");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE("MAIN", "Failed to find LittleFS partition");
        } else {
            ESP_LOGE("MAIN", "Failed to initialize LittleFS (%s)", esp_err_to_name(ret));
        }
        return false;
    }
    size_t total = 0, used = 0;
    ret = esp_littlefs_info(conf.partition_label, &total, &used);
    if (ret != ESP_OK) {
        ESP_LOGE("MAIN", "Failed to get LittleFS info (%s)", esp_err_to_name(ret));
    } else {
        ESP_LOGI("MAIN", "LittleFS mounted successfully. Total bytes: %u, Used bytes: %u", (unsigned int)total, (unsigned int)used);
        // Automatically extract uPlot static assets to LittleFS if missing or mismatching
        auto_extract_files();
    }
    return true;
}

static bool get_partition_sha256_hex(const char *label, char out[65])
{
    const esp_partition_t *partition =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, label);
    if(partition == nullptr)
    {
        strlcpy(out, "partition_not_found", 65);
        return false;
    }

    uint8_t digest[32] = {};
    esp_err_t err = esp_partition_get_sha256(partition, digest);
    if(err != ESP_OK)
    {
        snprintf(out, 65, "sha_error_%s", esp_err_to_name(err));
        return false;
    }

    for(size_t i = 0; i < sizeof(digest); i++)
    {
        snprintf(&out[i * 2], 3, "%02x", digest[i]);
    }
    out[64] = '\0';
    return true;
}

/******************************************************************************
* App main
******************************************************************************/
extern "C" void app_main()
{
	// Protect normal esp_restart() calls from any source, including third-party
	// components which cannot call the C++ safe-restart wrapper directly.
	ESP_ERROR_CHECK(esp_register_shutdown_handler(Utils::prepare_safe_restart_gpios));

	// Mount diagnostics storage before NVS initialization. If NVS recovery is
	// required, preserve the cause and pre-erase partition hash outside NVS.
	init_littlefs();
	Utils::capture_early_boot_diagnostics();

#ifdef CONFIG_SCREENLESS
	// Disable pin hold on RTS (GPIO_NUM_1) so it can be controlled by UART
	gpio_hold_dis(GPIO_NUM_1);
    // Start LED indicator task immediately
    xTaskCreate(led_indicator_task, "led_indicator", 2048, NULL, 1, NULL);
	// Wait 5 seconds to let the macOS host enumerate the USB CDC serial port
	vTaskDelay(pdMS_TO_TICKS(5000));
#endif
	esp_err_t nvs_initial_ret = nvs_flash_init();
	esp_err_t esp_ret = nvs_initial_ret;
	if (esp_ret == ESP_ERR_NVS_NO_FREE_PAGES)
	{
		char nvs_sha256[65] = "";
		get_partition_sha256_hex("nvs", nvs_sha256);
		ESP_LOGE(TAG, "NVS has no free pages; preserving evidence before recovery erase. sha256=%s",
				 nvs_sha256);
		Utils::append_diagnostic_event("nvs_recovery_erase",
									  esp_ret,
									  0,
									  nvs_sha256);
#ifdef CONFIG_SCREENLESS
		g_led_status = LED_STATUS_ERROR;
#endif
		ESP_ERROR_CHECK(nvs_flash_erase());
		esp_ret = nvs_flash_init();
	}
	else if(esp_ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
	{
		char nvs_sha256[65] = "";
		get_partition_sha256_hex("nvs", nvs_sha256);
		ESP_LOGE(TAG,
				 "NVS uses a newer format. Preserving it instead of erasing; install a compatible firmware. sha256=%s",
				 nvs_sha256);
		Utils::append_diagnostic_event("nvs_new_version_preserved",
									  esp_ret,
									  0,
									  nvs_sha256);
	}
	Utils::record_nvs_diagnostics(nvs_initial_ret, esp_ret);
	if(esp_ret != ESP_OK)
	{
#ifdef CONFIG_SCREENLESS
		g_led_status = LED_STATUS_ERROR;
#endif
	}
	ESP_ERROR_CHECK(esp_ret);

	// Crash-loop detection and OTA rollback. Must run before any subsystem
	// that could crash so a bad OTA cannot brick field devices.
	BootGuard::init();

	ESP_ERROR_CHECK(esp_event_loop_create_default());

	esp_log_level_set("*", ESP_LOG_INFO);

#ifndef CONFIG_SCREENLESS
	ESP_ERROR_CHECK(bsp_board_init());
	esp_err_t err_btn = bsp_btn_register_callback(BOARD_BTN_ID_USER, BUTTON_LONG_PRESS_HOLD, [](void *args){
		// GPIO38 is the visible display button and is also used as the
		// factory-reset input by newer bootloaders. Restart paths must wait
		// for this active-low input to be released.
		ESP_LOGI(TAG, "Display button held for 1.5 seconds. Turning screen off...");
		Backlight::turn_screen_off();
	}, NULL);
	if(err_btn != ESP_OK)
	{
		ESP_LOGE(TAG, "Failed to register BUTTON_LONG_PRESS_HOLD callback: %s", esp_err_to_name(err_btn));
	}
#endif

#ifndef CONFIG_SCREENLESS
	lv_port_init();
#endif
		
	GlobalState::init();
	GlobalEvents::init();
	Settings::init();
	WeatherLogger::init();

	if(Credentials::init() != RET_OK)
	{
		ESP_LOGE(TAG, "Credentials init failed!");
#ifdef CONFIG_SCREENLESS
        g_led_status = LED_STATUS_ERROR;
#endif
		return; // What should we do?
	}

	Utils::print_boot_info();


#ifndef CONFIG_SCREENLESS
	Backlight::init();
#endif

	WifiConfigMode::init();

	bool bypass_wifi_check = false;
	nvs_handle_t handle;
	if (nvs_open("wifi_cfg", NVS_READWRITE, &handle) == ESP_OK)
	{
		uint8_t val = 0;
		if (nvs_get_u8(handle, "bypass_wifi", &val) == ESP_OK && val == 1)
		{
			bypass_wifi_check = true;
			ESP_LOGW(TAG, "Bypassing Wi-Fi credential check for this boot session.");
			nvs_erase_key(handle, "bypass_wifi");
			nvs_commit(handle);
		}
		nvs_close(handle);
	}

	// If the device is definitively unprovisioned, automatically trigger AP
	// config mode. On NVS read errors just log and continue with normal boot:
	// a transient error must not drop a provisioned field device into config
	// mode, where it would sit offline until someone physically reconfigures it.
	if (!bypass_wifi_check)
	{
		Utils::WifiCredsState creds_state = Utils::wifi_manager_creds_state();
		if (creds_state == Utils::WifiCredsState::ABSENT)
		{
			ESP_LOGW(TAG, "No Wi-Fi credentials found. Automatically triggering Wi-Fi Config Mode...");
			WifiConfigMode::set_on_next_boot();
		}
		else if (creds_state == Utils::WifiCredsState::ERROR)
		{
			ESP_LOGE(TAG, "Could not read Wi-Fi credentials from NVS. Continuing normal boot.");
		}
		else
		{
			ESP_LOGI(TAG, "Found valid Wi-Fi credentials. Proceeding with boot.");
		}
	}
	else
	{
		ESP_LOGW(TAG, "Wi-Fi credential check bypassed as requested.");
	}

#ifndef CONFIG_SCREENLESS
	static lv_theme_t *wxm_theme_dark = UiThemeWxm::lv_theme_basic_init(NULL);
	lv_disp_set_theme(NULL, wxm_theme_dark);	
#endif

	// TODO: Turn "modes" to "states" with proper "enter"/"leave"
	if (bypass_wifi_check)
	{
		ESP_LOGI(TAG, "Bypassing Wi-Fi config mode on this boot.");
		WifiConfigMode::clear_boot_flags();
	}
	else
	{
		WifiConfigMode::handle();
	}
	Wifi::init();
	DatetimeRtc::init();

	static esp_event_handler_instance_t event_handler;
	GlobalEvents::subscribe(GlobalEvents::EventId::TIME_SYNCED, [](void* event_handler_arg,
							esp_event_base_t event_base, int32_t event_id, void* event_data){		
		if(*((GlobalEvents::TimeSyncedEvent *)event_data) == GlobalEvents::TimeSyncedEvent::FIRST_SYNC)
		{
			Log::logc(Log::Code::BOOT, esp_reset_reason());
		}	
	}, &event_handler, &event_handler);


	Utils::start_diag_monitor_task();

	if(Utils::normal_boot())
	{
#ifdef CONFIG_SCREENLESS
        g_led_status = LED_STATUS_OK;
#endif
#ifndef CONFIG_SCREENLESS
		esp_err_t err_click = bsp_btn_register_callback(BOARD_BTN_ID_USER, BUTTON_SINGLE_CLICK, [](void *args){
			ESP_LOGI("Main", "Button single click detected. Toggling backlight...");
			Backlight::is_screen_on() ?
				Backlight::turn_screen_off() :
				Backlight::turn_screen_on(BACKLIGHT_WAKE_PHYSICAL_BUTTON);
		}, NULL);
		if(err_click != ESP_OK)
		{
			ESP_LOGE(TAG, "Failed to register BUTTON_SINGLE_CLICK callback: %s", esp_err_to_name(err_click));
		}
#endif

		ESP_LOGI(TAG, "Boot step: initializing Tb...");
		Tb::init();
		ESP_LOGI(TAG, "Boot step: initializing WsHandler...");
		WsHandler::init();
		ESP_LOGI(TAG, "Boot step: initializing Pressure...");
		Pressure::init();
		ESP_LOGI(TAG, "Boot step: initializing Submit...");
		Submit::init();
		ESP_LOGI(TAG, "Boot step: initializing MeshTelemetry...");
		MeshTelemetry::init();

#ifdef CONFIG_SCREENLESS
		// Start network scheduler task, TB MQTT, and Submit task under screenless mode
#ifdef CONFIG_CELLULAR_BACKUP
		xTaskCreate(network_scheduler_task, "network_scheduler", 4096, NULL, 5, NULL);
#else
		Wifi::connect(NULL, NULL, true);
#endif
		Tb::start();
		Submit::enable_autostart();

		// Start the sleep scheduler task
		xTaskCreate(sleep_scheduler_task, "sleep_scheduler", 4096, NULL, 5, NULL);
#endif

#ifndef CONFIG_SCREENLESS
		ESP_LOGI(TAG, "Boot step: initializing StateSniffer...");
		StateSniffer::init();    
		ESP_LOGI(TAG, "Boot step: switching to STATE_SNIFFER...");
		GlobalState::switch_to(GlobalState::STATE_SNIFFER);        
#endif
	}
	else
	{
		ESP_LOGW(TAG, "Enter recovery");
#ifndef CONFIG_SCREENLESS
		RecoveryMode::enter();
#endif
	}
}
