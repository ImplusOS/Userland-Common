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
 *   --no-sandbox            required: there is no seccomp/user-namespace
 *                           support to sandbox into.
 *   --ozone-platform=x11    the only Ozone backend a prebuilt Chromium ships
 *                           that ImplusOS can host (headless is the other,
 *                           and paints nothing to look at).
 *   --disable-gpu           there is no real GPU; composite in software and
 *                           request no GL implementation at all. Do NOT pass
 *                           --use-gl=swiftshader here: Chromium 155 no longer
 *                           accepts it, so GL resolves to none, the only
 *                           allowed implementation on Ozone/X11 is
 *                           (gl=egl-angle,angle=default), and the in-process
 *                           GPU thread dies with
 *                           "gl_factory_ozone.cc:62] NOTREACHED hit. Expected
 *                           Mock or Stub, actual:0" about 50 s after start.
 *   --single-process --no-zygote
 *                           COW fork is implemented but disabled by default
 *                           (KERNEL_COW_FORK=0): a zygote fork per renderer
 *                           would eager-copy the ~330 MB resident image on
 *                           every tab. Running the renderer in the browser
 *                           process sidesteps that entirely; the tab strip,
 *                           omnibox and toolbar are browser-process UI either
 *                           way.
 *   --window-position=0,0 --window-size=...
 *                           there is no X window manager inside the Xorg
 *                           instance XSession starts, so Chromium has to be
 *                           told explicitly to fill the mirrored surface.
 *   --user-data-dir=/tmp/chrome-profile
 *                           HOME=/tmp is tmpfs-backed and writable; naming
 *                           the profile dir explicitly avoids depending on
 *                           Chromium's default (~/.config/google-chrome).
 *   --no-first-run --no-default-browser-check --disable-sync
 *   --disable-background-networking --disable-component-update
 *                           skip first-run flows and network calls this
 *                           environment cannot serve (no real DNS/TLS to
 *                           Google's update/sync backends).
 */
#define CHROME_PATH "/Userland/Chromium/Resource/chrome"

/* The page Chromium opens at start. */
#define CHROME_START_URL "http://example.com/"

#define CHROME_WINDOW_W 1024u
#define CHROME_WINDOW_H 680u

#define CHROME_ARGS \
    "--no-sandbox --disable-gpu " \
    "--ozone-platform=x11 " \
    "--single-process --no-zygote " \
    "--window-position=0,0 --window-size=1024,680 " \
    "--user-data-dir=/tmp/chrome-profile " \
    "--no-first-run --no-default-browser-check --disable-sync " \
    "--disable-background-networking --disable-component-update " \
    "--disable-breakpad --disable-crash-reporter " \
    "--disable-crashpad-for-testing " CHROME_START_URL

void _start(void)
{
    xsession_t session;

    if (xsession_open(&session, "Chromium", CHROME_WINDOW_W,
                      CHROME_WINDOW_H) < 0) {
        serial_write_string("[chromium] no X server; giving up\n");
        process_exit(1);
    }

    int32_t status = xsession_run(&session, CHROME_PATH, CHROME_ARGS);
    if (status < 0) {
        serial_write_string("[chromium] could not start chrome\n");
    }

    xsession_close(&session);
    process_exit(status < 0 ? 1 : status);
}
