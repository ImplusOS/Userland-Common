#include "WM_StartMenu.h"

#include "../Compositor/WM_Raster.h"
#include "../Font/WM_Font.h"
#include "WM_Taskbar.h"
#include "WM_Icons.h"
#include "../Core/WM_Assets.h"
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

/* The line under an app's name: what it does, or failing that what kind of
 * thing it is. */
static const char *app_subtitle(const wm_launcher_app_t *app)
{
    if (app->comment[0]) return app->comment;
    if (app->category != WM_APP_CATEGORY_OTHER)
        return wm_app_category_label(app->category);
    return app->linux_backed ? "Linux アプリケーション" : "アプリケーション";
}

bool wm_start_menu_input_char(wm_state_t *state, char ch)
{
    if (!state || !state->launcher_open) return false;
    if (ch < 0x20 || state->search_len >= WM_SEARCH_MAX - 1u) return false;
    state->search_text[state->search_len++] = ch;
    state->search_text[state->search_len] = '\0';
    state->launcher_scroll = 0u;
    /* A new query means new results: whatever was selected is about to be a
     * different program, or gone. */
    state->launcher_selected_index = -1;
    return true;
}

bool wm_start_menu_input_backspace(wm_state_t *state)
{
    if (!state || !state->launcher_open || state->search_len == 0u) return false;
    state->search_text[--state->search_len] = '\0';
    state->launcher_scroll = 0u;
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
#endif
    return (wm_rect_t){4, y, width, height};
}

#define SM_HEADER_PAD_TOP   12u
#define SM_ICON_SIZE        32u
#define SM_HEADER_PAD_LEFT  16u
#define SM_HEADER_GAP       10u
#define SM_SEARCH_H         44u
#define SM_LABEL_PAD        14u
#define SM_CARD_PAD_X       8u
#define SM_CARD_PAD_Y       9u
#define SM_CARD_GAP         4u
#define SM_FOOTER_H         56u
#define SM_FOOTER_PAD       12u
#define SM_SESSION_BTN      40u

/* The avatar sits to the LEFT of the name and the "ImplusOS" line, so the
 * block is as tall as whichever of the two columns is taller. Adding them
 * up, which is what this did, reserved some sixty pixels of empty space
 * between the header and the search box -- and took the room out of the
 * application list below it. */
static uint32_t sm_header_height(const wm_state_t *state)
{
    uint32_t title_line = (uint32_t)(state->theme.type.title_medium + 0.5f) + 4u;
    uint32_t sub_line = (uint32_t)(state->theme.type.label_small + 0.5f) + 2u;
    uint32_t text_column = title_line + sub_line;
    uint32_t tallest = text_column > SM_ICON_SIZE ? text_column : SM_ICON_SIZE;
    return SM_HEADER_PAD_TOP + tallest + SM_HEADER_PAD_TOP;
}

static uint32_t sm_label_height(const wm_state_t *state)
{
    return (uint32_t)(state->theme.type.label_medium + 0.5f) + 10u;
}

static uint32_t sm_card_height(const wm_state_t *state)
{
    uint32_t name_h = (uint32_t)(state->theme.type.body_medium + 0.5f) + 4u;
    uint32_t sub_h = (uint32_t)(state->theme.type.label_small + 0.5f) + 2u;
    uint32_t content = SM_CARD_PAD_Y + name_h + 2u + sub_h + SM_CARD_PAD_Y;
    uint32_t icon_area = SM_CARD_PAD_X + 36u + SM_CARD_PAD_X;
    return content > icon_area ? content : icon_area;
}

/* Vertical offset of the application list inside the menu. The 5px is the
 * gap under the header divider that the search bar is drawn in; keeping it
 * here means the list, the list's overline and the search bar all measure
 * from the same place. */
#define SM_SEARCH_GAP 5u

