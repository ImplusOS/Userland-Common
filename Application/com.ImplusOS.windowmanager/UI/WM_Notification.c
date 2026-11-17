#include "WM_Notification.h"

#include "../Compositor/WM_Damage.h"
#include "../Compositor/WM_Raster.h"
#include "../Font/WM_Font.h"
#include "WM_Icons.h"
#include "WM_Taskbar.h"
#include "../Animation/WM_Animation.h"

#include <stdio.h>
#include <string.h>

static wm_rect_t screen_bounds(const wm_state_t *state)
{
    return (wm_rect_t){0, 0, state->compositor.framebuffer_width,
                       state->compositor.framebuffer_height};
}

/* Where the toasts can be: the stack above the taskbar, one card per
 * active toast plus one (so the frame after a toast expires also clears
 * the slot it left), with room for the slide-in and the card shadow.
 * Only this is repainted while toasts animate -- not the whole right-hand
 * column of the screen. */
static wm_rect_t notification_area(const wm_state_t *state)
{
    uint32_t active = 0u;
    for (uint32_t i = 0; i < WM_MAX_NOTIFICATIONS; ++i)
        if (state->notifications[i].active) ++active;
    uint32_t cards = active + 1u;
    if (cards > WM_MAX_NOTIFICATIONS) cards = WM_MAX_NOTIFICATIONS;
    int32_t bottom = wm_taskbar_rect(state).y;
    int32_t top = bottom - 12 - (int32_t)(cards * 106u) - 16;
    if (top < 0) top = 0;
    uint32_t width = wm_min_u32(412u, state->compositor.framebuffer_width);
    return (wm_rect_t){
        (int32_t)(state->compositor.framebuffer_width - width),
        top, width, (uint32_t)(bottom - top)
    };
}

wm_rect_t wm_notification_center_rect(const wm_state_t *state)
{
    if (!state) return (wm_rect_t){0, 0, 0, 0};
    wm_rect_t dock = wm_taskbar_rect(state);
    uint32_t width = wm_min_u32(392u,
        state->compositor.framebuffer_width > 20u ?
            state->compositor.framebuffer_width - 20u : state->compositor.framebuffer_width);
    /* Anchored to the bottom: the center opens up out of the taskbar. */
    int32_t bottom = dock.y - 10;
    uint32_t max_h = bottom > 10 ? (uint32_t)(bottom - 10) : 0u;
    uint32_t height = wm_min_u32(560u, max_h);
    wm_rect_t rest = {
        (int32_t)state->compositor.framebuffer_width - (int32_t)width - 10,
        bottom - (int32_t)height,
        width,
        height
    };
    rest.y += wm_panel_slide_offset(&state->notification_anim, rest, dock.y);
    return rest;
}

static void damage_notifications(wm_state_t *state)
{
    if (!state) return;
    wm_region_add(&state->compositor.damage, notification_area(state),
                  screen_bounds(state));
}

/* The center itself is repainted by wm_panel_animation_tick() as it slides;
 * this covers the toasts it hides or reveals and in-place changes such as
 * scrolling. */
static void damage_center(wm_state_t *state)
{
    wm_rect_t r = wm_notification_center_rect(state);
    wm_region_add(&state->compositor.damage,
        (wm_rect_t){r.x - 16, r.y - 12, r.w + 32u, r.h + 40u},
        screen_bounds(state));
    damage_notifications(state);
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
    damage_center(state);
}

void wm_notification_close_center(wm_state_t *state)
{
    if (!state || !state->notification_center_open) return;
    state->notification_center_open = false;
    damage_center(state);
}

bool wm_notification_center_contains(const wm_state_t *state, int32_t x, int32_t y)
{
    if (!state || !state->notification_center_open) return false;
    return wm_rect_intersects((wm_rect_t){x, y, 1u, 1u},
                              wm_notification_center_rect(state));
}

#define CENTER_CARD_H     68u
#define CENTER_CARD_PITCH 76u   /* card plus the gap under it */

/* The scrolling part of the center, below its header. */
static wm_rect_t center_list_rect(wm_rect_t panel)
{
    return (wm_rect_t){panel.x + 12, panel.y + 84, panel.w - 24u,
                       panel.h > 96u ? panel.h - 96u : 0u};
}

wm_rect_t wm_notification_center_list_rect(const wm_state_t *state)
{
    if (!state) return (wm_rect_t){0, 0, 0u, 0u};
    return center_list_rect(wm_notification_center_rect(state));
}

