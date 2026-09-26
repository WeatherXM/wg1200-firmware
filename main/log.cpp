#include "log.h"
#include "datetime_rtc.h"
#include "ctime"
#include "settings.h"


namespace Log
{
// Log ring buffer
EXT_RAM_BSS_ATTR RingBuff<Entry, LOG_BUFFER_ITEM_COUNT> _buff;

//Last timestamp logged
u32 _last_log_ts = 0;

/******************************************************************************
* Init
******************************************************************************/
void init()
{	

}

/******************************************************************************
* Create current unique (from other entries) timestamp
******************************************************************************/
u32 calc_unique_timestamp()
{
	u32 ts = DatetimeRtc::get_tstamp();
	if(ts <= _last_log_ts) ts = ++_last_log_ts;
	else _last_log_ts = ts;
	return ts;
}

/******************************************************************************
* Create log entry
* @param msg Log message
******************************************************************************/
RetResult log(const char *msg)
{
	if(Settings::get_debug_mode_verbosity() != Level::DEBUG || !DatetimeRtc::cur_time_valid())
	{
		return RET_INVALID_STATE;
	}

	Entry entry = {};
	entry.tstamp = calc_unique_timestamp();
	entry.type = Entry::LOG_TYPE_MSG;
	
	strncpy(entry.msg, msg, sizeof(entry.msg));

	_buff.lock();
	_buff.push(&entry);
	_buff.unlock();

	return RET_OK;
}

/******************************************************************************
 * @brief Shorthand for log(Log::Code code) with WARNING verbosity.
 * @param code Error code
 * @param meta1 Metadata 1
 * @param meta2 Metadata 2
 ******************************************************************************/
RetResult logw(Log::Code code, int meta1, int meta2)
{
	return log(code, meta1, meta2, Level::WARNING);
}

/******************************************************************************
 * @brief Shorthand for log(Log::Code code) with CRITICAL verbosity.
 * @param code Error code
 * @param meta1 Metadata 1
 * @param meta2 Metadata 2
 ******************************************************************************/
RetResult logc(Log::Code code, int meta1, int meta2)
{
	return log(code, meta1, meta2, Level::CRITICAL);
}

/******************************************************************************
* Create log entry
* @param code Error code
* @param meta1 Metadata 1
* @param meta2 Metadata 2
******************************************************************************/
RetResult log(Log::Code code, int meta1, int meta2, Level verb_lvl)
{
	if((int) verb_lvl > (int) Settings::get_debug_mode_verbosity() || !DatetimeRtc::cur_time_valid())
	{
		return RET_INVALID_STATE;
	}

	Entry entry = {};
	entry.tstamp = calc_unique_timestamp();

	entry.type = Entry::LOG_TYPE_CODE;
	entry.code = code;
	entry.meta1 = meta1;
	entry.meta2 = meta2;

	_buff.lock();
	_buff.push(&entry);
	_buff.unlock();

	return RET_OK;
}

/******************************************************************************
* Pop latest log entry
* @param entry log entry pointer output
* @return RET_ERROR if no item to pop, RET_OK if pop is OK
******************************************************************************/
RetResult pop(Entry *entry)
{
	if(!entry) return RET_INVALID_PARAMS;

	_buff.lock();
	bool ret = _buff.pop(entry);
	_buff.unlock();

	return ret ? RET_OK : RET_ERROR;
}

/******************************************************************************
* Get log entries count
* @return uint32_t number of log entries
******************************************************************************/
uint32_t get_count()
{
	_buff.lock();
	uint32_t entries_count = _buff.get_item_count();
	_buff.unlock();

	return entries_count;
}

/******************************************************************************
* Cleanup
******************************************************************************/
void cleanup()
{

}

}