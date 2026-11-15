#include "WM_Theme.h"

#include "../../../../Userland/API/Source/File.h"

#include <stdlib.h>
#include <string.h>

void wm_theme_set_accent(wm_theme_t *theme, uint32_t accent, bool dark)
{
    if (theme) theme_init(&theme->color, accent, dark);
}

void wm_theme_set_defaults(wm_theme_t *theme)
{
    if (!theme) return;
    memset(theme, 0, sizeof(*theme));
    wm_theme_set_accent(theme, THEME_ACCENT_DEFAULT, false);
    theme->title_height  = WM_TITLE_HEIGHT;
    theme->corner_radius = WM_CORNER_RADIUS;
    theme->shadow_size   = WM_SHADOW_SIZE;
    theme->dock_height   = WM_DOCK_HEIGHT;
}

static uint32_t clamp_u32(uint32_t v, uint32_t lo, uint32_t hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/* `key = <unsigned>` anywhere in `text`; `fallback` when absent. */
static uint32_t read_metric(const char *text, const char *key, uint32_t fallback)
{
    size_t key_len = strlen(key);
    for (const char *line = text; line && *line; line = strchr(line, '\n')) {
        while (*line == '\n' || *line == ' ' || *line == '\t') ++line;
        if (strncmp(line, key, key_len) != 0) continue;
        const char *p = line + key_len;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p++ != '=') continue;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p < '0' || *p > '9') return fallback;
        uint32_t v = 0u;
        while (*p >= '0' && *p <= '9' && v < 100000u) v = v * 10u + (uint32_t)(*p++ - '0');
        return v;
    }
    return fallback;
}

bool wm_theme_load(wm_theme_t *theme, const char *path)
{
    if (!theme || !path) return false;
    uint32_t accent = theme->color.accent;
    bool dark = theme->color.dark;
    if (!theme_read_file(path, &accent, &dark)) return false;
    wm_theme_set_accent(theme, accent, dark);

    file_stat_t st;
    if (file_stat(path, &st) < 0 || st.size == 0u || st.size > 65536u) return true;
    int32_t fd = file_open(path, 0);
    if (fd < 0) return true;
    char *text = (char *)malloc((size_t)st.size + 1u);
    int64_t got = text ? file_read(fd, text, st.size) : -1;
    file_close(fd);
    if (got > 0) {
        text[(size_t)got] = '\0';
        theme->title_height = clamp_u32(read_metric(text, "title_height", theme->title_height), 26u, 64u);
        theme->corner_radius = clamp_u32(read_metric(text, "corner_radius", theme->corner_radius), 0u, 28u);
        theme->shadow_size = clamp_u32(read_metric(text, "shadow_size", theme->shadow_size), 0u, 36u);
        theme->dock_height = clamp_u32(read_metric(text, "dock_height", theme->dock_height), 34u, 80u);
    }
    free(text);
    return true;
}
