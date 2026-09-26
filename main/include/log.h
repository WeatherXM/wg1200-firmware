#ifndef LOG_H
#define LOG_H

#include "const.h"
#include "common.h"
#include "struct.h"

#include "ring_buff.h"
#include "log_codes.h"

namespace Log
{
	/**
	 * Verbosity levels.
	 */
	enum class Level
	{
		NONE = 0,  // This is the default level
		CRITICAL,  // Very important messages, including errors. This will be the default level on Thingsboard on new device creation
		WARNING,   // Warnings about possible (recoverable) issues
		DEBUG	   // Debug/informational messages (most verbose level)
	};

	/**
	 * Entry in the log buffer.
	 */
	struct Entry
	{
		enum Type
		{
			LOG_TYPE_CODE = 1,
			LOG_TYPE_MSG
		} type;

		u32 tstamp;
		
		union
		{	
			struct
			{
				Log::Code code;
				u32 meta1;
				u32 meta2;
			};
			char msg[LOG_MSG_LEN];
		};
	};

	void init();
	RetResult log(const char *msg); // Verbosity level DEBUG
	RetResult log(Log::Code code, int meta1 = 0, int meta2 = 0, Level verb_lvl = Level::DEBUG); // Verbosity level DEBUG
	RetResult logw(Log::Code code, int meta1 = 0, int meta2 = 0); // Verbosity level WARNING
	RetResult logc(Log::Code code, int meta1 = 0, int meta2 = 0); // Verbosity level CRITICAL

	RetResult pop(Entry * entry);
	uint32_t get_count();
}

#endif