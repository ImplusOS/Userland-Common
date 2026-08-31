#include "WM_WifiPanel.h"

#include "../Compositor/WM_Damage.h"
#include "../Compositor/WM_Raster.h"
#include "../Font/WM_Font.h"
#include "WM_Taskbar.h"
#include "../../../../Userland/Syscalls.h"

#include <stdio.h>
#include <string.h>

#define WIFI_PANEL_WIDTH 340u
#define WIFI_ROW_H       54u
#define WIFI_HEADER_H    78u
#define WIFI_CONNECTED_CARD_H 60u

wm_rect_t wm_wifi_panel_rect(const wm_state_t *state)
{
    if (!state) return (wm_rect_t){0, 0, 0, 0};
    uint32_t width = wm_min_u32(WIFI_PANEL_WIDTH,
        state->compositor.framebuffer_width > 20u ?
            state->compositor.framebuffer_width - 20u : state->compositor.framebuffer_width);
    wm_rect_t dock = wm_taskbar_rect(state);
    int32_t y = 10;
    uint32_t max_h = dock.y > y + 14 ? (uint32_t)(dock.y - y - 14) :
        state->compositor.framebuffer_height > 20u ?
            state->compositor.framebuffer_height - 20u : state->compositor.framebuffer_height;
    uint32_t height = wm_min_u32(440u, max_h);
    return (wm_rect_t){
        (int32_t)state->compositor.framebuffer_width - (int32_t)width - 10,
        y, width, height
    };
}

bool wm_wifi_panel_contains(const wm_state_t *state, int32_t x, int32_t y)
{
    if (!state || !state->wifi_panel.open) return false;
    return wm_rect_intersects((wm_rect_t){x, y, 1u, 1u}, wm_wifi_panel_rect(state));
}

static wm_rect_t scan_button_rect(wm_rect_t panel)
{
    return (wm_rect_t){panel.x + (int32_t)panel.w - 40, panel.y + 16, 26u, 26u};
}

static wm_rect_t body_rect(wm_rect_t panel)
{
    return (wm_rect_t){panel.x + 12, panel.y + (int32_t)WIFI_HEADER_H,
                       panel.w > 24u ? panel.w - 24u : 0u,
                       panel.h > WIFI_HEADER_H + 12u ? panel.h - WIFI_HEADER_H - 12u : 0u};
}

static bool is_connected(const wm_state_t *state)
{
    return state->wifi_panel.status.state == WIFI_STATE_ASSOCIATED;
}

static wm_rect_t connected_card_rect(wm_rect_t body)
{
    return (wm_rect_t){body.x, body.y, body.w, WIFI_CONNECTED_CARD_H - 6u};
}

static wm_rect_t list_start(wm_rect_t body, const wm_state_t *state)
{
    int32_t offset = is_connected(state) ? (int32_t)WIFI_CONNECTED_CARD_H : 0;
    return (wm_rect_t){body.x, body.y + offset, body.w,
                       body.h > (uint32_t)offset ? body.h - (uint32_t)offset : 0u};
}

static wm_rect_t list_row_rect(wm_rect_t list, uint32_t scroll, uint32_t index)
{
    int32_t y = list.y - (int32_t)(scroll * WIFI_ROW_H) + (int32_t)(index * WIFI_ROW_H);
    return (wm_rect_t){list.x, y, list.w, WIFI_ROW_H - 6u};
}

static wm_rect_t pw_back_rect(wm_rect_t body)     { return (wm_rect_t){body.x, body.y, 70u, 24u}; }
static wm_rect_t pw_field_rect(wm_rect_t body)    { return (wm_rect_t){body.x, body.y + 58, body.w, 38u}; }
static wm_rect_t pw_connect_rect(wm_rect_t body)  {
    uint32_t half = body.w > 8u ? (body.w - 8u) / 2u : 0u;
    return (wm_rect_t){body.x, body.y + 110, half, 36u};
}
static wm_rect_t pw_cancel_rect(wm_rect_t body) {
    uint32_t half = body.w > 8u ? (body.w - 8u) / 2u : 0u;
    return (wm_rect_t){body.x + (int32_t)half + 8, body.y + 110, half, 36u};
}

