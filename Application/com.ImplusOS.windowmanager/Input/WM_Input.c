#include "WM_Input.h"

#include "../Compositor/WM_Damage.h"
#include "../Core/WM_Display.h"
#include "../SceneGraph/WM_Node.h"
#include "../UI/WM_StartMenu.h"
#include "../UI/WM_Taskbar.h"
#include "../UI/WM_Desktop.h"
#include "../UI/WM_Notification.h"
#include "../UI/WM_Dialog.h"
#include "../UI/WM_WifiPanel.h"
#include "../../../../Userland/Source/Syscalls.h"
#include "../../../../Userland/API/Source/Process.h"

#include <string.h>

#define WM_POINTER_FRAME_THROTTLE_MS 16u

typedef struct {
    wm_msg_header_t header;
    input_mouse_event_t event;
    uint32_t absolute_x;
    uint32_t absolute_y;
    uint64_t sequence;
} wm_mouse_event_message_t;

static wm_rect_t screen_bounds(const wm_state_t *state)
{
    return (wm_rect_t){0, 0, state->compositor.framebuffer_width,
                       state->compositor.framebuffer_height};
}

static wm_rect_t expand_rect(wm_rect_t rect, int32_t amount)
{
    return (wm_rect_t){
        rect.x - amount,
        rect.y - amount,
        rect.w + (uint32_t)(amount * 2),
        rect.h + (uint32_t)(amount * 2)
    };
}

static void damage_taskbar(wm_state_t *state)
{
    wm_region_add(&state->compositor.damage,
                  expand_rect(wm_taskbar_rect(state), 6),
                  screen_bounds(state));
}

static void damage_start_menu(wm_state_t *state)
{
    if (!state) return;
    wm_region_add(&state->compositor.damage,
                  expand_rect(wm_start_menu_rect(state), 8),
                  screen_bounds(state));
}

static void damage_notification_center(wm_state_t *state)
{
    if (!state) return;
    wm_region_add(&state->compositor.damage,
                  expand_rect(wm_notification_center_rect(state), 8),
                  screen_bounds(state));
}

static void damage_wifi_panel(wm_state_t *state)
{
    if (!state) return;
    wm_region_add(&state->compositor.damage,
                  expand_rect(wm_wifi_panel_rect(state), 8),
                  screen_bounds(state));
}

static void damage_ui(wm_state_t *state)
{
    damage_taskbar(state);
    damage_start_menu(state);
    damage_notification_center(state);
    damage_wifi_panel(state);
}

static void close_wifi_panel(wm_state_t *state)
{
    state->wifi_panel.open = false;
    state->wifi_panel.selected_index = -1;
    state->wifi_panel.password_active = false;
    state->wifi_panel.password_len = 0u;
    state->wifi_panel.password[0] = '\0';
}

/* Defined further down (needs damage_ui/close_wifi_panel above it in the
 * file already, but is itself needed by both the password-entry Enter-key
 * handler here and the mouse click handler below). */
static void perform_wifi_action(wm_state_t *state, wm_wifi_action_t action);

static void route_keyboard(wm_state_t *state, const ipc_message_t *message)
{
    wm_window_t *window = wm_scene_find(&state->scene, state->scene.focused_id);
    if (window && window->owner_pid > 0 && !window->close_requested)
        ipc_send_message(window->owner_pid, message->data, message->size);
}

static uint16_t clamp_u16(int32_t value)
{
    if (value < 0)      return 0u;
    if (value > 65535)  return 65535u;
    return (uint16_t)value;
}

static void route_mouse(wm_state_t *state, const input_mouse_event_t *event)
{
    wm_window_t *window = wm_scene_find(&state->scene, state->scene.focused_id);
    if (!window || window->owner_pid <= 0 || window->close_requested) return;
    struct {
        wm_msg_header_t header;
        input_mouse_event_t event;
    } outgoing;
    memset(&outgoing, 0, sizeof(outgoing));
    outgoing.header.type = WM_MOUSE_EVENT;
    outgoing.header.window_id = window->id;
    outgoing.event = *event;
    outgoing.event.x = clamp_u16((int32_t)state->scene.cursor_x - window->frame.x);
    outgoing.event.y = clamp_u16((int32_t)state->scene.cursor_y - window->frame.y -
        (int32_t)(window->is_system ? 0u : state->theme.title_height));
    ipc_send_message(window->owner_pid, &outgoing, sizeof(outgoing));
}

