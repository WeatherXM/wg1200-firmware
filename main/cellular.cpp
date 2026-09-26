#include "include/cellular.h"

#ifdef CONFIG_CELLULAR_BACKUP

#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

// LwIP / PPP headers
#include "netif/ppp/ppp.h"
#include "netif/ppp/pppos.h"
#include "lwip/dns.h"
#include "lwip/netif.h"

#include "global_state.h"

#define TAG "Cellular"

#if defined(LILYGO_SIM7000_ESP32)
#define MODEM_TX_PIN 27
#define MODEM_RX_PIN 26
#define MODEM_PWRKEY_PIN 4
#elif defined(LILYGO_SIM7000G_S3_STANDARD)
#define MODEM_TX_PIN 5
#define MODEM_RX_PIN 4
#define MODEM_PWRKEY_PIN 41
#else
// Fallback defaults
#define MODEM_TX_PIN -1
#define MODEM_RX_PIN -1
#define MODEM_PWRKEY_PIN -1
#endif

static uart_port_t g_uart_num = UART_NUM_2;
static bool g_initialized = false;
static bool g_connected = false;
static bool g_ppp_active = false;

static ppp_pcb *g_ppp = nullptr;
static struct netif g_ppp_netif;
static TaskHandle_t g_rx_task_handle = nullptr;

