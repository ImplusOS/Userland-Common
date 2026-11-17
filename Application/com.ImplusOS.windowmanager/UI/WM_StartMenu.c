#include "WM_StartMenu.h"

#include "../Compositor/WM_Raster.h"
#include "../Font/WM_Font.h"
#include "WM_Taskbar.h"
#include "WM_Icons.h"
#include "../Core/WM_Assets.h"
#include "../Animation/WM_Animation.h"
#include "../Linux/WM_DesktopEntry.h"
#include "../../../../Userland/API/Source/Process.h"
#include "../../../../Userland/API/Source/File.h"

#include <string.h>
#include <stdio.h>

/* Logged-in user, read once from the session file the login screen writes. */
static const char *sm_session_user(void)
{
    static char name[64];
    static int loaded = 0;
    if (!loaded) {
        loaded = 1;
        name[0] = '\0';
        int32_t fd = file_open("/run/session.user", 0);
        if (fd >= 0) {
            int64_t n = file_read(fd, name, sizeof(name) - 1);
            file_close(fd);
            if (n > 0) {
                name[n] = '\0';
                for (char *p = name; *p; ++p) if (*p == '\n' || *p == '\r') { *p = '\0'; break; }
            }
        }
        if (!name[0]) { name[0] = 'U'; name[1] = 's'; name[2] = 'e'; name[3] = 'r'; name[4] = '\0'; }
    }
    return name;
}

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

/* What a search matches against. The command line is in there on purpose:
 * typing "xterm" or "chrome" finds the entry whose display name is
 * "ターミナル" or "Chromium", which is how a Linux program is usually
 * remembered. */
static bool app_matches(const wm_launcher_app_t *app, const char *query)
{
    if (!query || !query[0]) return true;
    return str_icontains(app->name, query) ||
           str_icontains(app->comment, query) ||
           str_icontains(app->argument, query) ||
           str_icontains(app->path, query) ||
           str_icontains(wm_app_category_label(app->category), query);
}


bool wm_start_menu_input_char(wm_state_t *state, char ch)
{
    if (!state || !state->launcher_open) return false;
    if (ch < 0x20 || state->search_len >= WM_SEARCH_MAX - 1u) return false;
    state->search_text[state->search_len++] = ch;
    state->search_text[state->search_len] = '\0';
    wm_scroll_reset(&state->launcher_scroll);
    /* A new query means new results: whatever was selected is about to be a
     * different program, or gone. */
    state->launcher_selected_index = -1;
    return true;
}

bool wm_start_menu_input_backspace(wm_state_t *state)
{
    if (!state || !state->launcher_open || state->search_len == 0u) return false;
    state->search_text[--state->search_len] = '\0';
    wm_scroll_reset(&state->launcher_scroll);
    state->launcher_selected_index = -1;
    return true;
}

wm_rect_t wm_start_menu_rect(const wm_state_t *state)
{
    if (!state) return (wm_rect_t){0, 0, 0, 0};
    wm_rect_t dock = wm_taskbar_rect(state);
    uint32_t width  = wm_min_u32(520u, state->compositor.framebuffer_width);
#if WM_TASKBAR_AT_TOP
    uint32_t max_h = state->compositor.framebuffer_height >
        (uint32_t)(dock.y + (int32_t)dock.h + 16u) ?
        state->compositor.framebuffer_height -
        (uint32_t)(dock.y + (int32_t)dock.h) - 16u : 0u;
    uint32_t height = wm_min_u32(680u, max_h);
    int32_t y = dock.y + (int32_t)dock.h + 6;
#else
    uint32_t max_h = dock.y > 16 ? (uint32_t)dock.y - 16u : 0u;
    uint32_t height = wm_min_u32(680u, max_h);
    int32_t y = dock.y - (int32_t)height - 6;
    if (y < 0) y = 0;
    wm_rect_t rest = {4, y, width, height};
    y += wm_panel_slide_offset(&state->launcher_anim, rest, dock.y);
#endif
    return (wm_rect_t){4, y, width, height};
}

