#include <stdint.h>
#include <stddef.h>

#include "File.h"
#include "Process.h"
#include "Serial.h"
#include "SystemInfo.h"
#include "XSession.h"

/*
 * Chromium launcher (shares the Doom "Method A" X hosting path:
 * Docs/Others/TODO_Doom_Xorg_MethodA.md, Docs/Others/TODO_Chromium_LinuxABI.md
 * section 10).
 *
 * Resource/chrome is an unmodified upstream Linux desktop Chromium binary.
 * It is a normal X11 (Ozone "x11" backend) client, so it reaches the screen
 * the same way linuxxdoom does: Xorg is brought up (or joined, if a terminal
 * or Doom already started it) and hosted in a WM window via
 * Userland/API/Source/XSession.c, and Chromium is then spawned as an ordinary
 * client of that server.
 *
 * DISPLAY=:0, FONTCONFIG_PATH, the D-Bus "refuse immediately" address and
 * the rest of the desktop-stack variables come from the shared environment
 * xsession_run() builds (API/Source/LinuxEnv.c); LANG/LC_ALL, HOME/XDG_*,
 * SHELL/TERM and LD_LIBRARY_PATH are the kernel's generic defaults for a
 * Linux-ABI process (ProcessManager_Create.c). Nothing Chromium-specific
 * needs to be added to either: the dbus/inotify ERROR lines it prints on
 * startup are logged and harmless
 * (Docs/Others/TODO_Chromium_LinuxABI.md 10.-3).
 *
 * process_spawn_with_arg() takes one argument string that the kernel splits
 * on whitespace into argv[1..], so every flag below has to survive that
 * split (no quoting is available -- see ProcessManager_Create.c's comment
 * next to LINUX_ARGV_MAX).
 *
 * THE COMMAND LINE
 *
 * A plain desktop Chromium command line: a URL, a window and a profile. None
 * of the switches that used to hold Chromium back are needed any more --
 * it runs with its namespace sandbox, a separate GPU process (falling back to
 * SwiftShader on its own, there being no GPU), renderers forked from the
 * zygote, the first-run flow, Sync/background networking/component updates
 * and the out-of-process Crashpad handler, exactly as on a Linux desktop.
 * What each of those needed from the kernel is in
 * Docs/Others/TODO_Chromium_LinuxABI.md sections 11 and 12.
 *
 *   --ozone-platform=x11    the only Ozone backend a prebuilt Chromium ships
 *                           that ImplusOS can host (headless is the other,
 *                           and paints nothing to look at). Not a workaround:
 *                           X11 is how this system does graphics.
 *   --window-position=0,0 --window-size=...
 *                           there is no X window manager inside the Xorg
 *                           instance XSession starts, so Chromium has to be
 *                           told explicitly to fill the mirrored surface.
 *   --user-data-dir=/tmp/chrome-profile
 *                           HOME=/tmp is tmpfs-backed and writable; naming
 *                           the profile dir explicitly avoids depending on
 *                           Chromium's default (~/.config/chromium).
 *   --ignore-gpu-blocklist  turn the GPU process's work on -- but only when
 *                           the kernel reports a real 3D render node (virgl).
 *                           Chromium's software rendering list disables every
 *                           GPU feature for a software renderer: compositing,
 *                           rasterization, canvas, WebGL, WebGPU and video
 *                           decode all fall back to the browser's software
 *                           paths. With it they run in the GPU process through
 *                           ANGLE/Mesa. On llvmpipe that is a pessimisation,
 *                           not an optimisation: ANGLE-on-llvmpipe plus the
 *                           GLX/DRI3 presentation path measured ~5 fps on a
 *                           854x480 YouTube page, against ~30 fps for the
 *                           plain software compositor (Skia CPU + X11 output).
 *                           So the switch is added only for a real GPU; see
 *                           build_args().
 *
 * Chromium runs as uid/gid 1000 (CHROME_UID below): it refuses to start its
 * sandbox as root. The X server it connects to keeps running as root.
 */
#define CHROME_PATH "/Userland/Chromium/Resource/chrome"

/* The page Chromium opens at start. */
#define CHROME_START_URL "http://example.com/"

#define CHROME_WINDOW_W 1024u
#define CHROME_WINDOW_H 680u

/* The uid/gid Chromium runs as (the "implus" account in /etc/passwd).
 * Chromium will not start its sandbox as root. */
#define CHROME_UID 1000u
#define CHROME_GID 1000u

#define CHROME_BASE_ARGS \
    "--ozone-platform=x11 " \
    "--window-position=0,0 --window-size=1024,680 " \
    "--user-data-dir=/tmp/chrome-profile " \

/* Optional: when this file exists its contents (switches and/or URLs,
 * whitespace separated, newlines allowed) take the place of
 * CHROME_START_URL. A way to open a different page -- or add a switch --
 * without rebuilding the launcher. Not shipped by default. */
#define CHROME_LAUNCH_ARGS_FILE "/Userland/Chromium/Resource/launch.args"

static char g_args[512]; /* process_t.launch_argument */

static uint32_t append(uint32_t at, const char *text, uint32_t len)
{
    for (uint32_t i = 0; i < len && at + 1u < sizeof(g_args); ++i) {
        char c = text[i];
        if (c == '\n' || c == '\r' || c == '\t') {
            c = ' ';
        }
        g_args[at++] = c;
    }
    g_args[at] = '\0';
    return at;
}

static uint32_t cstrlen(const char *s)
{
    uint32_t n = 0;
    while (s[n] != '\0') {
        ++n;
    }
    return n;
}

static const char *build_args(void)
{
    uint32_t at = append(0, CHROME_BASE_ARGS " ", cstrlen(CHROME_BASE_ARGS " "));

    /* GPU features only pay off on a real render node. With llvmpipe the
     * browser's own software compositor is faster (see the comment above),
     * so leave Chromium's software rendering list in effect there. */
    system_graphics_info_t graphics = {0};
    if (os_get_graphics_info(&graphics) >= 0 &&
        (graphics.acceleration_flags & SYSTEM_GRAPHICS_ACCEL_3D) != 0u) {
        at = append(at, "--ignore-gpu-blocklist ",
                    cstrlen("--ignore-gpu-blocklist "));
    }

    char extra[400];
    int64_t got = -1;
    int32_t fd = file_open(CHROME_LAUNCH_ARGS_FILE, 0);
    if (fd >= 0) {
        got = file_read(fd, extra, sizeof(extra) - 1u);
        (void)file_close(fd);
    }
    if (got > 0) {
        serial_write_string("[chromium] using " CHROME_LAUNCH_ARGS_FILE "\n");
        (void)append(at, extra, (uint32_t)got);
    } else {
        (void)append(at, CHROME_START_URL, cstrlen(CHROME_START_URL));
    }
    return g_args;
}

void _start(void)
{
    xsession_t session;

    if (xsession_open(&session, "Chromium", CHROME_WINDOW_W,
                      CHROME_WINDOW_H) < 0) {
        serial_write_string("[chromium] no X server; giving up\n");
        process_exit(1);
    }

    /* After the X server is up (it keeps running as root), before Chromium
     * is spawned: everything Chromium starts inherits this. */
    (void)process_set_credentials(CHROME_UID, CHROME_GID);

    int32_t status = xsession_run(&session, CHROME_PATH, build_args());
    if (status < 0) {
        serial_write_string("[chromium] could not start chrome\n");
    }

    xsession_close(&session);
    process_exit(status < 0 ? 1 : status);
}
