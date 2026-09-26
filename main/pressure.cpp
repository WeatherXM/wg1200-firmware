#include "pressure.h"
#include "freertos/FreeRTOS.h"
#ifndef CONFIG_SCREENLESS
#include "bmp3xx.h"
#endif
#include "const.h"

#ifdef CONFIG_SCREENLESS
#include "ws/include/kiss_client.h"
#endif

namespace Pressure
{

/** Task stack */
EXT_RAM_BSS_ATTR StackType_t _update_task_stack[PRESSURE_UPDATE_TASK_STACK_SIZE] = {};

/** Task buffer */
StaticTask_t _update_task_buff;

/** Pressure update task handle */
static TaskHandle_t _update_task_handle = NULL;

float _last_pressure_val = -1.0f;

static SemaphoreHandle_t _last_pressure_val_mutex = NULL;

void pressure_update_task(void *data)
{
	while(1)
	{
		float pressure = -1.0f;
#ifdef CONFIG_SCREENLESS
		// Query pressure from the NRF52 via UART KISS
		pressure = KissClient::get_baro_pressure();
#else
		float temperature;
		if (bmp3xx_read_data(&pressure, &temperature) == ESP_OK)
		{
			// bmp3xx_read_data return Pa, we want hPa
			pressure /= 100.0f;
		}
		else
		{
			pressure = -1.0f;
		}
#endif

		xSemaphoreTake(_last_pressure_val_mutex, portMAX_DELAY);
		_last_pressure_val = pressure;
		xSemaphoreGive(_last_pressure_val_mutex);

		vTaskDelay(pdMS_TO_TICKS(PRESSURE_UPDATE_INT_SEC * 1000));
	}
}

void init()
{

	_last_pressure_val_mutex = xSemaphoreCreateMutex();
	// Create the task using static creation API
	_update_task_handle = xTaskCreateStaticPinnedToCore(pressure_update_task, PRESSURE_UPDATE_TASK_NAME, sizeof(_update_task_stack), NULL,
												PRESSURE_UPDATE_TASK_PRIORITY, _update_task_stack, &_update_task_buff, 1);
}

float get_latest_val()
{
	float ret = -1.0f;;
	xSemaphoreTake(_last_pressure_val_mutex, portMAX_DELAY);
	ret = _last_pressure_val;
	xSemaphoreGive(_last_pressure_val_mutex);
	return ret;
}

}