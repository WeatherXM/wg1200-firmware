#ifndef DEVICE_STATE_H
#define DEVICE_STATE_H

#include "struct.h"
#include "common.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

namespace GlobalState
{
	/**
	 * States
	 */
	enum State
	{
		STATE_DEFAULT,
		STATE_SNIFFER,
		STATE_FO_SCAN,
	};

	/**
	 * Global event flags
	 */
	enum EventBits
	{
		EV_WIFI_CONNECTED_BIT = BIT0,
		EV_WIFI_DISCONNECTED_BIT = BIT1,
		EV_FO_SNIFFER_CAPTURE_STARTED_BIT = BIT2,
		EV_FO_SNIFFER_CAPTURE_STOPPED_BIT = BIT3,
		EV_GPS_ENABLED_BIT = BIT4,
		EV_GPS_DISABLED_BIT = BIT5,
		EV_TB_CONNECTED_BIT = BIT6,
		EV_TB_DISCONNECTED_BIT = BIT7,
		EV_SUBMIT_AUTO_ENABLED_BIT = BIT8,
		EV_SUBMIT_AUTO_DISABLED_BIT = BIT9,
		EV_TIME_SYNCED = BIT10
	};

	RetResult init();
	void switch_to(State state);

	EventGroupHandle_t get_event_group();
}

#endif
