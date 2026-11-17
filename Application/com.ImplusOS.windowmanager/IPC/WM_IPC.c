#include "WM_IPC.h"

#include "../Compositor/WM_Compositor.h"
#include "../Compositor/WM_Damage.h"
#include "../Compositor/WM_Raster.h"
#include "../Input/WM_Input.h"
#include "../SceneGraph/WM_Node.h"
#include "../UI/WM_Notification.h"
#include "../../../../Userland/Source/Syscalls.h"

#include <stdlib.h>
#include <string.h>

static bool message_has(const ipc_message_t *message, size_t bytes)
{
    return message && message->size >= bytes;
}

static wm_window_t *owned_window(wm_state_t *state,
                                 const ipc_message_t *message,
                                 const wm_msg_header_t *header)
{
    wm_window_t *window = wm_scene_find(&state->scene, header->window_id);
    if (!window || window->owner_pid != message->sender_pid) return NULL;
    return window;
}

static void send_status(const ipc_message_t *message, const wm_msg_header_t *request,
                        uint32_t type, int32_t status)
{
    struct {
        wm_msg_header_t header;
        int32_t status;
    } response;
    memset(&response, 0, sizeof(response));
    response.header.type = type;
    response.header.request_id = request->request_id;
    response.header.window_id = request->window_id;
    response.status = status;
    ipc_send_message(message->sender_pid, &response, sizeof(response));
}

static void handle_create(wm_state_t *state, const ipc_message_t *message,
                          const wm_msg_header_t *header)
{
    struct create_message {
        wm_msg_header_t header;
        uint32_t width, height, x, y, background;
        char title[64];
    };
    if (!message_has(message, sizeof(struct create_message))) {
        send_status(message, header, WM_WINDOW_CREATED, WM_STATUS_INVALID_ARG);
        return;
    }
    const struct create_message *command = (const void *)message->data;
    int32_t id = wm_scene_create_window(state, message->sender_pid,
        (wm_rect_t){(int32_t)command->x, (int32_t)command->y,
                    command->width, command->height},
        command->background, command->title);
    struct {
        wm_msg_header_t header;
        int32_t window_id;
    } response;
    memset(&response, 0, sizeof(response));
    response.header.type = WM_WINDOW_CREATED;
    response.header.request_id = header->request_id;
    response.header.window_id = id > 0 ? (uint32_t)id : 0u;
    response.window_id = id;
    ipc_send_message(message->sender_pid, &response, sizeof(response));
}

