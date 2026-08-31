#include "WM_Animation.h"

#include "../Compositor/WM_Damage.h"
#include "../SceneGraph/WM_Node.h"

static float ease_out_cubic(float value)
{
    float inverse = 1.0f - value;
    return 1.0f - inverse * inverse * inverse;
}

static float ease_in_cubic(float value)
{
    return value * value * value;
}

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
        window->visual_alpha = 0.0f;
        window->visual_scale = 0.92f;
        window->visual_offset_y = 14.0f;
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
        float eased = appearing ? ease_out_cubic(progress) : ease_in_cubic(progress);
        if (appearing) {
            window->visual_alpha = eased;
            window->visual_scale = 0.92f + 0.08f * eased;
            window->visual_offset_y = 14.0f * (1.0f - eased);
        } else {
            window->visual_alpha = 1.0f - eased;
            window->visual_scale = 1.0f - 0.05f * eased;
            window->visual_offset_y = 10.0f * eased;
        }
        wm_rect_t new_bounds = wm_window_visual_bounds(state, window);
        wm_rect_t merged = wm_rect_union(old_bounds, new_bounds);
        wm_rect_t screen = {0, 0, state->compositor.framebuffer_width,
                            state->compositor.framebuffer_height};
        wm_region_add(&state->compositor.damage, merged, screen);
        uint32_t dh = state->theme.dock_height + WM_DOCK_MARGIN * 2u;
        wm_region_add(&state->compositor.damage,
            (wm_rect_t){0, (int32_t)(state->compositor.framebuffer_height - dh),
                        state->compositor.framebuffer_width, dh}, screen);
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
            window->visual_scale = 0.95f;
            if (completed == WM_TRANSITION_HIDE) window->visible = false;
            if (completed == WM_TRANSITION_MINIMIZE) window->minimized = true;
        }
    }
    return active;
}
