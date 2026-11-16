/*
 * com.ImplusOS.editor -- a plain text editor built on the UI toolkit.
 *
 *   [New] [Open] [Save]   ( /path/to/file .................... )   1234 chars
 *   +----------------------------------------------------------------+
 *   | text area (multi-line, caret, arrow keys, scroll)             |
 *   +----------------------------------------------------------------+
 *   Ctrl+S saved · /tmp/notes.txt                                   |
 *
 * Started standalone or as `editor <path>` (the file manager opens text
 * files this way).
 *
 * Two things this file is careful about, both learned the hard way:
 *
 *   - Saving has to *trim* the file. The open flags carry no truncate bit,
 *     so writing 40 bytes over a file that was 900 leaves bytes 40..899
 *     behind, and a file that reads back longer than what was typed is the
 *     worst kind of bug for an editor. file_truncate() closes the gap after
 *     the write.
 *
 *   - Discarding has to be asked for. New/Open throw the buffer away, so
 *     they arm a five-second confirmation while the buffer is dirty instead
 *     of doing it on the first press -- there is no modal dialog to sit
 *     behind, and losing an edit on one click would be unforgivable.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "UI.h"
#include "File.h"
#include "Process.h"
#include "Serial.h"

#define MAX_FILE (1024u * 1024u)
#define DISCARD_ARM_MS 5000u

static ui_app_t    *g_app;
static ui_widget_t *w_name, *w_text, *w_info, *w_status;
static char           g_dirty;

/* The one-shot "this throws work away" window: set when a destructive
 * action warns, accepted if the same action comes again inside the window. */
static uint64_t g_arm_ms;

static void update_info(void) {
    const char *t = ui_get_text(w_text);
    uint32_t chars = (uint32_t)strlen(t), lines = 1;
    for (const char *p = t; *p; ++p) if (*p == '\n') lines++;
    char s[64];
    snprintf(s, sizeof(s), "%s%u chars · %u lines", g_dirty ? "* " : "", chars, lines);
    ui_set_text(w_info, s);
}

/* The status line is the only place with room for a sentence: the toolbar
 * label next to it is deliberately just the counts. */
static void note(const char *text) {
    ui_set_text(w_status, text);
    ui_redraw();
}

static void note_fmt(const char *fmt, const char *arg) {
    char s[256];
    snprintf(s, sizeof(s), fmt, arg);
    note(s);
}

/* Returns true when the caller may go ahead and throw the buffer away. */
static bool arm_or_pass(const char *what) {
    if (!g_dirty) return true;
    uint64_t now = get_uptime_ms();
    if (g_arm_ms != 0u && now - g_arm_ms < DISCARD_ARM_MS) {
        g_arm_ms = 0u;
        return true;
    }
    g_arm_ms = now;
    char s[192];
    snprintf(s, sizeof(s),
             "未保存の変更があります。もう一度 %s を押すと破棄します（5 秒）", what);
    note(s);
    return false;
}

static void do_open(ui_widget_t *w) {
    (void)w;
    const char *path = ui_get_text(w_name);
    if (!path || !path[0]) { note("開くファイル名を入力してください"); return; }

    file_stat_t st;
    if (file_stat(path, &st) < 0 || !st.exists) {
        note_fmt("開けません: ファイルがありません (%s)", path);
        return;
    }
    if (st.is_dir) { note_fmt("開けません: ディレクトリーです (%s)", path); return; }
    if (st.size > MAX_FILE) { note("開けません:1 MiB を超えるファイルは対象外です"); return; }
    /* Everything above can be refused without touching the buffer, so the
     * discard warning is only armed once the open is really going ahead. */
    if (!arm_or_pass("Open")) return;

    int32_t fd = file_open(path, 0);
    if (fd < 0) { note_fmt("開けません (%s)", path); return; }
    char *buf = malloc((size_t)st.size + 1u);
    if (!buf) { file_close(fd); note("メモリが足りません"); return; }
    uint32_t got = 0;
    while (got < st.size) {
        int64_t n = file_read(fd, buf + got, st.size - got);
        if (n <= 0) break;
        got += (uint32_t)n;
    }
    file_close(fd);
    buf[got] = '\0';
    ui_set_text(w_text, buf);
    free(buf);
    g_dirty = 0;
    g_arm_ms = 0u;
    update_info();
    note_fmt("開きました (%s)", path);
    ui_redraw();
}