#define SM_HEADER_PAD_TOP   12u
#define SM_SEARCH_H         44u
#define SM_LABEL_PAD        14u
#define SM_CARD_PAD_X       4u   /* 36px icon + 4px each side = 44px rows */
#define SM_CARD_PAD_Y       9u
#define SM_CARD_GAP         2u
#define SM_FOOTER_H         56u
#define SM_FOOTER_PAD       12u
#define SM_SESSION_BTN      40u

/* There is no header any more -- the signed-in user is shown in the
 * footer -- so this is only the space above the search bar (which is drawn
 * SM_SEARCH_GAP below it): 12px from the menu's top edge. */
static uint32_t sm_header_height(void)
{
    return SM_HEADER_PAD_TOP - 5u;   /* minus SM_SEARCH_GAP */
}

static uint32_t sm_label_height(void)
{
    /* 10px above the overline text (it would otherwise sit against the
     * search bar), 8px below it. */
    return (uint32_t)(THEME_FONT_SMALL + 0.5f) + 18u;
}

static uint32_t sm_card_height(void)
{
    /* One line per app: just its name, no description under it. */
    uint32_t name_h = (uint32_t)(THEME_FONT_BODY + 0.5f) + 4u;
    uint32_t content = SM_CARD_PAD_Y + name_h + SM_CARD_PAD_Y;
    uint32_t icon_area = SM_CARD_PAD_X + 36u + SM_CARD_PAD_X;
    return content > icon_area ? content : icon_area;
}

/* Vertical offset of the application list inside the menu. The 5px is the
 * gap under the header divider that the search bar is drawn in; keeping it
 * here means the list, the list's overline and the search bar all measure
 * from the same place. */
#define SM_SEARCH_GAP 5u

static uint32_t sm_content_top(void)
{
    return sm_header_height() + SM_SEARCH_GAP + SM_SEARCH_H +
           sm_label_height();
}

static uint32_t sm_viewport_bottom(void)
{
    return sm_content_top() + sm_card_height() + 8u;
}

static wm_rect_t app_viewport_rect(const wm_state_t *state)
{
    wm_rect_t menu = wm_start_menu_rect(state);
    uint32_t top = sm_content_top();
    uint32_t bottom = sm_viewport_bottom();
    uint32_t height = menu.h > bottom ? menu.h - bottom : 0u;
    uint32_t width  = menu.w > 24u ? menu.w - 24u : 0u;
    return (wm_rect_t){menu.x + 12, (int32_t)(menu.y + (int32_t)top), width, height};
}

/* Every row is one app card, so the list is a fixed pitch and the scroll
 * position is a plain pixel offset into it. */
static uint32_t row_pitch(void)
{
    return sm_card_height() + SM_CARD_GAP;
}

static uint32_t content_height(const wm_state_t *state)
{
    return state->filter_count == 0u ? 0u :
        state->filter_count * row_pitch() - SM_CARD_GAP;
}

/* Rows for the current search: every matching app, in the order
 * wm_assets_init_metadata() sorted them (by name -- symbols, letters,
 * digits). There are no category headings any more. */
static void rebuild_filter_cache(wm_state_t *state)
{
    const uint32_t capacity =
        (uint32_t)(sizeof(state->menu_rows) / sizeof(state->menu_rows[0]));

    state->filter_count = 0u;
    for (uint32_t i = 0; i < state->assets.app_count; i++) {
        const wm_launcher_app_t *app = &state->assets.apps[i];
        if (!app_matches(app, state->search_text)) continue;
        if (state->filter_count >= capacity) break;
        state->menu_rows[state->filter_count++] = (uint16_t)i;
    }
}

/* The cache is keyed on both the query length and how many apps there are:
 * the launcher list keeps growing after startup as deferred work adds
 * entries, and a rebuild keyed on the query alone would keep serving rows
 * that predate them. Neither field can make this equal the UINT32_MAX
 * sentinel wm_service_init() starts from. */
static uint32_t filter_key(const wm_state_t *state)
{
    return (state->search_len << 16) | state->assets.app_count;
}

static void ensure_filter_cache(wm_state_t *state)
{
    if (state->filter_generation != filter_key(state)) {
        rebuild_filter_cache(state);
        state->filter_generation = filter_key(state);
    }
}

static float max_scroll(const wm_state_t *state)
{
    wm_rect_t vp = app_viewport_rect(state);
    uint32_t content = content_height(state);
    return content > vp.h ? (float)(content - vp.h) : 0.0f;
}

