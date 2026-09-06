#include <stdint.h>
#include <string.h>

#include "Process.h"
#include "Serial.h"
#include "Socket.h"
#include "Window.h"

/*
 * GTK3 / Wayland launcher.
 *
 * The Wayland session it starts does not need com.ImplusOS.windowmanager:
 * com.ImplusOS.waylandcompositor is a display server in its own right and
 * takes the panel when no WM is running (see its Compositor.c). So this
 * launcher only has to make sure a compositor is listening, then run the
 * stock Debian binary against it.
 *
 * With a launch argument:
 *   an absolute path  -- run that client instead of gtk3-demo
 *                        (e.g. /usr/bin/gtk3-widget-factory)
 *   "panel"           -- force the compositor onto the panel even if a
 *                        window manager is running
 * Both may be given, separated by a space.
 *
 * See Docs/Others/TODO_GTK3_Wayland_LinuxABI.md.
 */
#define COMPOSITOR_PATH "/Userland/com.ImplusOS.waylandcompositor/com.ImplusOS.waylandcompositor.ELF"
#define GTK3_DEMO_PATH  "/usr/bin/gtk3-demo"
#define WL_SOCKET_PATH  "/tmp/wayland-0"

/* The compositor binds its socket almost immediately, but it is started
 * from cold and the scheduler may not run it for a while. Wait for the
 * socket rather than for a guessed interval. */
#define COMPOSITOR_READY_TIMEOUT_MS 10000u
#define COMPOSITOR_POLL_MS            25u

static int32_t wait_for_exit(int32_t pid)
{
    int32_t status = 0;
    for (;;) {
        int32_t reaped = process_waitpid(pid, &status, 0);
        if (reaped == pid) break;   /* exited, status valid */
        if (reaped < 0) break;      /* no such child */
        sleep_ms(100);
    }
    return status;
}

void _start(void)
{
    char arg[128];
    memset(arg, 0, sizeof(arg));
    (void)process_get_launch_argument(arg, (uint32_t)sizeof(arg));

    const char *client = GTK3_DEMO_PATH;
    const char *slash = strchr(arg, '/');
    if (slash != NULL) client = slash;

    const char *compositor_arg = (strstr(arg, "panel") != NULL) ? "panel" : "";

    /* Reuse a compositor that is already up -- a second one could not bind
     * the socket anyway, and two of them would fight over the panel. */
    int32_t compositor = -1;
    if (unix_socket_is_listening(WL_SOCKET_PATH) <= 0) {
        compositor = process_spawn_with_arg(COMPOSITOR_PATH, compositor_arg);
        if (compositor < 0) {
            serial_write_string("[gtk3demo] compositor spawn failed\n");
            process_exit(1);
        }

        uint32_t waited = 0u;
        while (waited < COMPOSITOR_READY_TIMEOUT_MS) {
            if (unix_socket_is_listening(WL_SOCKET_PATH) > 0) break;
            sleep_ms(COMPOSITOR_POLL_MS);
            waited += COMPOSITOR_POLL_MS;
        }
        if (waited >= COMPOSITOR_READY_TIMEOUT_MS) {
            serial_write_string("[gtk3demo] compositor never bound "
                                WL_SOCKET_PATH "\n");
            (void)process_kill(compositor);
            process_exit(1);
        }
    }

    serial_write_string("[gtk3demo] starting ");
    serial_write_string(client);
    serial_write_string("\n");

    int32_t pid = process_spawn(client);
    if (pid < 0) {
        if (compositor > 0) (void)process_kill(compositor);
        process_exit(1);
    }

    int32_t status = wait_for_exit(pid);

    /* Only tear down a compositor this launcher started; one that was
     * already running belongs to somebody else's session. */
    if (compositor > 0) (void)process_kill(compositor);
    process_exit(status);
}