static float max_center_scroll(const wm_state_t *state)
{
    if (!state) return 0.0f;
    wm_rect_t list = center_list_rect(wm_notification_center_rect(state));
    uint32_t content_height = state->notification_history_count == 0u ? 0u :
        state->notification_history_count * CENTER_CARD_PITCH -
        (CENTER_CARD_PITCH - CENTER_CARD_H);
    return content_height > list.h ? (float)(content_height - list.h) : 0.0f;
}

bool wm_notification_center_scroll(wm_state_t *state, int32_t delta_px)
{
    if (!state || !state->notification_center_open || delta_px == 0) return false;
    /* The glide itself is driven, and repainted, by
     * wm_panel_animation_tick(). */
    return wm_scroll_to(&state->notification_center_scroll,
                        state->notification_center_scroll.target + (float)delta_px,
                        max_center_scroll(state));
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
    const theme_t *th = &state->theme.color;
    uint32_t radius = THEME_RADIUS_LARGE;
    uint32_t container = th->surface;

    wm_rect_t halo = {card.x - 12, card.y - 6, card.w + 24u, card.h + 24u};
    if (!wm_rect_intersects(halo, canvas->clip)) return;
    /* Mid-fade the card is translucent and its whole shadow shows
     * through; wm_canvas_popup_surface() tells the two apart. */
    wm_canvas_popup_surface(canvas, card, radius, WM_SHADOW_CARD,
                            color_alpha(0xFF000000u, 22u), opacity,
                            apply_opacity(th->border, opacity),
                            apply_opacity(container, opacity));

    wm_rect_t badge = {card.x + 16, card.y + (int32_t)(card.h - 36u) / 2 - 4,
                       36u, 36u};
    wm_canvas_fill_rounded(canvas, badge,
        wm_pill_radius(badge.w, badge.h),
        apply_opacity(th->accent_soft, opacity));
    wm_icon_draw(canvas,
        (wm_rect_t){badge.x + 9, badge.y + 9, 18u, 18u},
        WM_ICON_NOTIFICATIONS,
        apply_opacity(th->text, opacity));

    int32_t text_x = card.x + 66;
    uint32_t text_max = card.w > 82u ? card.w - 82u : 0u;
    wm_font_draw(&state->font, canvas, text_x, card.y + 18,
                 title, apply_opacity(th->text, opacity),
                 THEME_FONT_BODY, text_max);
    wm_font_draw(&state->font, canvas, text_x,
                 card.y + 18 + (int32_t)(THEME_FONT_BODY + 0.5f) + 6,
                 message, apply_opacity(th->text_dim, opacity),
                 THEME_FONT_SMALL, text_max);

    if (progress) {
        /* M3 linear progress indicator: a full-shape track in
         * secondary-container with the indicator in primary over it. */
        wm_rect_t track = {card.x + 16, card.y + (int32_t)card.h - 12,
                           card.w - 32u, 4u};
        wm_canvas_fill_rounded(canvas, track, 2u,
            apply_opacity(th->accent_soft, opacity));
        if (remaining_width > track.w) remaining_width = track.w;
        wm_canvas_fill_rounded(canvas,
            (wm_rect_t){track.x, track.y, remaining_width, track.h}, 2u,
            apply_opacity(th->accent, opacity));
    }
}

/* Toasts stack upward from just above the taskbar, newest-slot lowest,
 * and slide up into place (down and out when they expire). */
static void draw_popups(wm_state_t *state, wm_canvas_t *canvas, uint64_t now_ms)
{
    int32_t y = wm_taskbar_rect(state).y - 12 - 96;
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
            (int32_t)state->compositor.framebuffer_width - (int32_t)width - 12,
            y + (int32_t)offset,
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
        y -= 106;
    }
}

