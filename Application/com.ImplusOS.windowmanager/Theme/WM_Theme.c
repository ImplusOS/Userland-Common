#include "WM_Theme.h"

#include "../../../../Userland/API/Source/File.h"

#include <stdlib.h>
#include <string.h>

/* `c` as a pure grey of the same brightness, alpha kept. */
static uint32_t to_grey(uint32_t c)
{
    uint32_t l = (((c >> 16) & 0xFFu) * 299u + ((c >> 8) & 0xFFu) * 587u +
                  (c & 0xFFu) * 114u) / 1000u;
    return (c & 0xFF000000u) | (l << 16) | (l << 8) | l;
}

/*
 * The shell draws in white and greys only: the shared palette is built as
 * usual (so the light/dark surfaces stay the same), then the accent and the
 * danger colour are swapped for fixed greys and every entry is stripped of
 * its hue. Applications drawing with UI.h keep the coloured accent; this
 * only affects the window manager's own UI.
 */
static void wm_theme_set_accent(wm_theme_t *theme, uint32_t accent, bool dark)
{
    if (!theme) return;
    theme_t *t = &theme->color;
    theme_init(t, accent, dark);
    if (dark) {
        t->accent    = 0xFFC8C8C8u;
        t->on_accent = 0xFF111111u;
        t->danger    = 0xFFB4B4B4u;
        t->on_danger = 0xFF111111u;
    } else {
        t->accent    = 0xFF5F5F5Fu;
        t->on_accent = 0xFFFFFFFFu;
        t->danger    = 0xFF404040u;
        t->on_danger = 0xFFFFFFFFu;
    }
    t->accent_soft = color_mix(t->surface, t->accent, dark ? 30u : 16u);

    uint32_t *all[] = { &t->accent, &t->on_accent, &t->accent_soft, &t->bg,
                        &t->surface, &t->surface_alt, &t->border, &t->text,
                        &t->text_dim, &t->danger, &t->on_danger };
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); ++i)
        *all[i] = to_grey(*all[i]);
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