static void do_save(ui_widget_t *w) {
    (void)w;
    const char *path = ui_get_text(w_name);
    if (!path || !path[0]) { note("先にファイル名を決めてください"); return; }

    int32_t fd = file_creat(path);
    if (fd < 0) fd = file_open(path, 1);
    if (fd < 0) { note_fmt("保存できません (%s)", path); return; }

    const char *t = ui_get_text(w_text);
    uint32_t len = (uint32_t)strlen(t), off = 0;
    while (off < len) {
        int64_t n = file_write(fd, t + off, len - off);
        if (n <= 0) break;
        off += (uint32_t)n;
    }
    /* Everything past `off` is the previous contents showing through: cut it
     * off while the file is still open, or a shorter save reads back with
     * the tail of the longer text it replaced. */
    int32_t trimmed = file_truncate(fd, off);
    file_close(fd);

    if (off < len) { note_fmt("保存できませんでした（書き込みが途中で止まりました: %s）", path); return; }

    g_dirty = 0;
    g_arm_ms = 0u;
    update_info();
    if (trimmed < 0)
        note_fmt("保存しましたが、古いファイル末尾の切り詰めに失敗しました (%s)", path);
    else
        note_fmt("保存しました (%s)", path);
    ui_redraw();
}

static void do_new(ui_widget_t *w) {
    (void)w;
    if (!arm_or_pass("New")) return;
    ui_set_text(w_text, "");
    ui_set_text(w_name, "");
    g_dirty = 0;
    g_arm_ms = 0u;
    update_info();
    note("新規の空ファイル");
    ui_redraw();
}

static void on_edit(ui_widget_t *w) {
    (void)w;
    g_dirty = 1;
    update_info();
}

/* Shortcuts belong to the app, not to whichever widget happens to have the
 * focus -- Ctrl+S has to save while the caret sits in the text *and* while
 * it sits in the file-name box. */
static bool on_key(const input_keyboard_event_t *ev) {
    if ((ev->modifiers & INPUT_KBD_MOD_CTRL) == 0u) return false;
    switch (ev->ascii) {
    case 's': case 'S': do_save(NULL); return true;
    case 'o': case 'O': do_open(NULL); return true;
    case 'n': case 'N': do_new(NULL);  return true;
    default: break;
    }
    return false;
}

int _start(void);
int _start(void) {
    serial_write_string("[editor] start\n");
    g_app = ui_create(860, 600, "Editor");
    if (!g_app) { serial_write_string("[editor] no window\n"); for (;;) {} }

    ui_widget_t *root = g_app->root;

    ui_widget_t *tb = ui_bar(root, 52);
    ui_button(tb, "New", UI_ICON_NEW, do_new, NULL);
    ui_button(tb, "Open", UI_ICON_OPEN, do_open, NULL);
    /* Saving is what this window is for, so it is the one button that
     * carries M3's tonal emphasis. */
    ui_button(tb, "Save", UI_ICON_SAVE, do_save, NULL);
    w_name = ui_textbox(tb, "");
    w_name->grow = 1;
    w_name->on_action = do_open;
    w_info = ui_caption(tb, "0 chars · 1 lines");
    w_info->min_w = 150;

    ui_widget_t *body = ui_col(root, 8, 0);
    body->grow = 1;
    w_text = ui_textarea(body);
    w_text->on_change = on_edit;

    ui_widget_t *sb = ui_bar(root, 28);
    sb->pad = 8; sb->gap = 0;
    w_status = ui_caption(sb, "Ctrl+S: 保存  Ctrl+O: 開く  Ctrl+N: 新規");
    w_status->grow = 1;

    g_app->on_key = on_key;
    ui_focus(w_text);       /* typing goes straight into the text */

    /* opened as `editor <path>` ? */
    char arg[512] = {0};
    if (process_get_launch_argument(arg, sizeof(arg)) > 0 && arg[0]) {
        ui_set_text(w_name, arg);
        do_open(NULL);
    }
    update_info();

    ui_run();
}