static void draw_center(wm_state_t *state, wm_canvas_t *canvas)
{
    if (!wm_panel_visible(&state->notification_anim,
                          state->notification_center_open)) return;
    const theme_t *th = &state->theme.color;
    wm_rect_t panel = wm_notification_center_rect(state);
    wm_rect_t halo = {panel.x - 12, panel.y - 6, panel.w + 24u, panel.h + 24u};
    if (!wm_rect_intersects(halo, canvas->clip)) return;
    wm_scroll_clamp(&state->notification_center_scroll, max_center_scroll(state));

    uint32_t radius = THEME_RADIUS_LARGE;
    uint32_t container = th->surface;

    wm_canvas_popup_surface(canvas, panel, radius, WM_SHADOW_POPUP,
                            color_alpha(0xFF000000u, 22u), 255u,
                            th->border, container);

    wm_rect_t badge = {panel.x + 20, panel.y + 18, 40u, 40u};
    wm_canvas_fill_rounded(canvas, badge,
        wm_pill_radius(badge.w, badge.h),
        th->accent_soft);
    wm_icon_draw(canvas, (wm_rect_t){badge.x + 10, badge.y + 10, 20u, 20u},
                 WM_ICON_NOTIFICATIONS, th->text);

    uint32_t header_max = panel.w > 96u ? panel.w - 96u : 0u;
    wm_font_draw(&state->font, canvas, panel.x + 72, panel.y + 18,
                 "通知", th->text, THEME_FONT_TITLE, header_max);
    char count_text[32];
    snprintf(count_text, sizeof(count_text), "%u 件",
             state->notification_history_count);
    wm_font_draw(&state->font, canvas, panel.x + 72,
                 panel.y + 18 + (int32_t)(THEME_FONT_TITLE + 0.5f) + 4,
                 count_text, th->text_dim, THEME_FONT_SMALL,
                 header_max);
    wm_canvas_fill(canvas,
        (wm_rect_t){panel.x + 16, panel.y + 70, panel.w - 32u, 1u},
        th->border);

    wm_rect_t list = center_list_rect(panel);
    wm_rect_t old_clip = canvas->clip;
    wm_canvas_set_clip(canvas, wm_rect_intersection(old_clip, list));
    if (state->notification_history_count == 0u) {
        wm_icon_draw(canvas,
            (wm_rect_t){list.x + (int32_t)list.w / 2 - 16, list.y + 28, 32u, 32u},
            WM_ICON_NOTIFICATIONS, th->border);
        const char *empty = "通知はありません";
        uint32_t empty_w = wm_font_measure(&state->font, empty, THEME_FONT_BODY);
        wm_font_draw(&state->font, canvas,
                     list.x + ((int32_t)list.w - (int32_t)empty_w) / 2,
                     list.y + 76, empty, th->text_dim,
                     THEME_FONT_BODY, list.w);
    } else if (canvas->clip.w != 0u && canvas->clip.h != 0u) {
        int32_t y = list.y - wm_scroll_offset(&state->notification_center_scroll);
        for (uint32_t display = 0u; display < state->notification_history_count; ++display) {
            uint32_t idx = history_index_for(state, display);
            if (idx >= WM_MAX_NOTIFICATION_HISTORY) continue;
            wm_notification_history_t *item = &state->notification_history[idx];
            if (!item->valid) continue;
            wm_rect_t card = {list.x, y, list.w, CENTER_CARD_H};
            /* Cards share the panel's surface colour, so a card is just
             * its content -- no container to fill. */
            if (wm_rect_intersects(card, canvas->clip)) {
                wm_rect_t dot = {card.x + 12, card.y + (int32_t)(card.h - 32u) / 2,
                                 32u, 32u};
                wm_canvas_fill_rounded(canvas, dot,
                    wm_pill_radius(dot.w, dot.h),
                    th->accent_soft);
                wm_icon_draw(canvas,
                    (wm_rect_t){dot.x + 8, dot.y + 8, 16u, 16u},
                    WM_ICON_NOTIFICATIONS, th->text);
                uint32_t item_max = card.w > 72u ? card.w - 72u : 0u;
                wm_font_draw(&state->font, canvas, card.x + 56, card.y + 14,
                             item->title, th->text, THEME_FONT_BODY,
                             item_max);
                wm_font_draw(&state->font, canvas, card.x + 56,
                             card.y + 14 + (int32_t)(THEME_FONT_BODY + 0.5f) + 6,
                             item->message, th->text_dim,
                             THEME_FONT_SMALL, item_max);
            }
            y += (int32_t)CENTER_CARD_PITCH;
        }
    }
    canvas->clip = old_clip;
}

void wm_notification_draw(wm_state_t *state, wm_canvas_t *canvas, uint64_t now_ms)
{
    if (!state || !canvas) return;
    wm_rect_t clip = wm_panel_begin_clip(state, canvas, &state->notification_anim);
    draw_center(state, canvas);
    wm_canvas_set_clip(canvas, clip);
    if (!state->notification_center_open) draw_popups(state, canvas, now_ms);
}
