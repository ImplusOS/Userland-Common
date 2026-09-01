/*
 * com.ImplusOS.editor -- a plain text editor built on the ImUI toolkit.
 *
 *   [New] [Open] [Save]   ( /path/to/file .................... )   1234 chars
 *   +----------------------------------------------------------------+
 *   | text area (multi-line, caret, arrow keys, scroll)             |
 *   +----------------------------------------------------------------+
 *
 * Started standalone or as `editor <path>` (the file manager opens text
 * files this way).
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "ImUI.h"
#include "File.h"
#include "Process.h"
#include "Serial.h"

#define MAX_FILE (1024u * 1024u)

static imui_app_t    *g_app;
static imui_widget_t *w_name, *w_text, *w_info;
static char           g_dirty;

static void update_info(void) {
    const char *t = imui_textarea_get(w_text);
    uint32_t chars = (uint32_t)strlen(t), lines = 1;
    for (const char *p = t; *p; ++p) if (*p == '\n') lines++;
    char s[64];
    snprintf(s, sizeof(s), "%s%u chars, %u lines", g_dirty ? "* " : "", chars, lines);
    imui_set_text(g_app, w_info, s);
}

static void do_open(imui_app_t *a, imui_widget_t *w, void *u) {
    (void)a; (void)w; (void)u;
    const char *path = imui_get_text(w_name);
    if (!path || !path[0]) return;
    file_stat_t st;
    if (file_stat(path, &st) < 0 || !st.exists || st.is_dir || st.size > MAX_FILE) {
        imui_set_text(g_app, w_info, "open failed");
        return;
    }
    int32_t fd = file_open(path, 0);
    if (fd < 0) { imui_set_text(g_app, w_info, "open failed"); return; }
    char *buf = malloc(st.size + 1);
    if (!buf) { file_close(fd); return; }
    uint32_t got = 0;
    while (got < st.size) {
        int64_t n = file_read(fd, buf + got, st.size - got);
        if (n <= 0) break;
        got += (uint32_t)n;
    }
    file_close(fd);
    buf[got] = '\0';
    imui_textarea_set(w_text, buf);
    free(buf);
    g_dirty = 0;
    update_info();
    imui_request_paint(g_app);
}

static void do_save(imui_app_t *a, imui_widget_t *w, void *u) {
    (void)a; (void)w; (void)u;
    const char *path = imui_get_text(w_name);
    if (!path || !path[0]) { imui_set_text(g_app, w_info, "set a filename first"); return; }
    int32_t fd = file_creat(path);
    if (fd < 0) fd = file_open(path, 1);
    if (fd < 0) { imui_set_text(g_app, w_info, "save failed"); return; }
    const char *t = imui_textarea_get(w_text);
    uint32_t len = (uint32_t)strlen(t), off = 0;
    while (off < len) {
        int64_t n = file_write(fd, t + off, len - off);
        if (n <= 0) break;
        off += (uint32_t)n;
    }
    file_close(fd);
    g_dirty = 0;
    update_info();
    imui_set_text(g_app, w_info, "saved");
}

static void do_new(imui_app_t *a, imui_widget_t *w, void *u) {
    (void)a; (void)w; (void)u;
    imui_textarea_set(w_text, "");
    imui_set_text(g_app, w_name, "");
    g_dirty = 0;
    update_info();
    imui_request_paint(g_app);
}

static void on_edit(imui_app_t *a, imui_widget_t *w, void *u) {
    (void)a; (void)w; (void)u;
    g_dirty = 1;
    update_info();
}

int _start(void);
int _start(void) {
    serial_write_string("[editor] start\n");
    g_app = imui_create(860, 600, "Editor");
    if (!g_app) { serial_write_string("[editor] no window\n"); for (;;) {} }

    imui_widget_t *root = g_app->root;
    root->kind = IMUI_COL; root->pad = 0; root->gap = 0;

    imui_widget_t *tb = imui_container(root, IMUI_ROW, 8, 6);
    tb->grow = 0; tb->min_h = 46; tb->fill_bg = true;
    imui_button(tb, "New", IMUI_ICON_NEW, do_new, NULL);
    imui_button(tb, "Open", IMUI_ICON_OPEN, do_open, NULL);
    imui_button(tb, "Save", IMUI_ICON_SAVE, do_save, NULL);
    w_name = imui_textbox(tb, "");
    w_name->grow = 1;
    w_name->on_submit = do_open;
    w_info = imui_label(tb, "0 chars, 1 lines");
    w_info->min_w = 130;

    imui_widget_t *body = imui_container(root, IMUI_COL, 8, 0);
    body->grow = 1;
    w_text = imui_textarea(body);
    w_text->on_change = on_edit;

    /* opened as `editor <path>` ? */
    char arg[512] = {0};
    if (process_get_launch_argument(arg, sizeof(arg)) > 0 && arg[0]) {
        imui_set_text(g_app, w_name, arg);
        do_open(g_app, NULL, NULL);
    }
    update_info();

    return imui_run(g_app);
}
