/*
 * ImplusOS Task Manager.
 *
 * A task manager in the shape people already know from Windows: a
 * navigation rail on the left with プロセス / パフォーマンス / 起動の記録 /
 * 詳細, a search box across the top, a process table with column totals,
 * heat-shaded cells and click-to-sort headers, a performance page with
 * scrolling 60-second graphs, and right-click to end a task or change its
 * priority.
 *
 * Everything shown is something the kernel actually measures:
 *
 *   - CPU      os_get_cpu_usage() (cumulative idle per core) for the
 *              machine, process_perf_info_t.runtime_ns for each process.
 *              Both are cumulative, so the share is the difference between
 *              two samples over the wall time between them.
 *   - Memory   os_get_memory_info() and each process's memory_usage.
 *   - GPU      the kernel counts every present a process makes and the
 *              bytes it pushes to the display, so "画面転送" is that rate.
 *              There is no GPU-busy counter, so there is no GPU-% graph.
 *   - Disks    capacity and attributes only. The block layer keeps no
 *              transfer counters and the kernel reports no used/free space,
 *              so the page says so rather than drawing an empty graph. The
 *              first disk query also waits for the kernel's disk probe
 *              (~15 s on a fresh boot under QEMU), so disks are fetched only
 *              when the Performance page is opened, after it has drawn a
 *              "loading" tile, and then every 30 s -- never on the path to
 *              the window's first frame.
 *   - Boot     the kernel's own boot profile (the 19 instrumented phases).
 *
 * The whole window is one UI canvas drawn immediate-mode: every paint
 * lays the page out again and records the clickable rectangles ("hot"
 * regions) it drew, and the pointer handler looks clicks up in that list.
 * Colours come from the desktop theme (Theme.h), so the window follows the
 * accent colour and light/dark mode like every other app.
 */

#include "UI.h"
#include "Process.h"
#include "SystemInfo.h"
#include "Serial.h"
#include "Window.h"
#include "Input.h"

#include "../../../Kernel/Source/include/kernel/keycodes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------ constants */

#define PID_SCAN_MAX     512        /* OS_CONFIG_PROCESS_MAX_COUNT; pids index it */
#define MAX_PROCS        512u
#define HISTORY          60u        /* samples a graph spans */
#define MAX_CORES        OS_CPU_USAGE_MAX_CORES
#define MAX_DISKS        8u
#define MAX_BOOT         32u
#define MAX_HOT          320u

#define TITLE_H          48
#define NAV_W            196
#define PAGE_HEAD_H      56
#define ROW_H            32
#define GROUP_H          34
#define SCROLLBAR_W      10

typedef enum { PAGE_PROCESSES = 0, PAGE_PERFORMANCE, PAGE_BOOT, PAGE_DETAILS, PAGE_COUNT } page_t;
typedef enum { TBL_PROC = 0, TBL_DETAIL, TBL_BOOT, TBL_COUNT } table_id_t;

/* Performance page devices, in sidebar order. Disks follow GPU. */
enum { PERF_CPU = 0, PERF_MEMORY, PERF_GPU, PERF_DISK0 };

typedef enum {
    HOT_NONE = 0,
    HOT_NAV,            /* a = page                         */
    HOT_SEARCH,
    HOT_SPEED,
    HOT_HEADER,         /* a = table, b = column            */
    HOT_ROW,            /* a = pid (boot: index), b = table */
    HOT_GROUP,          /* a = group                        */
    HOT_THUMB,          /* a = table                        */
    HOT_TRACK,          /* a = table, b = +1 / -1           */
    HOT_END_TASK,
    HOT_CPU_MODE,
    HOT_PERF_TILE,      /* a = device index                 */
    HOT_MENU_ITEM,      /* a = menu action                  */
    HOT_SCRIM,
    HOT_DLG_OK,
    HOT_DLG_CANCEL,
} hot_kind_t;

typedef enum {
    MENU_END = 0,
    MENU_PRIO_LOW, MENU_PRIO_NORMAL, MENU_PRIO_HIGH, MENU_PRIO_HIGHEST,
    MENU_GOTO_DETAILS,
    MENU_COUNT
} menu_action_t;

/* Process table groups, Windows-style: the session's own machinery (init,
 * the window manager, login, notifications) is "ImplusOS プロセス"; what the
 * window manager launched -- the Start menu, the desktop icons -- or anything
 * that has pushed pixels to the display is an app; the rest works in the
 * background. */
enum { GROUP_APPS = 0, GROUP_BACKGROUND, GROUP_SYSTEM, GROUP_COUNT };

static const char *const k_system_names[] = {
    "Userland", "com.ImplusOS.windowmanager", "com.ImplusOS.loginui",
    "com.ImplusOS.sysnotif",
};

/* ------------------------------------------------------------ data */

typedef struct {
    int32_t  pid, parent;
    uint8_t  state, priority;
    uint8_t  group;
    char     name[64];      /* what the kernel calls it, minus path and .ELF */
    char     label[64];     /* what the Processes page calls it              */
    uint64_t runtime_ns;
    uint64_t memory;
    uint64_t page_faults;
    uint64_t syscalls;
    uint64_t display_bytes;
    uint64_t display_presents;
    uint32_t cpu_pm;        /* share of the whole machine, per mille */
    uint64_t display_rate;  /* bytes per second */
    uint64_t present_rate;  /* presents per second */
    uint64_t syscall_rate;  /* per second */
} proc_t;

typedef struct {
    bool     valid;
    uint64_t runtime_ns, display_bytes, display_presents, syscalls;
} prev_t;

typedef struct { int32_t x, y, w, h; hot_kind_t kind; int32_t a, b; } hot_t;

typedef struct { const char *title; int32_t width; bool right; uint8_t heat; } column_t;

enum { PC_NAME, PC_STATE, PC_CPU, PC_MEM, PC_DISP, PC_COUNT };
static const column_t k_proc_cols[PC_COUNT] = {
    { "名前",     0,   false, 0 },
    { "状態",     96,  false, 0 },
    { "CPU",      84,  true,  1 },
    { "メモリ",   112, true,  2 },
    { "画面転送", 120, true,  3 },
};

enum { DC_NAME, DC_PID, DC_STATE, DC_PRIO, DC_CPU, DC_TIME, DC_MEM, DC_PF, DC_SYS, DC_COUNT };
static const column_t k_detail_cols[DC_COUNT] = {
    { "名前",              0,   false, 0 },
    { "PID",               60,  true,  0 },
    { "状態",              80,  false, 0 },
    { "優先度",            72,  false, 0 },
    { "CPU",               56,  true,  1 },
    { "CPU 時間",          88,  true,  0 },
    { "メモリ",            96,  true,  2 },
    { "ページ フォールト", 112, true,  0 },
    { "システムコール/秒", 128, true,  0 },
};

enum { BC_NAME, BC_START, BC_DUR, BC_BAR, BC_COUNT };
static const column_t k_boot_cols[BC_COUNT] = {
    { "フェーズ",     220, false, 0 },
    { "開始",         96,  true,  0 },
    { "所要時間",     104, true,  3 },
    { "タイムライン", 0,   false, 0 },
};

static const char *const k_page_name[PAGE_COUNT] = {
    "プロセス", "パフォーマンス", "起動の記録", "詳細",
};
static const ui_icon_t k_page_icon[PAGE_COUNT] = {
    UI_ICON_APPS, UI_ICON_MEMORY, UI_ICON_RESTART, UI_ICON_MENU,
};

/* Update speed, as Windows offers it. 0 = paused. */
static const uint32_t k_speed_ms[4] = { 500u, 1000u, 4000u, 0u };
static const char *const k_speed_name[4] = { "高速", "標準", "低速", "一時停止" };

/* First-party processes by their reverse-domain names, so the Processes
 * page can say "テキストエディタ" where Details says com.ImplusOS.editor. */
static const struct { const char *name, *label; } k_friendly[] = {
    { "com.ImplusOS.windowmanager",     "ウィンドウ マネージャー" },
    { "com.ImplusOS.sysnotif",          "通知サービス" },
    { "com.ImplusOS.loginui",           "ログイン画面" },
    { "com.ImplusOS.editor",            "テキストエディタ" },
    { "com.ImplusOS.filemanager",       "ファイル" },
    { "com.ImplusOS.settings",          "設定" },
    { "com.ImplusOS.calculator",        "電卓" },
    { "com.ImplusOS.serialmonitor",     "シリアルモニタ" },
    { "com.ImplusOS.taskmanager",       "タスク マネージャー" },
    { "com.ImplusOS.waylandcompositor", "Wayland コンポジタ" },
    { "com.ImplusOS.linuxapp",          "Linux アプリ ホスト" },
    { "com.ImplusOS.gtk3demo",          "GTK3 デモ" },
    { "Terminal",                       "ターミナル" },
};

/* ------------------------------------------------------------ state */

static ui_app_t    *g_app;
static ui_widget_t *g_canvas;

static proc_t   g_procs[MAX_PROCS];
static uint32_t g_proc_count;
static prev_t   g_prev[PID_SCAN_MAX + 1];
static uint64_t g_prev_sample_ns;
static int32_t  g_self_pid;

/* view orders: indexes into g_procs (group headers encoded as ~group) */
static int32_t  g_proc_view[MAX_PROCS + GROUP_COUNT];
static uint32_t g_proc_view_count;
static uint32_t g_group_count[GROUP_COUNT];
static bool     g_group_collapsed[GROUP_COUNT];
static int32_t  g_detail_view[MAX_PROCS];
static uint32_t g_detail_view_count;

/* machine */
static system_cpu_info_t      g_cpu;
static system_graphics_info_t g_gfx;
static bool                   g_have_cpu, g_have_gfx;
static system_cpu_usage_t     g_cpu_prev;
static bool                   g_cpu_have_prev;
static uint32_t               g_cores = 1u;
static system_memory_info_t   g_mem;
static system_vmem_info_t     g_vmem;
static bool                   g_have_vmem;
static system_disk_info_t     g_disks[MAX_DISKS];
static uint32_t               g_disk_count;
static enum { DISKS_NONE = 0, DISKS_PENDING, DISKS_LOADED } g_disk_state;
static uint32_t               g_disk_pending_frame;
static uint64_t               g_disk_ms;
static uint32_t               g_frames;      /* paints so far */
static boot_profile_entry_t   g_boot[MAX_BOOT];
static uint32_t               g_boot_count;
static int32_t                g_boot_view[MAX_BOOT];

/* histories, oldest first; per mille of the graph's scale except where noted */
static uint32_t g_hist_n;
static uint64_t g_samples_taken;
static uint32_t g_h_cpu[HISTORY];
static uint32_t g_h_core[MAX_CORES][HISTORY];
static uint32_t g_h_mem[HISTORY];
static uint64_t g_h_disp[HISTORY];     /* bytes per second */
static uint32_t g_cpu_now_pm, g_core_now_pm[MAX_CORES];
static uint64_t g_disp_now, g_present_now;
static uint32_t g_presenters;

/* UI */
static page_t   g_page = PAGE_PROCESSES;
static uint32_t g_perf_sel = PERF_CPU;
static bool     g_cpu_per_core;
static uint32_t g_speed = 1u;
static uint64_t g_last_sample_ms;
static char     g_search[64];
static bool     g_search_focus;
static int32_t  g_sel_pid = -1;
static int32_t  g_sel_boot = -1;
static int32_t  g_sort_col[TBL_COUNT] = { -1, DC_PID, -1 };
static bool     g_sort_desc[TBL_COUNT];
static int32_t  g_scroll[TBL_COUNT];
static int32_t  g_visible_rows[TBL_COUNT] = { 10, 10, 10 };

/* scrollbar geometry from the last paint, for dragging */
static struct { int32_t track_y, track_h, thumb_h, max_scroll; int32_t x, y, w, h; }
    g_tbl_geo[TBL_COUNT];
static int32_t g_drag_table = -1, g_drag_start_y, g_drag_start_scroll;

static hot_t    g_hot[MAX_HOT];
static uint32_t g_hot_count;
static hot_t    g_press;              /* what the left button went down on */
static int32_t  g_px = -1, g_py = -1;
static bool     g_pin;

static bool     g_menu_open;
static int32_t  g_menu_x, g_menu_y, g_menu_pid;
static bool     g_dialog_open;
static int32_t  g_dialog_pid;
static char     g_dialog_name[64];

/* ------------------------------------------------------------ format */

