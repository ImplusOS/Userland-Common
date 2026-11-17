#include "WM_WifiPanel.h"

#include "../Compositor/WM_Damage.h"
#include "../Compositor/WM_Raster.h"
#include "../Font/WM_Font.h"
#include "WM_Icons.h"
#include "WM_Taskbar.h"
#include "../Animation/WM_Animation.h"
#include "../../../../Userland/Source/Syscalls.h"

#include <stdio.h>
#include <string.h>

/*
 * The Wi-Fi panel is a Material Design 3 sheet: an extra-large shape on
 * surface-container at elevation 3, an M3 list of networks, and -- for a
 * secured network -- a filled text field with the M3 filled/outlined button
 * pair under it.
 */

#define WIFI_PANEL_WIDTH 360u
#define WIFI_ROW_H       56u
#define WIFI_HEADER_H    84u
#define WIFI_CONNECTED_CARD_H 64u
#define WIFI_SCAN_BTN    40u

wm_rect_t wm_wifi_panel_rect(const wm_state_t *state)
{
    if (!state) return (wm_rect_t){0, 0, 0, 0};
    uint32_t width = wm_min_u32(WIFI_PANEL_WIDTH,
        state->compositor.framebuffer_width > 20u ?
            state->compositor.framebuffer_width - 20u : state->compositor.framebuffer_width);
    /* Anchored to the bottom: the panel opens up out of the taskbar. */
    wm_rect_t dock = wm_taskbar_rect(state);
    int32_t bottom = dock.y - 10;
    uint32_t max_h = bottom > 10 ? (uint32_t)(bottom - 10) : 0u;
    uint32_t height = wm_min_u32(440u, max_h);
    wm_rect_t rest = {
        (int32_t)state->compositor.framebuffer_width - (int32_t)width - 10,
        bottom - (int32_t)height, width, height
    };
    rest.y += wm_panel_slide_offset(&state->wifi_anim, rest, dock.y);
    return rest;
}

bool wm_wifi_panel_contains(const wm_state_t *state, int32_t x, int32_t y)
{
    if (!state || !state->wifi_panel.open) return false;
    return wm_rect_intersects((wm_rect_t){x, y, 1u, 1u}, wm_wifi_panel_rect(state));
}

static wm_rect_t scan_button_rect(wm_rect_t panel)
{
    return (wm_rect_t){panel.x + (int32_t)panel.w - (int32_t)WIFI_SCAN_BTN - 12,
                       panel.y + 18, WIFI_SCAN_BTN, WIFI_SCAN_BTN};
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

static wm_rect_t list_row_rect(wm_rect_t list, const wm_scroll_t *scroll,
                               uint32_t index)
{
    int32_t y = list.y - wm_scroll_offset(scroll) + (int32_t)(index * WIFI_ROW_H);
    return (wm_rect_t){list.x, y, list.w, WIFI_ROW_H - 6u};
}

wm_rect_t wm_wifi_panel_list_rect(const wm_state_t *state)
{
    if (!state) return (wm_rect_t){0, 0, 0u, 0u};
    return list_start(body_rect(wm_wifi_panel_rect(state)), state);
}

static float max_list_scroll(const wm_state_t *state)
{
    const wm_wifi_panel_t *p = &state->wifi_panel;
    wm_rect_t list = list_start(body_rect(wm_wifi_panel_rect(state)), state);
    uint32_t content = p->result_count == 0u ? 0u :
        p->result_count * WIFI_ROW_H - 6u;
    return content > list.h ? (float)(content - list.h) : 0.0f;
}

static wm_rect_t pw_back_rect(wm_rect_t body)     { return (wm_rect_t){body.x, body.y, 88u, 24u}; }
static wm_rect_t pw_field_rect(wm_rect_t body)    { return (wm_rect_t){body.x, body.y + 66, body.w, 56u}; }
static wm_rect_t pw_connect_rect(wm_rect_t body)  {
    uint32_t half = body.w > 12u ? (body.w - 12u) / 2u : 0u;
    return (wm_rect_t){body.x, body.y + 138, half, 40u};
}
static wm_rect_t pw_cancel_rect(wm_rect_t body) {
    uint32_t half = body.w > 12u ? (body.w - 12u) / 2u : 0u;
    return (wm_rect_t){body.x + (int32_t)half + 12, body.y + 138, half, 40u};
}

/* No lock glyph in the Material Symbols set the shell ships, so it is drawn
 * by hand on the same 24px grid: a rounded body under a shackle arc. */
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
        wm_canvas_fill_rounded(canvas,
            (wm_rect_t){x + (int32_t)(bar * 5u), y + (int32_t)(14u - h), 3u, h},
            1u, bar < lit ? on_color : off_color);
    }
}

