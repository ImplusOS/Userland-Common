#include "WM_Dialog.h"

#include "../Compositor/WM_Damage.h"
#include "../Compositor/WM_Raster.h"
#include "../Font/WM_Font.h"
#include "WM_Icons.h"

#include <string.h>

/*
 * A Material Design 3 basic dialog: an extra-large shape on
 * surface-container-high at elevation 3, a hero icon in the severity's
 * colour role, a headline, supporting text, and the confirming action as a
 * filled button at the end of the action row. It keeps a draggable title
 * bar, which M3 has nothing to say about, because it is a dialog on a
 * desktop rather than on a phone.
 */

#define WM_DIALOG_WIDTH 400u
#define WM_DIALOG_HEIGHT 190u
#define WM_DIALOG_BUTTON_WIDTH 96u
#define WM_DIALOG_BUTTON_HEIGHT 40u
#define WM_DIALOG_CLOSE_SIZE 32u
#define WM_DIALOG_PAD 24
#define WM_DIALOG_HERO 28u

static wm_rect_t screen_bounds(const wm_state_t *state)
{
    return (wm_rect_t){0, 0, state->compositor.framebuffer_width,
                       state->compositor.framebuffer_height};
}

/* Severity, in M3 colour roles rather than in traffic-light hex: an error
 * is the error role, a warning is tertiary, and anything else is primary.
 * All three follow the seed, so a retinted desktop retints these too. */
static uint32_t dialog_accent_color(const theme_t *scheme,
                                    wm_dialog_type_t type)
{
    switch (type) {
    case WM_DIALOG_ERROR:   return scheme->danger;
    case WM_DIALOG_WARNING: return scheme->accent;
    case WM_DIALOG_INFO:
    default:                return scheme->accent;
    }
}

static uint32_t dialog_accent_container(const theme_t *scheme,
                                        wm_dialog_type_t type)
{
    switch (type) {
    case WM_DIALOG_ERROR:   return scheme->danger;
    case WM_DIALOG_WARNING: return scheme->accent_soft;
    case WM_DIALOG_INFO:
    default:                return scheme->accent_soft;
    }
}

