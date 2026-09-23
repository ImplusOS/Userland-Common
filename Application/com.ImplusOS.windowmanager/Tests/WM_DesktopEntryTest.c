/*
 * Host-side test for the desktop-entry reader (Linux/WM_DesktopEntry.c).
 *
 * The shell's file API is stubbed onto the host filesystem, rooted at a
 * directory this test writes, so the parser is exercised against real files
 * with the awkward shapes that actually occur: Debian's xterm entry (bare
 * Exec, a comment block before the group header), a NoDisplay entry, one
 * naming a program the image does not have, and one with a Japanese Name[ja].
 *
 * Build and run:  make -C Userland/Application/com.ImplusOS.windowmanager test
 */
#include "../Linux/WM_DesktopEntry.h"
#include "../../../API/Source/File.h"

#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char g_root[512];

/* --------------------------------------------------------------- stubs ---*/

/* The shell's handles are int32_t and host handles are pointers, so they go
 * through a table rather than being cast -- a pointer truncated to 32 bits
 * comes back as a different address. */
#define MAX_HANDLES 32
static void *g_handles[MAX_HANDLES];

static int32_t handle_open(void *pointer)
{
    if (!pointer) return -1;
    for (int i = 0; i < MAX_HANDLES; ++i) {
        if (!g_handles[i]) {
            g_handles[i] = pointer;
            return i;
        }
    }
    return -1;
}

static void *handle_get(int32_t handle)
{
    if (handle < 0 || handle >= MAX_HANDLES) return NULL;
    return g_handles[handle];
}

static void *handle_close(int32_t handle)
{
    void *pointer = handle_get(handle);
    if (pointer) g_handles[handle] = NULL;
    return pointer;
}

static void host_path(const char *path, char *out, size_t capacity)
{
    snprintf(out, capacity, "%s%s", g_root, path);
}

int32_t file_stat(const char *path, file_stat_t *stat_out)
{
    char real[1024];
    host_path(path, real, sizeof(real));
    struct stat st;
    if (stat(real, &st) != 0) {
        if (stat_out) memset(stat_out, 0, sizeof(*stat_out));
        return -1;
    }
    if (stat_out) {
        stat_out->size = (uint32_t)st.st_size;
        stat_out->is_dir = S_ISDIR(st.st_mode) ? 1u : 0u;
        stat_out->exists = 1u;
    }
    return 0;
}

int32_t file_open(const char *path, uint64_t flags)
{
    (void)flags;
    char real[1024];
    host_path(path, real, sizeof(real));
    return handle_open(fopen(real, "rb"));
}

int64_t file_read(int32_t fd, void *buffer, uint64_t len)
{
    return (int64_t)fread(buffer, 1u, (size_t)len, (FILE *)handle_get(fd));
}

int32_t file_close(int32_t fd)
{
    return fclose((FILE *)handle_close(fd));
}

int32_t file_opendir(const char *path)
{
    char real[1024];
    host_path(path, real, sizeof(real));
    return handle_open(opendir(real));
}

int32_t file_readdir(int32_t handle, file_dirent_t *out_entry)
{
    struct dirent *entry = readdir((DIR *)handle_get(handle));
    if (!entry) return 0;
    memset(out_entry, 0, sizeof(*out_entry));
    snprintf(out_entry->name, sizeof(out_entry->name), "%s", entry->d_name);
    return 1;
}

int32_t file_closedir(int32_t handle)
{
    return closedir((DIR *)handle_close(handle));
}

/* ---------------------------------------------------------------- setup ---*/

static void write_file(const char *path, const char *contents)
{
    char real[1024];
    host_path(path, real, sizeof(real));
    FILE *f = fopen(real, "wb");
    assert(f != NULL);
    fputs(contents, f);
    fclose(f);
}

static void make_dir(const char *path)
{
    char real[1024];
    host_path(path, real, sizeof(real));
    (void)mkdir(real, 0755);
}

