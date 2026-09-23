#include "WM_Desktop.h"
#include "WM_Icons.h"
#include "WM_Taskbar.h"

#include "../Compositor/WM_Raster.h"
#include "../Core/WM_Assets.h"
#include "../Font/WM_Font.h"
#include "../../../../Userland/API/Source/File.h"

#include <stdio.h>
#include <string.h>

/* Where the user's arrangement lives, and the arrangement shipped with the
 * image when they have not made one. Both are the same format. */
#define ICON_CFG          "/var/System/desktop.icons"
#define ICON_CFG_DEFAULT  "/Userland/com.ImplusOS.windowmanager/Resource/Apps/desktop.icons"

#define CELL_W        88
#define CELL_H        90
#define MARGIN_X      20
#define MARGIN_Y      20
#define ICON_BOX      36
#define LABEL_H       16

typedef struct {
    char label[48];
    char path[192];
    /* Launch argument. A Linux program sits on the desktop the same way it
     * sits in the Start menu: `path` is the host launcher and this is the
     * "Title|command" line it parses. */
    char argument[WM_APP_ARG_MAX];
} desktop_icon_t;

static desktop_icon_t g_icons[WM_DESKTOP_MAX_ICONS];
static uint32_t        g_icon_count;
static int             g_loaded;

static wm_icon_kind_t kind_for(const char *label, const char *path,
                               const char *argument)
{
    (void)label;
    if (strstr(path, "terminal") || strstr(argument, "xterm"))
        return WM_ICON_TERMINAL;
    if (strstr(path, "editor"))      return WM_ICON_EDIT;
    if (strstr(path, "filemanager")) return WM_ICON_FOLDER;
    if (strstr(path, "settings"))    return WM_ICON_SETTINGS;
    if (strstr(path, "processmanager")) return WM_ICON_MEMORY;
    if (strstr(path, "linuxapp") || strstr(path, "gtk3demo"))
        return WM_ICON_APPS;
    return WM_ICON_DOC;
}

/* One "Label|Path|Argument" line; the argument field is optional and the
 * label may be omitted entirely (a bare path). */
static bool parse_line(char *line, desktop_icon_t *out)
{
    memset(out, 0, sizeof(*out));
    char *fields[3] = {line, NULL, NULL};
    uint32_t count = 1u;
    char *cursor = line;
    while (count < 3u) {
        char *separator = strchr(cursor, '|');
        if (!separator) break;
        *separator = '\0';
        cursor = separator + 1;
        fields[count++] = cursor;
    }
    const char *label = count >= 2u ? fields[0] : "";
    const char *path  = count >= 2u ? fields[1] : fields[0];
    const char *argument = count >= 3u ? fields[2] : "";
    strncpy(out->label, label, sizeof(out->label) - 1u);
    strncpy(out->path, path, sizeof(out->path) - 1u);
    strncpy(out->argument, argument, sizeof(out->argument) - 1u);
    for (char *p = out->path; *p; ++p) if (*p == '\r') { *p = '\0'; break; }
    for (char *p = out->argument; *p; ++p) if (*p == '\r') { *p = '\0'; break; }
    return out->path[0] != '\0';
}

static bool load_from(const char *config_path)
{
    file_stat_t st;
    if (file_stat(config_path, &st) < 0 || !st.exists || st.is_dir ||
        st.size == 0 || st.size > 8192u)
        return false;
    int32_t fd = file_open(config_path, 0);
    if (fd < 0) return false;
    char buf[8193];
    int64_t n = file_read(fd, buf, sizeof(buf) - 1);
    file_close(fd);
    if (n <= 0) return false;
    buf[n] = '\0';

    char *line = buf;
    while (*line && g_icon_count < WM_DESKTOP_MAX_ICONS) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        while (*line == ' ' || *line == '\t') ++line;
        if (line[0] && line[0] != '#') {
            desktop_icon_t icon;
            if (parse_line(line, &icon)) g_icons[g_icon_count++] = icon;
        }
        if (!nl) break;
        line = nl + 1;
    }
    return g_icon_count != 0u;
}

void wm_desktop_reload(void)
{
    g_loaded = 1;
    g_icon_count = 0;
    if (load_from(ICON_CFG)) return;
    (void)load_from(ICON_CFG_DEFAULT);
}

/* Columns fill downwards and wrap, the way a desktop has always laid icons
 * out -- the previous single column ran off the bottom of the screen after
 * seven entries and simply stopped drawing the rest. The taskbar's own rect
 * is what bounds the usable area, so this follows it if it ever moves. */
