#pragma once

#include "../Core/WM_State.h"

bool wm_compositor_init(wm_state_t *state, uint32_t width, uint32_t height);
bool wm_compositor_resize(wm_state_t *state, uint32_t width, uint32_t height);
void wm_compositor_destroy(wm_compositor_t *compositor);
void wm_compositor_generate_background(wm_state_t *state);
void wm_compositor_damage_all(wm_state_t *state);
bool wm_compositor_has_pending_frame(const wm_state_t *state);
/* True when the pointer sprite is the only thing waiting to be presented -
 * no other damage is queued and the cursor moved (or changed shape) since the
 * last render. WM_Main uses it to let cursor motion through immediately
 * instead of waiting for the compositor's frame cadence: a pointer that
 * trails the hand by a frame or more is the single most visible kind of
 * lag, and a cursor-only frame touches nothing but the two small squares it
 * moved between. */
bool wm_compositor_pending_is_cursor_only(const wm_state_t *state);
void wm_compositor_render(wm_state_t *state, uint64_t now_ms);
