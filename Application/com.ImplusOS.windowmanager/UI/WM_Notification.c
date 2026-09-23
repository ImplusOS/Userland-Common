#include "WM_Notification.h"

#include "../Compositor/WM_Damage.h"
#include "../Compositor/WM_Raster.h"
#include "../Font/WM_Font.h"
#include "WM_Icons.h"
#include "WM_Taskbar.h"

#include <stdio.h>
#include <string.h>

static wm_rect_t screen_bounds(const wm_state_t *state)
{
    return (wm_rect_t){0, 0, state->compositor.framebuffer_width,
                       state->compositor.framebuffer_height};
}

static wm_rect_t notification_area(const wm_state_t *state)
{
    uint32_t width = wm_min_u32(412u, state->compositor.framebuffer_width);
    return (wm_rect_t){
        (int32_t)(state->compositor.framebuffer_width - width),
        0,
        width,
        state->compositor.framebuffer_height
    };
}

wm_rect_t wm_notification_center_rect(const wm_state_t *state)
{
    if (!state) return (wm_rect_t){0, 0, 0, 0};
    wm_rect_t dock = wm_taskbar_rect(state);
    uint32_t width = wm_min_u32(392u,
        state->compositor.framebuffer_width > 20u ?
            state->compositor.framebuffer_width - 20u : state->compositor.framebuffer_width);
    int32_t y = 10;
    uint32_t height = dock.y > y + 14 ? (uint32_t)(dock.y - y - 14) :
        state->compositor.framebuffer_height > 20u ?
            state->compositor.framebuffer_height - 20u : state->compositor.framebuffer_height;
    return (wm_rect_t){
        (int32_t)state->compositor.framebuffer_width - (int32_t)width - 10,
        y,
        width,
        height
    };
}

static void damage_notifications(wm_state_t *state)
{
    if (!state) return;
    wm_region_add(&state->compositor.damage, notification_area(state),
                  screen_bounds(state));
}

static void push_history(wm_state_t *state, const char *title,
                         const char *message, uint64_t now_ms)
{
    if (!state) return;
    wm_notification_history_t *slot =
        &state->notification_history[state->notification_history_next];
    memset(slot, 0, sizeof(*slot));
    strncpy(slot->title, title, sizeof(slot->title) - 1u);
    strncpy(slot->message, message, sizeof(slot->message) - 1u);
    slot->created_ms = now_ms;
    slot->valid = true;
    state->notification_history_next =
        (state->notification_history_next + 1u) % WM_MAX_NOTIFICATION_HISTORY;
    if (state->notification_history_count < WM_MAX_NOTIFICATION_HISTORY)
        ++state->notification_history_count;
    if (!state->notification_center_open &&
        state->notification_unread_count < UINT32_MAX)
        ++state->notification_unread_count;
}

static uint32_t history_index_for(const wm_state_t *state, uint32_t display_index)
{
    if (!state || display_index >= state->notification_history_count)
        return WM_MAX_NOTIFICATION_HISTORY;
    return (state->notification_history_next + WM_MAX_NOTIFICATION_HISTORY -
            1u - display_index) % WM_MAX_NOTIFICATION_HISTORY;
}

void wm_notification_add(wm_state_t *state, const char *title, const char *message)
{
    if (!state || !title || !message) return;
    extern uint64_t get_uptime_ms(void);
    uint64_t now_ms = get_uptime_ms();
    push_history(state, title, message, now_ms);

    wm_notification_t *slot = NULL;
    for (uint32_t i = 0; i < WM_MAX_NOTIFICATIONS; ++i) {
        if (!state->notifications[i].active) {
            slot = &state->notifications[i];
            break;
        }
    }
    if (!slot) slot = &state->notifications[0];
    memset(slot, 0, sizeof(*slot));
    strncpy(slot->title, title, sizeof(slot->title) - 1u);
    strncpy(slot->message, message, sizeof(slot->message) - 1u);
    slot->start_ms = now_ms;
    slot->duration_ms = 5200u;
    slot->active = true;
    damage_notifications(state);
}

