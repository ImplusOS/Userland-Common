#include "WM_Dialog.h"

#include "../Compositor/WM_Damage.h"
#include "../Compositor/WM_Raster.h"
#include "../Font/WM_Font.h"

#include <string.h>

#define WM_DIALOG_WIDTH 380u
#define WM_DIALOG_HEIGHT 170u
#define WM_DIALOG_BUTTON_WIDTH 80u
#define WM_DIALOG_BUTTON_HEIGHT 30u

static wm_rect_t screen_bounds(const wm_state_t *state)
{
    return (wm_rect_t){0, 0, state->compositor.framebuffer_width,
                       state->compositor.framebuffer_height};
}

static uint32_t dialog_accent_color(wm_dialog_type_t type)
{
    switch (type) {
    case WM_DIALOG_ERROR:   return 0xFFE53935u;
    case WM_DIALOG_WARNING: return 0xFFFFA726u;
    case WM_DIALOG_INFO:
    default:                return 0xFF42A5F5u;
    }
}

static void damage_dialog(wm_state_t *state)
{
    if (!state || !state->dialog.active) return;
    uint32_t margin = state->theme.shadow_size * 2u + 10u;
    wm_region_add(&state->compositor.damage,
        (wm_rect_t){state->dialog.x - (int32_t)margin,
                    state->dialog.y - (int32_t)margin,
                    state->dialog.w + margin * 2u,
                    state->dialog.h + state->theme.title_height + margin * 2u},
        screen_bounds(state));
}

static void center_dialog(wm_state_t *state)
{
    uint32_t total_h = state->dialog.h + state->theme.title_height;
    uint32_t fb_w = state->compositor.framebuffer_width;
    uint32_t fb_h = state->compositor.framebuffer_height;
    state->dialog.x = (int32_t)(fb_w > WM_DIALOG_WIDTH ?
        (fb_w - WM_DIALOG_WIDTH) / 2u : 0u);
    state->dialog.y = (int32_t)(fb_h > total_h ?
        (fb_h - total_h) / 3u : 0u);
    state->dialog.w = WM_DIALOG_WIDTH;
    state->dialog.h = WM_DIALOG_HEIGHT;
}

void wm_dialog_show(wm_state_t *state, wm_dialog_type_t type,
                    const char *title, const char *message)
{
    if (!state || !title || !message) return;
    extern uint64_t get_uptime_ms(void);
    damage_dialog(state);
    memset(&state->dialog, 0, sizeof(state->dialog));
    strncpy(state->dialog.title, title, sizeof(state->dialog.title) - 1u);
    strncpy(state->dialog.message, message, sizeof(state->dialog.message) - 1u);
    state->dialog.type = type;
    state->dialog.active = true;
    state->dialog.start_ms = get_uptime_ms();
    center_dialog(state);
    damage_dialog(state);
}

void wm_dialog_close(wm_state_t *state)
{
    if (!state || !state->dialog.active) return;
    damage_dialog(state);
    state->dialog.active = false;
}

bool wm_dialog_is_active(const wm_state_t *state)
{
    return state && state->dialog.active;
}

bool wm_dialog_contains(const wm_state_t *state, int32_t x, int32_t y)
{
    if (!state || !state->dialog.active) return false;
    wm_rect_t r = {state->dialog.x, state->dialog.y,
                   state->dialog.w, state->dialog.h + state->theme.title_height};
    return wm_rect_intersects((wm_rect_t){x, y, 1u, 1u}, r);
}

bool wm_dialog_title_contains(const wm_state_t *state, int32_t x, int32_t y)
{
    if (!state || !state->dialog.active) return false;
    wm_rect_t r = {state->dialog.x, state->dialog.y,
                   state->dialog.w, state->theme.title_height};
    return wm_rect_intersects((wm_rect_t){x, y, 1u, 1u}, r);
}

bool wm_dialog_close_contains(const wm_state_t *state, int32_t x, int32_t y)
{
    if (!state || !state->dialog.active) return false;
    int32_t right = state->dialog.x + (int32_t)state->dialog.w - 6;
    int32_t by = state->dialog.y + ((int32_t)state->theme.title_height - 24) / 2;
    wm_rect_t btn = {right - 24, by, 24u, 24u};
    return wm_rect_intersects((wm_rect_t){x, y, 1u, 1u}, btn);
}

bool wm_dialog_ok_contains(const wm_state_t *state, int32_t x, int32_t y)
{
    if (!state || !state->dialog.active) return false;
    int32_t bx = state->dialog.x + (int32_t)(state->dialog.w - WM_DIALOG_BUTTON_WIDTH) / 2;
    int32_t by = state->dialog.y + (int32_t)state->theme.title_height + (int32_t)state->dialog.h - 20 - (int32_t)WM_DIALOG_BUTTON_HEIGHT;
    return wm_rect_intersects((wm_rect_t){x, y, 1u, 1u},
        (wm_rect_t){bx, by, WM_DIALOG_BUTTON_WIDTH, WM_DIALOG_BUTTON_HEIGHT});
}

bool wm_dialog_dragging(const wm_state_t *state)
{
    return state && state->dialog.active && state->dialog.dragging;
}

void wm_dialog_set_dragging(wm_state_t *state, bool dragging)
{
    if (!state) return;
    state->dialog.dragging = dragging;
}

void wm_dialog_set_hover_close(wm_state_t *state, bool hover)
{
    if (!state || !state->dialog.active) return;
    if (state->dialog.hover_close != hover) {
        state->dialog.hover_close = hover;
        damage_dialog(state);
    }
}

void wm_dialog_set_hover_ok(wm_state_t *state, bool hover)
{
    if (!state || !state->dialog.active) return;
    if (state->dialog.hover_ok != hover) {
        state->dialog.hover_ok = hover;
        damage_dialog(state);
    }
}

