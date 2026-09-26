#include "../main/ui/include/ui_common.h"
#include "lv_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace UiCommon
{

void lock()
{
    lv_port_sem_take();
}

void unlock()
{
    lv_port_sem_give();
}

uint32_t get_millis_count()
{
	return pdTICKS_TO_MS(xTaskGetTickCount());
}

uint32_t get_seconds_count()
{
	return get_millis_count() / 1000;
}

}