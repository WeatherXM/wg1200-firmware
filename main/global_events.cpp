#include "global_events.h"
#include "string.h"
#include "esp_log.h"

namespace GlobalEvents
{

ESP_EVENT_DEFINE_BASE(APP_EVENT);

/** Logger tag */
static const char TAG[] = "GlobalEvents";

/******************************************************************************
 * @brief Handler notification tag in order to distinguish between other notifications
 * and reassure that the task got notified from our handler.
 * Value = CRC32 of "HANDLER_NOTIFICATION_TAG"
 ******************************************************************************/
static const uint32_t HANDLER_NOTIFICATION_TAG = 0xD8DCF28F;

struct Waiter
{
    TaskHandle_t task_handle;
    void *data;
    int data_size;
};

esp_event_loop_args_t _app_event_loop_cfg = 
{
	.queue_size = 10,
	.task_name = "app_events_task",	
	.task_priority = 2,
	.task_stack_size = 4 * 1024,
	.task_core_id = 1
};

esp_event_loop_handle_t _app_event_loop_handle;

/******************************************************************************
 * @brief Local event handler to convert events to notifications.
 * @param arg Arguments provided at register.
 * @param base Event base is constantly APP_EVENT.
 * @param id APP event id.
 * @param event_data Data pointer provided by publisher.
 ******************************************************************************/
void notification_handler(void *arg, esp_event_base_t base, int32_t id, void *event_data)
{
    if(NULL == arg)
    {
        ESP_LOGE(TAG, "notification_handler: null arg");
        return;
    }
    memcpy(((Waiter*) arg)->data, event_data,((Waiter*) arg)->data_size);
    if(xTaskNotify( ((Waiter*) arg)->task_handle, HANDLER_NOTIFICATION_TAG, eSetValueWithoutOverwrite) != pdPASS)
    {
        ESP_LOGE(TAG, "notification_handler: notification missed");
    }
}

/******************************************************************************
 * @brief Local event handler to convert events to queue messages.
 * @param arg Arguments provided at register.
 * @param base Event base is constantly APP_EVENT.
 * @param id APP event id.
 * @param event_data Data pointer provided by publisher.
 ******************************************************************************/
void queue_handler(void *arg, esp_event_base_t base, int32_t id, void *event_data)
{
    if(NULL == arg)
    {
        ESP_LOGE(TAG, "queue_handler: null arg");
        return;
    }

    QueueMsg msg =
    {
        .event_id = (GlobalEvents::EventId) id,
        .event_data = {}
    };
    memcpy(&msg.event_data, event_data, GlobalEvents::QUEUE_MSG_DATA_MAX_SIZE_BYTES);

    // Hanging here for CONFIG_INT_WDT_TIMEOUT_MS in the default loop will reset the device. 
    // We prefer to drop the message than reset the device, so we wait less time.
    if(xQueueSend((QueueHandle_t) arg, &msg, pdMS_TO_TICKS(CONFIG_INT_WDT_TIMEOUT_MS/2)) != pdTRUE)
    {
        ESP_LOGE(TAG, "queue_handler: queue is full");
    }
}

/******************************************************************************
 * @brief Global events initialization.
 ******************************************************************************/
void init()
{
	ESP_ERROR_CHECK(esp_event_loop_create(&_app_event_loop_cfg, &_app_event_loop_handle));
}

/******************************************************************************
 * @brief Subscribe to APP events providing an event handler.
 * @param event_id APP event id.
 * @param handler Event handler pointer.
 * @param handler_instance Out - instance of the created handler used to unsubscribe. NULL if not interested to unsubscribe.
 * @retval ESP_OK: Success.
 * @retval ESP_ERR_NO_MEM: Cannot allocate memory for the handler.
 * @retval ESP_ERR_INVALID_ARG: Invalid parameters.
 * @retval Others: Fail.
 ******************************************************************************/
RetResult subscribe(EventId event_id, esp_event_handler_t handler, esp_event_handler_instance_t *handler_instance)
{
    if(NULL == handler)
    {
        return RetResult::RET_INVALID_PARAMS;
    }

	esp_err_t ret = esp_event_handler_instance_register_with(_app_event_loop_handle, APP_EVENT, (int32_t) event_id, handler, NULL, handler_instance);

    return ret == ESP_OK ? RET_OK : RET_ERROR;
}

/******************************************************************************
 * @brief Subscribe to APP events providing an event handler.
 * @param event_id APP event id.
 * @param handler Event handler pointer.
 * @param handler_instance Out - instance of the created handler used to unsubscribe. NULL if not interested to unsubscribe.
 * @param handler_arg Parameter to pass in handler when called.
 * @retval ESP_OK: Success.
 * @retval ESP_ERR_NO_MEM: Cannot allocate memory for the handler.
 * @retval ESP_ERR_INVALID_ARG: Invalid parameters.
 * @retval Others: Fail.
 ******************************************************************************/
RetResult subscribe(EventId event_id, esp_event_handler_t handler, esp_event_handler_instance_t *handler_instance, void *handler_arg)
{
    if(NULL == handler)
    {
        return RetResult::RET_INVALID_PARAMS;
    }

	esp_err_t ret = esp_event_handler_instance_register_with(_app_event_loop_handle, APP_EVENT, (int32_t) event_id, handler, handler_arg, handler_instance);

    return ret == ESP_OK ? RET_OK : RET_ERROR;
}

/******************************************************************************
 * @brief Subscribe to APP events providing a message queue.
 * @param event_id APP event id.
 * @param queue Message queue pointer.
 * @param handler_instance Out - instance of the created handler used to unsubscribe. NULL if not interested to unsubscribe.
 * @retval ESP_OK: Success.
 * @retval ESP_ERR_NO_MEM: Cannot allocate memory for the handler.
 * @retval ESP_ERR_INVALID_ARG: Invalid parameters.
 * @retval Other: Fail.
 ******************************************************************************/
RetResult subscribe_queue(EventId event_id, QueueHandle_t queue, esp_event_handler_instance_t *handler_instance)
{
    if(NULL == queue)
    {
        return RetResult::RET_INVALID_PARAMS;
    }

	esp_err_t ret = esp_event_handler_instance_register_with(_app_event_loop_handle, APP_EVENT, (int32_t) event_id, &queue_handler, queue, handler_instance);

    return ret == ESP_OK ? RET_OK : RET_ERROR;
}

/******************************************************************************
 * @brief Unsubscribe from APP events providing the handler instance.
 * @param event_id APP event id.
 * @param handler_instance Provided handler instance to unsubscribe.
 * @retval ESP_OK: Success.
 * @retval ESP_ERR_INVALID_ARG: Invalid combination of event base and event id.
 * @retval Other: Fail.
 ******************************************************************************/
RetResult unsubscribe(EventId event_id, esp_event_handler_instance_t *handler_instance)
{
    if(NULL == handler_instance)
    {
        return RetResult::RET_INVALID_PARAMS;
    }

	esp_err_t ret = esp_event_handler_instance_unregister_with(_app_event_loop_handle, APP_EVENT, (int32_t) event_id, *handler_instance);

	return ret == ESP_OK ? RET_OK : RET_ERROR;
}

/******************************************************************************
 * @brief Wait to be notified by an APP event. Waiter will be notified once!
 * @param event_id APP event id.
 * @param event_data Buffer to write the published data when arrived.
 * @param event_data_size Size of buffer to write the published data when arrived.
 * @param xTicksToWait Ticks to wait for.
 * @retval ESP_OK: Success.
 * @retval ESP_ERR_TIMEOUT: Time to wait for notification expired.
 * @retval ESP_ERR_INVALID_RESPONSE: Wrong notification received.
 ******************************************************************************/
RetResult wait(EventId event_id, void *event_data, int event_data_size, TickType_t xTicksToWait)
{
    if(NULL == event_data)
    {
        return RetResult::RET_INVALID_PARAMS;
    }

    Waiter waiter =
    {
        .task_handle = xTaskGetCurrentTaskHandle(),
        .data = event_data,
        .data_size = event_data_size
    };

    esp_event_handler_instance_t notification_handler_instance;
    esp_err_t ret = esp_event_handler_instance_register_with(_app_event_loop_handle, APP_EVENT, (int32_t) event_id, &notification_handler, &waiter, &notification_handler_instance);
    if(ESP_OK != ret)
    {
        return RET_ERROR;
    }
    uint32_t ntf_val;
    BaseType_t ret_ntf = xTaskNotifyWait(ULONG_MAX, ULONG_MAX, &ntf_val, xTicksToWait);
    esp_event_handler_instance_unregister_with(_app_event_loop_handle, APP_EVENT, (int32_t) event_id, notification_handler_instance);

    if(pdFAIL == ret_ntf)
    {
        return RetResult::RET_TIMEOUT;
    }
    if(HANDLER_NOTIFICATION_TAG != ntf_val)
    {
        return RetResult::RET_ERROR;
    }

    return RET_OK;
}

/******************************************************************************
 * @brief Publish an APP event.
 * @param event_id APP event id.
 * @param event_data APP event data.
 * @param event_data_size APP event data size.
 * @retval ESP_OK: Success.
 * @retval ESP_ERR_TIMEOUT: Time to wait for event queue to unblock expired, queue full when posting from ISR.
 * @retval ESP_ERR_INVALID_ARG: Invalid parameters.
 * @retval Other: Fail.
 ******************************************************************************/
RetResult publish(EventId event_id, void *event_data, size_t event_data_size)
{
    if(NULL == event_data)
    {
        return RET_INVALID_PARAMS;
    }

	esp_err_t ret = esp_event_post_to(_app_event_loop_handle, APP_EVENT, (int32_t) event_id, event_data, event_data_size, pdMS_TO_TICKS(PUBLISH_TIMEOUT_MS));

    return ret == ESP_OK ? RET_OK : RET_ERROR;
}

/******************************************************************************
 * @brief Publish an APP event from ISR context.
 * @param event_id APP event id.
 * @param event_data APP event data.
 * @param event_data_size APP event data size.
 * @retval ESP_OK: Success.
 * @retval ESP_ERR_TIMEOUT: Time to wait for event queue to unblock expired, queue full when posting from ISR.
 * @retval ESP_ERR_INVALID_ARG: Invalid parameters.
 * @retval Other: Fail.
 ******************************************************************************/
RetResult publish_from_isr(EventId event_id, void *event_data, size_t event_data_size, BaseType_t *task_unblocked)
{
    if(NULL == event_data)
    {
        return RET_INVALID_PARAMS;
    }

	esp_err_t ret = esp_event_isr_post_to(_app_event_loop_handle, APP_EVENT, (int32_t) event_id, event_data, event_data_size, task_unblocked);

    return ret == ESP_OK ? RET_OK : RET_ERROR;
}

}
