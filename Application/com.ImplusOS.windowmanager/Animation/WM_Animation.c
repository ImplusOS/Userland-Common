#include "WM_Animation.h"

#include "../Compositor/WM_Damage.h"
#include "../SceneGraph/WM_Node.h"
#include "../Compositor/WM_Raster.h"
#include "../UI/WM_Taskbar.h"
#include "../UI/WM_StartMenu.h"
#include "../UI/WM_Notification.h"
#include "../UI/WM_WifiPanel.h"

void wm_animation_start(wm_state_t *state, wm_window_t *window,
                        wm_transition_t transition, uint32_t duration_ms)
{
    if (!state || !window) return;
    extern uint64_t get_uptime_ms(void);
    wm_window_mark_frame_damage(state, window);
    window->transition = transition;
    window->transition_started_ms = get_uptime_ms();
    window->transition_duration_ms = duration_ms == 0u ? 1u : duration_ms;
    if (transition == WM_TRANSITION_SHOW || transition == WM_TRANSITION_RESTORE) {
        /* Fade and a short rise only, at scale 1: a scaled window has to
         * be resampled every frame (wm_canvas_blit_scaled) and can never
         * be an opaque occluder, so scaling is what made these costly. */
        window->visual_alpha = 0.0f;
        window->visual_scale = 1.0f;
        window->visual_offset_y = 12.0f;
    } else {
        window->visual_alpha = 1.0f;
        window->visual_scale = 1.0f;
        window->visual_offset_y = 0.0f;
    }
    wm_window_mark_frame_damage(state, window);
}

bool wm_animation_tick(wm_state_t *state, uint64_t now_ms)
{
    if (!state) return false;
    bool active = false;
    for (uint32_t id = 1u; id <= WM_MAX_WINDOWS; ++id) {
        wm_window_t *window = state->scene.id_table[id];
        if (!window || window->transition == WM_TRANSITION_NONE) continue;
        active = true;
        wm_rect_t old_bounds = wm_window_visual_bounds(state, window);
        uint64_t elapsed = now_ms - window->transition_started_ms;
        float progress = elapsed >= window->transition_duration_ms ? 1.0f :
            (float)elapsed / (float)window->transition_duration_ms;
        bool appearing = window->transition == WM_TRANSITION_SHOW ||
                         window->transition == WM_TRANSITION_RESTORE;
        /* Cubic ease-out arriving, ease-in leaving. */
        float inv = 1.0f - progress;
        float eased = appearing ? 1.0f - inv * inv * inv
                                : progress * progress * progress;
        if (appearing) {
            window->visual_alpha = eased;
            window->visual_scale = 1.0f;
            window->visual_offset_y = 12.0f * (1.0f - eased);
        } else {
            window->visual_alpha = 1.0f - eased;
            window->visual_scale = 1.0f;
            window->visual_offset_y = 8.0f * eased;
        }
        wm_rect_t new_bounds = wm_window_visual_bounds(state, window);
        wm_rect_t merged = wm_rect_union(old_bounds, new_bounds);
        wm_rect_t screen = {0, 0, state->compositor.framebuffer_width,
                            state->compositor.framebuffer_height};
        wm_region_add(&state->compositor.damage, merged, screen);
        if (progress < 1.0f) continue;

        wm_transition_t completed = window->transition;
        window->transition = WM_TRANSITION_NONE;
        if (appearing) {
            window->visual_alpha = 1.0f;
            window->visual_scale = 1.0f;
            window->visual_offset_y = 0.0f;
        } else if (completed == WM_TRANSITION_CLOSE) {
            wm_scene_destroy_immediate(state, id);
        } else {
            window->visual_alpha = 0.0f;
            window->visual_scale = 1.0f;
            if (completed == WM_TRANSITION_MINIMIZE) window->minimized = true;
            /* The dock changes only when the transition commits. */
            wm_window_mark_frame_damage(state, window);
        }
    }
    return active;
}

/* ------------------------------------------------------------ panels */

wm_rect_t wm_panel_begin_clip(wm_state_t *state, wm_canvas_t *canvas,
                              const wm_panel_anim_t *anim)
{
    wm_rect_t old = canvas->clip;
    if (anim && anim->active) {
        wm_rect_t dock = wm_taskbar_rect(state);
        wm_rect_t above = {0, 0, state->compositor.framebuffer_width,
                           dock.y > 0 ? (uint32_t)dock.y : 0u};
        wm_canvas_set_clip(canvas, wm_rect_intersection(old, above));
    }
    return old;
}

typedef wm_rect_t (*panel_rect_fn)(const wm_state_t *state);

static bool tick_panel(wm_state_t *state, wm_panel_anim_t *anim, bool open,
                       panel_rect_fn rect_of, uint64_t now_ms)
{
    if (open != anim->shown) {
        /* Start from wherever the panel is now, so reversing mid-slide
         * does not jump. A panel that was never drawn starts hidden. */
        anim->from = anim->active ? anim->hidden : (open ? 1.0f : 0.0f);
        anim->shown = open;
        anim->active = true;
        anim->start_ms = now_ms;
    }
    if (!anim->active) return false;

    wm_rect_t before = rect_of(state);
    uint32_t duration = open ? WM_PANEL_OPEN_MS : WM_PANEL_CLOSE_MS;
    uint64_t elapsed = now_ms - anim->start_ms;
    float t = elapsed >= duration ? 1.0f : (float)elapsed / (float)duration;
    float target = open ? 0.0f : 1.0f;
    /* Cubic ease-out arriving, quadratic ease-in leaving. */
    float inv = 1.0f - t;
    float eased = open ? 1.0f - inv * inv * inv : t * t;
    anim->hidden = anim->from + (target - anim->from) * eased;
    if (t >= 1.0f) {
        anim->hidden = target;
        anim->active = false;
    }
    wm_rect_t after = rect_of(state);

    /* Repaint what the panel (and its shadow) covered and now covers, but
     * nothing below the taskbar edge: the panel is clipped there. */
    wm_rect_t dock = wm_taskbar_rect(state);
    wm_rect_t area = wm_rect_union(before, after);
    area = (wm_rect_t){area.x - 16, area.y - 12, area.w + 32u, area.h + 40u};
    wm_rect_t above = {0, 0, state->compositor.framebuffer_width,
                       dock.y > 0 ? (uint32_t)dock.y : 0u};
    /* The frame that ends a slide also restores the shadow over the bar. */
    wm_rect_t limit = anim->active ? above :
        (wm_rect_t){0, 0, state->compositor.framebuffer_width,
                    state->compositor.framebuffer_height};
    wm_region_add(&state->compositor.damage, area, limit);
    return true;
}

