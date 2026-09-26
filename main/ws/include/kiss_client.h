#pragma once
#include "esp_err.h"
#include <stdint.h>

namespace KissClient {
    extern bool g_nrf52_uart_ok;
    esp_err_t init(int tx_pin, int rx_pin, int cts_pin, int rts_pin);
    esp_err_t set_radio(uint32_t freq_hz, uint32_t bw_hz, uint8_t sf, uint8_t cr);
    esp_err_t set_preamble(uint16_t preamble_len);
    float get_baro_pressure();
    esp_err_t send_data(const uint8_t* payload, uint16_t len);
    int read_packet(uint8_t* rx_buffer, uint16_t max_len, int8_t* out_snr, int8_t* out_rssi);
    void ping();
}