void wm_start_menu_clamp_scroll(wm_state_t *state)
{
    if (!state) return;
    ensure_filter_cache(state);
    wm_scroll_clamp(&state->launcher_scroll, max_scroll(state));
}

bool wm_start_menu_scroll(wm_state_t *state, int32_t delta_px)
{
    if (!state || !state->launcher_open || delta_px == 0) return false;
    ensure_filter_cache(state);
    return wm_scroll_to(&state->launcher_scroll,
                        state->launcher_scroll.target + (float)delta_px,
                        max_scroll(state));
}

bool wm_start_menu_scroll_page(wm_state_t *state, int32_t pages)
{
    if (!state) return false;
    /* A page keeps one card of overlap, so the eye has something to
     * follow across the jump. */
    wm_rect_t vp = app_viewport_rect(state);
    int32_t page = (int32_t)vp.h - (int32_t)row_pitch();
    if (page < (int32_t)row_pitch()) page = (int32_t)row_pitch();
    return wm_start_menu_scroll(state, pages * page);
}

/* Card for `row` at the current scroll position. */
static wm_rect_t card_rect(const wm_state_t *state, uint32_t row)
{
    wm_rect_t vp = app_viewport_rect(state);
    return (wm_rect_t){
        vp.x,
        vp.y + (int32_t)(row * row_pitch()) -
            wm_scroll_offset(&state->launcher_scroll),
        vp.w,
        sm_card_height()
    };
}

/* Rows [*first, *last) that are at least partly inside the viewport. */
static void visible_rows(const wm_state_t *state, uint32_t *first, uint32_t *last)
{
    wm_rect_t vp = app_viewport_rect(state);
    uint32_t offset = (uint32_t)wm_scroll_offset(&state->launcher_scroll);
    uint32_t pitch = row_pitch();
    *first = offset / pitch;
    *last = (offset + vp.h + pitch - 1u) / pitch + 1u;
    if (*last > state->filter_count) *last = state->filter_count;
    if (*first > *last) *first = *last;
}

/* The two session buttons in the footer. Shared by the drawing code and
 * the hit test so one cannot drift from the other. */
static wm_rect_t sm_footer_rect(wm_rect_t menu)
{
    uint32_t footer_top = menu.h > SM_FOOTER_H ? menu.h - SM_FOOTER_H : 0u;
    return (wm_rect_t){menu.x + 1, menu.y + (int32_t)footer_top,
                       menu.w - 2u, SM_FOOTER_H};
}

static wm_rect_t sm_session_button_rect(wm_rect_t menu, bool shutdown)
{
    wm_rect_t footer = sm_footer_rect(menu);
    int32_t right = menu.x + (int32_t)menu.w - 12;
    int32_t x = shutdown ? right - (int32_t)SM_SESSION_BTN
                         : right - (int32_t)(SM_SESSION_BTN * 2u) - 4;
    return (wm_rect_t){x, footer.y + 8, SM_SESSION_BTN, SM_SESSION_BTN};
}

/*
 * The Start menu is an M3 surface at elevation 3: an extra-large shape on
 * surface-container, a search bar, and a list whose rows are navigation
 * drawer items -- a full-shape container that the pointer tints with a
 * state layer and the keyboard fills with secondary-container.
 */
static void draw_menu(wm_state_t *state, wm_canvas_t *canvas);

void wm_start_menu_draw(wm_state_t *state, wm_canvas_t *canvas)
{
    if (!state || !canvas ||
        !wm_panel_visible(&state->launcher_anim, state->launcher_open)) return;
    wm_rect_t clip = wm_panel_begin_clip(state, canvas, &state->launcher_anim);
    draw_menu(state, canvas);
    wm_canvas_set_clip(canvas, clip);
}

