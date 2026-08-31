#pragma once

#include "WM_State.h"

void wm_display_set_fallback(wm_state_t *state, uint32_t width, uint32_t height);
bool wm_display_update_from_system(wm_state_t *state);
bool wm_display_reconfigure_if_needed(wm_state_t *state);
wm_rect_t wm_display_virtual_bounds(const wm_state_t *state);
uint32_t wm_display_monitor_at_point(const wm_state_t *state, int32_t x, int32_t y);
uint32_t wm_display_monitor_for_rect(const wm_state_t *state, wm_rect_t rect);
wm_rect_t wm_display_monitor_bounds(const wm_state_t *state, uint32_t monitor_index);
wm_rect_t wm_display_work_area_for_monitor(const wm_state_t *state,
                                           uint32_t monitor_index);
wm_rect_t wm_display_work_area_for_rect(const wm_state_t *state, wm_rect_t rect);
