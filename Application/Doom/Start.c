#include <stdint.h>
#include <stdio.h>

#include "Process.h"

/*
 * Doom ランチャ（方法A: TODO_Doom_Xorg_MethodA.md）。
 *
 * linuxxdoom-x86_64 は無改変の Linux ELF で、X11 + GLX クライアントとして動く。
 * ImplusOS には X サーバが無いので、まず Debian trixie の Xorg（modesetting DDX
 * 内蔵・無改変）を起動し、それがカーネルの KMS shim（/dev/dri/card0）へスキャン
 * アウトする。Xorg / Mesa DRI / xkb / フォント / xorg.conf は Vendor/LinuxRuntime
 * が OS イメージへステージ済み（/usr/bin/Xorg, /usr/lib/xorg/modules, /etc/X11 …）。
 *
 * 環境変数は process_spawn では渡せないので、外来 ELF 用の envp は
 * カーネル側（ProcessManager_Create.c の glibc_envp）が組み立てる:
 *   - DISPLAY=:0 … 必須。Xlib は DISPLAY 未設定だと :0 にフォールバックせず、
 *     XOpenDisplay(NULL) がそのまま失敗する（"Couldn't connect to display!"）。
 *   - DOOMWADDIR … Doom は未設定だと "." を見る。カーネルは Linux ELF の cwd を
 *     実行体のあるディレクトリに設定するので、どちらでも Resource/doom1.wad が拾われる。
 */
#define XORG_PATH  "/usr/bin/Xorg"
/* -logfile /dev/tty routes the whole X log to COM1 (DevFS /dev/tty -> serial)
 * so bring-up failures are visible without a way to read files back. */
/* "+iglx": indirect GLX. linuxxdoom is a GLX client (glXCreateContext /
 * glXMakeCurrent / glXSwapBuffers / glewInit are all UND in the binary) and
 * there is no direct-rendering path here, so the server has to provide GL
 * itself. GLX used to be disabled with "-extension GLX" because
 * GlxExtensionInit() died on a near-NULL dereference -- but that was during
 * the period when brk() and mmap() shared an allocator and shared objects
 * were being mapped over malloc'd memory (TODO_Doom_Xorg_MethodA.md M20), so
 * the old diagnosis was never sound. Re-enabled now that the corruption is
 * fixed. */
/* "-ac": no host-based access control. There is no .Xauthority here and no
 * xhost to run, so a client arrives with no credentials of any kind.
 *
 * (The fd-range half of this problem is fixed in the kernel, not here:
 * xserver's AllocNewConnection() drops any client whose fd is >= lastfdesc,
 * and lastfdesc is pinned to the compile-time MAXCLIENTS of 256 -- "-maxclients"
 * does not move it, which was measured. AF_UNIX fds were therefore moved below
 * 256; see OS_CONFIG_FILE_MAX_FD and TODO_Doom_Xorg_MethodA.md M22.) */
/* "-dumbSched": run input on the main loop instead of X's separate input
 * thread (and drop the SIGALRM smart scheduler with it). X's InputThread died
 * on a #GP the moment it started -- "[OS] [#GP] ... name=InputThread rip=
 * 0x6E672D78756E696C" -- and taking the server down with it. The rip there is
 * ASCII ("linux-gn"), so the thread returned into string data: a kernel
 * threading bug that is tracked separately (TODO_Doom_Xorg_MethodA.md M18).
 * -dumbSched is a supported X option for exactly this situation, not a
 * workaround for something X does wrong; remove it once threads are sound. */
#define XORG_ARGS  ":0 -config /etc/X11/xorg.conf -nolisten tcp -novtswitch " \
                   "-keeptty +iglx -dumbSched -ac " \
                   "-logfile /dev/tty -verbose 3 -logverbose 3"

#define DOOM_PATH  "/Userland/Doom/Resource/linuxxdoom-x86_64"

/* Xorg が /tmp/.X11-unix/X0 を bind し accept を回し始めるまでの猶予。
 * Xlib 側も接続を数回リトライするため、厳密なハンドシェイクは省く。 */
/* Doom is launched this long after the launcher starts, i.e. after userland
 * init has brought the launcher up. Xorg needs the head of this window to
 * reach its accept loop before linuxxdoom calls XOpenDisplay(":0"). */
#define XORG_SETTLE_MS  15000u

/* DIAGNOSTIC (TODO_Doom_Xorg_MethodA.md M6): X dies at "Initializing extension
 * GLX" with "libglx.so: undefined symbol: <name>" and the name is raced off the
 * shared COM1 by linuxxdoom's startup banner every time. For this bring-up pass
 * run ONLY Xorg and wait on it, so the serial console is uncontended and the
 * full symbol name (plus any LD_WARN lines) is legible. Restore the Doom spawn
 * once X reaches its screen. */
/* Back to 0: with LD_BIND_NOW=1 + LD_WARN=1 any libglx.so symbol miss is now
 * reported by name at *load* time (~t+1s), well before this launcher's
 * XORG_SETTLE_MS elapses, so linuxxdoom's banner no longer races it. If X
 * reaches its screen, Doom then runs for real. */
#define DOOM_DIAGNOSTIC_XORG_ONLY 0

void _start(void)
{
    int32_t xpid = process_spawn_with_arg(XORG_PATH, XORG_ARGS);

#if DOOM_DIAGNOSTIC_XORG_ONLY
    if (xpid > 0) {
        int32_t st = 0;
        (void)process_waitpid(xpid, &st, 0);
    }
    process_exit(0);
#else
    if (xpid < 0) {
        /* Xorg が起動できない環境（KMS shim 未接続など）。Doom 単体でも
         * ld.so 依存解決までは進むので、そのまま試す。 */
        (void)0;
    } else {
        sleep_ms(XORG_SETTLE_MS);
    }

    int32_t pid = process_spawn(DOOM_PATH);
    if (pid < 0) {
        if (xpid > 0) {
            (void)process_kill(xpid);
        }
        process_exit(1);
    }

    int32_t status = 0;
    (void)process_waitpid(pid, &status, 0);

    if (xpid > 0) {
        (void)process_kill(xpid);
    }
    process_exit(status);
#endif
}
