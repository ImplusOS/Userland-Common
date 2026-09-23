#pragma once

#include "../Core/WM_State.h"

/*
 * Desktop icons. Targets come from /var/System/desktop.icons, falling back
 * to the arrangement shipped in Resource/Apps/desktop.icons; one
 * "Label|/path/to/App.ELF|launch argument" per line, with the last field
 * optional (blank / '#' lines ignored). Icons fill top-to-bottom and wrap
 * into further columns, and are painted into the compositor background; a
 * left click that misses every window launches the icon under the cursor.
 *
 * An entry that matches a Start-menu entry is drawn with that entry's icon,
 * which is how a Linux program gets its own artwork onto the desktop.
 */
#define WM_DESKTOP_MAX_ICONS 32u

void        wm_desktop_reload(void);
void        wm_desktop_draw(wm_state_t *state, wm_canvas_t *canvas);

/* Target under (x, y), or NULL. `*argument_out` receives the launch
 * argument to pass with it, or NULL when it takes none. */
const char *wm_desktop_hit_test(const wm_state_t *state, int32_t x, int32_t y,
                                const char **argument_out);
