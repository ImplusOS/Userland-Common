#include "WM_Display.h"

#include "../Compositor/WM_Compositor.h"
#include "../../../../Userland/Syscalls.h"

#include <stdlib.h>
#include <string.h>

static bool point_in_rect(int32_t x, int32_t y, wm_rect_t rect)
{
    return x >= rect.x && y >= rect.y &&
           (int64_t)x < (int64_t)rect.x + rect.w &&
           (int64_t)y < (int64_t)rect.y + rect.h;
}

static bool rect_intersects(wm_rect_t a, wm_rect_t b)
{
    return (int64_t)a.x < (int64_t)b.x + b.w &&
           (int64_t)b.x < (int64_t)a.x + a.w &&
           (int64_t)a.y < (int64_t)b.y + b.h &&
           (int64_t)b.y < (int64_t)a.y + a.h;
}

static uint64_t distance_sq_to_rect(int32_t x, int32_t y, wm_rect_t rect)
{
    int64_t dx = 0;
    int64_t dy = 0;
    if (x < rect.x) {
        dx = (int64_t)rect.x - x;
    } else if ((int64_t)x >= (int64_t)rect.x + rect.w) {
        dx = (int64_t)x - ((int64_t)rect.x + rect.w - 1);
    }
    if (y < rect.y) {
        dy = (int64_t)rect.y - y;
    } else if ((int64_t)y >= (int64_t)rect.y + rect.h) {
        dy = (int64_t)y - ((int64_t)rect.y + rect.h - 1);
    }
    return (uint64_t)(dx * dx + dy * dy);
}

void wm_display_set_fallback(wm_state_t *state, uint32_t width, uint32_t height)
{
    if (!state) return;
    if (width == 0u) width = 1024u;
    if (height == 0u) height = 768u;

    memset(&state->display_topology, 0, sizeof(state->display_topology));
    state->display_topology.generation = 1u;
    state->display_topology.monitor_count = 1u;
    state->display_topology.primary_monitor = 0u;
    state->display_topology.width = width;
    state->display_topology.height = height;

    memset(state->monitors, 0, sizeof(state->monitors));
    state->monitor_count = 1u;
    state->monitors[0].bounds = (wm_rect_t){0, 0, width, height};
    state->monitors[0].info.index = 0u;
    state->monitors[0].info.flags =
        DISPLAY_MONITOR_FLAG_CONNECTED | DISPLAY_MONITOR_FLAG_PRIMARY;
    state->monitors[0].info.output_type = DISPLAY_OUTPUT_UNKNOWN;
    state->monitors[0].info.width = width;
    state->monitors[0].info.height = height;
    state->monitors[0].info.mode_count = 1u;
}

bool wm_display_update_from_system(wm_state_t *state)
{
    if (!state) return false;

    display_topology_t topology;
    memset(&topology, 0, sizeof(topology));
    if (display_get_topology(&topology) < 0 ||
        topology.width == 0u || topology.height == 0u ||
        topology.monitor_count == 0u) {
        wm_display_set_fallback(state, get_display_width(), get_display_height());
        return false;
    }

    uint32_t count = topology.monitor_count;
    if (count > DISPLAY_MAX_MONITORS) count = DISPLAY_MAX_MONITORS;

    memset(state->monitors, 0, sizeof(state->monitors));
    state->display_topology = topology;
    state->monitor_count = 0u;
    for (uint32_t i = 0u; i < count; ++i) {
        display_monitor_info_t info;
        memset(&info, 0, sizeof(info));
        if (display_get_monitor_info(i, &info) < 0 ||
            info.width == 0u || info.height == 0u) {
            continue;
        }
        uint32_t out = state->monitor_count++;
        state->monitors[out].info = info;
        state->monitors[out].bounds =
            (wm_rect_t){info.x, info.y, info.width, info.height};
    }

    if (state->monitor_count == 0u) {
        wm_display_set_fallback(state, topology.width, topology.height);
        return false;
    }
    state->display_topology.monitor_count = state->monitor_count;
    return true;
}

