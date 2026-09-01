/*
 * com.ImplusOS.filemanager -- a Windows 10 Explorer style file browser
 * built on the ImUI toolkit.
 *
 *   [<] [>] [^]  ( path .......................... )  [refresh]
 *   +----------------+--------------------------------------------+
 *   | Home           |  Name                        Kind / size   |
 *   | This PC (/)     |  ..                          Folder        |
 *   | Userland        |  Documents                   Folder        |
 *   | Var            |  notes.txt                   1.2 KB        |
 *   +----------------+--------------------------------------------+
 *   N items
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

#define PATH_MAX 512
#define HIST_MAX 64

static char        g_cwd[PATH_MAX] = "/";
static char        g_hist[HIST_MAX][PATH_MAX];
static int         g_hist_n, g_hist_i;

static imui_widget_t *w_path, *w_list, *w_status;
static imui_app_t    *g_app;

static const char *EDITOR = "/Userland/com.ImplusOS.editor/com.ImplusOS.editor.ELF";

/* ---- helpers ---- */

static bool is_text_name(const char *n) {
    const char *dot = strrchr(n, '.');
    if (!dot) return false;
    static const char *ext[] = { ".txt",".md",".c",".h",".cpp",".log",".cfg",".ini",
                                 ".sh",".json",".xml",".list",".theme",".conf",".py" };
    for (unsigned i = 0; i < sizeof(ext)/sizeof(ext[0]); ++i)
        if (strcmp(dot, ext[i]) == 0) return true;
    return false;
}

static void human_size(uint32_t bytes, char *out, size_t n) {
    if (bytes < 1024u) snprintf(out, n, "%u B", bytes);
    else if (bytes < 1024u*1024u) snprintf(out, n, "%u.%u KB", bytes/1024u, (bytes%1024u)*10u/1024u);
    else snprintf(out, n, "%u.%u MB", bytes/(1024u*1024u), (bytes%(1024u*1024u))*10u/(1024u*1024u));
}

static void join_path(const char *base, const char *name, char *out, size_t n) {
    if (strcmp(name, "..") == 0) {
        strncpy(out, base, n - 1); out[n - 1] = '\0';
        char *slash = strrchr(out, '/');
        if (slash && slash != out) *slash = '\0';
        else strcpy(out, "/");
        return;
    }
    if (base[strlen(base) - 1] == '/') snprintf(out, n, "%s%s", base, name);
    else snprintf(out, n, "%s/%s", base, name);
}

/* ---- directory load ---- */

typedef struct { char name[260]; bool dir; uint32_t size; } entry_t;

static int ent_cmp(const void *a, const void *b) {
    const entry_t *x = a, *y = b;
    if (x->dir != y->dir) return x->dir ? -1 : 1;
    return strcmp(x->name, y->name);
}

static void load_dir(const char *path) {
    imui_list_clear(w_list);

    int32_t d = file_opendir(path);
    static entry_t ents[1024];
    uint32_t n = 0;

    if (d >= 0) {
        file_dirent_t de;
        while (n < 1024 && file_readdir(d, &de) > 0) {
            if (de.name[0] == '\0') continue;
            if (strcmp(de.name, ".") == 0 || strcmp(de.name, "..") == 0) continue;
            char full[PATH_MAX];
            join_path(path, de.name, full, sizeof(full));
            file_stat_t st;
            bool dir = (de.attributes & 0x10u) != 0;
            uint32_t sz = de.size;
            if (file_stat(full, &st) >= 0 && st.exists) { dir = st.is_dir != 0; sz = st.size; }
            strncpy(ents[n].name, de.name, sizeof(ents[n].name) - 1);
            ents[n].name[sizeof(ents[n].name) - 1] = '\0';
            ents[n].dir = dir;
            ents[n].size = sz;
            n++;
        }
        file_closedir(d);
    }

    qsort(ents, n, sizeof(entry_t), ent_cmp);

    if (strcmp(path, "/") != 0)
        imui_list_add(w_list, IMUI_ICON_UP, "..", "Parent folder");

    uint32_t folders = 0, files = 0;
    for (uint32_t i = 0; i < n; ++i) {
        if (ents[i].dir) {
            imui_list_add(w_list, IMUI_ICON_FOLDER, ents[i].name, "Folder");
            folders++;
        } else {
            char sz[32]; human_size(ents[i].size, sz, sizeof(sz));
            imui_list_add(w_list, is_text_name(ents[i].name) ? IMUI_ICON_EDIT : IMUI_ICON_FILE,
                          ents[i].name, sz);
            files++;
        }
    }

    char st[96];
    snprintf(st, sizeof(st), "%u item%s  \xe2\x80\x94  %u folders, %u files",
             folders + files, (folders + files) == 1 ? "" : "s", folders, files);
    imui_set_text(g_app, w_status, st);
    imui_set_text(g_app, w_path, path);
    imui_request_paint(g_app);
}

