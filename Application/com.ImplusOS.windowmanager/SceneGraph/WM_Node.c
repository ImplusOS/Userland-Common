#include "WM_Node.h"

#include "../Animation/WM_Animation.h"
#include "../Compositor/WM_Damage.h"
#include "../../../../Userland/API/Source/Memory.h"
#include "../../../../Userland/API/Source/Process.h"

#include <stdlib.h>
#include <string.h>

#define WM_LARGE_WINDOW_ANIMATION_PIXELS 500000u

static wm_rect_t screen_bounds(const wm_state_t *state)
{
    return (wm_rect_t){0, 0, state->compositor.framebuffer_width,
                       state->compositor.framebuffer_height};
}

static wm_rect_t content_bounds(const wm_window_t *window)
{
    return (wm_rect_t){0, 0, window->frame.w, window->frame.h};
}

static bool window_can_use_partial_content_damage(const wm_window_t *window)
{
    if (!window || !window->visible || window->minimized ||
        window->close_requested || window->transition != WM_TRANSITION_NONE)
        return false;
    return window->visual_alpha >= 0.999f &&
           window->visual_scale >= 0.999f &&
           window->visual_scale <= 1.001f &&
           window->visual_offset_y > -0.5f &&
           window->visual_offset_y < 0.5f;
}

static void mark_content_damage_on_screen(wm_state_t *state,
                                          const wm_window_t *window,
                                          wm_rect_t rect)
{
    if (!state || !window) return;
    wm_rect_t clipped = wm_rect_intersection(rect, content_bounds(window));
    if (!clipped.w || !clipped.h) return;

    if (!window_can_use_partial_content_damage(window)) {
        wm_window_mark_frame_damage(state, window);
        return;
    }

    if (!window->is_system) {
        wm_rect_t visible_content = {
            0,
            0,
            window->frame.w > 2u ? window->frame.w - 2u : 0u,
            window->frame.h > 2u ? window->frame.h - 2u : 0u
        };
        clipped = wm_rect_intersection(clipped, visible_content);
        if (!clipped.w || !clipped.h) return;
    }

    wm_rect_t screen = {
        window->frame.x + clipped.x,
        window->frame.y +
            (int32_t)(window->is_system ? 0u : state->theme.title_height) +
            clipped.y,
        clipped.w,
        clipped.h
    };

    if (!window->is_system) {
        ++screen.x;
        ++screen.y;
    }

    wm_region_add(&state->compositor.damage, screen, screen_bounds(state));
}

static void layer_remove(wm_scene_t *scene, wm_window_t *window)
{
    wm_layer_t layer = window->layer;
    if (window->z_prev) window->z_prev->z_next = window->z_next;
    else scene->layer_top[layer] = window->z_next;
    if (window->z_next) window->z_next->z_prev = window->z_prev;
    else scene->layer_bottom[layer] = window->z_prev;
    window->z_prev = NULL;
    window->z_next = NULL;
}

static void layer_push_top(wm_scene_t *scene, wm_window_t *window, wm_layer_t layer)
{
    window->layer = layer;
    window->z_prev = NULL;
    window->z_next = scene->layer_top[layer];
    if (scene->layer_top[layer]) scene->layer_top[layer]->z_prev = window;
    else scene->layer_bottom[layer] = window;
    scene->layer_top[layer] = window;
}

static void layer_push_bottom(wm_scene_t *scene, wm_window_t *window, wm_layer_t layer)
{
    window->layer = layer;
    window->z_next = NULL;
    window->z_prev = scene->layer_bottom[layer];
    if (scene->layer_bottom[layer]) scene->layer_bottom[layer]->z_next = window;
    else scene->layer_top[layer] = window;
    scene->layer_bottom[layer] = window;
}

void wm_scene_init(wm_scene_t *scene)
{
    if (scene) memset(scene, 0, sizeof(*scene));
}

wm_window_t *wm_scene_find(wm_scene_t *scene, uint32_t id)
{
    if (!scene || id == 0u || id > WM_MAX_WINDOWS) return NULL;
    return scene->id_table[id];
}

