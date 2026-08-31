#include "WM_StartMenu.h"

#include "../Compositor/WM_Raster.h"
#include "../Font/WM_Font.h"
#include "WM_Taskbar.h"
#include "../../../../Userland/API/Process.h"

#include <string.h>
#include <stdio.h>

static bool str_icontains(const char *haystack, const char *needle)
{
    if (!needle || !needle[0]) return true;
    if (!haystack) return false;
    for (const char *h = haystack; *h; h++) {
        const char *n = needle;
        const char *p = h;
        while (*n && *p) {
            char ch = *p >= 'A' && *p <= 'Z' ? (char)(*p + 32) : *p;
            char cn = *n >= 'A' && *n <= 'Z' ? (char)(*n + 32) : *n;
            if (ch != cn) break;
            n++; p++;
        }
        if (!*n) return true;
    }
    return false;
}

bool wm_start_menu_input_char(wm_state_t *state, char ch)
{
    if (!state || !state->launcher_open) return false;
    if (ch < 0x20 || state->search_len >= WM_SEARCH_MAX - 1u) return false;
    state->search_text[state->search_len++] = ch;
    state->search_text[state->search_len] = '\0';
    state->launcher_scroll = 0u;
    return true;
}

bool wm_start_menu_input_backspace(wm_state_t *state)
{
    if (!state || !state->launcher_open || state->search_len == 0u) return false;
    state->search_text[--state->search_len] = '\0';
    state->launcher_scroll = 0u;
    return true;
}

wm_rect_t wm_start_menu_rect(const wm_state_t *state)
{
    if (!state) return (wm_rect_t){0, 0, 0, 0};
    wm_rect_t dock = wm_taskbar_rect(state);
    uint32_t width  = wm_min_u32(480u, state->compositor.framebuffer_width);
#if WM_TASKBAR_AT_TOP
    uint32_t max_h = state->compositor.framebuffer_height >
        (uint32_t)(dock.y + (int32_t)dock.h + 16u) ?
        state->compositor.framebuffer_height -
        (uint32_t)(dock.y + (int32_t)dock.h) - 16u : 0u;
    uint32_t height = wm_min_u32(560u, max_h);
    int32_t y = dock.y + (int32_t)dock.h + 6;
#else
    uint32_t max_h = dock.y > 16 ? (uint32_t)dock.y - 16u : 0u;
    uint32_t height = wm_min_u32(560u, max_h);
    int32_t y = dock.y - (int32_t)height - 6;
    if (y < 0) y = 0;
#endif
    return (wm_rect_t){4, y, width, height};
}

#define SM_HEADER_PAD_TOP   12u
#define SM_ICON_SIZE        32u
#define SM_HEADER_PAD_LEFT  16u
#define SM_HEADER_GAP       10u
#define SM_SEARCH_H         38u
#define SM_LABEL_PAD        14u
#define SM_CARD_PAD_X       8u
#define SM_CARD_PAD_Y       9u
#define SM_CARD_GAP         4u
#define SM_FOOTER_H         52u
#define SM_FOOTER_PAD       12u

static uint32_t sm_header_height(const wm_state_t *state)
{
    uint32_t title_line = (uint32_t)(state->theme.font_title + 0.5f) + 4u;
    uint32_t sub_line = (uint32_t)(state->theme.font_small + 0.5f) + 2u;
    return SM_HEADER_PAD_TOP + SM_ICON_SIZE + SM_HEADER_GAP +
           title_line + sub_line + SM_HEADER_PAD_TOP;
}

static uint32_t sm_label_height(const wm_state_t *state)
{
    return (uint32_t)(state->theme.font_small + 0.5f) + 8u;
}

static uint32_t sm_card_height(const wm_state_t *state)
{
    uint32_t name_h = (uint32_t)(state->theme.font_normal + 0.5f) + 4u;
    uint32_t sub_h = (uint32_t)(state->theme.font_small + 0.5f) + 2u;
    uint32_t content = SM_CARD_PAD_Y + name_h + 2u + sub_h + SM_CARD_PAD_Y;
    uint32_t icon_area = SM_CARD_PAD_X + 36u + SM_CARD_PAD_X;
    return content > icon_area ? content : icon_area;
}