namespace Cellular {

static u32_t ppp_output_callback(ppp_pcb *pcb, const void *data, u32_t len, void *ctx) {
    return uart_write_bytes(g_uart_num, data, len);
}

static void ppp_status_callback(ppp_pcb *pcb, int err_code, void *ctx) {
    struct netif *pppif = ppp_netif(pcb);
    EventGroupHandle_t evg = GlobalState::get_event_group();

    switch (err_code) {
        case PPPERR_NONE: {
            ESP_LOGI(TAG, "PPP interface up and connected!");
            ESP_LOGI(TAG, "   IP: %s", ipaddr_ntoa(&pppif->ip_addr));
            ESP_LOGI(TAG, "   GW: %s", ipaddr_ntoa(&pppif->gw));
            ESP_LOGI(TAG, "   NM: %s", ipaddr_ntoa(&pppif->netmask));
#if LWIP_DNS
            ESP_LOGI(TAG, "   DNS1: %s", ipaddr_ntoa(dns_getserver(0)));
            ESP_LOGI(TAG, "   DNS2: %s", ipaddr_ntoa(dns_getserver(1)));
#endif
            g_connected = true;
            
            // Set as the default LwIP interface
            netif_set_default(pppif);

            if (evg) {
                xEventGroupSetBits(evg, GlobalState::EV_WIFI_CONNECTED_BIT);
                xEventGroupClearBits(evg, GlobalState::EV_WIFI_DISCONNECTED_BIT);
            }
            break;
        }
        case PPPERR_USER: {
            ESP_LOGI(TAG, "PPP connection closed by user request.");
            g_connected = false;
            if (evg) {
                xEventGroupClearBits(evg, GlobalState::EV_WIFI_CONNECTED_BIT);
                xEventGroupSetBits(evg, GlobalState::EV_WIFI_DISCONNECTED_BIT);
            }
            break;
        }
        case PPPERR_CONNECT: {
            ESP_LOGE(TAG, "PPP connection lost/failed.");
            g_connected = false;
            if (evg) {
                xEventGroupClearBits(evg, GlobalState::EV_WIFI_CONNECTED_BIT);
                xEventGroupSetBits(evg, GlobalState::EV_WIFI_DISCONNECTED_BIT);
            }
            break;
        }
        default: {
            ESP_LOGE(TAG, "PPP general error: %d", err_code);
            g_connected = false;
            if (evg) {
                xEventGroupClearBits(evg, GlobalState::EV_WIFI_CONNECTED_BIT);
                xEventGroupSetBits(evg, GlobalState::EV_WIFI_DISCONNECTED_BIT);
            }
            break;
        }
    }
}

static void cellular_ppp_rx_task(void *pvParameters) {
    uint8_t rx_buf[1024];
    ESP_LOGI(TAG, "PPP RX background reader task started.");
    
    while (g_ppp_active) {
        int len = uart_read_bytes(g_uart_num, rx_buf, sizeof(rx_buf) - 1, pdMS_TO_TICKS(10));
        if (len > 0) {
            if (g_ppp) {
                pppos_input(g_ppp, rx_buf, len);
            }
        }
    }
    ESP_LOGI(TAG, "PPP RX background reader task stopped.");
    vTaskDelete(NULL);
}

static esp_err_t send_at_command(const char* cmd, const char* expected_resp, uint32_t timeout_ms) {
    // Flush UART RX buffer first to prevent matching old responses
    uart_flush_input(g_uart_num);
    
    uart_write_bytes(g_uart_num, cmd, strlen(cmd));
    uart_write_bytes(g_uart_num, "\r\n", 2);

    char rx_buf[512];
    uint32_t start = esp_log_timestamp();
    int len = 0;
    while (esp_log_timestamp() - start < timeout_ms) {
        int r = uart_read_bytes(g_uart_num, rx_buf + len, sizeof(rx_buf) - len - 1, pdMS_TO_TICKS(50));
        if (r > 0) {
            len += r;
            rx_buf[len] = '\0';
            if (strstr(rx_buf, expected_resp)) {
                return ESP_OK;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return ESP_ERR_TIMEOUT;
}

static void toggle_pwrkey() {
    if (MODEM_PWRKEY_PIN < 0) return;
    ESP_LOGI(TAG, "Toggling PWRKEY on GPIO %d...", MODEM_PWRKEY_PIN);
    gpio_set_level((gpio_num_t)MODEM_PWRKEY_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(1200)); // Hold low for 1.2 seconds
    gpio_set_level((gpio_num_t)MODEM_PWRKEY_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(3000)); // Wait for module to boot
}

esp_err_t init() {
    if (g_initialized) return ESP_OK;
    if (MODEM_TX_PIN < 0 || MODEM_RX_PIN < 0) {
        ESP_LOGE(TAG, "Cellular modem pins are not configured for this hardware target.");
        return ESP_ERR_NOT_SUPPORTED;
    }

    // Configure PWRKEY pin as GPIO output if valid
    if (MODEM_PWRKEY_PIN >= 0) {
        gpio_config_t io_conf = {
            .pin_bit_mask = (1ULL << MODEM_PWRKEY_PIN),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE
        };
        gpio_config(&io_conf);
        gpio_set_level((gpio_num_t)MODEM_PWRKEY_PIN, 1);
    }

    uart_config_t uart_config = {};
    uart_config.baud_rate = 115200;
    uart_config.data_bits = UART_DATA_8_BITS;
    uart_config.parity = UART_PARITY_DISABLE;
    uart_config.stop_bits = UART_STOP_BITS_1;
    uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uart_config.rx_flow_ctrl_thresh = 122;
    uart_config.source_clk = UART_SCLK_DEFAULT;

    esp_err_t err = uart_driver_install(g_uart_num, 2048, 2048, 0, NULL, 0);
    if (err != ESP_OK) return err;
    err = uart_param_config(g_uart_num, &uart_config);
    if (err != ESP_OK) return err;
    err = uart_set_pin(g_uart_num, MODEM_TX_PIN, MODEM_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) return err;

    g_initialized = true;

    // Check if modem is responsive, if not, toggle PWRKEY
    if (send_at_command("AT", "OK", 1000) != ESP_OK) {
        toggle_pwrkey();
        if (send_at_command("AT", "OK", 5000) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to communicate with cellular modem after power cycle.");
            return ESP_FAIL;
        }
    }

    // Turn off echo for predictable responses
    send_at_command("ATE0", "OK", 1000);

    ESP_LOGI(TAG, "Cellular modem initialized successfully.");
    return ESP_OK;
}

bool is_connected() {
    return g_connected;
}

bool is_ppp_active() {
    return g_ppp_active;
}

esp_err_t start_ppp() {
    if (g_connected || g_ppp_active) {
        ESP_LOGW(TAG, "PPP connection already active or in progress.");
        return ESP_OK;
    }

    esp_err_t err = init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Initialization failed. Cannot start PPP.");
        return err;
    }

    ESP_LOGI(TAG, "Starting Cellular PPP connection...");

    // 1. Verify SIM ready
    if (send_at_command("AT+CPIN?", "READY", 2000) != ESP_OK) {
        ESP_LOGE(TAG, "SIM card not ready.");
        return ESP_ERR_INVALID_STATE;
    }

    // 2. Set APN (use default 1NCE APN "iot.1nce.net")
    char apn_cmd[128];
    snprintf(apn_cmd, sizeof(apn_cmd), "AT+CGDCONT=1,\"IP\",\"iot.1nce.net\"");
    if (send_at_command(apn_cmd, "OK", 2000) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure GPRS PDP Context APN.");
        return ESP_FAIL;
    }

    // 3. Wait for network registration (home or roaming)
    ESP_LOGI(TAG, "Waiting for cellular network registration...");
    uint32_t register_start = esp_log_timestamp();
    bool registered = false;
    while (esp_log_timestamp() - register_start < 30000) {
        if (send_at_command("AT+CGREG?", "+CGREG: 0,1", 1000) == ESP_OK ||
            send_at_command("AT+CGREG?", "+CGREG: 0,5", 1000) == ESP_OK ||
            send_at_command("AT+CREG?", "+CREG: 0,1", 1000) == ESP_OK ||
            send_at_command("AT+CREG?", "+CREG: 0,5", 1000) == ESP_OK) {
            registered = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    if (!registered) {
        ESP_LOGE(TAG, "Network registration timeout.");
        return ESP_ERR_TIMEOUT;
    }
    ESP_LOGI(TAG, "Registered on network successfully.");

    // 4. Dial cellular network (switch modem to data mode)
    if (send_at_command("ATD*99#", "CONNECT", 15000) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to connect/dial GPRS (*99#).");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Modem switched to transparent PPP data mode.");

    // 5. Create LwIP PPP Interface
    g_ppp = pppos_create(&g_ppp_netif, ppp_output_callback, ppp_status_callback, NULL);
    if (g_ppp == nullptr) {
        ESP_LOGE(TAG, "Failed to create LwIP PPPoS interface block.");
        return ESP_ERR_NO_MEM;
    }

    ppp_set_usepeerdns(g_ppp, 1);
    ppp_connect(g_ppp, 0);

    // 6. Launch background RX task
    g_ppp_active = true;
    BaseType_t task_err = xTaskCreate(cellular_ppp_rx_task, "ppp_rx", 4096, NULL, 6, &g_rx_task_handle);
    if (task_err != pdPASS) {
        ESP_LOGE(TAG, "Failed to spawn PPP RX task.");
        ppp_free(g_ppp);
        g_ppp = nullptr;
        g_ppp_active = false;
        return ESP_FAIL;
    }

    // 7. Await PPP interface up
    ESP_LOGI(TAG, "Awaiting PPP negotiation IP configuration...");
    uint32_t ip_start = esp_log_timestamp();
    while (esp_log_timestamp() - ip_start < 20000) {
        if (g_connected) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    ESP_LOGE(TAG, "PPP IP assignment timed out.");
    stop_ppp();
    return ESP_ERR_TIMEOUT;
}

esp_err_t stop_ppp() {
    if (!g_ppp_active) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Closing PPP connection gracefully...");

    g_connected = false;

    // Disconnect and free LwIP PPP PCB
    if (g_ppp) {
        ppp_close(g_ppp, 0);
        // Wait up to 2 seconds for status callback and closing sequence
        vTaskDelay(pdMS_TO_TICKS(1000));
        ppp_free(g_ppp);
        g_ppp = nullptr;
    }

    g_ppp_active = false;

    // Stop background task
    if (g_rx_task_handle) {
        // Yield to allow task to terminate cleanly
        vTaskDelay(pdMS_TO_TICKS(100));
        vTaskDelete(g_rx_task_handle);
        g_rx_task_handle = nullptr;
    }

    // Trigger modem escape sequence to exit transparent data mode back to command mode
    ESP_LOGI(TAG, "Modem escape sequence: returning to AT command mode.");
    vTaskDelay(pdMS_TO_TICKS(1000)); // 1 sec guard time before
    uart_write_bytes(g_uart_num, "+++", 3);
    vTaskDelay(pdMS_TO_TICKS(1000)); // 1 sec guard time after

    // Send hangup command
    send_at_command("ATH", "OK", 2000);

    return ESP_OK;
}

void power_off() {
    stop_ppp();
    ESP_LOGI(TAG, "Powering down cellular modem...");
    if (g_initialized) {
        // Send software normal power down
        if (send_at_command("AT+CPOWD=1", "NORMAL POWER DOWN", 3000) != ESP_OK) {
            ESP_LOGW(TAG, "Modem did not respond to software shutdown. Forcing PWRKEY...");
            toggle_pwrkey();
        }
    }
}

} // namespace Cellular

#else // !CONFIG_CELLULAR_BACKUP

namespace Cellular {
    esp_err_t init() { return ESP_OK; }
    bool is_connected() { return false; }
    bool is_ppp_active() { return false; }
    esp_err_t start_ppp() { return ESP_OK; }
    esp_err_t stop_ppp() { return ESP_OK; }
    void power_off() {}
}

#endif // CONFIG_CELLULAR_BACKUP