static void cycle_focus(wm_state_t *state)
{
    wm_window_t *current = wm_scene_find(&state->scene, state->scene.focused_id);
    wm_window_t *candidate = current ? current->z_next :
        state->scene.layer_top[WM_LAYER_NORMAL];
    while (candidate && (!candidate->visible || candidate->minimized ||
                         candidate->is_system || candidate->close_requested))
        candidate = candidate->z_next;
    if (!candidate) {
        candidate = state->scene.layer_top[WM_LAYER_NORMAL];
        while (candidate && (!candidate->visible || candidate->minimized ||
                             candidate->is_system || candidate->close_requested))
            candidate = candidate->z_next;
    }
    if (candidate && candidate != current) {
        wm_scene_raise(state, candidate);
        wm_scene_focus(state, candidate);
    }
}

static bool str_icontains(const char *haystack, const char *needle)
{
    if (!needle || !needle[0]) return true;
    if (!haystack) return false;
    for (const char *h = haystack; *h; h++) {
        const char *n = needle;
        const char *p = h;
        while (*n && *p) {
            char ch = *p>='A'&&*p<='Z'?(char)(*p+32):*p;
            char cn = *n>='A'&&*n<='Z'?(char)(*n+32):*n;
            if (ch != cn) break;
            n++; p++;
        }
        if (!*n) return true;
    }
    return false;
}

void wm_input_handle_keyboard(wm_state_t *state, const ipc_message_t *message)
{
    if (!state || !message ||
        message->size < sizeof(wm_msg_header_t) + sizeof(input_keyboard_event_t)) return;
    const input_keyboard_event_t *event =
        (const input_keyboard_event_t *)(message->data + sizeof(wm_msg_header_t));
        
    if (event->pressed && event->keycode == 0x0Fu && (event->modifiers & 0x04u)) {
        cycle_focus(state);
        return;
    }
    if (event->pressed && event->keycode == 0x01u &&
        (state->launcher_open || state->notification_center_open || state->wifi_panel.open)) {
        if (state->launcher_open && state->search_len > 0u) {
            state->search_len = 0u;
            state->search_text[0] = '\0';
            state->launcher_scroll = 0u;
        } else if (state->wifi_panel.open && state->wifi_panel.selected_index >= 0) {
            state->wifi_panel.selected_index = -1;
            state->wifi_panel.password_active = false;
            state->wifi_panel.password_len = 0u;
            state->wifi_panel.password[0] = '\0';
        } else {
            state->launcher_open = false;
            state->search_len = 0u;
            state->search_text[0] = '\0';
            wm_notification_close_center(state);
            close_wifi_panel(state);
        }
        damage_ui(state);
        return;
    }
    if (event->pressed && (event->keycode == 0x5Bu || event->keycode == 0x5Cu)) {
        state->launcher_open = !state->launcher_open;
        if (state->launcher_open) {
            wm_notification_close_center(state);
            wm_start_menu_clamp_scroll(state);
            state->search_active = true;
        } else {
            state->search_len = 0u;
            state->search_text[0] = '\0';
            state->search_active = false;
        }
        damage_ui(state);
        return;
    }
    if (state->launcher_open && state->search_active && event->pressed) {
        if (event->keycode == 0x0Eu) {
            if (wm_start_menu_input_backspace(state)) damage_ui(state);
            return;
        }
        if (event->keycode == 0x1Cu) {
            for (uint32_t i = 0; i < state->assets.app_count; i++) {
                if (str_icontains(state->assets.apps[i].name, state->search_text)) {
                    process_spawn(state->assets.apps[i].path);
                    state->launcher_open = false;
                    state->search_len = 0u;
                    state->search_text[0] = '\0';
                    damage_ui(state);
                    return;
                }
            }
            return;
        }
        if (event->ascii >= 0x20 && event->ascii < 0x7F) {
            if (wm_start_menu_input_char(state, (char)event->ascii))
                damage_ui(state);
            return;
        }
    }
    if (state->wifi_panel.open && state->wifi_panel.password_active && event->pressed) {
        if (event->keycode == 0x0Eu) {
            if (wm_wifi_panel_input_backspace(state)) damage_wifi_panel(state);
            return;
        }
        if (event->keycode == 0x1Cu) {
            perform_wifi_action(state, (wm_wifi_action_t){
                WM_WIFI_ACTION_CONNECT_PSK, (uint32_t)state->wifi_panel.selected_index});
            return;
        }
        if (event->ascii >= 0x20 && event->ascii < 0x7F) {
            if (wm_wifi_panel_input_char(state, (char)event->ascii))
                damage_wifi_panel(state);
            return;
        }
    }
    route_keyboard(state, message);
}

static void toggle_maximize(wm_state_t *state, wm_window_t *window)
{
    if (!state || !window) return;
    if (window->maximized) {
        window->maximized = false;
        wm_scene_set_frame(state, window, window->restore_frame);
        return;
    }
    window->restore_frame = window->frame;
    window->maximized = true;
    wm_rect_t area = wm_display_work_area_for_rect(state, window->frame);
    wm_rect_t frame = {
        area.x,
        area.y,
        area.w,
        area.h > state->theme.title_height ?
            area.h - state->theme.title_height : WM_MIN_WINDOW_HEIGHT
    };
    wm_scene_set_frame(state, window, frame);
}

