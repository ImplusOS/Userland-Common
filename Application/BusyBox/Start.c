#include <stdint.h>
#include <stdio.h>

#include "Process.h"
#include "Serial.h"

#define BUSYBOX_PATH "/Userland/BusyBox/Resource/busybox"

/* process_waitpid() は WNOHANG 相当で、子が動いている間は 0 を返す。
 * 一度呼んだだけでは busybox が起きたかどうかを見ずに終了してしまい、
 * 入力も出力も拾えなくなる。終了するまで回す（FastFetch と同じ）。 */
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
    serial_write_string("[busybox] exec " BUSYBOX_PATH "\n");

    int32_t pid = process_spawn(BUSYBOX_PATH);
    if (pid < 0) {
        process_exit(1);
    }

    process_exit(wait_for_exit(pid));
}
