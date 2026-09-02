#pragma once

#include <time.h>

void input_done(int action);
void input_undo(int action, time_t slot);   // §7.7: takes back a done or skip