static void snap_window(wm_state_t *state, wm_window_t *window)
{
    if (!state || !window || window->maximized) return;
    wm_rect_t area = wm_display_work_area_for_rect(state, window->frame);
    int32_t x = window->frame.x;
    int32_t y = window->frame.y;
    int32_t screen_right = area.x + (int32_t)area.w;
    if (y <= area.y + 4) {
        toggle_maximize(state, window);
        return;
    }
    uint32_t half_w = area.w / 2u;
    uint32_t content_h = area.h > state->theme.title_height ?
        area.h - state->theme.title_height : WM_MIN_WINDOW_HEIGHT;
    if (x <= area.x + 4) {
        window->restore_frame = window->frame;
        wm_scene_set_frame(state, window,
            (wm_rect_t){area.x, area.y, half_w > 10u ? half_w - 10u : half_w, content_h});
    } else if (x + (int32_t)window->frame.w >= screen_right - 4) {
        window->restore_frame = window->frame;
        wm_scene_set_frame(state, window,
            (wm_rect_t){area.x + (int32_t)half_w + 4, area.y,
                        area.w > half_w + 10u ?
                            area.w - half_w - 10u : half_w,
                        content_h});
    }
}

static void update_resize_cursor(wm_state_t *state, wm_hit_zone_t zone)
{
    wm_cursor_style_t style = WM_CURSOR_DEFAULT;
    if (zone == WM_HIT_RESIZE_LEFT || zone == WM_HIT_RESIZE_RIGHT)
        style = WM_CURSOR_RESIZE_HORIZONTAL;
    else if (zone == WM_HIT_RESIZE_TOP || zone == WM_HIT_RESIZE_BOTTOM)
        style = WM_CURSOR_RESIZE_VERTICAL;
    else if (zone == WM_HIT_RESIZE_TOP_LEFT || zone == WM_HIT_RESIZE_BOTTOM_RIGHT)
        style = WM_CURSOR_RESIZE_DIAGONAL_NW_SE;
    else if (zone == WM_HIT_RESIZE_TOP_RIGHT || zone == WM_HIT_RESIZE_BOTTOM_LEFT)
        style = WM_CURSOR_RESIZE_DIAGONAL_NE_SW;
    state->scene.cursor_style = style;
}

static void update_hover(wm_state_t *state)
{
    wm_hit_zone_t zone = WM_HIT_NONE;
    wm_window_t *hit = wm_scene_hit_test(state,
        (int32_t)state->scene.cursor_x, (int32_t)state->scene.cursor_y, &zone);
    uint32_t hit_id = hit ? hit->id : 0u;
    bool launcher_active = state->launcher_open;
    if (state->input.last_hover_window_id == hit_id &&
        state->input.last_hover_zone == zone &&
        !launcher_active) {
        if (!state->input.resizing) update_resize_cursor(state, zone);
        return;
    }
    state->input.last_hover_window_id = hit_id;
    state->input.last_hover_zone = zone;
    for (uint32_t id = 1u; id <= WM_MAX_WINDOWS; ++id) {
        wm_window_t *window = state->scene.id_table[id];
        if (!window) continue;
        bool close    = window == hit && zone == WM_HIT_CLOSE;
        bool maximize = window == hit && zone == WM_HIT_MAXIMIZE;
        bool minimize = window == hit && zone == WM_HIT_MINIMIZE;
        if (window->hover_close    != close    ||
            window->hover_maximize != maximize ||
            window->hover_minimize != minimize) {
            window->hover_close    = close;
            window->hover_maximize = maximize;
            window->hover_minimize = minimize;
            wm_window_mark_frame_damage(state, window);
        }
    }
    if (!state->input.resizing) update_resize_cursor(state, zone);

    int32_t old_hover = state->launcher_hover_index;
    state->launcher_hover_index = -1;
    if (state->launcher_open) {
        wm_launcher_action_t action = wm_start_menu_hit_test(state,
            (int32_t)state->scene.cursor_x, (int32_t)state->scene.cursor_y);
        if (action.kind == WM_LAUNCHER_ACTION_APP)
            state->launcher_hover_index = (int32_t)action.app_index;
    }
    if (old_hover != state->launcher_hover_index) damage_start_menu(state);

    wm_dialog_set_hover_ok(state,
        state->dialog.active && wm_dialog_ok_contains(state,
            (int32_t)state->scene.cursor_x, (int32_t)state->scene.cursor_y));
    wm_dialog_set_hover_close(state,
        state->dialog.active && wm_dialog_close_contains(state,
            (int32_t)state->scene.cursor_x, (int32_t)state->scene.cursor_y));
}

