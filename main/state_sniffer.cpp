#ifndef CONFIG_SCREENLESS

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_debug_helpers.h"
#include "global_events.h"
#include "global_state.h"
#include "math.h"
#include "datetime_rtc.h"
#include "state_sniffer.h"
#include "lv_port.h"
#include "tb.h"
#include "time.h"
#include "ui/include/ui_common.h"
#include "ui/resources/ui_resources.h"
#include "ui/views/include/ui_view_alert.h"
#include "ui/views/include/ui_view_sniffer.h"
#include "ui/views/include/ui_view_claiming.h"
#include "units.h"
#include "utils.h"
#include "wifi.h"
#include "env.h"
#include "submit.h"
#include "wifi_config_mode.h"
#include "bsp_lcd.h"
#include "claiming.h"
#include "credentials.h"
#include "backlight.h"
#include "pressure.h"
#include "ws/include/ws_handler.h"
#include "ws/include/ws_data.h"
#include "ws/include/ws_fields.h"
#include "weather_logger.h"
#include "ws/include/ws_gps.h"
#include "public_api.h"
#include "https_client.h"

namespace StateSniffer
{
/** Logger tag */
static const char TAG[] = "StateSniffer";

//
// Local vars
//
/** Task stack */
EXT_RAM_BSS_ATTR StackType_t _update_task_stack[UI_UPDATE_TASK_STACK_SIZE] = {};

/** Task buffer */
StaticTask_t _ui_update_task_buff;

/** Flag used to initialise UI by the  UI task*/
bool _initialized = false;

/** UI monitor task handle */
static TaskHandle_t _ui_update_task_handle = NULL;

/** Last WS packet rendered */
WsFields::FieldPack _last_valid_ws_pack = {};

u32 _last_valid_ws_data_ticks_ts_sec = 0;

/** Weather station content is not too old to present */
bool _ws_content_presentable = false;

/** Sniffer view object */
EXT_RAM_BSS_ATTR UiViewSniffer::View _view_sniffer;

/** Claim view object */
EXT_RAM_BSS_ATTR UiViewClaiming::View _view_claiming;

/** Station low battery alert msg */
EXT_RAM_BSS_ATTR UiViewAlert::View _view_alert_ws_low_bat;

/** Station low battery alert msg popup time interval*/
const int ALERT_WS_LOW_BAT_SHOW_EVERY = 86400;

/** Station is inactive alert msg */
EXT_RAM_BSS_ATTR UiViewAlert::View _view_alert_ws_inactive;

/** Station inactive msg popup time interval*/
const int ALERT_WS_INACTIVE_SHOW_EVERY = 3600;

/** Station abnormal state alert msg */
// EXT_RAM_BSS_ATTR UiViewAlert::View _view_alert_time_sync;

/** Station abnormal state msg popup time interval*/
const int ALERT_WS_BAD_STATE_SHOW_EVERY = 86400;

/** Station abnormal values alert msg */
EXT_RAM_BSS_ATTR UiViewAlert::View _view_alert_ws_abnormal_values;

/** Station abnormal values msg popup time interval*/
const int ALERT_WS_BAD_VALUES_SHOW_EVERY = 86400;

/** Station low signal reception alert msg */
EXT_RAM_BSS_ATTR UiViewAlert::View _view_alert_ws_low_reception;

/** Station low signal reception msg popup time interval*/
const int ALERT_WS_RECEPTION_SHOW_EVERY = 3600;

/** Network connectivity alert msg */
EXT_RAM_BSS_ATTR UiViewAlert::View _view_alert_net_con;

/** Network connectivity alert msg popup time interval*/
const int ALERT_NET_CON_SHOW_EVERY = 300;

/** SD Card Operation alert msg */
EXT_RAM_BSS_ATTR UiViewAlert::View _view_alert_sd_op;

/** Timer to save last on screen tab*/
TimerHandle_t _timer_last_tab = NULL;

//
// Local functions
//
void ui_update_task(void *data);
void update_ws_content();
void publish_ws_unpair();
void set_ws_field_pack(WsFields::FieldPack* field_pack);
static bool field_pack_has_weather(WsFields::FieldPack* pack);
static bool try_hydrate_ws_pack_from_buffer(uint32_t target_station_id);

/******************************************************************************
 * @brief Show/Hide alert pop-ups.
 ******************************************************************************/
void alert_handler(UiViewAlert::View *alert, bool show)
{
	if(NULL == alert)
	{
		return;
	}
	if(show)
	{
		if(!alert->is_time_to_show())
		{
			return;
		}
		alert->show();
	}
	else
	{
		alert->hide();
	}
}

/******************************************************************************
 * Tab changed event callback.
 ******************************************************************************/
void tab_changed_cb(lv_event_t *event)
{
	lv_event_code_t  event_code = lv_event_get_code(event);
	if(event_code == LV_EVENT_VALUE_CHANGED)
	{
		//UiViewSniffer::View* view_sniffer = (UiViewSniffer::View*)ctx;
		//ESP_LOGW(TAG, "Tab changed: %d", view_sniffer->_tabview_main.get_active_tab_index());		
		xTimerStart(_timer_last_tab, (TickType_t)0);
	}
}

/******************************************************************************
 * WiFi config button callback.
 ******************************************************************************/
void wifi_config_btn_cb(lv_event_t *event)
{
	lv_obj_t *obj = lv_event_get_current_target(event);
	lv_event_code_t  event_code = lv_event_get_code(event);

	if(event_code != LV_EVENT_CLICKED)
	{
		return;
	}

	ESP_LOGI(TAG, "Prompt for WifiConfigMode");
	static const char *btns[] = {"Cancel", "Continue", ""};

	lv_obj_t * msgbox = lv_msgbox_create(NULL,
							"WARNING!",
							"This action will configure WiFi on this device. \n\nContinue to this action?"							
							, btns, false);

	lv_obj_set_width(msgbox, UI_DEFAULT_MSGBOX_W);
	lv_obj_add_event_cb(msgbox, [](lv_event_t *event) {
		lv_obj_t *obj = lv_event_get_current_target(event);
		auto active_btn = lv_msgbox_get_active_btn(obj);
			if(active_btn == 0)
			{
				lv_msgbox_close_async(obj);
			}
			else
			{
				WifiConfigMode::set_on_next_boot();
				Utils::safe_restart(Utils::RestartReason::WIFI_CONFIG_REQUEST);
			}
	}, LV_EVENT_VALUE_CHANGED, NULL);
	lv_obj_align(msgbox, LV_ALIGN_CENTER, 0, 0);
}

/******************************************************************************
 * Claim device button callback.
 ******************************************************************************/
void claim_btn_cb(lv_event_t *event)
{
	lv_obj_t *obj = lv_event_get_current_target(event);
	lv_event_code_t  event_code = lv_event_get_code(event);

	if(event_code != LV_EVENT_CLICKED)
	{
		return;
	}

	if(Tb::is_connected())
	{
		_view_claiming.create(_view_sniffer.get_popups_layer())
			.size(392, 379)
			.align(LV_ALIGN_CENTER, 0, 0)
			.set_max_fail_streak(TB_CLAIM_ENABLE_MAX_FAILS);
		_view_sniffer.set_background_touch(false);
		_view_claiming._btn_close.set_cb([](lv_event_t *event){			
			lv_event_code_t  event_code = lv_event_get_code(event);
			if(event_code != LV_EVENT_CLICKED)
			{
				return;
			}

			UiViewClaiming::View *claiming_view = (UiViewClaiming::View*) lv_event_get_user_data(event);
			if(claiming_view->is_open())
			{
				_view_sniffer.set_background_touch(true);
				GlobalEvents::UiEvent ui_event = {.ui_event_type = GlobalEvents::UiEventType::CLAIM_FINISHED,
									  .tstamp = static_cast<u32>(Utils::get_tick_sec())};
				GlobalEvents::publish(GlobalEvents::EventId::UI_EVENT, &ui_event, sizeof(GlobalEvents::UiEvent));
				claiming_view->del_self();
			}		
		}, &_view_claiming);

		GlobalEvents::UiEvent ui_event = {.ui_event_type = GlobalEvents::UiEventType::CLAIM_REQUESTED,
									  .tstamp = static_cast<u32>(Utils::get_tick_sec())};
		GlobalEvents::publish(GlobalEvents::EventId::UI_EVENT, &ui_event, sizeof(GlobalEvents::UiEvent));
	}
	else
	{
		static const char *btns[] = {"OK", ""};
		lv_obj_t * msgbox;
		if(Wifi::is_connected())
		{
			msgbox = lv_msgbox_create(NULL,
								"NO CLOUD CONNECTION!",
"It seems that your device is not connected to the Cloud. \
Please make sure your WiFi is configured correctly."			
								, btns, false);
		}
		else
		{
			msgbox = lv_msgbox_create(NULL,
								"NO INTERNET!",
"It seems that your device is not connected to the Internet. \
Please make sure your WiFi is configured correctly."
															
								, btns, false);
		}
		lv_obj_set_width(msgbox, UI_DEFAULT_MSGBOX_W);
		lv_obj_add_event_cb(msgbox, [](lv_event_t *event) {
			lv_obj_t *obj = lv_event_get_current_target(event);
			auto active_btn = lv_msgbox_get_active_btn(obj);
				if(active_btn == 0)
				{
					lv_msgbox_close_async(obj);
				}
		}, LV_EVENT_VALUE_CHANGED, NULL);
		lv_obj_align(msgbox, LV_ALIGN_CENTER, 0, 0);
	}	
}

/******************************************************************************
 * 
 ******************************************************************************/
void ws_unpair_btn_cb(lv_event_t *event)
{
	lv_obj_t *obj = lv_event_get_current_target(event);
	lv_event_code_t  event_code = lv_event_get_code(event);

	if(event_code != LV_EVENT_CLICKED)
	{
		return;
	}

	static const char *btns[] = {"Cancel", "Continue", ""};
	lv_obj_t * msgbox = lv_msgbox_create(NULL,
							"WARNING!",
							"This action will unpair your weather station. \n\nContinue to this action?"							
							, btns, false);

	lv_obj_set_width(msgbox, UI_DEFAULT_MSGBOX_W);
	lv_obj_add_event_cb(msgbox, [](lv_event_t *event) {
		lv_obj_t *obj = lv_event_get_current_target(event);
		auto active_btn = lv_msgbox_get_active_btn(obj);

		if(active_btn == 0)
		{
			lv_msgbox_close_async(obj);
		}
		else
		{
			WsHandler::unpair();
			lv_msgbox_close_async(obj);			
		}
	}, LV_EVENT_VALUE_CHANGED, NULL);
	lv_obj_align(msgbox, LV_ALIGN_CENTER, 0, 0);
	
}

/******************************************************************************
 * Publishes a WS_UPDATE_CONTENT event.
 * This is used by handlers that are called when the UI semaphore is already locked by the lvgl.
 * If for example a dropdown handler is called, the UI semaphore is already locked by the lvgl,
 * so we cannot have a direct call to update_ws_content().
 ******************************************************************************/
void publish_ws_content_update()
{
	GlobalEvents::UiEvent ui_event = {.ui_event_type = GlobalEvents::UiEventType::WS_UPDATE_CONTENT,
									  .tstamp = static_cast<u32>(Utils::get_tick_sec())};
	GlobalEvents::publish(GlobalEvents::EventId::UI_EVENT, &ui_event, sizeof(GlobalEvents::UiEvent));
}

/******************************************************************************
 * Frequency dropdown callback. TODO Move UI code to a msgbox component.
 ******************************************************************************/
void frequency_dd_cb(lv_event_t *event)
{
	lv_obj_t *obj = lv_event_get_current_target(event);
	lv_event_code_t  event_code = lv_event_get_code(event);
	// Takes current value on click event so it can be used as previous value
	// on change event.
	static char prev_item_str[15] = "";
	static int prev_item_id = 0;

	if(event_code == LV_EVENT_VALUE_CHANGED)
	{
		// The row normally shows live power/RSSI/SNR diagnostics. Reveal the
		// actual selected option while the user changes the frequency.
		_view_sniffer._view_settings._dd_ws_freq.clear_display_text();
		// Trigger only if selection changed
		// Event is called VALUE_CHANGED, yet it triggers even if you select the same value
		static char cur_item[15] = "";
		lv_dropdown_get_selected_str(obj, cur_item, sizeof(cur_item));

		// lv_obj_set_size(_view_sniffer.get_popups_layer(), UI_SCREEN_WIDTH, UI_SCREEN_HEIGHT);
		// lv_obj_align(_view_sniffer.get_popups_layer(), LV_ALIGN_CENTER, 0, 0);

		if(strcmp(cur_item, prev_item_str) == 0)
		{
			return;
		}

		static const char *btns[] = {"Cancel", "Continue", ""};
		// _view_sniffer.set_background_touch(false);

		lv_obj_t * msgbox = lv_msgbox_create(NULL,
							"WARNING!",
							"This action will change the frequency of your miner."
							"Check your weather station version for compatibility. \n\nContinue to this action?"							
							, btns, false);

		lv_obj_set_width(msgbox, UI_DEFAULT_MSGBOX_W);
		lv_obj_add_event_cb(msgbox, [](lv_event_t *event) {
			lv_obj_t *obj = lv_event_get_current_target(event);
			auto active_btn = lv_msgbox_get_active_btn(obj);
				// _view_sniffer.set_background_touch(true);
				ESP_LOGE(TAG, "Setting freq to %d EVENT %d", (u32)active_btn, event);

				// No
				if(active_btn == 0)
				{
					// lv_obj_del(obj);
					lv_msgbox_close_async(obj);

					// Restore previous selection

					_view_sniffer._view_settings._dd_ws_freq.set_selected(prev_item_id);
				}
				// Yes
				else
				{
					const auto *freq_desc = WsHandler::get_freq_desc(cur_item, WS_FREQ_REGION_NONE);
					if(freq_desc == nullptr)
					{
						ESP_LOGE(TAG, "Invalid freq value");
					}
					else
					{
						Settings::set_freq_region(freq_desc->freq_region);
						Settings::commit();
						lv_msgbox_close_async(obj);
						// esp_restart();
					}
				}
		}, LV_EVENT_VALUE_CHANGED, NULL);
		lv_obj_align(msgbox, LV_ALIGN_CENTER, 0, 0);
	}
	else if(event_code == LV_EVENT_READY)
	{
		// Keep current item before its changed and VALUE_CHANGED is called
		lv_dropdown_get_selected_str(obj, prev_item_str, sizeof(prev_item_str));
		prev_item_id = lv_dropdown_get_selected(obj);

		ESP_LOGE(TAG, "Clicked State: %s", prev_item_str);
	}
}

/******************************************************************************
 * Timezone dropdown callback
 ******************************************************************************/
void timezone_dd_cb(lv_event_t *event)
{
	lv_obj_t *obj = lv_event_get_current_target(event);
	lv_event_code_t  event_code = lv_event_get_code(event);		
	if(event_code == LV_EVENT_VALUE_CHANGED)
	{
		char item[5] = "";
		lv_dropdown_get_selected_str(obj, item, sizeof(item));

		int tz_offset;
		sscanf(item, "%d", &tz_offset);

		Settings::set_tz_offset(tz_offset);
		Settings::commit();
	}
}

/******************************************************************************
 * Screen dim dropdown callback
 ******************************************************************************/
void screen_dim_dd_cb(lv_event_t *event)
{
	lv_obj_t *obj = lv_event_get_current_target(event);
	lv_event_code_t  event_code = lv_event_get_code(event);
	if(event_code == LV_EVENT_VALUE_CHANGED)
	{
		char item[10] = "";
		lv_dropdown_get_selected_str(obj, item, sizeof(item));

		const BacklightLevelDesc *desc = Backlight::get_idle_backlight_level_desc(item, BACKLIGHT_LEVEL_NONE);

		if(desc == nullptr)
		{
			return;
		}	

		Settings::set_idle_backlight_level(desc->level);
		Settings::commit();
		Backlight::set_backlight_level(Settings::get_idle_backlight_level(), BACKLIGHT_WAKE_SETTING_CHANGE);
	}
}

/******************************************************************************
 * Temperature units dropdown callback
 ******************************************************************************/
void temp_units_dd_cb(lv_event_t *event)
{
	lv_obj_t *obj = lv_event_get_current_target(event);
	lv_event_code_t  event_code = lv_event_get_code(event);
	if(event_code == LV_EVENT_VALUE_CHANGED)
	{
		char item[30] = "";
		lv_dropdown_get_selected_str(obj, item, sizeof(item));

		const TempUnitDesc *unit_desc = Units::get_temp_unit_desc(item, TEMP_UNIT_NONE);
		if(unit_desc == nullptr)
		{
			return;
		}	

		Settings::set_temp_unit(unit_desc->unit);
		Settings::commit();

		publish_ws_content_update();
	}
}

/******************************************************************************
 * Precipitation units dropdown callback
 ******************************************************************************/
void precip_units_dd_cb(lv_event_t *event)
{
	lv_obj_t *obj = lv_event_get_current_target(event);
	lv_event_code_t  event_code = lv_event_get_code(event);
	if(event_code == LV_EVENT_VALUE_CHANGED)
	{
		char item[30] = "";
		lv_dropdown_get_selected_str(obj, item, sizeof(item));

		const PrecipUnitDesc *unit_desc = Units::get_precip_unit_desc(item, PRECIP_UNIT_NONE);
		if(unit_desc == nullptr)
		{
			return;
		}	

		Settings::set_precip_unit(unit_desc->unit);
		Settings::commit();

		publish_ws_content_update();
	}
}

/******************************************************************************
 * Wind units dropdown callback
 ******************************************************************************/
void wind_units_dd_cb(lv_event_t *event)
{
	lv_obj_t *obj = lv_event_get_current_target(event);
	lv_event_code_t  event_code = lv_event_get_code(event);
	if(event_code == LV_EVENT_VALUE_CHANGED)
	{
		char item[30] = "";
		lv_dropdown_get_selected_str(obj, item, sizeof(item));

		const WindSpeedUnitDesc *unit_desc = Units::get_wind_speed_unit_desc(item, WIND_SPEED_UNIT_NONE);
		if(unit_desc == nullptr)
		{
			return;
		}	

		Settings::set_wind_speed_unit(unit_desc->unit);
		Settings::commit();

		publish_ws_content_update();
	}
}

/******************************************************************************
 * Pressure units dropdown callback
 ******************************************************************************/
void press_units_dd_cb(lv_event_t *event)
{
	lv_obj_t *obj = lv_event_get_current_target(event);
	lv_event_code_t  event_code = lv_event_get_code(event);
	if(event_code == LV_EVENT_VALUE_CHANGED)
	{
		char item[30] = "";
		lv_dropdown_get_selected_str(obj, item, sizeof(item));

		const PressureUnitDesc *unit_desc = Units::get_press_unit_desc(item, PRESSURE_UNIT_NONE);
		if(unit_desc == nullptr)
		{
			return;
		}		

		Settings::set_press_unit(unit_desc->unit);
		Settings::commit();

		publish_ws_content_update();
	}
}

/******************************************************************************
 * Weather station type dropdown callback
 ******************************************************************************/
void ws_type_dd_cb(lv_event_t *event)
{
	lv_event_code_t  event_code = lv_event_get_code(event);
	lv_obj_t *obj = lv_event_get_current_target(event);
	// Takes current value on click event so it can be used as previous value
	// on change event.
	static char prev_item_str[15] = "";
	static int prev_item_id = 0;

	if(event_code == LV_EVENT_VALUE_CHANGED)
	{
		static char cur_item[15] = "";
		lv_dropdown_get_selected_str(obj, cur_item, sizeof(cur_item));

		ESP_LOGI(TAG, "Prompt for WsType");
		static const char *btns[] = {"Cancel", "Continue", ""};

		lv_obj_t * msgbox = lv_msgbox_create(NULL,
								"WARNING!",
								"This action will change the station type and restart the device. \n\nContinue to this action?"							
								, btns, false);
		
		if(strcmp(cur_item, prev_item_str) == 0)
		{
			return;
		}

		lv_obj_set_width(msgbox, UI_DEFAULT_MSGBOX_W);
		lv_obj_add_event_cb(msgbox, [](lv_event_t *event) {
			lv_obj_t *obj = lv_event_get_current_target(event);
			auto active_btn = lv_msgbox_get_active_btn(obj);
			if(active_btn == 0)
			{
				lv_msgbox_close_async(obj);
				_view_sniffer._view_settings._dd_ws_type.set_selected(prev_item_id);
			}
			else
			{
				const WsTypeDesc *ws_type_desc = Utils::lookup_desc_array(WEATHER_STATION_TYPES, (sizeof(WEATHER_STATION_TYPES)/sizeof(*WEATHER_STATION_TYPES)),
																			&WsTypeDesc::title, cur_item);
				if(ws_type_desc == nullptr)
				{
					return;
				}
				WsHandler::unpair();

				Settings::set_ws_type(ws_type_desc->type);
				Settings::commit();
				vTaskDelay(pdMS_TO_TICKS(100)); // TODO: Why this one needs delay and freq is not???
				Utils::safe_restart(Utils::RestartReason::SETTINGS_CHANGE);

			}
		}, LV_EVENT_VALUE_CHANGED, NULL);
		lv_obj_align(msgbox, LV_ALIGN_CENTER, 0, 0);
	}else if(event_code == LV_EVENT_READY)
	{
		// Keep current item before its changed and VALUE_CHANGED is called
		lv_dropdown_get_selected_str(obj, prev_item_str, sizeof(prev_item_str));
		prev_item_id = lv_dropdown_get_selected(obj);
	}
}

/******************************************************************************
 * Handle APP events
 ******************************************************************************/
void event_handler(void *arg, esp_event_base_t base, int32_t id, void *event_data)
{
	if(NULL == event_data || !_initialized)
	{
		return;
	}
	else if(id == (int32_t)GlobalEvents::EventId::WIFI_STATE_CHANGED)
	{
		UiCommon::lock();
		if(*((GlobalEvents::WifiStateChangedEvent *)event_data) == GlobalEvents::WifiStateChangedEvent::CONNECTED)
		{
			_view_sniffer._view_statusbar.update_icon_status_wifi(UiTypes::StatusIconComponent::StatusType::POS,
																  UiViewStatusBar::WiFiSignalType::GOOD);
		}
		else if(*((GlobalEvents::WifiStateChangedEvent *)event_data) ==
				GlobalEvents::WifiStateChangedEvent::CONNECTED_NO_INTERNET)
		{
			_view_sniffer._view_statusbar.update_icon_status_wifi(UiTypes::StatusIconComponent::StatusType::WARN,
																  UiViewStatusBar::WiFiSignalType::GOOD);
		}
		else
		{
			_view_sniffer._view_statusbar.update_icon_status_wifi(UiTypes::StatusIconComponent::StatusType::NEG,
																  UiViewStatusBar::WiFiSignalType::GOOD);

		}
		UiCommon::unlock();
	}
	else if(id == (int32_t)GlobalEvents::EventId::MQTT_STATE_CHANGED)
	{
		UiCommon::lock();
		if(*((GlobalEvents::MqttStateChangedEvent *)event_data) == GlobalEvents::MqttStateChangedEvent::CONNECTED)
		{
			_view_sniffer._view_statusbar.update_icon_status_tb(UiTypes::StatusIconComponent::StatusType::POS);
		}
		else
		{
			_view_sniffer._view_statusbar.update_icon_status_tb(UiTypes::StatusIconComponent::StatusType::NEG);
		}
		UiCommon::unlock();
	}
	else if(id == (int32_t)GlobalEvents::EventId::WS_DATA_RECEIVED)
	{
		UiCommon::lock();
		set_ws_field_pack((WsFields::FieldPack *)event_data);
		update_ws_content();
		UiCommon::unlock();

		// Feed the packet to WeatherLogger for SRAM aggregation and SD Card logging
		WeatherLogger::push_raw((WsFields::FieldPack *)event_data);
	}
	else if(id == (int32_t)GlobalEvents::EventId::WS_STATUS_CHANGED)
	{
		GlobalEvents::WsStatusChangedEvent ws_status_event = *((GlobalEvents::WsStatusChangedEvent *)event_data);
		UiCommon::lock();
		if(ws_status_event == GlobalEvents::WsStatusChangedEvent::INACTIVE || ws_status_event == GlobalEvents::WsStatusChangedEvent::NONE)
		{
			_ws_content_presentable = false;
			_view_sniffer._view_statusbar.update_icon_status_ws(UiTypes::StatusIconComponent::StatusType::NEG);
			update_ws_content();
		}
		else if(ws_status_event == GlobalEvents::WsStatusChangedEvent::ACTIVE_BAD_DATA)
		{
			_view_sniffer._view_statusbar.update_icon_status_ws(UiTypes::StatusIconComponent::StatusType::WARN);
			//Show station abnormal values alert
			alert_handler(&_view_alert_ws_abnormal_values, true);
		}
		else if(ws_status_event == GlobalEvents::WsStatusChangedEvent::ACTIVE_LOW_RECEPTION)
		{
			//Show station reception alert
			alert_handler(&_view_alert_ws_low_reception, true);
			_view_sniffer._view_statusbar.update_icon_status_ws(UiTypes::StatusIconComponent::StatusType::WARN);
			if(!_ws_content_presentable && WsHandler::get_pair_state() == WsHandler::PairState::PAIRED &&
			   try_hydrate_ws_pack_from_buffer(WsHandler::get_paired_id()))
			{
				update_ws_content();
			}
		}
		else if(ws_status_event == GlobalEvents::WsStatusChangedEvent::ACTIVE_OK || ws_status_event == GlobalEvents::WsStatusChangedEvent::ACTIVE_BUSY)
		{
			//Hide station inactive alert
			alert_handler(&_view_alert_ws_inactive, false);
			//Hide station reception alert
			alert_handler(&_view_alert_ws_low_reception, false);
			_view_sniffer._view_statusbar.update_icon_status_ws(UiTypes::StatusIconComponent::StatusType::POS);
			if(!_ws_content_presentable && WsHandler::get_pair_state() == WsHandler::PairState::PAIRED &&
			   try_hydrate_ws_pack_from_buffer(WsHandler::get_paired_id()))
			{
				update_ws_content();
			}
		}		
		UiCommon::unlock();		
	}
	else if(id == (int32_t)GlobalEvents::EventId::WS_INFO_CHANGED)
	{
		UiCommon::lock();
		GlobalEvents::WsInfoChangedEvent* ws_info = ((GlobalEvents::WsInfoChangedEvent *)event_data);

		if(ws_info->station != NULL)
		{
			const WsTypeDesc *ws_type_desc = Utils::lookup_desc_array(WEATHER_STATION_TYPES, (sizeof(WEATHER_STATION_TYPES)/sizeof(*WEATHER_STATION_TYPES)),
																		&WsTypeDesc::type, ws_info->station->TYPE);
			if(ws_type_desc != NULL)
			{
				_view_sniffer._view_settings._dd_ws_type.set_selected_text(ws_type_desc->title);
			}
			_view_sniffer.enable_ws_info(ws_info->station->CONFIG.user_select_freq);

			if(ws_info->freq_desc != NULL)
			{
				_view_sniffer._view_settings._dd_ws_freq.set_selected_text(ws_info->freq_desc->title);
				_view_sniffer._view_settings._dd_ws_freq.clickable(ws_info->station->CONFIG.user_select_freq);
			}

			if(ws_info->pair_state == WsHandler::PairState::PAIRED)
			{
				if(ws_info->station->CONFIG.extended_pair_info)
				{
					_view_sniffer.set_ws_pair_info(true, ws_info->pair_data.sn, sizeof(ws_info->pair_data.sn));
				}
				else
				{
					_view_sniffer.set_ws_pair_info(true, ws_info->pair_data.id);
				}
				if(!_ws_content_presentable && try_hydrate_ws_pack_from_buffer(ws_info->pair_data.id))
				{
					update_ws_content();
				}
			}
			else
			{
				_view_sniffer.set_ws_pair_info(false, 0);
			}
		}
		UiCommon::unlock();
	}
	else if(id == (int32_t)GlobalEvents::EventId::UI_EVENT)
	{
		if(((GlobalEvents::UiEvent *)event_data)->ui_event_type == GlobalEvents::UiEventType::WS_UPDATE_CONTENT)
		{
			UiCommon::lock();
			update_ws_content();
			UiCommon::unlock();
		}
	}
	else if(id == (int32_t)GlobalEvents::EventId::CLAIM_ENABLE_RESPONSE)
	{
		if (!_view_claiming.is_open())
		{
			return;
		}
		
		UiCommon::lock();
		if(((GlobalEvents::ClaimEnRespEvent *)event_data)->type == GlobalEvents::ClaimEnRespStatus::CLAIM_ENABLE_SUCCESS)
		{
			_view_claiming.claim_enable_success(Credentials::get_serial_str(), Credentials::get_serial_str_length(), ((GlobalEvents::ClaimEnRespEvent *)event_data)->code);
		}
		else if(((GlobalEvents::ClaimEnRespEvent *)event_data)->type == GlobalEvents::ClaimEnRespStatus::CLAIM_ENABLE_FAIL)
		{
			_view_claiming.claim_enable_failed();
		}
		else if(((GlobalEvents::ClaimEnRespEvent *)event_data)->type == GlobalEvents::ClaimEnRespStatus::CLAIM_ENABLE_TIMEOUT)
		{
			_view_sniffer.set_background_touch(true);
			_view_claiming.del_self();
		}
		UiCommon::unlock();
	}
	else if(id == (int32_t)GlobalEvents::EventId::RM_TS_DATA_UPDATED)
	{
		// Device name is a plain cloud-assigned setting (Settings::dev_name),
		// unrelated to remote-maintenance jobs - refresh the LCD labels on every
		// remote-settings sync, not only when a RM job happens to be scheduled
		// (run_ts != 0). Previously this update lived inside the run_ts-only
		// branch below, so a device name pushed by the cloud with no RM job
		// pending was persisted (web UI showed it fine, reading Settings live)
		// but the LCD fields stayed blank until reboot.
		UiCommon::lock();
		_view_sniffer._view_crypto.update_device_name(Settings::get_device_name());
		_view_sniffer.set_device_name_info(Settings::get_device_name());
		UiCommon::unlock();

		if(((GlobalEvents::RmDataUpdatedEvent *)event_data)->run_ts == 0)
		{
			UiCommon::lock();
			_view_sniffer._view_crypto.set_pending_state(true);
			UiCommon::unlock();
		}
		else
		{
			if(_view_sniffer._view_crypto.get_last_rewards_update_ts() == 0
				|| _view_sniffer._view_crypto.get_last_rewards_update_ts() < ((GlobalEvents::RmDataUpdatedEvent *)event_data)->run_ts)
			{
				PublicApi::req_rewards_data();
			}

			if(_view_sniffer._view_crypto.get_last_dev_info_update_ts() == 0
				|| _view_sniffer._view_crypto.get_last_dev_info_update_ts() < ((GlobalEvents::RmDataUpdatedEvent *)event_data)->run_ts)
			{
				PublicApi::req_dev_info_data();
			}
		}
	}
	else if(id == (int32_t)GlobalEvents::EventId::PUB_API_DEV_INFO)
	{
		UiCommon::lock();
		_view_sniffer._view_crypto.set_pending_state(false);
		GlobalEvents::PubApiDevInfEvent* dev_info_evt = (GlobalEvents::PubApiDevInfEvent*)event_data;

		_view_sniffer._view_crypto.update_qod(dev_info_evt->qod_score);

		const char* pol_annotation = UI_POL_TILE_TITLE_TEXT_OK;
		if(dev_info_evt->pol_reason == PublicApi::PolReason::LOCATION_NOT_VERIFIED)
		{
			pol_annotation = UI_POL_TILE_TITLE_TEXT_NOT_VER;
		}
		else if(dev_info_evt->pol_reason == PublicApi::PolReason::NO_LOCATION_DATA)
		{
			pol_annotation = UI_POL_TILE_TITLE_TEXT_NO_LOC;
		}
		else if(dev_info_evt->pol_reason == PublicApi::PolReason::NONE)
		{
			pol_annotation = UI_POL_TILE_TITLE_TEXT_OK;
		}
		_view_sniffer._view_crypto.update_pol(pol_annotation, dev_info_evt->address);

		_view_sniffer._view_crypto.set_last_dev_info_update_ts(time(NULL));
		
		UiCommon::unlock();
	}
	else if(id == (int32_t)GlobalEvents::EventId::PUB_API_REWARDS)
	{
		UiCommon::lock();
		_view_sniffer._view_crypto.set_pending_state(false);
		GlobalEvents::PubApiRewardsEvent * rewards_evt = (GlobalEvents::PubApiRewardsEvent*)event_data;
		_view_sniffer._view_crypto.update_rewards(rewards_evt->total_rewards, rewards_evt->latest_total_reward);
		_view_sniffer._view_crypto.set_last_rewards_update_ts(time(NULL));

		static char buff_time[10] = "";
		Utils::get_formatted_datetime(buff_time, sizeof(buff_time), rewards_evt->ts, "%d %b", false);

		_view_sniffer._view_crypto.set_last_update(buff_time);
		UiCommon::unlock();
	}
}

/******************************************************************************
 * Init
 ******************************************************************************/
void init()
{
	GlobalEvents::subscribe(GlobalEvents::EventId::ANY_EVENT, event_handler, NULL);

	// Create the task using static creation API
	_ui_update_task_handle = xTaskCreateStaticPinnedToCore(ui_update_task, UI_UPDATE_TASK_NAME, sizeof(_update_task_stack), NULL,
												UI_UPDATE_TASK_PRIORITY, _update_task_stack, &_ui_update_task_buff, 1);

	_timer_last_tab = xTimerCreate( NULL, pdMS_TO_TICKS(UI_LAST_TAB_SAVE_SEC*1000), pdFALSE, ( void * ) 0, []( TimerHandle_t xTimer ){
		uint16_t last_tab = _view_sniffer._tabview_main.get_active_tab_index();
		if(last_tab != 0)
		{
			Settings::set_last_tab(last_tab);
			Settings::commit();
		}
	}); 

	if(_ui_update_task_handle == NULL)
	{
		ESP_LOGE(TAG, "Could not create Sniffer state UI task.");
	}
}

/******************************************************************************
 * Entering state
 ******************************************************************************/
void on_enter()
{
	// Deinit first, in case coming from a mode that used wifi
	Wifi::connect(NULL, NULL, true);

	// FoSniffer::start_capture();
	// Gps::enable();
	Tb::start();
	//RTC::sync_from_ntp_async();
	Submit::enable_autostart();
	Claiming::init();

	PublicApi::init();
	HttpsClient::init();

	// setup_ui();
	vTaskResume(_ui_update_task_handle);
}

/******************************************************************************
 * Leaving state, cleanup
 ******************************************************************************/
void leave()
{
	// WsHandler::stop_capture();
	// Gps::disable();
	Tb::stop();
	// Submit::disable_autostart();

	auto evg = GlobalState::get_event_group();

	const EventBits_t wait_flags = GlobalState::EV_FO_SNIFFER_CAPTURE_STOPPED_BIT | GlobalState::EV_GPS_DISABLED_BIT |
								   GlobalState::EV_TB_DISCONNECTED_BIT | GlobalState::EV_SUBMIT_AUTO_DISABLED_BIT;

	auto bits = xEventGroupWaitBits(evg, wait_flags, false, true, pdMS_TO_TICKS(8000));

	if((bits & wait_flags) != wait_flags)
	{
		ESP_LOGE(TAG, "Timeout, not all flags set: 0x%X", bits);
	}

	Wifi::disconnect();
	Wifi::deinit();

	if(_ui_update_task_handle != NULL)
		vTaskSuspend(_ui_update_task_handle);

	ESP_LOGW(TAG, "Done exiting sniffer.");
}

/******************************************************************************
 * Redraw changed values/units for weather data
 * Needs UI locking to call it!!!
 ******************************************************************************/
void update_ws_content()
{
	_view_sniffer._view_settings._dd_units_temperature.set_selected(Settings::get_temp_unit());
	_view_sniffer._view_settings._dd_units_wind_speed.set_selected(Settings::get_wind_speed_unit());
	_view_sniffer._view_settings._dd_units_precipitation.set_selected(Settings::get_precip_unit());
	_view_sniffer._view_settings._dd_units_pressure.set_selected(Settings::get_press_unit());

	const WindSpeedUnitDesc *wind_descr = Units::get_wind_speed_unit_desc(nullptr, Settings::get_wind_speed_unit());
	if(NULL != wind_descr)
	{
		_view_sniffer.update_wind_units((char *)wind_descr->symbol);
	}

	const PrecipUnitDesc *precip_descr = Units::get_precip_unit_desc(nullptr, Settings::get_precip_unit());
	if(NULL != precip_descr)
	{
		_view_sniffer.update_precip_units((char *)precip_descr->symbol);
	}

	const PressureUnitDesc *press_descr = Units::get_press_unit_desc(nullptr, Settings::get_press_unit());
	if(NULL != press_descr)
	{
		_view_sniffer.update_pressure_units((char *)press_descr->symbol);
	}

	const TempUnitDesc *temp_descr = Units::get_temp_unit_desc(nullptr, Settings::get_temp_unit());
	if(NULL != temp_descr)
	{
		_view_sniffer.update_dew_units((char *)temp_descr->symbol);
	}

	if(_ws_content_presentable)
	{
		float ws_data_temp;
		bool temp_avail = WsFields::get_field_val_from_pack(&_last_valid_ws_pack, 
							WsFields::FieldType::WEATHER_TEMP, NULL, &ws_data_temp) == WsCommon::RetRes::OK &&
							NULL != temp_descr;
		float ws_data_hum;
		bool hum_avail = WsFields::get_field_val_from_pack(&_last_valid_ws_pack, 
							WsFields::FieldType::WEATHER_HUM, NULL, &ws_data_hum) == WsCommon::RetRes::OK;
		float ws_data_wind_speed;
		bool wind_speed_avail = WsFields::get_field_val_from_pack(&_last_valid_ws_pack, 
							WsFields::FieldType::WEATHER_WIND_SPEED, NULL, &ws_data_wind_speed) == WsCommon::RetRes::OK;
		float ws_data_wind_gust;
		bool wind_gust_avail = WsFields::get_field_val_from_pack(&_last_valid_ws_pack, 
							WsFields::FieldType::WEATHER_WIND_GUST, NULL, &ws_data_wind_gust) == WsCommon::RetRes::OK;
		uint16_t ws_data_wind_dir;
		bool wind_dir_avail = WsFields::get_field_val_from_pack(&_last_valid_ws_pack, 
							WsFields::FieldType::WEATHER_WIND_DIR, NULL, &ws_data_wind_dir) == WsCommon::RetRes::OK;
		float ws_data_irradiance;
		bool irr_avail = WsFields::get_field_val_from_pack(&_last_valid_ws_pack, 
							WsFields::FieldType::WEATHER_SOLAR_RAD, NULL, &ws_data_irradiance) == WsCommon::RetRes::OK;
		uint8_t ws_data_uv;
		bool uv_avail = WsFields::get_field_val_from_pack(&_last_valid_ws_pack, 
							WsFields::FieldType::WEATHER_UV_INDEX, NULL, &ws_data_uv) == WsCommon::RetRes::OK;
		bool ws_data_low_bat;
		bool low_bat_avail = WsFields::get_field_val_from_pack(&_last_valid_ws_pack, 
							WsFields::FieldType::META_BAT_LOW, NULL, &ws_data_low_bat) == WsCommon::RetRes::OK;

		float ws_data_precip_rate;
		bool precip_rate_avail = WsFields::get_field_val_from_pack(&_last_valid_ws_pack, 
			WsFields::FieldType::WEATHER_PRECIP_RATE, NULL, &ws_data_precip_rate) == WsCommon::RetRes::OK;

		float ws_data_pressure;
		bool pressure_avail = WsFields::get_field_val_from_pack(&_last_valid_ws_pack, 
			WsFields::FieldType::WEATHER_PRESSURE, NULL, &ws_data_pressure) == WsCommon::RetRes::OK;

		if(temp_avail)
		{
			_view_sniffer.update_temp(Units::convert_temp(ws_data_temp, Settings::get_temp_unit()),
				(char *)temp_descr->symbol);
			if(hum_avail)
			{
				_view_sniffer.update_humidity(ws_data_hum);
				_view_sniffer.update_dew(Units::convert_temp(Units::calculate_dew(ws_data_hum, ws_data_temp),
								Settings::get_temp_unit()));
				if(wind_speed_avail)
				{
					_view_sniffer.update_feels_like(
					Units::convert_temp(Units::calculate_feels_like(ws_data_temp, ws_data_wind_speed, ws_data_hum),
					Settings::get_temp_unit()),
					(char *)temp_descr->symbol);
				}
				
			}			
		}
		if(wind_speed_avail && wind_gust_avail)
		{
			_view_sniffer.update_wind_speed_and_gust(Units::convert_wind_speed(ws_data_wind_speed, Settings::get_wind_speed_unit()),
												Units::convert_wind_speed(ws_data_wind_gust, Settings::get_wind_speed_unit()));
		}
		if(wind_dir_avail)
		{
			_view_sniffer.update_wind_dir(ws_data_wind_dir);
		}
		if(uv_avail)
		{
			_view_sniffer.update_uv(ws_data_uv);
		}
		if(irr_avail)
		{
			_view_sniffer.update_irradiance(ws_data_irradiance);
		}
		if(low_bat_avail)
		{
			_view_sniffer.set_ws_status(ws_data_low_bat, WsHandler::get_packet_reception());
		}
		const auto *freq_desc = WsHandler::get_freq_desc(nullptr, Settings::get_freq_region());
		int16_t station_rssi = 0;
		int8_t station_snr = 0;
		bool signal_available = WsHandler::get_station_signal(&station_rssi, &station_snr);
		_view_sniffer.set_station_radio_info(freq_desc != nullptr ? freq_desc->title : "--",
			WsHandler::get_station_power_setting_title(), station_rssi, station_snr,
			signal_available);
		float live_precip_rate = precip_rate_avail ? ws_data_precip_rate : WsData::get_precip_rate();
		float daily_precip = WsData::get_daily_precip();
		_view_sniffer.update_precip(Units::convert_precip(daily_precip, Settings::get_precip_unit()),
									Units::convert_precip(live_precip_rate, Settings::get_precip_unit()));
		if(pressure_avail)
		{
			_view_sniffer.update_pressure(Units::convert_press(ws_data_pressure, Settings::get_press_unit()));
		}
		else
		{
			_view_sniffer.update_pressure(Units::convert_press(Pressure::get_latest_val(), Settings::get_press_unit()));
		}

		_view_sniffer.update_daily_extremes(WsData::get_daily_extremes());
	}
	else
	{
		_view_sniffer.empty_weather_content();
		_view_sniffer.update_pressure(Units::convert_press(Pressure::get_latest_val(), Settings::get_press_unit()));
		//_view_sniffer._view_settings.empty_ws_bat_level();
	}
}

/******************************************************************************
 * Create all UI components and set initial values
 ******************************************************************************/
void setup_ui()
{
	UiCommon::lock();

	ESP_LOGI(TAG, "Setting up UI");

	_view_sniffer.create(NULL);

	_view_sniffer._view_settings._button_unpair.set_cb(ws_unpair_btn_cb, NULL);
	_view_sniffer._view_settings._button_wifi_config.set_cb(wifi_config_btn_cb, NULL);
	_view_sniffer._view_settings._button_claim.set_cb(claim_btn_cb, NULL);
	_view_sniffer._view_settings._dd_ws_freq.set_cb(frequency_dd_cb, NULL);
	_view_sniffer._view_settings._dd_units_precipitation.set_cb(precip_units_dd_cb, NULL);
	_view_sniffer._view_settings._dd_units_pressure.set_cb(press_units_dd_cb, NULL);
	_view_sniffer._view_settings._dd_screen_dim.set_cb(screen_dim_dd_cb, NULL);
	_view_sniffer._view_settings._dd_units_temperature.set_cb(temp_units_dd_cb, NULL);
	_view_sniffer._view_settings._dd_timezone.set_cb(timezone_dd_cb, NULL);
	_view_sniffer._view_settings._dd_units_wind_speed.set_cb(wind_units_dd_cb, NULL);
	_view_sniffer._view_settings._dd_ws_type.set_cb(ws_type_dd_cb, NULL);

	_view_sniffer.set_cb(tab_changed_cb, NULL);
	_view_sniffer.set_act_tab(Settings::get_last_tab());
	const auto *bkl_desc = Backlight::get_idle_backlight_level_desc(nullptr, Settings::get_idle_backlight_level());
	if(bkl_desc != nullptr)
	{
		_view_sniffer._view_settings._dd_screen_dim.set_selected_text(bkl_desc->title);
	}
	_view_sniffer._view_settings._dd_timezone.set_selected(Settings::get_tz_offset() + 12);
	_view_sniffer._view_settings._dd_units_temperature.set_selected(Settings::get_temp_unit());
	_view_sniffer._view_settings._dd_units_wind_speed.set_selected(Settings::get_wind_speed_unit());
	_view_sniffer._view_settings._dd_units_precipitation.set_selected(Settings::get_precip_unit());
	_view_sniffer._view_settings._dd_units_pressure.set_selected(Settings::get_press_unit());
	if(WsHandler::get_cur_station() != NULL)
	{
		_view_sniffer._view_settings._dd_ws_type.set_selected(
			WsHandler::get_cur_station()->TYPE);
	}

	_view_sniffer.set_fw_ver_info(APP_VERSION);

	_view_sniffer.set_serial_info(Credentials::get_serial_str());
	_view_sniffer.set_device_name_info(Settings::get_device_name());

	if(WsHandler::get_cur_station() != NULL)
	{
		const WsTypeDesc *ws_type_desc = Utils::lookup_desc_array(WEATHER_STATION_TYPES, (sizeof(WEATHER_STATION_TYPES)/sizeof(*WEATHER_STATION_TYPES)),
																			&WsTypeDesc::type, WsHandler::get_cur_station()->TYPE);
		if(ws_type_desc != NULL)
		{
			_view_sniffer._view_settings._dd_ws_type.set_selected_text(ws_type_desc->title);
		}
		
		if(WsHandler::get_cur_station() != NULL)
		{
			_view_sniffer.enable_ws_info(WsHandler::get_cur_station()->CONFIG.user_select_freq);
		}

		const auto *freq_desc = WsHandler::get_freq_desc(nullptr, Settings::get_freq_region());
		if(freq_desc != nullptr)
		{
			_view_sniffer._view_settings._dd_ws_freq.set_selected_text(freq_desc->title);
			bool freq_clickable = (WsHandler::get_cur_station() != nullptr) ? WsHandler::get_cur_station()->CONFIG.user_select_freq : false;
			_view_sniffer._view_settings._dd_ws_freq.clickable(freq_clickable);
		}

		if(WsHandler::get_pair_state() == WsHandler::PairState::PAIRED)
		{
			if(WsHandler::get_cur_station() != NULL && WsHandler::get_cur_station()->CONFIG.extended_pair_info)
			{
				uint8_t ws_sn[10];
				WsHandler::get_paired_sn(ws_sn, sizeof(ws_sn));
				_view_sniffer.set_ws_pair_info(true, ws_sn, sizeof(ws_sn));
			}
			else
			{
				_view_sniffer.set_ws_pair_info(true, WsHandler::get_paired_id());
			}
		}
		else
		{
			_view_sniffer.set_ws_pair_info(false, 0);
		}
	}

	_view_alert_ws_low_bat.create(_view_sniffer.get_popups_layer())
		.type(UiViewAlert::View::AlertType::WARNING)
		.icon(&UiResources::icon_alert_ws_low_bat)
		.title("Station Battery\nLevel is Low")
		.msg("Please replace the AA batteries in your\nstation to ensure proper operation.")
		.show_every_s(ALERT_WS_LOW_BAT_SHOW_EVERY); // Once per day. Show on low bat. Close only by user.

	_view_alert_net_con.create(_view_sniffer.get_popups_layer())
		.type(UiViewAlert::View::AlertType::ERROR)
		.icon(&UiResources::icon_alert_net_con)
		.title("No Internet\nConnection")
		.msg("Check your WiFi connection.\nThis will affect your rewards.")
		.show_every_s(ALERT_NET_CON_SHOW_EVERY); // Once per 5 minutes. Show on disconnected for 5min. Close when connected.

	_view_alert_ws_inactive.create(_view_sniffer.get_popups_layer())
		.type(UiViewAlert::View::AlertType::ERROR)
		.icon(&UiResources::icon_alert_ws_error)
		.title("No Weather\nStation Signal")
		.msg("No stations found. Will continue scanning.\nCheck your station is powered and within range.")
		.show_every_s(ALERT_WS_INACTIVE_SHOW_EVERY); // Once per hour. Show on inactive for 5min. Close when active.

	_view_alert_ws_abnormal_values.create(_view_sniffer.get_popups_layer())
		.type(UiViewAlert::View::AlertType::WARNING)
		.icon(&UiResources::icon_alert_ws_warning)
		.title("Station Reported\nAbnormal Values")
		.msg("Check the sensors on your station.\nThis may affect your rewards.")
		.show_every_s(ALERT_WS_BAD_VALUES_SHOW_EVERY); // Once per day. Show on active_bad_data status. Close on different status.

	// TODO: Alert for no sync for long time
	//  _view_alert_time_sync.create(_view_sniffer.get_popups_layer())
	// 	.type(UiViewAlert::View::AlertType::WARNING)
	// 	.icon(&UiResources::icon_alert_ws_warning)
	// 	.title("Station Reported\nAbnormal State")
	// 	.msg("Check your station is working well.\nThis issue may affect your rewards.")
	// 	.show_every_s(ALERT_WS_BAD_STATE_SHOW_EVERY); // Once per day. Show on active_bad_state for 5min. Close only by user.

	_view_alert_ws_low_reception.create(_view_sniffer.get_popups_layer())
		.type(UiViewAlert::View::AlertType::WARNING)
		.icon(&UiResources::icon_alert_ws_signal)
		.title("Station Signal\nReception is Low")
		.msg("Station power is already at maximum.\nCheck its distance and obstructions.")
		.show_every_s(ALERT_WS_RECEPTION_SHOW_EVERY); // Once per hour, only after adaptive power is exhausted.

	_view_alert_sd_op.create(_view_sniffer.get_popups_layer())
		.type(UiViewAlert::View::AlertType::INFO)
		.icon(&UiResources::icon_alert_ws_warning)
		.title("SD Card Operation")
		.msg("Please wait...")
		.show_every_s(0); // Show on demand

	if(!_ws_content_presentable && WsHandler::get_pair_state() == WsHandler::PairState::PAIRED)
	{
		try_hydrate_ws_pack_from_buffer(WsHandler::get_paired_id());
	}
	update_ws_content();
	UiCommon::unlock();
	
}

/******************************************************************************
 * Update GPS data on screen
 ******************************************************************************/
void update_gps_content()
{
	UiCommon::lock();
	WsGps::GpsData gps_data = {};
	WsGps::get_last_gps_data(&gps_data);
	
	if(!WsGps::gps_data_expired(&gps_data))
	{
		uint32_t gps_ts = 0;
		float lat = 0.0f;
		float lon = 0.0f;
		bool fields_ok =
			WsFields::get_field_val_from_pack(&gps_data.field_pack, WsFields::FieldType::GPS_TS, NULL, &gps_ts) == WsCommon::RetRes::OK &&
			WsFields::get_field_val_from_pack(&gps_data.field_pack, WsFields::FieldType::GPS_LAT, NULL, &lat) == WsCommon::RetRes::OK &&
			WsFields::get_field_val_from_pack(&gps_data.field_pack, WsFields::FieldType::GPS_LON, NULL, &lon) == WsCommon::RetRes::OK;

		if(!fields_ok)
		{
			_view_sniffer._view_statusbar.update_icon_status_gps(UiTypes::StatusIconComponent::StatusType::NEG);
			_view_sniffer.empty_gps_info();
			UiCommon::unlock();
			return;
		}

		time_t local_gps_ts = gps_ts + Settings::get_tz_offset() * 3600;
		char time_fmt_str[30];
		strftime(time_fmt_str, sizeof(time_fmt_str), "%F %H:%M", gmtime(&local_gps_ts));

		_view_sniffer._view_statusbar.update_icon_status_gps(UiTypes::StatusIconComponent::StatusType::POS);
		_view_sniffer.set_gps_info(lat, lon, time_fmt_str);
	}
	else
	{
		_view_sniffer._view_statusbar.update_icon_status_gps(UiTypes::StatusIconComponent::StatusType::NEG);
		_view_sniffer.empty_gps_info();
	}
	UiCommon::unlock();
}

/******************************************************************************
 * Update datetime on screen
 ******************************************************************************/
void update_datetime_content()
{
	UiCommon::lock();
	_view_sniffer._view_settings._dd_timezone.set_selected(Settings::get_tz_offset() + 12);
	if(DatetimeRtc::cur_time_valid())
	{
		static char buff_time[20] = "";

		Utils::get_formatted_datetime(buff_time, sizeof(buff_time), time(NULL), "%d %b %H:%M", true);
		_view_sniffer._view_statusbar.update_datetime(buff_time);
	}
	else
	{
		_view_sniffer._view_statusbar.update_datetime("");
	}
	UiCommon::unlock();
}

static bool field_pack_has_weather(WsFields::FieldPack* pack)
{
	if(pack == NULL || pack->field_count == 0)
	{
		return false;
	}
	float f_val = 0.0f;
	uint16_t u16_val = 0;
	uint8_t u8_val = 0;
	return (WsFields::get_field_val_from_pack(pack, WsFields::FieldType::WEATHER_TEMP, NULL, &f_val) == WsCommon::RetRes::OK) ||
	       (WsFields::get_field_val_from_pack(pack, WsFields::FieldType::WEATHER_HUM, NULL, &f_val) == WsCommon::RetRes::OK) ||
	       (WsFields::get_field_val_from_pack(pack, WsFields::FieldType::WEATHER_WIND_SPEED, NULL, &f_val) == WsCommon::RetRes::OK) ||
	       (WsFields::get_field_val_from_pack(pack, WsFields::FieldType::WEATHER_WIND_DIR, NULL, &u16_val) == WsCommon::RetRes::OK) ||
	       (WsFields::get_field_val_from_pack(pack, WsFields::FieldType::WEATHER_SOLAR_RAD, NULL, &f_val) == WsCommon::RetRes::OK) ||
	       (WsFields::get_field_val_from_pack(pack, WsFields::FieldType::WEATHER_UV_INDEX, NULL, &u8_val) == WsCommon::RetRes::OK) ||
	       (WsFields::get_field_val_from_pack(pack, WsFields::FieldType::WEATHER_PRECIP_RATE, NULL, &f_val) == WsCommon::RetRes::OK) ||
	       (WsFields::get_field_val_from_pack(pack, WsFields::FieldType::WEATHER_PRESSURE, NULL, &f_val) == WsCommon::RetRes::OK);
}

static bool try_hydrate_ws_pack_from_buffer(uint32_t target_station_id)
{
	auto raw_buf = WeatherLogger::get_raw_buffer();
	for(auto it = raw_buf.rbegin(); it != raw_buf.rend(); ++it)
	{
		WsFields::FieldPack candidate = *it;
		if(!field_pack_has_weather(&candidate))
		{
			continue;
		}
		if(target_station_id != 0)
		{
			uint32_t pack_id = 0;
			if(WsFields::get_field_val_from_pack(&candidate, WsFields::FieldType::META_ID, NULL, &pack_id) == WsCommon::RetRes::OK &&
			   pack_id != 0 && pack_id != target_station_id)
			{
				continue;
			}
		}
		memcpy(&_last_valid_ws_pack, &candidate, sizeof(WsFields::FieldPack));
		_ws_content_presentable = true;
		_last_valid_ws_data_ticks_ts_sec = Utils::get_tick_sec();
		return true;
	}
	return false;
}

/******************************************************************************
 * Set FO packet to draw on screen and redraw. Locks UI.
 * @param packet Ptr to packet to display. Will be copied.
 ******************************************************************************/
void set_ws_field_pack(WsFields::FieldPack* field_pack)
{
	if(NULL != field_pack && field_pack_has_weather(field_pack))
	{
		memcpy(&_last_valid_ws_pack, field_pack, sizeof(WsFields::FieldPack));
		_ws_content_presentable = true;
		_last_valid_ws_data_ticks_ts_sec = Utils::get_tick_sec();
	}
}

/******************************************************************************
 * Get latest valid weather field pack if present.
 * @param field_pack Ptr to field pack where the data will be copied.
 * @return True if a valid weather pack is available, false otherwise.
 ******************************************************************************/
bool get_latest_weather_pack(WsFields::FieldPack* field_pack)
{
	if(field_pack == NULL)
	{
		return false;
	}
	if(!_ws_content_presentable)
	{
		if(WsHandler::get_pair_state() != WsHandler::PairState::PAIRED ||
		   !try_hydrate_ws_pack_from_buffer(WsHandler::get_paired_id()))
		{
			return false;
		}
	}
	memcpy(field_pack, &_last_valid_ws_pack, sizeof(WsFields::FieldPack));
	return true;
}


/******************************************************************************
 * Update WiFi oriented data.
 ******************************************************************************/
void update_wifi_content()
{
	UiCommon::lock();
	if(Wifi::get_status() == Wifi::WifiStatus::CONNECTED || Wifi::get_status() == Wifi::WifiStatus::CONNECTED_NO_INTERNET)
	{
		_view_sniffer.set_wifi_info(Wifi::get_ssid() ,Wifi::get_rssi());
	}
	else
	{
		_view_sniffer.empty_wifi_info();
	}	
	UiCommon::unlock();
}

/******************************************************************************
 *
 ******************************************************************************/
void update_debug_content()
{
	UiCommon::lock();
	// _view_sniffer._view_debug.update_ws_reception(FoSniffer::get_ws_packet_reception());
	// _view_sniffer._view_debug.update_ws_status(FoSniffer::get_ws_status_title(FoSniffer::get_ws_status()));
	UiCommon::unlock();
}

/******************************************************************************
 *
 ******************************************************************************/
void update_warning_content()
{
	UiCommon::lock();

	// _view_sniffer._view_settings.update_ws_reception(FoSniffer::get_ws_packet_reception());
	if(Wifi::get_status() != Wifi::WifiStatus::CONNECTED && Wifi::get_sec_since_status_change() > 120) // TODO: Constants
	{
		
		//Show no internet alert
		alert_handler(&_view_alert_net_con, true);
	}
	if(Wifi::get_status() == Wifi::WifiStatus::CONNECTED)
	{
		//Hide no internet alert
		alert_handler(&_view_alert_net_con, false);
	}

	bool ws_data_low_bat = false;
	bool has_low_bat_field = (WsFields::get_field_val_from_pack(&_last_valid_ws_pack, WsFields::FieldType::META_BAT_LOW, NULL, &ws_data_low_bat) == WsCommon::RetRes::OK);
	if(_ws_content_presentable && has_low_bat_field)
	{
		//Show or hide low battery alert based on latest status
		alert_handler(&_view_alert_ws_low_bat, ws_data_low_bat);
	}
	else
	{
		alert_handler(&_view_alert_ws_low_bat, false);
	}

	if(WsHandler::get_activity_status() == WsHandler::ActivityStatus::INACTIVE && WsHandler::get_activity_status_duration_sec() > 300) //5min  // TODO: Constants
	{
		//Show station inactive alert
		alert_handler(&_view_alert_ws_inactive, true);
	}

	UiCommon::unlock();

	const SdLoggerStatus sd_st = WeatherLogger::get_sd_status();
	UiCommon::lock();
	static bool s_prev_sd_ejected = false;
	static bool s_prev_sd_ejected_present = true;
	if (sd_st.ejected && !sd_st.formatting)
	{
		const char *eject_title = sd_st.card_present ? "SD Card Ready to Eject" : "SD Card Ejected";
		const char *eject_msg = sd_st.card_present
			? "Safe to remove microSD card.\nLogging is paused until remounted."
			: "Card removed. Re-insert card or\ntap Mount SD to resume logging.";
		if (!s_prev_sd_ejected || s_prev_sd_ejected_present != sd_st.card_present)
		{
			_view_alert_sd_op
				.type(UiViewAlert::View::AlertType::WARNING)
				.icon(&UiResources::icon_alert_ws_warning)
				.title(eject_title)
				.msg(eject_msg)
				.show_every_s(60)
				.action_btn("Mount SD", []() {
					WeatherLogger::request_sd_probe();
				});
		}
		if (!s_prev_sd_ejected)
		{
			_view_alert_sd_op.set_last_shown_ts(0);
			_view_alert_sd_op.show();
			Backlight::set_backlight_level(Settings::get_idle_backlight_level(), BACKLIGHT_WAKE_SETTING_CHANGE);
		}
		else if (!_view_alert_sd_op.is_shown() && _view_alert_sd_op.is_time_to_show())
		{
			_view_alert_sd_op.show();
			Backlight::set_backlight_level(Settings::get_idle_backlight_level(), BACKLIGHT_WAKE_SETTING_CHANGE);
		}
		s_prev_sd_ejected = true;
		s_prev_sd_ejected_present = sd_st.card_present;
	}
	else if (s_prev_sd_ejected && !sd_st.formatting)
	{
		_view_alert_sd_op.action_btn(nullptr, nullptr);
		_view_alert_sd_op.show_every_s(0);
		_view_alert_sd_op.hide();
		s_prev_sd_ejected = false;
	}
	if (false)
	{
		alert_handler(&_view_alert_ws_inactive, true);
	}	
	UiCommon::unlock();
}

/******************************************************************************
 * UI update task
 * Runs every Xms and takes care of update content that is not event based
 ******************************************************************************/
void ui_update_task(void *data)
{
	while(1)
	{
		if(!_initialized)
		{
			setup_ui();
			_initialized = true;
		}

		update_gps_content();
		update_datetime_content();
		update_wifi_content();
		update_warning_content();

		// #if WXM_ENV_DEBUG == 1
		// update_debug_content();
		// #endif
		

		vTaskDelay(pdMS_TO_TICKS(1000));
	}
}

} // namespace StateSniffer

