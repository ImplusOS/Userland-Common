#include <stdint.h>
#include <stdio.h>

#include "Process.h"

#define BUSYBOX_PATH "/Userland/BusyBox/Resource/busybox"

void _start(void)
{
    int32_t pid = process_spawn(BUSYBOX_PATH);
    if (pid < 0) {
        process_exit(1);
    }

    int32_t status = 0;
    (void)process_waitpid(pid, &status, 0);
    process_exit(status);
}