/* No dedicated "lock" icon in wm_system_icons_t -- drawn by hand: a small
 * rounded body + a shackle arc approximated as two short vertical bars. */
static void draw_lock_glyph(wm_canvas_t *canvas, int32_t cx, int32_t cy, uint32_t color)
{
    wm_canvas_fill_rounded(canvas, (wm_rect_t){cx - 5, cy - 1, 10u, 8u}, 2u, color);
    wm_canvas_fill(canvas, (wm_rect_t){cx - 3, cy - 5, 1u, 5u}, color);
    wm_canvas_fill(canvas, (wm_rect_t){cx + 2, cy - 5, 1u, 5u}, color);
    wm_canvas_fill(canvas, (wm_rect_t){cx - 3, cy - 6, 6u, 1u}, color);
}

static void draw_signal_bars(wm_canvas_t *canvas, int32_t x, int32_t y, int8_t rssi_dbm,
                             uint32_t on_color, uint32_t off_color)
{
    uint32_t lit = rssi_dbm >= -50 ? 4u : rssi_dbm >= -60 ? 3u : rssi_dbm >= -70 ? 2u : 1u;
    for (uint32_t bar = 0u; bar < 4u; ++bar) {
        uint32_t h = 4u + bar * 3u;
        wm_canvas_fill(canvas,
            (wm_rect_t){x + (int32_t)(bar * 5u), y + (int32_t)(14u - h), 3u, h},
            bar < lit ? on_color : off_color);
    }
}

static const char *status_line(const wm_state_t *state, char *buf, size_t buf_len)
{
    const wm_wifi_panel_t *p = &state->wifi_panel;
    switch (p->status.state) {
        case WIFI_STATE_NO_ADAPTER:      return "No Wi-Fi adapter detected";
        case WIFI_STATE_ADAPTER_ATTACHED:
        case WIFI_STATE_FIRMWARE_LOADING: return "Starting adapter...";
        case WIFI_STATE_FIRMWARE_FAILED: return "Adapter firmware failed to load";
        case WIFI_STATE_SCANNING:        return "Scanning for networks...";
        case WIFI_STATE_CONNECTING:
            snprintf(buf, buf_len, "Connecting to %s...", p->status.ssid);
            return buf;
        case WIFI_STATE_ASSOCIATED:
            snprintf(buf, buf_len, "Connected to %s", p->status.ssid);
            return buf;
        case WIFI_STATE_CONNECT_FAILED:
            snprintf(buf, buf_len, "Couldn't connect to %s", p->status.ssid);
            return buf;
        case WIFI_STATE_READY:
        default:
            return "Not connected";
    }
}

static void draw_header(wm_state_t *state, wm_canvas_t *canvas, wm_rect_t panel)
{
    wm_canvas_draw_icon(canvas,
        (wm_rect_t){panel.x + 18, panel.y + 18, 22u, 22u},
        &state->assets.system_icons.network, 255u, 2u, state->theme.text);
    wm_font_draw(&state->font, canvas, panel.x + 50, panel.y + 16,
                 "Wi-Fi", state->theme.text, 15.0f, panel.w > 100u ? panel.w - 100u : 0u);

    char buf[64];
    const char *line = status_line(state, buf, sizeof(buf));
    wm_font_draw(&state->font, canvas, panel.x + 50, panel.y + 38,
                 line, state->theme.text_dim, 11.0f, panel.w > 100u ? panel.w - 100u : 0u);

    wm_rect_t scan_btn = scan_button_rect(panel);
    bool scanning = state->wifi_panel.scan_active;
    wm_canvas_fill_rounded(canvas, scan_btn, 13u,
        scanning ? state->theme.accent_soft : state->theme.surface_alt);
    wm_canvas_draw_icon(canvas,
        (wm_rect_t){scan_btn.x + 5, scan_btn.y + 5, 16u, 16u},
        &state->assets.system_icons.search, 220u, 2u, state->theme.text_dim);

    wm_canvas_fill(canvas,
        (wm_rect_t){panel.x + 16, panel.y + (int32_t)WIFI_HEADER_H - 8, panel.w - 32u, 1u},
        state->theme.border);
}

