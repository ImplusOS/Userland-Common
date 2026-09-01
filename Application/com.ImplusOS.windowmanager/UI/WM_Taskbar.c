#include "WM_Taskbar.h"
#include "WM_Icons.h"

#include "../Compositor/WM_Raster.h"
#include "../Font/WM_Font.h"
#include "../../../../Userland/API/Source/Time.h"
#include "../../../../Userland/API/Source/Network.h"
#include "../../../../Userland/API/Source/Process.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#define NTP_TIMEOUT_MS   5000u

/* --- taskbar geometry (px) --- */
#define TB_PAD           5
#define TB_START_W       46u
#define TB_PIN_W         42u
#define TB_TASK_MAX      220u
#define TB_TRAY_ICON_W   30u
#define TB_NOTIF_W       34u
#define TB_CLOCK_W       160u
#define TB_SEP_GAP       6

/* Simple day-of-week -- kept only for potential callers; unused in the
 * YYYY/MM/DD HH:MM:SS clock format. */
static uint32_t day_of_week(uint32_t y, uint32_t m, uint32_t d)
{
    static const uint32_t t[] = {0,3,2,5,0,3,5,1,4,6,2,4};
    if (m < 3) y--;
    return (y + y/4u - y/100u + y/400u + t[m-1u] + d) % 7u;
}

bool wm_taskbar_update_clock(wm_state_t *state)
{
    if (!state) return false;

    char next_clock[sizeof(state->clock_text)] = {0};

    if (state->ntp.ntp_ready) {
        uint64_t now_ms = get_uptime_ms();
        uint64_t elapsed_ms = now_ms > state->ntp.ntp_base_uptime_ms ?
            now_ms - state->ntp.ntp_base_uptime_ms : 0u;
        time_t now_sec = state->ntp.ntp_base_sec + (time_t)(elapsed_ms / 1000ULL);
        struct tm r;
        if (!gmtime_r(&now_sec, &r)) return false;
        snprintf(next_clock, sizeof(next_clock),
                 "%04d/%02d/%02d %02d:%02d:%02d",
                 r.tm_year + 1900, r.tm_mon + 1, r.tm_mday,
                 r.tm_hour, r.tm_min, r.tm_sec);
    } else {
        rtc_time_t t;
        if (sys_get_rtc_time(&t) < 0) return false;
        snprintf(next_clock, sizeof(next_clock),
                 "%04u/%02u/%02u %02u:%02u:%02u",
                 (unsigned)t.year, (unsigned)t.month, (unsigned)t.day,
                 (unsigned)t.hour, (unsigned)t.minute, (unsigned)t.second);
        (void)day_of_week;
    }

    if (strcmp(state->clock_text, next_clock) == 0) return false;
    memcpy(state->clock_text, next_clock, sizeof(state->clock_text));
    state->date_text[0] = '\0';
    return true;
}

/* ---------------------------------------------------------------- NTP */

void wm_taskbar_start_ntp(wm_state_t *state)
{
    if (!state || state->ntp.ntp_local_port != 0u) return;

    uint32_t ip = dns_resolve("time.google.com");
    if (ip == 0u) return;

    uint16_t port = 0u;
    for (uint16_t try_port = 49152u; try_port < 65535u; ++try_port) {
        if (udp_bind_port(try_port) >= 0) { port = try_port; break; }
    }
    if (port == 0u) return;

    uint8_t packet[48] = {0};
    packet[0] = 0x1Bu;
    if (!udp_send(ip, port, 123u, packet, 48)) {
        udp_unbind_port(port);
        return;
    }

    state->ntp.ntp_server_ip = ip;
    state->ntp.ntp_local_port = port;
    state->ntp.ntp_poll_start_ms = get_uptime_ms();
    state->ntp.ntp_ready = false;
}

