#include "ws/include/kiss_client.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

#define TAG "KissClient"

#define KISS_FEND  0xC0
#define KISS_FESC  0xDB
#define KISS_TFEND 0xDC
#define KISS_TFESC 0xDD

#define KISS_CMD_DATA        0x00
#define KISS_CMD_SETHARDWARE 0x06

#define HW_CMD_SET_RADIO         0x09
#define HW_CMD_SET_SIGNAL_REPORT 0x19
#define HW_CMD_GET_SENSORS       0x15
#define HW_CMD_SET_PREAMBLE      0x20
#define HW_CMD_PING              0x17

#define HW_RESP_OK               0xF0
#define HW_RESP_RX_META          0xF9

static uart_port_t g_uart_num = UART_NUM_1;
static bool g_initialized = false;
static SemaphoreHandle_t g_write_mutex = NULL;
static SemaphoreHandle_t g_state_mutex = NULL;

// Internal RX parsing state
static uint8_t g_rx_frame[1024];
static uint16_t g_rx_len = 0;
static bool g_rx_escaped = false;
static bool g_rx_active = false;

// Last received metadata
static int8_t g_last_snr = 0;
static int8_t g_last_rssi = 0;
static bool g_meta_updated = false;

namespace KissClient {

bool g_nrf52_uart_ok = false;

static void write_byte_escaped(uint8_t b) {
    if (b == KISS_FEND) {
        uint8_t esc[2] = { KISS_FESC, KISS_TFEND };
        uart_write_bytes(g_uart_num, (const char*)esc, 2);
    } else if (b == KISS_FESC) {
        uint8_t esc[2] = { KISS_FESC, KISS_TFESC };
        uart_write_bytes(g_uart_num, (const char*)esc, 2);
    } else {
        uart_write_bytes(g_uart_num, (const char*)&b, 1);
    }
}

static void send_hardware_cmd(uint8_t sub_cmd, const uint8_t* data, uint16_t len) {
    if (g_write_mutex && xSemaphoreTake(g_write_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        uint8_t fend = KISS_FEND;
        uint8_t cmd = KISS_CMD_SETHARDWARE;
        
        uart_write_bytes(g_uart_num, (const char*)&fend, 1);
        write_byte_escaped(cmd);
        write_byte_escaped(sub_cmd);
        for (uint16_t i = 0; i < len; i++) {
            write_byte_escaped(data[i]);
        }
        uart_write_bytes(g_uart_num, (const char*)&fend, 1);
        xSemaphoreGive(g_write_mutex);
    } else {
        ESP_LOGE(TAG, "Failed to acquire write mutex for send_hardware_cmd");
    }
}

esp_err_t init(int tx_pin, int rx_pin, int cts_pin, int rts_pin) {
    printf("[KissClient] init called: TX:%d, RX:%d, CTS:%d, RTS:%d\n", tx_pin, rx_pin, cts_pin, rts_pin);
    if (!g_write_mutex) {
        g_write_mutex = xSemaphoreCreateMutex();
    }
    if (!g_state_mutex) {
        g_state_mutex = xSemaphoreCreateMutex();
    }

    if (g_initialized) {
        printf("[KissClient] init: Already initialized\n");
        return ESP_OK;
    }

    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 122,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_driver_install(g_uart_num, 2048, 2048, 0, NULL, 0);
    if (err != ESP_OK) {
        printf("[KissClient] init: Failed to install UART driver: %d\n", err);
        ESP_LOGE(TAG, "Failed to install UART driver: %d", err);
        return err;
    }

    err = uart_param_config(g_uart_num, &uart_config);
    if (err != ESP_OK) {
        printf("[KissClient] init: Failed to configure UART params: %d\n", err);
        ESP_LOGE(TAG, "Failed to configure UART params: %d", err);
        return err;
    }

    err = uart_set_pin(g_uart_num, tx_pin, rx_pin, rts_pin, cts_pin);
    if (err != ESP_OK) {
        printf("[KissClient] init: Failed to set UART pins: %d\n", err);
        ESP_LOGE(TAG, "Failed to set UART pins: %d", err);
        return err;
    }

    g_initialized = true;
    printf("[KissClient] init: UART KISS Client initialized successfully.\n");
    ESP_LOGI(TAG, "UART KISS Client initialized. TX:%d, RX:%d, CTS:%d, RTS:%d", tx_pin, rx_pin, cts_pin, rts_pin);

    // Enable signal reports on the modem
    uint8_t enable = 1;
    send_hardware_cmd(HW_CMD_SET_SIGNAL_REPORT, &enable, 1);

    return ESP_OK;
}

esp_err_t set_radio(uint32_t freq_hz, uint32_t bw_hz, uint8_t sf, uint8_t cr) {
    if (!g_initialized) return ESP_ERR_INVALID_STATE;

    uint8_t buf[10];
    memcpy(buf, &freq_hz, 4);
    memcpy(buf + 4, &bw_hz, 4);
    buf[8] = sf;
    buf[9] = cr;

    ESP_LOGI(TAG, "Setting radio: Freq=%d, BW=%d, SF=%d, CR=%d", freq_hz, bw_hz, sf, cr);
    send_hardware_cmd(HW_CMD_SET_RADIO, buf, 10);
    return ESP_OK;
}

esp_err_t set_preamble(uint16_t preamble_len) {
    if (!g_initialized) return ESP_ERR_INVALID_STATE;

    ESP_LOGI(TAG, "Setting custom preamble: %d", preamble_len);
    send_hardware_cmd(HW_CMD_SET_PREAMBLE, (const uint8_t*)&preamble_len, 2);
    return ESP_OK;
}

void ping() {
    printf("[KissClient] ping() called. g_initialized=%d\n", g_initialized);
    if (!g_initialized) return;
    printf("[KissClient] Sending HW_CMD_PING (0x17) to nRF52 over UART...\n");
    ESP_LOGI(TAG, "Sending HW_CMD_PING (0x17) to nRF52 over UART...");
    send_hardware_cmd(HW_CMD_PING, NULL, 0);
}

static float g_last_pressure = -1.0f;
static bool g_pressure_updated = false;

float get_baro_pressure() {
    // No pressure sensor is physically connected.
    // Return -1.0f immediately to avoid serial queries and 500ms blocking timeouts.
    return -1.0f;
}

esp_err_t send_data(const uint8_t* payload, uint16_t len) {
    if (!g_initialized) return ESP_ERR_INVALID_STATE;

    if (g_write_mutex && xSemaphoreTake(g_write_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        uint8_t fend = KISS_FEND;
        uint8_t cmd = KISS_CMD_DATA;

        uart_write_bytes(g_uart_num, (const char*)&fend, 1);
        write_byte_escaped(cmd);
        for (uint16_t i = 0; i < len; i++) {
            write_byte_escaped(payload[i]);
        }
        uart_write_bytes(g_uart_num, (const char*)&fend, 1);
        xSemaphoreGive(g_write_mutex);
        return ESP_OK;
    } else {
        ESP_LOGE(TAG, "Failed to acquire write mutex for send_data");
        return ESP_FAIL;
    }
}

int read_packet(uint8_t* rx_buffer, uint16_t max_len, int8_t* out_snr, int8_t* out_rssi) {
    if (!g_initialized) return 0;

    uint8_t b;
    while (uart_read_bytes(g_uart_num, &b, 1, 0) > 0) {
        printf("[KissClient] Received UART byte: 0x%02X\n", b);
        if (b == KISS_FEND) {
            if (g_rx_active && g_rx_len > 0) {
                // Completed frame
                if (g_rx_frame[0] == KISS_CMD_DATA) {
                    // Raw received packet payload
                    uint16_t packet_len = g_rx_len - 1;
                    if (packet_len > max_len) packet_len = max_len;
                    memcpy(rx_buffer, g_rx_frame + 1, packet_len);

                    ESP_LOGI(TAG, "Successfully received KISS packet data frame! Len: %d", packet_len);
                    ESP_LOG_BUFFER_HEX(TAG, rx_buffer, packet_len);

                    if (out_snr && out_rssi) {
                        if (g_state_mutex && xSemaphoreTake(g_state_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                            if (g_meta_updated) {
                                *out_snr = g_last_snr;
                                *out_rssi = g_last_rssi;
                                g_meta_updated = false;
                            } else {
                                *out_snr = 0;
                                *out_rssi = 0;
                            }
                            xSemaphoreGive(g_state_mutex);
                        } else {
                            *out_snr = 0;
                            *out_rssi = 0;
                        }
                    }

                    g_rx_len = 0;
                    g_rx_escaped = false;
                    g_rx_active = false;
                    return packet_len;
                } else if (g_rx_frame[0] == KISS_CMD_SETHARDWARE && g_rx_frame[1] == (HW_CMD_PING | 0x80)) {
                    g_nrf52_uart_ok = true;
                    ESP_LOGI(TAG, "Received PING response (0x97) from nRF52! UART communication is successful.");
                } else if (g_rx_frame[0] == KISS_CMD_SETHARDWARE && g_rx_frame[1] == HW_RESP_RX_META) {
                    // Signal report metadata frame
                    if (g_rx_len >= 4) {
                        if (g_state_mutex && xSemaphoreTake(g_state_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                            g_last_snr = (int8_t)g_rx_frame[2];
                            g_last_rssi = (int8_t)g_rx_frame[3];
                            g_meta_updated = true;
                            xSemaphoreGive(g_state_mutex);
                        }
                        ESP_LOGD(TAG, "Received signal report: SNR=%d, RSSI=%d", (int8_t)g_rx_frame[2], (int8_t)g_rx_frame[3]);
                    }
                } else if (g_rx_frame[0] == KISS_CMD_SETHARDWARE && g_rx_frame[1] == (HW_CMD_GET_SENSORS | 0x80)) {
                    // Sensors response frame containing CayenneLPP telemetry data
                    if (g_rx_len >= 4) {
                        uint16_t telemetry_len = g_rx_len - 2;
                        uint8_t* telemetry_data = g_rx_frame + 2;
                        
                        // Parse CayenneLPP for barometric pressure (type 115 / 0x73)
                        uint16_t i = 0;
                        while (i < telemetry_len) {
                            if (i + 1 >= telemetry_len) {
                                break; // Not enough bytes for channel and type
                            }
                            uint8_t channel = telemetry_data[i];
                            uint8_t type = telemetry_data[i + 1];
                            if (type == 0x73) { // Barometric pressure
                                if (i + 3 >= telemetry_len) {
                                    break; // Not enough bytes for pressure value
                                }
                                uint16_t press_raw = (telemetry_data[i + 2] << 8) | telemetry_data[i + 3];
                                if (g_state_mutex && xSemaphoreTake(g_state_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                                    g_last_pressure = (float)press_raw / 10.0f;
                                    g_pressure_updated = true;
                                    xSemaphoreGive(g_state_mutex);
                                }
                                ESP_LOGI(TAG, "Decoded pressure: %.1f hPa", (float)press_raw / 10.0f);
                                break;
                            }
                            
                            // Advance index depending on CayenneLPP type size
                            uint16_t step = 0;
                            if (type == 100) step = 3; // Lux: 2 bytes
                            else if (type == 101) step = 3; // Thermocouple: 2 bytes
                            else if (type == 102) step = 3; // Temp: 2 bytes
                            else if (type == 104) step = 11; // GPS: 9 bytes
                            else if (type == 115) step = 4; // Barometer: 2 bytes
                            else step = 3; // Default fallback
                            
                            if (step == 0 || i + step <= i || i + step > telemetry_len) {
                                // Prevent infinite loops or out-of-bounds advancement
                                break;
                            }
                            i += step;
                        }
                    }
                }
            }
            g_rx_len = 0;
            g_rx_escaped = false;
            g_rx_active = true;
        } else if (b == KISS_FESC) {
            g_rx_escaped = true;
        } else {
            uint8_t val = b;
            if (g_rx_escaped) {
                if (b == KISS_TFEND) val = KISS_FEND;
                else if (b == KISS_TFESC) val = KISS_FESC;
                g_rx_escaped = false;
            }
            if (g_rx_active && g_rx_len < sizeof(g_rx_frame)) {
                g_rx_frame[g_rx_len++] = val;
            }
        }
    }

    return 0;
}

}

