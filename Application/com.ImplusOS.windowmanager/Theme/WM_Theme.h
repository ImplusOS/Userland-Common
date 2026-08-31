#pragma once

#include "../Core/WM_State.h"

void wm_theme_set_defaults(wm_theme_t *theme);
bool wm_theme_load(wm_theme_t *theme, const char *path);
uint32_t wm_theme_parse_color(const char *text, uint32_t fallback);
