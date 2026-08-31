#pragma once

#include "../Core/WM_State.h"

void wm_scene_init(wm_scene_t *scene);
wm_window_t *wm_scene_find(wm_scene_t *scene, uint32_t id);
int32_t wm_scene_create_window(wm_state_t *state, int32_t owner_pid,
                               wm_rect_t frame, uint32_t background,
                               const char *title);
void wm_scene_destroy_window(wm_state_t *state, uint32_t id);
void wm_scene_destroy_immediate(wm_state_t *state, uint32_t id);
bool wm_scene_set_frame(wm_state_t *state, wm_window_t *window, wm_rect_t frame);
void wm_scene_show(wm_state_t *state, wm_window_t *window);
void wm_scene_hide(wm_state_t *state, wm_window_t *window);
void wm_scene_minimize(wm_state_t *state, wm_window_t *window);
void wm_scene_restore(wm_state_t *state, wm_window_t *window);
void wm_scene_raise(wm_state_t *state, wm_window_t *window);
void wm_scene_lower(wm_state_t *state, wm_window_t *window);
void wm_scene_focus(wm_state_t *state, wm_window_t *window);
void wm_scene_focus_next(wm_state_t *state, uint32_t excluded_id);
void wm_scene_set_system(wm_state_t *state, wm_window_t *window, bool system);
wm_window_t *wm_scene_hit_test(wm_state_t *state, int32_t x, int32_t y,
                               wm_hit_zone_t *zone);
wm_rect_t wm_window_visual_bounds(const wm_state_t *state, const wm_window_t *window);
void wm_window_mark_frame_damage(wm_state_t *state, const wm_window_t *window);
void wm_window_damage_content(wm_state_t *state, wm_window_t *window, wm_rect_t rect);
void wm_window_end_transaction(wm_state_t *state, wm_window_t *window);
