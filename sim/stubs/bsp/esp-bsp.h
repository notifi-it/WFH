#pragma once
#include <stdbool.h>
// The simulator is single-threaded, so the display lock is a no-op. It exists
// only so ui.c compiles unchanged against both targets — the whole point is
// that the simulator runs the *same* file the board runs.
static inline bool bsp_display_lock(int t)   { (void)t; return true; }
static inline void bsp_display_unlock(void)  { }
