#ifndef MDNS_DISCOVERY_H
#define MDNS_DISCOVERY_H

#include "esp_err.h"

namespace MdnsDiscovery {

/**
 * @brief Initialize mDNS and register _weatherxm._tcp service on port 80.
 *        Includes TXT records for stable gateway ID, model, and API version.
 * @return ESP_OK on success, error code otherwise.
 */
esp_err_t start();

/**
 * @brief Stop and free mDNS service.
 */
void stop();

/**
 * @brief Retrieve the stable gateway identifier.
 *        Returns provisioned serial string or falls back to station MAC address.
 */
const char* get_stable_gateway_id();

} // namespace MdnsDiscovery

#endif // MDNS_DISCOVERY_H