bool wm_display_reconfigure_if_needed(wm_state_t *state)
{
    if (!state) return false;

    display_topology_t topology;
    memset(&topology, 0, sizeof(topology));
    if (display_get_topology(&topology) < 0 ||
        topology.width == 0u || topology.height == 0u) {
        return false;
    }
    if (topology.generation == state->display_topology.generation &&
        topology.width == state->compositor.framebuffer_width &&
        topology.height == state->compositor.framebuffer_height &&
        topology.monitor_count == state->monitor_count) {
        return false;
    }

    uint32_t old_cursor_x = state->scene.cursor_x;
    uint32_t old_cursor_y = state->scene.cursor_y;
    if (!wm_display_update_from_system(state)) {
        return false;
    }
    if (!wm_compositor_resize(state, state->display_topology.width,
                              state->display_topology.height)) {
        return false;
    }
    if (old_cursor_x >= state->display_topology.width) {
        old_cursor_x = state->display_topology.width - 1u;
    }
    if (old_cursor_y >= state->display_topology.height) {
        old_cursor_y = state->display_topology.height - 1u;
    }
    state->scene.cursor_x = old_cursor_x;
    state->scene.cursor_y = old_cursor_y;
    wm_compositor_generate_background(state);
    wm_compositor_damage_all(state);
    return true;
}

wm_rect_t wm_display_virtual_bounds(const wm_state_t *state)
{
    if (!state) return (wm_rect_t){0, 0, 0, 0};
    return (wm_rect_t){0, 0, state->compositor.framebuffer_width,
                       state->compositor.framebuffer_height};
}

uint32_t wm_display_monitor_at_point(const wm_state_t *state, int32_t x, int32_t y)
{
    if (!state || state->monitor_count == 0u) return 0u;
    for (uint32_t i = 0u; i < state->monitor_count; ++i) {
        if (point_in_rect(x, y, state->monitors[i].bounds)) {
            return i;
        }
    }

    uint32_t best = 0u;
    uint64_t best_dist = UINT64_MAX;
    for (uint32_t i = 0u; i < state->monitor_count; ++i) {
        uint64_t dist = distance_sq_to_rect(x, y, state->monitors[i].bounds);
        if (dist < best_dist) {
            best_dist = dist;
            best = i;
        }
    }
    return best;
}

uint32_t wm_display_monitor_for_rect(const wm_state_t *state, wm_rect_t rect)
{
    int32_t cx = rect.x + (int32_t)(rect.w / 2u);
    int32_t cy = rect.y + (int32_t)(rect.h / 2u);
    return wm_display_monitor_at_point(state, cx, cy);
}

wm_rect_t wm_display_monitor_bounds(const wm_state_t *state, uint32_t monitor_index)
{
    if (!state || state->monitor_count == 0u ||
        monitor_index >= state->monitor_count) {
        return wm_display_virtual_bounds(state);
    }
    return state->monitors[monitor_index].bounds;
}

wm_rect_t wm_display_work_area_for_monitor(const wm_state_t *state,
                                           uint32_t monitor_index)
{
    wm_rect_t monitor = wm_display_monitor_bounds(state, monitor_index);
    if (!state || monitor.w == 0u || monitor.h == 0u) return monitor;

    const int32_t margin = 6;
    int32_t left = monitor.x + margin;
    int32_t top = monitor.y + margin;
    int32_t right = monitor.x + (int32_t)monitor.w - margin;
    int32_t bottom = monitor.y + (int32_t)monitor.h - margin;
    uint32_t dock_h = state->theme.dock_height;
    if (dock_h > state->compositor.framebuffer_height) {
        dock_h = state->compositor.framebuffer_height;
    }
    wm_rect_t dock = {
        0,
#if WM_TASKBAR_AT_TOP
        0,
#else
        (int32_t)(state->compositor.framebuffer_height - dock_h),
#endif
        state->compositor.framebuffer_width,
        dock_h
    };
    if (rect_intersects(monitor, dock)) {
#if WM_TASKBAR_AT_TOP
        top = dock.y + (int32_t)dock.h + margin;
#else
        bottom = dock.y - margin;
#endif
    }
    if (right < left) right = left;
    if (bottom < top) bottom = top;
    return (wm_rect_t){left, top, (uint32_t)(right - left),
                       (uint32_t)(bottom - top)};
}

wm_rect_t wm_display_work_area_for_rect(const wm_state_t *state, wm_rect_t rect)
{
    uint32_t monitor = wm_display_monitor_for_rect(state, rect);
    return wm_display_work_area_for_monitor(state, monitor);
}
