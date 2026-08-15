#include "config.h"
#include "actions.g.h"      // the only translation unit that includes it

#include <string.h>

const settings_t *config_default(void) { return &SETTINGS_DEFAULT; }

int config_action_index(const settings_t *s, const char *id) {
    for (int i = 0; i < s->n_actions; i++) {
        if (strcmp(s->actions[i].id, id) == 0) return i;
    }
    return -1;
}
