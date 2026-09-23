#include <stdint.h>
#include <stddef.h>

#include "File.h"
#include "Process.h"
#include "Serial.h"
#include "XSession.h"

/*
 * FireFox launcher (the same "Method A" X hosting path as Chromium, Doom and
 * the terminal: Docs/Others/TODO_Doom_Xorg_MethodA.md section 10).
 *
 * Resource/firefox is an unmodified upstream Linux Firefox tarball (156.0).
 * It is an ordinary X11 client, so it reaches the screen the way every other
 * foreign X client here does: Xorg is brought up (or joined, if somebody got
 * there first) and hosted in a WM window by Userland/API/Source/XSession.c,
 * and Firefox is then spawned as a client of that server.
 *
 * THE TWO-PROCESS LAUNCH CHAIN
 *
 * `Resource/firefox` is a 5.5 KiB stub. All it does is readlink
 * /proc/self/exe, append "-bin" to the path and execv() that:
 *
 *     /Userland/FireFox/Resource/firefox  ->  .../firefox-bin
 *
 * so the path below MUST be the stub beside its own firefox-bin -- a path
 * that does not resolve to a directory holding both (the wrong case, or the
 * directory without the Resource/ component) dies with the stub's
 * "Exec failed with error: <strerror>" and never reaches Gecko.
 * firefox-bin in turn finds the rest of the install (omni.ja, libxul.so,
 * defaults/pref) from its own /proc/self/exe, which is why the kernel's
 * "foreign ELF cwd = the executable's directory" rule matters too.
 *
 * THE LIBRARY PATH
 *
 * LD_LIBRARY_PATH is the shared table kernel-side in ProcessManager_Create.c
 * (/lib64:/usr/lib/x86_64-linux-gnu:/usr/lib) and does NOT name this
 * directory. That is fine: unlike Chrome, Firefox's launcher dlopens every
 * entry of dependentlibs.list by absolute path derived from its own
 * directory, so the app tree is self-contained. Everything Firefox needs
 * from outside it (libX11/libgtk-3/libfontconfig/libasound, ...) is already
 * in the staged glibc closure -- see Vendor/LinuxRuntime and
 * Docs/Others/TODO_Chromium_LinuxABI.md.
 *
 * Everything else Firefox wants from the system -- DISPLAY, FONTCONFIG_*,
 * HOME/XDG_* (all /tmp, tmpfs), LANG, and the "do not autolaunch D-Bus"
 * address -- is likewise part of that shared glibc_envp table.
 *
 * process_spawn_with_arg() takes one argument string that the kernel splits
 * on whitespace into argv[1..] (no quoting -- see LINUX_ARGV_MAX in
 * ProcessManager_Create.c), so every flag below has to survive that split.
 */
#define FIREFOX_PATH "/Userland/FireFox/Resource/firefox"

/* The page FireFox opens at start. */
#define FIREFOX_START_URL "https://www.google.com/"

#define FIREFOX_WINDOW_W 1280u
#define FIREFOX_WINDOW_H  720u

/* The uid/gid FireFox runs as (the "implus" account in /etc/passwd), same as
 * Chromium: the sandbox refuses to start as root. The X server this spawns
 * keeps running as root -- only the client is dropped. */
#define FIREFOX_UID 1000u
#define FIREFOX_GID 1000u

/* Firefox has no equivalent of Chromium's --window-position/--window-size
 * (there is no window manager in the X instance XSession starts to ask, and
 * -width/-geometry are not options of the current stub), so the window comes
 * up wherever Gecko puts it on a fresh profile. Nothing to pass here by
 * default; it stays empty unless a launcher wants a first flag. */
/* Optional: when this file exists its contents (switches and/or URLs,
 * whitespace separated, newlines allowed) take the place of
 * FIREFOX_START_URL. A way to open a different page -- or add a flag --
 * without rebuilding the launcher. Not shipped by default. */
#define FIREFOX_BASE_ARGS ""
#define FIREFOX_LAUNCH_ARGS_FILE "/Userland/FireFox/Resource/launch.args"

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
    /* "BASE URL" -- or just "URL" when there is no base, so the string does
     * not start with the space the kernel's splitter would otherwise have
     * to skip. */
    uint32_t at = 0u;
    if (FIREFOX_BASE_ARGS[0] != '\0') {
        at = append(0, FIREFOX_BASE_ARGS, cstrlen(FIREFOX_BASE_ARGS));
        at = append(at, " ", 1u);
    }

    char extra[400];
    int64_t got = -1;
    int32_t fd = file_open(FIREFOX_LAUNCH_ARGS_FILE, 0);
    if (fd >= 0) {
        got = file_read(fd, extra, sizeof(extra) - 1u);
        (void)file_close(fd);
    }
    if (got > 0) {
        serial_write_string("[firefox] using " FIREFOX_LAUNCH_ARGS_FILE "\n");
        (void)append(at, extra, (uint32_t)got);
    } else {
        (void)append(at, FIREFOX_START_URL, cstrlen(FIREFOX_START_URL));
    }
    return g_args;
}

void _start(void)
{
    xsession_t session;

    if (xsession_open(&session,
                      "FireFox",
                      FIREFOX_WINDOW_W,
                      FIREFOX_WINDOW_H) < 0) {
        serial_write_string("[firefox] no X server; giving up\n");
        process_exit(1);
    }

    /* After the X server is up (it keeps running as root), before FireFox is
     * spawned: everything FireFox starts inherits this. */
    (void)process_set_credentials(FIREFOX_UID, FIREFOX_GID);

    int32_t status = xsession_run(&session,
                                  FIREFOX_PATH,
                                  build_args());
    if (status < 0) {
        serial_write_string("[firefox] could not start firefox\n");
    }

    xsession_close(&session);
    process_exit(status < 0 ? 1 : status);
}
