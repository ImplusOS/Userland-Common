#include "WM_Desktop.h"
#include "WM_Icons.h"

#include "../Compositor/WM_Raster.h"
#include "../Font/WM_Font.h"
#include "../../../../Userland/API/Source/File.h"

#include <string.h>

#define ICON_CFG      "/var/System/desktop.icons"
#define CELL_W        96
#define CELL_H        100
#define MARGIN_X      24
#define MARGIN_Y      24
#define ICON_BOX      44

typedef struct {
    char label[48];
    char path[192];
} desktop_icon_t;

static desktop_icon_t g_icons[WM_DESKTOP_MAX_ICONS];
static uint32_t        g_icon_count;
static int             g_loaded;

static wm_icon_kind_t kind_for(const char *label, const char *path)
{
    (void)label;
    if (strstr(path, "terminal"))    return WM_ICON_TERMINAL;
    if (strstr(path, "editor"))      return WM_ICON_EDIT;
    if (strstr(path, "filemanager")) return WM_ICON_FOLDER;
    return WM_ICON_DOC;
}

void wm_desktop_reload(void)
{
    g_loaded = 1;
    g_icon_count = 0;

    file_stat_t st;
    if (file_stat(ICON_CFG, &st) < 0 || !st.exists || st.is_dir ||
        st.size == 0 || st.size > 8192u)
        return;
    int32_t fd = file_open(ICON_CFG, 0);
    if (fd < 0) return;
    char buf[8193];
    int64_t n = file_read(fd, buf, sizeof(buf) - 1);
    file_close(fd);
    if (n <= 0) return;
    buf[n] = '\0';

    char *line = buf;
    while (*line && g_icon_count < WM_DESKTOP_MAX_ICONS) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        while (*line == ' ' || *line == '\t') ++line;
        if (line[0] && line[0] != '#') {
            char *bar = strchr(line, '|');
            desktop_icon_t *it = &g_icons[g_icon_count];
            if (bar) {
                size_t ll = (size_t)(bar - line);
                if (ll >= sizeof(it->label)) ll = sizeof(it->label) - 1;
                memcpy(it->label, line, ll); it->label[ll] = '\0';
                strncpy(it->path, bar + 1, sizeof(it->path) - 1);
                it->path[sizeof(it->path) - 1] = '\0';
            } else {
                it->label[0] = '\0';
                strncpy(it->path, line, sizeof(it->path) - 1);
                it->path[sizeof(it->path) - 1] = '\0';
            }
            for (char *p = it->path; *p; ++p) if (*p == '\r') *p = '\0';
            if (it->path[0]) ++g_icon_count;
        }
        if (!nl) break;
        line = nl + 1;
    }
}

static void icon_cell(uint32_t idx, wm_rect_t *box, wm_rect_t *label)
{
    int32_t col_x = MARGIN_X;
    int32_t y = MARGIN_Y + (int32_t)idx * CELL_H;
    box->x = col_x + (CELL_W - ICON_BOX) / 2;
    box->y = y;
    box->w = ICON_BOX;
    box->h = ICON_BOX;
    label->x = col_x;
    label->y = y + ICON_BOX + 4;
    label->w = CELL_W;
    label->h = 16;
}

void wm_desktop_draw(wm_state_t *state, wm_canvas_t *canvas)
{
    if (!state || !canvas) return;
    if (!g_loaded) wm_desktop_reload();

    for (uint32_t i = 0; i < g_icon_count; ++i) {
        wm_rect_t box, label;
        icon_cell(i, &box, &label);
        if (box.y + (int32_t)box.h > (int32_t)state->compositor.framebuffer_height) break;

        wm_canvas_fill_rounded(canvas, (wm_rect_t){box.x - 6, box.y - 6,
                               box.w + 12u, box.h + 12u}, 12u, 0x22FFFFFFu);
        wm_icon_draw(canvas, box, kind_for(g_icons[i].label, g_icons[i].path),
                     0xFFFFFFFFu);
        const char *txt = g_icons[i].label[0] ? g_icons[i].label : g_icons[i].path;
        uint32_t tw = wm_font_measure(&state->font, txt, state->theme.font_small);
        int32_t tx = label.x + ((int32_t)label.w - (int32_t)tw) / 2;
        if (tx < label.x) tx = label.x;
        /* shadow + text for contrast on any wallpaper */
        wm_font_draw(&state->font, canvas, tx + 1, label.y + 1, txt,
                     0xC0000000u, state->theme.font_small, label.w);
        wm_font_draw(&state->font, canvas, tx, label.y, txt,
                     0xFFFFFFFFu, state->theme.font_small, label.w);
    }
}

const char *wm_desktop_hit_test(const wm_state_t *state, int32_t x, int32_t y)
{
    (void)state;
    if (!g_loaded) wm_desktop_reload();
    for (uint32_t i = 0; i < g_icon_count; ++i) {
        wm_rect_t box, label;
        icon_cell(i, &box, &label);
        if (x >= box.x - 6 && x < box.x + (int32_t)box.w + 6 &&
            y >= box.y - 6 && y < label.y + (int32_t)label.h)
            return g_icons[i].path[0] ? g_icons[i].path : NULL;
    }
    return NULL;
}
