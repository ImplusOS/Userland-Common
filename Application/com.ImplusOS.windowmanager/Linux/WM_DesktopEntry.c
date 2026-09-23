#include "WM_DesktopEntry.h"

#include "../../../../Userland/API/Source/File.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Searched in order; the first entry that defines an application wins, so a
 * user override in /var/System shadows the shipped description of the same
 * program. /usr/share/applications is whatever Vendor/LinuxRuntime staged. */
static const char *const ENTRY_DIRECTORIES[] = {
    "/var/System/applications",
    "/Userland/com.ImplusOS.windowmanager/Resource/Applications",
    "/usr/local/share/applications",
    "/usr/share/applications",
};

/* Where a bare Exec name is looked up, in $PATH order. */
static const char *const BINARY_DIRECTORIES[] = {
    "/usr/local/bin",
    "/usr/bin",
    "/bin",
    "/usr/games",
};

/* Icon theme search. Large sizes first: the Start menu draws at 36px and the
 * desktop at 48px, and downscaling a 48x48 PNG looks better than doubling a
 * 16x16 one. Only PNG is listed because that is all the shell can decode --
 * Adwaita's scalable/ directory is SVG and xterm ships XPM. */
static const char *const ICON_DIRECTORIES[] = {
    "/Userland/com.ImplusOS.windowmanager/Resource/Icons/Apps",
    "/usr/share/icons/hicolor/64x64/apps",
    "/usr/share/icons/hicolor/48x48/apps",
    "/usr/share/icons/hicolor/256x256/apps",
    "/usr/share/icons/hicolor/128x128/apps",
    "/usr/share/icons/hicolor/32x32/apps",
    "/usr/share/icons/Adwaita/48x48/apps",
    "/usr/share/icons/Adwaita/32x32/apps",
    "/usr/share/icons/Adwaita/16x16/apps",
    "/usr/share/icons/hicolor/24x24/apps",
    "/usr/share/icons/hicolor/22x22/apps",
    "/usr/share/icons/hicolor/16x16/apps",
    "/usr/share/pixmaps",
};

/* The locale the shell's own UI is written in. The spec's full matching rule
 * (lang_COUNTRY@MODIFIER down to lang) is overkill for one language: an exact
 * "[ja]" and nothing else. */
#define ENTRY_LOCALE "ja"

static bool path_exists(const char *path)
{
    file_stat_t stat;
    return path != NULL && file_stat(path, &stat) >= 0 && stat.exists && !stat.is_dir;
}

static bool has_suffix(const char *text, const char *suffix)
{
    size_t text_length = strlen(text);
    size_t suffix_length = strlen(suffix);
    if (suffix_length > text_length) return false;
    return strcmp(text + (text_length - suffix_length), suffix) == 0;
}

static void copy_bounded(char *destination, uint32_t capacity, const char *source)
{
    if (capacity == 0u) return;
    strncpy(destination, source, capacity - 1u);
    destination[capacity - 1u] = '\0';
}

static char *trim(char *text)
{
    while (*text == ' ' || *text == '\t') ++text;
    size_t length = strlen(text);
    while (length > 0u) {
        char last = text[length - 1u];
        if (last != ' ' && last != '\t' && last != '\r') break;
        text[--length] = '\0';
    }
    return text;
}

static bool value_is_true(const char *value)
{
    return strcmp(value, "true") == 0 || strcmp(value, "1") == 0;
}

/* ---------------------------------------------------------------- Exec ---
 *
 * Strip the field codes the spec defines (%f %F %u %U %d %D %n %N %i %c %k
 * %v %m) and unescape %%. They name arguments a file manager would
 * substitute -- a dropped file, a URL, the entry's own icon -- and there is
 * nothing to substitute when the Start menu launches a program with no
 * document, so passing them through would hand the program a literal "%U".
 */
