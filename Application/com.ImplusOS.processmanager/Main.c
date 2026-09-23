/*
 * ImplusOS process manager.
 *
 * What is running, what it is costing, and a way to stop it. The kernel
 * already keeps per-process accounting (Process.h: process_perf_info_t), so
 * this is a view onto that rather than a sampler of its own -- with one
 * exception: CPU share has to be a rate, and the kernel reports cumulative
 * runtime, so the difference between two polls is taken here.
 *
 * Drawn with ImUI, which means Material Design 3 and the desktop's own
 * colour scheme without this file naming a colour.
 *
 * Two details that decide whether the window is usable:
 *
 *   - The rebuild every second must not move the user. imui_list_clear()
 *     resets the scroll offset and the selection along with the rows, so
 *     both are saved and put back -- otherwise the list snapped to the top
 *     once a second and any row past the first screenful could not be read.
 *     The selection is remembered by pid rather than by row, because rows
 *     move when the order does.
 *
 *   - Ending a process is one press too easy. It takes the same two-press
 *     window the editor uses for discarding an edit: the button says what
 *     the next press will do, for five seconds, and then goes back.
 */

#include "ImUI.h"
#include "Process.h"
#include "SystemInfo.h"
#include "Serial.h"
#include "Window.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The kernel's process table is bounded (OS_CONFIG_PROCESS_MAX_COUNT), and
 * pids index it, so enumerating is a scan rather than a cursor. */
#define PID_SCAN_MAX      512
#define MAX_TRACKED       128u
#define MAX_ROWS          (PID_SCAN_MAX + 1)
#define MAX_VIEW          (PID_SCAN_MAX + 1)
#define REFRESH_MS        1000u
#define CONFIRM_ARM_MS    5000u

typedef struct {
    int32_t  pid;
    uint64_t runtime_ns;      /* last sample, for the CPU delta */
    bool     seen;
} sample_t;

/* One scanned process, held until the order is decided. */
typedef struct {
    int32_t  pid;
    int32_t  parent;
    uint8_t  state;
    uint32_t permille;        /* CPU share since the previous poll */
    uint64_t memory;
    char     name[64];
} row_t;

typedef enum {
    SORT_PID = 0,
    SORT_CPU,
    SORT_MEMORY,
    SORT_NAME,
    SORT_COUNT
} sort_t;

static imui_app_t   *g_app;
static imui_widget_t *w_list;
static imui_widget_t *w_status;
static imui_widget_t *w_filter;
static imui_widget_t *w_end;
static imui_widget_t *w_sort[SORT_COUNT];

static sample_t  g_prev[MAX_TRACKED];
static uint32_t  g_prev_count;
static uint64_t  g_prev_ms;
static int32_t   g_selected_pid = -1;
static char      g_filter[64];
static sort_t    g_sort = SORT_PID;
static uint64_t  g_arm_ms;          /* "really end it?" window */

static row_t     g_rows[MAX_ROWS];  /* static: a frame's worth of pids is 45 KiB */
static uint32_t  g_view[MAX_VIEW];  /* pids, in the order the rows appear */

static const char *const k_sort_name[SORT_COUNT] = { "PID", "CPU", "メモリ", "名前" };

/* ------------------------------------------------------------- helpers */

/* Mirrors PROCESS_STATE_* in Kernel/Core/process/ProcessScheduler.h. */
static const char *state_name(uint8_t state)
{
    static const char *const names[] = {
        [0] = "未使用",    /* UNUSED  */
        [1] = "実行可能",  /* READY   */
        [2] = "実行中",    /* RUNNING */
        [3] = "終了",      /* DEAD    */
        [4] = "起動中",    /* INIT    */
        [5] = "ゾンビ",    /* ZOMBIE  */
        [6] = "待機中",    /* BLOCKED */
    };
    return state < sizeof(names) / sizeof(names[0]) && names[state]
        ? names[state] : "不明";
}

