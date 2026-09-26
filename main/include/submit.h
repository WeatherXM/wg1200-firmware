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

}

#endif