static void fmt_bytes(uint64_t bytes, char *out, size_t cap)
{
    static const char *const unit[] = { "B", "KB", "MB", "GB", "TB" };
    uint64_t div = 1u;
    uint32_t u = 0u;
    while (u < 4u && bytes >= div * 1024u) { div *= 1024u; ++u; }
    if (u == 0u) { snprintf(out, cap, "%llu B", (unsigned long long)bytes); return; }
    uint64_t tenths = (bytes * 10u + div / 2u) / div;
    snprintf(out, cap, "%llu.%llu %s", (unsigned long long)(tenths / 10u),
             (unsigned long long)(tenths % 10u), unit[u]);
}

static void fmt_rate(uint64_t bytes_per_s, char *out, size_t cap)
{
    if (bytes_per_s == 0u) { snprintf(out, cap, "0 MB/秒"); return; }
    uint64_t tenths = (bytes_per_s * 10u + (1u << 19)) >> 20;
    if (tenths == 0u) {
        snprintf(out, cap, "%llu KB/秒", (unsigned long long)((bytes_per_s + 512u) >> 10));
        return;
    }
    snprintf(out, cap, "%llu.%llu MB/秒", (unsigned long long)(tenths / 10u),
             (unsigned long long)(tenths % 10u));
}

static void fmt_pm(uint32_t pm, char *out, size_t cap)
{
    if (pm == 0u) snprintf(out, cap, "0%%");
    else snprintf(out, cap, "%u.%u%%", pm / 10u, pm % 10u);
}

static void fmt_count(uint64_t v, char *out, size_t cap)
{
    char tmp[32];
    snprintf(tmp, sizeof(tmp), "%llu", (unsigned long long)v);
    size_t n = strlen(tmp), o = 0u;
    for (size_t i = 0; i < n && o + 2u < cap; ++i) {
        if (i > 0u && (n - i) % 3u == 0u) out[o++] = ',';
        out[o++] = tmp[i];
    }
    out[o] = '\0';
}

static void fmt_clock(uint64_t seconds, bool days, char *out, size_t cap)
{
    unsigned long long s = seconds % 60u, m = (seconds / 60u) % 60u;
    if (days) {
        snprintf(out, cap, "%llu:%02llu:%02llu:%02llu",
                 (unsigned long long)(seconds / 86400u),
                 (unsigned long long)((seconds / 3600u) % 24u), m, s);
    } else {
        snprintf(out, cap, "%llu:%02llu:%02llu",
                 (unsigned long long)(seconds / 3600u), m, s);
    }
}

static void fmt_ms(uint64_t ns, char *out, size_t cap)
{
    uint64_t hundredths = ns / 10000u;
    if (hundredths >= 100000u)
        snprintf(out, cap, "%llu ms", (unsigned long long)(hundredths / 100u));
    else
        snprintf(out, cap, "%llu.%02llu ms", (unsigned long long)(hundredths / 100u),
                 (unsigned long long)(hundredths % 100u));
}

/* Mirrors PROCESS_STATE_* in Kernel/Core/process/ProcessScheduler.h. */
static const char *state_name(uint8_t state)
{
    static const char *const names[] = {
        "未使用", "実行可能", "実行中", "終了", "起動中", "ゾンビ", "待機中",
    };
    return state < sizeof(names) / sizeof(names[0]) ? names[state] : "不明";
}

static const char *priority_name(uint8_t prio)
{
    static const char *const names[] = { "低", "通常", "高", "最高" };
    return prio < 4u ? names[prio] : "?";
}

/* ------------------------------------------------------------ sampling */

static void derive_names(proc_t *p, const char *raw)
{
    const char *base = raw;
    for (const char *c = raw; *c; ++c)
        if (*c == '/') base = c + 1;
    if (!*base) base = "(名前なし)";
    strncpy(p->name, base, sizeof(p->name) - 1u);
    p->name[sizeof(p->name) - 1u] = '\0';
    size_t n = strlen(p->name);
    if (n > 4u && strcmp(p->name + n - 4u, ".ELF") == 0) p->name[n - 4u] = '\0';

    strcpy(p->label, p->name);
    for (size_t i = 0; i < sizeof(k_friendly) / sizeof(k_friendly[0]); ++i) {
        if (strcmp(p->name, k_friendly[i].name) == 0) {
            strncpy(p->label, k_friendly[i].label, sizeof(p->label) - 1u);
            p->label[sizeof(p->label) - 1u] = '\0';
            break;
        }
    }
}

static void history_push32(uint32_t *h, uint32_t v)
{
    memmove(h, h + 1, sizeof(uint32_t) * (HISTORY - 1u));
    h[HISTORY - 1u] = v;
}

static void history_push64(uint64_t *h, uint64_t v)
{
    memmove(h, h + 1, sizeof(uint64_t) * (HISTORY - 1u));
    h[HISTORY - 1u] = v;
}

static void build_views(void);

static void sample(void)
{
    uint64_t now_ns = get_uptime_ms() * 1000000ull;

    /* Machine CPU: busy = wall - idle, per core. */
    system_cpu_usage_t usage;
    bool have_cpu_delta = false;
    if (os_get_cpu_usage(&usage) >= 0) {
        uint32_t cores = usage.cpu_count;
        if (cores == 0u) cores = 1u;
        if (cores > MAX_CORES) cores = MAX_CORES;
        g_cores = cores;
        if (g_cpu_have_prev && usage.wall_ns > g_cpu_prev.wall_ns) {
            uint64_t wall = usage.wall_ns - g_cpu_prev.wall_ns;
            uint64_t busy_total = 0u;
            for (uint32_t i = 0; i < cores; ++i) {
                uint64_t idle = usage.idle_ns[i] >= g_cpu_prev.idle_ns[i]
                                    ? usage.idle_ns[i] - g_cpu_prev.idle_ns[i] : 0u;
                if (idle > wall) idle = wall;
                uint64_t busy = wall - idle;
                busy_total += busy;
                g_core_now_pm[i] = (uint32_t)((busy * 1000u) / wall);
            }
            g_cpu_now_pm = (uint32_t)((busy_total * 1000u) / (wall * cores));
            have_cpu_delta = true;
        }
        g_cpu_prev = usage;
        g_cpu_have_prev = true;
    }

    if (os_get_memory_info(&g_mem) < 0) memset(&g_mem, 0, sizeof(g_mem));
    g_have_vmem = os_get_vmem_info(&g_vmem) >= 0;

    /* Processes. */
    uint64_t elapsed_ns = g_prev_sample_ns && now_ns > g_prev_sample_ns
                              ? now_ns - g_prev_sample_ns : 0u;
    bool seen[PID_SCAN_MAX + 1];
    memset(seen, 0, sizeof(seen));
    g_proc_count = 0u;
    g_disp_now = 0u;
    g_present_now = 0u;
    g_presenters = 0u;

    for (int32_t pid = 1; pid <= PID_SCAN_MAX && g_proc_count < MAX_PROCS; ++pid) {
        process_info_t info;
        if (get_process_info(pid, &info) < 0) continue;
        if (info.state == 0u || info.state == 3u) continue;   /* free / dead */

        process_perf_info_t perf;
        bool have_perf = get_process_perf_info(pid, &perf) >= 0;

        proc_t *p = &g_procs[g_proc_count++];
        memset(p, 0, sizeof(*p));
        p->pid = pid;
        p->parent = info.parent_pid;
        p->state = info.state;
        p->priority = info.reserved[0];
        derive_names(p, info.name);
        p->memory = have_perf ? perf.memory_usage : info.memory_usage;
        if (have_perf) {
            p->runtime_ns = perf.runtime_ns;
            p->page_faults = perf.page_fault_count;
            p->syscalls = perf.syscalls;
            p->display_bytes = perf.display_bytes;
            p->display_presents = perf.display_present_calls;
        }

        prev_t *pv = &g_prev[pid];
        if (pv->valid && elapsed_ns > 0u) {
            uint64_t d_run = p->runtime_ns >= pv->runtime_ns ? p->runtime_ns - pv->runtime_ns : 0u;
            uint64_t pm = (d_run * 1000u) / (elapsed_ns * g_cores);
            p->cpu_pm = pm > 1000u ? 1000u : (uint32_t)pm;
            uint64_t d_disp = p->display_bytes >= pv->display_bytes
                                  ? p->display_bytes - pv->display_bytes : 0u;
            uint64_t d_pres = p->display_presents >= pv->display_presents
                                  ? p->display_presents - pv->display_presents : 0u;
            uint64_t d_sys = p->syscalls >= pv->syscalls ? p->syscalls - pv->syscalls : 0u;
            p->display_rate = (d_disp * 1000000000ull) / elapsed_ns;
            p->present_rate = (d_pres * 1000000000ull) / elapsed_ns;
            p->syscall_rate = (d_sys * 1000000000ull) / elapsed_ns;
        }
        pv->valid = true;
        pv->runtime_ns = p->runtime_ns;
        pv->display_bytes = p->display_bytes;
        pv->display_presents = p->display_presents;
        pv->syscalls = p->syscalls;
        seen[pid] = true;

        g_disp_now += p->display_rate;
        g_present_now += p->present_rate;
        if (p->present_rate > 0u) ++g_presenters;
    }
    /* Groups need every process's name first: an app is a child of the
     * window manager. */
    int32_t wm_pid = -1;
    for (uint32_t i = 0; i < g_proc_count; ++i)
        if (strcmp(g_procs[i].name, "com.ImplusOS.windowmanager") == 0) wm_pid = g_procs[i].pid;
    for (uint32_t i = 0; i < g_proc_count; ++i) {
        proc_t *p = &g_procs[i];
        p->group = GROUP_BACKGROUND;
        for (size_t k = 0; k < sizeof(k_system_names) / sizeof(k_system_names[0]); ++k)
            if (p->pid == 1 || strcmp(p->name, k_system_names[k]) == 0) p->group = GROUP_SYSTEM;
        if (p->group != GROUP_SYSTEM &&
            ((wm_pid > 0 && p->parent == wm_pid) || p->display_presents > 0u))
            p->group = GROUP_APPS;
    }

    /* A pid that went away must not lend its counters to the next process
     * that gets the slot. */
    for (int32_t pid = 0; pid <= PID_SCAN_MAX; ++pid)
        if (!seen[pid]) g_prev[pid].valid = false;
    g_prev_sample_ns = now_ns;

    /* Histories. The first sample has no delta yet, so it only seeds. */
    if (have_cpu_delta) {
        history_push32(g_h_cpu, g_cpu_now_pm);
        for (uint32_t i = 0; i < MAX_CORES; ++i)
            history_push32(g_h_core[i], i < g_cores ? g_core_now_pm[i] : 0u);
        uint32_t mem_pm = g_mem.total_bytes
            ? (uint32_t)((g_mem.used_bytes * 1000u) / g_mem.total_bytes) : 0u;
        history_push32(g_h_mem, mem_pm);
        history_push64(g_h_disp, g_disp_now);
        if (g_hist_n < HISTORY) ++g_hist_n;
        ++g_samples_taken;
    }

    build_views();
    ui_redraw();
}

/* Disks change rarely, but a USB stick can arrive. See the header comment
 * for why this is kept off the sampling path. */
static void refresh_disks(void)
{
    uint32_t disks = 0u;
    g_disk_count = 0u;
    if (os_get_disk_count(&disks) >= 0) {
        for (uint32_t i = 0; i < disks && g_disk_count < MAX_DISKS; ++i)
            if (os_get_disk_info(i, &g_disks[g_disk_count]) >= 0) ++g_disk_count;
    }
    if (g_perf_sel >= PERF_DISK0 + g_disk_count) g_perf_sel = PERF_CPU;
    g_disk_ms = get_uptime_ms();
    g_disk_state = DISKS_LOADED;
    ui_redraw();
}

static void load_static_info(void)
{
    g_have_cpu = os_get_cpu_info(&g_cpu) >= 0;
    g_have_gfx = os_get_graphics_info(&g_gfx) >= 0;
    int32_t count = get_boot_profile_count();
    g_boot_count = 0u;
    for (int32_t i = 0; i < count && g_boot_count < MAX_BOOT; ++i)
        if (get_boot_profile_entry(i, &g_boot[g_boot_count]) >= 0) ++g_boot_count;
}

/* ------------------------------------------------------------ views */

