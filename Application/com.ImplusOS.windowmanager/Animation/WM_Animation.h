#pragma once

#include "../Core/WM_State.h"

void wm_animation_start(wm_state_t *state, wm_window_t *window,
                        wm_transition_t transition, uint32_t duration_ms);
bool wm_animation_tick(wm_state_t *state, uint64_t now_ms);
