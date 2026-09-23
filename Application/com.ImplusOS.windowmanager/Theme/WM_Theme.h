#pragma once

#include "../Core/WM_State.h"

/* Build the Material Design 3 baseline theme: the M3 baseline seed, the
 * light scheme, the specified type scale and the shell's own metrics. */
void wm_theme_set_defaults(wm_theme_t *theme);

/* Regenerate every colour role from `seed`, keeping the metrics. This is
 * the only way the shell's palette changes -- a .theme file's [Material]
 * section and the WM_SET_THEME message both come through here. */
void wm_theme_set_seed(wm_theme_t *theme, uint32_t seed, bool dark);

/* Apply the multiplier to the M3 type scale. */
void wm_theme_set_type_scale(wm_theme_t *theme, float scale);

/* Read a .theme file: a [Material] section naming the seed, the mode and
 * the type scale, a [Metrics] section, and an optional [Colors] section
 * overriding individual M3 roles by name. Section headers are decoration;
 * keys are read wherever they appear. */
bool wm_theme_load(wm_theme_t *theme, const char *path);

uint32_t wm_theme_parse_color(const char *text, uint32_t fallback);