static bool icontains(const char *hay, const char *needle)
{
    if (!*needle) return true;
    for (const char *h = hay; *h; ++h) {
        const char *n = needle, *p = h;
        while (*n && *p) {
            char a = (*p >= 'A' && *p <= 'Z') ? (char)(*p + 32) : *p;
            char b = (*n >= 'A' && *n <= 'Z') ? (char)(*n + 32) : *n;
            if (a != b) break;
            ++n; ++p;
        }
        if (!*n) return true;
    }
    return false;
}

static bool matches_search(const proc_t *p)
{
    if (!g_search[0]) return true;
    char pid[16];
    snprintf(pid, sizeof(pid), "%d", p->pid);
    return icontains(p->name, g_search) || icontains(p->label, g_search) ||
           strcmp(pid, g_search) == 0;
}

static int g_cmp_table;

static int cmp_u64(uint64_t a, uint64_t b) { return a < b ? -1 : (a > b ? 1 : 0); }

static int proc_cmp(const void *va, const void *vb)
{
    const proc_t *a = &g_procs[*(const int32_t *)va];
    const proc_t *b = &g_procs[*(const int32_t *)vb];
    int t = g_cmp_table, col = g_sort_col[t], r = 0;
    if (t == TBL_PROC) {
        switch (col) {
        case PC_NAME:  r = strcmp(a->label, b->label); break;
        case PC_STATE: r = (int)a->state - (int)b->state; break;
        case PC_CPU:   r = cmp_u64(a->cpu_pm, b->cpu_pm); break;
        case PC_MEM:   r = cmp_u64(a->memory, b->memory); break;
        case PC_DISP:  r = cmp_u64(a->display_rate, b->display_rate); break;
        default:       r = strcmp(a->label, b->label); break;
        }
    } else {
        switch (col) {
        case DC_NAME:  r = strcmp(a->name, b->name); break;
        case DC_PID:   r = cmp_u64((uint64_t)a->pid, (uint64_t)b->pid); break;
        case DC_STATE: r = (int)a->state - (int)b->state; break;
        case DC_PRIO:  r = (int)a->priority - (int)b->priority; break;
        case DC_CPU:   r = cmp_u64(a->cpu_pm, b->cpu_pm); break;
        case DC_TIME:  r = cmp_u64(a->runtime_ns, b->runtime_ns); break;
        case DC_MEM:   r = cmp_u64(a->memory, b->memory); break;
        case DC_PF:    r = cmp_u64(a->page_faults, b->page_faults); break;
        case DC_SYS:   r = cmp_u64(a->syscall_rate, b->syscall_rate); break;
        default: break;
        }
    }
    if (col >= 0 && g_sort_desc[t]) r = -r;
    if (r == 0) r = a->pid - b->pid;
    return r;
}

static int boot_cmp(const void *va, const void *vb)
{
    const boot_profile_entry_t *a = &g_boot[*(const int32_t *)va];
    const boot_profile_entry_t *b = &g_boot[*(const int32_t *)vb];
    int col = g_sort_col[TBL_BOOT], r = 0;
    if (col == BC_NAME) r = strcmp(a->name, b->name);
    else if (col == BC_DUR) r = cmp_u64(a->duration_ns, b->duration_ns);
    else r = cmp_u64(a->start_ns, b->start_ns);
    if (col >= 0 && g_sort_desc[TBL_BOOT]) r = -r;
    if (r == 0) r = *(const int32_t *)va - *(const int32_t *)vb;
    return r;
}

static void build_views(void)
{
    int32_t group_items[GROUP_COUNT][MAX_PROCS];
    memset(g_group_count, 0, sizeof(g_group_count));
    g_detail_view_count = 0u;
    for (uint32_t i = 0; i < g_proc_count; ++i) {
        if (!matches_search(&g_procs[i])) continue;
        g_detail_view[g_detail_view_count++] = (int32_t)i;
        /* a zombie is a pid waiting to be reaped, not a task: Details lists
         * it, the Processes page does not */
        if (g_procs[i].state == 5u) continue;
        uint32_t g = g_procs[i].group;
        group_items[g][g_group_count[g]++] = (int32_t)i;
    }

    g_cmp_table = TBL_PROC;
    g_proc_view_count = 0u;
    for (uint32_t g = 0; g < GROUP_COUNT; ++g) {
        if (g_group_count[g] == 0u) continue;
        qsort(group_items[g], g_group_count[g], sizeof(int32_t), proc_cmp);
        g_proc_view[g_proc_view_count++] = ~(int32_t)g;
        if (g_group_collapsed[g]) continue;
        for (uint32_t k = 0; k < g_group_count[g]; ++k)
            g_proc_view[g_proc_view_count++] = group_items[g][k];
    }

    g_cmp_table = TBL_DETAIL;
    qsort(g_detail_view, g_detail_view_count, sizeof(int32_t), proc_cmp);

    for (uint32_t i = 0; i < g_boot_count; ++i) g_boot_view[i] = (int32_t)i;
    qsort(g_boot_view, g_boot_count, sizeof(int32_t), boot_cmp);
}

static const proc_t *find_proc(int32_t pid)
{
    for (uint32_t i = 0; i < g_proc_count; ++i)
        if (g_procs[i].pid == pid) return &g_procs[i];
    return NULL;
}

/* ------------------------------------------------------------ drawing kit */

static const theme_t *TH(void) { return &g_app->theme; }

static void hot_add(int32_t x, int32_t y, int32_t w, int32_t h, hot_kind_t kind,
                    int32_t a, int32_t b)
{
    if (g_hot_count >= MAX_HOT) return;
    hot_t *t = &g_hot[g_hot_count++];
    t->x = x; t->y = y; t->w = w; t->h = h; t->kind = kind; t->a = a; t->b = b;
}

static const hot_t *hot_at(int32_t x, int32_t y)
{
    for (uint32_t i = g_hot_count; i-- > 0u;) {
        const hot_t *t = &g_hot[i];
        if (x >= t->x && y >= t->y && x < t->x + t->w && y < t->y + t->h) return t;
    }
    return NULL;
}

/* Hovering only counts for what is on top: the topmost hot region drawn so
 * far is not known until the paint ends, so a modal suppresses hover below
 * it by being open rather than by geometry. */
static bool hovered(int32_t x, int32_t y, int32_t w, int32_t h)
{
    if (!g_pin || g_menu_open || g_dialog_open) return false;
    return g_px >= x && g_py >= y && g_px < x + w && g_py < y + h;
}

/* Text vertically centred in a band of height h. */
static void text_mid(ui_canvas_t *c, int32_t x, int32_t y, int32_t h, const char *s,
                     float px, uint32_t col, int32_t clip)
{
    ui_draw_text(c, x, y + (h - (int32_t)px) / 2 - 2, s, px, col, clip);
}

static void text_right(ui_canvas_t *c, int32_t right, int32_t y, int32_t h,
                       const char *s, float px, uint32_t col)
{
    text_mid(c, right - ui_text_width(s, px), y, h, s, px, col, 0);
}

/* A small chevron, pointing up or down, for sort order and groups. */
static void chevron(ui_canvas_t *c, int32_t cx, int32_t cy, bool up, uint32_t col)
{
    int32_t d = up ? -1 : 1;
    ui_draw_line(c, cx - 4, cy - 2 * d, cx, cy + 2 * d, 1, col);
    ui_draw_line(c, cx + 4, cy - 2 * d, cx, cy + 2 * d, 1, col);
}

/* Button styles, lowest emphasis first. */
typedef enum { BTN_TEXT = 0, BTN_FILLED, BTN_TONAL, BTN_OUTLINED } btn_style_t;

/* A pill button. Returns its width. */
static int32_t button(ui_canvas_t *c, int32_t x, int32_t y, const char *label,
                      ui_icon_t icon, btn_style_t v, bool enabled, hot_kind_t kind,
                      int32_t a)
{
    const theme_t *th = TH();
    float px = THEME_FONT_BODY;
    int32_t w = ui_text_width(label, px) + (icon ? 56 : 32), h = 36;
    uint32_t bg = 0u, fg = th->accent;
    if (v == BTN_FILLED) { bg = th->accent; fg = th->on_accent; }
    else if (v == BTN_TONAL) { bg = th->accent_soft; fg = th->text; }
    if (!enabled) {
        bg = bg ? color_alpha(th->text, 12u) : 0u;
        fg = color_alpha(th->text, THEME_DISABLED_PCT);
    }
    if (bg) ui_draw_round(c, x, y, w, h, h / 2, bg);
    if (v == BTN_OUTLINED) {
        ui_draw_round(c, x, y, w, h, h / 2, th->border);
        ui_draw_round(c, x + 1, y + 1, w - 2, h - 2, h / 2 - 1, th->surface);
    }
    if (enabled && hovered(x, y, w, h))
        ui_draw_round(c, x, y, w, h, h / 2, color_alpha(fg, THEME_HOVER_PCT));
    int32_t tx = x + 16;
    if (icon) { ui_draw_icon(c, x + 14, y + 9, 18, icon, fg); tx = x + 40; }
    text_mid(c, tx, y, h, label, px, fg, 0);
    if (enabled) hot_add(x, y, w, h, kind, a, 0);
    return w;
}

/* The fill tint of a heat-mapped cell. Amber, as Windows does, at an
 * opacity that rises with the value -- legible over the light and the dark
 * surface alike because it is translucent. */
static uint32_t heat_color(uint32_t pm)
{
    if (pm > 1000u) pm = 1000u;
    uint32_t percent = 10u + (pm * 55u) / 1000u;
    return color_alpha(0xFFFFB300u, percent);
}

/* ------------------------------------------------------------ graph */

/* One 60-second graph. `values` are per mille of the graph's full scale,
 * oldest first; only the newest `valid` are real. */
static void graph(ui_canvas_t *c, int32_t x, int32_t y, int32_t w, int32_t h,
                  const uint32_t *values, uint32_t valid, uint32_t color, bool grid)
{
    const theme_t *th = TH();
    if (w < 4 || h < 4) return;
    ui_draw_fill(c, x, y, w, h, th->surface);
    if (grid) {
        uint32_t line = color_alpha(color, 14u);
        for (int32_t i = 1; i < 10; ++i)
            ui_draw_fill(c, x, y + h * i / 10, w, 1, line);
        /* vertical lines scroll left with time, one step per sample */
        int32_t step = w / 10;
        int32_t shift = (int32_t)(g_samples_taken % 6u) * w / 60;
        for (int32_t gx = x + w - shift; gx > x; gx -= step)
            if (step > 0) ui_draw_fill(c, gx, y, 1, h, line);
    }

    if (valid > HISTORY) valid = HISTORY;
    if (valid >= 1u) {
        uint32_t fill = color_alpha(color, 22u);
        uint32_t first = HISTORY - valid;
        int32_t prev_x = -1, prev_y = 0;
        for (int32_t px = 0; px < w; ++px) {
            /* position along the 60 samples, fixed point 16.16 */
            uint32_t pos = (uint32_t)(((uint64_t)px * (HISTORY - 1u) << 16) /
                                      (uint64_t)(w > 1 ? w - 1 : 1));
            uint32_t i = pos >> 16, frac = pos & 0xFFFFu;
            if (i < first) continue;
            uint32_t v0 = values[i], v1 = values[i + 1u < HISTORY ? i + 1u : i];
            uint32_t v = (uint32_t)(((uint64_t)v0 * (65536u - frac) + (uint64_t)v1 * frac) >> 16);
            if (v > 1000u) v = 1000u;
            int32_t vy = y + h - 1 - (int32_t)((uint64_t)v * (uint64_t)(h - 2) / 1000u);
            ui_draw_fill(c, x + px, vy, 1, y + h - vy, fill);
            if (prev_x >= 0) ui_draw_line(c, x + prev_x, prev_y, x + px, vy, 1, color);
            prev_x = px; prev_y = vy;
        }
    }
    ui_draw_frame(c, x, y, w, h, color);
}

/* ------------------------------------------------------------ table */

static const column_t *table_cols(int32_t t, uint32_t *n)
{
    switch (t) {
    case TBL_PROC:   *n = PC_COUNT; return k_proc_cols;
    case TBL_DETAIL: *n = DC_COUNT; return k_detail_cols;
    default:         *n = BC_COUNT; return k_boot_cols;
    }
}

static uint32_t table_rows(int32_t t)
{
    if (t == TBL_PROC) return g_proc_view_count;
    if (t == TBL_DETAIL) return g_detail_view_count;
    return g_boot_count;
}