static void update_taskbar_hover(wm_state_t *state)
{
    wm_taskbar_hit_t hit = wm_taskbar_hit_test(state,
        (int32_t)state->scene.cursor_x, (int32_t)state->scene.cursor_y);
    if (state->taskbar_hover_kind != (uint32_t)hit.kind ||
        state->taskbar_hover_window_id != hit.window_id) {
        state->taskbar_hover_kind = (uint32_t)hit.kind;
        state->taskbar_hover_window_id = hit.window_id;
        damage_taskbar(state);
    }
}

static void begin_pointer_action(wm_state_t *state, wm_window_t *window,
                                 wm_hit_zone_t zone)
{
    state->input.active_window_id = window->id;
    state->input.pointer_start_x  = (int32_t)state->scene.cursor_x;
    state->input.pointer_start_y  = (int32_t)state->scene.cursor_y;
    state->input.frame_start      = window->frame;
    if (zone >= WM_HIT_RESIZE_LEFT) {
        state->input.resizing    = true;
        state->input.resize_zone = zone;
        update_resize_cursor(state, zone);
    } else if (zone == WM_HIT_TITLE && !window->maximized) {
        state->input.dragging = true;
    }
}

static void update_drag(wm_state_t *state)
{
    wm_window_t *window = wm_scene_find(&state->scene, state->input.active_window_id);
    if (!window) return;
    int32_t dx = (int32_t)state->scene.cursor_x - state->input.pointer_start_x;
    int32_t dy = (int32_t)state->scene.cursor_y - state->input.pointer_start_y;
    wm_rect_t frame = state->input.frame_start;
    frame.x += dx;
    frame.y += dy;
    wm_rect_t bounds = wm_display_virtual_bounds(state);
    int32_t min_y = bounds.y + 4;
    if (min_y < 4) min_y = 4;
    if (frame.y < min_y) frame.y = min_y;
#if !WM_TASKBAR_AT_TOP
    int32_t max_y = bounds.y + (int32_t)bounds.h -
        (int32_t)state->theme.title_height - 4;
    if (frame.y > max_y) frame.y = max_y;
#endif
    int32_t right_limit = bounds.x + (int32_t)bounds.w - 48;
    if (frame.x > right_limit) frame.x = right_limit;
    if (frame.x + (int32_t)frame.w < bounds.x + 48)
        frame.x = bounds.x + 48 - (int32_t)frame.w;
    wm_scene_set_frame(state, window, frame);
}

static void update_resize(wm_state_t *state)
{
    wm_window_t *window = wm_scene_find(&state->scene, state->input.active_window_id);
    if (!window) return;
    int32_t dx = (int32_t)state->scene.cursor_x - state->input.pointer_start_x;
    int32_t dy = (int32_t)state->scene.cursor_y - state->input.pointer_start_y;
    wm_rect_t frame = state->input.frame_start;
    int32_t right  = frame.x + (int32_t)frame.w;
    int32_t bottom = frame.y + (int32_t)state->theme.title_height + (int32_t)frame.h;
    wm_hit_zone_t zone = state->input.resize_zone;
    bool left        = zone==WM_HIT_RESIZE_LEFT||zone==WM_HIT_RESIZE_TOP_LEFT||zone==WM_HIT_RESIZE_BOTTOM_LEFT;
    bool right_edge  = zone==WM_HIT_RESIZE_RIGHT||zone==WM_HIT_RESIZE_TOP_RIGHT||zone==WM_HIT_RESIZE_BOTTOM_RIGHT;
    bool top         = zone==WM_HIT_RESIZE_TOP||zone==WM_HIT_RESIZE_TOP_LEFT||zone==WM_HIT_RESIZE_TOP_RIGHT;
    bool bottom_edge = zone==WM_HIT_RESIZE_BOTTOM||zone==WM_HIT_RESIZE_BOTTOM_LEFT||zone==WM_HIT_RESIZE_BOTTOM_RIGHT;
    if (left) {
        int32_t new_x = frame.x + dx;
        int32_t new_w = right - new_x;
        if (new_w >= (int32_t)WM_MIN_WINDOW_WIDTH) { frame.x = new_x; frame.w = (uint32_t)new_w; }
    }
    if (right_edge) {
        int32_t new_w = (int32_t)frame.w + dx;
        if (new_w >= (int32_t)WM_MIN_WINDOW_WIDTH) frame.w = (uint32_t)new_w;
    }
    if (top) {
        int32_t new_y = frame.y + dy;
        int32_t new_h = bottom - new_y - (int32_t)state->theme.title_height;
        if (new_h >= (int32_t)WM_MIN_WINDOW_HEIGHT) { frame.y = new_y; frame.h = (uint32_t)new_h; }
    }
    if (bottom_edge) {
        int32_t new_h = (int32_t)frame.h + dy;
        if (new_h >= (int32_t)WM_MIN_WINDOW_HEIGHT) frame.h = (uint32_t)new_h;
    }
    wm_scene_set_frame(state, window, frame);
}