static const char *status_line(const wm_state_t *state, char *buf, size_t buf_len)
{
    const wm_wifi_panel_t *p = &state->wifi_panel;
    switch (p->status.state) {
        case WIFI_STATE_NO_ADAPTER:      return "No Wi-Fi adapter detected";
        case WIFI_STATE_ADAPTER_ATTACHED:
        case WIFI_STATE_FIRMWARE_LOADING: return "Starting adapter...";
        case WIFI_STATE_FIRMWARE_FAILED:
            if (p->status.detail[0] != '\0') {
                snprintf(buf, buf_len, "Firmware failed: %s", p->status.detail);
                return buf;
            }
            return "Adapter firmware failed to load";
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

/* An M3 text button: a label in primary over a full-shape state layer. */
static void draw_text_button(wm_state_t *state, wm_canvas_t *canvas,
                             wm_rect_t box, const char *label, uint32_t on_color)
{
    uint32_t label_w = wm_font_measure(&state->font, label, THEME_FONT_BODY);
    wm_font_draw(&state->font, canvas,
                 box.x + ((int32_t)box.w - (int32_t)label_w) / 2,
                 box.y + ((int32_t)box.h - (int32_t)THEME_FONT_BODY) / 2,
                 label, on_color, THEME_FONT_BODY, box.w);
}

static void draw_header(wm_state_t *state, wm_canvas_t *canvas, wm_rect_t panel)
{
    const theme_t *th = &state->theme.color;
    bool connected = is_connected(state);

    wm_rect_t badge = {panel.x + 20, panel.y + 18, 40u, 40u};
    if (connected)    /* otherwise it is the panel's own surface colour */
        wm_canvas_fill_rounded(canvas, badge,
            wm_pill_radius(badge.w, badge.h), th->accent_soft);
    wm_icon_draw(canvas, (wm_rect_t){badge.x + 10, badge.y + 10, 20u, 20u},
                 connected ? WM_ICON_WIFI : WM_ICON_WIFI_OFF,
                 connected ? th->text : th->text_dim);

    uint32_t text_max = panel.w > 140u ? panel.w - 140u : 0u;
    wm_font_draw(&state->font, canvas, panel.x + 72, panel.y + 18,
                 "Wi-Fi", th->text, THEME_FONT_LARGE, text_max);

    char buf[64];
    const char *line = status_line(state, buf, sizeof(buf));
    wm_font_draw(&state->font, canvas, panel.x + 72,
                 panel.y + 18 + (int32_t)(THEME_FONT_LARGE + 0.5f) + 4,
                 line, th->text_dim, THEME_FONT_SMALL, text_max);

    /* Rescanning is an M3 icon button; while a scan is running it wears the
     * active indicator rather than changing its glyph. */
    wm_rect_t scan_btn = scan_button_rect(panel);
    bool scanning = state->wifi_panel.scan_active;
    if (scanning)
        wm_canvas_fill_rounded(canvas, scan_btn,
            wm_pill_radius(scan_btn.w, scan_btn.h), th->accent_soft);
    wm_icon_draw(canvas,
        (wm_rect_t){scan_btn.x + 10, scan_btn.y + 10, 20u, 20u},
        WM_ICON_SEARCH,
        scanning ? th->text : th->text_dim);

    wm_canvas_fill(canvas,
        (wm_rect_t){panel.x + 16, panel.y + (int32_t)WIFI_HEADER_H - 8,
                    panel.w - 32u, 1u},
        th->border);
}

static void draw_connected_card(wm_state_t *state, wm_canvas_t *canvas, wm_rect_t card)
{
    const theme_t *th = &state->theme.color;
    wm_canvas_fill_rounded(canvas, card,
        THEME_RADIUS,
        th->accent_soft);
    draw_signal_bars(canvas, card.x + 16, card.y + (int32_t)card.h / 2 + 4,
                     state->wifi_panel.status.state == WIFI_STATE_ASSOCIATED ? -40 : -80,
                     th->text,
                     color_mix(th->accent_soft,
                                       th->text, 24u));
    wm_font_draw(&state->font, canvas, card.x + 48, card.y + 12,
                 state->wifi_panel.status.ssid, th->text,
                 THEME_FONT_BODY, card.w > 150u ? card.w - 150u : 0u);
    wm_font_draw(&state->font, canvas, card.x + 48,
                 card.y + 12 + (int32_t)(THEME_FONT_BODY + 0.5f) + 4,
                 "接続済み", th->text, THEME_FONT_SMALL,
                 card.w > 150u ? card.w - 150u : 0u);
    draw_text_button(state, canvas,
                     (wm_rect_t){card.x + (int32_t)card.w - 96, card.y + 10,
                                 86u, card.h > 20u ? card.h - 20u : card.h},
                     "切断", th->accent);
}

static void draw_list(wm_state_t *state, wm_canvas_t *canvas, wm_rect_t body)
{
    const theme_t *th = &state->theme.color;
    wm_rect_t list = list_start(body, state);
    /* A rescan can return fewer networks than were scrolled past. */
    wm_scroll_clamp(&state->wifi_panel.scroll, max_list_scroll(state));
    wm_rect_t old_clip = canvas->clip;
    wm_canvas_set_clip(canvas, wm_rect_intersection(old_clip, list));

    const wm_wifi_panel_t *p = &state->wifi_panel;
    if (p->result_count == 0u) {
        const char *msg = p->scan_active ? "スキャン中..."
                                         : "ネットワークが見つかりません";
        wm_font_draw(&state->font, canvas, list.x + 8, list.y + 20,
                     msg, th->text_dim, THEME_FONT_BODY,
                     list.w > 16u ? list.w - 16u : 0u);
    } else if (canvas->clip.w != 0u && canvas->clip.h != 0u) {
        /* Rows are a fixed pitch: start at the first one on screen. */
        uint32_t first = (uint32_t)wm_scroll_offset(&p->scroll) / WIFI_ROW_H;
        for (uint32_t i = first; i < p->result_count; ++i) {
            wm_rect_t row = list_row_rect(list, &p->scroll, i);
            if (row.y >= list.y + (int32_t)list.h) break;
            if (!wm_rect_intersects(row, canvas->clip)) continue;
            const wifi_scan_result_t *r = &p->results[i];
            bool is_current = is_connected(state) &&
                strncmp(r->ssid, p->status.ssid, sizeof(r->ssid)) == 0;
            uint32_t on_container = th->text;
            /* Only the current network has a container of its own; the
             * others sit on the panel's surface. */
            if (is_current)
                wm_canvas_fill_rounded(canvas, row,
                    wm_pill_radius(row.w, row.h), th->accent_soft);
            draw_signal_bars(canvas, row.x + 16, row.y + (int32_t)row.h / 2 + 5,
                             r->rssi_dbm, on_container, th->border);
            wm_font_draw(&state->font, canvas, row.x + 48,
                         row.y + ((int32_t)row.h - (int32_t)THEME_FONT_BODY) / 2,
                         r->ssid, on_container, THEME_FONT_BODY,
                         row.w > 92u ? row.w - 92u : 0u);
            if (r->security != WIFI_SECURITY_OPEN) {
                draw_lock_glyph(canvas, row.x + (int32_t)row.w - 24,
                                row.y + (int32_t)row.h / 2,
                                is_current ? on_container : th->text_dim);
            }
        }
    }
    canvas->clip = old_clip;
}

static void draw_password_view(wm_state_t *state, wm_canvas_t *canvas, wm_rect_t body)
{
    const theme_t *th = &state->theme.color;
    const wm_wifi_panel_t *p = &state->wifi_panel;
    if (p->selected_index < 0 || (uint32_t)p->selected_index >= p->result_count) return;
    const wifi_scan_result_t *r = &p->results[p->selected_index];

    wm_rect_t back = pw_back_rect(body);
    wm_icon_draw(canvas, (wm_rect_t){back.x, back.y + 2, 20u, 20u},
                 WM_ICON_ARROW_BACK, th->text_dim);
    wm_font_draw(&state->font, canvas, back.x + 26, back.y + 4,
                 "戻る", th->text_dim, THEME_FONT_BODY,
                 back.w > 26u ? back.w - 26u : 0u);

    wm_font_draw(&state->font, canvas, body.x, body.y + 32,
                 r->ssid, th->text, THEME_FONT_LARGE, body.w);

    /* An M3 filled text field: a container with a square-ish top, an
     * indicator line along the bottom that turns primary on focus. */
    wm_rect_t field = pw_field_rect(body);
    wm_canvas_fill_shape(canvas, field,
        THEME_RADIUS_SMALL, THEME_RADIUS_SMALL, 0u, 0u,
        th->surface_alt);
    char masked[WM_WIFI_PASSWORD_MAX + 1u];
    uint32_t n = p->password_len < WM_WIFI_PASSWORD_MAX ? p->password_len : WM_WIFI_PASSWORD_MAX;
    for (uint32_t i = 0u; i < n; ++i) masked[i] = '*';
    masked[n] = '\0';
    const char *display = n > 0u ? masked : "パスワード";
    uint32_t tc = n > 0u ? th->text : th->text_dim;
    int32_t text_y = field.y + ((int32_t)field.h - (int32_t)THEME_FONT_LARGE) / 2;
    wm_font_draw(&state->font, canvas, field.x + 16, text_y,
                 display, tc, THEME_FONT_LARGE,
                 field.w > 32u ? field.w - 32u : 0u);
    wm_canvas_fill(canvas,
        (wm_rect_t){field.x, field.y + (int32_t)field.h - (p->password_active ? 2 : 1),
                    field.w, p->password_active ? 2u : 1u},
        p->password_active ? th->accent : th->text_dim);
    if (p->password_active) {
        uint32_t cursor_x = (uint32_t)(field.x + 16) +
            wm_font_measure(&state->font, masked, THEME_FONT_LARGE);
        wm_canvas_fill(canvas,
            (wm_rect_t){(int32_t)cursor_x, text_y - 2,
                        2u, (uint32_t)THEME_FONT_LARGE + 4u},
            th->accent);
    }

    /* The action pair M3 asks for: the confirming one filled, the
     * dismissing one outlined. */
    wm_rect_t connect = pw_connect_rect(body);
    bool can_connect = p->password_len >= 8u;
    uint32_t connect_radius = wm_pill_radius(connect.w, connect.h);
    wm_canvas_fill_rounded(canvas, connect, connect_radius,
        can_connect ? th->accent
                    : color_mix(th->surface, th->text,
                                        12u));
    draw_text_button(state, canvas, connect, "接続",
        can_connect ? th->on_accent
                    : color_mix(th->surface, th->text,
                                        THEME_DISABLED_PCT));

    wm_rect_t cancel = pw_cancel_rect(body);
    wm_canvas_stroke_rounded(canvas, cancel,
        wm_pill_radius(cancel.w, cancel.h), 1u,
        th->border);
    draw_text_button(state, canvas, cancel, "キャンセル", th->accent);

    wm_font_draw(&state->font, canvas, body.x, body.y + 170,
                 "WPA2-PSK passwords are 8-63 characters.",
                 th->text_dim, THEME_FONT_SMALL, body.w);
}

static void draw_panel(wm_state_t *state, wm_canvas_t *canvas);

void wm_wifi_panel_draw(wm_state_t *state, wm_canvas_t *canvas)
{
    if (!state || !canvas ||
        !wm_panel_visible(&state->wifi_anim, state->wifi_panel.open)) return;
    wm_rect_t clip = wm_panel_begin_clip(state, canvas, &state->wifi_anim);
    draw_panel(state, canvas);
    wm_canvas_set_clip(canvas, clip);
}

static void draw_panel(wm_state_t *state, wm_canvas_t *canvas)
{
    const theme_t *th = &state->theme.color;
    wm_rect_t panel = wm_wifi_panel_rect(state);
    if (panel.w == 0u || panel.h == 0u) return;
    wm_rect_t halo = {panel.x - 12, panel.y - 6, panel.w + 24u, panel.h + 24u};
    if (!wm_rect_intersects(halo, canvas->clip)) return;

    uint32_t radius = THEME_RADIUS_LARGE;
    uint32_t container = th->surface;

    wm_canvas_popup_surface(canvas, panel, radius, WM_SHADOW_POPUP,
                            color_alpha(0xFF000000u, 22u), 255u,
                            th->border, container);

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
        wm_rect_t row = list_row_rect(list, &p->scroll, i);
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

bool wm_wifi_panel_scroll(wm_state_t *state, int32_t delta_px)
{
    if (!state || !state->wifi_panel.open || state->wifi_panel.selected_index >= 0 || delta_px == 0) {
        return false;
    }
    wm_wifi_panel_t *p = &state->wifi_panel;
    return wm_scroll_to(&p->scroll, p->scroll.target + (float)delta_px,
                        max_list_scroll(state));
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
#define WIFI_SCAN_WINDOW_MS           3500u /* driver scan window (AIC8800: 3s) + slack */

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
