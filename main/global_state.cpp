#include "esp_log.h"

#include "global_state.h"
#include "state_sniffer.h"

namespace GlobalState
{
	///////////////////////////////////////////////////////////////////////////////
	// Private functions
	///////////////////////////////////////////////////////////////////////////////
	void call_leave_handler(State state);
	void call_enter_handler(State state);

	///////////////////////////////////////////////////////////////////////////////
	// Local vars
	///////////////////////////////////////////////////////////////////////////////
	/** Logger tag */
	static const char TAG[] = "GlobalState";

	/** Current state */
	State _cur_state = STATE_DEFAULT;

	/** General event group shared by most components. All global event flags are
	 * here. */
	EventGroupHandle_t _evg = 0;

	/** Buffer for above event group */
	StaticEventGroup_t _evg_buffer = {};


	/******************************************************************************
	* Init
	******************************************************************************/
	RetResult init()
	{
		_evg = xEventGroupCreateStatic(&_evg_buffer);
		if(_evg == NULL)
		{
			return RET_ERROR;
		}

		return RET_OK;
	}

	/******************************************************************************
	* Switch to state. Before switching call leave handler of previous state
	******************************************************************************/
	void switch_to(State state)
	{
		if(state == _cur_state)
			return;

		call_leave_handler(_cur_state);

		ESP_LOGI(TAG, "Switching state: %d", state);

		_cur_state = state;

		call_enter_handler(state);
	}

	/******************************************************************************
	* Call enter handler on provided state
	* @param state Target state
	******************************************************************************/
	void call_enter_handler(State state)
	{
		switch(state)
		{
		case STATE_SNIFFER:
			ESP_LOGW(TAG, "Entering state: Sniffer");
			StateSniffer::on_enter();
			break;
		case STATE_FO_SCAN:
			ESP_LOGW(TAG, "Entering state: FO Scan");
			break;
		case STATE_DEFAULT:
		default:
			ESP_LOGE(TAG, "Invalid state: %d", state);
		}
	}

	/******************************************************************************
	* Call leave handler of provided state
	* @param state Target state
	******************************************************************************/
	void call_leave_handler(State state)
	{
		// TODO: Deprecate

		/*
		switch(state)
		{
		case STATE_SNIFFER:
			ESP_LOGW(TAG, "Leaving state: Sniffer");
			break;
		case STATE_FO_SCAN:
			ESP_LOGW(TAG, "Leaving: FO Scan");
			break;
		case STATE_WIFI_CONFIG:
			ESP_LOGW(TAG, "Leaving state: WiFi Config");
			StateWifiConfig::on_leave();
			break;
		case STATE_DEFAULT:
		default:
			ESP_LOGE(TAG, "Invalid state");
		}
		*/
	}

	/******************************************************************************
	* Get main event group
	******************************************************************************/
	EventGroupHandle_t get_event_group()
	{
		return _evg;
	}
}