static uint32_t rows_per_column(const wm_state_t *state)
{
    wm_rect_t dock = wm_taskbar_rect(state);
    int32_t top = MARGIN_Y;
    int32_t bottom = (int32_t)state->compositor.framebuffer_height - MARGIN_Y;
#if WM_TASKBAR_AT_TOP
    top = dock.y + (int32_t)dock.h + MARGIN_Y;
#else
    if (dock.h != 0u && dock.y < bottom) bottom = dock.y - 8;
#endif
    int32_t usable = bottom - top;
    if (usable < CELL_H) return 1u;
    return (uint32_t)(usable / CELL_H);
}

static int32_t grid_top(const wm_state_t *state)
{
#if WM_TASKBAR_AT_TOP
    wm_rect_t dock = wm_taskbar_rect(state);
    return dock.y + (int32_t)dock.h + MARGIN_Y;
#else
    (void)state;
    return MARGIN_Y;
#endif
}

static void icon_cell(const wm_state_t *state, uint32_t index,
                      wm_rect_t *box, wm_rect_t *label)
{
    uint32_t rows = rows_per_column(state);
    uint32_t column = index / rows;
    uint32_t row = index % rows;
    int32_t cell_x = MARGIN_X + (int32_t)(column * CELL_W);
    int32_t cell_y = grid_top(state) + (int32_t)(row * CELL_H);
    box->x = cell_x + (CELL_W - ICON_BOX) / 2;
    box->y = cell_y;
    box->w = ICON_BOX;
    box->h = ICON_BOX;
    label->x = cell_x;
    label->y = cell_y + ICON_BOX + 4;
    label->w = CELL_W;
    label->h = LABEL_H;
}

void wm_desktop_draw(wm_state_t *state, wm_canvas_t *canvas)
{
    if (!state || !canvas) return;
    if (!g_loaded) wm_desktop_reload();

    for (uint32_t i = 0; i < g_icon_count; ++i) {
        wm_rect_t box, label;
        icon_cell(state, i, &box, &label);
        if (box.x + (int32_t)box.w > (int32_t)state->compositor.framebuffer_width)
            break;   /* ran out of columns */

        /* Reuse the launcher's artwork when this icon points at something
         * the Start menu also lists, so a desktop icon gets the program's
         * real icon -- including the one its Linux distribution ships --
         * instead of the generic glyph that used to be the only option. */
        int32_t app_index = wm_assets_find_app(&state->assets, g_icons[i].path,
                                               g_icons[i].argument);
        const wm_launcher_app_t *app = app_index >= 0
            ? &state->assets.apps[(uint32_t)app_index] : NULL;

        /* Desktop icons sit straight on the wallpaper: no plate behind
         * them and no shadow under the label, so the picture shows through
         * and the grid stays quiet. */
        const m3_scheme_t *m3 = &state->theme.color;
        uint32_t on_wallpaper = m3_scheme_is_dark(m3) ? m3->on_surface
                                                      : m3->inverse_on_surface;
        if (app && app->icon_pixels) {
            wm_canvas_blit_scaled(canvas, box, app->icon_pixels,
                                  app->icon_width, app->icon_height, 255u,
                                  M3_SHAPE_SMALL);
        } else if (app && app->vector_icon >= 0) {
            wm_icon_draw(canvas, box, (wm_icon_kind_t)app->vector_icon,
                         on_wallpaper);
        } else {
            wm_icon_draw(canvas, box,
                         kind_for(g_icons[i].label, g_icons[i].path,
                                  g_icons[i].argument),
                         on_wallpaper);
        }

        const char *txt = g_icons[i].label[0] ? g_icons[i].label
                        : app                 ? app->name
                                              : g_icons[i].path;
        float size = state->theme.type.label_small;
        uint32_t tw = wm_font_measure(&state->font, txt, size);
        int32_t tx = label.x + ((int32_t)label.w - (int32_t)tw) / 2;
        if (tx < label.x) tx = label.x;
        wm_font_draw(&state->font, canvas, tx, label.y, txt,
                     on_wallpaper, size, label.w);
    }
}

const char *wm_desktop_hit_test(const wm_state_t *state, int32_t x, int32_t y,
                                const char **argument_out)
{
    if (argument_out) *argument_out = NULL;
    if (!state) return NULL;
    if (!g_loaded) wm_desktop_reload();
    for (uint32_t i = 0; i < g_icon_count; ++i) {
        wm_rect_t box, label;
        icon_cell(state, i, &box, &label);
        if (x >= box.x - 8 && x < box.x + (int32_t)box.w + 8 &&
            y >= box.y - 4 && y < label.y + (int32_t)label.h) {
            if (!g_icons[i].path[0]) return NULL;
            if (argument_out && g_icons[i].argument[0])
                *argument_out = g_icons[i].argument;
            return g_icons[i].path;
        }
    }
    return NULL;
}
