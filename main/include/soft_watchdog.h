#ifndef SOFT_WATCHDOG_H
#define SOFT_WATCHDOG_H

#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"

namespace SoftWatchdog
{

struct Handle
{
	TimerHandle_t timer_handle = NULL;
	char task_name[CONFIG_FREERTOS_MAX_TASK_NAME_LEN];
};

void init(Handle* swd_handle, uint32_t timeout_ms);
void start(Handle* swd_handle);
void feed(Handle* swd_handle);
void stop(Handle* swd_handle);
void deinit(Handle* swd_handle);

}

#endif