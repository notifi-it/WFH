// Geist, converted from design/fonts/Geist-*.ttf with lv_font_conv at the
// sizes the UI actually uses. ASCII only (0x20-0x7F): the board shows names,
// numbers and two words of English, so the rest is flash spent on nothing.
//
// Regenerate with tools/gen-fonts.sh after changing a size or the typeface.
#pragma once

#include "lvgl.h"

LV_FONT_DECLARE(geist_14);          // blurb, status
LV_FONT_DECLARE(geist_18);          // buttons
LV_FONT_DECLARE(geist_22);          // tile countdown
LV_FONT_DECLARE(geist_28);          // large numerals
LV_FONT_DECLARE(geist_bold_30);     // popup title
