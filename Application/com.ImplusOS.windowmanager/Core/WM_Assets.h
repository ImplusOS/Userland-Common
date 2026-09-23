#pragma once

#include "WM_State.h"

bool wm_assets_init(wm_assets_t *assets);
bool wm_assets_init_metadata(wm_assets_t *assets);
bool wm_assets_load_next_icon(wm_assets_t *assets);
bool wm_assets_reload_wallpaper(wm_assets_t *assets);
void wm_assets_destroy(wm_assets_t *assets);
uint32_t *wm_assets_load_png(const char *path, uint32_t *width, uint32_t *height);
bool wm_assets_load_logo(wm_assets_t *assets);

/* Start a launcher entry. Native entries are spawned directly; a Linux entry
 * spawns the X11 host launcher with the command line it carries. Returns the
 * new pid, or a negative value. */
int32_t wm_assets_launch(const wm_launcher_app_t *app);

/* Index of the launcher entry whose ELF and argument match, or -1. Lets the
 * desktop-icon grid reuse a menu entry's icon and label. */
int32_t wm_assets_find_app(const wm_assets_t *assets, const char *path,
                           const char *argument);
