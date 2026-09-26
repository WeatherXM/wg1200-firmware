#ifndef PUBLIC_API_H
#define PUBLIC_API_H

#include "const.h"
#include "struct.h"
#include "math.h"

namespace PublicApi
{

/** Response callback */
typedef void (*DevSearchCb) (const char* dev_id, const char* cell_index);

struct Rewards
{
	float total_rewards = NAN;
	float latest_total_reward = NAN;
	uint32_t ts = 0;
};

enum class PolReason
{
	NONE = 0,
	NO_LOCATION_DATA,
	LOCATION_NOT_VERIFIED
};

struct DevInfo
{
	PolReason pol_reason = PolReason::NONE;
	uint8_t qod_score = 0;
	char address[PUB_API_ADDRESS_CHAR_SIZE] = {0};
};

void req_rewards_data();
void req_dev_info_data();

void init();

}

#endif