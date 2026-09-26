#ifndef WIFI_H
#define WIFI_H

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_event_base.h"

#include "struct.h"
#include "app_config.h"
#include "common.h"

namespace Wifi
{

///////////////////////////////////////////////////////////////////////////////
// Types
///////////////////////////////////////////////////////////////////////////////

/** Manager task orders/events **/
enum ManagerNotification
{
	ORDER_CONNECT_STA = 1,
	ORDER_DISCONNECT_STA,
	ORDER_DNS_RESOLVE,
	ORDER_RECOVER_STA,
	ORDER_RESTART_STA,
	ORDER_REBOOT_STA_RECOVERY,
	EVENT_STA_DISCONNECTED,
	EVENT_STA_GOT_IP,
	EVENT_WAITING_IP_TIMEOUT,
	EVENT_STA_LOST_IP
};

/** WiFI status **/
enum class WifiStatus 
{
	NONE = 0,
	CONNECTING, 			// transition status
	CONNECTED_NO_IP, 		// fixed status
	CONNECTED_NO_INTERNET, 	// fixed status
	CONNECTED,				// fixed status
	CONNECTION_FAILED,		// transition status
	DISCONNECTING,			// transition status
	DISCONNECTED			// fixed status
};

///////////////////////////////////////////////////////////////////////////////
// Functions
///////////////////////////////////////////////////////////////////////////////
RetResult init();
RetResult deinit();
RetResult connect(char *ssid = NULL, char *pass = NULL, bool autoreconnect = false);
RetResult disconnect();
Wifi::WifiStatus get_status();
uint32_t get_sec_since_status_change();
RetResult wait_connected(u32 timeout = portMAX_DELAY);
RetResult wait_disconnected(u32 timeout = portMAX_DELAY);
bool is_connected();
int get_rssi();
char *get_ssid();
char *get_ip_str();
bool has_ip();

}

#endif
