#pragma once
#include "esp_err.h"
#include "const.h"

namespace Cellular {
    esp_err_t init();
    bool is_connected();
    bool is_ppp_active();
    esp_err_t start_ppp();
    esp_err_t stop_ppp();
    void power_off();
}
