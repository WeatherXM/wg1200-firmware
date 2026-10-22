#ifndef SUBMIT_H
#define SUBMIT_H

#include "common.h"
#include "struct.h"

namespace Submit
{
RetResult init();
RetResult enable_autostart();
RetResult disable_autostart();
RetResult set_timer();
void start(void *param);
bool try_lock_ota();
void unlock_ota();

}

#endif