static void draw_connected_card(wm_state_t *state, wm_canvas_t *canvas, wm_rect_t card)
{
    wm_canvas_fill_rounded(canvas, card, 12u, state->theme.accent_soft);
    draw_signal_bars(canvas, card.x + 16, card.y + 32,
                     state->wifi_panel.status.state == WIFI_STATE_ASSOCIATED ? -40 : -80,
                     state->theme.accent, state->theme.border);
    wm_font_draw(&state->font, canvas, card.x + 44, card.y + 12,
                 state->wifi_panel.status.ssid, state->theme.text, 12.0f,
                 card.w > 130u ? card.w - 130u : 0u);
    wm_font_draw(&state->font, canvas, card.x + 44, card.y + 32,
                 "Connected", state->theme.text_dim, 10.0f,
                 card.w > 130u ? card.w - 130u : 0u);
    wm_font_draw(&state->font, canvas, card.x + (int32_t)card.w - 78, card.y + 20,
                 "Disconnect", state->theme.text, 11.0f, 72u);
}

static void draw_list(wm_state_t *state, wm_canvas_t *canvas, wm_rect_t body)
{
    wm_rect_t list = list_start(body, state);
    wm_rect_t old_clip = canvas->clip;
    wm_canvas_set_clip(canvas, wm_rect_intersection(old_clip, list));

    const wm_wifi_panel_t *p = &state->wifi_panel;
    if (p->result_count == 0u) {
        const char *msg = p->scan_active ? "Scanning..." : "No networks found. Tap the scan icon.";
        wm_font_draw(&state->font, canvas, list.x + 4, list.y + 20,
                     msg, state->theme.text_dim, 11.0f, list.w > 8u ? list.w - 8u : 0u);
    } else {
        for (uint32_t i = 0u; i < p->result_count; ++i) {
            wm_rect_t row = list_row_rect(list, p->scroll, i);
            if (!wm_rect_intersects(row, list)) continue;
            const wifi_scan_result_t *r = &p->results[i];
            bool is_current = is_connected(state) &&
                strncmp(r->ssid, p->status.ssid, sizeof(r->ssid)) == 0;
            wm_canvas_fill_rounded(canvas, row, 10u,
                is_current ? state->theme.accent_soft : state->theme.surface_alt);
            draw_signal_bars(canvas, row.x + 12, row.y + (int32_t)row.h / 2 + 5,
                             r->rssi_dbm, state->theme.text, state->theme.border);
            wm_font_draw(&state->font, canvas, row.x + 40, row.y + 8,
                         r->ssid, state->theme.text, 12.0f,
                         row.w > 80u ? row.w - 80u : 0u);
            if (r->security != WIFI_SECURITY_OPEN) {
                draw_lock_glyph(canvas, row.x + (int32_t)row.w - 20, row.y + (int32_t)row.h / 2,
                               state->theme.text_dim);
            }
        }
    }
    canvas->clip = old_clip;
}

