#pragma once

#include "../Core/WM_State.h"

/*
 * freedesktop.org Desktop Entry support for the shell.
 *
 * ImplusOS stages an entire Debian userland for the Linux-ABI compatibility
 * layer (Vendor/LinuxRuntime), and a Debian userland already describes its
 * applications: a .desktop file under /usr/share/applications says what a program is
 * called, what it does, which category it belongs to, which icon to draw and
 * exactly how to start it. Before this, the Start menu instead read a
 * hand-maintained "Name|Path|Badge" list, so a Linux program only appeared on
 * the desktop once somebody wrote a bespoke C launcher and added a line to
 * that file.
 *
 * Reading the real format instead means:
 *   - anything the runtime stages shows up by itself;
 *   - the user (or an installer) can add an application by dropping one text
 *     file into /var/System/applications, with no rebuild;
 *   - names and comments arrive already translated (Name[ja] is preferred
 *     over Name, which matches the rest of this shell's UI).
 *
 * ImplusOS's own applications are described the same way, from the
 * .desktop files under Resource/Applications, so native and foreign programs are one
 * kind of thing to everything above this file. The legacy Resource/Apps/
 * apps.list is still read, but only when no desktop entry is found at all.
 *
 * Precedence follows the spec: an entry is identified by its desktop file
 * ID (its filename), and the first directory in the search order that has
 * that ID wins. So shipping Resource/Applications/debian-xterm.desktop
 * replaces the one the xterm package installs -- which is how this image
 * gives Debian's terminal the arguments it needs here without touching a
 * third-party file -- and dropping a file of the same name into
 * /var/System/applications lets the user replace either.
 *
 * Deliberately NOT implemented (nothing in the image uses them, and each
 * would be dead code here): Type=Link/Directory, Actions, DBusActivatable,
 * OnlyShowIn/NotShowIn, MimeType dispatch, and the XDG_DATA_DIRS search-path
 * algorithm -- the directory list below is fixed because the image's layout
 * is fixed.
 */

/* The last token of Exec is the longest thing here, and a Chromium-style
 * command line is easily 200 bytes of switches. */
#define WM_DESKTOP_ENTRY_MAX_BYTES (16u * 1024u)

typedef struct {
    /* Desktop file ID: the filename, which is what decides precedence. */
    char id[64];
    char name[WM_APP_NAME_MAX];
    char comment[WM_APP_COMMENT_MAX];
    /* Program to run, absolute, with its arguments; field codes (%f, %U ...)
     * already removed. */
    char exec[WM_APP_ARG_MAX];
    /* Icon= verbatim: either an absolute path or a bare theme icon name.
     * Resolving a name costs a handful of file_stat() calls per candidate
     * directory, so it is left to wm_desktop_entry_resolve_icon() at the
     * point the icon is actually loaded rather than paid for at startup. */
    char icon[WM_APP_PATH_MAX];
    wm_app_category_t category;
    /* How to start it: a native ELF is spawned as it stands, a Linux binary
     * needs the X11 host launcher around it. */
    wm_app_kind_t kind;
    /* Whether what finally runs is a foreign glibc program. Not the same
     * question as `kind`: Chromium, Doom and the terminal are all Linux
     * binaries reached through a native ImplusOS launcher of their own, so
     * they are kind NATIVE and linux_backed. An entry says so with
     * X-ImplusOS-Linux=true; a Linux `kind` implies it. */
    bool linux_backed;
    /* Window size the host launcher should ask for, from
     * X-ImplusOS-Geometry=WxH. Zero means the launcher's default. */
    uint32_t width;
    uint32_t height;
} wm_desktop_entry_t;

/* Scan every applications directory and fill `out` with the entries that
 * should be shown. Returns how many were written. */
uint32_t wm_desktop_entry_scan(wm_desktop_entry_t *out, uint32_t capacity);

/* Turn an Icon= value into an absolute PNG path, or return false. */
bool wm_desktop_entry_resolve_icon(const char *icon, char *out, uint32_t capacity);

/* Japanese section heading for a category. */
const char *wm_app_category_label(wm_app_category_t category);
