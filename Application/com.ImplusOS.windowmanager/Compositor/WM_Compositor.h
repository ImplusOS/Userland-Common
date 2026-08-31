#pragma once

#include "../Core/WM_State.h"

bool wm_compositor_init(wm_state_t *state, uint32_t width, uint32_t height);
bool wm_compositor_resize(wm_state_t *state, uint32_t width, uint32_t height);
void wm_compositor_destroy(wm_compositor_t *compositor);
void wm_compositor_generate_background(wm_state_t *state);
void wm_compositor_damage_all(wm_state_t *state);
bool wm_compositor_has_pending_frame(const wm_state_t *state);
void wm_compositor_render(wm_state_t *state, uint64_t now_ms);
