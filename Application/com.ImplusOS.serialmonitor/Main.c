/*
 * com.ImplusOS.serialmonitor -- taps the kernel serial log from the desktop.
 *
 *   [Pause] [Clear] [Save] [View: kernel]  Filter( ......... )
 *   kernel  1234 lines, 87 shown  LIVE
 *   +----------------------------------------------------------------+
 *   | [ahci] port 0 link up                                          |
 *   | [app] /usr/bin/xterm exec                                      |
 *   +----------------------------------------------------------------+
 *
 * Everything the kernel prints over COM1 also lands in an in-RAM ring
 * (Kernel/Debug/serial/Serial.c), and read_kernel_log() (Userland/API/
 * OSDebug.h, SYSCALL_READ_KERNEL_LOG) copies the tail of that ring into a
 * userland buffer. That is the whole tap: no serial cable, no device node.
 *
 * Why not /dev/kmsg, which is the same ring published as a character device
 * with a per-reader cursor? Because a *native* app cannot read it. The native
 * read syscall stages through a kernel bounce buffer
 * (Syscall_Dispatch.c: syscall_file_read_to_user) and then hands that kernel
 * pointer to the character-device hook, which copy_to_user()s into it --
 * copy_to_user rejects a kernel address, so every read of /dev/kmsg that has
 * data to return comes back OS_STATUS_FAULT. Linux-ABI programs are fine
 * because linux_read passes the user pointer straight through, which is why
 * `cat /dev/kmsg` works in xterm and this app cannot use the same node. If
 * that path is ever fixed, switching back buys a per-reader cursor and
 * blocking reads; until then the snapshot below is what works.
 *
 * The snapshot is a *window on the tail*, not a stream, so consecutive reads
 * overlap heavily. append_snapshot() finds the overlap and keeps only what is
 * new, which is what turns a tail into a feed.
 *
 * One caveat worth keeping: a window showing the whole kernel log can feed
 * back on itself if drawing it produces log traffic -- that is what happens to
 * an xterm streaming /dev/kmsg, where drawing is X traffic and X traffic is
 * logged. This app draws into the window manager's backing store and writes
 * nothing to the serial port while it runs; the only serial_write_string()
 * calls are at startup. Do not add logging to the tick path.
 *
 * The capture itself (line splitting, the ring, filtering, rendering) is in
 * LogRing.c, which has no UI or syscall dependencies so Tests/ can drive it
 * on the host.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "ImUI.h"
#include "File.h"
#include "OSDebug.h"
#include "Process.h"
#include "Serial.h"

#include "LogRing.h"

#define VIEW_KERNEL 0          /* everything the kernel logs */
#define VIEW_APPS   1          /* only the "[app] ..." launch/exit lines */

#define SNAP_BYTES      16384u /* tail window read from the kernel each poll */
#define VIEW_MAX_LINES    400u /* newest N matching lines get rendered */
#define VIEW_BYTES      (VIEW_MAX_LINES * LOG_LINE_MAX + 1u)
#define POLL_MS            120u /* tail poll; a line takes ~1 ms to arrive */
#define SAVE_PATH "/var/serial-monitor.log"

static const char *const g_view_name[2]   = { "kernel", "apps" };
static const char *const g_view_needle[2] = { NULL, "[app]" };

static imui_app_t    *g_app;
static imui_widget_t *w_view, *w_filter, *w_status, *w_pause, *w_source;

static log_ring_t g_ring;
static char       g_render[VIEW_BYTES];

/*
 * Both buffers are on the heap and written to before first use, because a
 * syscall buffer has to be mapped *before* the call: the kernel checks it with
 * process_user_buffer_is_valid() and will not fault pages in on the caller's
 * behalf. A read into a fresh stack frame or an untouched .bss page comes back
 * OS_STATUS_FAULT (-14) and the pane just stays empty with nothing to say why.
 */
static char *g_snap;           /* this poll's tail snapshot */
static char *g_prev;           /* the previous one, for the overlap */
static uint32_t g_prev_len;
static char *g_msg;            /* scratch for serial_write_string */

static int32_t  g_last_error;  /* last failed read_kernel_log, 0 = none */
static int      g_source = VIEW_KERNEL;
static bool     g_paused;
static bool     g_dirty;
static uint64_t g_last_poll;

/* ------------------------------------------------------------ capture */

static void touch_pages(char *p, uint32_t bytes)
{
    if (!p) return;
    for (uint32_t i = 0; i < bytes; i += 4096u) {
        ((volatile char *)p)[i] = 0;
    }
    ((volatile char *)p)[bytes - 1u] = 0;
}

