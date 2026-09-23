#include "WM_Theme.h"

#include "../../../../Userland/API/Source/File.h"

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

static bool parse_bool(const char *text, bool fallback)
{
    if (!text || !*text) return fallback;
    if (strcmp(text, "dark") == 0 || strcmp(text, "Dark") == 0 ||
        strcmp(text, "true") == 0 || strcmp(text, "1") == 0 ||
        strcmp(text, "yes") == 0) return true;
    if (strcmp(text, "light") == 0 || strcmp(text, "Light") == 0 ||
        strcmp(text, "false") == 0 || strcmp(text, "0") == 0 ||
        strcmp(text, "no") == 0) return false;
    return fallback;
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

/* -------------------------------------------------------------- defaults */

void wm_theme_set_type_scale(wm_theme_t *theme, float scale)
{
    if (!theme) return;
    if (!(scale > 0.1f) || scale > 4.0f) scale = 1.0f;
    theme->type_scale = scale;
    m3_typescale_init(&theme->type, scale);
}

void wm_theme_set_seed(wm_theme_t *theme, uint32_t seed, bool dark)
{
    if (!theme) return;
    /* A seed arriving without an alpha channel is still a colour; the
     * palettes only ever look at its RGB. */
    theme->seed = seed | 0xFF000000u;
    theme->dark = dark;
    m3_scheme_init(&theme->color, theme->seed, dark);
}

void wm_theme_set_defaults(wm_theme_t *theme)
{
    if (!theme) return;
    memset(theme, 0, sizeof(*theme));
    wm_theme_set_seed(theme, M3_SEED_BASELINE, false);
    wm_theme_set_type_scale(theme, 1.0f);
    theme->title_height  = WM_TITLE_HEIGHT;
    theme->corner_radius = WM_CORNER_RADIUS;
    theme->shadow_size   = WM_SHADOW_SIZE;
    theme->dock_height   = WM_DOCK_HEIGHT;
}

/* --------------------------------------------------------------- parsing */

/* Per-role overrides. A theme that just wants a different accent sets
 * `seed` and is done; these exist for the rarer case of a hand-tuned
 * scheme, and they are applied after the seed so they win. */
static bool apply_color_override(m3_scheme_t *scheme,
                                 const char *key, const char *value)
{
#define ROLE(name) \
    if (strcmp(key, #name) == 0) { \
        scheme->name = wm_theme_parse_color(value, scheme->name); \
        return true; \
    }
    ROLE(primary)
    ROLE(on_primary)
    ROLE(primary_container)
    ROLE(on_primary_container)
    ROLE(secondary)
    ROLE(on_secondary)
    ROLE(secondary_container)
    ROLE(on_secondary_container)
    ROLE(tertiary)
    ROLE(on_tertiary)
    ROLE(tertiary_container)
    ROLE(on_tertiary_container)
    ROLE(error)
    ROLE(on_error)
    ROLE(error_container)
    ROLE(on_error_container)
    ROLE(surface)
    ROLE(on_surface)
    ROLE(surface_variant)
    ROLE(on_surface_variant)
    ROLE(surface_dim)
    ROLE(surface_bright)
    ROLE(surface_container_lowest)
    ROLE(surface_container_low)
    ROLE(surface_container)
    ROLE(surface_container_high)
    ROLE(surface_container_highest)
    ROLE(outline)
    ROLE(outline_variant)
    ROLE(inverse_surface)
    ROLE(inverse_on_surface)
    ROLE(inverse_primary)
    ROLE(surface_tint)
    ROLE(shadow)
    ROLE(scrim)
#undef ROLE
    return false;
}

static void apply_metric(wm_theme_t *theme, const char *key, const char *value)
{
    if (strcmp(key, "title_height") == 0)
        theme->title_height = parse_u32(value, theme->title_height);
    else if (strcmp(key, "corner_radius") == 0)
        theme->corner_radius = parse_u32(value, theme->corner_radius);
    else if (strcmp(key, "shadow_size") == 0)
        theme->shadow_size = parse_u32(value, theme->shadow_size);
    else if (strcmp(key, "dock_height") == 0)
        theme->dock_height = parse_u32(value, theme->dock_height);
}

/* Walk the file, handing every `key = value` line to `visit`. */
static void for_each_setting(char *buffer, wm_theme_t *theme,
                             void (*visit)(wm_theme_t *, const char *, const char *))
{
    char *line = buffer;
    while (*line) {
        char *next = strchr(line, '\n');
        if (next) *next++ = '\0';
        char *clean = trim(line);
        if (*clean && *clean != ';' && *clean != '#' && *clean != '[') {
            char *equals = strchr(clean, '=');
            if (equals) {
                *equals = '\0';
                visit(theme, trim(clean), trim(equals + 1));
            }
        }
        if (!next) break;
        line = next;
    }
}

/* Pass one: everything the palettes are generated from. */
static void visit_generator(wm_theme_t *theme, const char *key, const char *value)
{
    if (strcmp(key, "seed") == 0)
        theme->seed = wm_theme_parse_color(value, theme->seed) | 0xFF000000u;
    else if (strcmp(key, "mode") == 0)
        theme->dark = parse_bool(value, theme->dark);
    else if (strcmp(key, "type_scale") == 0)
        theme->type_scale = parse_float(value, theme->type_scale);
}

/* Pass two: the metrics and any per-role colour overrides. */
static void visit_override(wm_theme_t *theme, const char *key, const char *value)
{
    if (apply_color_override(&theme->color, key, value)) return;
    apply_metric(theme, key, value);
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

    /* The generator settings have to be read, and the scheme rebuilt,
     * before any per-role override is applied -- otherwise re-seeding would
     * throw the overrides away. for_each_setting() rewrites the buffer as
     * it splits it, so pass two works on a copy. */
    size_t length = (size_t)read_bytes;
    char *second_pass = (char *)malloc(length + 1u);
    if (second_pass) memcpy(second_pass, buffer, length + 1u);

    for_each_setting(buffer, theme, visit_generator);
    wm_theme_set_seed(theme, theme->seed, theme->dark);
    wm_theme_set_type_scale(theme, theme->type_scale);

    if (second_pass) {
        for_each_setting(second_pass, theme, visit_override);
        free(second_pass);
    }
    free(buffer);

    if (theme->title_height < 26u) theme->title_height = 26u;
    if (theme->title_height > 64u) theme->title_height = 64u;
    if (theme->corner_radius > M3_SHAPE_EXTRA_LARGE)
        theme->corner_radius = M3_SHAPE_EXTRA_LARGE;
    if (theme->shadow_size > 36u) theme->shadow_size = 36u;
    if (theme->dock_height < 34u) theme->dock_height = 34u;
    if (theme->dock_height > 80u) theme->dock_height = 80u;
    return true;
}