wm_rect_t wm_window_visual_bounds(const wm_state_t *state, const wm_window_t *window)
{
    if (!state || !window) return (wm_rect_t){0, 0, 0, 0};
    float scale = window->visual_scale;
    if (scale <= 0.0f) scale = 1.0f;
    uint32_t title_height = window->is_system ? 0u : state->theme.title_height;
    uint32_t total_height = window->frame.h + title_height;
    uint32_t width = (uint32_t)((float)window->frame.w * scale);
    uint32_t height = (uint32_t)((float)total_height * scale);
    int32_t x = window->frame.x + (int32_t)((window->frame.w - width) / 2u);
    int32_t y = window->frame.y + (int32_t)((total_height - height) / 2u) +
                (int32_t)window->visual_offset_y;
    uint32_t margin = window->is_system ? 0u :
        state->theme.shadow_size * 2u + 6u;
    return (wm_rect_t){
        x - (int32_t)margin,
        y - (int32_t)margin,
        width + margin * 2u,
        height + margin * 2u
    };
}

void wm_window_mark_frame_damage(wm_state_t *state, const wm_window_t *window)
{
    if (!state || !window) return;
    wm_region_add(&state->compositor.damage, wm_window_visual_bounds(state, window),
                  screen_bounds(state));
    uint32_t dock_height = state->theme.dock_height + WM_DOCK_MARGIN * 2u;
    wm_region_add(&state->compositor.damage,
        (wm_rect_t){0, (int32_t)(state->compositor.framebuffer_height - dock_height),
                    state->compositor.framebuffer_width, dock_height},
        screen_bounds(state));
}

static bool allocate_surface(wm_window_t *window, uint32_t width, uint32_t height,
                             bool preserve)
{
    uint64_t bytes64 = (uint64_t)width * (uint64_t)height * sizeof(uint32_t);
    if (width == 0u || height == 0u || bytes64 > WM_SURFACE_MAX_BYTES ||
        bytes64 > SIZE_MAX) return false;
    int32_t shared_memory_handle =
        os_shared_memory_create((uint32_t)bytes64);
    if (shared_memory_handle < 0) return false;
    uint32_t *surface =
        (uint32_t *)os_shared_memory_map(shared_memory_handle);
    if (!surface ||
        os_shared_memory_grant(shared_memory_handle, window->owner_pid) < 0) {
        if (surface)
            (void)os_shared_memory_unmap(shared_memory_handle, surface);
        (void)os_shared_memory_close(shared_memory_handle);
        return false;
    }
    for (uint32_t col = 0u; col < width; ++col) surface[col] = window->bg_color;
    for (uint32_t row = 1u; row < height; ++row)
        memcpy(&surface[row * width], &surface[0], (size_t)width * sizeof(uint32_t));

    if (preserve && window->surface) {
        uint32_t copy_width = wm_min_u32(width, window->frame.w);
        uint32_t copy_height = wm_min_u32(height, window->frame.h);
        for (uint32_t row = 0; row < copy_height; ++row) {
            memcpy(&surface[row * width], &window->surface[row * window->frame.w],
                   (size_t)copy_width * sizeof(uint32_t));
        }
    }
    if (window->surface_shared_memory_handle > 0) {
        (void)os_shared_memory_unmap(window->surface_shared_memory_handle,
                                     window->surface);
        (void)os_shared_memory_close(window->surface_shared_memory_handle);
    } else {
        free(window->surface);
    }
    window->surface = surface;
    window->surface_bytes = (uint32_t)bytes64;
    window->surface_shared_memory_handle = shared_memory_handle;
    return true;
}

static bool window_should_animate_show(wm_rect_t frame)
{
    uint64_t pixels = (uint64_t)frame.w * (uint64_t)frame.h;
    return pixels < WM_LARGE_WINDOW_ANIMATION_PIXELS;
}