static bool handle_taskbar_click(wm_state_t *state)
{
    wm_taskbar_hit_t hit = wm_taskbar_hit_test(state,
        (int32_t)state->scene.cursor_x, (int32_t)state->scene.cursor_y);
    if (hit.kind == WM_TASKBAR_HIT_NONE) return false;
    if (hit.kind == WM_TASKBAR_HIT_LAUNCHER) {
        state->launcher_open = !state->launcher_open;
        if (state->launcher_open) {
            wm_notification_close_center(state);
            wm_start_menu_clamp_scroll(state);
            state->search_active = true;
        } else {
            state->search_len = 0u;
            state->search_text[0] = '\0';
            state->search_active = false;
        }
        damage_ui(state);
    } else if (hit.kind == WM_TASKBAR_HIT_NOTIFICATION ||
               hit.kind == WM_TASKBAR_HIT_CLOCK) {
        state->launcher_open = false;
        state->search_len = 0u;
        state->search_text[0] = '\0';
        close_wifi_panel(state);
        wm_notification_toggle_center(state);
        damage_ui(state);
    } else if (hit.kind == WM_TASKBAR_HIT_NETWORK) {
        state->launcher_open = false;
        state->search_len = 0u;
        state->search_text[0] = '\0';
        wm_notification_close_center(state);
        bool opening = !state->wifi_panel.open;
        close_wifi_panel(state);
        state->wifi_panel.open = opening;
        if (opening) {
            /* Force an immediate status/results refresh rather than
             * waiting for wm_wifi_panel_poll()'s next tick, and kick a
             * scan so the list isn't empty the first time this opens. */
            state->wifi_panel.last_status_poll_ms = 0u;
            state->wifi_panel.last_results_poll_ms = 0u;
            perform_wifi_action(state, (wm_wifi_action_t){WM_WIFI_ACTION_SCAN, 0u});
        }
        damage_ui(state);
    } else if (hit.kind == WM_TASKBAR_HIT_PIN) {
        if (hit.index < state->assets.app_count &&
            state->assets.apps[hit.index].path[0]) {
            process_spawn(state->assets.apps[hit.index].path);
        }
        state->launcher_open = false;
        state->search_len = 0u;
        state->search_text[0] = '\0';
        state->search_active = false;
        wm_notification_close_center(state);
        close_wifi_panel(state);
        damage_ui(state);
    } else if (hit.kind == WM_TASKBAR_HIT_AUDIO ||
               hit.kind == WM_TASKBAR_HIT_IME) {
        /* Audio + IME trays are stubs today. Close any open shell surface
         * and swallow the click; the hit kinds exist so a future mixer /
         * input-method panel can hang off them without touching layout. */
        state->launcher_open = false;
        state->search_len = 0u;
        state->search_text[0] = '\0';
        state->search_active = false;
        wm_notification_close_center(state);
        close_wifi_panel(state);
        damage_ui(state);
    } else if (hit.kind == WM_TASKBAR_HIT_WINDOW) {
        wm_window_t *window = wm_scene_find(&state->scene, hit.window_id);
        if (!window) return true;
        if (window->minimized || !window->visible) {
            wm_scene_restore(state, window);
        } else if (window->has_focus) {
            wm_scene_minimize(state, window);
        } else {
            wm_scene_raise(state, window);
            wm_scene_focus(state, window);
        }
        damage_ui(state);
    }
    return true;
}

static bool handle_notification_center_click(wm_state_t *state)
{
    if (!state->notification_center_open) return false;
    if (wm_notification_center_contains(state,
            (int32_t)state->scene.cursor_x, (int32_t)state->scene.cursor_y))
        return true;
    wm_notification_close_center(state);
    damage_ui(state);
    return false;
}

/* Applies one wm_wifi_action_t -- the only place that actually calls
 * wifi_scan_start()/wifi_connect()/wifi_disconnect() (Syscalls.c ->
 * SYSCALL_WIFI_*). Shared between the mouse click handler below and the
 * password field's Enter-key submit in wm_input_handle_keyboard(). */
