/*
 * com.ImplusOS.filemanager -- a Windows 10 Explorer style file browser
 * built on the UI toolkit.
 *
 *   [<] [>] [^]  ( path .......................... )  [refresh]
 *   名前 [ ................ ] [新規フォルダー] [名前変更] [削除]
 *   +----------------+--------------------------------------------+
 *   | Home           |  Name                        Kind / size   |
 *   | This PC (/)    |  ..                          Folder        |
 *   | Userland       |  Documents                   Folder        |
 *   | Var            |  notes.txt                   1.2 KB        |
 *   +-------------------------------------------------------------+
 *   N items
 *
 * The second toolbar row is the file operations. They act on what is typed
 * in the box (create, rename-to) or on what is selected (delete), because
 * that keeps one control per idea rather than a context menu the toolkit
 * has no widget for. Deleting takes the same two-press confirmation the
 * editor uses for discarding an edit: one press warns, one within five
 * seconds goes ahead.
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

#define PATH_MAX 512
#define HIST_MAX 64
#define DISCARD_ARM_MS 5000u

/*
 * Back/forward history.
 *
 * g_hist holds the visited paths, g_hist_i is the index of where the
 * browser *is*, and g_hist_n is how many entries exist -- so "forward" is
 * simply "the next entry", which is only meaningful because navigating with
 * push=true drops everything after the current index first. (The old shape
 * kept the index one past the end and never stored the place a back had
 * just left, so forward re-loaded the directory already on screen and then
 * ran out of entries: it moved the index and went nowhere.)
 */
static char        g_cwd[PATH_MAX] = "/";
static char        g_hist[HIST_MAX][PATH_MAX];
static int         g_hist_n, g_hist_i;

static ui_widget_t *w_path, *w_list, *w_status, *w_name;
static ui_app_t    *g_app;

/* Delete's confirmation window, as in the editor. */
static uint64_t g_arm_ms;

static const char *EDITOR = "/Userland/com.ImplusOS.editor/com.ImplusOS.editor.ELF";

/* The user's home as Linux programs see it: HOME=/tmp in the environment
 * the kernel gives them (ProcessManager_Create.c), so this is where Chromium
 * saves downloads ($HOME/Downloads) and where GTK/xterm keep their files.
 * Without these two places a finished download was nowhere to be found:
 * /tmp is not under any of the other entries. */
#define HOME_DIR      "/tmp"
#define DOWNLOADS_DIR "/tmp/Downloads"

/* ---- helpers ---- */

/*
 * Extensions the editor is worth opening on. Kept in step with XdgOpen's
 * list (XdgOpen/Main.c): a file offered as text from a download has to open
 * in the editor whichever of the two routes it took to get here.
 */
static const char *const TEXT_EXT[] = {
    ".txt", ".md", ".c", ".h", ".cpp", ".cc", ".hpp", ".log", ".cfg", ".ini",
    ".sh",  ".json", ".xml", ".list", ".theme", ".conf", ".py", ".csv",
    ".js",  ".html", ".css", ".yml", ".yaml", ".desktop", ".patch",
};

/* Case-insensitive suffix match: README.TXT is text, and treating it as a
 * blob because the suffix arrived in capitals was a coin flip. */