int32_t wm_scene_create_window(wm_state_t *state, int32_t owner_pid,
                               wm_rect_t frame, uint32_t background,
                               const char *title)
{
    if (!state || owner_pid <= 0 || frame.w < 1u || frame.h < 1u ||
        state->scene.window_count >= WM_MAX_WINDOWS) return -1;
    uint32_t max_width = wm_max_u32(state->compositor.framebuffer_width * 2u, 4096u);
    uint32_t max_height = wm_max_u32(state->compositor.framebuffer_height * 2u, 4096u);
    if (frame.w > max_width) frame.w = max_width;
    if (frame.h > max_height) frame.h = max_height;
    uint32_t id = 0u;
    for (uint32_t candidate = 1u; candidate <= WM_MAX_WINDOWS; ++candidate) {
        if (!state->scene.id_table[candidate]) {
            id = candidate;
            break;
        }
    }
    if (id == 0u) return -1;

    wm_window_t *window = (wm_window_t *)malloc(sizeof(*window));
    if (!window) return -1;
    memset(window, 0, sizeof(*window));
    window->id = id;
    window->owner_pid = owner_pid;
    window->frame = frame;
    window->restore_frame = frame;
    window->bg_color = background | 0xFF000000u;
    window->visible = true;
    window->layer = WM_LAYER_NORMAL;
    bool animate_show = window_should_animate_show(frame);
    window->visual_alpha = animate_show ? 0.0f : 1.0f;
    window->visual_scale = animate_show ? 0.92f : 1.0f;
    window->visual_offset_y = animate_show ? 14.0f : 0.0f;
    if (title) strncpy(window->title, title, sizeof(window->title) - 1u);
    if (!allocate_surface(window, frame.w, frame.h, false)) {
        free(window);
        return -1;
    }
    state->scene.id_table[id] = window;
    ++state->scene.window_count;
    layer_push_top(&state->scene, window, WM_LAYER_NORMAL);
    wm_scene_focus(state, window);
    if (animate_show) {
        wm_animation_start(state, window, WM_TRANSITION_SHOW, 220u);
    }
    wm_window_mark_frame_damage(state, window);
    return (int32_t)id;
}

void wm_scene_destroy_immediate(wm_state_t *state, uint32_t id)
{
    wm_window_t *window = wm_scene_find(&state->scene, id);
    if (!window) return;
    wm_window_mark_frame_damage(state, window);
    layer_remove(&state->scene, window);
    state->scene.id_table[id] = NULL;
    --state->scene.window_count;
    for (uint32_t i = 0; i < state->scene.input_sub_count;) {
        if (state->scene.input_subs[i].window_id == id) {
            state->scene.input_subs[i] =
                state->scene.input_subs[--state->scene.input_sub_count];
        } else {
            ++i;
        }
    }
    if (state->scene.focused_id == id) state->scene.focused_id = 0u;
    int32_t owner_pid = window->owner_pid;
    if (window->owner_pid > 0) {
        wm_msg_header_t destroyed = {
            .type = WM_WINDOW_DESTROYED,
            .request_id = 0u,
            .window_id = id,
        };
        ipc_send_message(window->owner_pid, &destroyed, sizeof(destroyed));
    }
    if (window->surface_shared_memory_handle > 0) {
        (void)os_shared_memory_unmap(window->surface_shared_memory_handle,
                                     window->surface);
        (void)os_shared_memory_close(window->surface_shared_memory_handle);
    } else {
        free(window->surface);
    }
    free(window->xml_buffer);
    free(window->shadow_mask);
    free(window);
    if (state->scene.focused_id == 0u) wm_scene_focus_next(state, id);

    if (owner_pid > 0 && owner_pid != process_get_current_pid()) {
        bool has_other = false;
        for (uint32_t i = 1u; i <= WM_MAX_WINDOWS; ++i) {
            if (state->scene.id_table[i] && state->scene.id_table[i]->owner_pid == owner_pid) {
                has_other = true;
                break;
            }
        }
        if (!has_other) {
            process_kill(owner_pid);
        }
    }
}

void wm_scene_destroy_window(wm_state_t *state, uint32_t id)
{
    wm_window_t *window = wm_scene_find(&state->scene, id);
    if (!window || window->close_requested) return;
    window->close_requested = true;
    if (state->scene.focused_id == id) wm_scene_focus_next(state, id);
    wm_animation_start(state, window, WM_TRANSITION_CLOSE, 170u);
}

