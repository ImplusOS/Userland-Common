#include <stdint.h>
#include <stddef.h>

#include "Process.h"
#include "Serial.h"
#include "XSession.h"

/*
 * Terminal launcher.
 *
 * Runs Debian's unmodified xterm as an X client on the Xorg that
 * Vendor/LinuxRuntime stages, and gives it the statically linked busybox at
 * /bin/sh to talk to. Everything specific to getting an X client onto the
 * ImplusOS desktop lives in Userland/API/Source/XSession.c, which Doom uses
 * too; what is left here is the choice of terminal and how it is configured.
 *
 * Why xterm rather than a smaller terminal: with no -fa argument it draws with
 * X core bitmap fonts, so it needs nothing beyond the font path Xorg already
 * has (stage-xorg.sh installs xfonts-base). st and foot both require Xft +
 * fontconfig before they will draw a single character. Of xterm's fourteen
 * shared libraries, twelve were already in the closure for Xorg and GTK3; only
 * libXft and libutempter were added.
 *
 * The kernel side of this is the pseudo-terminal in Kernel/Source/Core/tty:
 * xterm allocates a pty pair through /dev/ptmx, forks, and the child makes the
 * slave its controlling terminal before exec'ing the shell. None of that
 * existed before -- see Docs/Others/TODO_Terminal_xterm.md.
 */

#define XTERM_PATH "/usr/bin/xterm"

/* -fn fixed        the X core bitmap font. The default already is "fixed",
 *                  but saying so keeps a stray Xft resource from a future
 *                  app-defaults file from pulling fontconfig into the path.
 * +sb              no scrollbar: one less Xaw widget on the first boot.
 * -ut              do not record the session in utmp (xterm spells the
 *                  negative form with a leading '-' for this one option).
 *                  There is no utmp here,
 *                  and xterm does the recording by fork+exec'ing libutempter's
 *                  helper (/usr/lib/utempter/utempter) and waiting for it --
 *                  a binary this image does not ship, so the wait is for a
 *                  child that only ever fails to exec.
 * -geometry 80x24  the size every terminfo entry assumes.
 * -e /bin/sh /bin/imsession
 *                  run the shell directly instead of going through $SHELL and
 *                  getpwuid(), and without the leading '-' of a login shell,
 *                  so no profile scripts have to exist yet. busybox dispatches
 *                  on argv[0] and gives us its ash applet.
 *
 *                  /bin/imsession (written by the top-level Makefile's
 *                  STAGE_POSIX_SHELL) backgrounds `cat /dev/kmsg` before
 *                  exec'ing the shell, so the kernel log lands in this window
 *                  the way it lands on a Linux console -- the only way to see
 *                  it on a machine with no serial cable attached. The kernel
 *                  writes one [app] line per Linux-ABI program launch and
 *                  exit (OS_CONFIG_FOREIGN_LAUNCH_LOG), so "why is this app
 *                  slow to come up" is answerable from the desktop. Passed as
 *                  a script path rather than `sh -c '...'` because the launch
 *                  argument is split on whitespace with no quoting. */
#define XTERM_ARGS "-fn fixed -bg black -fg white +sb -ut " \
                   "-geometry 80x24 -e /bin/sh /bin/imsession"

/* The hosting window, and so the X screen the KMS mirror advertises. Large
 * enough for 80x24 of the 6x13 "fixed" font (486x316) with room for xterm's
 * border, and small enough to sit on the 1280x800 panel with the taskbar. */
#define TERMINAL_WINDOW_W 800u
#define TERMINAL_WINDOW_H 480u

void _start(void)
{
    xsession_t session;

    if (xsession_open(&session, "Terminal", TERMINAL_WINDOW_W,
                      TERMINAL_WINDOW_H) < 0) {
        serial_write_string("[terminal] no X server; giving up\n");
        process_exit(1);
    }

    int32_t status = xsession_run(&session, XTERM_PATH, XTERM_ARGS);
    if (status < 0) {
        serial_write_string("[terminal] could not start xterm\n");
    }

    xsession_close(&session);
    process_exit(status < 0 ? 1 : status);
}
