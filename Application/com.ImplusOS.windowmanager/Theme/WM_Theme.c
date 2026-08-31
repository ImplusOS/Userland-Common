#include "WM_Theme.h"

#include "../../../../Userland/API/File.h"

#include <stdlib.h>
#include <string.h>

static char *trim(char *text)
{
    while (*text == ' ' || *text == '\t' || *text == '\r') ++text;
    size_t len = strlen(text);
    while (len > 0u &&
           (text[len - 1u] == ' ' || text[len - 1u] == '\t' ||
            text[len - 1u] == '\r' || text[len - 1u] == '\n')) {
        text[--len] = '\0';
    }
    return text;
}

static uint32_t parse_u32(const char *text, uint32_t fallback)
{
    if (!text || !*text) return fallback;
    uint64_t value = 0u;
    while (*text >= '0' && *text <= '9') {
        value = value * 10u + (uint64_t)(*text - '0');
        if (value > UINT32_MAX) return fallback;
        ++text;
    }
    return (uint32_t)value;
}

static float parse_float(const char *text, float fallback)
{
    if (!text || !*text) return fallback;
    uint32_t whole = 0u;
    uint32_t fraction = 0u;
    uint32_t divisor = 1u;
    bool any = false;
    while (*text >= '0' && *text <= '9') {
        any = true;
        whole = whole * 10u + (uint32_t)(*text - '0');
        ++text;
    }
    if (*text == '.') {
        ++text;
        while (*text >= '0' && *text <= '9' && divisor < 10000u) {
            fraction = fraction * 10u + (uint32_t)(*text - '0');
            divisor *= 10u;
            ++text;
        }
    }
    return any ? (float)whole + (float)fraction / (float)divisor : fallback;
}

uint32_t wm_theme_parse_color(const char *text, uint32_t fallback)
{
    if (!text) return fallback;
    if (text[0] == '#') ++text;
    else if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) text += 2;

    uint32_t value = 0u;
    uint32_t digits = 0u;
    while (*text) {
        uint32_t digit;
        if (*text >= '0' && *text <= '9') digit = (uint32_t)(*text - '0');
        else if (*text >= 'a' && *text <= 'f') digit = (uint32_t)(*text - 'a') + 10u;
        else if (*text >= 'A' && *text <= 'F') digit = (uint32_t)(*text - 'A') + 10u;
        else break;
        if (digits >= 8u) return fallback;
        value = (value << 4u) | digit;
        ++digits;
        ++text;
    }
    if (digits == 6u) value |= 0xFF000000u;
    return (digits == 6u || digits == 8u) ? value : fallback;
}

void wm_theme_set_defaults(wm_theme_t *theme)
{
    if (!theme) return;
    *theme = (wm_theme_t){
        .bg_top = 0xFFFAFAFAu,
        .bg_mid = 0xFFF5F5F5u,
        .bg_bottom = 0xFFEEEEEEu,
        .bg_glow = 0xFFFFFFFFu,
        .surface = 0xFFFFFFFFu,
        .surface_alt = 0xFFF3F4F6u,
        .surface_hover = 0x18000000u,
        .title_active = 0xFFFFFFFFu,
        .title_inactive = 0xFFF8F9FAu,
        .border = 0x22000000u,
        .border_focus = 0xFF1C1B1Fu,
        .accent = 0xFF1C1B1Fu,
        .accent_alt = 0xFF49454Fu,
        .accent_soft = 0x16000000u,
        .text = 0xFF1C1B1Fu,
        .text_dim = 0xFF49454Fu,
        .danger = 0xFFB3261Eu,
        .shadow = 0x28000000u,
        .dock = 0xF8FFFFFFu,
        .dock_border = 0x18000000u,
        .notification = 0xF8FFFFFFu,
        .title_height = 36u,
        .corner_radius = 12u,
        .shadow_size = 10u,
        .dock_height = 20u,
        .font_normal = 13.0f,
        .font_small = 11.0f,
        .font_title = 13.0f,
    };
}

static void apply_value(wm_theme_t *theme, const char *key, const char *value)
{
#define COLOR_FIELD(name) \
    if (strcmp(key, #name) == 0) { theme->name = wm_theme_parse_color(value, theme->name); return; }
    COLOR_FIELD(bg_top)
    COLOR_FIELD(bg_mid)
    COLOR_FIELD(bg_bottom)
    COLOR_FIELD(bg_glow)
    COLOR_FIELD(surface)
    COLOR_FIELD(surface_alt)
    COLOR_FIELD(surface_hover)
    COLOR_FIELD(title_active)
    COLOR_FIELD(title_inactive)
    COLOR_FIELD(border)
    COLOR_FIELD(border_focus)
    COLOR_FIELD(accent)
    COLOR_FIELD(accent_alt)
    COLOR_FIELD(accent_soft)
    COLOR_FIELD(text)
    COLOR_FIELD(text_dim)
    COLOR_FIELD(danger)
    COLOR_FIELD(shadow)
    COLOR_FIELD(dock)
    COLOR_FIELD(dock_border)
    COLOR_FIELD(notification)
#undef COLOR_FIELD

    if (strcmp(key, "title_height") == 0)
        theme->title_height = parse_u32(value, theme->title_height);
    else if (strcmp(key, "corner_radius") == 0)
        theme->corner_radius = parse_u32(value, theme->corner_radius);
    else if (strcmp(key, "shadow_size") == 0)
        theme->shadow_size = parse_u32(value, theme->shadow_size);
    else if (strcmp(key, "dock_height") == 0)
        theme->dock_height = parse_u32(value, theme->dock_height);
    else if (strcmp(key, "font_normal") == 0)
        theme->font_normal = parse_float(value, theme->font_normal);
    else if (strcmp(key, "font_small") == 0)
        theme->font_small = parse_float(value, theme->font_small);
    else if (strcmp(key, "font_title") == 0)
        theme->font_title = parse_float(value, theme->font_title);
}

bool wm_theme_load(wm_theme_t *theme, const char *path)
{
    if (!theme || !path) return false;
    file_stat_t stat;
    if (file_stat(path, &stat) < 0 || stat.size == 0u || stat.size > 65536u) return false;
    int32_t fd = file_open(path, 0);
    if (fd < 0) return false;

    char *buffer = (char *)malloc((size_t)stat.size + 1u);
    if (!buffer) {
        file_close(fd);
        return false;
    }
    int64_t read_bytes = file_read(fd, buffer, stat.size);
    file_close(fd);
    if (read_bytes <= 0) {
        free(buffer);
        return false;
    }
    buffer[(size_t)read_bytes] = '\0';

    char *line = buffer;
    while (*line) {
        char *next = strchr(line, '\n');
        if (next) *next++ = '\0';
        char *clean = trim(line);
        if (*clean && *clean != ';' && *clean != '#' && *clean != '[') {
            char *equals = strchr(clean, '=');
            if (equals) {
                *equals = '\0';
                apply_value(theme, trim(clean), trim(equals + 1));
            }
        }
        if (!next) break;
        line = next;
    }
    free(buffer);

    if (theme->title_height < 28u) theme->title_height = 28u;
    if (theme->title_height > 64u) theme->title_height = 64u;
    if (theme->corner_radius > 30u) theme->corner_radius = 30u;
    if (theme->shadow_size > 36u) theme->shadow_size = 36u;
    if (theme->dock_height < 20u) theme->dock_height = 20u;
    if (theme->dock_height > 80u) theme->dock_height = 80u;
    return true;
}
