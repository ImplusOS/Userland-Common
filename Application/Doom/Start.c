#include <stdint.h>
#include <stddef.h>

#include "Process.h"
#include "XSession.h"

/*
 * Doom launcher (method A: Docs/Others/TODO_Doom_Xorg_MethodA.md).
 *
 * linuxxdoom-x86_64 is an unmodified Linux ELF that runs as an X11 + GLX
 * client. ImplusOS has no X server, so Debian's unmodified Xorg (modesetting
 * DDX, glamor over Mesa) is brought up first and scans out through the
 * kernel's KMS shim; Doom then starts as an ordinary GLX client. Xorg, Mesa,
 * xkb, the fonts and xorg.conf are all staged into the image by
 * Vendor/LinuxRuntime.
 *
 * Bringing the server up -- and hosting it in a window so it does not fight
 * the compositor for the panel -- is Userland/API/Source/XSession.c, shared
 * with the Terminal launcher. If a terminal is already open, this joins that
 * server instead of starting a second one on the same display.
 *
 * Environment variables are data, not code: Environment/linuxxdoom-x86_64.env
 * beside this file names the IWAD/soundfont and the null audio backends, and
 * xsession_run() loads it as the program profile of DOOM_PATH, together with
 * the x11-client role profile (DISPLAY, XKB_CONFIG_ROOT, Mesa: virgl when the
 * kernel exposes a 3D render node, llvmpipe otherwise). See
 * Userland/API/Source/LinuxEnv.h. The kernel only supplies the generic
 * defaults (PATH, HOME, locale, XDG_*).
 */

#define DOOM_PATH "/Userland/Doom/Resource/linuxxdoom-x86_64"

/* Size of the window Xorg renders into, and so the only mode the KMS shim
 * advertises. Kept below the 1280x800 panel so the window, its decorations and
 * the taskbar all fit. */
#define DOOM_WINDOW_W 1024u
#define DOOM_WINDOW_H 640u

void _start(void)
{
    xsession_t session;

    if (xsession_open(&session, "Doom (X11)", DOOM_WINDOW_W,
                      DOOM_WINDOW_H) < 0) {
        process_exit(1);
    }

    int32_t status = xsession_run(&session, DOOM_PATH, NULL);

    xsession_close(&session);
    process_exit(status < 0 ? 1 : status);
}