static void draw_password_view(wm_state_t *state, wm_canvas_t *canvas, wm_rect_t body)
{
    const wm_wifi_panel_t *p = &state->wifi_panel;
    if (p->selected_index < 0 || (uint32_t)p->selected_index >= p->result_count) return;
    const wifi_scan_result_t *r = &p->results[p->selected_index];

    wm_rect_t back = pw_back_rect(body);
    wm_font_draw(&state->font, canvas, back.x, back.y + 4,
                 "< Back", state->theme.text_dim, 11.0f, back.w);

    wm_font_draw(&state->font, canvas, body.x, body.y + 30,
                 r->ssid, state->theme.text, 13.0f, body.w);

    wm_rect_t field = pw_field_rect(body);
    wm_canvas_fill_rounded(canvas, field, 10u, state->theme.surface_alt);
    char masked[WM_WIFI_PASSWORD_MAX + 1u];
    uint32_t n = p->password_len < WM_WIFI_PASSWORD_MAX ? p->password_len : WM_WIFI_PASSWORD_MAX;
    for (uint32_t i = 0u; i < n; ++i) masked[i] = '*';
    masked[n] = '\0';
    const char *display = n > 0u ? masked : "Password";
    uint32_t tc = n > 0u ? state->theme.text : state->theme.text_dim;
    wm_font_draw(&state->font, canvas, field.x + 12, field.y + 11,
                 display, tc, 13.0f, field.w > 24u ? field.w - 24u : 0u);
    if (p->password_active) {
        uint32_t cursor_x = (uint32_t)(field.x + 12) + wm_font_measure(&state->font, masked, 13.0f);
        wm_canvas_fill(canvas, (wm_rect_t){(int32_t)cursor_x, field.y + 9, 1u, 18u},
                      state->theme.text | 0xFF000000u);
    }

    wm_rect_t connect = pw_connect_rect(body);
    bool can_connect = p->password_len >= 8u;
    wm_canvas_fill_rounded(canvas, connect, 10u,
        can_connect ? state->theme.accent : state->theme.surface_alt);
    wm_font_draw(&state->font, canvas, connect.x + 16, connect.y + 10,
                 "Connect", can_connect ? 0xFFFFFFFFu : state->theme.text_dim, 12.0f, connect.w);

    wm_rect_t cancel = pw_cancel_rect(body);
    wm_canvas_fill_rounded(canvas, cancel, 10u, state->theme.surface_alt);
    wm_font_draw(&state->font, canvas, cancel.x + 20, cancel.y + 10,
                 "Cancel", state->theme.text_dim, 12.0f, cancel.w);

    wm_font_draw(&state->font, canvas, body.x, body.y + 158,
                 "WPA2 support is experimental on this driver -- see AX900.c.",
                 state->theme.text_dim, 9.0f, body.w);
}

void wm_wifi_panel_draw(wm_state_t *state, wm_canvas_t *canvas)
{
    if (!state || !canvas || !state->wifi_panel.open) return;
    wm_rect_t panel = wm_wifi_panel_rect(state);
    if (panel.w == 0u || panel.h == 0u) return;
    wm_rect_t shadow = {panel.x + 4, panel.y + 7, panel.w, panel.h};
    if (!wm_rect_intersects(wm_rect_union(panel, shadow), canvas->clip)) return;

    wm_canvas_fill_rounded(canvas, shadow, 20u, state->theme.shadow);
    wm_canvas_fill_rounded(canvas, panel, 20u, state->theme.dock_border);
    wm_canvas_fill_rounded(canvas,
        (wm_rect_t){panel.x + 1, panel.y + 1, panel.w - 2u, panel.h - 2u},
        19u, state->theme.surface);

    draw_header(state, canvas, panel);

    wm_rect_t body = body_rect(panel);
    if (state->wifi_panel.selected_index >= 0) {
        draw_password_view(state, canvas, body);
        return;
    }
    if (is_connected(state)) {
        draw_connected_card(state, canvas, connected_card_rect(body));
    }
    draw_list(state, canvas, body);
}

wm_wifi_action_t wm_wifi_panel_hit_test(wm_state_t *state, int32_t x, int32_t y)
{
    wm_wifi_action_t none = {WM_WIFI_ACTION_NONE, 0u};
    if (!state || !state->wifi_panel.open) return none;
    wm_rect_t panel = wm_wifi_panel_rect(state);
    if (!wm_rect_intersects((wm_rect_t){x, y, 1u, 1u}, panel)) return none;

    wm_rect_t scan_btn = scan_button_rect(panel);
    if (wm_rect_intersects((wm_rect_t){x, y, 1u, 1u}, scan_btn)) {
        return (wm_wifi_action_t){WM_WIFI_ACTION_SCAN, 0u};
    }

    wm_rect_t body = body_rect(panel);
    const wm_wifi_panel_t *p = &state->wifi_panel;

    if (p->selected_index >= 0) {
        if (wm_rect_intersects((wm_rect_t){x, y, 1u, 1u}, pw_back_rect(body))) {
            return (wm_wifi_action_t){WM_WIFI_ACTION_BACK, 0u};
        }
        if (wm_rect_intersects((wm_rect_t){x, y, 1u, 1u}, pw_connect_rect(body))) {
            return (wm_wifi_action_t){WM_WIFI_ACTION_CONNECT_PSK, (uint32_t)p->selected_index};
        }
        if (wm_rect_intersects((wm_rect_t){x, y, 1u, 1u}, pw_cancel_rect(body))) {
            return (wm_wifi_action_t){WM_WIFI_ACTION_BACK, 0u};
        }
        return none;
    }

    if (is_connected(state) &&
        wm_rect_intersects((wm_rect_t){x, y, 1u, 1u}, connected_card_rect(body))) {
        return (wm_wifi_action_t){WM_WIFI_ACTION_DISCONNECT, 0u};
    }

    wm_rect_t list = list_start(body, state);
    for (uint32_t i = 0u; i < p->result_count; ++i) {
        wm_rect_t row = list_row_rect(list, p->scroll, i);
        if (!wm_rect_intersects(row, list)) continue;
        if (wm_rect_intersects((wm_rect_t){x, y, 1u, 1u}, row)) {
            bool is_current = is_connected(state) &&
                strncmp(p->results[i].ssid, p->status.ssid, sizeof(p->results[i].ssid)) == 0;
            if (is_current) return none; /* use the connected card / disconnect for this one */
            wm_wifi_action_kind_t kind = p->results[i].security == WIFI_SECURITY_OPEN ?
                WM_WIFI_ACTION_CONNECT_OPEN : WM_WIFI_ACTION_SELECT;
            return (wm_wifi_action_t){kind, i};
        }
    }
    return none;
}

