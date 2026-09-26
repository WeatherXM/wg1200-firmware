#ifndef WS_COMMON_H
#define WS_COMMON_H

#include "stdio.h"
#include "stdint.h"
#include "esp_log.h"

namespace WsCommon
{

/******************************************************************************
 * @brief Struct used to pass buff/len data
 ******************************************************************************/
struct Packet
{
	uint8_t* pl = NULL;
	uint8_t pl_len = 0;
};

/******************************************************************************
 * @brief Common return status
 ******************************************************************************/
enum class RetRes
{
	NONE = 0,
	OK,
	ERROR,
	ERROR_NOT_SUPPORTED,
	ERROR_BAD_PARAMS,
	ERROR_BAD_RESULT,
	ERROR_BAD_SIGNATURE,
	ERROR_BAD_ID,
	ERROR_BAD_CRC,
	ERROR_NULL_POINTER,
	ERROR_OUT_OF_BOUNDS,
	ERROR_TIMEOUT,
	ERROR_NOT_FOUND
};

/******************************************************************************
 * @brief Common compare status
 ******************************************************************************/
enum class CmpRes
{
	NONE = 0,
	GREATER,
	LESS,
	EQUAL
};

/******************************************************************************
 * @brief Print return status
 ******************************************************************************/
inline void print_ret_val(RetRes retval)
{
	switch (retval)
	{
		case RetRes::NONE:
			printf("RetRes:NONE\n");
			break;
		case RetRes::OK:
			printf("RetRes:OK\n");
			break;
		case RetRes::ERROR:
			printf("RetRes:ERROR\n");
			break;
		case RetRes::ERROR_NOT_SUPPORTED:
			printf("RetRes:ERROR_NOT_SUPPORTED\n");
			break;
		case RetRes::ERROR_BAD_PARAMS:
			printf("RetRes:ERROR_BAD_PARAMS\n");
			break;
		case RetRes::ERROR_BAD_RESULT:
			printf("RetRes:ERROR_BAD_RESULT\n");
			break;
		case RetRes::ERROR_BAD_SIGNATURE:
			printf("RetRes:ERROR_BAD_SIGNATURE\n");
			break;
		case RetRes::ERROR_BAD_ID:
			printf("RetRes:ERROR_BAD_ID\n");
			break;
		case RetRes::ERROR_BAD_CRC:
			printf("RetRes:ERROR_BAD_CRC\n");
			break;
		case RetRes::ERROR_NULL_POINTER:
			printf("RetRes:ERROR_NULL_POINTER\n");
			break;
		case RetRes::ERROR_OUT_OF_BOUNDS:
			printf("RetRes:ERROR_OUT_OF_BOUNDS\n");
			break;
		case RetRes::ERROR_TIMEOUT:
			printf("RetRes:ERROR_TIMEOUT\n");
			break;
		case RetRes::ERROR_NOT_FOUND:
			printf("RetRes:ERROR_NOT_FOUND\n");
			break;
	}
}

/******************************************************************************
 * @brief Source of a received weather station observation
 ******************************************************************************/
enum class ObservationSource : uint8_t
{
	DIRECT_LORA = 0,     // Local paired station (WS1000/WS1300/WS2000)
	MESHCORE_REMOTE = 1, // Relayed observation received over MeshCore
	MESHTASTIC_REMOTE = 2 // Relayed observation received over Meshtastic
};

/******************************************************************************
 * @brief Unified observation envelope for local and relayed station telemetry
 ******************************************************************************/
struct ObservationEnvelope
{
	ObservationSource source;
	uint32_t station_id;
	uint32_t timestamp;
	bool is_signed;
};

}

#endif