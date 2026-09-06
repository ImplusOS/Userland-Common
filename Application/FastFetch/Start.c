#include <stdint.h>
#include <stdio.h>

#include "Process.h"
#include "Serial.h"

#define FASTFETCH_PATH "/usr/bin/fastfetch"
#define FASTFETCH_ARGS "--pipe false"

/* process_waitpid() は WNOHANG 相当で、子が動いている間は 0 を返す。
 * 一度呼んだだけでは即座に戻ってしまうので、終了するまで回す（Doom と同じ）。 */
static int32_t wait_for_exit(int32_t pid)
{
    int32_t status = 0;
    for (;;) {
        int32_t reaped = process_waitpid(pid, &status, 0);
        if (reaped == pid) {
            break;              /* exited, status valid */
        }
        if (reaped < 0) {
            break;              /* no such child */
        }
        sleep_ms(20u);
    }
    return status;
}

void _start(void)
{
    serial_write_string("[fastfetch] exec " FASTFETCH_PATH "\n");

    int32_t pid = process_spawn_with_arg(FASTFETCH_PATH, FASTFETCH_ARGS);
    if (pid < 0) {
        serial_write_string("[fastfetch] spawn failed\n");
        process_exit(1);
    }

    int32_t status = wait_for_exit(pid);

    char msg[64];
    snprintf(msg, sizeof msg, "[fastfetch] exit status=%d\n", (int)status);
    serial_write_string(msg);
    process_exit(status);
}
