#pragma once

#include "../Core/WM_State.h"

/* Transition lengths: a window opening, being shown or restored, and
 * leaving (hide, minimize, close). */
#define WM_ANIM_OPEN_MS 220u
#define WM_ANIM_SHOW_MS 180u
#define WM_ANIM_HIDE_MS 140u

/* Popup panels sliding up out of / down into the taskbar. */
#define WM_PANEL_OPEN_MS  200u
#define WM_PANEL_CLOSE_MS 150u

void wm_animation_start(wm_state_t *state, wm_window_t *window,
                        wm_transition_t transition, uint32_t duration_ms);
bool wm_animation_tick(wm_state_t *state, uint64_t now_ms);

/* Advance the three panel slides and the scrolling of their lists; adds
 * their damage. True while any of them moves. */
bool wm_panel_animation_tick(wm_state_t *state, uint64_t now_ms);

/* ---- smooth scrolling (wm_scroll_t, see WM_State.h) ---- */

/* How far one wheel notch moves a list. */
#define WM_SCROLL_WHEEL_PX 96

/* Glide toward `target`, clamped to [0, max]. True if the target moved. */
bool wm_scroll_to(wm_scroll_t *scroll, float target, float max);

/* Pull both the position and the target back into [0, max] at once, for
 * when the list itself got shorter. */
void wm_scroll_clamp(wm_scroll_t *scroll, float max);

/* Jump to the top without animating (a new search, a fresh scan). */
static inline void wm_scroll_reset(wm_scroll_t *scroll)
{
    if (!scroll) return;
    scroll->pos = 0.0f;
    scroll->target = 0.0f;
    scroll->last_ms = 0u;
}

/* Ease `pos` toward `target`. True while it is still moving. */
bool wm_scroll_step(wm_scroll_t *scroll, uint64_t now_ms);

/* Where to draw the content from, in whole pixels. */
static inline int32_t wm_scroll_offset(const wm_scroll_t *scroll)
{
    return scroll ? (int32_t)(scroll->pos + 0.5f) : 0;
}

/* A panel is drawn while it is open or still sliding shut. */
static inline bool wm_panel_visible(const wm_panel_anim_t *anim, bool open)
{
    return open || (anim && anim->active);
}

/* How far below its resting place to draw a panel resting at `r`: at
 * hidden = 1 its top (and the shadow above it) is under the taskbar edge
 * at `dock_y`. */
static inline int32_t wm_panel_slide_offset(const wm_panel_anim_t *anim,
                                            wm_rect_t r, int32_t dock_y)
{
    if (!anim || anim->hidden <= 0.0f) return 0;
    int32_t travel = dock_y - r.y + 16;
    if (travel < 0) travel = 0;
    return (int32_t)(anim->hidden * (float)travel + 0.5f);
}

/* While a panel slides, clip its drawing to above the taskbar so it seems
 * to come out from behind it. Returns the clip to restore. */
wm_rect_t wm_panel_begin_clip(wm_state_t *state, wm_canvas_t *canvas,
                              const wm_panel_anim_t *anim);