void wm_taskbar_poll_ntp(wm_state_t *state)
{
    if (!state || state->ntp.ntp_local_port == 0u) return;

    uint64_t now_ms = get_uptime_ms();
    if (now_ms - state->ntp.ntp_poll_start_ms > NTP_TIMEOUT_MS) {
        udp_unbind_port(state->ntp.ntp_local_port);
        state->ntp.ntp_local_port = 0u;
        return;
    }

    uint8_t buf[8u + 48u];
    int32_t received = udp_recv(state->ntp.ntp_local_port, buf, sizeof(buf));
    if (received <= 0) return;

    if (received < 8 + 48) {
        udp_unbind_port(state->ntp.ntp_local_port);
        state->ntp.ntp_local_port = 0u;
        return;
    }

    uint32_t ntp_sec = ((uint32_t)buf[48] << 24u) | ((uint32_t)buf[49] << 16u) |
                       ((uint32_t)buf[50] << 8u) | (uint32_t)buf[51];

    if (ntp_sec < 2208988800u) {
        udp_unbind_port(state->ntp.ntp_local_port);
        state->ntp.ntp_local_port = 0u;
        return;
    }
    time_t unix_sec = (time_t)(ntp_sec - 2208988800u);

    state->ntp.ntp_base_sec = unix_sec;
    state->ntp.ntp_base_uptime_ms = get_uptime_ms();
    state->ntp.ntp_ready = true;

    udp_unbind_port(state->ntp.ntp_local_port);
    state->ntp.ntp_local_port = 0u;
}

/* ---------------------------------------------------------------- layout */

wm_rect_t wm_taskbar_rect(const wm_state_t *state)
{
    if (!state || state->compositor.framebuffer_width == 0u)
        return (wm_rect_t){0, 0, 0, 0};
    uint32_t height = state->theme.dock_height;
    if (height < 40u) height = 40u;             /* room for the icon row */
    if (height > state->compositor.framebuffer_height)
        height = state->compositor.framebuffer_height;
    return (wm_rect_t){
        0,
        (int32_t)(state->compositor.framebuffer_height - height),   /* bottom */
        state->compositor.framebuffer_width,
        height
    };
}

/* x of the leftmost tray icon (IME); everything right of this is the tray */
static int32_t tray_left_x(wm_rect_t dock)
{
    int32_t clock_x = (int32_t)dock.w - (int32_t)TB_CLOCK_W - 6;
    return clock_x - 3 * (int32_t)TB_TRAY_ICON_W;
}

static int32_t notif_x(wm_rect_t dock)
{
    return tray_left_x(dock) - TB_SEP_GAP - (int32_t)TB_NOTIF_W;
}

wm_rect_t wm_taskbar_clock_rect(const wm_state_t *state)
{
    wm_rect_t dock = wm_taskbar_rect(state);
    if (dock.w == 0u) return (wm_rect_t){0, 0, 0, 0};
    int32_t x = (int32_t)dock.w - (int32_t)TB_CLOCK_W - 6;
    if (x < 0) x = 0;
    return (wm_rect_t){x, dock.y, TB_CLOCK_W + 6u, dock.h};
}

static uint32_t pinned_count(const wm_state_t *state)
{
    uint32_t n = state->assets.app_count;
    return n < WM_TASKBAR_MAX_PINS ? n : WM_TASKBAR_MAX_PINS;
}

static int32_t tasks_left_x(const wm_state_t *state)
{
    return TB_PAD + (int32_t)TB_START_W + 6 +
           (int32_t)(pinned_count(state) * TB_PIN_W);
}

static bool cursor_in(const wm_state_t *state, wm_rect_t r)
{
    int32_t cx = (int32_t)state->scene.cursor_x;
    int32_t cy = (int32_t)state->scene.cursor_y;
    return cx >= r.x && cx < r.x + (int32_t)r.w &&
           cy >= r.y && cy < r.y + (int32_t)r.h;
}

static uint32_t visible_window_count(const wm_state_t *state)
{
    uint32_t count = 0u;
    for (uint32_t id = 1u; id <= WM_MAX_WINDOWS; ++id) {
        const wm_window_t *w = state->scene.id_table[id];
        if (w && w->visible && !w->is_system && !w->close_requested) ++count;
    }
    return count;
}

static void task_layout(const wm_state_t *state, wm_rect_t dock,
                        uint32_t *button_width, uint32_t *slot_count,
                        int32_t *start_x)
{
    uint32_t count = visible_window_count(state);
    int32_t left  = tasks_left_x(state);
    int32_t right = notif_x(dock) - 8;
    uint32_t available = right > left ? (uint32_t)(right - left) : 0u;
    uint32_t width = (count == 0u) ? 0u : available / count;
    uint32_t slots = count;
    if (count != 0u && width == 0u) { width = 1u; slots = available; }
    if (width > TB_TASK_MAX) width = TB_TASK_MAX;
    *button_width = width;
    *slot_count   = slots;
    *start_x      = left;
}