static void build_tree(void)
{
    make_dir("/usr");
    make_dir("/usr/bin");
    make_dir("/usr/share");
    make_dir("/usr/share/applications");
    make_dir("/usr/share/icons");
    make_dir("/usr/share/icons/hicolor");
    make_dir("/usr/share/icons/hicolor/48x48");
    make_dir("/usr/share/icons/hicolor/48x48/apps");
    make_dir("/Userland");
    make_dir("/Userland/com.ImplusOS.windowmanager");
    make_dir("/Userland/com.ImplusOS.windowmanager/Resource");
    make_dir("/Userland/com.ImplusOS.windowmanager/Resource/Applications");
    make_dir("/Userland/com.ImplusOS.filemanager");

    write_file("/usr/bin/xterm", "ELF");
    write_file("/usr/bin/gtk3-demo", "ELF");
    write_file("/usr/share/icons/hicolor/48x48/apps/xterm.png", "PNG");
    write_file("/Userland/com.ImplusOS.filemanager/com.ImplusOS.filemanager.ELF",
               "ELF");

    /* Debian's own xterm entry, comment block and all. */
    write_file("/usr/share/applications/debian-xterm.desktop",
               "# $XTermId: xterm.desktop,v 1.16 $\n"
               "# Copyright 2006-2016,2022 by Thomas E. Dickey\n"
               "[Desktop Entry]\n"
               "Name=XTerm\n"
               "#GenericName=Terminal\n"
               "Comment=standard terminal emulator for the X window system\n"
               "Exec=xterm\n"
               "Terminal=false\n"
               "Type=Application\n"
               "Icon=mini.xterm\n"
               "Categories=System;TerminalEmulator;\n");

    /* NoDisplay: a program that exists but should not be offered. */
    write_file("/usr/share/applications/gtk3-demo.desktop",
               "[Desktop Entry]\n"
               "Name=GTK+ Demo\n"
               "Exec=gtk3-demo\n"
               "Type=Application\n"
               "NoDisplay=true\n");

    /* A program this image does not ship. */
    write_file("/usr/share/applications/debian-uxterm.desktop",
               "[Desktop Entry]\n"
               "Name=UXTerm\n"
               "Exec=uxterm\n"
               "Type=Application\n");

    /* Same file ID in a higher-precedence directory: this one wins, and
     * the packaged one above is never read. */
    write_file("/Userland/com.ImplusOS.windowmanager/Resource/Applications/"
               "debian-xterm.desktop",
               "[Desktop Entry]\n"
               "Type=Application\n"
               "Name=XTerm\n"
               "Exec=/usr/bin/xterm -fn fixed -ut\n"
               "Icon=xterm\n"
               "Categories=Utility;TerminalEmulator;\n"
               "X-ImplusOS-Geometry=800x480\n");

    /* An override whose only job is to hide the packaged entry. Nothing is
     * added to the list, but the packaged file must not appear either. */
    write_file("/usr/share/applications/hidden-me.desktop",
               "[Desktop Entry]\nType=Application\nName=Hidden Me\n"
               "Exec=gtk3-demo\n");
    write_file("/Userland/com.ImplusOS.windowmanager/Resource/Applications/"
               "hidden-me.desktop",
               "[Desktop Entry]\nType=Application\nName=Hidden Me\n"
               "Exec=gtk3-demo\nNoDisplay=true\n");

    /* A native launcher that runs a foreign binary: kind NATIVE, but it
     * should still be reported as Linux-backed. */
    write_file("/usr/share/applications/chromium.desktop",
               "[Desktop Entry]\n"
               "Type=Application\n"
               "Name=Chromium\n"
               "Exec=/Userland/com.ImplusOS.filemanager/com.ImplusOS.filemanager.ELF --browser\n"
               "Categories=Network;WebBrowser;\n"
               "X-ImplusOS-Linux=true\n");

    /* Native entry, localized, with an argument and field codes to strip. */
    write_file("/usr/share/applications/files.desktop",
               "[Desktop Entry]\n"
               "Type=Application\n"
               "Name=Files\n"
               "Name[ja]=ファイル\n"
               "Name[de]=Dateien\n"
               "Comment=Browse files\n"
               "Comment[ja]=ファイルを開く\n"
               "Exec=/Userland/com.ImplusOS.filemanager/com.ImplusOS.filemanager.ELF %U\n"
               "Icon=system-file-manager\n"
               "Categories=Utility;FileManager;\n"
               "[Desktop Action Window]\n"
               "Name=New Window\n"
               "Exec=/bin/should-not-be-read\n");

    /* Not an application. */
    write_file("/usr/share/applications/link.desktop",
               "[Desktop Entry]\nType=Link\nName=Somewhere\nURL=http://x/\n");
}