static void strip_field_codes(const char *exec, char *out, uint32_t capacity)
{
    uint32_t at = 0u;
    bool previous_was_space = true;   /* collapse the gap a removed code leaves */
    for (uint32_t i = 0u; exec[i] != '\0' && at + 1u < capacity; ++i) {
        char c = exec[i];
        if (c == '%' && exec[i + 1u] != '\0') {
            char code = exec[i + 1u];
            ++i;
            if (code == '%') {
                out[at++] = '%';
                previous_was_space = false;
            }
            continue;   /* every other code expands to nothing here */
        }
        if (c == '"') continue;        /* no quoting survives the kernel's split */
        if (c == ' ' || c == '\t') {
            if (previous_was_space) continue;
            previous_was_space = true;
            out[at++] = ' ';
            continue;
        }
        previous_was_space = false;
        out[at++] = c;
    }
    while (at > 0u && out[at - 1u] == ' ') --at;
    out[at] = '\0';
}

/* Copy the first whitespace-separated token of `command` into `out`. */
static void first_token(const char *command, char *out, uint32_t capacity)
{
    uint32_t at = 0u;
    while (command[at] != '\0' && command[at] != ' ' && at + 1u < capacity) {
        out[at] = command[at];
        ++at;
    }
    out[at] = '\0';
}

/* Rewrite `command` so its program is an absolute path, and say whether that
 * program exists. A bare name is searched for the way a shell would; this is
 * also the existence check that keeps an entry for a program the image does
 * not ship (Debian's .desktop files describe plenty of them) out of the
 * menu. */
static bool absolutise_exec(char *command, uint32_t capacity)
{
    char program[WM_APP_PATH_MAX];
    first_token(command, program, (uint32_t)sizeof(program));
    if (program[0] == '\0') return false;

    if (program[0] == '/') return path_exists(program);

    char resolved[WM_APP_PATH_MAX * 2u];
    for (uint32_t i = 0u; i < sizeof(BINARY_DIRECTORIES) / sizeof(BINARY_DIRECTORIES[0]); ++i) {
        (void)snprintf(resolved, sizeof(resolved), "%s/%s", BINARY_DIRECTORIES[i], program);
        if (!path_exists(resolved)) continue;
        char rest[WM_APP_ARG_MAX];
        copy_bounded(rest, (uint32_t)sizeof(rest), command + strlen(program));
        char rebuilt[WM_APP_PATH_MAX * 2u + WM_APP_ARG_MAX];
        (void)snprintf(rebuilt, sizeof(rebuilt), "%s%s", resolved, rest);
        copy_bounded(command, capacity, rebuilt);
        return true;
    }
    return false;
}

/* ------------------------------------------------------------ categories ---
 *
 * The first Categories= token that maps to a section wins, so
 * "Network;WebBrowser;" lands in Internet rather than in the catch-all. The
 * table is ordered most-specific-first for that reason.
 */
static wm_app_category_t category_from_list(const char *categories)
{
    static const struct {
        const char *token;
        wm_app_category_t category;
    } table[] = {
        {"WebBrowser",       WM_APP_CATEGORY_INTERNET},
        {"Email",            WM_APP_CATEGORY_INTERNET},
        {"TerminalEmulator", WM_APP_CATEGORY_UTILITY},
        {"FileManager",      WM_APP_CATEGORY_UTILITY},
        {"TextEditor",       WM_APP_CATEGORY_UTILITY},
        {"IDE",              WM_APP_CATEGORY_DEVELOPMENT},
        {"Development",      WM_APP_CATEGORY_DEVELOPMENT},
        {"Game",             WM_APP_CATEGORY_GAME},
        {"Photography",      WM_APP_CATEGORY_GRAPHICS},
        {"Graphics",         WM_APP_CATEGORY_GRAPHICS},
        {"AudioVideo",       WM_APP_CATEGORY_MULTIMEDIA},
        {"Audio",            WM_APP_CATEGORY_MULTIMEDIA},
        {"Video",            WM_APP_CATEGORY_MULTIMEDIA},
        {"Office",           WM_APP_CATEGORY_OFFICE},
        {"Network",          WM_APP_CATEGORY_INTERNET},
        {"Settings",         WM_APP_CATEGORY_SYSTEM},
        {"System",           WM_APP_CATEGORY_SYSTEM},
        {"Utility",          WM_APP_CATEGORY_UTILITY},
    };

    for (uint32_t i = 0u; i < sizeof(table) / sizeof(table[0]); ++i) {
        const char *found = strstr(categories, table[i].token);
        if (found == NULL) continue;
        /* Guard against "Network" matching inside "NetworkSettings": a
         * category token is delimited by ';' or the ends of the string. */
        char after = found[strlen(table[i].token)];
        if (after != '\0' && after != ';') continue;
        if (found != categories && found[-1] != ';') continue;
        return table[i].category;
    }
    return WM_APP_CATEGORY_OTHER;
}