/* Three %lld slots, always: a mismatched vararg here fails silently. */
static void log_fmt(const char *fmt, long long a, long long b, long long c)
{
    if (!g_msg) return;
    snprintf(g_msg, 128u, fmt, a, b, c);
    serial_write_string(g_msg);
}

/*
 * Feeds the part of `snap` that is not already in `g_prev`.
 *
 * The kernel hands back the newest bytes of the log, so two consecutive reads
 * are two windows onto the same stream: the tail of the older one is the head
 * of the newer one. The longest such match is the overlap, and everything
 * after it is new. No match at all means more than a window's worth arrived
 * between polls -- the gap is unavoidable and the newest bytes are the ones
 * worth keeping.
 */
static void append_snapshot(const char *snap, uint32_t len)
{
    uint32_t max = (g_prev_len < len) ? g_prev_len : len;
    uint32_t overlap = 0u;
    for (uint32_t l = max; l > 0u; --l) {
        if (memcmp(g_prev + (g_prev_len - l), snap, l) == 0) { overlap = l; break; }
    }
    if (len > overlap) {
        logring_feed(&g_ring, snap + overlap, len - overlap);
        g_dirty = true;
    }
    memcpy(g_prev, snap, len);
    g_prev_len = len;
}

static void poll_log(void)
{
    if (!g_snap || !g_prev) return;
    int32_t n = read_kernel_log(g_snap, SNAP_BYTES);
    if (n < 0) { g_last_error = n; return; }
    g_last_error = 0;
    append_snapshot(g_snap, (uint32_t)n);
}

/* ---------------------------------------------------------------- view */

static void rebuild_view(void)
{
    uint32_t shown = 0u;
    (void)logring_render(&g_ring, imui_get_text(w_filter),
                         g_view_needle[g_source], VIEW_MAX_LINES,
                         g_render, sizeof(g_render), &shown);

    imui_textarea_set(w_view, g_render);
    if (!g_paused) {
        /* Autoscroll: the paint pass keeps the caret line on screen, so
         * parking the caret at the end pins the pane to the newest line.
         * While paused the caret stays where the user scrolled it. */
        w_view->caret = w_view->buf_len;
    }

    char s[128];
    if (g_last_error != 0) {
        snprintf(s, sizeof(s), "%s  %llu lines, %u shown  read error %d",
                 g_view_name[g_source], (unsigned long long)g_ring.captured,
                 (unsigned)shown, (int)g_last_error);
    } else {
        snprintf(s, sizeof(s), "%s  %llu lines, %u shown  %s",
                 g_view_name[g_source], (unsigned long long)g_ring.captured,
                 (unsigned)shown, g_paused ? "PAUSED" : "LIVE");
    }
    imui_set_text(g_app, w_status, s);
}

/* ------------------------------------------------------------ actions */

static void on_tick(imui_app_t *a, imui_widget_t *w, void *u)
{
    (void)a; (void)w; (void)u;

    uint64_t now = get_uptime_ms();
    if (now - g_last_poll < POLL_MS) return;
    g_last_poll = now;

    /* Capture keeps running while paused -- pausing freezes the view, not the
     * tap. Stopping the tap would let the kernel ring roll past the window,
     * and the lines that fell off the back would be exactly the ones the user
     * paused to read. */
    poll_log();

    if (!g_dirty || g_paused) return;
    g_dirty = false;
    rebuild_view();
    imui_request_paint(g_app);
}

static void do_pause(imui_app_t *a, imui_widget_t *w, void *u)
{
    (void)a; (void)w; (void)u;
    g_paused = !g_paused;
    imui_set_text(g_app, w_pause, g_paused ? "Resume" : "Pause");
    rebuild_view();
    imui_request_paint(g_app);
}

static void do_clear(imui_app_t *a, imui_widget_t *w, void *u)
{
    (void)a; (void)w; (void)u;
    /* Drops what has been captured, not the kernel's ring: the next poll
     * starts from where this one left off rather than replaying the tail. */
    logring_reset(&g_ring);
    rebuild_view();
    imui_request_paint(g_app);
}

static void do_source(imui_app_t *a, imui_widget_t *w, void *u)
{
    (void)a; (void)w; (void)u;
    g_source = (g_source == VIEW_KERNEL) ? VIEW_APPS : VIEW_KERNEL;
    char label[48];
    snprintf(label, sizeof(label), "View: %s", g_view_name[g_source]);
    imui_set_text(g_app, w_source, label);
    rebuild_view();
    imui_request_paint(g_app);
}

