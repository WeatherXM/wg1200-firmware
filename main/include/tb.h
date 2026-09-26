#ifndef TB_H
#define TB_H

#include "mqtt_client.h"

#include "const.h"
#include "common.h"
#include "struct.h"

namespace Tb
{

///////////////////////////////////////////////////////////////////////////////
// Types
///////////////////////////////////////////////////////////////////////////////


/** ΤΒ status **/
enum class MqttStatus
{
	NONE = 0,
	CONNECTING,
	CONNECTED,
	CONNECTION_FAILED,
	DISCONNECTING,
	DISCONNECTED
};

/** MQTT response **/
struct MqttResponse
{
	char data[TB_MQTT_BUFFER_SIZE];
	int data_len = 0;
	char topic[TB_MQTT_DEFAULT_TOPIC_LEN];
	int msg_id;
};

RetResult init();
RetResult start();
RetResult stop();
Tb::MqttStatus get_status();
RetResult publish_telemetry(char *payload);
void ota_test();
esp_mqtt_client_handle_t get_client();

int publish(const char *topic, char *data, int qos = TB_MQTT_DEFAULT_QOS);
int subscribe(const char *topic, int qos = TB_MQTT_DEFAULT_QOS);
int unsubscribe(const char *topic);

RetResult start_wait_response(const char *topic, u32 timeout = TB_MQTT_DEFAULT_WAIT_RESP_TIMEOUT_MS);
void cancel_wait_response();
RetResult await_response(MqttResponse *out, u32 timeout);
RetResult wait_response(const char *topic, MqttResponse *out, u32 timeout);

RetResult wait_connected(u32 timeout = portMAX_DELAY);
RetResult wait_disconnected(u32 timeout = portMAX_DELAY);
bool is_connected();
}

#endif