const char *wm_app_category_label(wm_app_category_t category)
{
    switch (category) {
    case WM_APP_CATEGORY_SYSTEM:      return "システム";
    case WM_APP_CATEGORY_INTERNET:    return "インターネット";
    case WM_APP_CATEGORY_DEVELOPMENT: return "開発";
    case WM_APP_CATEGORY_GRAPHICS:    return "グラフィック";
    case WM_APP_CATEGORY_MULTIMEDIA:  return "マルチメディア";
    case WM_APP_CATEGORY_OFFICE:      return "オフィス";
    case WM_APP_CATEGORY_GAME:        return "ゲーム";
    case WM_APP_CATEGORY_UTILITY:     return "ユーティリティ";
    case WM_APP_CATEGORY_OTHER:
    case WM_APP_CATEGORY_COUNT:
    default:                          return "その他";
    }
}

bool wm_desktop_entry_resolve_icon(const char *icon, char *out, uint32_t capacity)
{
    if (icon == NULL || icon[0] == '\0' || out == NULL || capacity == 0u) return false;

    if (icon[0] == '/') {
        if (!path_exists(icon)) return false;
        copy_bounded(out, capacity, icon);
        return true;
    }

    /* A theme icon name has no extension; a few entries name a file anyway. */
    char stem[WM_APP_PATH_MAX];
    copy_bounded(stem, (uint32_t)sizeof(stem), icon);
    if (has_suffix(stem, ".png")) stem[strlen(stem) - 4u] = '\0';

    char candidate[WM_APP_PATH_MAX * 2u];
    for (uint32_t i = 0u; i < sizeof(ICON_DIRECTORIES) / sizeof(ICON_DIRECTORIES[0]); ++i) {
        (void)snprintf(candidate, sizeof(candidate), "%s/%s.png",
                       ICON_DIRECTORIES[i], stem);
        if (!path_exists(candidate)) continue;
        copy_bounded(out, capacity, candidate);
        return true;
    }
    return false;
}

/* -------------------------------------------------------------- parsing ---*/

typedef struct {
    char name[WM_APP_NAME_MAX];
    char name_localized[WM_APP_NAME_MAX];
    char comment[WM_APP_COMMENT_MAX];
    char comment_localized[WM_APP_COMMENT_MAX];
    char exec[WM_APP_ARG_MAX];
    char try_exec[WM_APP_PATH_MAX];
    char icon[WM_APP_PATH_MAX];
    char categories[192];
    char geometry[16];
    bool is_application;
    bool no_display;
    bool linux_backed;
} parsed_entry_t;

/* Split "Key[locale]" into key and locale. Returns the locale, or NULL. */
static char *split_locale(char *key)
{
    char *open_bracket = strchr(key, '[');
    if (open_bracket == NULL) return NULL;
    char *close_bracket = strchr(open_bracket, ']');
    if (close_bracket == NULL) return NULL;
    *open_bracket = '\0';
    *close_bracket = '\0';
    return open_bracket + 1;
}