static void perform_wifi_action(wm_state_t *state, wm_wifi_action_t action)
{
    wm_wifi_panel_t *p = &state->wifi_panel;
    switch (action.kind) {
        case WM_WIFI_ACTION_SCAN:
            if (wifi_scan_start()) {
                extern uint64_t get_uptime_ms(void);
                p->scan_active = true;
                p->scan_started_ms = get_uptime_ms();
                p->result_count = 0u;
                p->scroll = 0u;
            }
            damage_wifi_panel(state);
            break;
        case WM_WIFI_ACTION_CONNECT_OPEN:
            if (action.index < p->result_count) {
                (void)wifi_connect(p->results[action.index].ssid, NULL);
                p->last_status_poll_ms = 0u; /* force an immediate status refresh */
            }
            damage_wifi_panel(state);
            break;
        case WM_WIFI_ACTION_SELECT:
            if (action.index < p->result_count) {
                p->selected_index = (int32_t)action.index;
                p->password_active = true;
                p->password_len = 0u;
                p->password[0] = '\0';
            }
            damage_wifi_panel(state);
            break;
        case WM_WIFI_ACTION_CONNECT_PSK:
            if (p->selected_index >= 0 && (uint32_t)p->selected_index < p->result_count &&
                p->password_len >= 8u) {
                (void)wifi_connect(p->results[p->selected_index].ssid, p->password);
                p->last_status_poll_ms = 0u;
                p->selected_index = -1;
                p->password_active = false;
                p->password_len = 0u;
                p->password[0] = '\0';
            }
            damage_wifi_panel(state);
            break;
        case WM_WIFI_ACTION_BACK:
            p->selected_index = -1;
            p->password_active = false;
            p->password_len = 0u;
            p->password[0] = '\0';
            damage_wifi_panel(state);
            break;
        case WM_WIFI_ACTION_DISCONNECT:
            wifi_disconnect();
            p->last_status_poll_ms = 0u;
            damage_wifi_panel(state);
            break;
        case WM_WIFI_ACTION_NONE:
        default:
            break;
    }
}

static bool handle_wifi_panel_click(wm_state_t *state)
{
    if (!state->wifi_panel.open) return false;
    wm_wifi_action_t action = wm_wifi_panel_hit_test(state,
        (int32_t)state->scene.cursor_x, (int32_t)state->scene.cursor_y);
    if (action.kind != WM_WIFI_ACTION_NONE) {
        perform_wifi_action(state, action);
        return true;
    }
    if (!wm_wifi_panel_contains(state,
            (int32_t)state->scene.cursor_x, (int32_t)state->scene.cursor_y)) {
        close_wifi_panel(state);
        damage_ui(state);
    }
    return false;
}

static bool handle_launcher_click(wm_state_t *state)
{
    if (!state->launcher_open) return false;
    wm_launcher_action_t action = wm_start_menu_hit_test(state,
        (int32_t)state->scene.cursor_x, (int32_t)state->scene.cursor_y);
    if (action.kind == WM_LAUNCHER_ACTION_APP &&
        action.app_index < state->assets.app_count) {
        process_spawn(state->assets.apps[action.app_index].path);
        state->launcher_open = false;
        state->search_len = 0u;
        state->search_text[0] = '\0';
        damage_ui(state);
        return true;
    }
    if (action.kind == WM_LAUNCHER_ACTION_REBOOT)  { system_reboot();   return true; }
    if (action.kind == WM_LAUNCHER_ACTION_SHUTDOWN) { system_shutdown(); return true; }
    wm_rect_t menu = wm_start_menu_rect(state);
    if (!wm_rect_intersects((wm_rect_t){(int32_t)state->scene.cursor_x,
                                         (int32_t)state->scene.cursor_y, 1u, 1u}, menu)) {
        state->launcher_open = false;
        state->search_len = 0u;
        state->search_text[0] = '\0';
        damage_ui(state);
    }
    return true;
}

static void handle_window_click(wm_state_t *state, uint64_t now_ms)
{
    wm_hit_zone_t zone = WM_HIT_NONE;
    wm_window_t *window = wm_scene_hit_test(state,
        (int32_t)state->scene.cursor_x, (int32_t)state->scene.cursor_y, &zone);
    if (!window) {
        const char *target = wm_desktop_hit_test(state,
            (int32_t)state->scene.cursor_x, (int32_t)state->scene.cursor_y);
        if (target) process_spawn(target);
        return;
    }
    wm_scene_raise(state, window);
    wm_scene_focus(state, window);
    if (zone == WM_HIT_CLOSE)    { wm_scene_destroy_window(state, window->id); return; }
    if (zone == WM_HIT_MAXIMIZE) { toggle_maximize(state, window); return; }
    if (zone == WM_HIT_MINIMIZE) { wm_scene_minimize(state, window); return; }
    if (zone == WM_HIT_TITLE &&
        state->input.last_title_click_window == window->id &&
        now_ms - state->input.last_title_click_ms <= 350u) {
        toggle_maximize(state, window);
        state->input.last_title_click_window = 0u;
        state->input.last_title_click_ms     = 0u;
        return;
    }
    if (zone == WM_HIT_TITLE) {
        state->input.last_title_click_window = window->id;
        state->input.last_title_click_ms     = now_ms;
    }
    begin_pointer_action(state, window, zone);
}

