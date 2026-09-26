#include "esp_log.h"
#include "sys/time.h"
#include "esp_sntp.h"
#include "esp_log.h"
#include "time.h"
#include "esp_timer.h"
#include "global_events.h"
#include "app_config.h"
#include "datetime_rtc.h"
#include "utils.h"
#include "const.h"
#include "wifi.h"
#include "utils.h"
#include "log.h"
#include "esp_netif_sntp.h"

/** Logger tag */
static const char TAG[] = "RTC";

namespace DatetimeRtc
{

	/** Current tick value. Used by timer_x functions */
	uint64_t _timer_cur_tick = 0;

	void reset_last_sync_tick();
	void time_synced_cb(struct timeval *tv);
	bool _is_synced = false;

	/******************************************************************************
	* Init
	******************************************************************************/
	RetResult init()
	{
		// Init SNTP
		// CONFIG_LWIP_SNTP_UPDATE_DELAY to configure NTP sync interval, default 1hour
		esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(NTP_SERVER);
		config.sync_cb = time_synced_cb;
		config.wait_for_sync = true;
		esp_netif_sntp_init(&config);

		return RET_OK;
	}

	void time_synced_cb(struct timeval *tv)
	{
		ESP_LOGI(TAG, "NTP sync success.");
		Log::log(Log::Code::RTC_SYNCED, 0);
		print_system_time();
		GlobalEvents::TimeSyncedEvent time_sync_event;
		if(!_is_synced)
		{
			_is_synced = true;
			time_sync_event = GlobalEvents::TimeSyncedEvent::FIRST_SYNC;
		}
		else
		{
			time_sync_event = GlobalEvents::TimeSyncedEvent::INTERVAL_SYNC;
		}
		GlobalEvents::publish(GlobalEvents::EventId::TIME_SYNCED, &time_sync_event, sizeof(time_sync_event));
	}

	void force_sync()
	{
		// ntp starts anyway, but we can force it on internet connected to save some time
		esp_netif_sntp_start();
	}

	bool is_synced()
	{
		return _is_synced;
	}

	/******************************************************************************
	* Print system time. FOr debugging purposes.
	******************************************************************************/
	void print_system_time()
	{
		struct tm tmstruct = {};
		char systime[50] = "";
		time_t now = time(NULL);

		localtime_r(&now, &tmstruct);

		strftime(systime, sizeof(systime), "%x %X", &tmstruct);

		printf("Sys time: (%lld) %s\n", time(NULL), systime);
	}

	/******************************************************************************
	* Check if timestamp looks somewhat valid (i.e. not too far back in the past)
	******************************************************************************/
	bool tstamp_valid(u32 tstamp)
	{
		return tstamp <= TSTAMP_VALID_CHECK ? false : true;
	}

	/******************************************************************************
	* Check if current timestamp is valid
	******************************************************************************/
	bool cur_time_valid()
	{
		return tstamp_valid(DatetimeRtc::get_tstamp()) ? true : false;
	}

	/******************************************************************************
	* Get current timestamp
	******************************************************************************/
	time_t get_tstamp()
	{
		return time(NULL);
	}

	/******************************************************************************
	* Record the current tick value. For debugging
	******************************************************************************/
	void timer_reset()
	{
		_timer_cur_tick = esp_timer_get_time();
	}

	/******************************************************************************
	* Print time elapsed since last reset. For debugging. 
	******************************************************************************/
	void timer_print_elapsed(const char *source)
	{
		uint64_t diff = esp_timer_get_time() - _timer_cur_tick;

		const char *src = source == nullptr ? "" : source;

		printf("Time elapsed (\033[94m%s\033[0m): %lluuS (%llu mS)\n", src, diff, diff/1000);
	}

	/******************************************************************************
	* Get time elapsed since last reset. For debugging.
	******************************************************************************/
	uint64_t timer_get_elapsed()
	{
		return esp_timer_get_time() - _timer_cur_tick;
	}


	RetResult wait_for_ntp_sync(uint32_t timeout_ms)
	{
		return esp_netif_sntp_sync_wait(pdMS_TO_TICKS(timeout_ms)) == ESP_OK ?  RET_OK : RET_TIMEOUT;
	}

	void set_tstamp_from_gps(time_t tstamp)
	{
		struct timeval tv = { .tv_sec = tstamp, .tv_usec = 0 };
		settimeofday(&tv, NULL);
		_is_synced = true;
		ESP_LOGI(TAG, "System time synced manually from station GPS: %lld", (long long)tstamp);
		Log::log(Log::Code::RTC_SYNCED, 0);
		print_system_time();
		GlobalEvents::TimeSyncedEvent time_sync_event = GlobalEvents::TimeSyncedEvent::FIRST_SYNC;
		GlobalEvents::publish(GlobalEvents::EventId::TIME_SYNCED, &time_sync_event, sizeof(time_sync_event));
}

}