/* ---------------------------------------------------------------- draw */

static void tray_icon(wm_state_t *state, wm_canvas_t *canvas, int32_t x,
                      wm_rect_t dock, wm_icon_kind_t kind, bool active,
                      uint32_t tint)
{
    wm_rect_t btn = {x, dock.y + TB_PAD, TB_TRAY_ICON_W, dock.h - (uint32_t)TB_PAD * 2u};
    bool hover = cursor_in(state, btn);
    uint32_t bg = active ? state->theme.accent_soft :
                  hover  ? state->theme.surface_hover : 0u;
    if (bg) wm_canvas_fill_rounded(canvas, btn, 8u, bg);
    uint32_t isz = 18u;
    wm_icon_draw(canvas,
        (wm_rect_t){btn.x + (int32_t)(btn.w - isz) / 2,
                    btn.y + (int32_t)(btn.h - isz) / 2, isz, isz},
        kind, tint);
}

void wm_taskbar_draw(wm_state_t *state, wm_canvas_t *canvas)
{
    if (!state || !canvas) return;
    wm_rect_t dock = wm_taskbar_rect(state);
    if (dock.w == 0u) return;
    if (!wm_rect_intersects(dock, canvas->clip)) return;

    uint32_t dock_tint = (state->theme.dock & 0x00FFFFFFu) | (0xE6u << 24u);
    wm_canvas_fill(canvas, dock, dock_tint);
    wm_canvas_fill(canvas, (wm_rect_t){0, dock.y, dock.w, 1u}, state->theme.border);

    int32_t pad = TB_PAD;

    /* --- Start button (Material "apps" grid) --- */
    wm_rect_t start = {pad, dock.y + pad, TB_START_W - (uint32_t)pad, dock.h - (uint32_t)pad * 2u};
    bool start_hover = cursor_in(state, start);
    uint32_t start_bg = state->launcher_open ? state->theme.accent_soft :
                        start_hover           ? state->theme.surface_hover : 0u;
    if (start_bg) wm_canvas_fill_rounded(canvas, start, 8u, start_bg);
    {
        uint32_t isz = 22u;
        wm_icon_draw(canvas,
            (wm_rect_t){start.x + (int32_t)(start.w - isz) / 2,
                        start.y + (int32_t)(start.h - isz) / 2, isz, isz},
            WM_ICON_APPS,
            state->launcher_open ? state->theme.accent : state->theme.text);
    }

    /* --- pinned apps, in apps.list order, straight after Start --- */
    uint32_t pins = pinned_count(state);
    for (uint32_t i = 0; i < pins; ++i) {
        wm_launcher_app_t *app = &state->assets.apps[i];
        wm_rect_t slot = {pad + (int32_t)TB_START_W + 6 + (int32_t)(i * TB_PIN_W),
                          dock.y + pad, TB_PIN_W - 4u, dock.h - (uint32_t)pad * 2u};
        bool hover = cursor_in(state, slot);
        if (hover) wm_canvas_fill_rounded(canvas, slot, 8u, state->theme.surface_hover);
        wm_rect_t ir = {slot.x + (int32_t)(slot.w - 24u) / 2,
                        slot.y + (int32_t)(slot.h - 24u) / 2, 24u, 24u};
        if (app->icon_pixels) {
            wm_canvas_blit_scaled(canvas, ir, app->icon_pixels,
                                  app->icon_width, app->icon_height, 255u, 6u);
        } else {
            wm_canvas_fill_rounded(canvas, ir, 7u, state->theme.surface_alt);
            wm_font_draw(&state->font, canvas,
                         ir.x + (int32_t)(ir.w - 12u) / 2,
                         ir.y + (int32_t)(ir.h - 13u) / 2,
                         app->badge[0] ? app->badge : "?",
                         state->theme.text, state->theme.font_small, 20u);
        }
    }

    /* --- running windows --- */
    uint32_t btn_w = 0u, slots = 0u;
    int32_t btn_x = 0;
    task_layout(state, dock, &btn_w, &slots, &btn_x);
    uint32_t drawn = 0u;
    for (uint32_t id = 1u; id <= WM_MAX_WINDOWS && btn_w != 0u; ++id) {
        wm_window_t *win = state->scene.id_table[id];
        if (!win || !win->visible || win->is_system || win->close_requested) continue;
        if (drawn >= slots) break;
        uint32_t inset = btn_w >= 10u ? 3u : 0u;
        wm_rect_t btn = {btn_x + (int32_t)inset, dock.y + pad,
                         btn_w > inset * 2u ? btn_w - inset * 2u : btn_w,
                         dock.h - (uint32_t)pad * 2u};
        bool hover = cursor_in(state, btn);
        uint32_t bg = win->has_focus ? state->theme.accent_soft :
                      hover           ? state->theme.surface_hover : 0u;
        if (bg) wm_canvas_fill_rounded(canvas, btn, 7u, bg);

        if (btn.w >= 30u) {
            uint32_t isz = 20u;
            int32_t ix = btn.x + (btn.w >= 120u ? 8 : (int32_t)(btn.w - isz) / 2);
            int32_t iy = btn.y + (int32_t)(btn.h - isz) / 2;
            if (win->has_icon)
                wm_canvas_blit_scaled(canvas, (wm_rect_t){ix, iy, isz, isz},
                                      win->icon, 32u, 32u, 255u, 4u);
            else
                wm_icon_draw(canvas, (wm_rect_t){ix, iy, isz, isz},
                             WM_ICON_MAXIMIZE, state->theme.text_dim);
        }
        if (btn.w >= 100u) {
            const char *title = win->title[0] ? win->title : "Window";
            wm_font_draw(&state->font, canvas,
                         btn.x + 34, btn.y + (int32_t)(btn.h - 13u) / 2,
                         title,
                         win->has_focus ? state->theme.text : state->theme.text_dim,
                         state->theme.font_small,
                         btn.w > 44u ? btn.w - 44u : 0u);
        }
        if (win->has_focus || win->minimized) {
            uint32_t ind_w = win->has_focus ? 22u : 6u;
            ind_w = wm_min_u32(ind_w, btn.w);
            wm_canvas_fill_rounded(canvas,
                (wm_rect_t){btn.x + (int32_t)(btn.w - ind_w) / 2,
                            btn.y + (int32_t)btn.h - 3, ind_w, 3u},
                2u, win->has_focus ? state->theme.accent : state->theme.text_dim);
        }
        btn_x += (int32_t)btn_w;
        ++drawn;
    }

    /* --- notification bell --- */
    {
        wm_rect_t nbtn = {notif_x(dock), dock.y + pad, TB_NOTIF_W, dock.h - (uint32_t)pad * 2u};
        bool nhover = cursor_in(state, nbtn);
        uint32_t bg = state->notification_center_open ? state->theme.accent_soft :
                      nhover ? state->theme.surface_hover : 0u;
        if (bg) wm_canvas_fill_rounded(canvas, nbtn, 8u, bg);
        uint32_t isz = 18u;
        wm_icon_draw(canvas,
            (wm_rect_t){nbtn.x + (int32_t)(nbtn.w - isz) / 2,
                        nbtn.y + (int32_t)(nbtn.h - isz) / 2, isz, isz},
            WM_ICON_NOTIFICATIONS, state->theme.text);
        if (state->notification_unread_count) {
            wm_canvas_fill_rounded(canvas,
                (wm_rect_t){nbtn.x + (int32_t)nbtn.w - 12, nbtn.y + 5, 8u, 8u},
                4u, state->theme.accent | 0xFF000000u);
        }
    }

    /* --- separator --- */
    wm_canvas_fill(canvas,
        (wm_rect_t){tray_left_x(dock) - TB_SEP_GAP / 2, dock.y + pad + 4,
                    1u, dock.h - (uint32_t)pad * 2u - 8u},
        state->theme.border);

    /* --- tray: IME, Wi-Fi, Audio (left -> right toward the clock) --- */
    {
        int32_t tx = tray_left_x(dock);
        bool wifi_connected = state->wifi_panel.status.state == WIFI_STATE_ASSOCIATED;
        tray_icon(state, canvas, tx, dock, WM_ICON_IME, false, state->theme.text_dim);
        tray_icon(state, canvas, tx + (int32_t)TB_TRAY_ICON_W, dock,
                  wifi_connected ? WM_ICON_WIFI : WM_ICON_WIFI_OFF,
                  state->wifi_panel.open,
                  wifi_connected ? state->theme.accent : state->theme.text_dim);
        tray_icon(state, canvas, tx + 2 * (int32_t)TB_TRAY_ICON_W, dock,
                  WM_ICON_VOLUME, false, state->theme.text_dim);
    }

    /* --- clock: YYYY/MM/DD HH:MM:SS, far right --- */
    if (state->clock_text[0]) {
        wm_rect_t cr = wm_taskbar_clock_rect(state);
        uint32_t tw = wm_font_measure(&state->font, state->clock_text,
                                      state->theme.font_small);
        int32_t tx = cr.x + ((int32_t)cr.w - (int32_t)tw) / 2;
        if (tx < cr.x + 2) tx = cr.x + 2;
        int32_t ty = dock.y + (int32_t)dock.h / 2 - 7;
        wm_font_draw(&state->font, canvas, tx, ty,
                     state->clock_text, state->theme.text,
                     state->theme.font_small, cr.w);
    }
}

