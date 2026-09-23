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

/*
 * The taskbar is the shell's Material Design 3 navigation bar: a
 * surface-container plate holding a row of 40px destinations, each of which
 * is an icon button with an M3 state layer, and each of which shows it is
 * the active one by wearing a secondary-container "active indicator" pill
 * rather than by changing colour.
 */

/* --- navigation bar geometry (px) --- */
#define TB_PAD           3                 /* plate inset -> 34px buttons */
#define TB_START_W       42u
#define TB_TASK_MAX      220u
#define TB_TRAY_ICON_W   34u
#define TB_NOTIF_W       34u
#define TB_CLOCK_W       168u
#define TB_SEP_GAP       8
#define TB_ICON_SZ       18u

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
    if (height < 34u) height = 34u;             /* room for the icon row */
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

/* Only running windows sit on the taskbar -- the Start button, then the
 * open-window buttons. (No pinned apps.list shortcuts.) */
static int32_t tasks_left_x(const wm_state_t *state)
{
    (void)state;
    return TB_PAD + (int32_t)TB_START_W + 8;
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

/* One destination's container: M3's active-indicator pill when it is the
 * current one, a state layer while it is merely hovered, and nothing at all
 * otherwise -- a navigation bar's plate shows through the rest of the time. */
static void draw_destination(wm_state_t *state, wm_canvas_t *canvas,
                             wm_rect_t box, uint32_t plate, bool active,
                             bool hovered)
{
    const m3_scheme_t *m3 = &state->theme.color;
    if (!active && !hovered) return;
    uint32_t container = active
        ? m3->secondary_container
        : m3_state_layer(plate, m3->on_surface, M3_STATE_HOVER);
    wm_canvas_fill_rounded(canvas, box,
        m3_shape_radius(M3_SHAPE_FULL, box.w, box.h), container);
}

static void tray_icon(wm_state_t *state, wm_canvas_t *canvas, int32_t x,
                      wm_rect_t dock, uint32_t plate, wm_icon_kind_t kind,
                      bool active, bool emphasised)
{
    const m3_scheme_t *m3 = &state->theme.color;
    wm_rect_t btn = {x, dock.y + TB_PAD, TB_TRAY_ICON_W,
                     dock.h - (uint32_t)TB_PAD * 2u};
    bool hover = cursor_in(state, btn);
    draw_destination(state, canvas, btn, plate, active, hover);
    uint32_t tint = active      ? m3->on_secondary_container :
                    emphasised  ? m3->primary :
                                  m3->on_surface_variant;
    wm_icon_draw(canvas,
        (wm_rect_t){btn.x + (int32_t)(btn.w - TB_ICON_SZ) / 2,
                    btn.y + (int32_t)(btn.h - TB_ICON_SZ) / 2,
                    TB_ICON_SZ, TB_ICON_SZ},
        kind, tint);
}

void wm_taskbar_draw(wm_state_t *state, wm_canvas_t *canvas)
{
    if (!state || !canvas) return;
    wm_rect_t dock = wm_taskbar_rect(state);
    if (dock.w == 0u) return;
    if (!wm_rect_intersects(dock, canvas->clip)) return;

    const m3_scheme_t *m3 = &state->theme.color;
    /* M3 puts a navigation bar on surface-container, and marks its edge
     * with an outline-variant divider rather than a shadow. */
    uint32_t plate = m3->surface_container;
    wm_canvas_fill(canvas, dock, plate);
    wm_canvas_fill(canvas, (wm_rect_t){0, dock.y, dock.w, 1u}, m3->outline_variant);

    int32_t pad = TB_PAD;

    /* --- Start button: the ImplusOS logo (Resource/Icons/Logo.png) --- */
    wm_rect_t start = {pad, dock.y + pad, TB_START_W - (uint32_t)pad,
                       dock.h - (uint32_t)pad * 2u};
    bool start_hover = cursor_in(state, start);
    draw_destination(state, canvas, start, plate, state->launcher_open, start_hover);
    {
        uint32_t isz = 24u;
        wm_rect_t ir = {start.x + (int32_t)(start.w - isz) / 2,
                        start.y + (int32_t)(start.h - isz) / 2, isz, isz};
        const wm_icon_image_t *logo = &state->assets.system_icons.logo;
        if (logo->pixels && logo->width && logo->height) {
            wm_canvas_blit_scaled(canvas, ir, logo->pixels,
                                  logo->width, logo->height, 255u,
                                  M3_SHAPE_EXTRA_SMALL);
        } else {
            wm_icon_draw(canvas, ir, WM_ICON_APPS,
                state->launcher_open ? m3->on_secondary_container
                                     : m3->on_surface_variant);
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
        uint32_t inset = btn_w >= 10u ? 2u : 0u;
        wm_rect_t btn = {btn_x + (int32_t)inset, dock.y + pad,
                         btn_w > inset * 2u ? btn_w - inset * 2u : btn_w,
                         dock.h - (uint32_t)pad * 2u};
        bool hover = cursor_in(state, btn);
        uint32_t pill = m3_shape_radius(M3_SHAPE_FULL, btn.w, btn.h);
        draw_destination(state, canvas, btn, plate, win->has_focus, hover);
        /* A minimised window is still running, so it keeps its place on the
         * bar; M3's way of saying "present but not active" is an outline
         * rather than a filled container. */
        if (win->minimized && !win->has_focus)
            wm_canvas_stroke_rounded(canvas, btn, pill, 1u, m3->outline_variant);

        uint32_t on_container = win->has_focus ? m3->on_secondary_container
                                               : m3->on_surface_variant;
        if (btn.w >= 30u) {
            uint32_t isz = TB_ICON_SZ;
            int32_t ix = btn.x + (btn.w >= 120u ? 10 : (int32_t)(btn.w - isz) / 2);
            int32_t iy = btn.y + (int32_t)(btn.h - isz) / 2;
            if (win->has_icon)
                wm_canvas_blit_scaled(canvas, (wm_rect_t){ix, iy, isz, isz},
                                      win->icon, 32u, 32u, 255u,
                                      M3_SHAPE_EXTRA_SMALL);
            else
                wm_icon_draw(canvas, (wm_rect_t){ix, iy, isz, isz},
                             WM_ICON_DOC, on_container);
        }
        if (btn.w >= 100u) {
            const char *title = win->title[0] ? win->title : "Window";
            float size = state->theme.type.label_medium;
            wm_font_draw(&state->font, canvas,
                         btn.x + 38, btn.y + (int32_t)(btn.h - (uint32_t)size) / 2,
                         title, on_container, size,
                         btn.w > 50u ? btn.w - 50u : 0u);
        }
        btn_x += (int32_t)btn_w;
        ++drawn;
    }

    /* --- notification bell, with its M3 badge --- */
    {
        wm_rect_t nbtn = {notif_x(dock), dock.y + pad, TB_NOTIF_W,
                          dock.h - (uint32_t)pad * 2u};
        bool nhover = cursor_in(state, nbtn);
        bool nactive = state->notification_center_open;
        draw_destination(state, canvas, nbtn, plate, nactive, nhover);
        wm_icon_draw(canvas,
            (wm_rect_t){nbtn.x + (int32_t)(nbtn.w - TB_ICON_SZ) / 2,
                        nbtn.y + (int32_t)(nbtn.h - TB_ICON_SZ) / 2,
                        TB_ICON_SZ, TB_ICON_SZ},
            WM_ICON_NOTIFICATIONS,
            nactive ? m3->on_secondary_container : m3->on_surface_variant);
        if (state->notification_unread_count) {
            /* A small badge: 6px of error, ringed in the plate colour so it
             * reads against the glyph it sits on. */
            wm_rect_t badge = {nbtn.x + (int32_t)nbtn.w - 15, nbtn.y + 7, 10u, 10u};
            wm_canvas_fill_rounded(canvas, badge, 5u, plate);
            wm_canvas_fill_rounded(canvas,
                (wm_rect_t){badge.x + 2, badge.y + 2, 6u, 6u}, 3u, m3->error);
        }
    }

    /* --- divider between the task list and the tray --- */
    wm_canvas_fill(canvas,
        (wm_rect_t){tray_left_x(dock) - TB_SEP_GAP / 2, dock.y + pad + 8,
                    1u, dock.h - (uint32_t)pad * 2u - 16u},
        m3->outline_variant);

    /* --- tray: IME, Wi-Fi, Audio (left -> right toward the clock) --- */
    {
        int32_t tx = tray_left_x(dock);
        bool wifi_connected = state->wifi_panel.status.state == WIFI_STATE_ASSOCIATED;
        tray_icon(state, canvas, tx, dock, plate, WM_ICON_IME, false, false);
        tray_icon(state, canvas, tx + (int32_t)TB_TRAY_ICON_W, dock, plate,
                  wifi_connected ? WM_ICON_WIFI : WM_ICON_WIFI_OFF,
                  state->wifi_panel.open, wifi_connected);
        tray_icon(state, canvas, tx + 2 * (int32_t)TB_TRAY_ICON_W, dock, plate,
                  WM_ICON_VOLUME, false, false);
    }

    /* --- clock: YYYY/MM/DD HH:MM:SS, far right --- */
    if (state->clock_text[0]) {
        wm_rect_t cr = wm_taskbar_clock_rect(state);
        wm_rect_t hit = {cr.x, dock.y + pad, cr.w, dock.h - (uint32_t)pad * 2u};
        draw_destination(state, canvas, hit, plate, false, cursor_in(state, hit));
        float size = state->theme.type.label_medium;
        uint32_t tw = wm_font_measure(&state->font, state->clock_text, size);
        int32_t tx = cr.x + ((int32_t)cr.w - (int32_t)tw) / 2;
        if (tx < cr.x + 2) tx = cr.x + 2;
        int32_t ty = dock.y + ((int32_t)dock.h - (int32_t)size) / 2 - 1;
        wm_font_draw(&state->font, canvas, tx, ty,
                     state->clock_text, m3->on_surface, size, cr.w);
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