static void format_bytes(uint64_t bytes, char *out, size_t cap)
{
    if (bytes >= 1024ull * 1024ull * 1024ull)
        snprintf(out, cap, "%llu.%llu GB",
                 (unsigned long long)(bytes >> 30),
                 (unsigned long long)((bytes >> 20) % 1024ull) / 103ull);
    else if (bytes >= 1024ull * 1024ull)
        snprintf(out, cap, "%llu MB", (unsigned long long)(bytes >> 20));
    else if (bytes >= 1024ull)
        snprintf(out, cap, "%llu KB", (unsigned long long)(bytes >> 10));
    else
        snprintf(out, cap, "%llu B", (unsigned long long)bytes);
}

static bool matches_filter(const char *name, int32_t pid)
{
    if (!g_filter[0]) return true;
    char pid_text[16];
    snprintf(pid_text, sizeof(pid_text), "%d", pid);
    if (strstr(pid_text, g_filter)) return true;
    for (const char *h = name; *h; ++h) {
        const char *n = g_filter, *p = h;
        while (*n && *p) {
            char a = *p >= 'A' && *p <= 'Z' ? (char)(*p + 32) : *p;
            char b = *n >= 'A' && *n <= 'Z' ? (char)(*n + 32) : *n;
            if (a != b) break;
            ++n; ++p;
        }
        if (!*n) return true;
    }
    return false;
}

/* CPU share since the previous poll, in tenths of a percent. Cumulative
 * runtime minus what it was, over the wall time that passed. */
static uint32_t cpu_permille(int32_t pid, uint64_t runtime_ns, uint64_t elapsed_ms)
{
    uint64_t previous = 0;
    bool known = false;
    for (uint32_t i = 0; i < g_prev_count; ++i) {
        if (g_prev[i].pid != pid) continue;
        previous = g_prev[i].runtime_ns;
        known = true;
        break;
    }
    if (!known || elapsed_ms == 0u || runtime_ns < previous) return 0u;
    uint64_t delta_ns = runtime_ns - previous;
    uint64_t window_ns = elapsed_ms * 1000000ull;
    uint64_t permille = (delta_ns * 1000ull) / window_ns;
    return permille > 1000u ? 1000u : (uint32_t)permille;
}

static int row_cmp(const void *a, const void *b)
{
    const row_t *x = a, *y = b;
    switch (g_sort) {
    case SORT_CPU:
        if (x->permille != y->permille)
            return x->permille < y->permille ? 1 : -1;
        break;
    case SORT_MEMORY:
        if (x->memory != y->memory)
            return x->memory < y->memory ? 1 : -1;
        break;
    case SORT_NAME:
        return strcmp(x->name, y->name);
    case SORT_PID:
    default:
        break;
    }
    return x->pid < y->pid ? -1 : (x->pid > y->pid ? 1 : 0);
}

/* ------------------------------------------------------------- refresh */