static const wm_desktop_entry_t *find(const wm_desktop_entry_t *entries,
                                      uint32_t count, const char *name)
{
    for (uint32_t i = 0; i < count; ++i) {
        if (strcmp(entries[i].name, name) == 0) return &entries[i];
    }
    return NULL;
}

int main(void)
{
    snprintf(g_root, sizeof(g_root), "/tmp/wm_desktop_entry_test_%d", (int)getpid());
    if (mkdir(g_root, 0755) != 0) {
        perror("mkdir");
        return 1;
    }
    build_tree();

    wm_desktop_entry_t entries[WM_MAX_LAUNCHER_APPS];
    uint32_t count = wm_desktop_entry_scan(entries, WM_MAX_LAUNCHER_APPS);

    for (uint32_t i = 0; i < count; ++i) {
        printf("  %-10s exec=%-64s kind=%s category=%s\n",
               entries[i].name, entries[i].exec,
               entries[i].kind == WM_APP_KIND_LINUX ? "linux" : "native",
               wm_app_category_label(entries[i].category));
    }
    /* Exactly the three that should survive. */
    assert(count == 3u);
    assert(find(entries, count, "GTK+ Demo") == NULL);   /* NoDisplay */
    assert(find(entries, count, "UXTerm") == NULL);      /* no such program */
    assert(find(entries, count, "Somewhere") == NULL);   /* Type=Link */

    assert(find(entries, count, "Hidden Me") == NULL);   /* shadowed by an
                                                            override that
                                                            hides it */

    const wm_desktop_entry_t *xterm = find(entries, count, "XTerm");
    assert(xterm != NULL);
    /* The override won, not the packaged Exec=xterm. */
    assert(strcmp(xterm->exec, "/usr/bin/xterm -fn fixed -ut") == 0);
    assert(xterm->width == 800u && xterm->height == 480u);
    assert(strcmp(xterm->id, "debian-xterm.desktop") == 0);
    assert(xterm->kind == WM_APP_KIND_LINUX);
    assert(xterm->category == WM_APP_CATEGORY_UTILITY);  /* not System */
    assert(xterm->linux_backed);                         /* implied by kind */
    assert(xterm->comment[0] == '\0');   /* the override supplies none */

    const wm_desktop_entry_t *files = find(entries, count, "ファイル");
    assert(files != NULL);                               /* Name[ja] preferred */
    assert(strcmp(files->comment, "ファイルを開く") == 0);
    assert(files->kind == WM_APP_KIND_NATIVE);
    assert(files->category == WM_APP_CATEGORY_UTILITY);
    assert(!files->linux_backed);
    /* %U removed, and nothing from the [Desktop Action] group leaked in. */
    assert(strcmp(files->exec,
        "/Userland/com.ImplusOS.filemanager/com.ImplusOS.filemanager.ELF") == 0);

    const wm_desktop_entry_t *chromium = find(entries, count, "Chromium");
    assert(chromium != NULL);
    assert(chromium->kind == WM_APP_KIND_NATIVE);   /* spawned directly ... */
    assert(chromium->linux_backed);                 /* ... but runs a Linux binary */
    assert(chromium->category == WM_APP_CATEGORY_INTERNET);

    /* Icon lookup: an exact name hits, a missing one misses. */
    char resolved[512];
    assert(wm_desktop_entry_resolve_icon("xterm", resolved, sizeof(resolved)));
    assert(strcmp(resolved, "/usr/share/icons/hicolor/48x48/apps/xterm.png") == 0);
    assert(!wm_desktop_entry_resolve_icon("mini.xterm", resolved, sizeof(resolved)));
    assert(!wm_desktop_entry_resolve_icon("", resolved, sizeof(resolved)));

    /* Category labels exist for every value, including the sentinel. */
    for (int i = 0; i <= WM_APP_CATEGORY_COUNT; ++i) {
        assert(wm_app_category_label((wm_app_category_t)i)[0] != '\0');
    }

    char command[600];
    snprintf(command, sizeof(command), "rm -rf %s", g_root);
    (void)system(command);

    printf("WM_DesktopEntryTest: %u entries, all assertions passed\n", count);
    return 0;
}
