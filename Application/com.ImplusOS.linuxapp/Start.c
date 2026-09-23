#include <stdint.h>
#include <stddef.h>

#include "Graphics.h"
#include "Process.h"
#include "Serial.h"
#include "XSession.h"

/*
 * Generic host for a foreign X11 program.
 *
 * Every Linux GUI program on this system reaches the screen the same way:
 * a window manager window is created, Xorg is brought up (or joined) with
 * that window's backing store registered as the KMS scanout target, the
 * program is spawned as an ordinary client of that server, and the window's
 * input is forwarded into /dev/input/event*. All of that is
 * Userland/API/Source/XSession.c. What used to sit on top of it was one
 * hand-written launcher per program -- Doom, Terminal, Chromium -- each a
 * copy of the same twenty lines differing only in a path, a window size and
 * a command line.
 *
 * This is that launcher with those three things taken as an argument, so
 * adding a Linux program to the desktop is a text file rather than a source
 * file: the window manager reads the desktop entries under
 * /usr/share/applications, turns
 * each entry into a Start-menu row, and starts this with the entry's name
 * and Exec line. See Userland/Application/com.ImplusOS.windowmanager/Linux/
 * WM_DesktopEntry.c.
 *
 * The launchers that remain are the ones that do something this cannot:
 * Chromium drops privileges to uid 1000 before spawning (its sandbox
 * refuses to start as root), and com.ImplusOS.gtk3demo runs a Wayland
 * compositor instead of an X server.
 *
 * ARGUMENT
 *
 *   [Title|][WxH|]/usr/bin/program --flags
 *
 * The whole string arrives verbatim through process_get_launch_argument()
 * -- it is only split on whitespace for a Linux-ABI process, and this is a
 * native one -- so a title may contain spaces. Both leading fields are
 * optional: with neither, the window is named after the program's basename
 * and gets the default size.
 */

#define DEFAULT_WINDOW_W 1024u
#define DEFAULT_WINDOW_H 680u

/* Leave the taskbar and a margin visible rather than covering the desktop
 * with a window whose contents X sized to the whole panel. */
#define SCREEN_MARGIN_W 40u
#define SCREEN_MARGIN_H 120u

static uint32_t parse_u32(const char *text, const char **end)
{
    uint32_t value = 0u;
    while (*text >= '0' && *text <= '9') {
        value = value * 10u + (uint32_t)(*text - '0');
        if (value > 100000u) value = 100000u;
        ++text;
    }
    if (end) *end = text;
    return value;
}

/* "1024x680" -- a geometry field, as opposed to a title or a path. */
static int parse_geometry(const char *field, uint32_t *width, uint32_t *height)
{
    const char *cursor = NULL;
    uint32_t w = parse_u32(field, &cursor);
    if (w == 0u || cursor == field || *cursor != 'x') return 0;
    const char *after = NULL;
    uint32_t h = parse_u32(cursor + 1, &after);
    if (h == 0u || *after != '\0') return 0;
    *width = w;
    *height = h;
    return 1;
}

static const char *basename_of(const char *path)
{
    const char *last = path;
    for (const char *p = path; *p; ++p) {
        if (*p == '/') last = p + 1;
    }
    return last;
}

static uint32_t clamp(uint32_t value, uint32_t limit)
{
    return value > limit ? limit : value;
}

void _start(void)
{
    static char argument[512];
    if (process_get_launch_argument(argument, (uint32_t)sizeof(argument)) < 0 ||
        argument[0] == '\0') {
        serial_write_string("[linuxapp] no program given\n");
        process_exit(1);
    }

    /* Split the optional leading fields off in place. */
    char *title = NULL;
    uint32_t width = DEFAULT_WINDOW_W;
    uint32_t height = DEFAULT_WINDOW_H;
    char *command = argument;
    for (int field = 0; field < 2; ++field) {
        char *separator = NULL;
        for (char *p = command; *p; ++p) {
            if (*p == '|') { separator = p; break; }
        }
        if (separator == NULL) break;
        *separator = '\0';
        if (!parse_geometry(command, &width, &height)) {
            if (title == NULL) title = command;
            else break;   /* a second non-geometry field is part of nothing */
        }
        command = separator + 1;
    }
    if (command[0] != '/') {
        serial_write_string("[linuxapp] program path must be absolute\n");
        process_exit(1);
    }

    /* The program is argv[0]; everything after the first space is its
     * arguments, which xsession_run() hands to the kernel to split. */
    char *arguments = NULL;
    for (char *p = command; *p; ++p) {
        if (*p == ' ') { *p = '\0'; arguments = p + 1; break; }
    }
    if (title == NULL || title[0] == '\0') title = (char *)basename_of(command);

    uint32_t screen_w = get_display_width();
    uint32_t screen_h = get_display_height();
    if (screen_w > SCREEN_MARGIN_W) width = clamp(width, screen_w - SCREEN_MARGIN_W);
    if (screen_h > SCREEN_MARGIN_H) height = clamp(height, screen_h - SCREEN_MARGIN_H);

    serial_write_string("[linuxapp] hosting ");
    serial_write_string(command);
    serial_write_string("\n");

    xsession_t session;
    if (xsession_open(&session, title, width, height) < 0) {
        serial_write_string("[linuxapp] no X server; giving up\n");
        process_exit(1);
    }

    int32_t status = xsession_run(&session, command,
                                  (arguments && arguments[0]) ? arguments : NULL);
    if (status < 0) {
        serial_write_string("[linuxapp] could not start ");
        serial_write_string(command);
        serial_write_string("\n");
    }

    xsession_close(&session);
    process_exit(status < 0 ? 1 : status);
}
