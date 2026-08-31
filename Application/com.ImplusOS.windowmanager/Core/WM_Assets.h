#pragma once

#include "WM_State.h"

bool wm_assets_init(wm_assets_t *assets);
bool wm_assets_init_metadata(wm_assets_t *assets);
bool wm_assets_load_next_icon(wm_assets_t *assets);
bool wm_assets_reload_wallpaper(wm_assets_t *assets);
void wm_assets_destroy(wm_assets_t *assets);
uint32_t *wm_assets_load_png(const char *path, uint32_t *width, uint32_t *height);
bool wm_assets_load_logo(wm_assets_t *assets);
