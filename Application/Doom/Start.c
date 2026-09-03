#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#include "Process.h"
#include "Window.h"
#include "Graphics.h"
#include "Socket.h"
#include "Serial.h"

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
 *
 * このランチャは X を「ウィンドウの中で」動かす。X は本来 /dev/dri/card0 へ
 * スキャンアウトしてパネルを占有するが、それはウィンドウマネージャが持って
 * いる同じフレームバッファなので、両方を同時に動かすと後に present した方が
 * 勝ち、二つの無関係なデスクトップが交互に表示される（＝WM が起動していない
 * ように見える／黒画面）。そこで自前で普通の WM ウィンドウを作り、その
 * バッキングストアを KMS のスキャンアウト先として登録する（display_kms_set_mirror）。
 * 以後 X のフレームはそのサーフェスに入り、WM が他のアプリと同様に合成する。
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
/* No "-verbose 3 -logverbose 3": every X log line is written twice (once for
 * stderr, once for -logfile) and COM1 is driven a character at a time from
 * inside the syscall path, so raising the level from the default cost several
 * seconds of pure serial time before the first frame. Raise it again for a
 * bring-up boot, together with OS_CONFIG_FOREIGN_TRACE. */
#define XORG_ARGS  ":0 -config /etc/X11/xorg.conf -nolisten tcp -novtswitch " \
                   "-keeptty +iglx -dumbSched -ac " \
                   "-logfile /dev/tty"

#define DOOM_PATH  "/Userland/Doom/Resource/linuxxdoom-x86_64"

/* The socket Xorg binds once it is ready to accept clients. */
#define X_SOCKET_PATH "/tmp/.X11-unix/X0"

/* Upper bound on the wait for Xorg to reach its accept loop. This is a
 * timeout, not a delay: the wait ends as soon as the socket is listening.
 * The old code slept a fixed 60 s here because the true figure is wildly
 * load-dependent (Xorg dynamic-links ~40 shared objects and runs xkbcomp
 * twice), and 60 s was then paid in full on every boot. */
#define XORG_READY_TIMEOUT_MS  120000u
#define XORG_POLL_INTERVAL_MS  50u

/* Size of the window Xorg renders into. The KMS shim advertises this as the
 * connector's only mode, so X comes up at exactly this size. Kept below the
 * 1280x800 panel so the window, its decorations and the taskbar all fit. */
#define X_WINDOW_W 1024u
#define X_WINDOW_H 640u

/* DIAGNOSTIC (TODO_Doom_Xorg_MethodA.md M6): run only Xorg and wait on it, so
 * the serial console is uncontended while a server-side failure is chased. */
#define DOOM_DIAGNOSTIC_XORG_ONLY 0

/* Wait until `pid` is a reaped zombie. The native process_waitpid() never
 * blocks: it reports 0 while the child still runs (WNOHANG semantics) and
 * only returns the pid once the child has exited. A single call therefore
 * returns immediately, which is what used to tear Xorg down the instant Doom
 * started -- "Couldn't connect to display!". */
static int32_t wait_for_exit(int32_t pid, window_id_t win,
                             uint32_t win_w, uint32_t win_h, int mirrored)
{
    int32_t status = 0;
    for (;;) {
        int32_t reaped = process_waitpid(pid, &status, 0);
        if (reaped == pid) break;   /* exited, status valid */
        if (reaped < 0) break;      /* no such child */
        /* Repaint only when the redirected server actually produced a frame;
         * polling the flag is far cheaper than damaging the whole window at a
         * fixed rate, and X is idle most of the time during startup. */
        if (mirrored && display_kms_mirror_take_dirty()) {
            window_damage(win, 0u, 0u, win_w, win_h);
        }
        sleep_ms(mirrored ? 16u : 200u);
    }
    return status;
}

void _start(void)
{
    /* Host X inside a window when there is a compositor to host it. Without
     * one (early bring-up, or a WM that failed to start) fall back to letting
     * X own the panel, which is the only way anything is visible at all. */
    window_id_t win = 0u;
    uint32_t win_w = 0u, win_h = 0u;
    int mirrored = 0;

    if (window_get_wm_pid() >= 0) {
        win = window_create(X_WINDOW_W, X_WINDOW_H, "Doom (X11)");
        if (win != 0u) {
            uint32_t *pixels = window_get_backing_store(win, &win_w, &win_h);
            if (pixels != NULL && win_w != 0u && win_h != 0u &&
                display_kms_set_mirror(pixels, win_w, win_h) == 0) {
                mirrored = 1;
                /* The backing store starts fully transparent, and X only ever
                 * writes RGB -- it never sets an alpha byte -- so without
                 * this the compositor blends every frame against the
                 * wallpaper and the window reads as an empty pane. */
                (void)window_set_surface_opaque(win, true);
                window_show(win);
                window_raise(win);
                serial_write_string("[doom] X redirected into a window\n");
            } else {
                /* Leave the window up: it is where any error text goes, and
                 * destroying it would flash the desktop. */
                serial_write_string("[doom] window mirror unavailable, "
                                    "X will scan out to the panel\n");
            }
        }
    }

    int32_t xpid = process_spawn_with_arg(XORG_PATH, XORG_ARGS);

    /* Wait for the accept loop rather than for a guessed interval. The
     * elapsed figure is logged because it is the number to watch when
     * changing anything that touches foreign-process startup -- it is where
     * essentially all of the pre-first-frame time goes. */
    if (xpid > 0) {
        uint64_t t0 = get_uptime_ms();
        uint32_t waited = 0u;
        int ready = 0;
        while (waited < XORG_READY_TIMEOUT_MS) {
            if (unix_socket_is_listening(X_SOCKET_PATH) > 0) { ready = 1; break; }
            sleep_ms(XORG_POLL_INTERVAL_MS);
            waited += XORG_POLL_INTERVAL_MS;
        }
        char msg[96];
        snprintf(msg, sizeof msg, "[doom] Xorg %s after %lu ms\n",
                 ready ? "ready" : "TIMED OUT",
                 (unsigned long)(get_uptime_ms() - t0));
        serial_write_string(msg);
    }

#if DOOM_DIAGNOSTIC_XORG_ONLY
    if (xpid > 0) {
        (void)wait_for_exit(xpid, win, win_w, win_h, mirrored);
    }
    (void)display_kms_set_mirror(0, 0u, 0u);
    process_exit(0);
#else
    int32_t pid = process_spawn(DOOM_PATH);
    if (pid < 0) {
        if (xpid > 0) {
            (void)process_kill(xpid);
        }
        (void)display_kms_set_mirror(0, 0u, 0u);
        process_exit(1);
    }

    int32_t status = wait_for_exit(pid, win, win_w, win_h, mirrored);

    if (xpid > 0) {
        (void)process_kill(xpid);
    }
    /* Hand the panel back before leaving, or the next flip from a lingering
     * server would write into a surface this process no longer owns. */
    (void)display_kms_set_mirror(0, 0u, 0u);
    if (win != 0u) {
        window_destroy(win);
    }
    process_exit(status);
#endif
}
