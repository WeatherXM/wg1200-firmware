#ifndef STRUCT_H
#define STRUCT_H

#include <stdbool.h>
#include "common.h"

/**
 * Return struct for most functions
 */
enum RetResult
{
	RET_OK,

	// General error code
	RET_ERROR = -1,

	// Op timed out
	RET_TIMEOUT = -2,

	// Op cannot be executed in this state
	RET_INVALID_STATE = -3,

	// Invalid parameters provided
	RET_INVALID_PARAMS = -4
};

/**
  * Sample of a a rain counter measurement as received from the weather station
  */
struct PrecipCounterSample
{
	/** System tick of sample  */
	uint32_t tick_s;

	/** Counter cml value (mm) */
	float precip_cml;
};

/** Mqtt broker descriptor */
struct MqttBroker
{
	enum Id
	{
		NONE = -1,
		PROD,
		DEV
	} id;

	const char *name;
	const char *addr;
	int port;
};

enum WsFreqRegion
{
	WS_FREQ_REGION_NONE = -1,
	WS_FREQ_REGION_EU868 = 868,
	WS_FREQ_REGION_US915 = 915,
	WS_FREQ_REGION_AS923 = 923,
	WS_FREQ_REGION_AU915 = 916,
	WS_FREQ_REGION_IN865 = 865
};

struct WsFreqDesc
{
	const char *symbol;
	const char *title;
	WsFreqRegion freq_region;
	u32 freq_hz;
};

/** Temp unit constants
 * Constant val is used as index in dropdown lists, so vals must start from 0
*/
enum TempUnit
{
	TEMP_UNIT_NONE = -1,
	TEMP_UNIT_C,
	TEMP_UNIT_F
};

/** Single temp unit descriptor */
struct TempUnitDesc
{
	const char *symbol;
	const char *title;
	TempUnit unit;
};

/** Wind speed unit constants */
enum WindSpeedUnit
{
	WIND_SPEED_UNIT_NONE = -1,
	WIND_SPEED_UNIT_M_S,
	WIND_SPEED_UNIT_KM_H,
	WIND_SPEED_UNIT_MI_H,
	WIND_SPEED_UNIT_FT_S,
	WIND_SPEED_UNIT_BEAUFORT,
	WIND_SPEED_UNIT_KNOTS
};

/** Single wind speed unit descriptor */
struct WindSpeedUnitDesc
{
	const char *symbol;
	const char *title;
	WindSpeedUnit unit;
};

/** Rain unit constants */
enum PrecipUnit
{
	PRECIP_UNIT_NONE = -1,
	PRECIP_UNIT_MM,
	PRECIP_UNIT_IN
};

/** Single rain unit descriptor */
struct PrecipUnitDesc
{
	const char *symbol;
	const char *title;
	PrecipUnit unit;	
};

/** Pressure unit constants */
enum PressureUnit
{
	PRESSURE_UNIT_NONE = -1,
	PRESSURE_UNIT_HPA,
	PRESSURE_UNIT_MM_HG,
	PRESSURE_UNIT_IN_HG
};

/** Single pressure unit descriptor */
struct PressureUnitDesc
{
	const char *symbol;
	const char *title;
	PressureUnit unit;
};

/** Backlight levels. 
 * Values vs actual brightness relationship is not linear so names do not
 * not correspond to exact values. ie. Value for 25% brightness is actually
 * near 30. */
enum BacklightLevel
{
	BACKLIGHT_LEVEL_NONE = -1,
	BACKLIGHT_LEVEL_0 = 1,
	BACKLIGHT_LEVEL_25 = 20,
	BACKLIGHT_LEVEL_50 = 50,
	BACKLIGHT_LEVEL_75 = 75,
	BACKLIGHT_LEVEL_100 = 100
};

/** Backlight level descriptor */
struct BacklightLevelDesc
{
	const char *title;
	BacklightLevel level;
};

/** Screen rotations. 
 * All possible values of rotation */
enum ScreenRotation
{
	SCR_ROT_DEGREES_NONE = -1,
	SCR_ROT_DEGREES_0 = 0,
	SCR_ROT_DEGREES_180 = 180
};

/** Screen rotation descriptor */
struct ScreenRotationDesc
{
	const char *title;
	ScreenRotation degrees;
};

/** Pairing modes. Values are persisted in NVS - do not renumber.
 * OFF   - monitor only: sniff & decode packets, never pair, never uplink weather
 * CLOUD - station identity assigned via ThingsBoard shared attributes
 *         (with local auto-pair as fallback while waiting for the cloud)
 * LOCAL - locked to the locally paired station, weather uplink active
 * SCAN  - sweep all channels, auto-pair on ADV/BOOT, then transition to LOCAL
 *
 * Pairing Lifecycle States:
 * - standby_discovery : Unpaired gateway listening/sniffing (out-of-the-box, wiped NVS,
 *                       unassigned cloud ID, or active manual discovery scan). Decodes for display;
 *                       no weather telemetry uplink.
 * - cloud_verified    : Normal production mode. Assigned cloud station is paired, IDs match,
 *                       and ownership is confirmed/verified. Weather uplink active.
 * - local_override    : Explicitly locked to local NVS station (manually configured via Web UI
 *                       or local profile active while cloud ID differs/absent). Weather uplink active.
 * - fallback_latch    : Automated recovery fallback (Phase 3/4) timed out on legitimate targets
 *                       and latched onto an available WeatherXM signal to keep telemetry flowing.
 */
enum PairMode
{
	PAIR_MODE_NONE = -1,
	PAIR_MODE_OFF,
	PAIR_MODE_CLOUD,
	PAIR_MODE_LOCAL,
	PAIR_MODE_SCAN
};

/** Pairing identity selected by CLOUD/LOCAL. Values are persisted in NVS.
 * OFF preserves the selected source; SCAN selects/replaces the local profile. */
enum PairSource
{
	PAIR_SOURCE_NONE = 0,
	PAIR_SOURCE_CLOUD,
	PAIR_SOURCE_LOCAL
};

 /** Available stations */
enum WsType
{
	WS_TYPE_NONE = -1,
	WS_TYPE_1001,
	WS_TYPE_1300,
	WS_TYPE_ALL
};

/** Station type descriptor */
struct WsTypeDesc
{
	const char *title;
	WsType type;
};
#endif