static uint32_t sm_content_top(const wm_state_t *state)
{
    return sm_header_height(state) + 1u + SM_SEARCH_H + sm_label_height(state);
}

static uint32_t sm_viewport_bottom(const wm_state_t *state)
{
    return sm_content_top(state) + sm_card_height(state) + 8u;
}

static wm_rect_t app_viewport_rect(const wm_state_t *state)
{
    wm_rect_t menu = wm_start_menu_rect(state);
    uint32_t top = sm_content_top(state);
    uint32_t bottom = sm_viewport_bottom(state);
    uint32_t height = menu.h > bottom ? menu.h - bottom : 0u;
    uint32_t width  = menu.w > 24u ? menu.w - 24u : 0u;
    return (wm_rect_t){menu.x + 12, (int32_t)(menu.y + (int32_t)top), width, height};
}

static uint32_t visible_app_rows(const wm_state_t *state)
{
    wm_rect_t vp = app_viewport_rect(state);
    uint32_t stride = sm_card_height(state) + SM_CARD_GAP;
    uint32_t rows = stride ? (vp.h + SM_CARD_GAP) / stride : 1u;
    return rows == 0u ? 1u : rows;
}

static uint32_t filtered_count(const wm_state_t *state)
{
    return state->filter_count;
}

static void rebuild_filter_cache(wm_state_t *state)
{
    state->filter_count = 0u;
    for (uint32_t i = 0; i < state->assets.app_count; i++) {
        if (str_icontains(state->assets.apps[i].name, state->search_text)) {
            if (state->filter_count < WM_MAX_LAUNCHER_APPS)
                state->filter_indices[state->filter_count++] = i;
        }
    }
}

static void ensure_filter_cache(wm_state_t *state)
{
    if (state->filter_generation != state->search_len) {
        rebuild_filter_cache(state);
        state->filter_generation = state->search_len;
    }
}

static uint32_t max_scroll_row(const wm_state_t *state)
{
    uint32_t total   = filtered_count(state);
    uint32_t visible = visible_app_rows(state);
    return total > visible ? total - visible : 0u;
}

void wm_start_menu_clamp_scroll(wm_state_t *state)
{
    if (!state) return;
    uint32_t max_scroll = max_scroll_row(state);
    if (state->launcher_scroll > max_scroll) state->launcher_scroll = max_scroll;
}

bool wm_start_menu_scroll(wm_state_t *state, int32_t rows)
{
    if (!state || !state->launcher_open || rows == 0) return false;
    uint32_t old = state->launcher_scroll;
    uint32_t max_scroll = max_scroll_row(state);
    int32_t next = (int32_t)state->launcher_scroll + rows;
    if (next < 0) next = 0;
    if ((uint32_t)next > max_scroll) next = (int32_t)max_scroll;
    state->launcher_scroll = (uint32_t)next;
    return old != state->launcher_scroll;
}

static wm_rect_t filtered_card_rect(const wm_state_t *state, uint32_t visual_row)
{
    wm_rect_t vp = app_viewport_rect(state);
    uint32_t card_h = sm_card_height(state);
    return (wm_rect_t){
        vp.x,
        vp.y + (int32_t)(visual_row * (card_h + SM_CARD_GAP)),
        vp.w,
        card_h
    };
}