static void handle_mouse_event(wm_state_t *state,
                               const input_mouse_event_t *event,
                               bool absolute_position,
                               uint32_t absolute_x,
                               uint32_t absolute_y,
                               uint64_t sequence)
{
    if (!state || !event) return;
    if (sequence != 0u &&
        sequence <= state->input.last_kernel_pointer_sequence) {
        return;
    }

    int32_t x;
    int32_t y;
    if (absolute_position) {
        x = (int32_t)absolute_x;
        y = (int32_t)absolute_y;
    } else {
        int16_t delta_x = (int16_t)event->x;
        int16_t delta_y = (int16_t)event->y;
        x = (int32_t)state->scene.cursor_x + delta_x;
        y = (int32_t)state->scene.cursor_y + delta_y;
    }
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= (int32_t)state->compositor.framebuffer_width)
        x = (int32_t)state->compositor.framebuffer_width - 1;
    if (y >= (int32_t)state->compositor.framebuffer_height)
        y = (int32_t)state->compositor.framebuffer_height - 1;
    uint32_t old_x = state->scene.cursor_x;
    uint32_t old_y = state->scene.cursor_y;
    bool old_visible = state->scene.cursor_visible;
    state->scene.cursor_x = (uint32_t)x;
    state->scene.cursor_y = (uint32_t)y;
    state->scene.cursor_visible = true;
    if (!old_visible || old_x != state->scene.cursor_x ||
        old_y != state->scene.cursor_y) {
        state->compositor.next_frame_ms = 0u;
    }
    if (sequence != 0u) {
        state->input.last_kernel_pointer_sequence = sequence;
    }

    uint8_t buttons  = event->buttons;
    uint8_t previous = state->input.previous_buttons;
    bool left_down   = (buttons & 1u) != 0u && (previous & 1u) == 0u;
    bool left_held   = (buttons & 1u) != 0u;
    bool left_up     = (buttons & 1u) == 0u && (previous & 1u) != 0u;
    state->input.previous_buttons = buttons;

    if (state->input.dragging && left_held) {
        extern uint64_t get_uptime_ms(void);
        uint64_t now_ms = get_uptime_ms();
        if (now_ms - state->input.last_pointer_frame_ms >= WM_POINTER_FRAME_THROTTLE_MS) {
            state->input.last_pointer_frame_ms = now_ms;
            update_drag(state);
        }
        route_mouse(state, event);
        return;
    }
    if (wm_dialog_dragging(state) && left_held) {
        extern uint64_t get_uptime_ms(void);
        uint64_t now_ms = get_uptime_ms();
        if (now_ms - state->input.last_pointer_frame_ms >= WM_POINTER_FRAME_THROTTLE_MS) {
            state->input.last_pointer_frame_ms = now_ms;
            int32_t dx = (int32_t)state->scene.cursor_x - state->dialog.drag_start_x;
            int32_t dy = (int32_t)state->scene.cursor_y - state->dialog.drag_start_y;
            wm_dialog_move(state, state->dialog.drag_origin_x + dx,
                           state->dialog.drag_origin_y + dy);
        }
        return;
    }
    if (state->input.resizing && left_held) {
        extern uint64_t get_uptime_ms(void);
        uint64_t now_ms = get_uptime_ms();
        if (now_ms - state->input.last_pointer_frame_ms >= WM_POINTER_FRAME_THROTTLE_MS) {
            state->input.last_pointer_frame_ms = now_ms;
            update_resize(state);
        }
        route_mouse(state, event);
        return;
    }

    update_taskbar_hover(state);
    update_hover(state);

    bool wheel_up   = (buttons & 0x08u) != 0u && (previous & 0x08u) == 0u;
    bool wheel_down = (buttons & 0x10u) != 0u && (previous & 0x10u) == 0u;
    if (wheel_up || wheel_down) {
        int32_t rows = wheel_down ? 1 : -1;
        if (state->launcher_open && wm_start_menu_scroll(state, rows)) {
            damage_ui(state); return;
        }
        if (state->notification_center_open &&
            wm_notification_center_scroll(state, rows)) {
            damage_ui(state); return;
        }
        if (state->wifi_panel.open && wm_wifi_panel_scroll(state, rows)) {
            damage_wifi_panel(state); return;
        }
    }
    if (left_up) {
        wm_window_t *window = wm_scene_find(&state->scene, state->input.active_window_id);
        if (state->input.dragging && window) snap_window(state, window);
        state->input.dragging        = false;
        state->input.resizing        = false;
        state->input.active_window_id = 0u;
        state->scene.cursor_style    = WM_CURSOR_DEFAULT;
        wm_dialog_set_dragging(state, false);
    }
    bool consumed = false;
    if (left_down) {
        extern uint64_t get_uptime_ms(void);
        state->input.last_pointer_frame_ms = get_uptime_ms();
        if (state->dialog.active) {
            if (wm_dialog_close_contains(state, (int32_t)state->scene.cursor_x,
                                         (int32_t)state->scene.cursor_y)) {
                wm_dialog_close(state);
            } else if (wm_dialog_ok_contains(state, (int32_t)state->scene.cursor_x,
                                            (int32_t)state->scene.cursor_y)) {
                wm_dialog_close(state);
            } else if (wm_dialog_title_contains(state, (int32_t)state->scene.cursor_x,
                                               (int32_t)state->scene.cursor_y)) {
                wm_dialog_set_dragging(state, true);
                state->dialog.drag_start_x = (int32_t)state->scene.cursor_x;
                state->dialog.drag_start_y = (int32_t)state->scene.cursor_y;
                state->dialog.drag_origin_x = state->dialog.x;
                state->dialog.drag_origin_y = state->dialog.y;
            } else if (!wm_dialog_contains(state, (int32_t)state->scene.cursor_x,
                                          (int32_t)state->scene.cursor_y)) {
                wm_dialog_close(state);
            }
            consumed = true;
        } else if (handle_taskbar_click(state)) {
            consumed = true;
        } else if (handle_notification_center_click(state)) {
            consumed = true;
        } else if (handle_wifi_panel_click(state)) {
            consumed = true;
        } else if (handle_launcher_click(state)) {
            consumed = true;
        } else {
            handle_window_click(state, get_uptime_ms());
        }
    }
    if (!consumed && !state->launcher_open && !state->notification_center_open &&
        !state->wifi_panel.open)
        route_mouse(state, event);
}