static uint32_t sm_content_top(const wm_state_t *state)
{
    return sm_header_height(state) + SM_SEARCH_GAP + SM_SEARCH_H +
           sm_label_height(state);
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

/* The viewport holds two kinds of row -- a category heading and an app card
 * -- of different heights, so neither the number that fits nor the scroll
 * limit can be a division any more. Both are found by walking the rows and
 * accumulating height, forwards from the first visible row and backwards
 * from the last. */
static uint32_t sm_section_height(const wm_state_t *state)
{
    return (uint32_t)(state->theme.type.label_medium + 0.5f) + 16u;
}

static uint32_t row_height(const wm_state_t *state, uint32_t row)
{
    if (row >= state->filter_count) return 0u;
    return state->menu_rows[row].kind == WM_MENU_ROW_HEADER
        ? sm_section_height(state) : sm_card_height(state);
}

static uint32_t rows_from(const wm_state_t *state, uint32_t first)
{
    wm_rect_t vp = app_viewport_rect(state);
    uint32_t used = 0u;
    uint32_t count = 0u;
    for (uint32_t row = first; row < state->filter_count; ++row) {
        uint32_t height = row_height(state, row);
        if (used + height > vp.h) break;
        used += height + SM_CARD_GAP;
        ++count;
    }
    /* Always offer one row: a viewport too short for even a single card
     * would otherwise render nothing and refuse to scroll. */
    if (count == 0u && first < state->filter_count) return 1u;
    return count;
}

static uint32_t visible_app_rows(const wm_state_t *state)
{
    return rows_from(state, state->launcher_scroll);
}

/* Rows for the current search: every matching app, and -- when nothing is
 * being searched for -- a heading before each run of one category. Results
 * are left flat while searching, where a heading would only push matches out
 * of view. wm_assets_init_metadata() sorted the apps by category, so one
 * pass emits every section in order. */
static void rebuild_filter_cache(wm_state_t *state)
{
    const uint32_t capacity =
        (uint32_t)(sizeof(state->menu_rows) / sizeof(state->menu_rows[0]));
    const bool searching = state->search_len > 0u;
    uint32_t current_category = (uint32_t)WM_APP_CATEGORY_COUNT;

    state->filter_count = 0u;
    for (uint32_t i = 0; i < state->assets.app_count; i++) {
        const wm_launcher_app_t *app = &state->assets.apps[i];
        if (!app_matches(app, state->search_text)) continue;
        bool needs_header = !searching && (uint32_t)app->category != current_category;
        /* menu_rows is sized for every app plus one header per category, so
         * this only ever stops early if both are at their maximum at once. */
        if (state->filter_count + (needs_header ? 2u : 1u) > capacity) break;
        if (needs_header) {
            current_category = (uint32_t)app->category;
            state->menu_rows[state->filter_count].kind = WM_MENU_ROW_HEADER;
            state->menu_rows[state->filter_count].value = (uint16_t)current_category;
            ++state->filter_count;
        }
        state->menu_rows[state->filter_count].kind = WM_MENU_ROW_APP;
        state->menu_rows[state->filter_count].value = (uint16_t)i;
        ++state->filter_count;
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

static uint32_t max_scroll_row(const wm_state_t *state)
{
    wm_rect_t vp = app_viewport_rect(state);
    uint32_t used = 0u;
    uint32_t row = state->filter_count;
    while (row > 0u) {
        uint32_t height = row_height(state, row - 1u);
        if (used + height > vp.h) break;
        used += height + SM_CARD_GAP;
        --row;
    }
    return row;
}

/* Top of `row` relative to the viewport, given the first visible row. */
static int32_t row_offset(const wm_state_t *state, uint32_t first, uint32_t row)
{
    uint32_t used = 0u;
    for (uint32_t i = first; i < row; ++i) used += row_height(state, i) + SM_CARD_GAP;
    return (int32_t)used;
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

static wm_rect_t filtered_card_rect(const wm_state_t *state, uint32_t first,
                                    uint32_t row)
{
    wm_rect_t vp = app_viewport_rect(state);
    return (wm_rect_t){
        vp.x,
        vp.y + row_offset(state, first, row),
        vp.w,
        row_height(state, row)
    };
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
void wm_start_menu_draw(wm_state_t *state, wm_canvas_t *canvas)
{
    if (!state || !canvas || !state->launcher_open) return;
    wm_rect_t menu = wm_start_menu_rect(state);
    if (menu.w == 0u || menu.h == 0u) return;
    if (!wm_rect_intersects(menu, canvas->clip)) return;
    ensure_filter_cache(state);
    wm_start_menu_clamp_scroll(state);

    const m3_scheme_t *m3 = &state->theme.color;
    const m3_typescale_t *type = &state->theme.type;
    uint32_t radius = m3_shape_radius(M3_SHAPE_EXTRA_LARGE, menu.w, menu.h);
    uint32_t container = m3_elevated(m3, m3->surface_container,
                                        M3_ELEVATION_3);

    wm_canvas_elevation_shadow(canvas, menu, radius, M3_ELEVATION_3,
                               m3_alpha(m3->shadow, 22u), 255u);
    wm_canvas_fill_rounded(canvas, menu, radius, m3->outline_variant);
    wm_canvas_fill_rounded(canvas,
        (wm_rect_t){menu.x + 1, menu.y + 1, menu.w - 2u, menu.h - 2u},
        radius > 0u ? radius - 1u : 0u, container);

    /* --- header: the signed-in user --- */
    {
        wm_rect_t avatar = {menu.x + (int32_t)SM_HEADER_PAD_LEFT,
                            menu.y + (int32_t)SM_HEADER_PAD_TOP,
                            SM_ICON_SIZE, SM_ICON_SIZE};
        wm_canvas_fill_rounded(canvas, avatar,
            m3_shape_radius(M3_SHAPE_FULL, avatar.w, avatar.h),
            m3->primary_container);
        wm_icon_draw(canvas,
            (wm_rect_t){avatar.x + 6, avatar.y + 6, avatar.w - 12u, avatar.h - 12u},
            WM_ICON_PERSON, m3->on_primary_container);
        int32_t text_x = menu.x + (int32_t)SM_HEADER_PAD_LEFT +
                         (int32_t)SM_ICON_SIZE + (int32_t)SM_HEADER_GAP;
        uint32_t text_max = menu.w > (SM_HEADER_PAD_LEFT + SM_ICON_SIZE +
                                      SM_HEADER_GAP + 16u) ?
            menu.w - SM_HEADER_PAD_LEFT - SM_ICON_SIZE - SM_HEADER_GAP - 16u : 0u;
        wm_font_draw(&state->font, canvas, text_x,
                     menu.y + (int32_t)SM_HEADER_PAD_TOP,
                     sm_session_user(), m3->on_surface, type->title_medium,
                     text_max);
        wm_font_draw(&state->font, canvas, text_x,
                     menu.y + (int32_t)SM_HEADER_PAD_TOP +
                         (int32_t)(type->title_medium + 0.5f) + 2,
                     "ImplusOS", m3->on_surface_variant, type->label_small,
                     text_max);
    }

    uint32_t sep_y = sm_header_height(state);
    wm_canvas_fill(canvas,
        (wm_rect_t){menu.x + 12, menu.y + (int32_t)sep_y, menu.w - 24u, 1u},
        m3->outline_variant);

    /* --- M3 search bar: a full-shape container on surface-container-high,
     * outlined in primary while it has the keyboard --- */
    {
        int32_t sb_y = menu.y + (int32_t)sep_y + (int32_t)SM_SEARCH_GAP;
        wm_rect_t sb = {menu.x + 12, sb_y, menu.w - 24u, SM_SEARCH_H};
        uint32_t sb_radius = m3_shape_radius(M3_SHAPE_FULL, sb.w, sb.h);
        wm_canvas_fill_rounded(canvas, sb, sb_radius, m3->surface_container_high);
        if (state->search_active)
            wm_canvas_stroke_rounded(canvas, sb, sb_radius, 2u, m3->primary);
        uint32_t isz = 20u;
        wm_icon_draw(canvas,
            (wm_rect_t){sb.x + 14, sb.y + (int32_t)(sb.h - isz)/2, isz, isz},
            WM_ICON_SEARCH, m3->on_surface_variant);
        const char *display = state->search_len > 0u ?
            state->search_text : "アプリを検索";
        uint32_t tc = state->search_len > 0u ?
            m3->on_surface : m3->on_surface_variant;
        int32_t text_x = sb.x + 46;
        int32_t text_y = sb.y + ((int32_t)sb.h - (int32_t)type->body_large) / 2 - 1;
        wm_font_draw(&state->font, canvas, text_x, text_y,
                     display, tc, type->body_large,
                     sb.w > 60u ? sb.w - 60u : 0u);
        if (state->search_active && state->search_len > 0u) {
            uint32_t cursor_x = (uint32_t)text_x +
                wm_font_measure(&state->font, state->search_text, type->body_large);
            wm_canvas_fill(canvas,
                (wm_rect_t){(int32_t)cursor_x, text_y - 2,
                            2u, (uint32_t)type->body_large + 4u},
                m3->primary);
        }
    }

    /* --- list overline --- */
    {
        int32_t label_y = menu.y + (int32_t)sep_y + (int32_t)SM_SEARCH_GAP +
                          (int32_t)SM_SEARCH_H + 4;
        char label[64];
        (void)snprintf(label, sizeof(label), "%s (%u)",
                       state->search_len > 0u ? "検索結果" : "すべてのアプリ",
                       (unsigned)state->assets.app_count);
        if (state->search_len > 0u) {
            uint32_t matches = 0u;
            for (uint32_t i = 0; i < state->filter_count; ++i)
                if (state->menu_rows[i].kind == WM_MENU_ROW_APP) ++matches;
            (void)snprintf(label, sizeof(label), "検索結果 (%u)", (unsigned)matches);
        }
        wm_font_draw(&state->font, canvas, menu.x + (int32_t)SM_LABEL_PAD, label_y,
                     label, m3->on_surface_variant, type->label_medium,
                     menu.w > 28u ? menu.w - 28u : 0u);
    }

    /* --- the application list --- */
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
            wm_rect_t card = filtered_card_rect(state, first_row, row);

            if (state->menu_rows[row].kind == WM_MENU_ROW_HEADER) {
                wm_app_category_t category =
                    (wm_app_category_t)state->menu_rows[row].value;
                int32_t text_y = card.y +
                    (int32_t)(card.h - (uint32_t)(type->label_medium + 0.5f)) / 2;
                const char *heading = wm_app_category_label(category);
                wm_font_draw(&state->font, canvas, card.x + 12, text_y,
                             heading, m3->on_surface_variant, type->label_medium,
                             card.w > 24u ? card.w - 24u : 0u);
                uint32_t heading_w = wm_font_measure(&state->font, heading,
                                                     type->label_medium);
                int32_t rule_x = card.x + 12 + (int32_t)heading_w + 10;
                int32_t rule_end = card.x + (int32_t)card.w - 12;
                if (rule_end > rule_x) {
                    wm_canvas_fill(canvas,
                        (wm_rect_t){rule_x, text_y + (int32_t)(type->label_medium * 0.5f),
                                    (uint32_t)(rule_end - rule_x), 1u},
                        m3->outline_variant);
                }
                continue;
            }

            uint32_t app_index = state->menu_rows[row].value;
            wm_launcher_app_t *app = &state->assets.apps[app_index];

            /* The pointer wins while it is over a card; otherwise the
             * keyboard selection is what is highlighted. M3 draws the two
             * differently on purpose: a hover is a state layer that fades
             * with the cursor, a selection is a filled container that
             * stays put. */
            bool pointed = state->launcher_hover_index == (int32_t)app_index;
            bool selected = state->launcher_hover_index < 0 &&
                state->launcher_selected_index == (int32_t)app_index;
            uint32_t row_radius = m3_shape_radius(M3_SHAPE_FULL,
                                                     card.w, card.h);
            uint32_t row_container = container;
            uint32_t on_row = m3->on_surface;
            uint32_t on_row_dim = m3->on_surface_variant;
            if (selected) {
                row_container = m3->secondary_container;
                on_row = m3->on_secondary_container;
                on_row_dim = m3->on_secondary_container;
                wm_canvas_fill_rounded(canvas, card, row_radius, row_container);
            } else if (pointed) {
                row_container = m3_state_layer(container, m3->on_surface,
                                                  M3_STATE_HOVER);
                wm_canvas_fill_rounded(canvas, card, row_radius, row_container);
            }

            wm_rect_t icon_rect = {card.x + 10, card.y + (int32_t)(card.h - 36u) / 2,
                                   36u, 36u};
            if (app->icon_pixels) {
                wm_canvas_blit_scaled(canvas, icon_rect, app->icon_pixels,
                                      app->icon_width, app->icon_height, 255u,
                                      M3_SHAPE_SMALL);
            } else if (app->vector_icon >= 0) {
                wm_canvas_fill_rounded(canvas, icon_rect,
                    m3_shape_radius(M3_SHAPE_FULL, icon_rect.w, icon_rect.h),
                    m3->primary_container);
                wm_icon_draw(canvas,
                    (wm_rect_t){icon_rect.x + 8, icon_rect.y + 8,
                                icon_rect.w - 16u, icon_rect.h - 16u},
                    (wm_icon_kind_t)app->vector_icon, m3->on_primary_container);
            } else {
                wm_canvas_fill_rounded(canvas, icon_rect,
                    m3_shape_radius(M3_SHAPE_FULL, icon_rect.w, icon_rect.h),
                    m3->tertiary_container);
                const char *badge = app->badge[0] ? app->badge : "?";
                uint32_t badge_w = wm_font_measure(&state->font, badge,
                                                   type->label_large);
                wm_font_draw(&state->font, canvas,
                             icon_rect.x + (int32_t)(icon_rect.w - badge_w)/2,
                             icon_rect.y +
                                 (int32_t)(icon_rect.h - (uint32_t)type->label_large)/2,
                             badge, m3->on_tertiary_container,
                             type->label_large, 32u);
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
                                                  type->label_small) + 18u;
                uint32_t chip_h = (uint32_t)(type->label_small + 0.5f) + 10u;
                wm_rect_t chip_rect = {card.x + (int32_t)card.w - (int32_t)chip_w - 14,
                                       card.y + (int32_t)(card.h - chip_h) / 2,
                                       chip_w, chip_h};
                wm_canvas_stroke_rounded(canvas, chip_rect,
                    m3_shape_radius(M3_SHAPE_SMALL, chip_rect.w, chip_rect.h),
                    1u, m3->outline);
                wm_font_draw(&state->font, canvas, chip_rect.x + 9,
                             chip_rect.y + 5, chip, on_row_dim,
                             type->label_small, chip_w);
                uint32_t consumed = chip_w + 20u;
                text_limit = text_limit > consumed ? text_limit - consumed : 0u;
            }

            int32_t name_y = card.y + (int32_t)SM_CARD_PAD_Y;
            int32_t sub_y = name_y + (int32_t)(type->body_medium + 0.5f) + 2;
            wm_font_draw(&state->font, canvas,
                         card.x + 58, name_y,
                         app->name, on_row, type->body_medium, text_limit);
            wm_font_draw(&state->font, canvas,
                         card.x + 58, sub_y,
                         app_subtitle(app), on_row_dim, type->label_small,
                         card.w > 72u ? card.w - 72u : 0u);
        }

        if (total == 0u) {
            wm_font_draw(&state->font, canvas,
                         vp.x + 16, vp.y + 20,
                         "一致するアプリはありません", m3->on_surface_variant,
                         type->body_medium, vp.w > 32u ? vp.w - 32u : 0u);
        }

        uint32_t max_scroll = max_scroll_row(state);
        if (max_scroll > 0u && vp.h >= 24u) {
            wm_rect_t track = {vp.x + (int32_t)vp.w - 5, vp.y, 4u, vp.h};
            wm_canvas_fill_rounded(canvas, track, 2u, m3->outline_variant);
            uint32_t total_rows = total;
            uint32_t thumb_h = total_rows ?
                (uint32_t)(((uint64_t)visible_app_rows(state) * vp.h) / total_rows) : vp.h;
            if (thumb_h < 24u) thumb_h = 24u;
            if (thumb_h > vp.h) thumb_h = vp.h;
            uint32_t travel = vp.h - thumb_h;
            uint32_t thumb_y = max_scroll ?
                (uint32_t)(((uint64_t)state->launcher_scroll * travel) / max_scroll) : 0u;
            wm_canvas_fill_rounded(canvas,
                (wm_rect_t){track.x, track.y + (int32_t)thumb_y, 4u, thumb_h},
                2u, m3->outline);
        }
        canvas->clip = old_clip;
    }

    /* --- footer: the session, and the two ways out of it --- */
    {
        wm_rect_t footer = sm_footer_rect(menu);
        wm_canvas_fill(canvas,
            (wm_rect_t){menu.x + 12, footer.y, menu.w - 24u, 1u},
            m3->outline_variant);

        wm_rect_t avatar = {footer.x + (int32_t)SM_FOOTER_PAD, footer.y + 12, 32u, 32u};
        wm_canvas_fill_rounded(canvas, avatar,
            m3_shape_radius(M3_SHAPE_FULL, avatar.w, avatar.h),
            m3->primary_container);
        char initial[4] = {sm_session_user()[0], '\0', '\0', '\0'};
        uint32_t initial_w = wm_font_measure(&state->font, initial,
                                             type->label_large);
        wm_font_draw(&state->font, canvas,
                     avatar.x + (int32_t)(avatar.w - initial_w) / 2,
                     avatar.y + (int32_t)(avatar.h - (uint32_t)type->label_large) / 2,
                     initial, m3->on_primary_container, type->label_large, 24u);
        wm_font_draw(&state->font, canvas,
                     avatar.x + (int32_t)avatar.w + 12,
                     avatar.y + (int32_t)(avatar.h - (uint32_t)type->body_medium) / 2,
                     sm_session_user(), m3->on_surface, type->body_medium,
                     menu.w > 200u ? menu.w - 200u : 0u);

        /* Restarting is an ordinary action; powering off is the one that
         * ends the session, so it is the only control here wearing M3's
         * error container. */
        wm_rect_t rb = sm_session_button_rect(menu, false);
        wm_rect_t sd = sm_session_button_rect(menu, true);
        uint32_t btn_radius = m3_shape_radius(M3_SHAPE_FULL, sd.w, sd.h);
        wm_canvas_fill_rounded(canvas, rb, btn_radius, m3->surface_container_high);
        wm_canvas_fill_rounded(canvas, sd, btn_radius, m3->error_container);
        wm_icon_draw(canvas, (wm_rect_t){rb.x + 10, rb.y + 10, 20u, 20u},
                     WM_ICON_RESTART, m3->on_surface_variant);
        wm_icon_draw(canvas, (wm_rect_t){sd.x + 10, sd.y + 10, 20u, 20u},
                     WM_ICON_POWER, m3->on_error_container);
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
        uint32_t total = state->filter_count;
        uint32_t first_row = state->launcher_scroll;
        uint32_t vis_rows = visible_app_rows(state);
        uint32_t last_row = first_row + vis_rows;
        if (last_row > total) last_row = total;
        for (uint32_t row = first_row; row < last_row; ++row) {
            if (state->menu_rows[row].kind != WM_MENU_ROW_APP) continue;
            wm_rect_t card = filtered_card_rect(state, first_row, row);
            if (wm_rect_intersects((wm_rect_t){x, y, 1u, 1u}, card)) {
                result.kind = WM_LAUNCHER_ACTION_APP;
                result.app_index = state->menu_rows[row].value;
                return result;
            }
        }
    }
    return result;
}

/* ------------------------------------------------- keyboard navigation ---*/

/* Row holding `app_index`, or -1. */
static int32_t row_of_app(const wm_state_t *state, int32_t app_index)
{
    if (app_index < 0) return -1;
    for (uint32_t row = 0; row < state->filter_count; ++row) {
        if (state->menu_rows[row].kind == WM_MENU_ROW_APP &&
            (int32_t)state->menu_rows[row].value == app_index) {
            return (int32_t)row;
        }
    }
    return -1;
}

/* Scroll the least amount that brings `row` fully into the viewport. */
static void scroll_row_into_view(wm_state_t *state, uint32_t row)
{
    if (row < state->launcher_scroll) {
        state->launcher_scroll = row;
        return;
    }
    while (row >= state->launcher_scroll + rows_from(state, state->launcher_scroll) &&
           state->launcher_scroll < state->filter_count - 1u) {
        ++state->launcher_scroll;
    }
}

bool wm_start_menu_move_selection(wm_state_t *state, int32_t delta)
{
    if (!state || !state->launcher_open || delta == 0) return false;
    ensure_filter_cache(state);
    if (state->filter_count == 0u) return false;

    int32_t row = row_of_app(state, state->launcher_selected_index);
    int32_t step = delta > 0 ? 1 : -1;
    /* Nothing selected yet: Down takes the first app, Up the last. Headers
     * are skipped -- they are labels, not destinations. */
    if (row < 0) {
        row = step > 0 ? -1 : (int32_t)state->filter_count;
    }
    for (int32_t moved = 0; moved < (delta > 0 ? delta : -delta); ++moved) {
        int32_t next = row + step;
        while (next >= 0 && next < (int32_t)state->filter_count &&
               state->menu_rows[next].kind != WM_MENU_ROW_APP) {
            next += step;
        }
        if (next < 0 || next >= (int32_t)state->filter_count) break;
        row = next;
    }
    if (row < 0 || row >= (int32_t)state->filter_count ||
        state->menu_rows[row].kind != WM_MENU_ROW_APP) {
        return false;
    }

    int32_t selected = (int32_t)state->menu_rows[row].value;
    /* Bring the heading above the first app of a section into view with it,
     * so moving into a new section shows which section it is. */
    uint32_t top = (uint32_t)row;
    if (top > 0u && state->menu_rows[top - 1u].kind == WM_MENU_ROW_HEADER) --top;
    scroll_row_into_view(state, (uint32_t)row);
    if (top < state->launcher_scroll) state->launcher_scroll = top;

    state->launcher_selected_index = selected;
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
    for (uint32_t row = 0; row < state->filter_count; ++row) {
        if (state->menu_rows[row].kind != WM_MENU_ROW_APP) continue;
        uint32_t index = state->menu_rows[row].value;
        if (index >= state->assets.app_count) return -1;
        return wm_assets_launch(&state->assets.apps[index]);
    }
    return -1;
}
