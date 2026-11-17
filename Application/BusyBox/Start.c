#include <stdint.h>
#include <stdio.h>

#include "Process.h"

#define BUSYBOX_PATH "/Userland/BusyBox/Resource/busybox"

static int32_t wait_for_exit(int32_t pid)
{
    int32_t status = 0;
    for (;;) {
        int32_t reaped = process_waitpid(pid, &status, 0);
        if (reaped == pid) break;   /* exited, status valid */
        if (reaped < 0) break;      /* no such child */
        sleep_ms(20u);
    }
    return status;
}

void _start(void)
{
    int32_t pid = process_spawn(BUSYBOX_PATH);
    if (pid < 0) {
        process_exit(1);
    }

    process_exit(wait_for_exit(pid));
}