static void draw_menu(wm_state_t *state, wm_canvas_t *canvas)
{
    if (!state || !canvas ||
        !wm_panel_visible(&state->launcher_anim, state->launcher_open)) return;
    wm_rect_t menu = wm_start_menu_rect(state);
    if (menu.w == 0u || menu.h == 0u) return;
    /* The shadow reaches past the menu, so a damage rectangle that only
     * touches the shadow still has to repaint it. */
    wm_rect_t halo = {menu.x - 12, menu.y - 6, menu.w + 24u, menu.h + 24u};
    if (!wm_rect_intersects(halo, canvas->clip)) return;
    ensure_filter_cache(state);
    wm_start_menu_clamp_scroll(state);

    const theme_t *th = &state->theme.color;
    uint32_t radius = THEME_RADIUS_LARGE;
    uint32_t container = th->surface;

    wm_canvas_popup_surface(canvas, menu, radius, WM_SHADOW_POPUP,
                            color_alpha(0xFF000000u, 22u), 255u,
                            th->border, container);

    uint32_t sep_y = sm_header_height();

    /* --- M3 search bar: a full-shape container on surface-container-high,
     * outlined in primary while it has the keyboard --- */
    {
        int32_t sb_y = menu.y + (int32_t)sep_y + (int32_t)SM_SEARCH_GAP;
        wm_rect_t sb = {menu.x + 12, sb_y, menu.w - 24u, SM_SEARCH_H};
        uint32_t sb_radius = wm_pill_radius(sb.w, sb.h);
        /* The bar's container is the menu's own surface: nothing to fill. */
        if (state->search_active)
            wm_canvas_stroke_rounded(canvas, sb, sb_radius, 2u, th->accent);
        uint32_t isz = 20u;
        wm_icon_draw(canvas,
            (wm_rect_t){sb.x + 14, sb.y + (int32_t)(sb.h - isz)/2, isz, isz},
            WM_ICON_SEARCH, th->text_dim);
        const char *display = state->search_len > 0u ?
            state->search_text : "アプリを検索";
        uint32_t tc = state->search_len > 0u ?
            th->text : th->text_dim;
        int32_t text_x = sb.x + 46;
        int32_t text_y = sb.y + ((int32_t)sb.h - (int32_t)THEME_FONT_LARGE) / 2;
        wm_font_draw(&state->font, canvas, text_x, text_y,
                     display, tc, THEME_FONT_LARGE,
                     sb.w > 60u ? sb.w - 60u : 0u);
        if (state->search_active && state->search_len > 0u) {
            uint32_t cursor_x = (uint32_t)text_x +
                wm_font_measure(&state->font, state->search_text, THEME_FONT_LARGE);
            wm_canvas_fill(canvas,
                (wm_rect_t){(int32_t)cursor_x, text_y - 2,
                            2u, (uint32_t)THEME_FONT_LARGE + 4u},
                th->accent);
        }
    }

    /* --- list overline --- */
    {
        int32_t label_y = menu.y + (int32_t)sep_y + (int32_t)SM_SEARCH_GAP +
                          (int32_t)SM_SEARCH_H + 10;
        char label[64];
        (void)snprintf(label, sizeof(label), "%s (%u)",
                       state->search_len > 0u ? "検索結果" : "すべてのアプリ",
                       (unsigned)state->assets.app_count);
        if (state->search_len > 0u)
            (void)snprintf(label, sizeof(label), "検索結果 (%u)",
                           (unsigned)state->filter_count);
        wm_font_draw(&state->font, canvas, menu.x + (int32_t)SM_LABEL_PAD, label_y,
                     label, th->text_dim, THEME_FONT_SMALL,
                     menu.w > 28u ? menu.w - 28u : 0u);
    }

    /* --- the application list --- */
    {
        wm_rect_t vp = app_viewport_rect(state);
        wm_rect_t old_clip = canvas->clip;
        wm_canvas_set_clip(canvas, wm_rect_intersection(old_clip, vp));
        if (canvas->clip.w == 0u || canvas->clip.h == 0u) goto list_done;

        uint32_t total = state->filter_count;
        uint32_t first_row, last_row;
        visible_rows(state, &first_row, &last_row);

        for (uint32_t row = first_row; row < last_row; ++row) {
            wm_rect_t card = card_rect(state, row);
            uint32_t app_index = state->menu_rows[row];
            wm_launcher_app_t *app = &state->assets.apps[app_index];

            /* The pointer wins while it is over a card; otherwise the
             * keyboard selection is what is highlighted. M3 draws the two
             * differently on purpose: a hover is a state layer that fades
             * with the cursor, a selection is a filled container that
             * stays put. */
            bool pointed = state->launcher_hover_index == (int32_t)app_index;
            bool selected = state->launcher_hover_index < 0 &&
                state->launcher_selected_index == (int32_t)app_index;
            uint32_t row_radius = wm_pill_radius(card.w, card.h);
            uint32_t row_container = container;
            uint32_t on_row = th->text;
            uint32_t on_row_dim = th->text_dim;
            if (selected) {
                row_container = th->accent_soft;
                on_row = th->text;
                on_row_dim = th->text;
                wm_canvas_fill_rounded(canvas, card, row_radius, row_container);
            } else if (pointed) {
                row_container = color_mix(container, th->text,
                                                  THEME_HOVER_PCT);
                wm_canvas_fill_rounded(canvas, card, row_radius, row_container);
            }

            wm_rect_t icon_rect = {card.x + 10, card.y + (int32_t)(card.h - 36u) / 2,
                                   36u, 36u};
            if (app->icon_pixels) {
                wm_canvas_blit_scaled(canvas, icon_rect, app->icon_pixels,
                                      app->icon_width, app->icon_height, 255u,
                                      THEME_RADIUS);
            } else if (app->vector_icon >= 0) {
                wm_canvas_fill_rounded(canvas, icon_rect,
                    wm_pill_radius(icon_rect.w, icon_rect.h),
                    th->accent_soft);
                wm_icon_draw(canvas,
                    (wm_rect_t){icon_rect.x + 8, icon_rect.y + 8,
                                icon_rect.w - 16u, icon_rect.h - 16u},
                    (wm_icon_kind_t)app->vector_icon, th->text);
            } else {
                wm_canvas_fill_rounded(canvas, icon_rect,
                    wm_pill_radius(icon_rect.w, icon_rect.h),
                    th->accent_soft);
                const char *badge = app->badge[0] ? app->badge : "?";
                uint32_t badge_w = wm_font_measure(&state->font, badge,
                                                   THEME_FONT_BODY);
                wm_font_draw(&state->font, canvas,
                             icon_rect.x + (int32_t)(icon_rect.w - badge_w)/2,
                             icon_rect.y +
                                 (int32_t)(icon_rect.h - (uint32_t)THEME_FONT_BODY)/2,
                             badge, th->text,
                             THEME_FONT_BODY, 32u);
            }

            /* An M3 assist chip on the entries that run a foreign binary.
             * It is the one thing a user cannot tell from the name: whether
             * picking this brings up an X server and a glibc process, which
             * costs seconds on a cold start and behaves like a Linux
             * program once it is up. */
            uint32_t text_limit = card.w > 72u ? card.w - 72u : 0u;
            if (app->linux_backed) {
                const char *chip = "Linux";
                uint32_t chip_w = wm_font_measure(&state->font, chip,
                                                  THEME_FONT_SMALL) + 18u;
                uint32_t chip_h = (uint32_t)(THEME_FONT_SMALL + 0.5f) + 10u;
                wm_rect_t chip_rect = {card.x + (int32_t)card.w - (int32_t)chip_w - 14,
                                       card.y + (int32_t)(card.h - chip_h) / 2,
                                       chip_w, chip_h};
                wm_canvas_stroke_rounded(canvas, chip_rect,
                    THEME_RADIUS,
                    1u, th->border);
                wm_font_draw(&state->font, canvas, chip_rect.x + 9,
                             chip_rect.y + 5, chip, on_row_dim,
                             THEME_FONT_SMALL, chip_w);
                uint32_t consumed = chip_w + 20u;
                text_limit = text_limit > consumed ? text_limit - consumed : 0u;
            }

            int32_t name_y = card.y +
                ((int32_t)card.h - (int32_t)THEME_FONT_BODY) / 2;
            wm_font_draw(&state->font, canvas,
                         card.x + 58, name_y,
                         app->name, on_row, THEME_FONT_BODY, text_limit);
        }

        if (total == 0u) {
            wm_font_draw(&state->font, canvas,
                         vp.x + 16, vp.y + 20,
                         "一致するアプリはありません", th->text_dim,
                         THEME_FONT_BODY, vp.w > 32u ? vp.w - 32u : 0u);
        }

        float limit = max_scroll(state);
        uint32_t content = content_height(state);
        if (limit > 0.0f && vp.h >= 24u && content > 0u) {
            wm_rect_t track = {vp.x + (int32_t)vp.w - 5, vp.y, 4u, vp.h};
            wm_canvas_fill_rounded(canvas, track, 2u, th->border);
            uint32_t thumb_h = (uint32_t)(((uint64_t)vp.h * vp.h) / content);
            if (thumb_h < 24u) thumb_h = 24u;
            if (thumb_h > vp.h) thumb_h = vp.h;
            uint32_t travel = vp.h - thumb_h;
            float at = (float)wm_scroll_offset(&state->launcher_scroll) / limit;
            if (at > 1.0f) at = 1.0f;
            uint32_t thumb_y = (uint32_t)(at * (float)travel + 0.5f);
            wm_canvas_fill_rounded(canvas,
                (wm_rect_t){track.x, track.y + (int32_t)thumb_y, 4u, thumb_h},
                2u, th->text_dim);
        }
list_done:
        canvas->clip = old_clip;
    }

    /* --- footer: the session, and the two ways out of it --- */
    {
        wm_rect_t footer = sm_footer_rect(menu);
        wm_canvas_fill(canvas,
            (wm_rect_t){menu.x + 12, footer.y, menu.w - 24u, 1u},
            th->border);

        wm_rect_t avatar = {footer.x + (int32_t)SM_FOOTER_PAD, footer.y + 12, 32u, 32u};
        wm_canvas_fill_rounded(canvas, avatar,
            wm_pill_radius(avatar.w, avatar.h),
            th->accent_soft);
        char initial[4] = {sm_session_user()[0], '\0', '\0', '\0'};
        uint32_t initial_w = wm_font_measure(&state->font, initial,
                                             THEME_FONT_BODY);
        wm_font_draw(&state->font, canvas,
                     avatar.x + (int32_t)(avatar.w - initial_w) / 2,
                     avatar.y + (int32_t)(avatar.h - (uint32_t)THEME_FONT_BODY) / 2,
                     initial, th->text, THEME_FONT_BODY, 24u);
        wm_font_draw(&state->font, canvas,
                     avatar.x + (int32_t)avatar.w + 12,
                     avatar.y + (int32_t)(avatar.h - (uint32_t)THEME_FONT_BODY) / 2,
                     sm_session_user(), th->text, THEME_FONT_BODY,
                     menu.w > 200u ? menu.w - 200u : 0u);

        /* Restarting is an ordinary action; powering off is the one that
         * ends the session, so it is the only control here wearing M3's
         * error container. */
        wm_rect_t rb = sm_session_button_rect(menu, false);
        wm_rect_t sd = sm_session_button_rect(menu, true);
        uint32_t btn_radius = wm_pill_radius(sd.w, sd.h);
        /* The restart button sits on the footer's own surface colour. */
        wm_canvas_fill_rounded(canvas, sd, btn_radius, th->danger);
        wm_icon_draw(canvas, (wm_rect_t){rb.x + 10, rb.y + 10, 20u, 20u},
                     WM_ICON_RESTART, th->text_dim);
        wm_icon_draw(canvas, (wm_rect_t){sd.x + 10, sd.y + 10, 20u, 20u},
                     WM_ICON_POWER, th->on_danger);
    }
}