static void refresh(void)
{
    uint64_t now_ms = get_uptime_ms();
    uint64_t elapsed_ms = g_prev_ms && now_ms > g_prev_ms ? now_ms - g_prev_ms : 0u;

    /* Redrawing the list drops the selection, so remember it by pid and put
     * it back on whichever row that process landed on. */
    int32_t want_pid = g_selected_pid;

    sample_t next[MAX_TRACKED];
    uint32_t next_count = 0u;
    uint32_t row_count = 0u;
    uint64_t total_memory = 0u;

    for (int32_t pid = 1; pid <= PID_SCAN_MAX; ++pid) {
        process_info_t info;
        if (get_process_info(pid, &info) < 0) continue;
        if (info.state == 0u) continue;          /* free slot */

        process_perf_info_t perf;
        bool have_perf = get_process_perf_info(pid, &perf) >= 0;
        uint64_t runtime_ns = have_perf ? perf.runtime_ns : 0u;
        uint64_t memory = have_perf ? perf.memory_usage : info.memory_usage;
        total_memory += memory;

        if (next_count < MAX_TRACKED) {
            next[next_count].pid = pid;
            next[next_count].runtime_ns = runtime_ns;
            next[next_count].seen = true;
            ++next_count;
        }
        if (row_count < MAX_ROWS) {
            row_t *row = &g_rows[row_count++];
            row->pid = pid;
            row->parent = info.parent_pid;
            row->state = info.state;
            row->memory = memory;
            row->permille = cpu_permille(pid, runtime_ns, elapsed_ms);
            strncpy(row->name, info.name[0] ? info.name : "(名前なし)",
                    sizeof(row->name) - 1u);
            row->name[sizeof(row->name) - 1u] = '\0';
        }
    }

    memcpy(g_prev, next, sizeof(sample_t) * next_count);
    g_prev_count = next_count;
    g_prev_ms = now_ms;

    qsort(g_rows, row_count, sizeof(row_t), row_cmp);

    /* Save and restore what the user had: the scroll offset, and the row
     * they were on (tracked back to its pid). */
    uint32_t scroll = w_list->scroll;
    imui_list_clear(w_list);
    w_list->scroll = scroll;

    uint32_t listed = 0u;
    int32_t select_row = -1;
    for (uint32_t i = 0; i < row_count; ++i) {
        const row_t *row = &g_rows[i];
        if (!matches_filter(row->name, row->pid)) continue;

        char memory_text[24], subtext[96];
        format_bytes(row->memory, memory_text, sizeof(memory_text));
        snprintf(subtext, sizeof(subtext),
                 "PID %d  ·  親 %d  ·  %s  ·  CPU %u.%u%%  ·  %s",
                 row->pid, row->parent, state_name(row->state),
                 row->permille / 10u, row->permille % 10u, memory_text);
        imui_list_add(w_list, IMUI_ICON_NONE, row->name, subtext);
        if (listed < MAX_VIEW) g_view[listed] = (uint32_t)row->pid;
        if (row->pid == want_pid) select_row = (int32_t)listed;
        ++listed;
    }

    if (select_row >= 0) w_list->sel = select_row;
    else if (want_pid >= 0) { g_selected_pid = -1; w_list->sel = -1; }

    system_memory_info_t memory_info;
    char used[24] = "?", total[24] = "?", by_process[24];
    if (os_get_memory_info(&memory_info) >= 0) {
        format_bytes(memory_info.used_bytes, used, sizeof(used));
        format_bytes(memory_info.total_bytes, total, sizeof(total));
    }
    format_bytes(total_memory, by_process, sizeof(by_process));
    char status[224];
    snprintf(status, sizeof(status),
             "%u プロセス（表示 %u）  ·  並び順 %s  ·  プロセス合計 %s  ·  システム %s / %s",
             next_count, listed, k_sort_name[g_sort], by_process, used, total);
    imui_set_text(g_app, w_status, status);

    /* Nothing selected means nothing to end -- and the confirmation does
     * not outlive the selection it was about. */
    if (g_arm_ms != 0u && get_uptime_ms() - g_arm_ms >= CONFIRM_ARM_MS)
        g_arm_ms = 0u;
    if (g_arm_ms == 0u)
        imui_set_text(g_app, w_end,
                      g_selected_pid > 0 ? "選択したプロセスを終了" : "プロセスを終了");
    imui_request_paint(g_app);
}

/* ------------------------------------------------------------- callbacks */

static void on_select(imui_app_t *app, imui_widget_t *w, void *user)
{
    (void)app; (void)user;
    int row = imui_list_selected(w);
    g_selected_pid = -1;
    g_arm_ms = 0u;
    if (row >= 0 && row < (int)w->row_count && (uint32_t)row < MAX_VIEW)
        g_selected_pid = (int32_t)g_view[row];
    refresh();
}

static void on_end_process(imui_app_t *app, imui_widget_t *w, void *user)
{
    (void)w; (void)user;
    if (g_selected_pid <= 0) {
        window_show_warning("プロセスマネージャー",
                            "終了するプロセスを選択してください。");
        return;
    }
    if (g_selected_pid == process_get_current_pid()) {
        window_show_warning("プロセスマネージャー",
                            "自分自身は終了できません。");
        return;
    }
    process_info_t info;
    char name[64] = "";
    if (get_process_info(g_selected_pid, &info) >= 0) {
        strncpy(name, info.name, sizeof(name) - 1);
        name[sizeof(name) - 1] = '\0';
    }

    uint64_t now = get_uptime_ms();
    if (g_arm_ms == 0u || now - g_arm_ms >= CONFIRM_ARM_MS) {
        char message[160];
        g_arm_ms = now;
        snprintf(message, sizeof(message),
                 "本当に終了しますか？（PID %d）もう一度押すと終了します（5 秒）",
                 g_selected_pid);
        imui_set_text(g_app, w_end, message);
        return;
    }
    g_arm_ms = 0u;

    if (process_kill(g_selected_pid) < 0) {
        char message[160];
        snprintf(message, sizeof(message),
                 "PID %d (%s) を終了できませんでした。", g_selected_pid, name);
        window_show_error("プロセスマネージャー", message);
    }
    g_selected_pid = -1;
    (void)app;
    refresh();
}

