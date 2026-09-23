#pragma once

#include "../Core/WM_State.h"

/* Window title-bar controls, ordered outwards from the right edge, which is
 * also the order they are laid out in. */
typedef enum {
    WM_TITLE_BUTTON_CLOSE = 0,
    WM_TITLE_BUTTON_MAXIMIZE,
    WM_TITLE_BUTTON_MINIMIZE,
    WM_TITLE_BUTTON_COUNT
} wm_title_button_t;

void wm_decoration_draw_window(wm_state_t *state, wm_canvas_t *canvas,
                               wm_window_t *window);

/* Where one title-bar control sits, given the window's visual frame (the
 * title bar included). The drawing code and the hit test both ask this, so
 * a control cannot be painted somewhere it cannot be clicked. */
wm_rect_t wm_decoration_title_button_rect(const wm_state_t *state,
                                          wm_rect_t frame,
                                          wm_title_button_t which);

/* False for a window too narrow to hold the controls, which is then all
 * title bar. */
bool wm_decoration_has_title_buttons(uint32_t frame_width);
