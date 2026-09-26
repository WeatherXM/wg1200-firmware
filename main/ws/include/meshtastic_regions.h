#ifndef MESHTASTIC_REGIONS_H
#define MESHTASTIC_REGIONS_H

#include <stdint.h>
#include <stddef.h>
#include "struct.h"

namespace Meshtastic
{

/**
 * Mirrored from meshtastic/protobufs: meshtastic_Config_LoRaConfig_RegionCode
 */
enum class RegionCode : uint8_t
{
	UNSET = 0,
	US = 1,
	EU_433 = 2,
	EU_868 = 3,
	CN = 4,
	JP = 5,
	ANZ = 6,
	KR = 7,
	TW = 8,
	RU = 9,
	IN = 10,
	NZ_865 = 11,
	TH = 12,
	LORA_24 = 13,
	UA_433 = 14,
	UA_868 = 15,
	MY_433 = 16,
	MY_919 = 17,
	SG_923 = 18,
	PH_433 = 19,
	PH_868 = 20,
	PH_915 = 21,
	ANZ_433 = 22,
	KZ_433 = 23,
	KZ_863 = 24,
	NP_865 = 25,
	BR_902 = 26,
	ITU1_2M = 27,
	ITU2_2M = 28,
	EU_866 = 29,
	EU_874 = 30,
	EU_917 = 31,
	EU_N_868 = 32,
	ITU3_2M = 33,
	ITU1_70CM = 34,
	ITU2_70CM = 35,
	ITU3_70CM = 36,
	ITU2_125CM = 37
};

/**
 * Mirrored from meshtastic/protobufs: meshtastic_Config_LoRaConfig_ModemPreset
 */
enum class ModemPreset : uint8_t
{
	LONG_FAST = 0,
	LONG_SLOW = 1,
	VERY_LONG_SLOW = 2,
	MEDIUM_SLOW = 3,
	MEDIUM_FAST = 4,
	SHORT_SLOW = 5,
	SHORT_FAST = 6,
	LONG_MODERATE = 7,
	SHORT_TURBO = 8,
	LONG_TURBO = 9,
	LITE_FAST = 10,
	LITE_SLOW = 11,
	NARROW_FAST = 12,
	NARROW_SLOW = 13,
	TINY_FAST = 14,
	TINY_SLOW = 15,
	CUSTOM = 255
};

struct RegionInfo
{
	RegionCode code;
	const char *name;
	uint32_t freq_start_hz;
	uint32_t freq_end_hz;
	uint32_t default_freq_hz;
	uint8_t power_limit_dbm;
	ModemPreset default_preset;
};

static inline RegionCode get_region_code_for_freq_region(WsFreqRegion r)
{
	switch (r) {
	case WS_FREQ_REGION_EU868: return RegionCode::EU_868;
	case WS_FREQ_REGION_US915: return RegionCode::US;
	case WS_FREQ_REGION_AU915: return RegionCode::ANZ;
	case WS_FREQ_REGION_IN865: return RegionCode::IN;
	case WS_FREQ_REGION_AS923: return RegionCode::UNSET; // Generic 923 MHz does not uniquely identify region (JP/KR/TW/TH/SG)
	default: return RegionCode::UNSET;
	}
}

static inline const RegionInfo *get_region_info(RegionCode code)
{
	static const RegionInfo REGIONS[] = {
		{ RegionCode::EU_868, "EU_868", 869400000, 869650000, 869525000, 27, ModemPreset::LONG_FAST },
		{ RegionCode::US, "US", 902000000, 928000000, 906875000, 30, ModemPreset::LONG_FAST },
		{ RegionCode::EU_433, "EU_433", 433000000, 434000000, 433175000, 10, ModemPreset::LONG_FAST },
		{ RegionCode::EU_866, "EU_866", 865600000, 867600000, 866300000, 27, ModemPreset::LITE_FAST },
		{ RegionCode::EU_N_868, "EU_N_868", 869400000, 869650000, 869525000, 27, ModemPreset::NARROW_SLOW },
		{ RegionCode::ANZ, "ANZ", 915000000, 928000000, 916875000, 30, ModemPreset::LONG_FAST },
		{ RegionCode::JP, "JP", 920500000, 923500000, 921875000, 13, ModemPreset::LONG_FAST },
		{ RegionCode::KR, "KR", 920000000, 923000000, 921875000, 14, ModemPreset::LONG_FAST },
		{ RegionCode::TW, "TW", 920000000, 925000000, 923875000, 27, ModemPreset::LONG_FAST },
		{ RegionCode::RU, "RU", 868700000, 869200000, 868950000, 20, ModemPreset::LONG_FAST },
		{ RegionCode::IN, "IN", 865000000, 867000000, 865200000, 30, ModemPreset::LONG_FAST },
		{ RegionCode::NZ_865, "NZ_865", 864000000, 868000000, 865200000, 30, ModemPreset::LONG_FAST },
		{ RegionCode::TH, "TH", 920000000, 925000000, 923875000, 27, ModemPreset::LONG_FAST },
		{ RegionCode::SG_923, "SG_923", 920000000, 925000000, 923875000, 20, ModemPreset::LONG_FAST },
		{ RegionCode::MY_919, "MY_919", 919000000, 924000000, 921875000, 27, ModemPreset::LONG_FAST },
		{ RegionCode::PH_915, "PH_915", 915000000, 918000000, 916875000, 27, ModemPreset::LONG_FAST },
		{ RegionCode::BR_902, "BR_902", 902000000, 907500000, 905875000, 30, ModemPreset::LONG_FAST },
	};
	if (code == RegionCode::UNSET) {
		return nullptr;
	}
	for (size_t i = 0; i < sizeof(REGIONS)/sizeof(REGIONS[0]); ++i) {
		if (REGIONS[i].code == code) {
			return &REGIONS[i];
		}
	}
	return nullptr; // Fail-closed on invalid/unknown region
}

static inline void get_preset_params(ModemPreset preset, uint32_t &bw_hz, uint8_t &sf, uint8_t &cr)
{
	switch (preset) {
	case ModemPreset::SHORT_TURBO:
		bw_hz = 500000; sf = 7; cr = 5; break;
	case ModemPreset::SHORT_FAST:
		bw_hz = 250000; sf = 7; cr = 5; break;
	case ModemPreset::SHORT_SLOW:
		bw_hz = 250000; sf = 8; cr = 8; break;
	case ModemPreset::MEDIUM_FAST:
		bw_hz = 250000; sf = 9; cr = 5; break;
	case ModemPreset::MEDIUM_SLOW:
		bw_hz = 250000; sf = 10; cr = 5; break;
	case ModemPreset::LONG_TURBO:
		bw_hz = 500000; sf = 11; cr = 8; break;
	case ModemPreset::LONG_MODERATE:
		bw_hz = 125000; sf = 11; cr = 8; break;
	case ModemPreset::LONG_SLOW:
		bw_hz = 125000; sf = 12; cr = 8; break;
	case ModemPreset::LITE_FAST:
		bw_hz = 125000; sf = 9; cr = 5; break;
	case ModemPreset::LITE_SLOW:
		bw_hz = 125000; sf = 10; cr = 5; break;
	case ModemPreset::NARROW_FAST:
		bw_hz = 62500; sf = 7; cr = 6; break;
	case ModemPreset::NARROW_SLOW:
		bw_hz = 62500; sf = 8; cr = 6; break;
	case ModemPreset::TINY_FAST:
		bw_hz = 15600; sf = 7; cr = 5; break;
	case ModemPreset::TINY_SLOW:
		bw_hz = 15600; sf = 8; cr = 6; break;
	case ModemPreset::LONG_FAST:
	default:
		bw_hz = 250000; sf = 11; cr = 5; break;
	}
}

static inline const char *get_preset_name(ModemPreset preset)
{
	switch (preset) {
	case ModemPreset::LONG_FAST: return "LongFast";
	case ModemPreset::LONG_SLOW: return "LongSlow";
	case ModemPreset::VERY_LONG_SLOW: return "VeryLongSlow";
	case ModemPreset::MEDIUM_SLOW: return "MediumSlow";
	case ModemPreset::MEDIUM_FAST: return "MediumFast";
	case ModemPreset::SHORT_SLOW: return "ShortSlow";
	case ModemPreset::SHORT_FAST: return "ShortFast";
	case ModemPreset::LONG_MODERATE: return "LongModerate";
	case ModemPreset::SHORT_TURBO: return "ShortTurbo";
	case ModemPreset::LONG_TURBO: return "LongTurbo";
	case ModemPreset::LITE_FAST: return "LiteFast";
	case ModemPreset::LITE_SLOW: return "LiteSlow";
	case ModemPreset::NARROW_FAST: return "NarrowFast";
	case ModemPreset::NARROW_SLOW: return "NarrowSlow";
	case ModemPreset::TINY_FAST: return "TinyFast";
	case ModemPreset::TINY_SLOW: return "TinySlow";
	case ModemPreset::CUSTOM: return "Custom";
	default: return "Unknown";
	}
}

} // namespace Meshtastic

#endif // MESHTASTIC_REGIONS_H
