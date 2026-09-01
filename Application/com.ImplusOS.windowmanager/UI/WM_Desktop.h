#pragma once

#include "../Core/WM_State.h"

/*
 * Desktop icons. Positions + targets come from /var/System/desktop.icons,
 * one "label|/path/to/App.ELF" per line (blank / '#' ignored). Absent by
 * default -> nothing on the desktop. Icons are laid out in a top-left
 * column and painted into the compositor background; a left click that
 * misses every window launches the icon under the cursor.
 */
#define WM_DESKTOP_MAX_ICONS 32u

void        wm_desktop_reload(void);
void        wm_desktop_draw(wm_state_t *state, wm_canvas_t *canvas);
const char *wm_desktop_hit_test(const wm_state_t *state, int32_t x, int32_t y);