/* ---------------------------------------------------------------- hit test */

wm_taskbar_hit_t wm_taskbar_hit_test(wm_state_t *state, int32_t x, int32_t y)
{
    wm_taskbar_hit_t result = {WM_TASKBAR_HIT_NONE, 0u, 0u};
    if (!state) return result;
    wm_rect_t dock = wm_taskbar_rect(state);
    if (!wm_rect_intersects((wm_rect_t){x, y, 1u, 1u}, dock)) return result;

    if (x >= TB_PAD && x < TB_PAD + (int32_t)TB_START_W) {
        result.kind = WM_TASKBAR_HIT_LAUNCHER;
        return result;
    }

    uint32_t pins = pinned_count(state);
    int32_t pin0 = TB_PAD + (int32_t)TB_START_W + 6;
    if (x >= pin0 && x < pin0 + (int32_t)(pins * TB_PIN_W)) {
        result.kind = WM_TASKBAR_HIT_PIN;
        result.index = (uint32_t)((x - pin0) / (int32_t)TB_PIN_W);
        if (result.index >= pins) result.index = pins - 1u;
        return result;
    }

    int32_t nx = notif_x(dock);
    if (x >= nx && x < nx + (int32_t)TB_NOTIF_W) {
        result.kind = WM_TASKBAR_HIT_NOTIFICATION;
        return result;
    }

    int32_t tx = tray_left_x(dock);
    if (x >= tx && x < tx + (int32_t)TB_TRAY_ICON_W) { result.kind = WM_TASKBAR_HIT_IME; return result; }
    if (x >= tx + (int32_t)TB_TRAY_ICON_W && x < tx + 2 * (int32_t)TB_TRAY_ICON_W) { result.kind = WM_TASKBAR_HIT_NETWORK; return result; }
    if (x >= tx + 2 * (int32_t)TB_TRAY_ICON_W && x < tx + 3 * (int32_t)TB_TRAY_ICON_W) { result.kind = WM_TASKBAR_HIT_AUDIO; return result; }

    wm_rect_t cr = wm_taskbar_clock_rect(state);
    if (x >= cr.x) { result.kind = WM_TASKBAR_HIT_CLOCK; return result; }

    uint32_t btn_w = 0u, slots = 0u;
    int32_t btn_x = 0;
    task_layout(state, dock, &btn_w, &slots, &btn_x);
    uint32_t tested = 0u;
    for (uint32_t id = 1u; id <= WM_MAX_WINDOWS && btn_w != 0u; ++id) {
        wm_window_t *win = state->scene.id_table[id];
        if (!win || !win->visible || win->is_system || win->close_requested) continue;
        if (tested >= slots) break;
        if (x >= btn_x && x < btn_x + (int32_t)btn_w) {
            result.kind = WM_TASKBAR_HIT_WINDOW;
            result.window_id = id;
            return result;
        }
        btn_x += (int32_t)btn_w;
        ++tested;
    }
    return result;
}
