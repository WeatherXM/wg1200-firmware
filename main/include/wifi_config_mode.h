#ifndef WIFI_CONFIG_MODE_H
#define WIFI_CONFIG_MODE_H

#include "esp_wifi.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "string.h"

#include "const.h"
#include "utils.h"
#include "struct.h"

namespace WifiConfigMode
{
	RetResult init();

	void handle();

	void set_on_next_boot();

	void set_connection_failed_on_next_boot();

	void clear_boot_flags();
}

#endif