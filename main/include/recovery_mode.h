#ifndef RECOVERY_MODE_H
#define RECOVERY_MODE_H

namespace RecoveryMode
{
enum{
	EV_ENTER_TEST_MODE_BIT = BIT0,
	EV_RESTORE_FW_BIT = BIT1,
	EV_RECOVERY_MODE_FINISHED = BIT2
};
void enter();
}

#endif