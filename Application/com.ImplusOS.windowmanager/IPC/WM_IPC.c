#include "WM_IPC.h"

#include "../Compositor/WM_Compositor.h"
#include "../Compositor/WM_Damage.h"
#include "../Compositor/WM_Raster.h"
#include "../Core/WM_Assets.h"
#include "../Font/WM_Font.h"
#include "../Input/WM_Input.h"
#include "../SceneGraph/WM_Node.h"
#include "../Theme/WM_Theme.h"
#include "../UI/WM_Notification.h"
#include "../UI/WM_Dialog.h"
#include "../../../../Userland/API/Source/XMLParser.h"
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

static uint32_t parse_unsigned(const char *text, uint32_t fallback)
{
    if (!text || !*text) return fallback;
    uint64_t value = 0u;
    while (*text >= '0' && *text <= '9') {
        value = value * 10u + (uint64_t)(*text - '0');
        if (value > UINT32_MAX) return fallback;
        ++text;
    }
    return (uint32_t)value;
}

static float parse_float_value(const char *text, float fallback)
{
    if (!text || !*text) return fallback;
    uint32_t whole = parse_unsigned(text, 0u);
    while (*text >= '0' && *text <= '9') ++text;
    if (*text != '.') return (float)whole;
    ++text;
    uint32_t fraction = 0u;
    uint32_t divisor = 1u;
    while (*text >= '0' && *text <= '9' && divisor < 10000u) {
        fraction = fraction * 10u + (uint32_t)(*text - '0');
        divisor *= 10u;
        ++text;
    }
    return (float)whole + (float)fraction / (float)divisor;
}

static void surface_clear(wm_window_t *window)
{
    if (!window || !window->surface) return;
    uint32_t w = window->frame.w;
    uint32_t h = window->frame.h;
    uint32_t color = window->bg_color;
    for (uint32_t col = 0u; col < w; ++col) window->surface[col] = color;
    for (uint32_t row = 1u; row < h; ++row)
        memcpy(&window->surface[row * w], &window->surface[0], (size_t)w * sizeof(uint32_t));
}