bool wm_notification_tick(wm_state_t *state, uint64_t now_ms)
{
    if (!state) return false;
    bool active_animation = false;
    for (uint32_t i = 0; i < WM_MAX_NOTIFICATIONS; ++i) {
        wm_notification_t *notification = &state->notifications[i];
        if (!notification->active) continue;
        uint64_t elapsed = now_ms - notification->start_ms;
        if (elapsed >= notification->duration_ms) {
            notification->active = false;
            damage_notifications(state);
        } else {
            if (elapsed < 240u || elapsed + 240u >= notification->duration_ms) {
                active_animation = true;
                damage_notifications(state);
            }
        }
    }
    return active_animation;
}

void wm_notification_toggle_center(wm_state_t *state)
{
    if (!state) return;
    state->notification_center_open = !state->notification_center_open;
    if (state->notification_center_open) state->notification_unread_count = 0u;
    damage_notifications(state);
}

void wm_notification_close_center(wm_state_t *state)
{
    if (!state || !state->notification_center_open) return;
    state->notification_center_open = false;
    damage_notifications(state);
}

bool wm_notification_center_contains(const wm_state_t *state, int32_t x, int32_t y)
{
    if (!state || !state->notification_center_open) return false;
    return wm_rect_intersects((wm_rect_t){x, y, 1u, 1u},
                              wm_notification_center_rect(state));
}

static uint32_t max_center_scroll(const wm_state_t *state)
{
    if (!state) return 0u;
    wm_rect_t panel = wm_notification_center_rect(state);
    uint32_t list_height = panel.h > 92u ? panel.h - 92u : 0u;
    uint32_t content_height = state->notification_history_count == 0u ? 0u :
        state->notification_history_count * 76u + 12u;
    return content_height > list_height ? (content_height - list_height + 75u) / 76u : 0u;
}

bool wm_notification_center_scroll(wm_state_t *state, int32_t rows)
{
    if (!state || !state->notification_center_open || rows == 0) return false;
    uint32_t old = state->notification_center_scroll;
    uint32_t max_scroll = max_center_scroll(state);
    int32_t next = (int32_t)state->notification_center_scroll + rows;
    if (next < 0) next = 0;
    if ((uint32_t)next > max_scroll) next = (int32_t)max_scroll;
    state->notification_center_scroll = (uint32_t)next;
    if (old != state->notification_center_scroll) {
        damage_notifications(state);
        return true;
    }
    return false;
}

static uint32_t apply_opacity(uint32_t color, uint8_t opacity)
{
    uint32_t alpha = ((color >> 24u) * (uint32_t)opacity) / 255u;
    return (color & 0x00FFFFFFu) | (alpha << 24u);
}

/*
 * A notification is an M3 elevated card: a large shape on a surface
 * container, a leading icon in its own secondary-container circle, a title
 * and supporting text, and -- while the toast is counting itself down -- an
 * M3 linear progress indicator along the bottom edge.
 */
static void draw_notification_card(wm_state_t *state, wm_canvas_t *canvas,
                                   wm_rect_t card, const char *title,
                                   const char *message, uint8_t opacity,
                                   bool progress, uint32_t remaining_width)
{
    const m3_scheme_t *m3 = &state->theme.color;
    const m3_typescale_t *type = &state->theme.type;
    uint32_t radius = m3_shape_radius(M3_SHAPE_LARGE, card.w, card.h);
    uint32_t container = m3_elevated(m3, m3->surface_container_low,
                                        M3_ELEVATION_2);

    wm_canvas_elevation_shadow(canvas, card, radius, M3_ELEVATION_2,
                               m3_alpha(m3->shadow, 22u), opacity);
    wm_canvas_fill_rounded(canvas, card, radius,
        apply_opacity(m3->outline_variant, opacity));
    wm_canvas_fill_rounded(canvas,
        (wm_rect_t){card.x + 1, card.y + 1, card.w - 2u, card.h - 2u},
        radius > 0u ? radius - 1u : 0u, apply_opacity(container, opacity));

    wm_rect_t badge = {card.x + 16, card.y + (int32_t)(card.h - 36u) / 2 - 4,
                       36u, 36u};
    wm_canvas_fill_rounded(canvas, badge,
        m3_shape_radius(M3_SHAPE_FULL, badge.w, badge.h),
        apply_opacity(m3->secondary_container, opacity));
    wm_icon_draw(canvas,
        (wm_rect_t){badge.x + 9, badge.y + 9, 18u, 18u},
        WM_ICON_NOTIFICATIONS,
        apply_opacity(m3->on_secondary_container, opacity));

    int32_t text_x = card.x + 66;
    uint32_t text_max = card.w > 82u ? card.w - 82u : 0u;
    wm_font_draw(&state->font, canvas, text_x, card.y + 18,
                 title, apply_opacity(m3->on_surface, opacity),
                 type->title_small, text_max);
    wm_font_draw(&state->font, canvas, text_x,
                 card.y + 18 + (int32_t)(type->title_small + 0.5f) + 6,
                 message, apply_opacity(m3->on_surface_variant, opacity),
                 type->body_small, text_max);

    if (progress) {
        /* M3 linear progress indicator: a full-shape track in
         * secondary-container with the indicator in primary over it. */
        wm_rect_t track = {card.x + 16, card.y + (int32_t)card.h - 12,
                           card.w - 32u, 4u};
        wm_canvas_fill_rounded(canvas, track, 2u,
            apply_opacity(m3->secondary_container, opacity));
        if (remaining_width > track.w) remaining_width = track.w;
        wm_canvas_fill_rounded(canvas,
            (wm_rect_t){track.x, track.y, remaining_width, track.h}, 2u,
            apply_opacity(m3->primary, opacity));
    }
}