static void navigate(const char *path, bool push) {
    char clean[PATH_MAX];
    strncpy(clean, (path && path[0]) ? path : "/", sizeof(clean) - 1);
    clean[sizeof(clean) - 1] = '\0';
    size_t l = strlen(clean);
    while (l > 1 && clean[l - 1] == '/') clean[--l] = '\0';

    file_stat_t st;
    if (strcmp(clean, "/") != 0 &&
        (file_stat(clean, &st) < 0 || !st.exists || !st.is_dir)) {
        return; /* not a directory */
    }

    if (push && strcmp(clean, g_cwd) != 0) {
        if (g_hist_n < HIST_MAX) {
            strncpy(g_hist[g_hist_n], g_cwd, PATH_MAX - 1);
            g_hist_i = ++g_hist_n;
        }
    }
    strncpy(g_cwd, clean, sizeof(g_cwd) - 1);
    g_cwd[sizeof(g_cwd) - 1] = '\0';
    load_dir(g_cwd);
}

/* ---- callbacks ---- */

static void on_back(imui_app_t *a, imui_widget_t *w, void *u) {
    (void)a; (void)w; (void)u;
    if (g_hist_i > 0) {
        g_hist_i--;
        navigate(g_hist[g_hist_i], false);
    }
}
static void on_forward(imui_app_t *a, imui_widget_t *w, void *u) {
    (void)a; (void)w; (void)u;
    if (g_hist_i < g_hist_n) {
        navigate(g_hist[g_hist_i], false);
        g_hist_i++;
    }
}
static void on_up(imui_app_t *a, imui_widget_t *w, void *u) {
    (void)a; (void)w; (void)u;
    char parent[PATH_MAX];
    join_path(g_cwd, "..", parent, sizeof(parent));
    navigate(parent, true);
}
static void on_refresh(imui_app_t *a, imui_widget_t *w, void *u) {
    (void)a; (void)w; (void)u;
    load_dir(g_cwd);
}
static void on_path_submit(imui_app_t *a, imui_widget_t *w, void *u) {
    (void)a; (void)u;
    navigate(imui_get_text(w), true);
}
static void on_list_activate(imui_app_t *a, imui_widget_t *w, void *u) {
    (void)a; (void)u;
    int sel = imui_list_selected(w);
    if (sel < 0) return;
    const char *label = w->rows[sel].text;
    char full[PATH_MAX];
    join_path(g_cwd, label, full, sizeof(full));

    file_stat_t st;
    if (strcmp(label, "..") == 0) { on_up(a, w, u); return; }
    if (file_stat(full, &st) >= 0 && st.exists && st.is_dir) {
        navigate(full, true);
    } else if (is_text_name(label)) {
        process_spawn_with_arg(EDITOR, full);
    }
}
static void on_place(imui_app_t *a, imui_widget_t *w, void *u) {
    (void)a;
    int sel = imui_list_selected(w);
    if (sel < 0) return;
    const char *paths[] = { "/", "/Userland", "/var", "/run", "/BootManager" };
    if ((unsigned)sel < sizeof(paths)/sizeof(paths[0]))
        navigate(paths[sel], true);
    (void)u;
}

/* ---- build ---- */

int _start(void);
int _start(void) {
    serial_write_string("[filemanager] start\n");
    g_app = imui_create(920, 620, "Files");
    if (!g_app) { serial_write_string("[filemanager] no window\n"); for (;;) {} }

    imui_widget_t *root = g_app->root;
    root->kind = IMUI_COL; root->pad = 0; root->gap = 0;

    /* toolbar */
    imui_widget_t *tb = imui_container(root, IMUI_ROW, 8, 6);
    tb->grow = 0; tb->min_h = 46; tb->fill_bg = true;
    imui_iconbutton(tb, IMUI_ICON_BACK, on_back, NULL);
    imui_iconbutton(tb, IMUI_ICON_FORWARD, on_forward, NULL);
    imui_iconbutton(tb, IMUI_ICON_UP, on_up, NULL);
    w_path = imui_textbox(tb, "/");
    w_path->grow = 1;
    w_path->on_submit = on_path_submit;
    imui_iconbutton(tb, IMUI_ICON_REFRESH, on_refresh, NULL);

    /* body: places | files */
    imui_widget_t *body = imui_container(root, IMUI_ROW, 8, 8);
    body->grow = 1;

    imui_widget_t *places = imui_list(body);
    places->grow = 0; places->min_w = 190;
    places->on_activate = on_place;
    imui_list_add(places, IMUI_ICON_DRIVE, "This PC", "/");
    imui_list_add(places, IMUI_ICON_FOLDER, "Userland", "/Userland");
    imui_list_add(places, IMUI_ICON_FOLDER, "Var", "/var");
    imui_list_add(places, IMUI_ICON_FOLDER, "Run", "/run");
    imui_list_add(places, IMUI_ICON_FOLDER, "BootManager", "/BootManager");

    w_list = imui_list(body);
    w_list->grow = 1;
    w_list->on_activate = on_list_activate;

    /* status bar */
    imui_widget_t *sb = imui_container(root, IMUI_ROW, 8, 0);
    sb->grow = 0; sb->min_h = 24; sb->fill_bg = true;
    w_status = imui_label(sb, "");

    navigate("/", false);
    return imui_run(g_app);
}
