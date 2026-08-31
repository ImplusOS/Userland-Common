#pragma once

#include "../Core/WM_State.h"

void wm_notification_add(wm_state_t *state, const char *title, const char *message);
void wm_notification_draw(wm_state_t *state, wm_canvas_t *canvas, uint64_t now_ms);
bool wm_notification_tick(wm_state_t *state, uint64_t now_ms);
wm_rect_t wm_notification_center_rect(const wm_state_t *state);
void wm_notification_toggle_center(wm_state_t *state);
void wm_notification_close_center(wm_state_t *state);
bool wm_notification_center_contains(const wm_state_t *state, int32_t x, int32_t y);
bool wm_notification_center_scroll(wm_state_t *state, int32_t rows);