static int32_t row_height(int32_t t, uint32_t row)
{
    (void)row;
    return t == TBL_PROC ? ROW_H : ROW_H - 2;
}

static void clamp_scroll(int32_t t)
{
    int32_t max = (int32_t)table_rows(t) - g_visible_rows[t];
    if (max < 0) max = 0;
    if (g_scroll[t] > max) g_scroll[t] = max;
    if (g_scroll[t] < 0) g_scroll[t] = 0;
}

static void cell_text(int32_t t, int32_t col, int32_t index, char *out, size_t cap)
{
    out[0] = '\0';
    if (t == TBL_BOOT) {
        const boot_profile_entry_t *e = &g_boot[index];
        uint64_t base = g_boot_count ? g_boot[0].start_ns : 0u;
        for (uint32_t i = 0; i < g_boot_count; ++i)
            if (g_boot[i].start_ns < base) base = g_boot[i].start_ns;
        if (col == BC_NAME) snprintf(out, cap, "%s", e->name);
        else if (col == BC_START) fmt_ms(e->start_ns - base, out, cap);
        else if (col == BC_DUR) fmt_ms(e->duration_ns, out, cap);
        return;
    }
    const proc_t *p = &g_procs[index];
    if (t == TBL_PROC) {
        switch (col) {
        case PC_NAME:  snprintf(out, cap, "%s", p->label); break;
        case PC_STATE: snprintf(out, cap, "%s", p->state == 6u || p->state == 1u || p->state == 2u
                                                ? "" : state_name(p->state)); break;
        case PC_CPU:   fmt_pm(p->cpu_pm, out, cap); break;
        case PC_MEM:   fmt_bytes(p->memory, out, cap); break;
        case PC_DISP:  fmt_rate(p->display_rate, out, cap); break;
        default: break;
        }
        return;
    }
    switch (col) {
    case DC_NAME:  snprintf(out, cap, "%s", p->name); break;
    case DC_PID:   snprintf(out, cap, "%d", p->pid); break;
    case DC_STATE: snprintf(out, cap, "%s", state_name(p->state)); break;
    case DC_PRIO:  snprintf(out, cap, "%s", priority_name(p->priority)); break;
    case DC_CPU: {
        uint32_t pct = (p->cpu_pm + 5u) / 10u;
        snprintf(out, cap, "%02u", pct > 99u ? 99u : pct);
        break;
    }
    case DC_TIME:  fmt_clock(p->runtime_ns / 1000000000ull, false, out, cap); break;
    case DC_MEM:   fmt_bytes(p->memory, out, cap); break;
    case DC_PF:    fmt_count(p->page_faults, out, cap); break;
    case DC_SYS:   fmt_count(p->syscall_rate, out, cap); break;
    default: break;
    }
}

static uint32_t cell_heat(int32_t t, int32_t col, int32_t index)
{
    if (t == TBL_BOOT) {
        uint64_t longest = 1u;
        for (uint32_t i = 0; i < g_boot_count; ++i)
            if (g_boot[i].duration_ns > longest) longest = g_boot[i].duration_ns;
        return (uint32_t)((g_boot[index].duration_ns * 1000u) / longest);
    }
    const proc_t *p = &g_procs[index];
    uint32_t n;
    const column_t *cols = table_cols(t, &n);
    switch (cols[col].heat) {
    case 1: return p->cpu_pm * 2u;
    case 2: return g_mem.total_bytes ? (uint32_t)((p->memory * 4000u) / g_mem.total_bytes) : 0u;
    case 3: return (uint32_t)(p->display_rate / (24u * 1024u)); /* 24 MB/s = full */
    default: return 0u;
    }
}

/* Column x positions for a table `w` wide; the flexible column (width 0)
 * takes what the fixed ones leave, but never less than 80px. */
static void column_layout(int32_t t, int32_t w, int32_t *xs, int32_t *ws)
{
    uint32_t n;
    const column_t *cols = table_cols(t, &n);
    int32_t fixed = 0;
    for (uint32_t i = 0; i < n; ++i) fixed += cols[i].width;
    int32_t flex = w - fixed;
    if (flex < 80) flex = 80;
    int32_t x = 0;
    for (uint32_t i = 0; i < n; ++i) {
        xs[i] = x;
        ws[i] = cols[i].width ? cols[i].width : flex;
        x += ws[i];
    }
}

static void header_total(int32_t t, int32_t col, char *out, size_t cap)
{
    out[0] = '\0';
    if (t != TBL_PROC) return;
    if (col == PC_CPU) fmt_pm(g_cpu_now_pm, out, cap);
    else if (col == PC_MEM && g_mem.total_bytes)
        snprintf(out, cap, "%u%%", (unsigned)((g_mem.used_bytes * 100u) / g_mem.total_bytes));
    else if (col == PC_DISP) fmt_rate(g_disp_now, out, cap);
    if (col == PC_CPU && g_cpu_now_pm >= 10u)
        snprintf(out, cap, "%u%%", (g_cpu_now_pm + 5u) / 10u);
}

static void paint_group_row(ui_canvas_t *c, int32_t x, int32_t y, int32_t w, int32_t g)
{
    const theme_t *th = TH();
    if (hovered(x, y, w, GROUP_H))
        ui_draw_fill(c, x, y, w, GROUP_H, color_alpha(th->text, THEME_HOVER_PCT));
    chevron(c, x + 14, y + GROUP_H / 2, !g_group_collapsed[g], th->text_dim);
    char label[64];
    snprintf(label, sizeof(label), "%s (%u)",
             g == GROUP_APPS ? "アプリ" : g == GROUP_SYSTEM ? "ImplusOS プロセス"
                                                          : "バックグラウンド プロセス",
             g_group_count[g]);
    text_mid(c, x + 28, y + 2, GROUP_H, label, THEME_FONT_BODY, th->accent, 0);
    hot_add(x, y, w, GROUP_H, HOT_GROUP, g, 0);
}

static void paint_boot_bar(ui_canvas_t *c, int32_t x, int32_t y, int32_t w, int32_t h,
                           int32_t index, bool strong)
{
    const theme_t *th = TH();
    uint64_t base = ~0ull, end = 0u;
    for (uint32_t i = 0; i < g_boot_count; ++i) {
        if (g_boot[i].start_ns < base) base = g_boot[i].start_ns;
        uint64_t e = g_boot[i].start_ns + g_boot[i].duration_ns;
        if (e > end) end = e;
    }
    if (end <= base || w <= 16) return;
    uint64_t span = end - base;
    const boot_profile_entry_t *e = &g_boot[index];
    int32_t inner = w - 16;
    int32_t bx = x + 8 + (int32_t)(((e->start_ns - base) * (uint64_t)inner) / span);
    int32_t bw = (int32_t)((e->duration_ns * (uint64_t)inner) / span);
    if (bw < 2) bw = 2;
    ui_draw_fill(c, x + 8, y + h / 2, inner, 1, th->border);
    ui_draw_round(c, bx, y + 8, bw, h - 16, 3, strong ? th->accent : color_alpha(th->accent, 70u));
}

static void paint_table(ui_canvas_t *c, int32_t t, int32_t x, int32_t y, int32_t w, int32_t h)
{
    const theme_t *th = TH();
    uint32_t ncols;
    const column_t *cols = table_cols(t, &ncols);
    int32_t xs[16], ws[16];
    int32_t body_w = w - SCROLLBAR_W - 4;
    column_layout(t, body_w, xs, ws);

    /* header */
    int32_t head_h = t == TBL_PROC ? 52 : 36;
    for (uint32_t i = 0; i < ncols; ++i) {
        int32_t cx = x + xs[i], cw = ws[i];
        if (cx >= x + body_w) break;
        if (cx + cw > x + body_w) cw = x + body_w - cx;
        bool sorted = g_sort_col[t] == (int32_t)i;
        if (hovered(cx, y, cw, head_h))
            ui_draw_fill(c, cx, y, cw, head_h, color_alpha(th->text, THEME_HOVER_PCT));
        if (i > 0u) ui_draw_fill(c, cx, y + 6, 1, head_h - 12, th->border);
        char total[32];
        header_total(t, (int32_t)i, total, sizeof(total));
        float lpx = THEME_FONT_SMALL;
        uint32_t lcol = sorted ? th->accent : th->text_dim;
        if (t == TBL_PROC) {
            if (cols[i].right) {
                if (total[0])
                    text_right(c, cx + cw - 10, y + 2, 26, total, THEME_FONT_LARGE, th->text);
                text_right(c, cx + cw - 10, y + 26, 22, cols[i].title, lpx, lcol);
            } else {
                text_mid(c, cx + 10, y + 26, 22, cols[i].title, lpx, lcol, cw - 20);
            }
        } else if (cols[i].right) {
            text_right(c, cx + cw - 10, y, head_h, cols[i].title, lpx, lcol);
        } else {
            text_mid(c, cx + 10, y, head_h, cols[i].title, lpx, lcol, cw - 20);
        }
        if (sorted) chevron(c, cx + cw / 2, y + 6, !g_sort_desc[t], th->accent);
        hot_add(cx, y, cw, head_h, HOT_HEADER, t, (int32_t)i);
    }
    ui_draw_fill(c, x, y + head_h - 1, w, 1, th->border);

    /* body */
    int32_t by = y + head_h, bh = h - head_h;
    uint32_t rows = table_rows(t);
    int32_t rh = row_height(t, 0);
    g_visible_rows[t] = bh / rh > 0 ? bh / rh : 1;
    clamp_scroll(t);
    g_tbl_geo[t].x = x; g_tbl_geo[t].y = by; g_tbl_geo[t].w = w; g_tbl_geo[t].h = bh;

    int32_t ry = by;
    for (uint32_t r = (uint32_t)g_scroll[t]; r < rows && ry < by + bh; ++r) {
        int32_t index;
        if (t == TBL_PROC) {
            index = g_proc_view[r];
            if (index < 0) { paint_group_row(c, x, ry, body_w, ~index); ry += GROUP_H; continue; }
        } else if (t == TBL_DETAIL) {
            index = g_detail_view[r];
        } else {
            index = g_boot_view[r];
        }

        int32_t key = t == TBL_BOOT ? index : g_procs[index].pid;
        bool selected = t == TBL_BOOT ? key == g_sel_boot : key == g_sel_pid;
        bool hover = hovered(x, ry, body_w, rh);

        for (uint32_t i = 0; i < ncols; ++i) {
            if (!cols[i].heat) continue;
            uint32_t heat = cell_heat(t, (int32_t)i, index);
            ui_draw_fill(c, x + xs[i] + 1, ry, ws[i] - 1, rh - 1, heat_color(heat));
        }
        if (selected)
            ui_draw_fill(c, x, ry, body_w, rh - 1, color_alpha(th->accent, 22u));
        else if (hover)
            ui_draw_fill(c, x, ry, body_w, rh - 1, color_alpha(th->text, THEME_HOVER_PCT));
        if (selected) ui_draw_fill(c, x, ry + 6, 3, rh - 13, th->accent);

        for (uint32_t i = 0; i < ncols; ++i) {
            int32_t cx = x + xs[i], cw = ws[i];
            if (cx >= x + body_w) break;
            if (cx + cw > x + body_w) cw = x + body_w - cx;
            if (t == TBL_BOOT && i == BC_BAR) {
                paint_boot_bar(c, cx, ry, cw, rh, index, selected);
                continue;
            }
            char text[96];
            cell_text(t, (int32_t)i, index, text, sizeof(text));
            float px = THEME_FONT_BODY;
            uint32_t col = th->text;
            if (i == 0u && t == TBL_PROC) {
                const proc_t *p = &g_procs[index];
                ui_draw_icon(c, cx + 28, ry + (rh - 18) / 2, 18,
                               p->group == GROUP_APPS ? UI_ICON_APPS : UI_ICON_SETTINGS,
                               p->group == GROUP_APPS ? th->accent : th->text_dim);
                text_mid(c, cx + 56, ry, rh, text, px, col, cw - 64);
            } else if (cols[i].right) {
                text_right(c, cx + cw - 10, ry, rh, text, px, col);
            } else {
                text_mid(c, cx + 10, ry, rh, text, px,
                         i == 0u ? col : th->text_dim, cw - 20);
            }
        }
        hot_add(x, ry, body_w, rh, HOT_ROW, key, t);
        ry += rh;
    }
    if (rows == 0u) {
        const char *msg = t == TBL_BOOT ? "起動の記録がありません"
                        : g_search[0] ? "一致するプロセスはありません" : "読み込み中…";
        text_mid(c, x + 16, by + 12, 32, msg, THEME_FONT_BODY, th->text_dim, 0);
    }

    /* scrollbar */
    int32_t max_scroll = (int32_t)rows - g_visible_rows[t];
    g_tbl_geo[t].max_scroll = max_scroll > 0 ? max_scroll : 0;
    if (max_scroll > 0) {
        int32_t sx = x + w - SCROLLBAR_W, sy = by + 2, sh = bh - 4;
        int32_t thumb = sh * g_visible_rows[t] / (int32_t)rows;
        if (thumb < 28) thumb = 28;
        int32_t ty0 = sy + (sh - thumb) * g_scroll[t] / max_scroll;
        g_tbl_geo[t].track_y = sy; g_tbl_geo[t].track_h = sh; g_tbl_geo[t].thumb_h = thumb;
        hot_add(sx, sy, SCROLLBAR_W, ty0 - sy, HOT_TRACK, t, -1);
        hot_add(sx, ty0 + thumb, SCROLLBAR_W, sy + sh - ty0 - thumb, HOT_TRACK, t, 1);
        bool hot = g_drag_table == t || hovered(sx - 4, sy, SCROLLBAR_W + 4, sh);
        ui_draw_round(c, sx + (hot ? 1 : 3), ty0, hot ? SCROLLBAR_W - 2 : SCROLLBAR_W - 6,
                        thumb, 4, color_alpha(th->text_dim, hot ? 70u : 40u));
        hot_add(sx - 2, ty0, SCROLLBAR_W + 2, thumb, HOT_THUMB, t, 0);
    }
}

