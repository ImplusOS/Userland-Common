#include "WM_Assets.h"

#include "../Linux/WM_DesktopEntry.h"
#include "../UI/WM_Icons.h"

#include "../../../../Userland/API/Source/File.h"
#include "../../../../Userland/API/Source/Process.h"
#include "../../../../Userland/API/Source/Serial.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void *wm_realloc_sized(void *pointer, size_t old_size, size_t new_size)
{
    if (new_size == 0u) {
        free(pointer);
        return NULL;
    }
    void *replacement = malloc(new_size);
    if (!replacement) return NULL;
    if (pointer) {
        memcpy(replacement, pointer, old_size < new_size ? old_size : new_size);
        free(pointer);
    }
    return replacement;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wtype-limits"
#pragma GCC diagnostic ignored "-Wunused-function"
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_NO_STDIO
#define STBI_NO_SIMD
#define STBI_NO_THREAD_LOCALS
#define STBI_ONLY_PNG
#define STBI_MALLOC(size) malloc(size)
#define STBI_REALLOC(pointer, size) wm_realloc_sized(pointer, 0u, size)
#define STBI_REALLOC_SIZED(pointer, old_size, size) \
    wm_realloc_sized(pointer, old_size, size)
#define STBI_FREE(pointer) free(pointer)
#include "../../../../Vendor/Header/stb_image.h"
#pragma GCC diagnostic pop

typedef struct {
    const char *name;
    const char *path;
    const char *badge;
    wm_app_category_t category;
} default_app_t;

/* Last-resort list, used only when neither a desktop entry nor apps.list is
 * readable. Everything real is described by a file under Resource/Applications. */
static const default_app_t default_apps[] = {
    {"Files", "/Userland/com.ImplusOS.filemanager/com.ImplusOS.filemanager.ELF", "FI",
     WM_APP_CATEGORY_UTILITY},
    {"Editor", "/Userland/com.ImplusOS.editor/com.ImplusOS.editor.ELF", "ED",
     WM_APP_CATEGORY_OFFICE},
    {"Terminal", "/Userland/Terminal/Terminal.ELF", "TE", WM_APP_CATEGORY_UTILITY},
    {"BusyBox", "/Userland/BusyBox/BusyBox.ELF", "BB", WM_APP_CATEGORY_UTILITY},
    {"Serial Monitor", "/Userland/com.ImplusOS.serialmonitor/com.ImplusOS.serialmonitor.ELF",
     "SM", WM_APP_CATEGORY_SYSTEM},
};

/* Host launcher for a foreign X11 program. It creates the window, brings up
 * (or joins) Xorg, points the KMS mirror at the window's backing store and
 * pumps input -- see Userland/Application/com.ImplusOS.linuxapp. */
#define WM_LINUX_HOST_ELF \
    "/Userland/com.ImplusOS.linuxapp/com.ImplusOS.linuxapp.ELF"

uint32_t *wm_assets_load_png(const char *path, uint32_t *width, uint32_t *height)
{
    if (!path || !width || !height) return NULL;
    file_stat_t stat;
    if (file_stat(path, &stat) < 0 || stat.size == 0u || stat.size > 64u * 1024u * 1024u)
        return NULL;
    int32_t fd = file_open(path, 0);
    if (fd < 0) return NULL;
    uint8_t *encoded = (uint8_t *)malloc(stat.size);
    if (!encoded) {
        file_close(fd);
        return NULL;
    }
    int64_t bytes = file_read(fd, encoded, stat.size);
    file_close(fd);
    if (bytes != (int64_t)stat.size) {
        free(encoded);
        return NULL;
    }

    int image_width = 0, image_height = 0, channels = 0;
    uint8_t *rgba = stbi_load_from_memory(encoded, (int)stat.size,
                                           &image_width, &image_height, &channels, 4);
    free(encoded);
    if (!rgba || image_width <= 0 || image_height <= 0 ||
        image_width > 8192 || image_height > 8192) {
        STBI_FREE(rgba);
        return NULL;
    }
    uint64_t pixel_count64 = (uint64_t)(uint32_t)image_width * (uint64_t)(uint32_t)image_height;
    if (pixel_count64 > SIZE_MAX / sizeof(uint32_t)) {
        STBI_FREE(rgba);
        return NULL;
    }
    uint32_t *pixels = (uint32_t *)malloc((size_t)pixel_count64 * sizeof(uint32_t));
    if (!pixels) {
        STBI_FREE(rgba);
        return NULL;
    }
    for (uint64_t i = 0; i < pixel_count64; ++i) {
        pixels[i] = ((uint32_t)rgba[i * 4u + 3u] << 24u) |
                    ((uint32_t)rgba[i * 4u] << 16u) |
                    ((uint32_t)rgba[i * 4u + 1u] << 8u) |
                    (uint32_t)rgba[i * 4u + 2u];
    }
    STBI_FREE(rgba);
    *width = (uint32_t)image_width;
    *height = (uint32_t)image_height;
    return pixels;
}

/* Fill `badge` with the placeholder drawn when an app has no icon. ASCII
 * names get their first two letters; a Japanese name gets its first
 * character, which is one whole UTF-8 sequence -- copying two bytes of it
 * the way the ASCII path does would leave a truncated sequence the font
 * renders as a replacement box. char[4] holds a 3-byte CJK sequence and its
 * terminator exactly. */
static void derive_badge(char badge[4], const char *name)
{
    badge[0] = badge[1] = badge[2] = badge[3] = '\0';
    if (!name || !*name) return;
    unsigned char lead = (unsigned char)name[0];
    if (lead < 0x80u) {
        badge[0] = name[0];
        if (name[1] && (unsigned char)name[1] < 0x80u) badge[1] = name[1];
        return;
    }
    uint32_t length = lead >= 0xF0u ? 4u : lead >= 0xE0u ? 3u : 2u;
    if (length > 3u) return;   /* no room for a 4-byte sequence + terminator */
    for (uint32_t i = 0u; i < length; ++i) {
        if (!name[i]) return;
        badge[i] = name[i];
    }
}

static wm_launcher_app_t *add_app(wm_assets_t *assets, const char *name,
                                  const char *path, const char *badge,
                                  wm_app_category_t category)
{
    if (!assets || assets->app_count >= WM_MAX_LAUNCHER_APPS ||
        !name || !path || !*name || !*path) return NULL;
    wm_launcher_app_t *app = &assets->apps[assets->app_count++];
    memset(app, 0, sizeof(*app));
    strncpy(app->name, name, sizeof(app->name) - 1u);
    strncpy(app->path, path, sizeof(app->path) - 1u);
    app->category = category;
    app->kind = WM_APP_KIND_NATIVE;
    app->vector_icon = -1;
    if (badge && *badge) strncpy(app->badge, badge, sizeof(app->badge) - 1u);
    if (!app->badge[0]) derive_badge(app->badge, app->name);
    return app;
}

/* Turn one desktop entry into a launcher row.
 *
 * A native entry names an ImplusOS ELF and is spawned as it stands. A Linux
 * entry names a glibc binary, which the kernel can load but which draws
 * through X11 and therefore needs a window, a server and an input pump
 * around it: the row points at the host launcher instead and carries the
 * real command line as its argument, in the "Title|command" form the host
 * parses. */
static void add_desktop_entry(wm_assets_t *assets, const wm_desktop_entry_t *entry)
{
    if (!assets || !entry) return;

    if (entry->kind == WM_APP_KIND_NATIVE) {
        char program[WM_APP_PATH_MAX];
        const char *space = strchr(entry->exec, ' ');
        size_t program_length = space ? (size_t)(space - entry->exec)
                                      : strlen(entry->exec);
        if (program_length >= sizeof(program)) return;
        memcpy(program, entry->exec, program_length);
        program[program_length] = '\0';

        wm_launcher_app_t *app = add_app(assets, entry->name, program, NULL,
                                         entry->category);
        if (!app) return;
        if (space) strncpy(app->argument, space + 1, sizeof(app->argument) - 1u);
        strncpy(app->comment, entry->comment, sizeof(app->comment) - 1u);
        strncpy(app->icon_path, entry->icon, sizeof(app->icon_path) - 1u);
        app->vector_icon = wm_icon_kind_from_name(entry->icon);
        app->linux_backed = entry->linux_backed;
        return;
    }

    wm_launcher_app_t *app = add_app(assets, entry->name, WM_LINUX_HOST_ELF,
                                     NULL, entry->category);
    if (!app) return;
    app->kind = WM_APP_KIND_LINUX;
    if (entry->width != 0u && entry->height != 0u) {
        (void)snprintf(app->argument, sizeof(app->argument), "%s|%ux%u|%s",
                       entry->name, (unsigned)entry->width,
                       (unsigned)entry->height, entry->exec);
    } else {
        (void)snprintf(app->argument, sizeof(app->argument), "%s|%s",
                       entry->name, entry->exec);
    }
    strncpy(app->comment, entry->comment, sizeof(app->comment) - 1u);
    strncpy(app->icon_path, entry->icon, sizeof(app->icon_path) - 1u);
    app->vector_icon = wm_icon_kind_from_name(entry->icon);
    app->linux_backed = entry->linux_backed;
}

/* Split a '|'-separated apps.list line in place. Returns the field count. */
static uint32_t split_fields(char *line, char *fields[], uint32_t capacity)
{
    uint32_t count = 0u;
    char *cursor = line;
    while (count < capacity) {
        fields[count++] = cursor;
        char *separator = strchr(cursor, '|');
        if (!separator) break;
        *separator = '\0';
        cursor = separator + 1;
    }
    return count;
}

static bool load_registry(wm_assets_t *assets, const char *path)
{
    file_stat_t stat;
    if (file_stat(path, &stat) < 0 || stat.size == 0u || stat.size > 65536u) return false;
    int32_t fd = file_open(path, 0);
    if (fd < 0) return false;
    char *buffer = (char *)malloc((size_t)stat.size + 1u);
    if (!buffer) {
        file_close(fd);
        return false;
    }
    int64_t bytes = file_read(fd, buffer, stat.size);
    file_close(fd);
    if (bytes <= 0) {
        free(buffer);
        return false;
    }
    buffer[(size_t)bytes] = '\0';

    /* "Name|Path|Badge" is the original format; the three optional trailing
     * fields were added with the desktop-entry work so a line can say as
     * much as a .desktop file does. A file written before them still parses
     * -- the extra fields are simply absent. */
    char *line = buffer;
    while (*line && assets->app_count < WM_MAX_LAUNCHER_APPS) {
        char *next = strchr(line, '\n');
        if (next) *next++ = '\0';
        if (*line && *line != '#') {
            char *fields[6];
            uint32_t field_count = split_fields(line, fields, 6u);
            if (field_count >= 2u) {
                wm_app_category_t category = field_count >= 4u && fields[3][0]
                    ? (wm_app_category_t)atoi(fields[3]) : WM_APP_CATEGORY_OTHER;
                if (category >= WM_APP_CATEGORY_COUNT) category = WM_APP_CATEGORY_OTHER;
                wm_launcher_app_t *app = add_app(assets, fields[0], fields[1],
                                                 field_count >= 3u ? fields[2] : NULL,
                                                 category);
                if (app) {
                    if (field_count >= 5u)
                        strncpy(app->comment, fields[4], sizeof(app->comment) - 1u);
                    if (field_count >= 6u)
                        strncpy(app->argument, fields[5], sizeof(app->argument) - 1u);
                }
            }
        }
        if (!next) break;
        line = next;
    }
    free(buffer);
    return assets->app_count != 0u;
}

/* Order the launcher by category, then by name inside it, so the Start menu
 * can walk the array once and emit a section header whenever the category
 * changes. Insertion sort: the list is short and already close to sorted
 * (each source directory contributes a run). */
static void sort_apps(wm_assets_t *assets)
{
    for (uint32_t i = 1u; i < assets->app_count; ++i) {
        wm_launcher_app_t key = assets->apps[i];
        uint32_t j = i;
        while (j > 0u) {
            const wm_launcher_app_t *previous = &assets->apps[j - 1u];
            bool after = previous->category < key.category ||
                (previous->category == key.category &&
                 strcmp(previous->name, key.name) <= 0);
            if (after) break;
            assets->apps[j] = assets->apps[j - 1u];
            --j;
        }
        assets->apps[j] = key;
    }
}


static void load_system_icon(wm_icon_image_t *icon, const char *name)
{
    if (!icon || !name) return;
    char path[160];
    snprintf(path, sizeof(path),
             "/Userland/com.ImplusOS.windowmanager/Resource/Icons/%s.png",
             name);
    icon->pixels = wm_assets_load_png(path, &icon->width, &icon->height);
}

static wm_icon_image_t *system_icon_at(wm_assets_t *assets, uint32_t index,
                                       const char **name)
{
    if (!assets || !name) return NULL;
    switch (index) {
    case 0:  *name = "Launcher";     return &assets->system_icons.launcher;
    case 1:  *name = "Power";        return &assets->system_icons.power;
    case 2:  *name = "Reboot";       return &assets->system_icons.reboot;
    case 3:  *name = "Notification"; return &assets->system_icons.notification;
    case 4:  *name = "Network";      return &assets->system_icons.network;
    case 5:  *name = "Volume";       return &assets->system_icons.volume;
    case 6:  *name = "Battery";      return &assets->system_icons.battery;
    case 7:  *name = "Close";        return &assets->system_icons.close;
    case 8:  *name = "Maximize";     return &assets->system_icons.maximize;
    case 9:  *name = "Restore";      return &assets->system_icons.restore;
    case 10: *name = "Minimize";     return &assets->system_icons.minimize;
    case 11: *name = "Window";       return &assets->system_icons.window;
    case 12: *name = "Application";  return &assets->system_icons.application;
    case 13: *name = "Search";       return &assets->system_icons.search;
    default: return NULL;
    }
}

#define WM_SYSTEM_ICON_COUNT 14u

/* Load one launcher icon. Two sources, in order:
 *
 *   1. the entry's own Icon=, resolved through the freedesktop icon theme
 *      directories (so a Linux program keeps the icon its distribution
 *      ships);
 *   2. <directory of the ELF>/Resource/App.png, which is where an ImplusOS
 *      application has always kept its icon.
 *
 * A Linux entry has no directory of its own to fall back to -- `path` names
 * the shared host launcher -- so for it only (1) can succeed, and a miss
 * leaves the badge to be drawn. */
static bool load_app_icon(wm_assets_t *assets, uint32_t index)
{
    if (!assets || index >= assets->app_count) return false;
    wm_launcher_app_t *app = &assets->apps[index];
    if (app->icon_pixels) return false;

    if (app->icon_path[0]) {
        char resolved[WM_APP_PATH_MAX * 2u];
        if (wm_desktop_entry_resolve_icon(app->icon_path, resolved,
                                          (uint32_t)sizeof(resolved))) {
            app->icon_pixels = wm_assets_load_png(resolved, &app->icon_width,
                                                  &app->icon_height);
            if (app->icon_pixels) return true;
        }
    }

    /* Second chance for a foreign program: its own basename as an icon
     * name. Debian's xterm entry asks for "mini.xterm", an icon its package
     * ships only as XPM, while the PNG it also ships is called "xterm" --
     * the program's name. That is the common shape of a near miss, and
     * trying it turns a badge back into the distribution's own artwork. */
    if (app->kind == WM_APP_KIND_LINUX) {
        const char *command = strchr(app->argument, '|');
        command = command ? command + 1 : app->argument;
        const char *slash = strrchr(command, '/');
        const char *base = slash ? slash + 1 : command;
        char program[WM_APP_NAME_MAX];
        uint32_t at = 0u;
        while (base[at] != '\0' && base[at] != ' ' && at + 1u < sizeof(program)) {
            program[at] = base[at];
            ++at;
        }
        program[at] = '\0';
        char resolved[WM_APP_PATH_MAX * 2u];
        if (program[0] && wm_desktop_entry_resolve_icon(program, resolved,
                                                        (uint32_t)sizeof(resolved))) {
            app->icon_pixels = wm_assets_load_png(resolved, &app->icon_width,
                                                  &app->icon_height);
            if (app->icon_pixels) return true;
        }
        return false;
    }

    const char *slash = strrchr(app->path, '/');
    if (!slash) return false;
    size_t directory_length = (size_t)(slash - app->path);
    char icon_path[WM_APP_PATH_MAX + 32u];
    if (directory_length + sizeof("/Resource/App.png") > sizeof(icon_path))
        return false;
    memcpy(icon_path, app->path, directory_length);
    strcpy(icon_path + directory_length, "/Resource/App.png");
    app->icon_pixels = wm_assets_load_png(icon_path,
                                          &app->icon_width,
                                          &app->icon_height);
    return app->icon_pixels != NULL;
}

bool wm_assets_init(wm_assets_t *assets)
{
    if (!assets) return false;
    if (!wm_assets_init_metadata(assets)) return false;

    while (wm_assets_load_next_icon(assets)) {
    }

    (void)wm_assets_reload_wallpaper(assets);
    return true;
}

/* Build the launcher list. Desktop entries are the real source; apps.list
 * and the built-in table exist so that an image with a damaged or missing
 * Resource/Applications directory still boots to a usable menu rather than an empty
 * one. */
bool wm_assets_init_metadata(wm_assets_t *assets)
{
    if (!assets) return false;
    memset(assets, 0, sizeof(*assets));

    wm_desktop_entry_t *entries = (wm_desktop_entry_t *)malloc(
        sizeof(wm_desktop_entry_t) * WM_MAX_LAUNCHER_APPS);
    uint32_t entry_count = 0u;
    if (entries) {
        entry_count = wm_desktop_entry_scan(entries, WM_MAX_LAUNCHER_APPS);
        for (uint32_t i = 0; i < entry_count; ++i) add_desktop_entry(assets, &entries[i]);
        free(entries);
    }

    if (assets->app_count == 0u) {
        const char *registry =
            "/Userland/com.ImplusOS.windowmanager/Resource/Apps/apps.list";
        if (!load_registry(assets, registry)) {
            for (uint32_t i = 0; i < sizeof(default_apps) / sizeof(default_apps[0]); ++i)
                add_app(assets, default_apps[i].name, default_apps[i].path,
                        default_apps[i].badge, default_apps[i].category);
        }
    }

    sort_apps(assets);
    assets->metadata_loaded = true;

    /* One line per boot on COM1. Which applications the desktop offers is
     * now decided by files on the image rather than by this binary, so "the
     * menu is missing something" needs an answer from the boot log: how many
     * descriptions were read, and how many of them were foreign programs. */
    uint32_t linux_apps = 0u;
    for (uint32_t i = 0; i < assets->app_count; ++i)
        if (assets->apps[i].linux_backed) ++linux_apps;
    char line[96];
    (void)snprintf(line, sizeof(line),
                   "[wm] launcher: %u app(s) from %u desktop entr(y|ies), "
                   "%u running Linux binaries\n",
                   (unsigned)assets->app_count, (unsigned)entry_count,
                   (unsigned)linux_apps);
    serial_write_string(line);
    return true;
}

bool wm_assets_load_next_icon(wm_assets_t *assets)
{
    if (!assets || !assets->metadata_loaded || assets->icons_loaded) {
        return false;
    }

    if (assets->system_icon_load_index < WM_SYSTEM_ICON_COUNT) {
        uint32_t index = assets->system_icon_load_index++;
        const char *name = NULL;
        wm_icon_image_t *icon = system_icon_at(assets, index, &name);
        if (icon && name) load_system_icon(icon, name);
        return true;
    }

    if (assets->app_icon_load_index < assets->app_count) {
        uint32_t index = assets->app_icon_load_index++;
        (void)load_app_icon(assets, index);
        return true;
    }

    assets->icons_loaded = true;
    return true;
}

bool wm_assets_reload_wallpaper(wm_assets_t *assets)
{
    if (!assets) return false;
    uint32_t width = 0u;
    uint32_t height = 0u;
    uint32_t *pixels = wm_assets_load_png(
        "/Userland/com.ImplusOS.windowmanager/Resource/Background.png",
        &width, &height);
    free(assets->wallpaper_pixels);
    assets->wallpaper_pixels = pixels;
    assets->wallpaper_width = pixels ? width : 0u;
    assets->wallpaper_height = pixels ? height : 0u;
    assets->wallpaper_load_attempted = true;
    return pixels != NULL;
}

void wm_assets_destroy(wm_assets_t *assets)
{
    if (!assets) return;
    for (uint32_t i = 0; i < assets->app_count; ++i) free(assets->apps[i].icon_pixels);
    wm_icon_image_t *icons = &assets->system_icons.launcher;
    uint32_t icon_count = (uint32_t)(sizeof(assets->system_icons) / sizeof(wm_icon_image_t));
    for (uint32_t i = 0; i < icon_count; ++i) free(icons[i].pixels);
    free(assets->wallpaper_pixels);
    memset(assets, 0, sizeof(*assets));
}

int32_t wm_assets_launch(const wm_launcher_app_t *app)
{
    if (!app || !app->path[0]) return -1;
    if (app->argument[0])
        return process_spawn_with_arg(app->path, app->argument);
    return process_spawn(app->path);
}

int32_t wm_assets_find_app(const wm_assets_t *assets, const char *path,
                           const char *argument)
{
    if (!assets || !path) return -1;
    const char *want = argument ? argument : "";
    for (uint32_t i = 0; i < assets->app_count; ++i) {
        if (strcmp(assets->apps[i].path, path) != 0) continue;
        if (strcmp(assets->apps[i].argument, want) != 0) continue;
        return (int32_t)i;
    }
    return -1;
}

bool wm_assets_load_logo(wm_assets_t *assets)
{
    if (!assets) return false;
    assets->system_icons.logo.pixels = wm_assets_load_png(
        "/Userland/com.ImplusOS.windowmanager/Resource/Icons/Logo.png",
        &assets->system_icons.logo.width,
        &assets->system_icons.logo.height
    );
    if (!assets->system_icons.logo.pixels) {
        assets->system_icons.logo.width = 0;
        assets->system_icons.logo.height = 0;
        return false;
    }
    return true;
}