void wm_input_handle_mouse(wm_state_t *state, const ipc_message_t *message)
{
    if (!state || !message ||
        message->size < sizeof(wm_msg_header_t) + sizeof(input_mouse_event_t)) return;
    const input_mouse_event_t *event =
        (const input_mouse_event_t *)(message->data + sizeof(wm_msg_header_t));

    if (message->size >= sizeof(wm_mouse_event_message_t)) {
        const wm_mouse_event_message_t *ext =
            (const wm_mouse_event_message_t *)message->data;
        handle_mouse_event(state, &ext->event, true,
                           ext->absolute_x, ext->absolute_y,
                           ext->sequence);
        return;
    }

    handle_mouse_event(state, event, false, 0u, 0u, 0u);
}

static int32_t clamp_delta(int32_t value)
{
    if (value > 32767)  return 32767;
    if (value < -32768) return -32768;
    return value;
}

bool wm_input_poll_raw_mouse(wm_state_t *state)
{
    if (!state) return false;

    /* Coalesce every currently-queued raw delta into a single event before
     * running hit-test/hover/drag logic once, rather than once per micro
     * delta -- mirrors the coalescing the kernel used to do before handing
     * off a pointer snapshot. */
    input_mouse_event_t raw;
    input_mouse_event_t accum;
    memset(&accum, 0, sizeof(accum));
    int32_t dx = 0;
    int32_t dy = 0;
    uint32_t count = 0u;
    while (input_read_mouse(&raw) > 0) {
        dx += (int16_t)raw.x;
        dy += (int16_t)raw.y;
        accum.buttons = raw.buttons;
        accum.wheel   = raw.wheel;
        ++count;
    }
    if (count == 0u) return false;

    accum.x = (uint16_t)(int16_t)clamp_delta(dx);
    accum.y = (uint16_t)(int16_t)clamp_delta(dy);
    handle_mouse_event(state, &accum, false, 0u, 0u, 0u);
    return true;
}

bool wm_input_poll_raw_keyboard(wm_state_t *state)
{
    if (!state) return false;

    bool consumed_any = false;
    input_keyboard_event_t event;
    while (input_read_keyboard(&event) > 0) {
        consumed_any = true;
        struct {
            wm_msg_header_t header;
            input_keyboard_event_t event;
        } payload;
        memset(&payload, 0, sizeof(payload));
        payload.header.type = WM_KEYBOARD_EVENT;
        payload.event = event;

        ipc_message_t message;
        message.sender_pid = -1;
        message.size = (uint32_t)sizeof(payload);
        memcpy(message.data, &payload, sizeof(payload));

        wm_input_handle_keyboard(state, &message);
    }
    return consumed_any;
}