static void do_save(imui_app_t *a, imui_widget_t *w, void *u)
{
    (void)a; (void)w; (void)u;
    if (g_snap == NULL) return;

    /* The whole capture, unfiltered: the log is the artifact, the filter is
     * only how it is being read right now. /var is tmpfs (the boot medium is
     * read-only ISO9660), so this survives until reboot, not across one. */
    int32_t fd = file_creat(SAVE_PATH);
    if (fd < 0) fd = file_open(SAVE_PATH, 1);
    if (fd < 0) { imui_set_text(g_app, w_status, "save failed: " SAVE_PATH); return; }

    uint32_t written = 0u;
    for (uint32_t i = 0; i < g_ring.count; ++i) {
        /* Formatted into the heap snapshot buffer -- file_write() copies out
         * of it and the kernel checks it the same way a read destination is
         * checked. Nothing is polling while a click handler runs. */
        int n = snprintf(g_snap, SNAP_BYTES, "%s\n", logring_line(&g_ring, i));
        if (n <= 0) continue;
        uint32_t len = (uint32_t)n, off = 0u;
        while (off < len) {
            int64_t k = file_write(fd, g_snap + off, len - off);
            if (k <= 0) break;
            off += (uint32_t)k;
        }
        if (off < len) break;
        written++;
    }
    file_close(fd);

    /* The next poll re-reads the tail into g_snap; g_prev still holds the
     * previous window, so nothing is lost by having borrowed the buffer. */
    char s[96];
    snprintf(s, sizeof(s), "saved %u lines to " SAVE_PATH, (unsigned)written);
    imui_set_text(g_app, w_status, s);
    imui_request_paint(g_app);
}

static void on_filter(imui_app_t *a, imui_widget_t *w, void *u)
{
    (void)a; (void)w; (void)u;
    rebuild_view();
    imui_request_paint(g_app);
}

/* ---------------------------------------------------------------- main */

int _start(void);
int _start(void)
{
    serial_write_string("[serialmon] start\n");

    /* Tap first, window second: the kernel ring keeps moving while the window
     * is being created, and the tail window is only 16 KiB deep. */
    logring_reset(&g_ring);
    g_snap = malloc(SNAP_BYTES);
    g_prev = malloc(SNAP_BYTES);
    g_msg  = malloc(128u);
    touch_pages(g_snap, SNAP_BYTES);
    touch_pages(g_prev, SNAP_BYTES);
    touch_pages(g_msg, 128u);
    if (!g_snap || !g_prev) {
        serial_write_string("[serialmon] out of memory\n");
        process_exit(1);
    }
    poll_log();
    /* One line, at startup only: enough to tell a broken tap from a quiet one
     * on a machine with no serial cable, and -- unlike logging from the tick
     * path -- it cannot feed back into the pane. */
    log_fmt("[serialmon] tapped %lld lines err=%lld%lld\n",
            (long long)g_ring.captured, (long long)g_last_error, 0LL);

    g_app = imui_create(900, 620, "Serial Monitor");
    if (!g_app) {
        serial_write_string("[serialmon] no window\n");
        process_exit(1);
    }

    imui_widget_t *root = g_app->root;
    root->kind = IMUI_COL; root->pad = 0; root->gap = 0;

    imui_widget_t *tb = imui_container(root, IMUI_ROW, 8, 6);
    tb->grow = 0; tb->min_h = 46; tb->fill_bg = true;
    w_pause  = imui_button(tb, "Pause", IMUI_ICON_NONE, do_pause, NULL);
    imui_button(tb, "Clear", IMUI_ICON_TRASH, do_clear, NULL);
    imui_button(tb, "Save", IMUI_ICON_SAVE, do_save, NULL);
    w_source = imui_button(tb, "View: kernel", IMUI_ICON_REFRESH, do_source, NULL);
    w_source->min_w = 140;
    imui_label(tb, "Filter");
    w_filter = imui_textbox(tb, "");
    w_filter->grow = 1;
    w_filter->on_change = on_filter;
    w_filter->on_submit = on_filter;

    imui_widget_t *bar = imui_container(root, IMUI_ROW, 8, 6);
    bar->grow = 0; bar->min_h = 26;
    w_status = imui_label(bar, "");
    w_status->grow = 1;

    imui_widget_t *body = imui_container(root, IMUI_COL, 8, 8);
    w_view = imui_textarea(body);
    w_view->grow = 1;
    w_view->editable = false;      /* read-only: arrows and PgUp/PgDn scroll */

    g_app->on_tick = on_tick;
    rebuild_view();

    int rc = imui_run(g_app);

    imui_destroy(g_app);
    serial_write_string("[serialmon] exit\n");
    return rc;
}