static void draw_popups(wm_state_t *state, wm_canvas_t *canvas, uint64_t now_ms)
{
    int32_t y = 12;
    for (uint32_t i = 0; i < WM_MAX_NOTIFICATIONS; ++i) {
        wm_notification_t *notification = &state->notifications[i];
        if (!notification->active) continue;
        uint64_t elapsed = now_ms - notification->start_ms;
        float alpha = 1.0f;
        float offset = 0.0f;
        if (elapsed < 220u) {
            float progress = (float)elapsed / 220.0f;
            alpha = progress;
            offset = (1.0f - progress) * 28.0f;
        } else if (elapsed + 220u >= notification->duration_ms) {
            float remaining = (float)(notification->duration_ms - elapsed) / 220.0f;
            alpha = remaining;
            offset = (1.0f - remaining) * 20.0f;
        }
        uint8_t opacity = (uint8_t)(alpha * 255.0f);
        uint32_t width = wm_min_u32(372u,
            state->compositor.framebuffer_width > 16u ?
                state->compositor.framebuffer_width - 16u : 0u);
        wm_rect_t card = {
            (int32_t)state->compositor.framebuffer_width - (int32_t)width - 12 +
                (int32_t)offset,
            y,
            width,
            96u
        };
        uint32_t track_w = card.w > 32u ? card.w - 32u : 0u;
        uint32_t remaining_width = notification->duration_ms != 0u ?
            (uint32_t)(((uint64_t)(notification->duration_ms - elapsed) *
                        (uint64_t)track_w) / notification->duration_ms) : 0u;
        draw_notification_card(state, canvas, card, notification->title,
                               notification->message, opacity, true,
                               remaining_width);
        y += 106;
    }
}