/* ------------------------------------------------------------ chrome */

static void paint_title_bar(ui_canvas_t *c, int32_t w)
{
    const theme_t *th = TH();
    ui_draw_icon(c, 18, 14, 20, UI_ICON_MEMORY, th->accent);
    text_mid(c, 48, 0, TITLE_H, "タスク マネージャー", THEME_FONT_BODY, th->text, 0);

    /* search, centred like Windows 11's */
    int32_t sw = w < 900 ? 280 : 360, sh = 34;
    int32_t sx = (w - sw) / 2, sy = (TITLE_H - sh) / 2;
    if (sx < 220) sx = 220;
    ui_draw_round(c, sx, sy, sw, sh, sh / 2,
                    g_search_focus ? th->accent : th->border);
    ui_draw_round(c, sx + 1, sy + 1, sw - 2, sh - 2, sh / 2 - 1,
                    g_search_focus ? th->surface : th->surface_alt);
    ui_draw_icon(c, sx + 12, sy + 8, 18, UI_ICON_SEARCH, th->text_dim);
    float px = THEME_FONT_BODY;
    if (g_search[0]) {
        text_mid(c, sx + 40, sy, sh, g_search, px, th->text, sw - 80);
    } else {
        text_mid(c, sx + 40, sy, sh, "名前または PID で検索", px, th->text_dim, sw - 60);
    }
    if (g_search_focus) {
        int32_t cx = sx + 40 + ui_text_width(g_search, px);
        if (cx < sx + sw - 40) ui_draw_fill(c, cx + 1, sy + 8, 2, sh - 16, th->accent);
    }
    hot_add(sx, sy, sw, sh, HOT_SEARCH, 0, 0);

    /* update speed */
    char label[48];
    snprintf(label, sizeof(label), "更新の速度: %s", k_speed_name[g_speed]);
    int32_t bw = ui_text_width(label, THEME_FONT_BODY) + 56;
    button(c, w - bw - 12, (TITLE_H - 36) / 2, label,
           g_speed == 3u ? UI_ICON_CLOSE : UI_ICON_REFRESH, BTN_TEXT, true, HOT_SPEED, 0);
}

static void paint_nav(ui_canvas_t *c, int32_t h)
{
    const theme_t *th = TH();
    int32_t y = TITLE_H + 8;
    for (int32_t i = 0; i < PAGE_COUNT; ++i) {
        int32_t ix = 8, iw = NAV_W - 16, ih = 40;
        bool sel = g_page == (page_t)i;
        if (sel) ui_draw_round(c, ix, y, iw, ih, 6, th->accent_soft);
        else if (hovered(ix, y, iw, ih))
            ui_draw_round(c, ix, y, iw, ih, 6, color_alpha(th->text, THEME_HOVER_PCT));
        if (sel) ui_draw_round(c, ix, y + 10, 3, ih - 20, 2, th->accent);
        uint32_t fg = sel ? th->text : th->text_dim;
        ui_draw_icon(c, ix + 14, y + 10, 20, k_page_icon[i], sel ? th->accent : fg);
        text_mid(c, ix + 46, y, ih, k_page_name[i], THEME_FONT_BODY, fg, iw - 50);
        hot_add(ix, y, iw, ih, HOT_NAV, i, 0);
        y += ih + 4;
    }

    /* uptime and counts at the foot of the rail */
    char up[32], line[64];
    fmt_clock(get_uptime_ms() / 1000u, true, up, sizeof(up));
    snprintf(line, sizeof(line), "稼働時間 %s", up);
    ui_draw_text(c, 20, h - 44, line, THEME_FONT_SMALL, th->text_dim, NAV_W - 24);
    snprintf(line, sizeof(line), "プロセス %u  ·  CPU %u", g_proc_count, g_cores);
    ui_draw_text(c, 20, h - 26, line, THEME_FONT_SMALL, th->text_dim, NAV_W - 24);
}

static void page_header(ui_canvas_t *c, int32_t x, int32_t y, int32_t w, const char *title)
{
    (void)w;
    text_mid(c, x + 24, y, PAGE_HEAD_H, title, THEME_FONT_TITLE, TH()->text, 0);
}

/* ------------------------------------------------------------ pages */

static bool can_end(int32_t pid) { return pid > 0 && pid != g_self_pid && find_proc(pid); }

static int32_t end_task_button(ui_canvas_t *c, int32_t right, int32_t y)
{
    const char *label = "タスクを終了する";
    int32_t w = ui_text_width(label, THEME_FONT_BODY) + 56;
    button(c, right - w, y, label, UI_ICON_CLOSE, BTN_TONAL, can_end(g_sel_pid),
           HOT_END_TASK, 0);
    return w;
}

static void paint_processes(ui_canvas_t *c, int32_t x, int32_t y, int32_t w, int32_t h)
{
    page_header(c, x, y, w, "プロセス");
    end_task_button(c, x + w - 20, y + (PAGE_HEAD_H - 36) / 2);
    paint_table(c, TBL_PROC, x + 12, y + PAGE_HEAD_H, w - 24, h - PAGE_HEAD_H - 8);
}

static void paint_details(ui_canvas_t *c, int32_t x, int32_t y, int32_t w, int32_t h)
{
    page_header(c, x, y, w, "詳細");
    end_task_button(c, x + w - 20, y + (PAGE_HEAD_H - 36) / 2);
    paint_table(c, TBL_DETAIL, x + 12, y + PAGE_HEAD_H, w - 24, h - PAGE_HEAD_H - 8);
}

static void paint_boot(ui_canvas_t *c, int32_t x, int32_t y, int32_t w, int32_t h)
{
    page_header(c, x, y, w, "起動の記録");
    uint64_t base = ~0ull, end = 0u;
    for (uint32_t i = 0; i < g_boot_count; ++i) {
        if (g_boot[i].start_ns < base) base = g_boot[i].start_ns;
        uint64_t e = g_boot[i].start_ns + g_boot[i].duration_ns;
        if (e > end) end = e;
    }
    char total[32], line[128];
    fmt_ms(end > base ? end - base : 0u, total, sizeof(total));
    snprintf(line, sizeof(line), "カーネルの起動 %s  ·  %u フェーズ", total, g_boot_count);
    text_right(c, x + w - 24, y, PAGE_HEAD_H, line, THEME_FONT_BODY, TH()->text_dim);
    paint_table(c, TBL_BOOT, x + 12, y + PAGE_HEAD_H, w - 24, h - PAGE_HEAD_H - 8);
}

/* --- performance */

static uint32_t perf_device_count(void) { return PERF_DISK0 + g_disk_count; }

static uint32_t perf_color(uint32_t dev)
{
    const theme_t *th = TH();
    switch (dev) {
    case PERF_CPU:    return th->accent;
    case PERF_MEMORY: return 0xFF9C4DCCu;
    case PERF_GPU:    return 0xFF00897Bu;
    default:          return th->accent;
    }
}

/* GPU history rescaled to per mille of a round full-scale rate. */
static uint64_t gpu_scale(uint32_t *out)
{
    uint64_t peak = 0u;
    for (uint32_t i = 0; i < HISTORY; ++i) if (g_h_disp[i] > peak) peak = g_h_disp[i];
    uint64_t scale = 1024u * 1024u;
    while (scale < peak) scale *= 2u;
    for (uint32_t i = 0; i < HISTORY; ++i)
        out[i] = (uint32_t)((g_h_disp[i] * 1000u) / scale);
    return scale;
}

static void disk_name(uint32_t i, char *out, size_t cap)
{
    snprintf(out, cap, "ディスク %u", i);
}

static void paint_perf_tile(ui_canvas_t *c, int32_t x, int32_t y, int32_t w, uint32_t dev)
{
    const theme_t *th = TH();
    int32_t h = 64;
    bool sel = g_perf_sel == dev;
    if (sel) ui_draw_round(c, x, y, w, h, 6, th->accent_soft);
    else if (hovered(x, y, w, h))
        ui_draw_round(c, x, y, w, h, 6, color_alpha(th->text, THEME_HOVER_PCT));
    uint32_t color = perf_color(dev);
    int32_t gx = x + 8, gy = y + 10, gw = 64, gh = 44;
    char name[48], sub[64], a[24], b[24];
    if (dev == PERF_CPU) {
        graph(c, gx, gy, gw, gh, g_h_cpu, g_hist_n, color, false);
        snprintf(name, sizeof(name), "CPU");
        fmt_pm(g_cpu_now_pm, a, sizeof(a));
        if (g_have_cpu && g_cpu.frequency_mhz)
            snprintf(sub, sizeof(sub), "%s  %u.%02u GHz", a, g_cpu.frequency_mhz / 1000u,
                     (g_cpu.frequency_mhz % 1000u) / 10u);
        else snprintf(sub, sizeof(sub), "%s", a);
    } else if (dev == PERF_MEMORY) {
        graph(c, gx, gy, gw, gh, g_h_mem, g_hist_n, color, false);
        snprintf(name, sizeof(name), "メモリ");
        fmt_bytes(g_mem.used_bytes, a, sizeof(a));
        fmt_bytes(g_mem.total_bytes, b, sizeof(b));
        snprintf(sub, sizeof(sub), "%s / %s (%u%%)", a, b, g_mem.total_bytes
                 ? (unsigned)((g_mem.used_bytes * 100u) / g_mem.total_bytes) : 0u);
    } else if (dev == PERF_GPU) {
        uint32_t scaled[HISTORY];
        gpu_scale(scaled);
        graph(c, gx, gy, gw, gh, scaled, g_hist_n, color, false);
        snprintf(name, sizeof(name), "GPU");
        fmt_rate(g_disp_now, a, sizeof(a));
        snprintf(sub, sizeof(sub), "画面転送 %s", a);
    } else {
        const system_disk_info_t *d = &g_disks[dev - PERF_DISK0];
        ui_draw_fill(c, gx, gy, gw, gh, th->surface);
        ui_draw_icon(c, gx + (gw - 24) / 2, gy + (gh - 24) / 2, 24, UI_ICON_DRIVE, color);
        ui_draw_frame(c, gx, gy, gw, gh, color);
        disk_name(dev - PERF_DISK0, name, sizeof(name));
        fmt_bytes(d->total_bytes, a, sizeof(a));
        snprintf(sub, sizeof(sub), "%s  %s", d->disk_name[0] ? d->disk_name : d->model, a);
    }
    text_mid(c, gx + gw + 12, y + 8, 24, name, THEME_FONT_BODY,
             sel ? th->text : th->text, w - gw - 28);
    text_mid(c, gx + gw + 12, y + 32, 22, sub, THEME_FONT_SMALL, th->text_dim,
             w - gw - 28);
    hot_add(x, y, w, h, HOT_PERF_TILE, (int32_t)dev, 0);
}