static void apply_pair(parsed_entry_t *entry, char *key, const char *value)
{
    const char *locale = split_locale(key);
    bool wanted_locale = locale != NULL && strcmp(locale, ENTRY_LOCALE) == 0;

    if (strcmp(key, "Type") == 0) {
        entry->is_application = strcmp(value, "Application") == 0;
    } else if (strcmp(key, "Name") == 0) {
        if (locale == NULL) copy_bounded(entry->name, (uint32_t)sizeof(entry->name), value);
        else if (wanted_locale)
            copy_bounded(entry->name_localized, (uint32_t)sizeof(entry->name_localized), value);
    } else if (strcmp(key, "Comment") == 0 || strcmp(key, "GenericName") == 0) {
        /* GenericName only fills a Comment that is still empty: a real
         * Comment is the more informative of the two. */
        bool is_comment = strcmp(key, "Comment") == 0;
        if (locale == NULL) {
            if (is_comment || entry->comment[0] == '\0')
                copy_bounded(entry->comment, (uint32_t)sizeof(entry->comment), value);
        } else if (wanted_locale) {
            if (is_comment || entry->comment_localized[0] == '\0')
                copy_bounded(entry->comment_localized,
                             (uint32_t)sizeof(entry->comment_localized), value);
        }
    } else if (locale != NULL) {
        return;   /* no other key here has a localized form worth reading */
    } else if (strcmp(key, "Exec") == 0) {
        copy_bounded(entry->exec, (uint32_t)sizeof(entry->exec), value);
    } else if (strcmp(key, "TryExec") == 0) {
        copy_bounded(entry->try_exec, (uint32_t)sizeof(entry->try_exec), value);
    } else if (strcmp(key, "Icon") == 0) {
        copy_bounded(entry->icon, (uint32_t)sizeof(entry->icon), value);
    } else if (strcmp(key, "Categories") == 0) {
        copy_bounded(entry->categories, (uint32_t)sizeof(entry->categories), value);
    } else if (strcmp(key, "NoDisplay") == 0 || strcmp(key, "Hidden") == 0) {
        if (value_is_true(value)) entry->no_display = true;
    } else if (strcmp(key, "X-ImplusOS-Linux") == 0) {
        entry->linux_backed = value_is_true(value);
    } else if (strcmp(key, "X-ImplusOS-Geometry") == 0) {
        copy_bounded(entry->geometry, (uint32_t)sizeof(entry->geometry), value);
    }
}

/* Read one .desktop file into `out`. Only the [Desktop Entry] group is read;
 * a file's other groups are action definitions, which this shell does not
 * offer. */
/* "800x480" -> two dimensions, or nothing. */
static void parse_geometry(const char *text, uint32_t *width, uint32_t *height)
{
    uint32_t values[2] = {0u, 0u};
    uint32_t at = 0u;
    for (uint32_t i = 0u; text[i] != '\0' && at < 2u; ++i) {
        if (text[i] >= '0' && text[i] <= '9') {
            values[at] = values[at] * 10u + (uint32_t)(text[i] - '0');
            if (values[at] > 16384u) return;
        } else if (text[i] == 'x' || text[i] == 'X') {
            ++at;
        } else {
            return;
        }
    }
    if (at != 1u || values[0] == 0u || values[1] == 0u) return;
    *width = values[0];
    *height = values[1];
}

static bool parse_file(const char *path, wm_desktop_entry_t *out)
{
    file_stat_t stat;
    if (file_stat(path, &stat) < 0 || !stat.exists || stat.is_dir ||
        stat.size == 0u || stat.size > WM_DESKTOP_ENTRY_MAX_BYTES) {
        return false;
    }
    int32_t fd = file_open(path, 0);
    if (fd < 0) return false;
    char *text = (char *)malloc((size_t)stat.size + 1u);
    if (text == NULL) {
        (void)file_close(fd);
        return false;
    }
    int64_t bytes = file_read(fd, text, stat.size);
    (void)file_close(fd);
    if (bytes <= 0) {
        free(text);
        return false;
    }
    text[(size_t)bytes] = '\0';

    parsed_entry_t parsed;
    memset(&parsed, 0, sizeof(parsed));
    bool in_desktop_entry = false;

    char *line = text;
    while (line != NULL && *line != '\0') {
        char *next = strchr(line, '\n');
        if (next != NULL) *next++ = '\0';
        char *content = trim(line);
        line = next;

        if (content[0] == '\0' || content[0] == '#') continue;
        if (content[0] == '[') {
            in_desktop_entry = strcmp(content, "[Desktop Entry]") == 0;
            continue;
        }
        if (!in_desktop_entry) continue;

        char *equals = strchr(content, '=');
        if (equals == NULL) continue;
        *equals = '\0';
        char *key = trim(content);
        char *value = trim(equals + 1);
        if (key[0] == '\0' || value[0] == '\0') continue;
        apply_pair(&parsed, key, value);
    }
    free(text);

    if (!parsed.is_application || parsed.no_display) return false;
    if (parsed.exec[0] == '\0') return false;
    if (parsed.try_exec[0] != '\0' && parsed.try_exec[0] == '/' &&
        !path_exists(parsed.try_exec)) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    strip_field_codes(parsed.exec, out->exec, (uint32_t)sizeof(out->exec));
    if (!absolutise_exec(out->exec, (uint32_t)sizeof(out->exec))) return false;

    const char *name = parsed.name_localized[0] != '\0' ? parsed.name_localized
                                                        : parsed.name;
    if (name[0] == '\0') return false;
    copy_bounded(out->name, (uint32_t)sizeof(out->name), name);

    const char *comment = parsed.comment_localized[0] != '\0'
                              ? parsed.comment_localized : parsed.comment;
    copy_bounded(out->comment, (uint32_t)sizeof(out->comment), comment);
    copy_bounded(out->icon, (uint32_t)sizeof(out->icon), parsed.icon);
    out->category = category_from_list(parsed.categories);

    char program[WM_APP_PATH_MAX];
    first_token(out->exec, program, (uint32_t)sizeof(program));
    out->kind = has_suffix(program, ".ELF") ? WM_APP_KIND_NATIVE : WM_APP_KIND_LINUX;
    out->linux_backed = parsed.linux_backed || out->kind == WM_APP_KIND_LINUX;
    parse_geometry(parsed.geometry, &out->width, &out->height);
    return true;
}