extern "C" {

void bsp_show_sd_format_alert(const char* title, const char* msg, bool is_error)
{
	lv_port_sem_take();
	StateSniffer::_view_alert_sd_op.action_btn(nullptr, nullptr);
	StateSniffer::_view_alert_sd_op.show_every_s(0);
	StateSniffer::_view_alert_sd_op.type(is_error ? UiViewAlert::View::AlertType::ERROR : UiViewAlert::View::AlertType::INFO);
	StateSniffer::_view_alert_sd_op.title(title);
	StateSniffer::_view_alert_sd_op.msg(msg);
	StateSniffer::_view_alert_sd_op.show();
	lv_port_sem_give();
}

void bsp_hide_sd_format_alert(void)
{
	lv_port_sem_take();
	StateSniffer::_view_alert_sd_op.hide();
	lv_port_sem_give();
}

}

#else // CONFIG_SCREENLESS

#include "state_sniffer.h"

namespace StateSniffer
{
	void init() {}
	void on_enter() {}
	void leave() {}
	bool get_latest_weather_pack(WsFields::FieldPack* field_pack)
	{
		(void)field_pack;
		return false;
	}
}

extern "C" {
	void bsp_show_sd_format_alert(const char* title, const char* msg, bool is_error) {}
	void bsp_hide_sd_format_alert(void) {}
}

#endif // CONFIG_SCREENLESS
