#include "mdns_discovery.h"
#include "mdns.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "credentials.h"
#include "app_config.h"
#include "settings.h"
#include "utils.h"

#include <string.h>
#include <stdio.h>

#ifndef CONFIG_HW_NAME
#define CONFIG_HW_NAME "WXM-WG1200"
#endif

static const char *TAG = "MdnsDiscovery";

namespace MdnsDiscovery {

static bool s_mdns_initialized = false;
static char s_gateway_id[36] = {0};

const char* get_stable_gateway_id() {
    if (s_gateway_id[0] != '\0') {
        return s_gateway_id;
    }

    char *serial = Credentials::get_serial_str();
    if (serial != nullptr && strlen(serial) > 0 && strcmp(serial, "00:11:22:33:44:55:66:77:88:99") != 0) {
        strncpy(s_gateway_id, serial, sizeof(s_gateway_id) - 1);
        return s_gateway_id;
    }

    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        snprintf(s_gateway_id, sizeof(s_gateway_id), "%02X%02X%02X%02X%02X%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        return s_gateway_id;
    }

    snprintf(s_gateway_id, sizeof(s_gateway_id), "wxm-unknown");
    return s_gateway_id;
}

esp_err_t start() {
    if (s_mdns_initialized) {
        ESP_LOGI(TAG, "mDNS already running; stopping prior instance");
        stop();
    }

    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize mDNS: %d", err);
        return err;
    }

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    char hostname[32];
    snprintf(hostname, sizeof(hostname), "wxm-gw-%02x%02x%02x",
             mac[3], mac[4], mac[5]);

    err = mdns_hostname_set(hostname);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mdns_hostname_set failed: %d", err);
    }

    char instance_name[64];
    const char *dev_name = Settings::get_device_name();
    if (dev_name && dev_name[0] != '\0') {
        snprintf(instance_name, sizeof(instance_name), "WeatherXM Gateway (%s)", dev_name);
    } else {
        snprintf(instance_name, sizeof(instance_name), "WeatherXM Gateway %02X%02X", mac[4], mac[5]);
    }
    mdns_instance_name_set(instance_name);

    const char *gw_id = get_stable_gateway_id();
    const char *hw_model = CONFIG_HW_NAME;
    const char *api_ver = "1.0.0";
    const char *fw_ver = APP_VERSION;

    mdns_txt_item_t txt_records[] = {
        {"id", gw_id},
        {"gateway_id", gw_id},
        {"model", hw_model},
        {"api_version", api_ver},
        {"sw_version", fw_ver}
    };

    err = mdns_service_add(NULL, "_weatherxm", "_tcp", 80, txt_records,
                           sizeof(txt_records) / sizeof(txt_records[0]));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add _weatherxm._tcp service: %d", err);
        mdns_free();
        return err;
    }

    s_mdns_initialized = true;
    ESP_LOGI(TAG, "mDNS announced: %s.local, service: _weatherxm._tcp (id: %s, model: %s, api: %s)",
             hostname, gw_id, hw_model, api_ver);
    return ESP_OK;
}

void stop() {
    if (s_mdns_initialized) {
        mdns_free();
        s_mdns_initialized = false;
        ESP_LOGI(TAG, "mDNS service stopped");
    }
}

} // namespace MdnsDiscovery
