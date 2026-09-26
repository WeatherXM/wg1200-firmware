#include "soft_watchdog.h"
#include "boot_guard.h"
#include "esp_log.h"
#include "string.h"
#include "utils.h"

namespace SoftWatchdog
{

/** Logger tag */
static const char TAG[] = "SoftWatchdog";

/******************************************************************************
* @brief Timer callback that performs a system restart immediately
* @param xTimer Timer that invoked the callback (unused)
******************************************************************************/
void restart_cb(TimerHandle_t xTimer)
{
	Utils::safe_restart(Utils::RestartReason::SOFT_WATCHDOG);
}

/******************************************************************************
* @brief Watchdog timeout handler.
* Logs the task that armed the watchdog and schedules a short one-shot timer
* to restart the system after ~100 ms to allow logs to flush.
* Runs in the FreeRTOS timer service task; do not block here.
* @param xTimer Handle of the watchdog timer that expired
******************************************************************************/
void timeout_cb(TimerHandle_t xTimer)
{
	Handle *swd = (Handle*) pvTimerGetTimerID(xTimer);
	const char *task_name = swd ? swd->task_name : NULL;

	ESP_LOGE(TAG, "Reset triggered by task \"%s\"", task_name ? task_name : "Unknown");

	// This restart shows up as a plain SW reset; flag it so the boot guard
	// counts it as a crash boot
	BootGuard::note_soft_wd_reset();

	TimerHandle_t timer_handle = xTimerCreate("rst", pdMS_TO_TICKS(100), pdFALSE, NULL, restart_cb);

	if(!timer_handle)
	{
		ESP_LOGE(TAG, "timeout_cb: timer creation failed");
		return;
	}

	if(xTimerStart(timer_handle, (TickType_t)0) != pdPASS)
	{
		ESP_LOGE(TAG, "timeout_cb: timer start failed");
		return;
	}
}

/******************************************************************************
* @brief Initialize a SoftWatchdog instance and create its one-shot timer.
* @param swd_handle	Pointer to persistent Handle storage (must outlive timer)
* @param timeout_ms	Watchdog timeout in milliseconds
******************************************************************************/
void init(Handle* swd_handle, uint32_t timeout_ms)
{
	if(!swd_handle)
	{
		ESP_LOGE(TAG, "init: invalid params");
		return;
	}

	if (swd_handle->timer_handle)
	{
		ESP_LOGE(TAG, "init: handle already used");
		return;
	}

	swd_handle->timer_handle = xTimerCreate(NULL, pdMS_TO_TICKS(timeout_ms), pdFALSE, swd_handle, timeout_cb);
	if(!swd_handle->timer_handle)
	{
		ESP_LOGE(TAG, "init: timer creation failed");
		return;
	}
}

/******************************************************************************
* @brief Start (arm) the watchdog timer and record the current task's name.
* @param swd_handle	Pointer to an initialized Handle
******************************************************************************/
void start(Handle* swd_handle)
{
	if(!swd_handle || !swd_handle->timer_handle)
	{
		ESP_LOGE(TAG, "start: invalid params");
		return;
	}

	strlcpy(swd_handle->task_name, pcTaskGetName(NULL), sizeof(swd_handle->task_name));

	if(xTimerStart(swd_handle->timer_handle, (TickType_t)0) != pdPASS)
	{
		ESP_LOGE(TAG, "start: timer start failed");
		return;
	}
}

/******************************************************************************
* @brief Feed the watchdog by resetting its one-shot timer.
* @param swd_handle	Pointer to an initialized Handle
******************************************************************************/
void feed(Handle* swd_handle)
{
	if(!swd_handle || !swd_handle->timer_handle)
	{
		ESP_LOGE(TAG, "feed: invalid params");
		return;
	}

	if(xTimerReset(swd_handle->timer_handle, (TickType_t) 0) != pdPASS)
	{
		ESP_LOGE(TAG, "feed: timer reset failed");
		return;
	}
}

/******************************************************************************
* @brief Stop the watchdog timer if it is currently active
* The timer object remains allocated and can be started again later.
* @param swd_handle	Pointer to an initialized Handle
******************************************************************************/
void stop(Handle* swd_handle)
{
	if(!swd_handle || !swd_handle->timer_handle)
	{
		ESP_LOGE(TAG, "stop: invalid params");
		return;
	}

	if(xTimerIsTimerActive(swd_handle->timer_handle) != pdFALSE)
	{
		if(xTimerStop(swd_handle->timer_handle, 0) != pdPASS)
		{
			ESP_LOGE(TAG, "stop: timer stop failed");
			return;
		}
	}
}

/******************************************************************************
* @brief Deinitialize the SoftWatchdog by stopping and deleting its timer
* Clears the stored handle and task name. After this, call init() again
* before reuse.
* @param swd_handle	Pointer to an initialized Handle
******************************************************************************/
void deinit(Handle* swd_handle)
{
	if(!swd_handle || !swd_handle->timer_handle)
	{
		ESP_LOGE(TAG, "deinit: invalid params");
		return;
	}
	stop(swd_handle);
	if(xTimerDelete(swd_handle->timer_handle, 0) != pdPASS)
	{
		ESP_LOGE(TAG, "deinit: timer delete failed");
		return;
	}

	swd_handle->timer_handle = NULL;
	swd_handle->task_name[0] = '\0';
}

}
