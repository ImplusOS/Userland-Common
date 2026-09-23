#pragma once

#include "../Core/WM_State.h"

typedef enum {
    WM_LAUNCHER_ACTION_NONE = 0,
    WM_LAUNCHER_ACTION_APP,
    WM_LAUNCHER_ACTION_REBOOT,
    WM_LAUNCHER_ACTION_SHUTDOWN
} wm_launcher_action_kind_t;

typedef struct {
    wm_launcher_action_kind_t kind;
    uint32_t app_index;
} wm_launcher_action_t;

wm_rect_t wm_start_menu_rect(const wm_state_t *state);
void wm_start_menu_draw(wm_state_t *state, wm_canvas_t *canvas);
wm_launcher_action_t wm_start_menu_hit_test(wm_state_t *state, int32_t x, int32_t y);
bool wm_start_menu_scroll(wm_state_t *state, int32_t rows);
void wm_start_menu_clamp_scroll(wm_state_t *state);
bool wm_start_menu_input_char(wm_state_t *state, char ch);
bool wm_start_menu_input_backspace(wm_state_t *state);

/* Move the keyboard selection by `delta` application rows, skipping section
 * headings and scrolling to keep the selection visible. Returns true if the
 * menu needs repainting. */
bool wm_start_menu_move_selection(wm_state_t *state, int32_t delta);

/* Launch the selected entry, or the first result when none is selected.
 * Returns the new pid, or a negative value when nothing matched. */
int32_t wm_start_menu_launch_selected(wm_state_t *state);