static void render_xml_node(wm_state_t *state, wm_window_t *window,
                            wm_canvas_t *canvas, const xml_node_t *node,
                            int32_t parent_x, int32_t parent_y)
{
    if (!state || !window || !canvas || !node) return;
    const char *x_text = xml_get_attr(node, "x");
    const char *y_text = xml_get_attr(node, "y");
    const char *w_text = xml_get_attr(node, "width");
    const char *h_text = xml_get_attr(node, "height");
    const char *color_text = xml_get_attr(node, "color");
    const char *background_text = xml_get_attr(node, "bgColor");
    const char *font_text = xml_get_attr(node, "fontSize");
    int32_t x = parent_x + (int32_t)parse_unsigned(x_text, 0u);
    int32_t y = parent_y + (int32_t)parse_unsigned(y_text, 0u);
    uint32_t width = parse_unsigned(w_text,
        x < (int32_t)window->frame.w ? window->frame.w - (uint32_t)x : 0u);
    uint32_t height = parse_unsigned(h_text, 24u);
    uint32_t color = wm_theme_parse_color(color_text, state->theme.text);
    uint32_t background = wm_theme_parse_color(background_text, 0x00000000u);
    float font_size = parse_float_value(font_text, state->theme.font_normal);
    bool is_panel = strcmp(node->tag, "Panel") == 0;
    bool is_button = strcmp(node->tag, "Button") == 0;
    bool is_rect = strcmp(node->tag, "Rect") == 0;
    bool is_label = strcmp(node->tag, "Label") == 0;

    wm_rect_t rect = {x, y, width, height};
    if (is_panel)
        wm_canvas_fill_rounded(canvas, rect, 8u, background);
    else if (is_button)
        wm_canvas_fill_rounded(canvas, rect, 7u,
            background ? background : state->theme.accent_soft);
    else if (is_rect)
        wm_canvas_fill(canvas, rect, background);

    if ((is_label || is_button) && node->text[0]) {
        int32_t text_x = x;
        int32_t text_y = y;
        if (is_button) {
            uint32_t text_width = wm_font_measure(&state->font, node->text, font_size);
            if (text_width < width) text_x += (int32_t)(width - text_width) / 2;
            if ((uint32_t)font_size < height)
                text_y += (int32_t)(height - (uint32_t)font_size) / 2;
        }
        wm_font_draw(&state->font, canvas, text_x, text_y,
                     node->text, color, font_size, width);
    }
    for (uint32_t i = 0; i < node->child_count; ++i)
        render_xml_node(state, window, canvas, node->children[i], x, y);
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

static void handle_draw_rect(wm_state_t *state, const ipc_message_t *message,
                             const wm_msg_header_t *header)
{
    struct draw_rect_message {
        wm_msg_header_t header;
        uint32_t x, y, width, height, color;
    };
    if (!message_has(message, sizeof(struct draw_rect_message))) return;
    const struct draw_rect_message *command = (const void *)message->data;
    wm_window_t *window = owned_window(state, message, header);
    if (!window) return;
    wm_canvas_t canvas;
    wm_canvas_init(&canvas, window->surface, window->frame.w,
                   window->frame.h, window->frame.w);
    wm_rect_t rect = {(int32_t)command->x, (int32_t)command->y,
                      command->width, command->height};
    if (command->x == 0u && command->y == 0u &&
        command->width >= window->frame.w && command->height >= window->frame.h) {
        window->bg_color = command->color | 0xFF000000u;
        surface_clear(window);
        rect = (wm_rect_t){0, 0, window->frame.w, window->frame.h};
    } else {
        wm_canvas_fill(&canvas, rect, command->color);
    }
    wm_window_damage_content(state, window, rect);
}

static void handle_draw_text(wm_state_t *state, const ipc_message_t *message,
                             const wm_msg_header_t *header)
{
    struct draw_text_message {
        wm_msg_header_t header;
        uint32_t x, y, color;
        float font_size;
        char text[128];
    };
    if (!message_has(message, sizeof(struct draw_text_message))) return;
    const struct draw_text_message *command = (const void *)message->data;
    wm_window_t *window = owned_window(state, message, header);
    if (!window) return;
    wm_canvas_t canvas;
    wm_canvas_init(&canvas, window->surface, window->frame.w,
                   window->frame.h, window->frame.w);
    char text[128];
    memcpy(text, command->text, sizeof(text));
    text[sizeof(text) - 1u] = '\0';
    uint32_t max_width = command->x < window->frame.w ?
        window->frame.w - command->x : 0u;
    wm_font_draw(&state->font, &canvas, (int32_t)command->x, (int32_t)command->y,
                 text, command->color, command->font_size, max_width);
    uint32_t text_width = wm_font_measure(&state->font,
                                          text, command->font_size);
    wm_window_damage_content(state, window,
        (wm_rect_t){(int32_t)command->x, (int32_t)command->y,
                    wm_min_u32(text_width + 3u, max_width),
                    (uint32_t)command->font_size + 5u});
}

static void handle_xml(wm_state_t *state, const ipc_message_t *message,
                       const wm_msg_header_t *header)
{
    wm_window_t *window = owned_window(state, message, header);
    if (!window) return;
    if (header->type == WM_SET_LAYOUT_XML_START) {
        struct start_message {
            wm_msg_header_t header;
            uint32_t total_size;
        };
        if (!message_has(message, sizeof(struct start_message))) return;
        const struct start_message *command = (const void *)message->data;
        free(window->xml_buffer);
        window->xml_buffer = NULL;
        window->xml_size = 0u;
        window->xml_capacity = 0u;
        if (command->total_size == 0u || command->total_size > WM_XML_MAX_BYTES) return;
        window->xml_buffer = (char *)malloc((size_t)command->total_size + 1u);
        if (window->xml_buffer) {
            window->xml_capacity = command->total_size + 1u;
            window->xml_buffer[0] = '\0';
        }
    } else if (header->type == WM_SET_LAYOUT_XML_CHUNK) {
        if (!window->xml_buffer || message->size <= sizeof(wm_msg_header_t)) return;
        uint32_t chunk_size = message->size - (uint32_t)sizeof(wm_msg_header_t);
        if (chunk_size >= window->xml_capacity - window->xml_size) {
            free(window->xml_buffer);
            window->xml_buffer = NULL;
            window->xml_size = window->xml_capacity = 0u;
            return;
        }
        memcpy(window->xml_buffer + window->xml_size,
               message->data + sizeof(wm_msg_header_t), chunk_size);
        window->xml_size += chunk_size;
        window->xml_buffer[window->xml_size] = '\0';
    } else if (header->type == WM_SET_LAYOUT_XML_END) {
        if (window->xml_buffer) {
            xml_node_t *root = xml_parse(window->xml_buffer);
            if (root) {
                wm_canvas_t canvas;
                wm_canvas_init(&canvas, window->surface, window->frame.w,
                               window->frame.h, window->frame.w);
                surface_clear(window);
                if (root->child_count != 0u) {
                    for (uint32_t i = 0; i < root->child_count; ++i)
                        render_xml_node(state, window, &canvas,
                                        root->children[i], 0, 0);
                } else {
                    render_xml_node(state, window, &canvas, root, 0, 0);
                }
                xml_free(root);
                wm_window_damage_content(state, window,
                    (wm_rect_t){0, 0, window->frame.w, window->frame.h});
            }
            free(window->xml_buffer);
            window->xml_buffer = NULL;
            window->xml_size = window->xml_capacity = 0u;
        }
    }
}

/* Icons only ever legitimately live under the shared app-resource root; this
 * keeps a window-owning client from making the trusted WM process open (and
 * decode, up to 64 MiB) arbitrary files elsewhere on disk. */
static bool icon_path_is_allowed(const char *path)
{
    static const char prefix[] = "/Userland/";
    if (strncmp(path, prefix, sizeof(prefix) - 1u) != 0) return false;
    for (const char *p = path; *p; ++p) {
        if (p[0] == '.' && p[1] == '.' &&
            (p == path || p[-1] == '/') &&
            (p[2] == '/' || p[2] == '\0')) {
            return false;
        }
    }
    return true;
}

static void handle_icon_path(wm_state_t *state, const ipc_message_t *message,
                             const wm_msg_header_t *header)
{
    struct icon_path_message {
        wm_msg_header_t header;
        char path[192];
    };
    if (!message_has(message, sizeof(struct icon_path_message))) return;
    wm_window_t *window = owned_window(state, message, header);
    if (!window) return;
    const struct icon_path_message *command = (const void *)message->data;
    uint32_t width = 0u, height = 0u;
    char path[192];
    memcpy(path, command->path, sizeof(path));
    path[sizeof(path) - 1u] = '\0';
    if (!icon_path_is_allowed(path)) return;
    uint32_t *pixels = wm_assets_load_png(path, &width, &height);
    if (!pixels) return;
    for (uint32_t y = 0; y < 32u; ++y) {
        uint32_t source_y = (uint32_t)(((uint64_t)y * height) / 32u);
        for (uint32_t x = 0; x < 32u; ++x) {
            uint32_t source_x = (uint32_t)(((uint64_t)x * width) / 32u);
            window->icon[y * 32u + x] = pixels[source_y * width + source_x];
        }
    }
    free(pixels);
    window->has_icon = true;
    wm_window_mark_frame_damage(state, window);
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
        if (window) wm_scene_destroy_window(state, window->id);
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
    case WM_HIDE_WINDOW:
        window = owned_window(state, message, header);
        if (window) wm_scene_hide(state, window);
        break;
    case WM_RAISE_WINDOW:
        window = owned_window(state, message, header);
        if (window) wm_scene_raise(state, window);
        break;
    case WM_LOWER_WINDOW:
        window = owned_window(state, message, header);
        if (window) wm_scene_lower(state, window);
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
    case WM_GET_DISPLAY_INFO: {
        struct {
            wm_msg_header_t header;
            int32_t status;
            uint32_t width, height;
        } response;
        memset(&response, 0, sizeof(response));
        response.header.type = WM_GET_DISPLAY_INFO;
        response.header.request_id = header->request_id;
        response.status = WM_STATUS_OK;
        response.width = state->compositor.framebuffer_width;
        response.height = state->compositor.framebuffer_height;
        ipc_send_message(message->sender_pid, &response, sizeof(response));
        break;
    }
    case WM_GET_FOCUS: {
        struct {
            wm_msg_header_t header;
            int32_t status;
            uint32_t focused_id;
        } response;
        memset(&response, 0, sizeof(response));
        response.header.type = WM_GET_FOCUS;
        response.header.request_id = header->request_id;
        response.header.window_id = state->scene.focused_id;
        response.status = WM_STATUS_OK;
        response.focused_id = state->scene.focused_id;
        ipc_send_message(message->sender_pid, &response, sizeof(response));
        break;
    }
    case WM_DRAW_PIXEL: {
        struct pixel_message {
            wm_msg_header_t header;
            uint32_t x, y, color;
        };
        if (!message_has(message, sizeof(struct pixel_message))) break;
        window = owned_window(state, message, header);
        if (!window) break;
        const struct pixel_message *command = (const void *)message->data;
        if (command->x < window->frame.w && command->y < window->frame.h) {
            wm_canvas_t canvas;
            wm_canvas_init(&canvas, window->surface, window->frame.w,
                           window->frame.h, window->frame.w);
            wm_canvas_put(&canvas, (int32_t)command->x, (int32_t)command->y,
                          command->color);
            wm_window_damage_content(state, window,
                (wm_rect_t){(int32_t)command->x, (int32_t)command->y, 1u, 1u});
        }
        break;
    }
    case WM_DRAW_RECT:
        handle_draw_rect(state, message, header);
        break;
    case WM_DRAW_TEXT:
        handle_draw_text(state, message, header);
        break;
    case WM_CLEAR_WINDOW:
        window = owned_window(state, message, header);
        if (window) {
            surface_clear(window);
            wm_window_damage_content(state, window,
                (wm_rect_t){0, 0, window->frame.w, window->frame.h});
        }
        break;
    case WM_UPDATE_COMPLETE:
        window = owned_window(state, message, header);
        if (window) wm_window_end_transaction(state, window);
        break;
    case WM_SET_LAYOUT_XML_START:
    case WM_SET_LAYOUT_XML_CHUNK:
    case WM_SET_LAYOUT_XML_END:
        handle_xml(state, message, header);
        break;
    case WM_BEGIN_TRANSACTION:
        window = owned_window(state, message, header);
        if (window && window->transaction_depth < 32u) ++window->transaction_depth;
        break;
    case WM_END_TRANSACTION:
        window = owned_window(state, message, header);
        if (window) wm_window_end_transaction(state, window);
        break;
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
    case WM_UNSUBSCRIBE_INPUT:
        for (uint32_t i = 0; i < state->scene.input_sub_count; ++i) {
            wm_input_subscription_t *subscription = &state->scene.input_subs[i];
            if (subscription->subscriber_pid == message->sender_pid &&
                subscription->window_id == header->window_id) {
                *subscription =
                    state->scene.input_subs[--state->scene.input_sub_count];
                break;
            }
        }
        break;
    case WM_KEYBOARD_EVENT:
        wm_input_handle_keyboard(state, message);
        break;
    case WM_MOUSE_EVENT:
        wm_input_handle_mouse(state, message);
        break;
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
    case WM_SET_WINDOW_ICON:
        break;
    case WM_SET_WINDOW_ICON_PATH:
        handle_icon_path(state, message, header);
        break;
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
    case WM_SET_THEME: {
        struct theme_message {
            wm_msg_header_t header;
            uint32_t bg_top, bg_mid, bg_bottom, accent;
            uint32_t title_top, title_bottom;
        };
        if (!message_has(message, sizeof(struct theme_message))) break;
        const struct theme_message *command = (const void *)message->data;
        state->theme.bg_top = command->bg_top;
        state->theme.bg_mid = command->bg_mid;
        state->theme.bg_bottom = command->bg_bottom;
        state->theme.accent = command->accent;
        state->theme.border_focus = command->accent;
        state->theme.accent_soft = wm_color_lerp(
            state->theme.surface_alt, command->accent, 1u, 3u);
        state->theme.title_active = wm_color_lerp(
            state->theme.surface, command->title_top, 1u, 4u);
        state->theme.title_inactive = state->theme.surface;
        (void)command->title_bottom;
        wm_compositor_damage_all(state);
        break;
    }
    case WM_RELOAD_BACKGROUND:
        (void)wm_assets_reload_wallpaper(&state->assets);
        wm_compositor_generate_background(state);
        wm_compositor_damage_all(state);
        break;
    case WM_UPDATE_CLOCK: {
        struct clock_message {
            wm_msg_header_t header;
            char time[32];
            char date[16];
        };
        if (!message_has(message, sizeof(struct clock_message))) break;
        const struct clock_message *command = (const void *)message->data;
        char time_text[32];
        char date_text[16];
        memcpy(time_text, command->time, sizeof(time_text));
        memcpy(date_text, command->date, sizeof(date_text));
        time_text[sizeof(time_text) - 1u] = '\0';
        date_text[sizeof(date_text) - 1u] = '\0';
        strncpy(state->clock_text, time_text, sizeof(state->clock_text) - 1u);
        state->clock_text[sizeof(state->clock_text) - 1u] = '\0';
        strncpy(state->date_text, date_text, sizeof(state->date_text) - 1u);
        state->date_text[sizeof(state->date_text) - 1u] = '\0';
        wm_region_add(&state->compositor.damage,
                      (wm_rect_t){0,
                        (int32_t)(state->compositor.framebuffer_height -
                                  state->theme.dock_height - WM_DOCK_MARGIN * 2u),
                        state->compositor.framebuffer_width,
                        state->theme.dock_height + WM_DOCK_MARGIN * 2u},
                      (wm_rect_t){0, 0, state->compositor.framebuffer_width,
                                  state->compositor.framebuffer_height});
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
    case WM_SHOW_DIALOG: {
        struct dialog_message {
            wm_msg_header_t header;
            uint32_t type;
            char title[64];
            char message[128];
        };
        if (!message_has(message, sizeof(struct dialog_message))) break;
        const struct dialog_message *command = (const void *)message->data;
        char title[64];
        char body[128];
        memcpy(title, command->title, sizeof(title));
        memcpy(body, command->message, sizeof(body));
        title[sizeof(title) - 1u] = '\0';
        body[sizeof(body) - 1u] = '\0';
        uint32_t type = command->type;
        if (type > WM_DIALOG_ERROR) type = WM_DIALOG_INFO;
        wm_dialog_show(state, (wm_dialog_type_t)type, title, body);
        break;
    }
    case WM_GET_CAPABILITIES: {
        struct {
            wm_msg_header_t header;
            int32_t status;
            uint32_t capabilities;
        } response;
        memset(&response, 0, sizeof(response));
        response.header.type = WM_GET_CAPABILITIES;
        response.header.request_id = header->request_id;
        response.status = WM_STATUS_OK;
        response.capabilities = WM_CAP_SERVER_SURFACE | WM_CAP_DAMAGE_REGIONS |
            WM_CAP_TRANSACTIONS | WM_CAP_THEME_ENGINE | WM_CAP_NOTIFICATIONS |
            WM_CAP_SHARED_SURFACE | WM_CAP_DIALOGS;
        ipc_send_message(message->sender_pid, &response, sizeof(response));
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
    case WM_SET_CURSOR: {
        struct cursor_message {
            wm_msg_header_t header;
            uint32_t cursor;
        };
        if (!message_has(message, sizeof(struct cursor_message))) break;
        window = owned_window(state, message, header);
        if (!window) break;
        uint32_t cursor = ((const struct cursor_message *)message->data)->cursor;
        if (window->has_focus && cursor <= WM_CURSOR_RESIZE_DIAGONAL_NE_SW)
            state->scene.cursor_style = (wm_cursor_style_t)cursor;
        break;
    }
    default:
        break;
    }
}
