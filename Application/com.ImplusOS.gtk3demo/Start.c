#include <stdint.h>
#include <stdio.h>

#include "Process.h"

/*
 * GTK3 / Wayland launcher.
 *
 * 1. start the minimal Wayland compositor (com.ImplusOS.waylandcompositor),
 *    which binds /tmp/wayland-0 and bridges surfaces into a WM window;
 * 2. give it a moment to bind the socket;
 * 3. run the stock Debian gtk3-demo (Linux ABI, unmodified) with
 *    GDK_BACKEND=wayland (set in the kernel's glibc_envp).
 *
 * See Docs/Others/TODO_GTK3_Wayland_LinuxABI.md.
 */
#define COMPOSITOR_PATH "/Userland/com.ImplusOS.waylandcompositor/com.ImplusOS.waylandcompositor.ELF"
#define GTK3_DEMO_PATH  "/usr/bin/gtk3-demo"

void _start(void)
{
    int32_t comp = process_spawn(COMPOSITOR_PATH);
    if (comp < 0) {
        /* keep going: gtk3-demo will just fail to connect and exit */
        printf("[gtk3demo] compositor spawn failed\n");
    }

    /* let the compositor create its window and bind the socket */
    sleep_ms(1500);

    int32_t pid = process_spawn(GTK3_DEMO_PATH);
    if (pid < 0) {
        process_exit(1);
    }

    int32_t status = 0;
    (void)process_waitpid(pid, &status, 0);
    process_exit(status);
}