bool wm_wifi_panel_scroll(wm_state_t *state, int32_t rows)
{
    if (!state || !state->wifi_panel.open || state->wifi_panel.selected_index >= 0 || rows == 0) {
        return false;
    }
    wm_wifi_panel_t *p = &state->wifi_panel;
    uint32_t visible = p->result_count;
    uint32_t max_scroll = visible > 4u ? visible - 4u : 0u;
    int32_t next = (int32_t)p->scroll + rows;
    if (next < 0) next = 0;
    if ((uint32_t)next > max_scroll) next = (int32_t)max_scroll;
    if ((uint32_t)next == p->scroll) return false;
    p->scroll = (uint32_t)next;
    return true;
}

bool wm_wifi_panel_input_char(wm_state_t *state, char ch)
{
    if (!state || !state->wifi_panel.open || !state->wifi_panel.password_active) return false;
    if (ch < 0x20 || state->wifi_panel.password_len >= WM_WIFI_PASSWORD_MAX) return false;
    state->wifi_panel.password[state->wifi_panel.password_len++] = ch;
    state->wifi_panel.password[state->wifi_panel.password_len] = '\0';
    return true;
}

bool wm_wifi_panel_input_backspace(wm_state_t *state)
{
    if (!state || !state->wifi_panel.open || !state->wifi_panel.password_active ||
        state->wifi_panel.password_len == 0u) {
        return false;
    }
    state->wifi_panel.password[--state->wifi_panel.password_len] = '\0';
    return true;
}

#define WIFI_STATUS_POLL_INTERVAL_MS  1000u
#define WIFI_RESULTS_POLL_INTERVAL_MS 800u
#define WIFI_SCAN_WINDOW_MS           3500u /* matches AX900.c's ~3s scan window, plus slack */

bool wm_wifi_panel_poll(wm_state_t *state, uint64_t now_ms)
{
    if (!state || !state->wifi_panel.open) return false;
    wm_wifi_panel_t *p = &state->wifi_panel;
    bool changed = false;

    if (now_ms - p->last_status_poll_ms >= WIFI_STATUS_POLL_INTERVAL_MS ||
        p->last_status_poll_ms == 0u) {
        wifi_status_t prev = p->status;
        wifi_get_status(&p->status);
        p->last_status_poll_ms = now_ms;
        if (memcmp(&prev, &p->status, sizeof(prev)) != 0) changed = true;
    }

    if (p->selected_index < 0 &&
        (now_ms - p->last_results_poll_ms >= WIFI_RESULTS_POLL_INTERVAL_MS ||
         p->last_results_poll_ms == 0u)) {
        uint32_t prev_count = p->result_count;
        p->result_count = wifi_get_scan_results(p->results, WIFI_MAX_SCAN_RESULTS);
        p->last_results_poll_ms = now_ms;
        if (p->result_count != prev_count) changed = true;
    }

    if (p->scan_active && now_ms - p->scan_started_ms >= WIFI_SCAN_WINDOW_MS) {
        p->scan_active = false;
        changed = true;
    }

    return changed;
}