wm_launcher_action_t wm_start_menu_hit_test(wm_state_t *state, int32_t x, int32_t y)
{
    wm_launcher_action_t result = {WM_LAUNCHER_ACTION_NONE, 0u};
    if (!state || !state->launcher_open) return result;
    wm_rect_t menu = wm_start_menu_rect(state);
    if (!wm_rect_intersects((wm_rect_t){x, y, 1u, 1u}, menu)) return result;

    wm_rect_t point = {x, y, 1u, 1u};
    if (wm_rect_intersects(point, sm_session_button_rect(menu, false))) {
        result.kind = WM_LAUNCHER_ACTION_REBOOT;
        return result;
    }
    if (wm_rect_intersects(point, sm_session_button_rect(menu, true))) {
        result.kind = WM_LAUNCHER_ACTION_SHUTDOWN;
        return result;
    }

    wm_rect_t vp = app_viewport_rect(state);
    if (wm_rect_intersects((wm_rect_t){x, y, 1u, 1u}, vp)) {
        ensure_filter_cache(state);
        uint32_t first_row, last_row;
        visible_rows(state, &first_row, &last_row);
        for (uint32_t row = first_row; row < last_row; ++row) {
            if (wm_rect_intersects((wm_rect_t){x, y, 1u, 1u}, card_rect(state, row))) {
                result.kind = WM_LAUNCHER_ACTION_APP;
                result.app_index = state->menu_rows[row];
                return result;
            }
        }
    }
    return result;
}

