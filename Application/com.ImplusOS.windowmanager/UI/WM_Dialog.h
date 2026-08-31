#pragma once

#include "../Core/WM_State.h"

void wm_dialog_show(wm_state_t *state, wm_dialog_type_t type,
                    const char *title, const char *message);
void wm_dialog_close(wm_state_t *state);
bool wm_dialog_is_active(const wm_state_t *state);
bool wm_dialog_contains(const wm_state_t *state, int32_t x, int32_t y);
bool wm_dialog_title_contains(const wm_state_t *state, int32_t x, int32_t y);
bool wm_dialog_close_contains(const wm_state_t *state, int32_t x, int32_t y);
bool wm_dialog_ok_contains(const wm_state_t *state, int32_t x, int32_t y);
bool wm_dialog_dragging(const wm_state_t *state);
void wm_dialog_set_dragging(wm_state_t *state, bool dragging);
void wm_dialog_set_hover_close(wm_state_t *state, bool hover);
void wm_dialog_set_hover_ok(wm_state_t *state, bool hover);
void wm_dialog_move(wm_state_t *state, int32_t x, int32_t y);
void wm_dialog_draw(wm_state_t *state, wm_canvas_t *canvas);
