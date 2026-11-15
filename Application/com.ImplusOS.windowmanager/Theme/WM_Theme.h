#pragma once

#include "../Core/WM_State.h"

/* The default accent in light mode, with the shell's default metrics. */
void wm_theme_set_defaults(wm_theme_t *theme);

/* Rebuild the palette for `accent` / `dark`, keeping the metrics. */
void wm_theme_set_accent(wm_theme_t *theme, uint32_t accent, bool dark);

/* Apply a .theme file on top of what `theme` holds: `accent` and `mode`
 * (see Theme.h) and the [Metrics] keys title_height, corner_radius,
 * shadow_size and dock_height. Keys the file does not set are kept. */
bool wm_theme_load(wm_theme_t *theme, const char *path);