/* ------------------------------------------------------------ scrolling */

/* Time constant of the glide: each step closes dt / (dt + TAU) of the
 * remaining distance, so a wheel notch settles in roughly 200 ms and
 * notches arriving mid-glide just move the goal further along. */
#define WM_SCROLL_TAU_MS        50.0f
/* Step length assumed for the first step of a glide, one frame. */
#define WM_SCROLL_FIRST_STEP_MS 16u

static float clamp_scroll(float value, float max)
{
    if (max < 0.0f) max = 0.0f;
    if (value > max) value = max;
    if (value < 0.0f) value = 0.0f;
    return value;
}

bool wm_scroll_to(wm_scroll_t *scroll, float target, float max)
{
    if (!scroll) return false;
    target = clamp_scroll(target, max);
    if (target == scroll->target) return false;
    scroll->target = target;
    return true;
}

void wm_scroll_clamp(wm_scroll_t *scroll, float max)
{
    if (!scroll) return;
    scroll->target = clamp_scroll(scroll->target, max);
    scroll->pos = clamp_scroll(scroll->pos, max);
}

bool wm_scroll_step(wm_scroll_t *scroll, uint64_t now_ms)
{
    if (!scroll) return false;
    float diff = scroll->target - scroll->pos;
    if (diff > -0.5f && diff < 0.5f) {
        scroll->pos = scroll->target;
        scroll->last_ms = 0u;
        return false;
    }
    uint64_t dt = scroll->last_ms != 0u && now_ms > scroll->last_ms ?
        now_ms - scroll->last_ms : 0u;
    if (scroll->last_ms == 0u) dt = WM_SCROLL_FIRST_STEP_MS;
    if (dt == 0u) return true;      /* already stepped at this timestamp */
    if (dt > 100u) dt = 100u;       /* a stall should not become a jump */
    scroll->last_ms = now_ms;
    float k = (float)dt / ((float)dt + WM_SCROLL_TAU_MS);
    scroll->pos += diff * k;
    return true;
}

/* Glide one panel's list. The whole panel is repainted while it moves:
 * the scroll bar thumb lives outside the list's own rectangle. */
static bool tick_scroll(wm_state_t *state, wm_scroll_t *scroll, bool visible,
                        panel_rect_fn rect_of, uint64_t now_ms)
{
    if (!visible) {
        /* Nothing on screen to animate: settle at once. */
        scroll->pos = scroll->target;
        scroll->last_ms = 0u;
        return false;
    }
    int32_t before = wm_scroll_offset(scroll);
    bool moving = wm_scroll_step(scroll, now_ms);
    if (wm_scroll_offset(scroll) != before) {
        wm_rect_t r = rect_of(state);
        wm_region_add(&state->compositor.damage, r,
            (wm_rect_t){0, 0, state->compositor.framebuffer_width,
                        state->compositor.framebuffer_height});
    }
    return moving;
}

bool wm_panel_animation_tick(wm_state_t *state, uint64_t now_ms)
{
    if (!state) return false;
    bool a = tick_panel(state, &state->launcher_anim, state->launcher_open,
                        wm_start_menu_rect, now_ms);
    bool b = tick_panel(state, &state->notification_anim,
                        state->notification_center_open,
                        wm_notification_center_rect, now_ms);
    bool c = tick_panel(state, &state->wifi_anim, state->wifi_panel.open,
                        wm_wifi_panel_rect, now_ms);

    int32_t launcher_before = wm_scroll_offset(&state->launcher_scroll);
    bool d = tick_scroll(state, &state->launcher_scroll,
        wm_panel_visible(&state->launcher_anim, state->launcher_open),
        wm_start_menu_rect, now_ms);
    /* The list moved under a pointer that did not: what it points at
     * changed, so the hover highlight has to follow. Not when the keyboard
     * has the highlight (hover < 0): that scroll is the selection being
     * brought into view, and the pointer must not take it back. */
    if (state->launcher_open && state->launcher_hover_index >= 0 &&
        wm_scroll_offset(&state->launcher_scroll) != launcher_before) {
        wm_launcher_action_t hit = wm_start_menu_hit_test(state,
            (int32_t)state->scene.cursor_x, (int32_t)state->scene.cursor_y);
        state->launcher_hover_index = hit.kind == WM_LAUNCHER_ACTION_APP ?
            (int32_t)hit.app_index : -1;
    }
    bool e = tick_scroll(state, &state->notification_center_scroll,
        wm_panel_visible(&state->notification_anim,
                         state->notification_center_open),
        wm_notification_center_rect, now_ms);
    bool f = tick_scroll(state, &state->wifi_panel.scroll,
        wm_panel_visible(&state->wifi_anim, state->wifi_panel.open),
        wm_wifi_panel_rect, now_ms);
    return a || b || c || d || e || f;
}
