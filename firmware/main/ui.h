#pragma once

#include "card.h"

void ui_build(void);            // grid + card screens, once
void ui_refresh(void);          // redraw tiles from g_view
void ui_show_grid(void);
void ui_show_card(void);
void ui_show_popup(int action);
void ui_show_stretch(int action);   // guided flow, §7.6
void ui_show_history(int action);   // one action's day, §7.7
void ui_show_dayend(void);          // the tally after the last window closes, §7.8

/** The card's side effects, wired to input_* and the screens (§6). */
const card_host_t *ui_card_host(void);
extern card_t g_card;
