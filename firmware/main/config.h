#pragma once

#include "wfh_types.h"

/** The shipping schedule, generated from config/actions.json (§2.1). */
const settings_t *config_default(void);

/** Index of an action by string id, or -1. */
int config_action_index(const settings_t *s, const char *id);

/** The guided stretch set (§7.6). Returns the array, writes its length. */
const stretch_def_t *config_stretches(int *count);
