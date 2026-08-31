#include "WM_Assets.h"

#include "../../../../Userland/API/Source/File.h"

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
} default_app_t;

static const default_app_t default_apps[] = {
    {"Terminal", "/Userland/com.ImplusOS.shell/com.ImplusOS.shell.ELF", "TE"},
    {"Files", "/Userland/com.ImplusOS.filemanager/com.ImplusOS.filemanager.ELF", "FI"},
    {"Editor", "/Userland/com.ImplusOS.editor/com.ImplusOS.editor.ELF", "ED"},
    {"Processes", "/Userland/com.ImplusOS.procman/com.ImplusOS.procman.ELF", "PR"},
    {"OS Debug", "/Userland/com.ImplusOS.osdebug/com.ImplusOS.osdebug.ELF", "DB"},
    {"Implus Store", "/Userland/com.ImplusOS.ImplusStore/com.ImplusOS.ImplusStore.ELF", "ST"},
    {"Network", "/Userland/com.ImplusOS.NetworkTest/com.ImplusOS.NetworkTest.ELF", "NW"},
    {"Virtual Machine", "/Userland/com.ImplusOS.vm/com.ImplusOS.vm.ELF", "VM"},
    {"Example", "/Userland/com.ImplusOS.exampleApp/com.ImplusOS.exampleApp.ELF", "EX"},
    {"Settings", "/Userland/com.ImplusOS.settings/com.ImplusOS.settings.ELF", "SE"},
    {"PNG Viewer", "/Userland/com.ImplusOS.pngTest/com.ImplusOS.pngTest.ELF", "PV"},
    {"System Info", "/Userland/com.ImplusOS.version/com.ImplusOS.version.ELF", "IN"},
};

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

static void add_app(wm_assets_t *assets, const char *name,
                    const char *path, const char *badge)
{
    if (!assets || assets->app_count >= WM_MAX_LAUNCHER_APPS ||
        !name || !path || !*name || !*path) return;
    wm_launcher_app_t *app = &assets->apps[assets->app_count++];
    memset(app, 0, sizeof(*app));
    strncpy(app->name, name, sizeof(app->name) - 1u);
    strncpy(app->path, path, sizeof(app->path) - 1u);
    if (badge && *badge) strncpy(app->badge, badge, sizeof(app->badge) - 1u);
    if (!app->badge[0]) {
        app->badge[0] = app->name[0];
        app->badge[1] = app->name[1] ? app->name[1] : '\0';
    }
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

    char *line = buffer;
    while (*line && assets->app_count < WM_MAX_LAUNCHER_APPS) {
        char *next = strchr(line, '\n');
        if (next) *next++ = '\0';
        if (*line && *line != '#') {
            char *first = strchr(line, '|');
            if (first) {
                *first++ = '\0';
                char *second = strchr(first, '|');
                if (second) *second++ = '\0';
                add_app(assets, line, first, second);
            }
        }
        if (!next) break;
        line = next;
    }
    free(buffer);
    return assets->app_count != 0u;
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

static bool load_app_icon(wm_assets_t *assets, uint32_t index)
{
    if (!assets || index >= assets->app_count) return false;
    wm_launcher_app_t *app = &assets->apps[index];
    if (app->icon_pixels) return false;
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

bool wm_assets_init_metadata(wm_assets_t *assets)
{
    if (!assets) return false;
    memset(assets, 0, sizeof(*assets));
    const char *registry =
        "/Userland/com.ImplusOS.windowmanager/Resource/Apps/apps.list";
    if (!load_registry(assets, registry)) {
        for (uint32_t i = 0; i < sizeof(default_apps) / sizeof(default_apps[0]); ++i)
            add_app(assets, default_apps[i].name, default_apps[i].path, default_apps[i].badge);
    }
    assets->metadata_loaded = true;
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
