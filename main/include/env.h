#ifndef ENV_H
#define ENV_H

#if WXM_ENV_DEBUG == 1
    #if __has_include("env.debug.h")
        #include "env.debug.h"
    #else
        #include "env.build_env.h.template"
    #endif

	static_assert(sizeof(TB_MQTT_BROKERS) / sizeof(TB_MQTT_BROKERS[0]) >= 1, "At least one MQTT broker must be specified in debug build.");
#elif WXM_ENV_RELEASE == 1
    #if __has_include("env.release.h")
        #include "env.release.h"
    #else
        #include "env.build_env.h.template"
    #endif

	static_assert(sizeof(TB_MQTT_BROKERS) / sizeof(TB_MQTT_BROKERS[0]) == 1, "Only ONE MQTT broker must be specified in release build.");
#else
	#error "WXM_ENV_DEBUG/RELEASE not defined."
#endif

#endif