static void on_refresh(imui_app_t *app, imui_widget_t *w, void *user)
{
    (void)app; (void)w; (void)user;
    refresh();
}

static void on_sort(imui_app_t *app, imui_widget_t *w, void *user)
{
    (void)app; (void)w;
    sort_t want = (sort_t)(uintptr_t)user;
    if (want >= SORT_COUNT) return;
    g_sort = want;
    for (uint32_t i = 0; i < SORT_COUNT; ++i)
        imui_set_variant(w_sort[i], (sort_t)i == want ? IMUI_TONAL : IMUI_TEXT);
    refresh();
}

static void on_filter(imui_app_t *app, imui_widget_t *w, void *user)
{
    (void)app; (void)user;
    strncpy(g_filter, imui_get_text(w), sizeof(g_filter) - 1);
    g_filter[sizeof(g_filter) - 1] = '\0';
    refresh();
}

static void on_tick(imui_app_t *app, imui_widget_t *w, void *user)
{
    (void)app; (void)w; (void)user;
    static uint64_t last_ms;
    uint64_t now_ms = get_uptime_ms();
    if (last_ms != 0u && now_ms - last_ms < REFRESH_MS) return;
    last_ms = now_ms;
    refresh();
}

/* ------------------------------------------------------------- entry */

int _start(void);
int _start(void)
{
    serial_write_string("[processmanager] start\n");
    g_app = imui_create(940, 620, "Process Manager");
    if (!g_app) {
        serial_write_string("[processmanager] no window\n");
        for (;;) {}
    }

    imui_widget_t *root = g_app->root;
    root->kind = IMUI_COL; root->pad = 0; root->gap = 0;

    imui_widget_t *toolbar = imui_container(root, IMUI_ROW, 8, 6);
    toolbar->grow = 0; toolbar->min_h = 72; toolbar->fill_bg = true;
    imui_set_variant(toolbar, IMUI_FILLED);
    imui_iconbutton(toolbar, IMUI_ICON_REFRESH, on_refresh, NULL);
    /* Ending a process throws work away, so it is the one control here
     * that carries emphasis. */
    w_end = imui_set_variant(
        imui_button(toolbar, "プロセスを終了", IMUI_ICON_TRASH, on_end_process, NULL),
        IMUI_TONAL);

    /* Order is a view, not data: the same scan, arranged. The chosen one is
     * marked by taking M3's tonal emphasis, so no label needs a bullet. */
    imui_set_type_role(imui_label(toolbar, "並び順"), IMUI_LABEL_SMALL);
    for (uint32_t i = 0; i < SORT_COUNT; ++i)
        w_sort[i] = imui_button(toolbar, k_sort_name[i], IMUI_ICON_NONE,
                                on_sort, (void *)(uintptr_t)i);
    imui_set_variant(w_sort[SORT_PID], IMUI_TONAL);

    imui_set_type_role(imui_label(toolbar, "絞り込み"), IMUI_LABEL_SMALL);
    w_filter = imui_textbox(toolbar, "");
    w_filter->grow = 1;
    w_filter->on_change = on_filter;
    w_filter->on_submit = on_filter;

    imui_widget_t *body = imui_container(root, IMUI_COL, 8, 8);
    body->grow = 1;
    w_list = imui_list(body);
    w_list->grow = 1;
    w_list->on_activate = on_select;
    imui_set_single_click(w_list, true);

    imui_widget_t *status_bar = imui_container(root, IMUI_ROW, 8, 0);
    status_bar->grow = 0; status_bar->min_h = 28; status_bar->fill_bg = true;
    imui_set_variant(status_bar, IMUI_FILLED);
    w_status = imui_set_type_role(imui_label(status_bar, ""), IMUI_LABEL_SMALL);
    w_status->grow = 1;

    g_app->on_tick = on_tick;
    refresh();
    return imui_run(g_app);
}