static void stat_big(ui_canvas_t *c, int32_t x, int32_t y, const char *label, const char *value)
{
    ui_draw_text(c, x, y, label, THEME_FONT_SMALL, TH()->text_dim, 0);
    ui_draw_text(c, x, y + 16, value, THEME_FONT_TITLE, TH()->text, 0);
}

static void stat_kv(ui_canvas_t *c, int32_t x, int32_t y, int32_t key_w, const char *key,
                    const char *value, int32_t clip)
{
    ui_draw_text(c, x, y, key, THEME_FONT_SMALL, TH()->text_dim, key_w - 8);
    ui_draw_text(c, x + key_w, y, value, THEME_FONT_SMALL, TH()->text, clip - key_w);
}

static void paint_perf_panel(ui_canvas_t *c, int32_t x, int32_t y, int32_t w, int32_t h)
{
    const theme_t *th = TH();
    uint32_t dev = g_perf_sel;
    uint32_t color = perf_color(dev);
    char title[48], model[160];
    model[0] = '\0';
    if (dev == PERF_CPU) {
        snprintf(title, sizeof(title), "CPU");
        if (g_have_cpu) snprintf(model, sizeof(model), "%s", g_cpu.brand);
    } else if (dev == PERF_MEMORY) {
        snprintf(title, sizeof(title), "メモリ");
        fmt_bytes(g_mem.total_bytes, model, sizeof(model));
    } else if (dev == PERF_GPU) {
        snprintf(title, sizeof(title), "GPU");
        if (g_have_gfx) snprintf(model, sizeof(model), "%s", g_gfx.model);
    } else {
        const system_disk_info_t *d = &g_disks[dev - PERF_DISK0];
        disk_name(dev - PERF_DISK0, title, sizeof(title));
        snprintf(model, sizeof(model), "%s %s", d->manufacturer, d->model);
    }
    ui_draw_text(c, x, y + 4, title, THEME_FONT_TITLE, th->text, 0);
    int32_t mw = ui_text_width(model, THEME_FONT_BODY);
    int32_t tw = ui_text_width(title, THEME_FONT_TITLE);
    if (mw > w - tw - 24) mw = w - tw - 24;
    ui_draw_text(c, x + w - mw, y + 12, model, THEME_FONT_BODY, th->text_dim, mw);

    /* CPU: switch between the whole and one graph per logical processor */
    int32_t gy = y + 48;
    if (dev == PERF_CPU && g_cores > 1u) {
        const char *mode = g_cpu_per_core ? "グラフ: 論理プロセッサ" : "グラフ: 全体";
        int32_t bw = ui_text_width(mode, THEME_FONT_BODY) + 32;
        button(c, x + w - bw, y + 40, mode, UI_ICON_NONE, BTN_OUTLINED, true, HOT_CPU_MODE, 0);
        gy += 30;
    }

    int32_t stats_h = 168;
    int32_t graph_h = h - (gy - y) - stats_h - 40;
    if (graph_h < 60) graph_h = 60;
    float cap = THEME_FONT_SMALL;
    uint32_t capc = th->text_dim;
    char left[64], right[48];

    if (dev >= PERF_DISK0) {
        /* No transfer counters and no used/free figures exist (the kernel
         * reports 0 for both), so capacity and attributes are all there is. */
        const system_disk_info_t *d = &g_disks[dev - PERF_DISK0];
        char total[24], sector[24];
        fmt_bytes(d->total_bytes, total, sizeof(total));
        snprintf(sector, sizeof(sector), "%u バイト", d->sector_size);
        int32_t sy = gy + 8;
        stat_big(c, x, sy, "容量", total);
        stat_big(c, x + 180, sy, "種類",
                 (d->flags & SYSTEM_DISK_FLAG_WRITABLE) ? "読み書き" : "読み取り専用");
        int32_t ky = sy + 72;
        stat_kv(c, x, ky, 140, "名前", d->disk_name[0] ? d->disk_name : "-", w);
        stat_kv(c, x, ky + 20, 140, "製造元", d->manufacturer[0] ? d->manufacturer : "-", w);
        stat_kv(c, x, ky + 40, 140, "モデル", d->model[0] ? d->model : "-", w);
        stat_kv(c, x, ky + 60, 140, "セクタ サイズ", sector, w);
        stat_kv(c, x, ky + 80, 140, "起動ディスク",
                (d->flags & SYSTEM_DISK_FLAG_BOOT) ? "はい" : "いいえ", w);
        ui_draw_text(c, x, ky + 120,
                       "使用量と読み書き速度はカーネルが計測していないため表示できません。",
                       THEME_FONT_SMALL, capc, w);
        return;
    }

    /* graph captions */
    uint64_t gpu_full = 0u;
    uint32_t gpu_vals[HISTORY];
    if (dev == PERF_CPU) {
        snprintf(left, sizeof(left), "%% 使用率");
        snprintf(right, sizeof(right), "100%%");
    } else if (dev == PERF_MEMORY) {
        snprintf(left, sizeof(left), "メモリ使用量");
        fmt_bytes(g_mem.total_bytes, right, sizeof(right));
    } else {
        gpu_full = gpu_scale(gpu_vals);
        snprintf(left, sizeof(left), "画面転送（全プロセスの合計）");
        fmt_rate(gpu_full, right, sizeof(right));
    }
    ui_draw_text(c, x, gy, left, cap, capc, 0);
    ui_draw_text(c, x + w - ui_text_width(right, cap), gy, right, cap, capc, 0);
    int32_t grect_y = gy + 20;

    if (dev == PERF_CPU && g_cpu_per_core && g_cores > 1u) {
        uint32_t n = g_cores;
        uint32_t cols = n <= 2u ? n : n <= 4u ? 2u : n <= 9u ? 3u : 4u;
        uint32_t rows = (n + cols - 1u) / cols;
        int32_t gap = 6;
        int32_t cw = (w - gap * (int32_t)(cols - 1u)) / (int32_t)cols;
        int32_t ch = (graph_h - gap * (int32_t)(rows - 1u)) / (int32_t)rows;
        for (uint32_t i = 0; i < n; ++i) {
            int32_t cx = x + (int32_t)(i % cols) * (cw + gap);
            int32_t cy = grect_y + (int32_t)(i / cols) * (ch + gap);
            graph(c, cx, cy, cw, ch, g_h_core[i], g_hist_n, color, true);
            char tag[16];
            snprintf(tag, sizeof(tag), "CPU %u", i);
            ui_draw_text(c, cx + 6, cy + 4, tag, cap, capc, 0);
        }
    } else if (dev == PERF_CPU) {
        graph(c, x, grect_y, w, graph_h, g_h_cpu, g_hist_n, color, true);
    } else if (dev == PERF_MEMORY) {
        graph(c, x, grect_y, w, graph_h, g_h_mem, g_hist_n, color, true);
    } else {
        graph(c, x, grect_y, w, graph_h, gpu_vals, g_hist_n, color, true);
    }
    int32_t foot = grect_y + graph_h + 4;
    char secs[24];
    snprintf(secs, sizeof(secs), "%u 秒", (HISTORY * (k_speed_ms[g_speed] ? k_speed_ms[g_speed] : 1000u)) / 1000u);
    ui_draw_text(c, x, foot, secs, cap, capc, 0);
    ui_draw_text(c, x + w - ui_text_width("0", cap), foot, "0", cap, capc, 0);

    /* statistics: big numbers on the left, a key/value list on the right */
    int32_t sy = foot + 26;
    int32_t kx = x + w / 2 + 20 > x + 400 ? x + w / 2 + 20 : x + 400;
    int32_t kw = x + w - kx;
    char v1[32], v2[32], v3[32], v4[32];
    if (dev == PERF_CPU) {
        fmt_pm(g_cpu_now_pm, v1, sizeof(v1));
        if (g_have_cpu && g_cpu.frequency_mhz)
            snprintf(v2, sizeof(v2), "%u.%02u GHz", g_cpu.frequency_mhz / 1000u,
                     (g_cpu.frequency_mhz % 1000u) / 10u);
        else snprintf(v2, sizeof(v2), "-");
        snprintf(v3, sizeof(v3), "%u", g_proc_count);
        fmt_clock(get_uptime_ms() / 1000u, true, v4, sizeof(v4));
        stat_big(c, x, sy, "使用率", v1);
        stat_big(c, x + 120, sy, "速度", v2);
        stat_big(c, x, sy + 56, "プロセス", v3);
        stat_big(c, x + 120, sy + 56, "稼働時間", v4);

        char kv[48];
        int32_t ky = sy;
        if (g_have_cpu) {
            snprintf(kv, sizeof(kv), "%u", g_cpu.physical_cores);
            stat_kv(c, kx, ky, 140, "コア", kv, kw); ky += 20;
        }
        snprintf(kv, sizeof(kv), "%u", g_cores);
        stat_kv(c, kx, ky, 140, "論理プロセッサ数", kv, kw); ky += 20;
        if (g_have_cpu) {
            stat_kv(c, kx, ky, 140, "ベンダー", g_cpu.vendor, kw); ky += 20;
            snprintf(kv, sizeof(kv), "%u KB / %u KB", g_cpu.cache_l1d_kb, g_cpu.cache_l1i_kb);
            stat_kv(c, kx, ky, 140, "L1 キャッシュ (D/I)", kv, kw); ky += 20;
            snprintf(kv, sizeof(kv), "%u KB", g_cpu.cache_l2_kb);
            stat_kv(c, kx, ky, 140, "L2 キャッシュ", kv, kw); ky += 20;
            if (g_cpu.cache_l3_kb >= 1024u)
                snprintf(kv, sizeof(kv), "%u.%u MB", g_cpu.cache_l3_kb / 1024u,
                         (g_cpu.cache_l3_kb % 1024u) * 10u / 1024u);
            else snprintf(kv, sizeof(kv), "%u KB", g_cpu.cache_l3_kb);
            stat_kv(c, kx, ky, 140, "L3 キャッシュ", kv, kw); ky += 20;
            snprintf(kv, sizeof(kv), "%u / %u / %u", g_cpu.family, g_cpu.model, g_cpu.stepping);
            stat_kv(c, kx, ky, 140, "ファミリ/モデル/ステッピング", kv, kw);
        }
    } else if (dev == PERF_MEMORY) {
        uint64_t avail = g_mem.free_bytes;
        fmt_bytes(g_mem.used_bytes, v1, sizeof(v1));
        fmt_bytes(avail, v2, sizeof(v2));
        stat_big(c, x, sy, "使用中", v1);
        stat_big(c, x + 140, sy, "利用可能", v2);
        fmt_bytes(g_mem.cached_bytes, v3, sizeof(v3));
        fmt_bytes(g_mem.buffers_bytes, v4, sizeof(v4));
        stat_big(c, x, sy + 56, "キャッシュ済み", v3);
        stat_big(c, x + 140, sy + 56, "バッファ", v4);

        /* composition bar: in use / cached / free */
        int32_t bx = x, bw = kx - x - 40, by = sy + 116, bh = 18;
        if (bw > 40 && g_mem.total_bytes) {
            ui_draw_text(c, bx, by - 2, "メモリ構成", cap, capc, 0);
            by += 16;
            ui_draw_fill(c, bx, by, bw, bh, th->surface);
            int32_t uw = (int32_t)((g_mem.used_bytes * (uint64_t)bw) / g_mem.total_bytes);
            int32_t cw = (int32_t)((g_mem.cached_bytes * (uint64_t)bw) / g_mem.total_bytes);
            ui_draw_fill(c, bx, by, uw, bh, color_alpha(color, 55u));
            ui_draw_fill(c, bx + uw, by, cw, bh, color_alpha(color, 25u));
            ui_draw_frame(c, bx, by, bw, bh, color);
        }

        char kv[48];
        fmt_bytes(g_mem.total_bytes, kv, sizeof(kv));
        stat_kv(c, kx, sy, 140, "合計", kv, kw);
        snprintf(kv, sizeof(kv), "%u バイト", g_mem.page_size);
        stat_kv(c, kx, sy + 20, 140, "ページ サイズ", kv, kw);
        uint64_t procs = 0u;
        for (uint32_t i = 0; i < g_proc_count; ++i) procs += g_procs[i].memory;
        fmt_bytes(procs, kv, sizeof(kv));
        stat_kv(c, kx, sy + 40, 140, "プロセスの合計", kv, kw);
        if (g_have_vmem) {
            char a[24], b[24];
            fmt_bytes(g_vmem.mapped_pages * g_vmem.page_size, a, sizeof(a));
            fmt_bytes(g_vmem.total_pages * g_vmem.page_size, b, sizeof(b));
            snprintf(kv, sizeof(kv), "%s / %s", a, b);
            stat_kv(c, kx, sy + 60, 140, "仮想メモリ (マップ済み)", kv, kw);
        }
    } else {
        fmt_rate(g_disp_now, v1, sizeof(v1));
        snprintf(v2, sizeof(v2), "%llu 回/秒", (unsigned long long)g_present_now);
        snprintf(v3, sizeof(v3), "%u", g_presenters);
        stat_big(c, x, sy, "画面転送", v1);
        stat_big(c, x + 160, sy, "画面更新", v2);
        stat_big(c, x, sy + 56, "描画中のプロセス", v3);

        char kv[64];
        if (g_have_gfx) {
            stat_kv(c, kx, sy, 140, "ベンダー", g_gfx.vendor, kw);
            if (g_gfx.vram_mb) snprintf(kv, sizeof(kv), "%u MB", g_gfx.vram_mb);
            else snprintf(kv, sizeof(kv), "-");
            stat_kv(c, kx, sy + 20, 140, "ビデオ メモリ", kv, kw);
            snprintf(kv, sizeof(kv), "%u × %u", g_gfx.display_width, g_gfx.display_height);
            stat_kv(c, kx, sy + 40, 140, "解像度", kv, kw);
            snprintf(kv, sizeof(kv), "%u ビット", g_gfx.bits_per_pixel);
            stat_kv(c, kx, sy + 60, 140, "色深度", kv, kw);
            const char *accel = (g_gfx.acceleration_flags & SYSTEM_GRAPHICS_ACCEL_VIRGL) ? "virgl (3D)"
                              : (g_gfx.acceleration_flags & SYSTEM_GRAPHICS_ACCEL_3D) ? "3D"
                              : "なし（ソフトウェア描画）";
            stat_kv(c, kx, sy + 80, 140, "アクセラレーション", accel, kw);
            snprintf(kv, sizeof(kv), "%04X:%04X", g_gfx.vendor_id, g_gfx.device_id);
            stat_kv(c, kx, sy + 100, 140, "PCI ID", kv, kw);
        } else {
            stat_kv(c, kx, sy, 140, "情報", "取得できませんでした", kw);
        }
    }
}

