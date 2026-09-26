#ifndef BOOT_GUARD_H
#define BOOT_GUARD_H

#include "struct.h"

namespace BootGuard
{
	/** Init boot guard. Must be called as early as possible in app_main,
	 *  right after NVS flash init and before any subsystem that could crash. */
	RetResult init();

	/** Declare the current boot healthy: clears the consecutive-crash counter
	 *  and reports any pending rollback record to the log buffer (-> Thingsboard).
	 *  Called automatically after HEALTHY_UPTIME_SEC and on every TB MQTT connect. */
	void mark_healthy();

	/** Called by SoftWatchdog just before it force-restarts the system, so the
	 *  next boot is counted as a crash boot (soft WD resets appear as plain
	 *  ESP_RST_SW and would otherwise be indistinguishable from normal restarts). */
	void note_soft_wd_reset();
}

#endif