/* Has an earlier -- that is, higher-precedence -- directory already supplied
 * this desktop file ID? */
static bool already_present(const wm_desktop_entry_t *entries, uint32_t count,
                            const char *id)
{
    for (uint32_t i = 0u; i < count; ++i) {
        if (strcmp(entries[i].id, id) == 0) return true;
    }
    return false;
}

/* IDs of entries a higher-precedence directory hid with NoDisplay/Hidden.
 * The shadowing has to be recorded separately from the entry list, because
 * an entry that is not shown is not in that list -- and without this, an
 * override whose whole purpose is to hide the packaged entry would let the
 * packaged one through instead. */
typedef struct {
    char ids[WM_MAX_LAUNCHER_APPS][64];
    uint32_t count;
} shadow_list_t;

static bool shadowed(const shadow_list_t *shadow, const char *id)
{
    for (uint32_t i = 0u; i < shadow->count; ++i) {
        if (strcmp(shadow->ids[i], id) == 0) return true;
    }
    return false;
}

static void shadow_add(shadow_list_t *shadow, const char *id)
{
    if (shadow->count >= WM_MAX_LAUNCHER_APPS) return;
    copy_bounded(shadow->ids[shadow->count], 64u, id);
    ++shadow->count;
}

uint32_t wm_desktop_entry_scan(wm_desktop_entry_t *out, uint32_t capacity)
{
    if (out == NULL || capacity == 0u) return 0u;

    static shadow_list_t shadow;
    shadow.count = 0u;

    uint32_t count = 0u;
    for (uint32_t d = 0u;
         d < sizeof(ENTRY_DIRECTORIES) / sizeof(ENTRY_DIRECTORIES[0]) && count < capacity;
         ++d) {
        int32_t dir = file_opendir(ENTRY_DIRECTORIES[d]);
        if (dir < 0) continue;

        file_dirent_t dirent;
        while (count < capacity && file_readdir(dir, &dirent) > 0) {
            if (!has_suffix(dirent.name, ".desktop")) continue;
            if (already_present(out, count, dirent.name) ||
                shadowed(&shadow, dirent.name)) {
                continue;   /* a higher-precedence directory decided this one */
            }
            char path[WM_APP_PATH_MAX + sizeof(((file_dirent_t *)0)->name) + 2u];
            (void)snprintf(path, sizeof(path), "%s/%s",
                           ENTRY_DIRECTORIES[d], dirent.name);
            wm_desktop_entry_t entry;
            if (!parse_file(path, &entry)) {
                shadow_add(&shadow, dirent.name);
                continue;
            }
            copy_bounded(entry.id, (uint32_t)sizeof(entry.id), dirent.name);
            out[count++] = entry;
        }
        (void)file_closedir(dir);
    }
    return count;
}