void wm_start_menu_draw(wm_state_t *state, wm_canvas_t *canvas)
{
    if (!state || !canvas || !state->launcher_open) return;
    wm_rect_t menu = wm_start_menu_rect(state);
    if (menu.w == 0u || menu.h == 0u) return;
    if (!wm_rect_intersects(menu, canvas->clip)) return;
    ensure_filter_cache(state);
    wm_start_menu_clamp_scroll(state);

    float ft = state->theme.font_title;
    float fn = state->theme.font_normal;
    float fs = state->theme.font_small;

    wm_canvas_fill_rounded(canvas,
        (wm_rect_t){menu.x + 3, menu.y + 5, menu.w, menu.h},
        16u, 0x1A000000u);
    wm_canvas_fill_rounded(canvas, menu, 16u, state->theme.border);
    uint32_t menu_tint = (state->theme.surface & 0x00FFFFFFu) | (0xCCu << 24u);
    wm_canvas_fill_rounded(canvas,
        (wm_rect_t){menu.x + 1, menu.y + 1, menu.w - 2u, menu.h - 2u},
        15u, menu_tint);

    {
        wm_rect_t circle = {menu.x + (int32_t)SM_HEADER_PAD_LEFT,
                            menu.y + (int32_t)SM_HEADER_PAD_TOP,
                            SM_ICON_SIZE, SM_ICON_SIZE};
        wm_canvas_fill_rounded(canvas, circle, 16u,
            state->theme.text | 0xFF000000u);
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++)
                wm_canvas_fill(canvas,
                    (wm_rect_t){circle.x + 9 + c*5, circle.y + 9 + r*5, 3u, 3u},
                    0xFFFFFFFFu);
        int32_t text_x = menu.x + (int32_t)SM_HEADER_PAD_LEFT + (int32_t)SM_ICON_SIZE + 10;
        uint32_t text_max = menu.w > (SM_HEADER_PAD_LEFT + SM_ICON_SIZE + 20u) ?
            menu.w - SM_HEADER_PAD_LEFT - SM_ICON_SIZE - 20u : 0u;
        wm_font_draw(&state->font, canvas, text_x,
                     menu.y + (int32_t)SM_HEADER_PAD_TOP,
                     "Applications", state->theme.text, ft, text_max);
        wm_font_draw(&state->font, canvas, text_x,
                     menu.y + (int32_t)SM_HEADER_PAD_TOP + (int32_t)(ft + 0.5f) + 2,
                     "ImplusOS Workspace", state->theme.text_dim, fs, text_max);
    }

    uint32_t sep_y = sm_header_height(state);
    wm_canvas_fill(canvas,
        (wm_rect_t){menu.x + 12, menu.y + (int32_t)sep_y, menu.w - 24u, 1u},
        state->theme.border);

    {
        int32_t sb_y = menu.y + (int32_t)sep_y + 1;
        wm_rect_t sb = {menu.x + 12, sb_y, menu.w - 24u, SM_SEARCH_H};
        wm_canvas_fill_rounded(canvas, sb, 12u, state->theme.surface_alt);
        uint32_t isz = 16u;
        wm_canvas_draw_icon(canvas,
            (wm_rect_t){sb.x + 10, sb.y + (int32_t)(sb.h - isz)/2, isz, isz},
            &state->assets.system_icons.search, 160u, 2u, state->theme.text_dim);
        const char *display = state->search_len > 0u ?
            state->search_text : "Search apps...";
        uint32_t tc = state->search_len > 0u ?
            state->theme.text : state->theme.text_dim;
        wm_font_draw(&state->font, canvas, sb.x + 34, sb.y + 10,
                     display, tc, fn, sb.w > 44u ? sb.w - 44u : 0u);
        if (state->search_active && state->search_len > 0u) {
            uint32_t cursor_x = (uint32_t)(sb.x + 34) +
                wm_font_measure(&state->font, state->search_text, fn);
            wm_canvas_fill(canvas,
                (wm_rect_t){(int32_t)cursor_x, sb.y + 9, 1u, 16u},
                state->theme.text | 0xFF000000u);
        }
        if (state->search_active) {
            wm_canvas_fill_rounded(canvas,
                (wm_rect_t){sb.x, sb.y + (int32_t)sb.h - 2, sb.w, 2u},
                1u, state->theme.accent | 0xFF000000u);
        }
    }

    {
        int32_t label_y = menu.y + (int32_t)sep_y + 1 + (int32_t)SM_SEARCH_H;
        const char *label = state->search_len > 0u ? "Search results" : "All Apps";
        wm_font_draw(&state->font, canvas, menu.x + (int32_t)SM_LABEL_PAD, label_y,
                     label, state->theme.text_dim, fs,
                     menu.w > 28u ? menu.w - 28u : 0u);
    }

    {
        wm_rect_t vp = app_viewport_rect(state);
        wm_rect_t old_clip = canvas->clip;
        wm_canvas_set_clip(canvas, wm_rect_intersection(old_clip, vp));

        uint32_t total = state->filter_count;
        uint32_t first_row = state->launcher_scroll;
        uint32_t vis_rows = visible_app_rows(state);
        uint32_t last_row = first_row + vis_rows;
        if (last_row > total) last_row = total;

        for (uint32_t row = first_row; row < last_row; ++row) {
            uint32_t app_index = state->filter_indices[row];
            wm_launcher_app_t *app = &state->assets.apps[app_index];
            wm_rect_t card = filtered_card_rect(state, row - first_row);

            bool hover = state->launcher_hover_index == (int32_t)app_index;
            if (hover) {
                wm_canvas_fill_rounded(canvas, card, 10u, state->theme.surface_hover);
            }
            wm_rect_t icon_rect = {card.x + 8, card.y + 8, 36u, 36u};
            if (app->icon_pixels) {
                wm_canvas_blit_scaled(canvas, icon_rect, app->icon_pixels,
                                      app->icon_width, app->icon_height, 255u, 8u);
            } else {
                wm_canvas_fill_rounded(canvas, icon_rect, 10u,
                    state->theme.surface_alt);
                wm_font_draw(&state->font, canvas,
                             icon_rect.x + (int32_t)(icon_rect.w - 16u)/2,
                             icon_rect.y + (int32_t)(icon_rect.h - 13u)/2,
                             app->badge[0] ? app->badge : "?",
                             state->theme.text, fn, 24u);
            }
            int32_t name_y = card.y + (int32_t)SM_CARD_PAD_Y;
            int32_t sub_y = name_y + (int32_t)(fn + 0.5f) + 2;
            wm_font_draw(&state->font, canvas,
                         card.x + 54, name_y,
                         app->name, state->theme.text, fn,
                         card.w > 64u ? card.w - 64u : 0u);
            wm_font_draw(&state->font, canvas,
                         card.x + 54, sub_y,
                         "Application", state->theme.text_dim, fs,
                         card.w > 64u ? card.w - 64u : 0u);
        }

        if (total == 0u) {
            wm_font_draw(&state->font, canvas,
                         vp.x + 12, vp.y + 20,
                         "No results found.", state->theme.text_dim,
                         fn, vp.w > 24u ? vp.w - 24u : 0u);
        }

        uint32_t max_scroll = max_scroll_row(state);
        if (max_scroll > 0u && vp.h >= 24u) {
            wm_rect_t track = {vp.x + (int32_t)vp.w - 4, vp.y, 3u, vp.h};
            wm_canvas_fill_rounded(canvas, track, 2u, state->theme.border);
            uint32_t total_rows = total;
            uint32_t thumb_h = total_rows ?
                (uint32_t)(((uint64_t)visible_app_rows(state) * vp.h) / total_rows) : vp.h;
            if (thumb_h < 20u) thumb_h = 20u;
            if (thumb_h > vp.h) thumb_h = vp.h;
            uint32_t travel = vp.h - thumb_h;
            uint32_t thumb_y = max_scroll ?
                (uint32_t)(((uint64_t)state->launcher_scroll * travel) / max_scroll) : 0u;
            wm_canvas_fill_rounded(canvas,
                (wm_rect_t){track.x, track.y + (int32_t)thumb_y, 3u, thumb_h},
                2u, state->theme.text | 0xFF000000u);
        }
        canvas->clip = old_clip;
    }

    {
        uint32_t footer_top = menu.h > SM_FOOTER_H ? menu.h - SM_FOOTER_H : 0u;
        wm_rect_t footer = {menu.x + 1, menu.y + (int32_t)footer_top,
                            menu.w - 2u, SM_FOOTER_H};
        wm_canvas_fill(canvas,
            (wm_rect_t){menu.x + 12, footer.y, menu.w - 24u, 1u},
            state->theme.border);
        wm_canvas_fill_rounded(canvas,
            (wm_rect_t){footer.x + (int32_t)SM_FOOTER_PAD, footer.y + 10, 32u, 32u},
            16u, state->theme.surface_alt);
        wm_font_draw(&state->font, canvas,
                     footer.x + 16, footer.y + 17,
                     "U", state->theme.text, ft > 14.0f ? 14.0f : ft, 20u);
        wm_font_draw(&state->font, canvas,
                     footer.x + 52, footer.y + 16,
                     "ImplusOS User", state->theme.text, fn,
                     menu.w > 180u ? menu.w - 180u : 0u);
        int32_t btn_right = menu.x + (int32_t)menu.w - 12;
        wm_rect_t sd = {btn_right - 32, footer.y + 10, 32u, 32u};
        wm_rect_t rb = {btn_right - 70, footer.y + 10, 32u, 32u};
        wm_canvas_fill_rounded(canvas, rb, 16u, state->theme.surface_hover);
        wm_canvas_fill_rounded(canvas, sd, 16u, state->theme.surface_hover);
        wm_canvas_draw_icon(canvas,
            (wm_rect_t){rb.x+7, rb.y+7, 18u, 18u},
            &state->assets.system_icons.reboot, 220u, 2u, state->theme.text);
        wm_canvas_draw_icon(canvas,
            (wm_rect_t){sd.x+7, sd.y+7, 18u, 18u},
            &state->assets.system_icons.power, 220u, 2u, state->theme.danger);
    }
}