bool wm_scene_set_frame(wm_state_t *state, wm_window_t *window, wm_rect_t frame)
{
    if (!state || !window) return false;
    if (frame.w < WM_MIN_WINDOW_WIDTH) frame.w = WM_MIN_WINDOW_WIDTH;
    if (frame.h < WM_MIN_WINDOW_HEIGHT) frame.h = WM_MIN_WINDOW_HEIGHT;
    uint32_t max_width = wm_max_u32(state->compositor.framebuffer_width * 2u, 4096u);
    uint32_t max_height = wm_max_u32(state->compositor.framebuffer_height * 2u, 4096u);
    if (frame.w > max_width) frame.w = max_width;
    if (frame.h > max_height) frame.h = max_height;

    wm_window_mark_frame_damage(state, window);
    if ((frame.w != window->frame.w || frame.h != window->frame.h) &&
        !allocate_surface(window, frame.w, frame.h, true)) return false;
    window->frame = frame;
    wm_region_reset(&window->damage);
    wm_region_add_full(&window->damage);
    wm_window_mark_frame_damage(state, window);
    return true;
}

void wm_scene_show(wm_state_t *state, wm_window_t *window)
{
    if (!state || !window) return;
    window->visible = true;
    window->minimized = false;
    window->close_requested = false;
    wm_animation_start(state, window, WM_TRANSITION_SHOW, 200u);
}

void wm_scene_hide(wm_state_t *state, wm_window_t *window)
{
    if (!state || !window || !window->visible) return;
    if (state->scene.focused_id == window->id) wm_scene_focus_next(state, window->id);
    wm_animation_start(state, window, WM_TRANSITION_HIDE, 150u);
}

void wm_scene_minimize(wm_state_t *state, wm_window_t *window)
{
    if (!state || !window || window->minimized) return;
    if (state->scene.focused_id == window->id) wm_scene_focus_next(state, window->id);
    wm_animation_start(state, window, WM_TRANSITION_MINIMIZE, 180u);
}

void wm_scene_restore(wm_state_t *state, wm_window_t *window)
{
    if (!state || !window) return;
    window->visible = true;
    window->minimized = false;
    wm_scene_raise(state, window);
    wm_scene_focus(state, window);
    wm_animation_start(state, window, WM_TRANSITION_RESTORE, 200u);
}

void wm_scene_raise(wm_state_t *state, wm_window_t *window)
{
    if (!state || !window || state->scene.layer_top[window->layer] == window) return;
    wm_window_mark_frame_damage(state, window);
    wm_layer_t layer = window->layer;
    layer_remove(&state->scene, window);
    layer_push_top(&state->scene, window, layer);
    wm_window_mark_frame_damage(state, window);
}

void wm_scene_lower(wm_state_t *state, wm_window_t *window)
{
    if (!state || !window || state->scene.layer_bottom[window->layer] == window) return;
    wm_window_mark_frame_damage(state, window);
    wm_layer_t layer = window->layer;
    layer_remove(&state->scene, window);
    layer_push_bottom(&state->scene, window, layer);
    wm_window_mark_frame_damage(state, window);
}

void wm_scene_focus(wm_state_t *state, wm_window_t *window)
{
    if (!state || !window || window->is_system || window->close_requested) return;
    wm_window_t *old = wm_scene_find(&state->scene, state->scene.focused_id);
    if (old == window) return;
    if (old) {
        old->has_focus = false;
        wm_window_mark_frame_damage(state, old);
    }
    window->has_focus = true;
    state->scene.focused_id = window->id;
    wm_window_mark_frame_damage(state, window);
}

void wm_scene_focus_next(wm_state_t *state, uint32_t excluded_id)
{
    if (!state) return;
    wm_window_t *old = wm_scene_find(&state->scene, state->scene.focused_id);
    if (old) {
        old->has_focus = false;
        wm_window_mark_frame_damage(state, old);
    }
    state->scene.focused_id = 0u;
    for (int layer = (int)WM_LAYER_FLOATING; layer >= (int)WM_LAYER_NORMAL; --layer) {
        for (wm_window_t *window = state->scene.layer_top[layer];
             window; window = window->z_next) {
            if (window->id != excluded_id && window->visible && !window->minimized &&
                !window->close_requested && !window->is_system) {
                wm_scene_focus(state, window);
                return;
            }
        }
    }
}

void wm_scene_set_system(wm_state_t *state, wm_window_t *window, bool system)
{
    if (!state || !window || window->is_system == system) return;
    wm_window_mark_frame_damage(state, window);
    layer_remove(&state->scene, window);
    window->is_system = system;
    layer_push_top(&state->scene, window, system ? WM_LAYER_PANEL : WM_LAYER_NORMAL);
    if (system && state->scene.focused_id == window->id)
        wm_scene_focus_next(state, window->id);
    wm_window_mark_frame_damage(state, window);
}