static wm_icon_kind_t dialog_icon(wm_dialog_type_t type)
{
    switch (type) {
    case WM_DIALOG_ERROR:   return WM_ICON_ERROR;
    case WM_DIALOG_WARNING: return WM_ICON_WARNING;
    case WM_DIALOG_INFO:
    default:                return WM_ICON_INFO;
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

/* The controls, in one place: the drawing code and the hit tests below both
 * ask, so a button is always clickable exactly where it is painted. */
static wm_rect_t dialog_close_rect(const wm_state_t *state)
{
    int32_t right = state->dialog.x + (int32_t)state->dialog.w - 8;
    int32_t by = state->dialog.y +
        ((int32_t)state->theme.title_height - (int32_t)WM_DIALOG_CLOSE_SIZE) / 2;
    return (wm_rect_t){right - (int32_t)WM_DIALOG_CLOSE_SIZE, by,
                       WM_DIALOG_CLOSE_SIZE, WM_DIALOG_CLOSE_SIZE};
}

/* M3 puts a dialog's actions at the end of the row, not in the middle. */
static wm_rect_t dialog_ok_rect(const wm_state_t *state)
{
    int32_t bx = state->dialog.x + (int32_t)state->dialog.w -
                 (int32_t)WM_DIALOG_BUTTON_WIDTH - WM_DIALOG_PAD;
    int32_t by = state->dialog.y + (int32_t)state->theme.title_height +
                 (int32_t)state->dialog.h - WM_DIALOG_PAD -
                 (int32_t)WM_DIALOG_BUTTON_HEIGHT;
    return (wm_rect_t){bx, by, WM_DIALOG_BUTTON_WIDTH, WM_DIALOG_BUTTON_HEIGHT};
}

bool wm_dialog_close_contains(const wm_state_t *state, int32_t x, int32_t y)
{
    if (!state || !state->dialog.active) return false;
    return wm_rect_intersects((wm_rect_t){x, y, 1u, 1u}, dialog_close_rect(state));
}

bool wm_dialog_ok_contains(const wm_state_t *state, int32_t x, int32_t y)
{
    if (!state || !state->dialog.active) return false;
    return wm_rect_intersects((wm_rect_t){x, y, 1u, 1u}, dialog_ok_rect(state));
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

    const theme_t *th = &state->theme.color;

    int32_t x = state->dialog.x;
    int32_t y = state->dialog.y;
    uint32_t w = state->dialog.w;
    uint32_t h = state->dialog.h;
    uint32_t title_h = state->theme.title_height;

    wm_rect_t dialog_rect = {x, y, w, h + title_h};
    if (!wm_rect_intersects(dialog_rect, canvas->clip)) return;

    uint32_t radius = THEME_RADIUS_LARGE;
    uint32_t container = th->surface;
    uint32_t accent = dialog_accent_color(th, state->dialog.type);

    wm_canvas_shadow(canvas, dialog_rect, radius, WM_SHADOW_POPUP,
                               color_alpha(0xFF000000u, 25u), 255u);
    wm_canvas_fill_rounded(canvas, dialog_rect, radius, th->border);
    wm_canvas_fill_rounded(canvas,
        (wm_rect_t){x + 1, y + 1, w > 2u ? w - 2u : w,
                    h + title_h > 2u ? h + title_h - 2u : 0u},
        radius > 0u ? radius - 1u : 0u, container);

    /* The title bar is the same container as the body -- M3 dialogs are one
     * surface -- and is here only so the thing can be dragged. */
    wm_rect_t close_btn = dialog_close_rect(state);
    if (state->dialog.hover_close) {
        wm_canvas_fill_rounded(canvas, close_btn,
            wm_pill_radius(close_btn.w, close_btn.h),
            th->danger);
    }
    wm_icon_draw(canvas,
        (wm_rect_t){close_btn.x + 7, close_btn.y + 7, 18u, 18u},
        WM_ICON_CLOSE,
        state->dialog.hover_close ? th->on_danger
                                  : th->text_dim);

    if (!state->font.loaded) return;

    /* Hero icon, headline, supporting text: M3's dialog stack, aligned to
     * the 24px content margin. */
    wm_rect_t hero = {x + WM_DIALOG_PAD, y + (int32_t)title_h + 4,
                      WM_DIALOG_HERO, WM_DIALOG_HERO};
    wm_canvas_fill_rounded(canvas,
        (wm_rect_t){hero.x - 8, hero.y - 8, hero.w + 16u, hero.h + 16u},
        wm_pill_radius(hero.w + 16u, hero.h + 16u),
        dialog_accent_container(th, state->dialog.type));
    wm_icon_draw(canvas, hero, dialog_icon(state->dialog.type), accent);

    int32_t text_x = hero.x + (int32_t)WM_DIALOG_HERO + 20;
    uint32_t text_max = w > (uint32_t)(text_x - x) + WM_DIALOG_PAD ?
        w - (uint32_t)(text_x - x) - WM_DIALOG_PAD : 0u;

    wm_font_draw(&state->font, canvas, text_x, hero.y - 2,
                 state->dialog.title, th->text,
                 THEME_FONT_TITLE, text_max);
    wm_font_draw(&state->font, canvas, text_x,
                 hero.y + (int32_t)(THEME_FONT_TITLE + 0.5f) + 8,
                 state->dialog.message, th->text_dim,
                 THEME_FONT_BODY, text_max);

    /* The confirming action: an M3 filled button. */
    wm_rect_t btn = dialog_ok_rect(state);
    uint32_t btn_container = state->dialog.hover_ok
        ? color_mix(th->accent, th->on_accent, THEME_HOVER_PCT)
        : th->accent;
    wm_canvas_fill_rounded(canvas, btn,
        wm_pill_radius(btn.w, btn.h), btn_container);
    uint32_t label_w = wm_font_measure(&state->font, "OK", THEME_FONT_BODY);
    wm_font_draw(&state->font, canvas,
                 btn.x + (int32_t)(btn.w - label_w) / 2,
                 btn.y + (int32_t)(btn.h - (uint32_t)THEME_FONT_BODY) / 2,
                 "OK", th->on_accent, THEME_FONT_BODY, btn.w);
}
