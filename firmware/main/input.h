#pragma once

#include <stdbool.h>

extern bool g_settings_sound;

void input_done(int action);
void input_skip(int action);
void input_toggle_sound(void);