static void paint_performance(ui_canvas_t *c, int32_t x, int32_t y, int32_t w, int32_t h)
{
    const theme_t *th = TH();
    page_header(c, x, y, w, "パフォーマンス");
    int32_t list_w = 236;
    int32_t ty = y + PAGE_HEAD_H;
    uint32_t n = perf_device_count();
    for (uint32_t i = 0; i < n; ++i) {
        int32_t tile_y = ty + (int32_t)i * 68;
        if (tile_y + 64 > y + h) break;
        paint_perf_tile(c, x + 12, tile_y, list_w - 16, i);
    }
    if (g_disk_state != DISKS_LOADED) {
        int32_t tile_y = ty + (int32_t)n * 68;
        ui_draw_text(c, x + 28, tile_y + 20, "ディスク情報を読み込み中…",
                       THEME_FONT_SMALL, th->text_dim, list_w - 32);
        if (g_disk_state == DISKS_NONE) {
            g_disk_state = DISKS_PENDING;
            g_disk_pending_frame = g_frames;
        }
    }
    ui_draw_fill(c, x + list_w + 4, ty, 1, h - PAGE_HEAD_H - 12, th->border);
    paint_perf_panel(c, x + list_w + 24, ty, w - list_w - 48, h - PAGE_HEAD_H - 12);
}

/* ------------------------------------------------------------ overlays */

typedef struct { const char *label; menu_action_t action; bool check, enabled, rule; } menu_item_t;

static uint32_t menu_items(menu_item_t *items)
{
    const proc_t *p = find_proc(g_menu_pid);
    uint8_t prio = p ? p->priority : 0xFFu;
    uint32_t n = 0u;
    items[n++] = (menu_item_t){ "タスクを終了する", MENU_END, false, can_end(g_menu_pid), false };
    items[n++] = (menu_item_t){ "優先度: 低",   MENU_PRIO_LOW,     prio == 0u, p != NULL, true };
    items[n++] = (menu_item_t){ "優先度: 通常", MENU_PRIO_NORMAL,  prio == 1u, p != NULL, false };
    items[n++] = (menu_item_t){ "優先度: 高",   MENU_PRIO_HIGH,    prio == 2u, p != NULL, false };
    items[n++] = (menu_item_t){ "優先度: 最高", MENU_PRIO_HIGHEST, prio == 3u, p != NULL, false };
    if (g_page == PAGE_PROCESSES)
        items[n++] = (menu_item_t){ "詳細に移動", MENU_GOTO_DETAILS, false, p != NULL, true };
    return n;
}

static void paint_menu(ui_canvas_t *c, int32_t cw, int32_t ch)
{
    const theme_t *th = TH();
    menu_item_t items[MENU_COUNT];
    uint32_t n = menu_items(items);
    int32_t w = 220, ih = 36, h = 8;
    for (uint32_t i = 0; i < n; ++i) h += ih + (items[i].rule ? 9 : 0);
    int32_t x = g_menu_x, y = g_menu_y;
    if (x + w > cw - 4) x = cw - 4 - w;
    if (y + h > ch - 4) y = ch - 4 - h;
    if (x < 4) x = 4;
    if (y < 4) y = 4;

    hot_add(0, 0, cw, ch, HOT_SCRIM, 0, 0);
    ui_draw_round(c, x + 1, y + 3, w, h, 6, color_alpha(0xFF000000u, 18u));
    ui_draw_round(c, x, y, w, h, 6, th->border);
    ui_draw_round(c, x + 1, y + 1, w - 2, h - 2, 5, th->bg);
    int32_t iy = y + 4;
    for (uint32_t i = 0; i < n; ++i) {
        if (items[i].rule) { ui_draw_fill(c, x + 8, iy + 4, w - 16, 1, th->border); iy += 9; }
        bool over = g_pin && g_px >= x && g_px < x + w && g_py >= iy && g_py < iy + ih;
        uint32_t fg = items[i].enabled ? th->text : color_alpha(th->text, THEME_DISABLED_PCT);
        if (over && items[i].enabled)
            ui_draw_round(c, x + 4, iy, w - 8, ih, 4, color_alpha(th->text, THEME_HOVER_PCT));
        if (items[i].check) ui_draw_icon(c, x + 12, iy + 9, 18, UI_ICON_CHECK, th->accent);
        if (items[i].action == MENU_END) ui_draw_icon(c, x + 12, iy + 9, 18, UI_ICON_CLOSE, fg);
        text_mid(c, x + 40, iy, ih, items[i].label, THEME_FONT_BODY, fg, w - 48);
        if (items[i].enabled) hot_add(x + 4, iy, w - 8, ih, HOT_MENU_ITEM, (int32_t)items[i].action, 0);
        iy += ih;
    }
}

static void paint_dialog(ui_canvas_t *c, int32_t cw, int32_t ch)
{
    const theme_t *th = TH();
    hot_add(0, 0, cw, ch, HOT_SCRIM, 0, 0);
    ui_draw_fill(c, 0, 0, cw, ch, color_alpha(0xFF000000u, 32u));
    int32_t w = 440, h = 196;
    int32_t x = (cw - w) / 2, y = (ch - h) / 2;
    ui_draw_round(c, x, y, w, h, THEME_RADIUS_LARGE, th->surface);
    char title[128], body[160];
    snprintf(title, sizeof(title), "%s を終了しますか？", g_dialog_name);
    ui_draw_text(c, x + 24, y + 24, title, THEME_FONT_TITLE, th->text, w - 48);
    snprintf(body, sizeof(body), "PID %d のプロセスを強制終了します。", g_dialog_pid);
    ui_draw_text(c, x + 24, y + 72, body, THEME_FONT_BODY, th->text_dim, w - 48);
    ui_draw_text(c, x + 24, y + 94, "保存されていない作業は失われます。", THEME_FONT_BODY,
                   th->text_dim, w - 48);

    /* Actions right-aligned, the confirming one last. Ending takes the
     * danger colour because it throws work away. */
    const char *ok = "終了する", *cancel = "キャンセル";
    float px = THEME_FONT_BODY;
    int32_t okw = ui_text_width(ok, px) + 48, bh = 40, by = y + h - 24 - bh;
    int32_t okx = x + w - 24 - okw;
    ui_draw_round(c, okx, by, okw, bh, bh / 2, th->danger);
    if (g_pin && g_px >= okx && g_px < okx + okw && g_py >= by && g_py < by + bh)
        ui_draw_round(c, okx, by, okw, bh, bh / 2, color_alpha(th->on_danger, THEME_HOVER_PCT));
    text_mid(c, okx + 24, by, bh, ok, px, th->on_danger, 0);
    hot_add(okx, by, okw, bh, HOT_DLG_OK, 0, 0);
    int32_t cw2 = ui_text_width(cancel, px) + 24, cx = okx - 8 - cw2;
    if (g_pin && g_px >= cx && g_px < cx + cw2 && g_py >= by && g_py < by + bh)
        ui_draw_round(c, cx, by, cw2, bh, bh / 2, color_alpha(th->accent, THEME_HOVER_PCT));
    text_mid(c, cx + 12, by, bh, cancel, px, th->accent, 0);
    hot_add(cx, by, cw2, bh, HOT_DLG_CANCEL, 0, 0);
}

/* ------------------------------------------------------------ paint */

static void on_paint(ui_widget_t *w, ui_canvas_t *c)
{
    (void)w;
    const theme_t *th = TH();
    int32_t cw = c->w, ch = c->h;
    g_hot_count = 0u;
    ++g_frames;

    ui_draw_fill(c, 0, 0, cw, ch, th->bg);
    paint_title_bar(c, cw);
    paint_nav(c, ch);

    /* the page sits on a lighter sheet with a rounded top-left corner */
    int32_t px = NAV_W, py = TITLE_H, pw = cw - NAV_W, ph = ch - TITLE_H;
    ui_draw_round(c, px, py, pw + 16, ph + 16, 10, th->border);
    ui_draw_round(c, px + 1, py + 1, pw + 16, ph + 16, 10, th->surface);

    switch (g_page) {
    case PAGE_PROCESSES:  paint_processes(c, px, py, pw, ph); break;
    case PAGE_PERFORMANCE: paint_performance(c, px, py, pw, ph); break;
    case PAGE_BOOT:       paint_boot(c, px, py, pw, ph); break;
    case PAGE_DETAILS:    paint_details(c, px, py, pw, ph); break;
    default: break;
    }

    if (g_menu_open) paint_menu(c, cw, ch);
    if (g_dialog_open) paint_dialog(c, cw, ch);
}

/* ------------------------------------------------------------ actions */

static int32_t active_table(void)
{
    if (g_page == PAGE_PROCESSES) return TBL_PROC;
    if (g_page == PAGE_DETAILS) return TBL_DETAIL;
    if (g_page == PAGE_BOOT) return TBL_BOOT;
    return -1;
}

static void set_page(page_t page)
{
    g_page = page;
    g_menu_open = false;
}

static void ask_end(int32_t pid)
{
    const proc_t *p = find_proc(pid);
    if (!p || !can_end(pid)) return;
    g_dialog_open = true;
    g_dialog_pid = pid;
    snprintf(g_dialog_name, sizeof(g_dialog_name), "%s", p->label);
}

static void do_end(void)
{
    g_dialog_open = false;
    if (process_kill(g_dialog_pid) < 0) {
        char message[160];
        snprintf(message, sizeof(message), "%s (PID %d) を終了できませんでした。",
                 g_dialog_name, g_dialog_pid);
        window_show_error("タスク マネージャー", message);
    }
    if (g_sel_pid == g_dialog_pid) g_sel_pid = -1;
    sample();
}