static bool ends_with_ci(const char *text, const char *suffix) {
    size_t n = strlen(text), m = strlen(suffix);
    if (n < m) return false;
    text += n - m;
    for (size_t i = 0; i < m; ++i) {
        char a = text[i], b = suffix[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
        if (a != b) return false;
    }
    return true;
}

static bool is_text_name(const char *n) {
    if (!strchr(n, '.')) return false;
    for (unsigned i = 0; i < sizeof(TEXT_EXT) / sizeof(TEXT_EXT[0]); ++i)
        if (ends_with_ci(n, TEXT_EXT[i])) return true;
    return false;
}

static void status(const char *text) {
    ui_set_text(w_status, text);
    ui_redraw();
}

static void status_fmt(const char *fmt, const char *arg) {
    char s[256];
    snprintf(s, sizeof(s), fmt, arg);
    status(s);
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

/* Copy the selection out of the list, if there is one. Returns false when
 * nothing is selected or the row is the parent-folder link. */
static bool selected_name(char *out, size_t n) {
    int sel = w_list->sel;
    if (sel < 0 || sel >= (int)w_list->row_count) return false;
    const char *label = w_list->rows[sel].text;
    if (strcmp(label, "..") == 0) return false;
    strncpy(out, label, n - 1);
    out[n - 1] = '\0';
    return out[0] != '\0';
}

/* ---- directory load ---- */

typedef struct { char name[260]; bool dir; uint32_t size; } entry_t;

static int ent_cmp(const void *a, const void *b) {
    const entry_t *x = a, *y = b;
    if (x->dir != y->dir) return x->dir ? -1 : 1;
    return strcmp(x->name, y->name);
}

static void load_dir(const char *path) {
    ui_list_clear(w_list);

    int32_t d = file_opendir(path);
    static entry_t ents[1024];
    uint32_t n = 0;
    bool ok = d >= 0;

    if (ok) {
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
        ui_list_add(w_list, UI_ICON_UP, "..", "Parent folder");

    uint32_t folders = 0, files = 0;
    for (uint32_t i = 0; i < n; ++i) {
        if (ents[i].dir) {
            ui_list_add(w_list, UI_ICON_FOLDER, ents[i].name, "Folder");
            folders++;
        } else {
            char sz[32]; human_size(ents[i].size, sz, sizeof(sz));
            ui_list_add(w_list, is_text_name(ents[i].name) ? UI_ICON_EDIT : UI_ICON_FILE,
                          ents[i].name, sz);
            files++;
        }
    }

    char st[256];
    if (ok) {
        snprintf(st, sizeof(st), "%u item%s  \xe2\x80\x94  %u folders, %u files",
                 folders + files, (folders + files) == 1 ? "" : "s", folders, files);
    } else {
        /* An unreadable directory used to look exactly like an empty one. */
        snprintf(st, sizeof(st), "開けません: %s", path);
    }
    ui_set_text(w_status, st);
    ui_set_text(w_path, path);
    ui_redraw();
}

/* As load_dir, but keeping where the user was looking -- a refresh that
 * jumps back to the top of the list throws away the scroll position for no
 * reason, and the process manager's periodic rebuild has the same problem
 * solved the same way. */
static void load_dir_keep_view(const char *path) {
    uint32_t scroll = w_list->scroll;
    int32_t   sel   = w_list->sel;
    load_dir(path);
    w_list->scroll = scroll;
    w_list->sel = (sel >= 0 && sel < (int32_t)w_list->row_count) ? sel : -1;
}

/* Returns false when `path` is not a usable directory; the caller keeps its
 * history index in that case. */
static bool navigate(const char *path, bool push) {
    char clean[PATH_MAX];
    strncpy(clean, (path && path[0]) ? path : "/", sizeof(clean) - 1);
    clean[sizeof(clean) - 1] = '\0';
    size_t l = strlen(clean);
    while (l > 1 && clean[l - 1] == '/') clean[--l] = '\0';

    file_stat_t st;
    if (strcmp(clean, "/") != 0 &&
        (file_stat(clean, &st) < 0 || !st.exists || !st.is_dir)) {
        status_fmt("そのフォルダーはありません (%s)", clean);
        return false; /* not a directory */
    }

    if (push && strcmp(clean, g_cwd) != 0) {
        g_hist_n = g_hist_i + 1;              /* forget the forward branch */
        if (g_hist_n >= HIST_MAX) {           /* full: drop the oldest hop */
            memmove(&g_hist[0], &g_hist[1], sizeof(g_hist[0]) * (HIST_MAX - 1));
            g_hist_n = HIST_MAX - 1;
            g_hist_i = g_hist_n - 1;
        }
        strncpy(g_hist[g_hist_n], clean, PATH_MAX - 1);
        g_hist[g_hist_n][PATH_MAX - 1] = '\0';
        g_hist_i = g_hist_n;
        g_hist_n++;
    }
    strncpy(g_cwd, clean, sizeof(g_cwd) - 1);
    g_cwd[sizeof(g_cwd) - 1] = '\0';
    load_dir(g_cwd);
    return true;
}

/* ---- callbacks ---- */

static void on_back(ui_widget_t *w) {
    (void)w;
    if (g_hist_i > 0 && navigate(g_hist[g_hist_i - 1], false)) g_hist_i--;
}
static void on_forward(ui_widget_t *w) {
    (void)w;
    if (g_hist_i + 1 < g_hist_n && navigate(g_hist[g_hist_i + 1], false))
        g_hist_i++;
}
static void on_up(ui_widget_t *w) {
    (void)w;
    char parent[PATH_MAX];
    join_path(g_cwd, "..", parent, sizeof(parent));
    navigate(parent, true);
}
static void on_refresh(ui_widget_t *w) {
    (void)w;
    load_dir_keep_view(g_cwd);
}
static void on_path_submit(ui_widget_t *w) {
    navigate(ui_get_text(w), true);
}
static void on_list_activate(ui_widget_t *w) {
    int sel = w->sel;
    if (sel < 0) return;
    const char *label = w->rows[sel].text;
    char full[PATH_MAX];
    join_path(g_cwd, label, full, sizeof(full));

    file_stat_t st;
    if (strcmp(label, "..") == 0) { on_up(w); return; }
    if (file_stat(full, &st) >= 0 && st.exists && st.is_dir) {
        navigate(full, true);
    } else if (is_text_name(label)) {
        process_spawn_with_arg(EDITOR, full);
    } else {
        status_fmt("この種類のファイルは開けません (%s)", label);
    }
}
static void on_place(ui_widget_t *w) {
    int sel = w->sel;
    if (sel < 0) return;
    const char *paths[] = { HOME_DIR, DOWNLOADS_DIR, "/", "/Userland", "/var",
                            "/run", "/BootManager" };
    if ((unsigned)sel < sizeof(paths)/sizeof(paths[0])) {
        const char *p = paths[sel];
        /* Chromium creates its Downloads folder on the first download;
         * until then, show the folder anyway rather than refuse. */
        if (strcmp(p, DOWNLOADS_DIR) == 0) {
            file_stat_t st;
            if (file_stat(p, &st) < 0 || !st.exists) {
                (void)file_mkdir(p);
            }
        }
        navigate(p, true);
    }
}

/* ---- file operations ---- */

/* A name typed into the box: non-empty, and without a path separator, so it
 * can only ever name something *inside* the current folder. */
static bool typed_name(char *out, size_t n) {
    const char *raw = ui_get_text(w_name);
    while (*raw == ' ' || *raw == '\t') raw++;
    size_t len = strlen(raw);
    while (len > 0 && (raw[len - 1] == ' ' || raw[len - 1] == '\t')) len--;
    if (len == 0 || len + 1 > n) return false;
    if (len >= 2 && raw[0] == '.' && raw[1] == '.') return false;
    memcpy(out, raw, len);
    out[len] = '\0';
    return strchr(out, '/') == NULL;
}

static void on_new_folder(ui_widget_t *w) {
    (void)w;
    char name[256], full[PATH_MAX];
    if (!typed_name(name, sizeof(name))) {
        status("作る名前を入力してください（/ と .. は使えません）");
        return;
    }
    join_path(g_cwd, name, full, sizeof(full));
    file_stat_t st;
    if (file_stat(full, &st) >= 0 && st.exists) {
        status_fmt("すでに同じ名前があります (%s)", name);
        return;
    }
    if (file_mkdir(full) < 0) {
        status_fmt("作成できませんでした (%s)", name);
        return;
    }
    ui_set_text(w_name, "");
    load_dir(g_cwd);
    status_fmt("フォルダーを作成しました (%s)", name);
}

static void on_rename(ui_widget_t *w) {
    (void)w;
    char old_name[256], new_name[256];
    char old_path[PATH_MAX], new_path[PATH_MAX];
    if (!selected_name(old_name, sizeof(old_name))) {
        status("変更する項目をクリックしてから名前を入力してください");
        return;
    }
    if (!typed_name(new_name, sizeof(new_name))) {
        status("新しい名前を入力してください（/ と .. は使えません）");
        return;
    }
    if (strcmp(old_name, new_name) == 0) return;
    join_path(g_cwd, old_name, old_path, sizeof(old_path));
    join_path(g_cwd, new_name, new_path, sizeof(new_path));
    file_stat_t st;
    if (file_stat(new_path, &st) >= 0 && st.exists) {
        status_fmt("すでに同じ名前があります (%s)", new_name);
        return;
    }
    if (file_rename(old_path, new_path) < 0) {
        status_fmt("変更できませんでした (%s)", old_name);
        return;
    }
    ui_set_text(w_name, "");
    load_dir(g_cwd);
    {
        char msg[320];
        snprintf(msg, sizeof(msg), "名前を変更しました (%s \xe2\x86\x92 %s)",
                 old_name, new_name);
        status(msg);
    }
}

static void on_delete(ui_widget_t *w) {
    (void)w;
    char name[256];
    if (!selected_name(name, sizeof(name))) {
        status("削除する項目をクリックしてください");
        return;
    }
    char full[PATH_MAX];
    join_path(g_cwd, name, full, sizeof(full));
    file_stat_t st;
    if (file_stat(full, &st) < 0 || !st.exists) {
        status_fmt("見つかりません (%s)", name);
        return;
    }
    if (st.is_dir) {
        /* There is no rmdir in the File API, and unlinking a directory
         * would take its contents with it without asking. */
        status("フォルダーの削除には対応していません");
        return;
    }

    uint64_t now = get_uptime_ms();
    if (g_arm_ms == 0u || now - g_arm_ms >= DISCARD_ARM_MS) {
        g_arm_ms = now;
        status_fmt("削除しますか？ もう一度クリックすると削除します（5 秒）(%s)", name);
        return;
    }
    g_arm_ms = 0u;
    if (file_unlink(full) < 0) {
        status_fmt("削除できませんでした (%s)", name);
        return;
    }
    load_dir(g_cwd);
    status_fmt("削除しました (%s)", name);
}

/* ---- build ---- */

int _start(void);
int _start(void) {
    serial_write_string("[filemanager] start\n");
    g_app = ui_create(920, 620, "Files");
    if (!g_app) { serial_write_string("[filemanager] no window\n"); for (;;) {} }

    strcpy(g_hist[0], "/");
    g_hist_n = 1;
    g_hist_i = 0;

    ui_widget_t *root = g_app->root;

    /* toolbar: where am I, and where have I been */
    ui_widget_t *tb = ui_bar(root, 52);
    ui_button(tb, NULL, UI_ICON_BACK, on_back, NULL);
    ui_button(tb, NULL, UI_ICON_FORWARD, on_forward, NULL);
    ui_button(tb, NULL, UI_ICON_UP, on_up, NULL);
    w_path = ui_textbox(tb, "/");
    w_path->grow = 1;
    w_path->on_action = on_path_submit;
    ui_button(tb, NULL, UI_ICON_REFRESH, on_refresh, NULL);

    /* toolbar: what to do with a name or a selection */
    ui_widget_t *op = ui_bar(root, 52);
    ui_caption(op, "名前");
    w_name = ui_textbox(op, "");
    w_name->grow = 1;
    /* Enter is deliberately bound to nothing: folders cannot be deleted
     * yet, so creating one by accident would be permanent. */
    ui_button(op, "新規フォルダー", UI_ICON_NEW, on_new_folder, NULL);
    ui_button(op, "名前変更", UI_ICON_EDIT, on_rename, NULL);
    ui_button(op, "削除", UI_ICON_TRASH, on_delete, NULL);

    /* body: places | files */
    ui_widget_t *body = ui_row(root, 8, 8);
    body->grow = 1;

    ui_widget_t *places = ui_list(body);
    places->grow = 0; places->min_w = 190;
    places->on_action = on_place;
    /* A sidebar is a set of destinations, not a list to open: one click. */
    places->single_click = true;
    ui_list_add(places, UI_ICON_FOLDER, "Home", HOME_DIR);
    ui_list_add(places, UI_ICON_FOLDER, "Downloads", DOWNLOADS_DIR);
    ui_list_add(places, UI_ICON_DRIVE, "This PC", "/");
    ui_list_add(places, UI_ICON_FOLDER, "Userland", "/Userland");
    ui_list_add(places, UI_ICON_FOLDER, "Var", "/var");
    ui_list_add(places, UI_ICON_FOLDER, "Run", "/run");
    ui_list_add(places, UI_ICON_FOLDER, "BootManager", "/BootManager");

    w_list = ui_list(body);
    w_list->grow = 1;
    w_list->on_action = on_list_activate;

    /* status bar */
    ui_widget_t *sb = ui_bar(root, 28);
    sb->pad = 8; sb->gap = 0;
    w_status = ui_caption(sb, "");
    w_status->grow = 1;

    /* Started with a folder (xdg-open, "Show in folder"): open on it. */
    {
        char start[PATH_MAX];
        file_stat_t st;
        if (process_get_launch_argument(start, sizeof(start)) > 0 &&
            start[0] == '/' && file_stat(start, &st) >= 0 && st.exists &&
            st.is_dir) {
            navigate(start, false);
        } else {
            navigate("/", false);
        }
    }
    ui_run();
}