void wm_ipc_handle_message(wm_state_t *state, const ipc_message_t *message)
{
    if (!state || !message || !message_has(message, sizeof(wm_msg_header_t))) return;
    const wm_msg_header_t *header = (const wm_msg_header_t *)message->data;
    wm_window_t *window;

    switch (header->type) {
    case WM_CREATE_WINDOW:
        handle_create(state, message, header);
        break;
    case WM_DESTROY_WINDOW:
        window = owned_window(state, message, header);
        if (window) {
            /* The owner itself is closing it: no close request goes back. */
            window->owner_destroying = true;
            wm_scene_destroy_window(state, window->id);
        }
        break;
    case WM_SET_WINDOW_RECT: {
        struct set_frame_message {
            wm_msg_header_t header;
            uint32_t x, y, width, height;
        };
        if (!message_has(message, sizeof(struct set_frame_message))) break;
        window = owned_window(state, message, header);
        if (!window) break;
        const struct set_frame_message *command = (const void *)message->data;
        wm_scene_set_frame(state, window,
            (wm_rect_t){(int32_t)command->x, (int32_t)command->y,
                        command->width, command->height});
        break;
    }
    case WM_SHOW_WINDOW:
        window = owned_window(state, message, header);
        if (window) wm_scene_show(state, window);
        break;
    case WM_RAISE_WINDOW:
        window = owned_window(state, message, header);
        if (window) wm_scene_raise(state, window);
        break;
    case WM_SET_FOCUS:
        window = owned_window(state, message, header);
        if (window) {
            if (window->minimized) wm_scene_restore(state, window);
            else wm_scene_focus(state, window);
        }
        break;
    case WM_GET_WINDOW_RECT: {
        struct {
            wm_msg_header_t header;
            int32_t status;
            uint32_t x, y, width, height;
        } response;
        memset(&response, 0, sizeof(response));
        response.header.type = WM_GET_WINDOW_RECT;
        response.header.request_id = header->request_id;
        response.header.window_id = header->window_id;
        window = owned_window(state, message, header);
        response.status = window ? WM_STATUS_OK : WM_STATUS_NOT_FOUND;
        if (window) {
            response.x = (uint32_t)window->frame.x;
            response.y = (uint32_t)window->frame.y;
            response.width = window->frame.w;
            response.height = window->frame.h;
        }
        ipc_send_message(message->sender_pid, &response, sizeof(response));
        break;
    }
    case WM_DAMAGE: {
        struct damage_message {
            wm_msg_header_t header;
            uint32_t x, y, width, height;
        };
        if (!message_has(message, sizeof(struct damage_message))) break;
        window = owned_window(state, message, header);
        if (!window) break;
        const struct damage_message *command = (const void *)message->data;
        wm_window_damage_content(state, window,
            (wm_rect_t){(int32_t)command->x, (int32_t)command->y,
                        command->width, command->height});
        break;
    }
    case WM_SUBSCRIBE_INPUT: {
        if (!message_has(message, sizeof(wm_msg_header_t) + sizeof(uint32_t))) break;
        window = owned_window(state, message, header);
        if (!window) break;
        uint32_t types = 0u;
        memcpy(&types, message->data + sizeof(wm_msg_header_t), sizeof(types));
        for (uint32_t i = 0; i < state->scene.input_sub_count; ++i) {
            wm_input_subscription_t *subscription = &state->scene.input_subs[i];
            if (subscription->subscriber_pid == message->sender_pid &&
                subscription->window_id == header->window_id) {
                subscription->input_types |= types;
                return;
            }
        }
        if (state->scene.input_sub_count < WM_MAX_INPUT_SUBSCRIPTIONS) {
            wm_input_subscription_t *subscription =
                &state->scene.input_subs[state->scene.input_sub_count++];
            subscription->subscriber_pid = message->sender_pid;
            subscription->window_id = header->window_id;
            subscription->input_types = types;
        }
        break;
    }
    case WM_SET_WINDOW_SYSTEM: {
        struct system_message {
            wm_msg_header_t header;
            bool system;
        };
        if (!message_has(message, sizeof(struct system_message))) break;
        window = owned_window(state, message, header);
        if (window)
            wm_scene_set_system(state, window,
                ((const struct system_message *)message->data)->system);
        break;
    }
    case WM_SET_WINDOW_TITLE: {
        if (!message_has(message, sizeof(wm_set_title_t))) break;
        window = owned_window(state, message, header);
        if (!window) break;
        const wm_set_title_t *command = (const void *)message->data;
        char title[WM_TITLE_MAX];
        size_t n = 0u;
        while (n < WM_SET_TITLE_MAX && command->title[n] != '\0') ++n;
        /* Cut on a UTF-8 character boundary, not in the middle of one. */
        if (n > WM_TITLE_MAX - 1u) {
            n = WM_TITLE_MAX - 1u;
            while (n > 0u && ((uint8_t)command->title[n] & 0xC0u) == 0x80u) --n;
        }
        memcpy(title, command->title, n);
        title[n] = '\0';
        if (strcmp(window->title, title) != 0) {
            memcpy(window->title, title, n + 1u);
            wm_window_mark_frame_damage(state, window);
            wm_compositor_damage_all(state); /* the taskbar shows it too */
        }
        break;
    }
    case WM_SET_WINDOW_CLOSE_POLICY: {
        struct close_policy_message {
            wm_msg_header_t header;
            bool owner_handles;
        };
        if (!message_has(message, sizeof(struct close_policy_message))) break;
        window = owned_window(state, message, header);
        if (window)
            window->owner_handles_close =
                ((const struct close_policy_message *)message->data)->owner_handles;
        break;
    }
    case WM_GET_CONTENT_RECT: {
        struct {
            wm_msg_header_t header;
            int32_t status;
            uint32_t x, y, width, height;
        } response;
        memset(&response, 0, sizeof(response));
        response.header.type = WM_GET_CONTENT_RECT;
        response.header.request_id = header->request_id;
        response.header.window_id = header->window_id;
        window = owned_window(state, message, header);
        response.status = window ? WM_STATUS_OK : WM_STATUS_NOT_FOUND;
        if (window) {
            uint32_t title_h = window->is_system ? 0u : state->theme.title_height;
            response.x = (uint32_t)window->frame.x;
            response.y = (uint32_t)(window->frame.y + (int32_t)title_h);
            response.width = window->frame.w;
            response.height = window->frame.h;
        }
        ipc_send_message(message->sender_pid, &response, sizeof(response));
        break;
    }
    case WM_SET_WINDOW_SURFACE_OPAQUE: {
        struct opaque_message {
            wm_msg_header_t header;
            bool opaque;
        };
        if (!message_has(message, sizeof(struct opaque_message))) break;
        window = owned_window(state, message, header);
        if (!window) break;
        bool opaque = ((const struct opaque_message *)message->data)->opaque;
        if (window->surface_opaque != opaque) {
            window->surface_opaque = opaque;
            wm_window_mark_frame_damage(state, window);
        }
        break;
    }
    case WM_SHOW_NOTIFICATION: {
        struct notification_message {
            wm_msg_header_t header;
            char title[64];
            char message[128];
        };
        if (!message_has(message, sizeof(struct notification_message))) break;
        const struct notification_message *command = (const void *)message->data;
        char title[64];
        char body[128];
        memcpy(title, command->title, sizeof(title));
        memcpy(body, command->message, sizeof(body));
        title[sizeof(title) - 1u] = '\0';
        body[sizeof(body) - 1u] = '\0';
        wm_notification_add(state, title, body);
        break;
    }
    case WM_GET_BACKING_STORE: {
        wm_backing_store_response_t response;
        memset(&response, 0, sizeof(response));
        response.header.type = WM_BACKING_STORE_READY;
        response.header.request_id = header->request_id;
        response.header.window_id = header->window_id;
        window = owned_window(state, message, header);
        if (!window || window->surface_shared_memory_handle <= 0) {
            response.status = window ? WM_STATUS_UNSUPPORTED :
                                       WM_STATUS_NOT_FOUND;
        } else if (os_shared_memory_grant(
                       window->surface_shared_memory_handle,
                       message->sender_pid) < 0) {
            response.status = WM_STATUS_DENIED;
        } else {
            response.status = WM_STATUS_OK;
            response.shared_memory_handle =
                window->surface_shared_memory_handle;
            response.width = window->frame.w;
            response.height = window->frame.h;
            response.size_bytes = window->surface_bytes;
        }
        ipc_send_message(message->sender_pid, &response, sizeof(response));
        break;
    }
    default:
        break;
    }
}