static void menu_action(menu_action_t action)
{
    int32_t pid = g_menu_pid;
    g_menu_open = false;
    switch (action) {
    case MENU_END: ask_end(pid); break;
    case MENU_PRIO_LOW: case MENU_PRIO_NORMAL: case MENU_PRIO_HIGH: case MENU_PRIO_HIGHEST:
        if (process_set_priority(pid, (uint32_t)(action - MENU_PRIO_LOW)) < 0)
            window_show_error("タスク マネージャー", "優先度を変更できませんでした。");
        sample();
        break;
    case MENU_GOTO_DETAILS: {
        set_page(PAGE_DETAILS);
        g_sel_pid = pid;
        for (uint32_t i = 0; i < g_detail_view_count; ++i) {
            if (g_procs[g_detail_view[i]].pid != pid) continue;
            g_scroll[TBL_DETAIL] = (int32_t)i - g_visible_rows[TBL_DETAIL] / 2;
            clamp_scroll(TBL_DETAIL);
            break;
        }
        break;
    }
    default: break;
    }
}

static void header_click(int32_t t, int32_t col)
{
    uint32_t n;
    const column_t *cols = table_cols(t, &n);
    if (t == TBL_BOOT && col == BC_BAR) col = BC_START;
    if (g_sort_col[t] == col) {
        /* name: asc -> desc -> unsorted; numbers: desc -> asc -> unsorted */
        bool first_desc = cols[col].right;
        if (g_sort_desc[t] == first_desc) g_sort_desc[t] = !first_desc;
        else g_sort_col[t] = t == TBL_DETAIL ? DC_PID : -1;
    } else {
        g_sort_col[t] = col;
        g_sort_desc[t] = cols[col].right;
    }
    build_views();
}

static void select_row_key(int32_t t, int32_t key)
{
    if (t == TBL_BOOT) g_sel_boot = key;
    else g_sel_pid = key;
}

/* Move the selection by `delta` rows in the current table (group headers
 * are skipped), and bring it into view. */
static void move_selection(int32_t delta, bool absolute)
{
    int32_t t = active_table();
    if (t < 0) return;
    uint32_t rows = table_rows(t);
    if (rows == 0u) return;
    int32_t cur = -1;
    for (uint32_t r = 0; r < rows; ++r) {
        int32_t key = -1;
        if (t == TBL_PROC) { if (g_proc_view[r] >= 0) key = g_procs[g_proc_view[r]].pid; }
        else if (t == TBL_DETAIL) key = g_procs[g_detail_view[r]].pid;
        else key = g_boot_view[r];
        if (key >= 0 && key == (t == TBL_BOOT ? g_sel_boot : g_sel_pid)) { cur = (int32_t)r; break; }
    }
    int32_t next = absolute ? (delta < 0 ? 0 : (int32_t)rows - 1)
                            : (cur < 0 ? 0 : cur + delta);
    if (next < 0) next = 0;
    if (next >= (int32_t)rows) next = (int32_t)rows - 1;
    int32_t dir = delta < 0 ? -1 : 1;
    while (t == TBL_PROC && next >= 0 && next < (int32_t)rows && g_proc_view[next] < 0) next += dir;
    if (next < 0 || next >= (int32_t)rows) return;
    int32_t key = t == TBL_PROC ? g_procs[g_proc_view[next]].pid
                : t == TBL_DETAIL ? g_procs[g_detail_view[next]].pid : g_boot_view[next];
    select_row_key(t, key);
    if (next < g_scroll[t]) g_scroll[t] = next;
    else if (next >= g_scroll[t] + g_visible_rows[t] - 1)
        g_scroll[t] = next - g_visible_rows[t] + 2;
    clamp_scroll(t);
}

static void click(const hot_t *h)
{
    switch (h->kind) {
    case HOT_NAV:       set_page((page_t)h->a); break;
    case HOT_SPEED:
        g_speed = (g_speed + 1u) % 4u;
        g_last_sample_ms = get_uptime_ms();
        break;
    case HOT_HEADER:    header_click(h->a, h->b); break;
    case HOT_GROUP:
        g_group_collapsed[h->a] = !g_group_collapsed[h->a];
        build_views();
        break;
    case HOT_END_TASK:  ask_end(g_sel_pid); break;
    case HOT_CPU_MODE:  g_cpu_per_core = !g_cpu_per_core; break;
    case HOT_PERF_TILE: g_perf_sel = (uint32_t)h->a; break;
    case HOT_MENU_ITEM: menu_action((menu_action_t)h->a); break;
    case HOT_DLG_OK:    do_end(); break;
    case HOT_DLG_CANCEL: g_dialog_open = false; break;
    default: break;
    }
}

static void on_pointer(ui_widget_t *w, const ui_pointer_t *p)
{
    (void)w;
    g_px = p->x; g_py = p->y; g_pin = p->inside;

    /* dragging a scrollbar thumb */
    if (g_drag_table >= 0) {
        int32_t t = g_drag_table;
        if (p->buttons & INPUT_MOUSE_BTN_LEFT) {
            int32_t span = g_tbl_geo[t].track_h - g_tbl_geo[t].thumb_h;
            if (span > 0) {
                g_scroll[t] = g_drag_start_scroll +
                              (p->y - g_drag_start_y) * g_tbl_geo[t].max_scroll / span;
                clamp_scroll(t);
            }
            ui_redraw();
            return;
        }
        g_drag_table = -1;
    }

    if (p->released & INPUT_MOUSE_BTN_LEFT) {
        const hot_t *h = hot_at(p->x, p->y);
        if (g_press.kind != HOT_NONE && h && h->kind == g_press.kind &&
            h->a == g_press.a && h->b == g_press.b)
            click(h);
        g_press.kind = HOT_NONE;
    }

    if (p->pressed & INPUT_MOUSE_BTN_LEFT) {
        const hot_t *h = hot_at(p->x, p->y);
        if (!h || h->kind != HOT_SEARCH) g_search_focus = false;
        if (h) {
            switch (h->kind) {
            case HOT_SEARCH: g_search_focus = true; break;
            case HOT_ROW:    select_row_key(h->b, h->a); break;
            case HOT_THUMB:
                g_drag_table = h->a;
                g_drag_start_y = p->y;
                g_drag_start_scroll = g_scroll[h->a];
                break;
            case HOT_TRACK:
                g_scroll[h->a] += h->b * (g_visible_rows[h->a] - 1);
                clamp_scroll(h->a);
                break;
            case HOT_SCRIM:
                /* a click outside an open menu closes it; a dialog wants
                 * an answer, so the scrim behind it just eats the click */
                g_menu_open = false;
                break;
            default:
                g_press = *h;
                break;
            }
        }
    }

    if (p->pressed & INPUT_MOUSE_BTN_RIGHT) {
        const hot_t *h = hot_at(p->x, p->y);
        if (g_menu_open) {
            g_menu_open = false;
        } else if (h && h->kind == HOT_ROW && h->b != TBL_BOOT) {
            select_row_key(h->b, h->a);
            g_menu_open = true;
            g_menu_pid = h->a;
            g_menu_x = p->x;
            g_menu_y = p->y;
        }
    }

    if (p->wheel != 0 && !g_menu_open && !g_dialog_open) {
        int32_t t = active_table();
        if (t >= 0) {
            g_scroll[t] -= p->wheel * 3;
            clamp_scroll(t);
        } else if (g_page == PAGE_PERFORMANCE) {
            uint32_t n = perf_device_count();
            if (p->wheel < 0 && g_perf_sel + 1u < n) ++g_perf_sel;
            else if (p->wheel > 0 && g_perf_sel > 0u) --g_perf_sel;
        }
    }
    ui_redraw();
}

static void search_changed(void)
{
    g_scroll[TBL_PROC] = 0;
    g_scroll[TBL_DETAIL] = 0;
    build_views();
}

static bool on_key(const input_keyboard_event_t *ev)
{
    bool ctrl = (ev->modifiers & INPUT_KBD_MOD_CTRL) != 0u;
    bool shift = (ev->modifiers & INPUT_KBD_MOD_SHIFT) != 0u;
    ui_redraw();

    if (g_dialog_open) {
        if (ev->ascii == '\r' || ev->ascii == '\n') do_end();
        else if (ev->keycode == KEY_ESC) g_dialog_open = false;
        return true;
    }
    if (g_menu_open) {
        if (ev->keycode == KEY_ESC) g_menu_open = false;
        return true;
    }
    if (ctrl && ev->keycode == KEY_TAB) {
        set_page((page_t)((g_page + (shift ? PAGE_COUNT - 1 : 1)) % PAGE_COUNT));
        return true;
    }
    if (ctrl && (ev->ascii == 'f' || ev->ascii == 'F' || ev->ascii == 6)) {
        g_search_focus = true;
        return true;
    }
    if (ev->keycode == KEY_F5) { sample(); return true; }

    if (g_search_focus) {
        size_t n = strlen(g_search);
        if (ev->keycode == KEY_ESC) {
            if (n) { g_search[0] = '\0'; search_changed(); }
            else g_search_focus = false;
            return true;
        }
        if (ev->ascii == '\r' || ev->ascii == '\n' || ev->keycode == KEY_TAB) {
            g_search_focus = false;
            return true;
        }
        if (ev->ascii == 8 || ev->keycode == 0x0E) {
            /* drop one whole UTF-8 sequence */
            while (n > 0u && ((unsigned char)g_search[n - 1u] & 0xC0u) == 0x80u) --n;
            if (n > 0u) --n;
            g_search[n] = '\0';
            search_changed();
            return true;
        }
        if (ev->ascii >= 32u && ev->ascii < 127u && !ctrl) {
            if (n + 1u < sizeof(g_search)) {
                g_search[n] = (char)ev->ascii;
                g_search[n + 1u] = '\0';
                search_changed();
            }
            return true;
        }
        /* arrows fall through to the table, so typing then moving works */
    }

    if (g_page == PAGE_PERFORMANCE) {
        uint32_t n = perf_device_count();
        if (ev->keycode == KEY_UP && g_perf_sel > 0u) --g_perf_sel;
        else if (ev->keycode == KEY_DOWN && g_perf_sel + 1u < n) ++g_perf_sel;
        else return false;
        return true;
    }

    int32_t t = active_table();
    if (t < 0) return false;
    int32_t page = g_visible_rows[t] - 1 > 1 ? g_visible_rows[t] - 1 : 1;
    switch (ev->keycode) {
    case KEY_UP:       move_selection(-1, false); return true;
    case KEY_DOWN:     move_selection(1, false); return true;
    case KEY_PAGEUP:   move_selection(-page, false); return true;
    case KEY_PAGEDOWN: move_selection(page, false); return true;
    case KEY_HOME:     move_selection(-1, true); return true;
    case KEY_END:      move_selection(1, true); return true;
    case KEY_DELETE:
        if (t != TBL_BOOT) ask_end(g_sel_pid);
        return true;
    default: break;
    }
    /* typing anywhere starts a search, as in Windows 11 */
    if (t != TBL_BOOT && ev->ascii > 32u && ev->ascii < 127u && !ctrl) {
        size_t n = strlen(g_search);
        g_search_focus = true;
        if (n + 1u < sizeof(g_search)) {
            g_search[n] = (char)ev->ascii;
            g_search[n + 1u] = '\0';
            search_changed();
        }
        return true;
    }
    return false;
}

static void on_tick(void)
{
    uint32_t interval = k_speed_ms[g_speed];
    uint64_t now = get_uptime_ms();
    if (interval != 0u && now - g_last_sample_ms >= interval) {
        g_last_sample_ms = now;
        sample();
    }
    /* The loading tile is on screen once a frame has passed since it was
     * first drawn; only then is it worth blocking on the disk probe. */
    if ((g_disk_state == DISKS_PENDING && g_frames > g_disk_pending_frame) ||
        (g_disk_state == DISKS_LOADED && g_page == PAGE_PERFORMANCE &&
         interval != 0u && now - g_disk_ms >= 30000u))
        refresh_disks();
}

/* ------------------------------------------------------------ entry */

int _start(void);
int _start(void)
{
    serial_write_string("[taskmanager] start\n");
    /* Sized to fit a 1280x800 desktop with its title bar and taskbar. */
    g_app = ui_create(1060, 660, "Task Manager");
    if (!g_app) {
        serial_write_string("[taskmanager] no window\n");
        process_exit(1);
        for (;;) {}
    }
    g_self_pid = process_get_current_pid();

    g_canvas = ui_canvas(g_app->root, on_paint, on_pointer, NULL);

    g_app->on_tick = on_tick;
    g_app->on_key = on_key;

    load_static_info();
    sample();
    g_last_sample_ms = get_uptime_ms();
    ui_run();
}