void wm_dialog_move(wm_state_t *state, int32_t x, int32_t y)
{
    if (!state || !state->dialog.active) return;
    damage_dialog(state);
    state->dialog.x = x;
    state->dialog.y = y;
    damage_dialog(state);
}

void wm_dialog_draw(wm_state_t *state, wm_canvas_t *canvas)
{
    if (!state || !canvas || !state->dialog.active) return;

    int32_t x = state->dialog.x;
    int32_t y = state->dialog.y;
    uint32_t w = state->dialog.w;
    uint32_t h = state->dialog.h;
    uint32_t title_h = state->theme.title_height;
    uint32_t corner = state->theme.corner_radius;
    uint32_t shadow_size = state->theme.shadow_size;

    wm_rect_t dialog_rect = {x, y, w, h + title_h};
    if (!wm_rect_intersects(dialog_rect, canvas->clip)) return;

    uint32_t accent = dialog_accent_color(state->dialog.type);

    if (shadow_size > 0u) {
        for (uint32_t layer = shadow_size; layer > 0u; --layer) {
            uint32_t strength = ((shadow_size - layer + 1u) * 40u) /
                (shadow_size == 0u ? 1u : shadow_size);
            wm_rect_t sh = {x - (int32_t)layer, y - (int32_t)(layer / 2u) + 5,
                            w + layer * 2u, h + title_h + layer * 2u};
            uint32_t alpha = (strength * 255u) / 255u;
            wm_canvas_fill_rounded(canvas, sh, corner + layer,
                (state->theme.shadow & 0x00FFFFFFu) | (alpha << 24u));
        }
    }

    wm_canvas_fill_rounded(canvas, dialog_rect, corner,
        state->theme.border);

    wm_rect_t inner = {x + 1, y + 1, w > 2u ? w - 2u : w,
                       h + title_h > 2u ? h + title_h - 2u : 0u};
    wm_canvas_fill_rounded(canvas, inner,
        corner > 0u ? corner - 1u : 0u,
        state->theme.surface);

    wm_rect_t title_rect = {x + 1, y + 1, w > 2u ? w - 2u : w, title_h};
    uint32_t top_r = corner > 0u ? corner - 1u : 0u;
    wm_canvas_fill_rounded(canvas, title_rect, top_r,
        state->theme.title_active);
    if (title_rect.h > top_r) {
        wm_canvas_fill(canvas,
            (wm_rect_t){title_rect.x, title_rect.y + (int32_t)top_r,
                        title_rect.w, title_rect.h - top_r},
            state->theme.title_active);
    }

    wm_canvas_fill(canvas,
        (wm_rect_t){x + 1, y + (int32_t)title_h,
                    w > 2u ? w - 2u : w, 1u},
        state->theme.border);

    wm_canvas_fill(canvas,
        (wm_rect_t){x + 1, y + (int32_t)title_h + 1,
                    w > 2u ? w - 2u : w, 3u},
        accent);

    int32_t close_x = x + (int32_t)w - 6 - 24;
    int32_t close_y = y + ((int32_t)title_h - 24) / 2;
    wm_rect_t close_btn = {close_x, close_y, 24u, 24u};
    uint32_t close_bg = state->dialog.hover_close ?
        0x30FFFFFFu : 0x00000000u;
    if (close_bg != 0u)
        wm_canvas_fill_rounded(canvas, close_btn, 12u, close_bg);

    int32_t cx = close_btn.x + 12;
    int32_t cy = close_btn.y + 12;
    uint32_t xc = state->dialog.hover_close ?
        0xFFFFFFFFu : state->theme.text_dim;
    wm_canvas_line(canvas, cx - 4, cy - 4, cx + 4, cy + 4, xc);
    wm_canvas_line(canvas, cx + 4, cy - 4, cx - 4, cy + 4, xc);

    if (state->font.loaded) {
        int32_t text_x = x + 10;
        int32_t text_y = y + ((int32_t)title_h - (int32_t)state->theme.font_title) / 2 - 1;
        uint32_t text_max = w > 80u ? w - 80u : 0u;
        wm_font_draw(&state->font, canvas, text_x, text_y,
                     state->dialog.title,
                     state->theme.text, state->theme.font_title, text_max);

        int32_t msg_x = x + 16;
        int32_t msg_y = y + (int32_t)title_h + 18;
        uint32_t msg_w = w > 32u ? w - 32u : 0u;
        wm_font_draw(&state->font, canvas, msg_x, msg_y,
                     state->dialog.message, state->theme.text,
                     state->theme.font_normal, msg_w);

        int32_t bx = x + (int32_t)(w - WM_DIALOG_BUTTON_WIDTH) / 2;
        int32_t by = y + (int32_t)title_h + h - 20 - (int32_t)WM_DIALOG_BUTTON_HEIGHT;
        wm_rect_t btn_rect = {bx, by, WM_DIALOG_BUTTON_WIDTH, WM_DIALOG_BUTTON_HEIGHT};
        uint32_t btn_color = state->dialog.hover_ok ?
            state->theme.accent_alt : state->theme.accent;
        wm_canvas_fill_rounded(canvas, btn_rect, 8u, btn_color);
        int32_t label_x = bx + (int32_t)(WM_DIALOG_BUTTON_WIDTH - 24u) / 2;
        int32_t label_y = by + ((int32_t)WM_DIALOG_BUTTON_HEIGHT - (int32_t)state->theme.font_normal) / 2;
        wm_font_draw(&state->font, canvas, label_x, label_y,
                     "OK", 0xFFFFFFFFu, state->theme.font_normal,
                     WM_DIALOG_BUTTON_WIDTH);
    }
}
