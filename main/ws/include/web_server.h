#pragma once
#include "esp_err.h"

namespace WebServer {
    esp_err_t start();
    void stop();
    void log_packet(const uint8_t* payload, uint16_t len, int16_t rssi, int8_t snr, bool filtered = false);
    void log_tx_packet(const uint8_t* payload, uint16_t len, uint32_t freq_hz, uint8_t sf, uint32_t bw_hz, uint8_t cr, int8_t tx_power_dbm);
    extern uint32_t g_last_web_request_ts;
}
