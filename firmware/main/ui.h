#pragma once

#include "card.h"

void ui_build(void);            // grid + card screens, once
void ui_refresh(void);          // redraw tiles from g_view
void ui_show_grid(void);
void ui_show_card(void);
void ui_sound_icon_update(void);

/** The card's side effects, wired to input_* and the screens (§6). */
const card_host_t *ui_card_host(void);
extern card_t g_card;