wm_launcher_action_t wm_start_menu_hit_test(wm_state_t *state, int32_t x, int32_t y)
{
    wm_launcher_action_t result = {WM_LAUNCHER_ACTION_NONE, 0u};
    if (!state || !state->launcher_open) return result;
    wm_rect_t menu = wm_start_menu_rect(state);
    if (!wm_rect_intersects((wm_rect_t){x, y, 1u, 1u}, menu)) return result;

    int32_t btn_right = menu.x + (int32_t)menu.w - 12;
    uint32_t footer_top = menu.h > SM_FOOTER_H ? menu.h - SM_FOOTER_H : 0u;
    int32_t footer_y = menu.y + (int32_t)footer_top;
    if (y >= footer_y + 10 && y < footer_y + 42) {
        if (x >= btn_right - 70 && x < btn_right - 38) {
            result.kind = WM_LAUNCHER_ACTION_REBOOT;
            return result;
        }
        if (x >= btn_right - 32 && x < btn_right) {
            result.kind = WM_LAUNCHER_ACTION_SHUTDOWN;
            return result;
        }
    }

    wm_rect_t vp = app_viewport_rect(state);
    if (wm_rect_intersects((wm_rect_t){x, y, 1u, 1u}, vp)) {
        ensure_filter_cache(state);
        uint32_t total = state->filter_count;
        uint32_t first_row = state->launcher_scroll;
        uint32_t vis_rows = visible_app_rows(state);
        uint32_t last_row = first_row + vis_rows;
        if (last_row > total) last_row = total;
        for (uint32_t row = first_row; row < last_row; ++row) {
            uint32_t app_index = state->filter_indices[row];
            wm_rect_t card = filtered_card_rect(state, row - first_row);
            if (wm_rect_intersects((wm_rect_t){x, y, 1u, 1u}, card)) {
                result.kind = WM_LAUNCHER_ACTION_APP;
                result.app_index = app_index;
                return result;
            }
        }
    }
    return result;
}
