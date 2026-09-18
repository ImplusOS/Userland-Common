#include <stdint.h>
#include <stddef.h>

#include "Process.h"
#include "Serial.h"
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
 * DISPLAY=:0, FONTCONFIG_PATH, LANG/LC_ALL, HOME/XDG_* and the D-Bus
 * "refuse immediately" address are all part of the shared glibc_envp table
 * assembled kernel-side in ProcessManager_Create.c -- see that file for why
 * each one is there. Nothing Chromium-specific needs to be added to it: the
 * dbus/inotify ERROR lines it prints on startup are logged and harmless
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

#define CHROME_ARGS \
    "--ozone-platform=x11 " \
    "--window-position=0,0 --window-size=1024,680 " \
    "--user-data-dir=/tmp/chrome-profile " CHROME_START_URL

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

    int32_t status = xsession_run(&session, CHROME_PATH, CHROME_ARGS);
    if (status < 0) {
        serial_write_string("[chromium] could not start chrome\n");
    }

    xsession_close(&session);
    process_exit(status < 0 ? 1 : status);
}
