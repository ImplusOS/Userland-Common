#include <stdint.h>
#include <stddef.h>

#include "Process.h"
#include "Serial.h"
#include "XSession.h"

/*
 * Doom launcher (method A: Docs/Others/TODO_Doom_Xorg_MethodA.md).
 *
 * linuxxdoom-x86_64 is an unmodified Linux ELF that runs as an X11 + GLX
 * client. ImplusOS has no X server, so Debian's unmodified Xorg (modesetting
 * DDX, Mesa llvmpipe) is brought up first and scans out through the kernel's
 * KMS shim; Doom then starts as an ordinary GLX client. Xorg, Mesa, xkb, the
 * fonts and xorg.conf are all staged into the image by Vendor/LinuxRuntime.
 *
 * Bringing the server up -- and hosting it in a window so it does not fight
 * the compositor for the panel -- is Userland/API/Source/XSession.c, shared
 * with the Terminal launcher. If a terminal is already open, this joins that
 * server instead of starting a second one on the same display.
 *
 * Environment variables cannot be passed through process_spawn, so the
 * foreign-ELF envp is assembled kernel-side in ProcessManager_Create.c
 * (glibc_envp): DISPLAY=:0 is required -- Xlib does not fall back to :0 and
 * XOpenDisplay(NULL) simply fails -- along with LIBGL_ALWAYS_SOFTWARE,
 * GALLIUM_DRIVER=llvmpipe and DOOMWADDIR.
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
        serial_write_string("[doom] no X server; giving up\n");
        process_exit(1);
    }

    int32_t status = xsession_run(&session, DOOM_PATH, NULL);
    if (status < 0) {
        serial_write_string("[doom] could not start linuxxdoom\n");
    }

    xsession_close(&session);
    process_exit(status < 0 ? 1 : status);
}