wm_rect_t wm_start_menu_list_rect(const wm_state_t *state)
{
    if (!state) return (wm_rect_t){0, 0, 0u, 0u};
    return app_viewport_rect(state);
}

static int32_t row_of_app(const wm_state_t *state, int32_t app_index);

bool wm_start_menu_app_rect(wm_state_t *state, int32_t app_index, wm_rect_t *out)
{
    if (!state || !out || app_index < 0) return false;
    ensure_filter_cache(state);
    int32_t row = row_of_app(state, app_index);
    if (row < 0) return false;
    wm_rect_t r = wm_rect_intersection(card_rect(state, (uint32_t)row),
                                       app_viewport_rect(state));
    if (r.w == 0u || r.h == 0u) return false;
    *out = r;
    return true;
}

/* ------------------------------------------------- keyboard navigation ---*/

/* Row holding `app_index`, or -1. */
static int32_t row_of_app(const wm_state_t *state, int32_t app_index)
{
    if (app_index < 0) return -1;
    for (uint32_t row = 0; row < state->filter_count; ++row) {
        if ((int32_t)state->menu_rows[row] == app_index) return (int32_t)row;
    }
    return -1;
}

/* Glide the least distance that brings `row` fully into the viewport,
 * measured from where the list is heading rather than where it is now, so
 * holding an arrow key keeps a steady pace. */
