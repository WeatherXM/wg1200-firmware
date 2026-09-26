#ifndef GATEWAY_SNAPSHOT_H
#define GATEWAY_SNAPSHOT_H

#include "ws/include/adaptive_publish.h"

namespace GatewaySnapshot
{
    /**
     * @brief Build a pure WeatherSnapshot from the latest station FieldPack
     *        and onboard barometer (transparent fallback for WS1001).
     * @param out Output snapshot to populate
     * @return true if at least one field was successfully populated, false otherwise
     */
    bool build(AdaptivePublish::WeatherSnapshot *out);
}

#endif // GATEWAY_SNAPSHOT_H