static void draw_center(wm_state_t *state, wm_canvas_t *canvas)
{
    if (!state->notification_center_open) return;
    const m3_scheme_t *m3 = &state->theme.color;
    const m3_typescale_t *type = &state->theme.type;
    wm_rect_t panel = wm_notification_center_rect(state);
    wm_rect_t halo = {panel.x - 12, panel.y - 6, panel.w + 24u, panel.h + 24u};
    if (!wm_rect_intersects(halo, canvas->clip)) return;
    uint32_t max_scroll = max_center_scroll(state);
    if (state->notification_center_scroll > max_scroll)
        state->notification_center_scroll = max_scroll;

    uint32_t radius = m3_shape_radius(M3_SHAPE_EXTRA_LARGE,
                                         panel.w, panel.h);
    uint32_t container = m3_elevated(m3, m3->surface_container,
                                        M3_ELEVATION_3);

    wm_canvas_elevation_shadow(canvas, panel, radius, M3_ELEVATION_3,
                               m3_alpha(m3->shadow, 22u), 255u);
    wm_canvas_fill_rounded(canvas, panel, radius, m3->outline_variant);
    wm_canvas_fill_rounded(canvas,
        (wm_rect_t){panel.x + 1, panel.y + 1, panel.w - 2u, panel.h - 2u},
        radius > 0u ? radius - 1u : 0u, container);

    wm_rect_t badge = {panel.x + 20, panel.y + 18, 40u, 40u};
    wm_canvas_fill_rounded(canvas, badge,
        m3_shape_radius(M3_SHAPE_FULL, badge.w, badge.h),
        m3->secondary_container);
    wm_icon_draw(canvas, (wm_rect_t){badge.x + 10, badge.y + 10, 20u, 20u},
                 WM_ICON_NOTIFICATIONS, m3->on_secondary_container);

    uint32_t header_max = panel.w > 96u ? panel.w - 96u : 0u;
    wm_font_draw(&state->font, canvas, panel.x + 72, panel.y + 18,
                 "通知", m3->on_surface, type->title_large, header_max);
    char count_text[32];
    snprintf(count_text, sizeof(count_text), "%u 件",
             state->notification_history_count);
    wm_font_draw(&state->font, canvas, panel.x + 72,
                 panel.y + 18 + (int32_t)(type->title_large + 0.5f) + 4,
                 count_text, m3->on_surface_variant, type->label_medium,
                 header_max);
    wm_canvas_fill(canvas,
        (wm_rect_t){panel.x + 16, panel.y + 70, panel.w - 32u, 1u},
        m3->outline_variant);

    wm_rect_t list = {panel.x + 12, panel.y + 84, panel.w - 24u,
                      panel.h > 96u ? panel.h - 96u : 0u};
    wm_rect_t old_clip = canvas->clip;
    wm_canvas_set_clip(canvas, wm_rect_intersection(old_clip, list));
    if (state->notification_history_count == 0u) {
        wm_icon_draw(canvas,
            (wm_rect_t){list.x + (int32_t)list.w / 2 - 16, list.y + 28, 32u, 32u},
            WM_ICON_NOTIFICATIONS, m3->outline);
        const char *empty = "通知はありません";
        uint32_t empty_w = wm_font_measure(&state->font, empty, type->body_medium);
        wm_font_draw(&state->font, canvas,
                     list.x + ((int32_t)list.w - (int32_t)empty_w) / 2,
                     list.y + 76, empty, m3->on_surface_variant,
                     type->body_medium, list.w);
    } else {
        int32_t y = list.y - (int32_t)(state->notification_center_scroll * 76u);
        for (uint32_t display = 0u; display < state->notification_history_count; ++display) {
            uint32_t idx = history_index_for(state, display);
            if (idx >= WM_MAX_NOTIFICATION_HISTORY) continue;
            wm_notification_history_t *item = &state->notification_history[idx];
            if (!item->valid) continue;
            wm_rect_t card = {list.x, y, list.w, 68u};
            if (wm_rect_intersects(card, list)) {
                wm_canvas_fill_rounded(canvas, card,
                    m3_shape_radius(M3_SHAPE_MEDIUM, card.w, card.h),
                    m3->surface_container_high);
                wm_rect_t dot = {card.x + 12, card.y + (int32_t)(card.h - 32u) / 2,
                                 32u, 32u};
                wm_canvas_fill_rounded(canvas, dot,
                    m3_shape_radius(M3_SHAPE_FULL, dot.w, dot.h),
                    m3->secondary_container);
                wm_icon_draw(canvas,
                    (wm_rect_t){dot.x + 8, dot.y + 8, 16u, 16u},
                    WM_ICON_NOTIFICATIONS, m3->on_secondary_container);
                uint32_t item_max = card.w > 72u ? card.w - 72u : 0u;
                wm_font_draw(&state->font, canvas, card.x + 56, card.y + 14,
                             item->title, m3->on_surface, type->title_small,
                             item_max);
                wm_font_draw(&state->font, canvas, card.x + 56,
                             card.y + 14 + (int32_t)(type->title_small + 0.5f) + 6,
                             item->message, m3->on_surface_variant,
                             type->body_small, item_max);
            }
            y += 76;
        }
    }
    canvas->clip = old_clip;
}

void wm_notification_draw(wm_state_t *state, wm_canvas_t *canvas, uint64_t now_ms)
{
    if (!state || !canvas) return;
    draw_center(state, canvas);
    if (!state->notification_center_open) draw_popups(state, canvas, now_ms);
}
