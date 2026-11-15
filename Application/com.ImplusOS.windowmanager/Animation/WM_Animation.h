#pragma once

#include "../Core/WM_State.h"

/* Transition lengths: a window opening, being shown or restored, and
 * leaving (hide, minimize, close). */
#define WM_ANIM_OPEN_MS 400u
#define WM_ANIM_SHOW_MS 300u
#define WM_ANIM_HIDE_MS 200u

void wm_animation_start(wm_state_t *state, wm_window_t *window,
                        wm_transition_t transition, uint32_t duration_ms);
bool wm_animation_tick(wm_state_t *state, uint64_t now_ms);
