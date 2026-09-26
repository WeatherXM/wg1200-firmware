#ifndef MINER_RTC_H
#define MINER_RTC_H

#include <time.h>
#include "struct.h"
#include "common.h"

namespace DatetimeRtc
{
	RetResult init();
	
	time_t get_tstamp();
	bool cur_time_valid();
	bool tstamp_valid(u32 tstamp);

	uint64_t timer_get_elapsed();
	void timer_print_elapsed(const char *source = nullptr);
	void timer_reset();

	RetResult wait_for_ntp_sync(uint32_t timeout_ms);

	void print_system_time();

	void force_sync();
	bool is_synced();
	void set_tstamp_from_gps(time_t tstamp);


} // namespace RTC


#endif