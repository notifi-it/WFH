#pragma once

#include <stdbool.h>

/** §11 step 10: WiFi STA + SNTP, so the date the log is keyed by is real.
 *
 *  Blocking, and must run BEFORE the panel is powered: the radio and the
 *  QSPI panel share the memory bus and cannot coexist — the panel desyncs
 *  into garbage no matter how the transfers are scheduled around the radio
 *  window. Sync first, kill the radio completely, then bring the panel up.
 *
 *  Returns true if SNTP landed within timeout_s; false on timeout or when
 *  the configured SSID is empty (clock keeps the build-time seed either
 *  way). The radio is stopped before returning, always — stopped, not
 *  deinitialized: the driver keeps its RAM (see wifi_time.c). */
bool wifi_time_sync(const char *ssid, const char *pass, int timeout_s);