static bool point_in_rect(int32_t x, int32_t y, wm_rect_t rect)
{
    return x >= rect.x && y >= rect.y &&
           (int64_t)x < (int64_t)rect.x + rect.w &&
           (int64_t)y < (int64_t)rect.y + rect.h;
}

static wm_hit_zone_t window_zone(const wm_state_t *state,
                                 const wm_window_t *window, int32_t x, int32_t y)
{
    if (window->is_system) return WM_HIT_CONTENT;
    int32_t local_x = x - window->frame.x;
    int32_t local_y = y - window->frame.y;
    int32_t right = (int32_t)window->frame.w - local_x;
    int32_t total_height = (int32_t)(state->theme.title_height + window->frame.h);
    int32_t bottom = total_height - local_y;
    const int32_t edge = 7;
    bool left_edge = local_x <= edge;
    bool right_edge = right <= edge;
    bool top_edge = local_y <= edge;
    bool bottom_edge = bottom <= edge;
    if (top_edge && left_edge) return WM_HIT_RESIZE_TOP_LEFT;
    if (top_edge && right_edge) return WM_HIT_RESIZE_TOP_RIGHT;
    if (bottom_edge && left_edge) return WM_HIT_RESIZE_BOTTOM_LEFT;
    if (bottom_edge && right_edge) return WM_HIT_RESIZE_BOTTOM_RIGHT;
    if (left_edge) return WM_HIT_RESIZE_LEFT;
    if (right_edge) return WM_HIT_RESIZE_RIGHT;
    if (top_edge) return WM_HIT_RESIZE_TOP;
    if (bottom_edge) return WM_HIT_RESIZE_BOTTOM;
    if (local_y < (int32_t)state->theme.title_height) {
        if (right <= (int32_t)WM_TITLE_BUTTON_WIDTH) return WM_HIT_CLOSE;
        if (right <= (int32_t)(WM_TITLE_BUTTON_WIDTH * 2u)) return WM_HIT_MAXIMIZE;
        if (right <= (int32_t)(WM_TITLE_BUTTON_WIDTH * 3u)) return WM_HIT_MINIMIZE;
        return WM_HIT_TITLE;
    }
    return WM_HIT_CONTENT;
}

wm_window_t *wm_scene_hit_test(wm_state_t *state, int32_t x, int32_t y,
                               wm_hit_zone_t *zone)
{
    if (zone) *zone = WM_HIT_NONE;
    if (!state) return NULL;
    for (int layer = (int)WM_LAYER_OVERLAY; layer >= (int)WM_LAYER_NORMAL; --layer) {
        for (wm_window_t *window = state->scene.layer_top[layer];
             window; window = window->z_next) {
            if (!window->visible || window->minimized || window->visual_alpha < 0.99f ||
                window->close_requested) continue;
            wm_rect_t frame = window->frame;
            frame.h += window->is_system ? 0u : state->theme.title_height;
            if (point_in_rect(x, y, frame)) {
                if (zone) *zone = window_zone(state, window, x, y);
                return window;
            }
        }
    }
    return NULL;
}

void wm_window_damage_content(wm_state_t *state, wm_window_t *window, wm_rect_t rect)
{
    if (!state || !window) return;
    wm_rect_t clipped = wm_rect_intersection(rect, content_bounds(window));
    if (clipped.w == 0u || clipped.h == 0u) return;
    wm_region_add(&window->damage, clipped, content_bounds(window));
    if (window->transaction_depth != 0u) {
        window->transaction_dirty = true;
        return;
    }
    mark_content_damage_on_screen(state, window, clipped);
}

void wm_window_end_transaction(wm_state_t *state, wm_window_t *window)
{
    if (!state || !window || window->transaction_depth == 0u) return;
    --window->transaction_depth;
    if (window->transaction_depth == 0u && window->transaction_dirty) {
        window->transaction_dirty = false;
        if (window->damage.full) {
            mark_content_damage_on_screen(state, window, content_bounds(window));
        } else {
            for (uint32_t i = 0u; i < window->damage.count; ++i)
                mark_content_damage_on_screen(state, window,
                                              window->damage.rects[i]);
        }
    }
}