static void scroll_row_into_view(wm_state_t *state, uint32_t row)
{
    wm_rect_t vp = app_viewport_rect(state);
    float top = (float)(row * row_pitch());
    float bottom = top + (float)sm_card_height();
    float target = state->launcher_scroll.target;
    if (top < target) target = top;
    else if (bottom > target + (float)vp.h) target = bottom - (float)vp.h;
    (void)wm_scroll_to(&state->launcher_scroll, target, max_scroll(state));
}

bool wm_start_menu_move_selection(wm_state_t *state, int32_t delta)
{
    if (!state || !state->launcher_open || delta == 0) return false;
    ensure_filter_cache(state);
    if (state->filter_count == 0u) return false;

    int32_t row = row_of_app(state, state->launcher_selected_index);
    /* Nothing selected yet: Down takes the first app, Up the last. */
    if (row < 0) row = delta > 0 ? -1 : (int32_t)state->filter_count;
    row += delta;
    if (row < 0) row = 0;
    if (row >= (int32_t)state->filter_count) row = (int32_t)state->filter_count - 1;

    scroll_row_into_view(state, (uint32_t)row);
    state->launcher_selected_index = (int32_t)state->menu_rows[row];
    /* Take the highlight off the pointer, which may be resting over some
     * other card and would otherwise keep it. */
    state->launcher_hover_index = -1;
    return true;   /* the selection or the scroll offset moved, or both */
}

/* Enter runs the selected entry, or the first result when the user has
 * typed a query and not moved off it. Both read the same row cache the
 * viewport draws from, so what runs is the row on screen -- the open-coded
 * scan this replaced matched on the name alone and could start a different
 * program from the one highlighted. */
int32_t wm_start_menu_launch_selected(wm_state_t *state)
{
    if (!state || !state->launcher_open) return -1;
    ensure_filter_cache(state);

    int32_t chosen = state->launcher_hover_index >= 0
        ? state->launcher_hover_index : state->launcher_selected_index;
    if (chosen >= 0 && (uint32_t)chosen < state->assets.app_count) {
        return wm_assets_launch(&state->assets.apps[chosen]);
    }
    if (state->filter_count > 0u && state->menu_rows[0] < state->assets.app_count)
        return wm_assets_launch(&state->assets.apps[state->menu_rows[0]]);
    return -1;
}
