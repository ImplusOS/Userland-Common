/*
 * ImplusOS Task Manager.
 *
 * A task manager in the shape people already know from Windows: a
 * navigation rail on the left with プロセス / パフォーマンス / 起動の記録 /
 * 詳細, a search box across the top, a process table with column totals,
 * heat-shaded cells, click-to-sort headers and apps that fold their child
 * processes under one row, a performance page with scrolling 60-second
 * graphs for every device, and right-click to end a task or change its
 * priority.
 *
 * Everything shown is something the kernel actually measures:
 *
 *   - CPU      os_get_cpu_usage() (cumulative idle per core) for the
 *              machine, process_perf_info_t.runtime_ns for each process.
 *   - Memory   os_get_memory_info() and each process's memory_usage.
 *   - Disk     os_get_disk_stats(): per-disk operations, bytes, latency and
 *              busy time, counted by the block layer for every request that
 *              reaches the device. "Active time" is busy time over wall
 *              time, as in Windows. Each process's disk rate comes from the
 *              bytes its own requests moved (disk_read/write_bytes).
 *   - GPU      os_get_gpu_usage(): per-engine busy time (3D, copy), timed on
 *              the device by its driver -- or, on a display with no engine,
 *              the kernel's timing of the CPU copies that stand in for one.
 *              The GPU is as busy as its busiest engine. Each process gets
 *              its share of each engine: the kernel charges it the 3D time
 *              of the fences it submitted and the time its presents took
 *              (gpu_3d_ns / gpu_copy_ns), and those shares divide the busy
 *              time the device measured.
 *   - Boot     the kernel's own boot profile (the 19 instrumented phases).
 *
 * Every counter is cumulative, so every rate here is the difference between
 * two samples over the time between them.
 *
 * Threads take process slots in this kernel; a thread's numbers are added
 * to the process it belongs to (process_perf_info_t.owner_pid), so a row is
 * a process, never a thread.
 *
 * The first disk listing makes the kernel probe the transports it did not
 * boot from (~15 s on a fresh boot under QEMU), so the disk list is fetched
 * only when the Performance page is opened -- after it has drawn a
 * "loading" tile -- and then every 30 s. Totals that need no list (the
 * Processes page's disk column header) read SYSTEM_DISK_STATS_ALL, which
 * never waits.
 *
 * The whole window is one UI canvas drawn immediate-mode: every paint lays
 * the page out again and records the clickable rectangles ("hot" regions)
 * it drew, and the pointer handler looks clicks up in that list. Colours
 * come from the desktop theme (Theme.h) turned into the window manager's
 * greys (theme_make_grey), so the window follows light/dark mode and looks
 * like the shell rather than carrying the accent colour.
 *
 * The numbers update once a second (SAMPLE_MS); unlike Windows, the speed
 * is fixed.
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

#define PID_SCAN_MAX     1024       /* OS_CONFIG_PROCESS_MAX_COUNT's ceiling; pids index it */
#define MAX_PROCS        512u
#define HISTORY          60u        /* samples a graph spans */
#define MAX_CORES        OS_CPU_USAGE_MAX_CORES
#define MAX_DISKS        8u
#define MAX_BOOT         32u
#define MAX_HOT          400u
#define GPU_ENGINES      2u         /* SYSTEM_GPU_ENGINE_3D, _COPY */
#define FOLD_DEPTH_MAX   32         /* parent chain walked to find an app */

#define TITLE_H          48
#define NAV_W            196
#define PAGE_HEAD_H      56
#define ROW_H            32
#define GROUP_H          34
#define SCROLLBAR_W      10
#define TILE_H           64
#define SAMPLE_MS        1000u      /* fixed update interval; not user-adjustable */

typedef enum { PAGE_PROCESSES = 0, PAGE_PERFORMANCE, PAGE_BOOT, PAGE_DETAILS, PAGE_COUNT } page_t;
typedef enum { TBL_PROC = 0, TBL_DETAIL, TBL_BOOT, TBL_COUNT } table_id_t;

/* Performance devices in sidebar order, as Windows lists them: CPU,
 * memory, each disk, then the GPU. */
enum { PERF_CPU = 0, PERF_MEMORY, PERF_DISK0 };

typedef enum {
    HOT_NONE = 0,
    HOT_NAV,            /* a = page                         */
    HOT_SEARCH,
    HOT_HEADER,         /* a = table, b = column            */
    HOT_ROW,            /* a = pid (boot: index), b = table */
    HOT_GROUP,          /* a = group                        */
    HOT_EXPAND,         /* a = pid of an app row            */
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
    MENU_TOGGLE,
    MENU_PRIO_LOW, MENU_PRIO_NORMAL, MENU_PRIO_HIGH, MENU_PRIO_HIGHEST,
    MENU_GOTO_DETAILS,
    MENU_COUNT
} menu_action_t;

/* Process table groups, Windows-style: the session's own machinery (init,
 * the window manager, login, notifications) is "ImplusOS プロセス"; what the
 * window manager launched -- the Start menu, the desktop icons -- or anything
 * that has pushed pixels to the display is an app, with every process it
 * started folded under it; the rest works in the background. */
enum { GROUP_APPS = 0, GROUP_BACKGROUND, GROUP_SYSTEM, GROUP_COUNT };

static const char *const k_system_names[] = {
    "Userland", "com.ImplusOS.windowmanager", "com.ImplusOS.loginui",
    "com.ImplusOS.sysnotif",
};

/* ------------------------------------------------------------ data */

/* One task (process or thread) as read this sample. */
typedef struct {
    int32_t  pid, owner, parent;
    uint8_t  state, priority, abi, is_thread;
    char     name[64];
    uint64_t memory, page_faults, runtime_ns;
    uint64_t disk_read, disk_write, gpu_3d, gpu_copy, presents;
    /* since the previous sample */
    uint64_t d_run, d_read, d_write, d_3d, d_copy, d_sys;
} task_t;

/* What the previous sample saw in a pid slot. `tag` tells a slot that was
 * reused in between from the task that held it before. */
typedef struct {
    bool     valid;
    uint32_t tag;
    uint64_t runtime_ns, disk_read, disk_write, gpu_3d, gpu_copy, syscalls;
} prev_t;

/* One process: a task and all of its threads. */
typedef struct {
    int32_t  pid, parent;
    uint8_t  state, priority, abi, group;
    uint16_t threads;
    char     name[64];      /* what the kernel calls it, minus path and .ELF */
    char     label[64];     /* what the Processes page calls it              */
    uint64_t runtime_ns, memory, page_faults, presents;
    uint64_t disk_read, disk_write, gpu_ns;     /* cumulative */
    /* rates over the last interval */
    uint32_t cpu_pm;        /* share of the whole machine, per mille */
    uint32_t gpu_pm;        /* busiest engine's share, per mille     */
    uint8_t  gpu_engine;    /* ... and which engine that was         */
    uint64_t disk_rate, disk_read_rate, disk_write_rate;   /* bytes/s */
    uint64_t syscall_rate;
    /* app folding on the Processes page */
    int32_t  root;          /* index of the app row it folds into, or -1 */
    uint16_t members;       /* an app row: processes in it, itself too   */
    uint32_t sum_cpu_pm, sum_gpu_pm;
    uint64_t sum_memory, sum_disk_rate;
} proc_t;

/* A row of the Processes table. */
enum { VR_GROUP = 0, VR_PROC };
typedef struct { uint8_t kind, depth; int32_t index; } vrow_t;

typedef struct { int32_t x, y, w, h; hot_kind_t kind; int32_t a, b; } hot_t;

typedef struct { const char *title; int32_t width; bool right; uint8_t heat; } column_t;

enum { HEAT_NONE = 0, HEAT_CPU, HEAT_MEM, HEAT_DISK, HEAT_GPU, HEAT_BOOT };

enum { PC_NAME, PC_STATE, PC_CPU, PC_MEM, PC_DISK, PC_GPU, PC_COUNT };
static const column_t k_proc_cols[PC_COUNT] = {
    { "名前",     0,   false, HEAT_NONE },
    { "状態",     88,  false, HEAT_NONE },
    { "CPU",      80,  true,  HEAT_CPU  },
    { "メモリ",   104, true,  HEAT_MEM  },
    { "ディスク", 108, true,  HEAT_DISK },
    { "GPU",      80,  true,  HEAT_GPU  },
};

enum { DC_NAME, DC_PID, DC_STATE, DC_PRIO, DC_THREADS, DC_CPU, DC_TIME, DC_MEM,
       DC_DISK, DC_GPU, DC_ABI, DC_COUNT };
static const column_t k_detail_cols[DC_COUNT] = {
    { "名前",       0,   false, HEAT_NONE },
    { "PID",        56,  true,  HEAT_NONE },
    { "状態",       72,  false, HEAT_NONE },
    { "優先度",     56,  false, HEAT_NONE },
    { "スレッド",   64,  true,  HEAT_NONE },
    { "CPU",        48,  true,  HEAT_CPU  },
    { "CPU 時間",   80,  true,  HEAT_NONE },
    { "メモリ",     88,  true,  HEAT_MEM  },
    { "ディスク",   92,  true,  HEAT_DISK },
    { "GPU",        48,  true,  HEAT_GPU  },
    { "種類",       104, false, HEAT_NONE },
};

enum { BC_NAME, BC_START, BC_DUR, BC_BAR, BC_COUNT };
static const column_t k_boot_cols[BC_COUNT] = {
    { "フェーズ",     220, false, HEAT_NONE },
    { "開始",         96,  true,  HEAT_NONE },
    { "所要時間",     104, true,  HEAT_BOOT },
    { "タイムライン", 0,   false, HEAT_NONE },
};

static const char *const k_page_name[PAGE_COUNT] = {
    "プロセス", "パフォーマンス", "起動の記録", "詳細",
};
static const ui_icon_t k_page_icon[PAGE_COUNT] = {
    UI_ICON_APPS, UI_ICON_MEMORY, UI_ICON_RESTART, UI_ICON_MENU,
};

static const char *const k_engine_name[GPU_ENGINES] = { "3D", "コピー" };


/* First-party processes by their reverse-domain names, so the Processes
 * page can say "ウィンドウ マネージャー" where Details says
 * com.ImplusOS.windowmanager. */
static const struct { const char *name, *label; } k_friendly[] = {
    { "Userland",                       "ImplusOS 初期化プロセス" },
    { "com.ImplusOS.windowmanager",     "ウィンドウ マネージャー" },
    { "com.ImplusOS.sysnotif",          "通知サービス" },
    { "com.ImplusOS.loginui",           "ログイン画面" },
    { "com.ImplusOS.taskmanager",       "タスク マネージャー" },
    { "com.ImplusOS.waylandcompositor", "Wayland コンポジタ" },
    { "com.ImplusOS.linuxapp",          "Linux アプリ ホスト" },
    { "Terminal",                       "ターミナル" },
    { "XdgOpen",                        "xdg-open" },
};

/* ------------------------------------------------------------ state */

static ui_app_t    *g_app;

static task_t   g_tasks[PID_SCAN_MAX];
static uint32_t g_task_count;
static prev_t   g_prev[PID_SCAN_MAX];
static uint64_t g_prev_sample_ns;
static int32_t  g_self_pid;

static proc_t   g_procs[MAX_PROCS];
static uint32_t g_proc_count;
static int16_t  g_index_of[PID_SCAN_MAX];   /* pid -> g_procs index, -1 */
static uint32_t g_live_procs;               /* g_proc_count minus zombies */
static uint32_t g_thread_total;
static bool     g_expanded[PID_SCAN_MAX];   /* app rows opened, by pid */

static vrow_t   g_proc_view[MAX_PROCS + GROUP_COUNT];
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

/* disks: the list, and the rates of each from its counters */
typedef struct {
    uint32_t active_pm;
    uint64_t read_rate, write_rate;     /* bytes/s */
    uint64_t response_ns;               /* mean per request this interval */
    uint64_t iops;
} disk_rate_t;

static system_disk_info_t     g_disks[MAX_DISKS];
static system_disk_stats_t    g_disk_stats[MAX_DISKS], g_disk_prev[MAX_DISKS];
static bool                   g_disk_have_prev[MAX_DISKS];
static disk_rate_t            g_disk_now[MAX_DISKS];
static uint32_t               g_disk_count;
static enum { DISKS_NONE = 0, DISKS_PENDING, DISKS_LOADED } g_disk_state;
static uint32_t               g_disk_pending_frame;
static uint64_t               g_disk_ms;
static system_disk_stats_t    g_disk_all_prev;
static bool                   g_disk_all_have_prev;
static disk_rate_t            g_disk_all;

/* GPU */
static system_gpu_usage_t     g_gpu_usage, g_gpu_prev;
static bool                   g_have_gpu_usage, g_gpu_have_prev;
static uint32_t               g_gpu_now_pm, g_engine_now_pm[GPU_ENGINES];
static uint64_t               g_present_rate, g_present_bytes_rate;
static uint64_t               g_cmd_rate, g_fence_rate;

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
static uint32_t g_h_disk_active[MAX_DISKS][HISTORY];
static uint64_t g_h_disk_read[MAX_DISKS][HISTORY];    /* bytes/s */
static uint64_t g_h_disk_write[MAX_DISKS][HISTORY];   /* bytes/s */
static uint32_t g_h_gpu[HISTORY];
static uint32_t g_h_engine[GPU_ENGINES][HISTORY];
static uint64_t g_h_gpu_mem[HISTORY];                 /* bytes */
static uint32_t g_cpu_now_pm, g_core_now_pm[MAX_CORES];

/* UI */
static page_t   g_page = PAGE_PROCESSES;
static uint32_t g_perf_sel = PERF_CPU;
static bool     g_cpu_per_core;
static uint64_t g_last_sample_ms;
static uint64_t g_last_hist_ms;
static char     g_search[64];
static bool     g_search_focus;
static int32_t  g_sel_pid = -1;
static int32_t  g_sel_boot = -1;
static int32_t  g_sort_col[TBL_COUNT] = { -1, DC_PID, -1 };
static bool     g_sort_desc[TBL_COUNT];
static int32_t  g_scroll[TBL_COUNT];
static int32_t  g_visible_rows[TBL_COUNT] = { 10, 10, 10 };

/* scrollbar geometry from the last paint, for dragging */
static struct { int32_t track_y, track_h, thumb_h, max_scroll; } g_tbl_geo[TBL_COUNT];
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
static bool     g_dialog_tree;        /* end the app with everything under it */
static uint32_t g_dialog_members;
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

/* Whole percent, the way the big numbers and tiles show it. */
static void fmt_pct(uint32_t pm, char *out, size_t cap)
{
    snprintf(out, cap, "%u%%", (pm + 5u) / 10u > 100u ? 100u : (pm + 5u) / 10u);
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

/* Mirrors PROCESS_STATE_* in Kernel/Source/Core/process/ProcessScheduler.h. */
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

static const char *abi_name(uint8_t abi)
{
    switch (abi) {
    case PROCESS_ABI_IMPLUS:      return "ネイティブ";
    case PROCESS_ABI_LINUX:       return "Linux (x86_64)";
    case PROCESS_ABI_LINUX_I386:  return "Linux (i386)";
    case PROCESS_ABI_LINUX_ARM64: return "Linux (AArch64)";
    default:                      return "不明";
    }
}

/* Mirrors io_protocol_type_t in Kernel/Source/Platform/io/IO_Main.h. */
static const char *disk_protocol_name(uint32_t protocol)
{
    switch (protocol) {
    case 1u: return "ATA";
    case 2u: return "SATA (AHCI)";
    case 3u: return "USB";
    case 4u: return "NVMe";
    case 5u: return "VirtIO";
    default: return "不明";
    }
}

/* ------------------------------------------------------------ sampling */

static uint32_t name_tag(const char *s, int32_t parent)
{
    uint32_t h = 2166136261u ^ (uint32_t)parent;
    for (; *s; ++s) h = (h ^ (uint8_t)*s) * 16777619u;
    return h;
}

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

static uint64_t delta(uint64_t now, uint64_t before) { return now >= before ? now - before : 0u; }

static uint64_t per_second(uint64_t amount, uint64_t elapsed_ns)
{
    if (elapsed_ns == 0u) return 0u;
    /* amount * 1e9 / elapsed without overflowing for large amounts */
    if (amount < (UINT64_MAX / 1000000000ull)) return (amount * 1000000000ull) / elapsed_ns;
    return amount / (elapsed_ns / 1000000000ull + 1u);
}

static uint32_t per_mille(uint64_t part, uint64_t whole)
{
    if (whole == 0u) return 0u;
    uint64_t pm = (part * 1000u) / whole;
    return pm > 1000u ? 1000u : (uint32_t)pm;
}

/* From two samples of a disk's counters, what it did in between. */
static void disk_rates(const system_disk_stats_t *now, const system_disk_stats_t *before,
                       disk_rate_t *out)
{
    memset(out, 0, sizeof(*out));
    uint64_t elapsed = delta(now->timestamp_ns, before->timestamp_ns);
    if (elapsed == 0u) return;
    out->active_pm = per_mille(delta(now->busy_ns, before->busy_ns), elapsed);
    out->read_rate = per_second(delta(now->read_bytes, before->read_bytes), elapsed);
    out->write_rate = per_second(delta(now->write_bytes, before->write_bytes), elapsed);
    uint64_t ops = delta(now->read_ops, before->read_ops) +
                   delta(now->write_ops, before->write_ops);
    uint64_t lat = delta(now->read_ns, before->read_ns) +
                   delta(now->write_ns, before->write_ns);
    out->response_ns = ops ? lat / ops : 0u;
    out->iops = per_second(ops, elapsed);
}

static void sample_disks(void)
{
    system_disk_stats_t all;
    if (os_get_disk_stats(SYSTEM_DISK_STATS_ALL, &all) >= 0) {
        if (g_disk_all_have_prev) disk_rates(&all, &g_disk_all_prev, &g_disk_all);
        g_disk_all_prev = all;
        g_disk_all_have_prev = true;
    }
    if (g_disk_state != DISKS_LOADED) return;
    for (uint32_t i = 0; i < g_disk_count; ++i) {
        if (os_get_disk_stats(i, &g_disk_stats[i]) < 0) continue;
        if (g_disk_have_prev[i])
            disk_rates(&g_disk_stats[i], &g_disk_prev[i], &g_disk_now[i]);
        g_disk_prev[i] = g_disk_stats[i];
        g_disk_have_prev[i] = true;
    }
}

static void sample_gpu(void)
{
    system_gpu_usage_t u;
    if (os_get_gpu_usage(&u) < 0) {
        g_have_gpu_usage = false;
        return;
    }
    g_have_gpu_usage = true;
    g_gpu_usage = u;
    if (g_gpu_have_prev) {
        uint64_t elapsed = delta(u.timestamp_ns, g_gpu_prev.timestamp_ns);
        g_gpu_now_pm = 0u;
        for (uint32_t e = 0; e < GPU_ENGINES; ++e) {
            g_engine_now_pm[e] = per_mille(delta(u.engine_busy_ns[e], g_gpu_prev.engine_busy_ns[e]),
                                           elapsed);
            if (u.engine_flags[e] != 0u && g_engine_now_pm[e] > g_gpu_now_pm)
                g_gpu_now_pm = g_engine_now_pm[e];
        }
        g_present_rate = per_second(delta(u.present_count, g_gpu_prev.present_count), elapsed);
        g_present_bytes_rate = per_second(delta(u.present_bytes, g_gpu_prev.present_bytes), elapsed);
        g_cmd_rate = per_second(delta(u.command_bytes, g_gpu_prev.command_bytes), elapsed);
        g_fence_rate = per_second(delta(u.fences_retired, g_gpu_prev.fences_retired), elapsed);
    }
    g_gpu_prev = u;
    g_gpu_have_prev = true;
}

static bool is_system_name(const char *name)
{
    for (size_t k = 0; k < sizeof(k_system_names) / sizeof(k_system_names[0]); ++k)
        if (strcmp(name, k_system_names[k]) == 0) return true;
    return false;
}

/* Nearest ancestor of g_procs[i] that is an app row (root == -2 marks
 * one while folding), or -1. */
static int32_t app_ancestor(uint32_t i)
{
    int32_t parent = g_procs[i].parent;
    for (int depth = 0; depth < FOLD_DEPTH_MAX; ++depth) {
        if (parent <= 0 || parent >= PID_SCAN_MAX) return -1;
        int32_t idx = g_index_of[parent];
        if (idx < 0 || (uint32_t)idx == i) return -1;
        if (g_procs[idx].root == -2) return idx;
        parent = g_procs[idx].parent;
    }
    return -1;
}

static void build_views(void);

static void read_tasks(uint64_t elapsed_ns)
{
    g_task_count = 0u;
    bool seen[PID_SCAN_MAX];
    memset(seen, 0, sizeof(seen));

    for (int32_t pid = 1; pid < PID_SCAN_MAX; ++pid) {
        process_perf_info_t perf;
        if (get_process_perf_info(pid, &perf) < 0) continue;
        if (perf.state == 0u || perf.state == 3u) continue;   /* free / dead */

        task_t *t = &g_tasks[g_task_count++];
        memset(t, 0, sizeof(*t));
        t->pid = pid;
        t->owner = (perf.owner_pid > 0 && perf.owner_pid < PID_SCAN_MAX) ? perf.owner_pid : pid;
        t->parent = perf.parent_pid;
        t->state = perf.state;
        t->priority = perf.priority;
        t->abi = perf.abi_mode;
        t->is_thread = perf.is_thread;
        memcpy(t->name, perf.name, sizeof(t->name));
        t->name[sizeof(t->name) - 1u] = '\0';
        t->memory = perf.memory_usage;
        t->page_faults = perf.page_fault_count;
        t->runtime_ns = perf.runtime_ns;
        t->disk_read = perf.disk_read_bytes;
        t->disk_write = perf.disk_write_bytes;
        t->gpu_3d = perf.gpu_3d_ns;
        t->gpu_copy = perf.gpu_copy_ns;
        t->presents = perf.display_present_calls;

        prev_t *pv = &g_prev[pid];
        uint32_t tag = name_tag(t->name, t->parent);
        if (pv->valid && pv->tag == tag && elapsed_ns > 0u) {
            t->d_run = delta(perf.runtime_ns, pv->runtime_ns);
            t->d_read = delta(perf.disk_read_bytes, pv->disk_read);
            t->d_write = delta(perf.disk_write_bytes, pv->disk_write);
            t->d_3d = delta(perf.gpu_3d_ns, pv->gpu_3d);
            t->d_copy = delta(perf.gpu_copy_ns, pv->gpu_copy);
            t->d_sys = delta(perf.syscalls, pv->syscalls);
        }
        pv->valid = true;
        pv->tag = tag;
        pv->runtime_ns = perf.runtime_ns;
        pv->disk_read = perf.disk_read_bytes;
        pv->disk_write = perf.disk_write_bytes;
        pv->gpu_3d = perf.gpu_3d_ns;
        pv->gpu_copy = perf.gpu_copy_ns;
        pv->syscalls = perf.syscalls;
        seen[pid] = true;
    }
    /* A pid that went away must not lend its counters to the next process
     * that gets the slot. */
    for (int32_t pid = 0; pid < PID_SCAN_MAX; ++pid) {
        if (!seen[pid]) {
            g_prev[pid].valid = false;
            g_expanded[pid] = false;
        }
    }
}

static void add_task(proc_t *p, const task_t *t)
{
    p->runtime_ns += t->runtime_ns;
    p->page_faults += t->page_faults;
    p->presents += t->presents;
    p->disk_read += t->disk_read;
    p->disk_write += t->disk_write;
    p->gpu_ns += t->gpu_3d + t->gpu_copy;
}

static void build_procs(uint64_t elapsed_ns)
{
    /* Per-process interval deltas, summed over its threads. */
    static uint64_t d_run[MAX_PROCS], d_read[MAX_PROCS], d_write[MAX_PROCS];
    static uint64_t d_3d[MAX_PROCS], d_copy[MAX_PROCS], d_sys[MAX_PROCS];

    for (int32_t i = 0; i < PID_SCAN_MAX; ++i) g_index_of[i] = -1;
    g_proc_count = 0u;
    g_thread_total = 0u;
    for (uint32_t k = 0; k < g_task_count; ++k)
        if (g_tasks[k].state != 5u) ++g_thread_total;

    /* processes first, then their threads */
    for (int pass = 0; pass < 2; ++pass) {
        for (uint32_t k = 0; k < g_task_count; ++k) {
            const task_t *t = &g_tasks[k];
            bool leader = !t->is_thread || t->owner == t->pid;
            int32_t into = -1;
            if (pass == 0 && !leader) continue;
            if (pass == 1) {
                if (leader) continue;
                into = g_index_of[t->owner];
            }
            if (into < 0) {
                if (g_proc_count >= MAX_PROCS) continue;
                into = (int32_t)g_proc_count++;
                proc_t *p = &g_procs[into];
                memset(p, 0, sizeof(*p));
                p->pid = t->pid;
                p->parent = t->parent;
                p->state = t->state;
                p->priority = t->priority;
                p->abi = t->abi;
                p->memory = t->memory;      /* the address space's, once */
                p->root = -1;
                derive_names(p, t->name);
                g_index_of[t->pid] = (int16_t)into;
                d_run[into] = d_read[into] = d_write[into] = 0u;
                d_3d[into] = d_copy[into] = d_sys[into] = 0u;
            }
            proc_t *p = &g_procs[into];
            p->threads++;
            add_task(p, t);
            d_run[into] += t->d_run;
            d_read[into] += t->d_read;
            d_write[into] += t->d_write;
            d_3d[into] += t->d_3d;
            d_copy[into] += t->d_copy;
            d_sys[into] += t->d_sys;
        }
    }

    /* The kernel charges a process's GPU time per fence as it retires and
     * per present, which is only as fine as the timer tick: good for who
     * used the engine, coarse for how long. So the per-process figures are
     * used as shares of the engine time the device itself measured, and an
     * app can never be busier than the GPU it ran on. */
    uint64_t sum3d = 0u, sumcopy = 0u;
    for (uint32_t i = 0; i < g_proc_count; ++i) { sum3d += d_3d[i]; sumcopy += d_copy[i]; }
    bool scale3d = g_have_gpu_usage && g_gpu_have_prev &&
                   g_gpu_usage.engine_flags[SYSTEM_GPU_ENGINE_3D] != 0u;
    bool scalecopy = g_have_gpu_usage && g_gpu_have_prev &&
                     g_gpu_usage.engine_flags[SYSTEM_GPU_ENGINE_COPY] != 0u;

    for (uint32_t i = 0; i < g_proc_count; ++i) {
        proc_t *p = &g_procs[i];
        if (elapsed_ns == 0u) continue;
        p->cpu_pm = per_mille(d_run[i], elapsed_ns * g_cores);
        p->disk_read_rate = per_second(d_read[i], elapsed_ns);
        p->disk_write_rate = per_second(d_write[i], elapsed_ns);
        p->disk_rate = p->disk_read_rate + p->disk_write_rate;
        uint32_t pm3d = scale3d
            ? (sum3d ? (uint32_t)((uint64_t)g_engine_now_pm[SYSTEM_GPU_ENGINE_3D] * d_3d[i] / sum3d) : 0u)
            : per_mille(d_3d[i], elapsed_ns);
        uint32_t pmcopy = scalecopy
            ? (sumcopy ? (uint32_t)((uint64_t)g_engine_now_pm[SYSTEM_GPU_ENGINE_COPY] * d_copy[i] / sumcopy) : 0u)
            : per_mille(d_copy[i], elapsed_ns);
        p->gpu_pm = pm3d >= pmcopy ? pm3d : pmcopy;
        p->gpu_engine = pm3d >= pmcopy ? (uint8_t)SYSTEM_GPU_ENGINE_3D
                                       : (uint8_t)SYSTEM_GPU_ENGINE_COPY;
        p->syscall_rate = per_second(d_sys[i], elapsed_ns);
    }

    /* Groups. An app is a process the window manager started, or one that
     * draws without one above it; everything it started folds under it. */
    int32_t wm_pid = -1;
    for (uint32_t i = 0; i < g_proc_count; ++i)
        if (strcmp(g_procs[i].name, "com.ImplusOS.windowmanager") == 0) wm_pid = g_procs[i].pid;
    for (uint32_t i = 0; i < g_proc_count; ++i) {
        proc_t *p = &g_procs[i];
        p->group = (p->pid == 1 || is_system_name(p->name)) ? GROUP_SYSTEM : GROUP_BACKGROUND;
        if (p->group != GROUP_SYSTEM && wm_pid > 0 && p->parent == wm_pid) p->root = -2;
    }
    for (int round = 0; round < 2; ++round) {
        for (uint32_t i = 0; i < g_proc_count; ++i) {
            proc_t *p = &g_procs[i];
            if (p->group == GROUP_SYSTEM || p->root != -1) continue;
            int32_t a = app_ancestor(i);
            if (a >= 0) { p->root = a; p->group = GROUP_APPS; }
        }
        if (round == 1) break;
        /* the second round folds under apps that only drawing revealed */
        for (uint32_t i = 0; i < g_proc_count; ++i) {
            proc_t *p = &g_procs[i];
            if (p->group != GROUP_SYSTEM && p->root == -1 &&
                (p->presents > 0u || p->gpu_ns > 0u))
                p->root = -2;
        }
    }
    for (uint32_t i = 0; i < g_proc_count; ++i) {
        proc_t *p = &g_procs[i];
        if (p->root == -2) { p->root = -1; p->group = GROUP_APPS; }
        p->members = 1u;
        p->sum_cpu_pm = p->cpu_pm;
        p->sum_gpu_pm = p->gpu_pm;
        p->sum_memory = p->memory;
        p->sum_disk_rate = p->disk_rate;
    }
    for (uint32_t i = 0; i < g_proc_count; ++i) {
        proc_t *p = &g_procs[i];
        if (p->root < 0 || p->state == 5u) continue;   /* zombies are not listed */
        proc_t *r = &g_procs[p->root];
        r->members++;
        r->sum_cpu_pm += p->cpu_pm;
        r->sum_gpu_pm += p->gpu_pm;
        r->sum_memory += p->memory;
        r->sum_disk_rate += p->disk_rate;
    }
    g_live_procs = 0u;
    for (uint32_t i = 0; i < g_proc_count; ++i) {
        if (g_procs[i].sum_cpu_pm > 1000u) g_procs[i].sum_cpu_pm = 1000u;
        if (g_procs[i].sum_gpu_pm > 1000u) g_procs[i].sum_gpu_pm = 1000u;
        if (g_procs[i].state != 5u) ++g_live_procs;
    }
}

static void sample(void)
{
    /* Machine CPU: busy = wall - idle, per core. Its clock is the one
     * runtime_ns is kept on, so the per-process shares use it too. */
    system_cpu_usage_t usage;
    bool have_cpu_delta = false;
    uint64_t now_ns = get_uptime_ms() * 1000000ull;
    if (os_get_cpu_usage(&usage) >= 0) {
        uint32_t cores = usage.cpu_count;
        if (cores == 0u) cores = 1u;
        if (cores > MAX_CORES) cores = MAX_CORES;
        g_cores = cores;
        if (usage.timestamp_ns) now_ns = usage.timestamp_ns;
        if (g_cpu_have_prev && usage.wall_ns > g_cpu_prev.wall_ns) {
            uint64_t wall = usage.wall_ns - g_cpu_prev.wall_ns;
            uint64_t busy_total = 0u;
            for (uint32_t i = 0; i < cores; ++i) {
                uint64_t idle = delta(usage.idle_ns[i], g_cpu_prev.idle_ns[i]);
                if (idle > wall) idle = wall;
                uint64_t busy = wall - idle;
                busy_total += busy;
                g_core_now_pm[i] = per_mille(busy, wall);
            }
            g_cpu_now_pm = per_mille(busy_total, wall * cores);
            have_cpu_delta = true;
        }
        g_cpu_prev = usage;
        g_cpu_have_prev = true;
    }

    if (os_get_memory_info(&g_mem) < 0) memset(&g_mem, 0, sizeof(g_mem));
    g_have_vmem = os_get_vmem_info(&g_vmem) >= 0;

    uint64_t elapsed_ns = g_prev_sample_ns && now_ns > g_prev_sample_ns
                              ? now_ns - g_prev_sample_ns : 0u;
    sample_disks();
    sample_gpu();           /* first: the process GPU shares scale to it */
    read_tasks(elapsed_ns);
    build_procs(elapsed_ns);
    g_prev_sample_ns = now_ns;

    /* Histories, one point per SAMPLE_MS. The first sample has no delta yet,
     * so it only seeds; a refresh an action forces in between (ending a
     * task, F5) updates the numbers but adds no point, so a graph always
     * spans the same 60 seconds. */
    uint64_t now_ms = get_uptime_ms();
    if (have_cpu_delta &&
        (g_hist_n == 0u || now_ms - g_last_hist_ms >= SAMPLE_MS * 9u / 10u)) {
        g_last_hist_ms = now_ms;
        history_push32(g_h_cpu, g_cpu_now_pm);
        for (uint32_t i = 0; i < MAX_CORES; ++i)
            history_push32(g_h_core[i], i < g_cores ? g_core_now_pm[i] : 0u);
        history_push32(g_h_mem, per_mille(g_mem.used_bytes, g_mem.total_bytes));
        for (uint32_t i = 0; i < MAX_DISKS; ++i) {
            bool live = g_disk_state == DISKS_LOADED && i < g_disk_count;
            history_push32(g_h_disk_active[i], live ? g_disk_now[i].active_pm : 0u);
            history_push64(g_h_disk_read[i], live ? g_disk_now[i].read_rate : 0u);
            history_push64(g_h_disk_write[i], live ? g_disk_now[i].write_rate : 0u);
        }
        history_push32(g_h_gpu, g_gpu_now_pm);
        for (uint32_t e = 0; e < GPU_ENGINES; ++e)
            history_push32(g_h_engine[e], g_engine_now_pm[e]);
        history_push64(g_h_gpu_mem, g_gpu_usage.shared_memory_bytes);
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
    bool was_gpu = g_perf_sel == PERF_DISK0 + g_disk_count;
    uint32_t old_count = g_disk_count;
    uint32_t disks = 0u, n = 0u;
    system_disk_info_t list[MAX_DISKS];
    if (os_get_disk_count(&disks) >= 0) {
        for (uint32_t i = 0; i < disks && n < MAX_DISKS; ++i)
            if (os_get_disk_info(i, &list[n]) >= 0) ++n;
    }
    /* A different set of disks: the old histories describe other devices. */
    bool changed = n != old_count || g_disk_state != DISKS_LOADED;
    for (uint32_t i = 0; !changed && i < n; ++i)
        changed = strcmp(list[i].disk_name, g_disks[i].disk_name) != 0 ||
                  list[i].total_bytes != g_disks[i].total_bytes;
    memcpy(g_disks, list, sizeof(list[0]) * n);
    g_disk_count = n;
    if (changed) {
        memset(g_disk_have_prev, 0, sizeof(g_disk_have_prev));
        memset(g_disk_now, 0, sizeof(g_disk_now));
        memset(g_h_disk_active, 0, sizeof(g_h_disk_active));
        memset(g_h_disk_read, 0, sizeof(g_h_disk_read));
        memset(g_h_disk_write, 0, sizeof(g_h_disk_write));
    }
    if (was_gpu) g_perf_sel = PERF_DISK0 + g_disk_count;
    else if (g_perf_sel >= PERF_DISK0 + g_disk_count) g_perf_sel = PERF_CPU;
    g_disk_ms = get_uptime_ms();
    g_disk_state = DISKS_LOADED;
    sample_disks();     /* seed, so the next sample has a rate */
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

static int  g_cmp_table;
static bool g_cmp_sums;     /* Processes: compare app rows by their totals */

static int cmp_u64(uint64_t a, uint64_t b) { return a < b ? -1 : (a > b ? 1 : 0); }

static int proc_cmp(const void *va, const void *vb)
{
    const proc_t *a = &g_procs[*(const int32_t *)va];
    const proc_t *b = &g_procs[*(const int32_t *)vb];
    int t = g_cmp_table, col = g_sort_col[t], r = 0;
    if (t == TBL_PROC) {
        bool s = g_cmp_sums;
        switch (col) {
        case PC_NAME:  r = strcmp(a->label, b->label); break;
        case PC_STATE: r = (int)a->state - (int)b->state; break;
        case PC_CPU:   r = cmp_u64(s ? a->sum_cpu_pm : a->cpu_pm, s ? b->sum_cpu_pm : b->cpu_pm); break;
        case PC_MEM:   r = cmp_u64(s ? a->sum_memory : a->memory, s ? b->sum_memory : b->memory); break;
        case PC_DISK:  r = cmp_u64(s ? a->sum_disk_rate : a->disk_rate,
                                   s ? b->sum_disk_rate : b->disk_rate); break;
        case PC_GPU:   r = cmp_u64(s ? a->sum_gpu_pm : a->gpu_pm, s ? b->sum_gpu_pm : b->gpu_pm); break;
        default:       r = strcmp(a->label, b->label); break;
        }
    } else {
        switch (col) {
        case DC_NAME:    r = strcmp(a->name, b->name); break;
        case DC_PID:     r = cmp_u64((uint64_t)a->pid, (uint64_t)b->pid); break;
        case DC_STATE:   r = (int)a->state - (int)b->state; break;
        case DC_PRIO:    r = (int)a->priority - (int)b->priority; break;
        case DC_THREADS: r = (int)a->threads - (int)b->threads; break;
        case DC_CPU:     r = cmp_u64(a->cpu_pm, b->cpu_pm); break;
        case DC_TIME:    r = cmp_u64(a->runtime_ns, b->runtime_ns); break;
        case DC_MEM:     r = cmp_u64(a->memory, b->memory); break;
        case DC_DISK:    r = cmp_u64(a->disk_rate, b->disk_rate); break;
        case DC_GPU:     r = cmp_u64(a->gpu_pm, b->gpu_pm); break;
        case DC_ABI:     r = (int)a->abi - (int)b->abi; break;
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
    static int32_t group_items[GROUP_COUNT][MAX_PROCS];
    static int32_t members[MAX_PROCS];
    static bool    child_hit[MAX_PROCS];
    memset(g_group_count, 0, sizeof(g_group_count));
    memset(child_hit, 0, sizeof(child_hit));
    g_detail_view_count = 0u;

    /* A search that matches a process inside an app shows the app, opened. */
    for (uint32_t i = 0; i < g_proc_count; ++i) {
        const proc_t *p = &g_procs[i];
        if (p->root >= 0 && g_search[0] && matches_search(p)) child_hit[p->root] = true;
    }
    for (uint32_t i = 0; i < g_proc_count; ++i) {
        const proc_t *p = &g_procs[i];
        bool hit = matches_search(p);
        if (hit) g_detail_view[g_detail_view_count++] = (int32_t)i;
        /* a zombie is a pid waiting to be reaped, not a task: Details lists
         * it, the Processes page does not */
        if (p->state == 5u || p->root >= 0) continue;
        if (!hit && !child_hit[i]) continue;
        group_items[p->group][g_group_count[p->group]++] = (int32_t)i;
    }

    g_cmp_table = TBL_PROC;
    g_proc_view_count = 0u;
    for (uint32_t g = 0; g < GROUP_COUNT; ++g) {
        if (g_group_count[g] == 0u) continue;
        g_cmp_sums = true;
        qsort(group_items[g], g_group_count[g], sizeof(int32_t), proc_cmp);
        g_proc_view[g_proc_view_count++] = (vrow_t){ VR_GROUP, 0, (int32_t)g };
        if (g_group_collapsed[g]) continue;
        for (uint32_t k = 0; k < g_group_count[g]; ++k) {
            int32_t idx = group_items[g][k];
            const proc_t *p = &g_procs[idx];
            g_proc_view[g_proc_view_count++] = (vrow_t){ VR_PROC, 0, idx };
            if (p->members <= 1u || !(g_expanded[p->pid] || child_hit[idx])) continue;
            uint32_t n = 0u;
            for (uint32_t j = 0; j < g_proc_count; ++j)
                if (g_procs[j].root == idx && g_procs[j].state != 5u) members[n++] = (int32_t)j;
            g_cmp_sums = false;
            qsort(members, n, sizeof(int32_t), proc_cmp);
            for (uint32_t j = 0; j < n && g_proc_view_count < MAX_PROCS + GROUP_COUNT; ++j)
                g_proc_view[g_proc_view_count++] = (vrow_t){ VR_PROC, 1, members[j] };
        }
    }

    g_cmp_table = TBL_DETAIL;
    g_cmp_sums = false;
    qsort(g_detail_view, g_detail_view_count, sizeof(int32_t), proc_cmp);

    for (uint32_t i = 0; i < g_boot_count; ++i) g_boot_view[i] = (int32_t)i;
    qsort(g_boot_view, g_boot_count, sizeof(int32_t), boot_cmp);
}

static const proc_t *find_proc(int32_t pid)
{
    if (pid <= 0 || pid >= PID_SCAN_MAX || g_index_of[pid] < 0) return NULL;
    return &g_procs[g_index_of[pid]];
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

/* ... and one pointing right, for a closed app row. */
static void chevron_right(ui_canvas_t *c, int32_t cx, int32_t cy, uint32_t col)
{
    ui_draw_line(c, cx - 2, cy - 4, cx + 2, cy, 1, col);
    ui_draw_line(c, cx - 2, cy + 4, cx + 2, cy, 1, col);
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

/* The background and grid of a 60-second graph. */
static void graph_box(ui_canvas_t *c, int32_t x, int32_t y, int32_t w, int32_t h,
                      uint32_t color, bool grid)
{
    ui_draw_fill(c, x, y, w, h, TH()->surface);
    if (!grid) return;
    uint32_t line = color_alpha(color, 14u);
    for (int32_t i = 1; i < 10; ++i)
        ui_draw_fill(c, x, y + h * i / 10, w, 1, line);
    /* vertical lines scroll left with time, one step per sample */
    int32_t step = w / 10;
    int32_t shift = (int32_t)(g_samples_taken % 6u) * w / 60;
    if (step > 0)
        for (int32_t gx = x + w - shift; gx > x; gx -= step)
            ui_draw_fill(c, gx, y, 1, h, line);
}

/* One series: `values` are per mille of the graph's full scale, oldest
 * first; only the newest `valid` are real. `fill`: shade the area under
 * the line (the main series); otherwise the line alone, lighter. */
static void graph_series(ui_canvas_t *c, int32_t x, int32_t y, int32_t w, int32_t h,
                         const uint32_t *values, uint32_t valid, uint32_t color, bool fill)
{
    if (w < 4 || h < 4) return;
    if (valid > HISTORY) valid = HISTORY;
    if (valid < 1u) return;
    uint32_t shade = color_alpha(color, 22u);
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
        if (fill) ui_draw_fill(c, x + px, vy, 1, y + h - vy, shade);
        if (prev_x >= 0)
            ui_draw_line(c, x + prev_x, prev_y, x + px, vy, 1,
                         fill ? color : color_alpha(color, 70u));
        prev_x = px; prev_y = vy;
    }
}

static void graph(ui_canvas_t *c, int32_t x, int32_t y, int32_t w, int32_t h,
                  const uint32_t *values, uint32_t valid, uint32_t color, bool grid)
{
    if (w < 4 || h < 4) return;
    graph_box(c, x, y, w, h, color, grid);
    graph_series(c, x, y, w, h, values, valid, color, true);
    ui_draw_frame(c, x, y, w, h, color);
}

/* The smallest full scale (`floor` doubled as often as needed) that holds
 * every value of `a` and `b` (either may be NULL); both rescaled to per
 * mille of it. */
static uint64_t scale_series(const uint64_t *a, const uint64_t *b, uint64_t floor,
                             uint32_t *out_a, uint32_t *out_b)
{
    uint64_t peak = 0u;
    for (uint32_t i = 0; i < HISTORY; ++i) {
        if (a && a[i] > peak) peak = a[i];
        if (b && b[i] > peak) peak = b[i];
    }
    uint64_t scale = floor ? floor : 1u;
    while (scale < peak && scale < (UINT64_MAX >> 1)) scale *= 2u;
    for (uint32_t i = 0; i < HISTORY; ++i) {
        if (a && out_a) out_a[i] = per_mille(a[i], scale);
        if (b && out_b) out_b[i] = per_mille(b[i], scale);
    }
    return scale;
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

/* What row r of table t shows: a group header (kind VR_GROUP, index =
 * group), a process (index into g_procs) or a boot phase (index into
 * g_boot). */
static vrow_t row_at(int32_t t, uint32_t r)
{
    if (t == TBL_PROC) return g_proc_view[r];
    if (t == TBL_DETAIL) return (vrow_t){ VR_PROC, 0, g_detail_view[r] };
    return (vrow_t){ VR_PROC, 0, g_boot_view[r] };
}

/* The selection key of a row: a pid, or a boot index; -1 for a header. */
static int32_t row_key(int32_t t, vrow_t v)
{
    if (v.kind == VR_GROUP) return -1;
    return t == TBL_BOOT ? v.index : g_procs[v.index].pid;
}

static void clamp_scroll(int32_t t)
{
    int32_t max = (int32_t)table_rows(t) - g_visible_rows[t];
    if (max < 0) max = 0;
    if (g_scroll[t] > max) g_scroll[t] = max;
    if (g_scroll[t] < 0) g_scroll[t] = 0;
}

static void cell_text(int32_t t, int32_t col, vrow_t v, char *out, size_t cap)
{
    out[0] = '\0';
    if (t == TBL_BOOT) {
        const boot_profile_entry_t *e = &g_boot[v.index];
        uint64_t base = g_boot_count ? g_boot[0].start_ns : 0u;
        for (uint32_t i = 0; i < g_boot_count; ++i)
            if (g_boot[i].start_ns < base) base = g_boot[i].start_ns;
        if (col == BC_NAME) snprintf(out, cap, "%s", e->name);
        else if (col == BC_START) fmt_ms(e->start_ns - base, out, cap);
        else if (col == BC_DUR) fmt_ms(e->duration_ns, out, cap);
        return;
    }
    const proc_t *p = &g_procs[v.index];
    if (t == TBL_PROC) {
        /* an app row shows everything folded under it */
        bool top = v.depth == 0;
        switch (col) {
        case PC_NAME:
            if (top && p->members > 1u) snprintf(out, cap, "%s (%u)", p->label, p->members);
            else snprintf(out, cap, "%s", p->label);
            break;
        case PC_STATE: snprintf(out, cap, "%s", p->state == 6u || p->state == 1u || p->state == 2u
                                                ? "" : state_name(p->state)); break;
        case PC_CPU:   fmt_pm(top ? p->sum_cpu_pm : p->cpu_pm, out, cap); break;
        case PC_MEM:   fmt_bytes(top ? p->sum_memory : p->memory, out, cap); break;
        case PC_DISK:  fmt_rate(top ? p->sum_disk_rate : p->disk_rate, out, cap); break;
        case PC_GPU:   fmt_pm(top ? p->sum_gpu_pm : p->gpu_pm, out, cap); break;
        default: break;
        }
        return;
    }
    switch (col) {
    case DC_NAME:    snprintf(out, cap, "%s", p->name); break;
    case DC_PID:     snprintf(out, cap, "%d", p->pid); break;
    case DC_STATE:   snprintf(out, cap, "%s", state_name(p->state)); break;
    case DC_PRIO:    snprintf(out, cap, "%s", priority_name(p->priority)); break;
    case DC_THREADS: snprintf(out, cap, "%u", p->threads); break;
    case DC_CPU: {
        uint32_t pct = (p->cpu_pm + 5u) / 10u;
        snprintf(out, cap, "%02u", pct > 99u ? 99u : pct);
        break;
    }
    case DC_TIME:    fmt_clock(p->runtime_ns / 1000000000ull, false, out, cap); break;
    case DC_MEM:     fmt_bytes(p->memory, out, cap); break;
    case DC_DISK:    fmt_rate(p->disk_rate, out, cap); break;
    case DC_GPU: {
        uint32_t pct = (p->gpu_pm + 5u) / 10u;
        snprintf(out, cap, "%02u", pct > 99u ? 99u : pct);
        break;
    }
    case DC_ABI:     snprintf(out, cap, "%s", abi_name(p->abi)); break;
    default: break;
    }
}

static uint32_t cell_heat(int32_t t, int32_t col, vrow_t v)
{
    uint32_t n;
    const column_t *cols = table_cols(t, &n);
    if (t == TBL_BOOT) {
        uint64_t longest = 1u;
        for (uint32_t i = 0; i < g_boot_count; ++i)
            if (g_boot[i].duration_ns > longest) longest = g_boot[i].duration_ns;
        return per_mille(g_boot[v.index].duration_ns, longest);
    }
    const proc_t *p = &g_procs[v.index];
    bool top = t == TBL_PROC && v.depth == 0;
    switch (cols[col].heat) {
    case HEAT_CPU:  return (top ? p->sum_cpu_pm : p->cpu_pm) * 2u;
    case HEAT_MEM:  return g_mem.total_bytes
                        ? per_mille((top ? p->sum_memory : p->memory) * 4u, g_mem.total_bytes) : 0u;
    case HEAT_DISK: return per_mille(top ? p->sum_disk_rate : p->disk_rate,
                                     20u * 1024u * 1024u);     /* 20 MB/s = full */
    case HEAT_GPU:  return (top ? p->sum_gpu_pm : p->gpu_pm) * 2u;
    default:        return 0u;
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
    switch (col) {
    case PC_CPU:  fmt_pct(g_cpu_now_pm, out, cap); break;
    case PC_MEM:  fmt_pct(per_mille(g_mem.used_bytes, g_mem.total_bytes), out, cap); break;
    case PC_DISK: fmt_pct(g_disk_all.active_pm, out, cap); break;
    case PC_GPU:  if (g_have_gpu_usage) fmt_pct(g_gpu_now_pm, out, cap); break;
    default: break;
    }
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

/* The name cell of a Processes row: the fold chevron of an app, an icon
 * and the label, indented one step for a process inside an app. */
static void paint_proc_name(ui_canvas_t *c, int32_t cx, int32_t ry, int32_t cw, int32_t rh,
                            vrow_t v, const char *text)
{
    const theme_t *th = TH();
    const proc_t *p = &g_procs[v.index];
    int32_t indent = v.depth ? 24 : 0;
    if (v.depth == 0 && p->members > 1u) {
        bool open = g_expanded[p->pid];
        if (hovered(cx + 2, ry + 4, 24, rh - 8))
            ui_draw_round(c, cx + 2, ry + 4, 24, rh - 8, 4, color_alpha(th->text, THEME_HOVER_PCT));
        if (open) chevron(c, cx + 14, ry + rh / 2, false, th->text_dim);
        else chevron_right(c, cx + 14, ry + rh / 2, th->text_dim);
    }
    bool app = p->group == GROUP_APPS;
    ui_draw_icon(c, cx + 28 + indent, ry + (rh - 18) / 2, 18,
                 app ? UI_ICON_APPS : UI_ICON_SETTINGS,
                 app && v.depth == 0 ? th->accent : th->text_dim);
    text_mid(c, cx + 56 + indent, ry, rh, text, THEME_FONT_BODY, th->text, cw - 64 - indent);
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
    int32_t rh = t == TBL_PROC ? ROW_H : ROW_H - 2;
    g_visible_rows[t] = bh / rh > 0 ? bh / rh : 1;
    clamp_scroll(t);

    int32_t ry = by;
    for (uint32_t r = (uint32_t)g_scroll[t]; r < rows && ry < by + bh; ++r) {
        vrow_t v = row_at(t, r);
        if (v.kind == VR_GROUP) {
            paint_group_row(c, x, ry, body_w, v.index);
            ry += GROUP_H;
            continue;
        }

        int32_t key = row_key(t, v);
        bool selected = t == TBL_BOOT ? key == g_sel_boot : key == g_sel_pid;
        bool hover = hovered(x, ry, body_w, rh);

        for (uint32_t i = 0; i < ncols; ++i) {
            if (!cols[i].heat) continue;
            uint32_t heat = cell_heat(t, (int32_t)i, v);
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
                paint_boot_bar(c, cx, ry, cw, rh, v.index, selected);
                continue;
            }
            char text[96];
            cell_text(t, (int32_t)i, v, text, sizeof(text));
            float px = THEME_FONT_BODY;
            if (i == 0u && t == TBL_PROC) {
                paint_proc_name(c, cx, ry, cw, rh, v, text);
            } else if (cols[i].right) {
                text_right(c, cx + cw - 10, ry, rh, text, px, th->text);
            } else {
                text_mid(c, cx + 10, ry, rh, text, px,
                         i == 0u ? th->text : th->text_dim, cw - 20);
            }
        }
        hot_add(x, ry, body_w, rh, HOT_ROW, key, t);
        if (t == TBL_PROC && v.depth == 0 && g_procs[v.index].members > 1u)
            hot_add(x + 2, ry + 4, 24, rh - 8, HOT_EXPAND, key, 0);
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

    /* search, centred like Windows 11's */
    int32_t sw = w < 900 ? 280 : 360, sh = 34;
    int32_t sx = (w - sw) / 2, sy = (TITLE_H - sh) / 2;
    if (sx < 16) sx = 16;
    ui_draw_round(c, sx, sy, sw, sh, sh / 2,
                  g_search_focus ? th->accent : th->border);
    ui_draw_round(c, sx + 1, sy + 1, sw - 2, sh - 2, sh / 2 - 1,
                  g_search_focus ? th->surface : th->surface_alt);

    float px = THEME_FONT_BODY;
    /* text_mid() leaves a 14 px line's ink about 1.5 px above the middle of
     * the band; this puts it on the line the 18 px icon is centred on. */
    int32_t ty = sy + 2;
    int32_t text_x;
    if (g_search[0]) {
        /* typing: an ordinary left-aligned field */
        ui_draw_icon(c, sx + 12, sy + (sh - 18) / 2, 18, UI_ICON_SEARCH, th->text_dim);
        text_x = sx + 40;
        text_mid(c, text_x, ty, sh, g_search, px, th->text, sw - 80);
    } else {
        /* empty: the icon and the hint centred together in the field */
        const char *hint = "名前または PID で検索";
        int32_t group = 18 + 10 + ui_text_width(hint, px);
        int32_t gx = sx + (sw - group) / 2;
        if (gx < sx + 12) gx = sx + 12;
        ui_draw_icon(c, gx, sy + (sh - 18) / 2, 18, UI_ICON_SEARCH, th->text_dim);
        text_x = gx + 28;
        text_mid(c, text_x, ty, sh, hint, px, th->text_dim, sx + sw - 20 - text_x);
    }
    if (g_search_focus) {
        int32_t cx = text_x + ui_text_width(g_search, px);
        if (cx < sx + sw - 20) ui_draw_fill(c, cx + 1, sy + 8, 2, sh - 16, th->accent);
    }
    hot_add(sx, sy, sw, sh, HOT_SEARCH, 0, 0);
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

    /* uptime at the foot of the rail */
    char up[32], line[64];
    fmt_clock(get_uptime_ms() / 1000u, true, up, sizeof(up));
    snprintf(line, sizeof(line), "稼働時間 %s", up);
    ui_draw_text(c, 20, h - 26, line, THEME_FONT_SMALL, th->text_dim, NAV_W - 24);
}

static void page_header(ui_canvas_t *c, int32_t x, int32_t y, const char *title)
{
    text_mid(c, x + 24, y, PAGE_HEAD_H, title, THEME_FONT_TITLE, TH()->text, 0);
}

/* ------------------------------------------------------------ pages */

static bool can_end(int32_t pid)
{
    return pid > 1 && pid != g_self_pid && find_proc(pid) != NULL;
}

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
    page_header(c, x, y, "プロセス");
    end_task_button(c, x + w - 20, y + (PAGE_HEAD_H - 36) / 2);
    paint_table(c, TBL_PROC, x + 12, y + PAGE_HEAD_H, w - 24, h - PAGE_HEAD_H - 8);
}

static void paint_details(ui_canvas_t *c, int32_t x, int32_t y, int32_t w, int32_t h)
{
    page_header(c, x, y, "詳細");
    end_task_button(c, x + w - 20, y + (PAGE_HEAD_H - 36) / 2);
    paint_table(c, TBL_DETAIL, x + 12, y + PAGE_HEAD_H, w - 24, h - PAGE_HEAD_H - 8);
}

static void paint_boot(ui_canvas_t *c, int32_t x, int32_t y, int32_t w, int32_t h)
{
    page_header(c, x, y, "起動の記録");
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

static uint32_t perf_gpu_index(void) { return PERF_DISK0 + g_disk_count; }
static uint32_t perf_device_count(void) { return perf_gpu_index() + 1u; }
static bool     perf_is_disk(uint32_t dev) { return dev >= PERF_DISK0 && dev < perf_gpu_index(); }

static uint32_t perf_color(uint32_t dev)
{
    if (dev == PERF_CPU) return TH()->accent;
    if (dev == PERF_MEMORY) return TH()->accent;
    if (perf_is_disk(dev)) return 0xFF4CAF50u;
    return 0xFF00897Bu;      /* GPU */
}

static void disk_title(uint32_t i, char *out, size_t cap)
{
    if (i < g_disk_count && (g_disks[i].flags & SYSTEM_DISK_FLAG_BOOT))
        snprintf(out, cap, "ディスク %u (起動)", i);
    else
        snprintf(out, cap, "ディスク %u", i);
}

static const char *gpu_model(void)
{
    if (g_have_gfx && g_gfx.model[0] && strcmp(g_gfx.model, "Unknown PCI Device") != 0)
        return g_gfx.model;
    if (g_have_gfx && g_gfx.vendor[0] && strcmp(g_gfx.vendor, "Unknown") != 0)
        return g_gfx.vendor;
    return "フレームバッファ";
}

static void paint_perf_tile(ui_canvas_t *c, int32_t x, int32_t y, int32_t w, uint32_t dev)
{
    const theme_t *th = TH();
    int32_t h = TILE_H;
    bool sel = g_perf_sel == dev;
    if (sel) ui_draw_round(c, x, y, w, h, 6, th->accent_soft);
    else if (hovered(x, y, w, h))
        ui_draw_round(c, x, y, w, h, 6, color_alpha(th->text, THEME_HOVER_PCT));
    uint32_t color = perf_color(dev);
    int32_t gx = x + 8, gy = y + 10, gw = 64, gh = 44;
    char name[48], sub[96], a[24], b[24];
    if (dev == PERF_CPU) {
        graph(c, gx, gy, gw, gh, g_h_cpu, g_hist_n, color, false);
        snprintf(name, sizeof(name), "CPU");
        fmt_pct(g_cpu_now_pm, a, sizeof(a));
        if (g_have_cpu && g_cpu.frequency_mhz)
            snprintf(sub, sizeof(sub), "%s  %u.%02u GHz", a, g_cpu.frequency_mhz / 1000u,
                     (g_cpu.frequency_mhz % 1000u) / 10u);
        else snprintf(sub, sizeof(sub), "%s", a);
    } else if (dev == PERF_MEMORY) {
        graph(c, gx, gy, gw, gh, g_h_mem, g_hist_n, color, false);
        snprintf(name, sizeof(name), "メモリ");
        fmt_bytes(g_mem.used_bytes, a, sizeof(a));
        fmt_bytes(g_mem.total_bytes, b, sizeof(b));
        snprintf(sub, sizeof(sub), "%s / %s (%u%%)", a, b,
                 (per_mille(g_mem.used_bytes, g_mem.total_bytes) + 5u) / 10u);
    } else if (perf_is_disk(dev)) {
        uint32_t i = dev - PERF_DISK0;
        graph(c, gx, gy, gw, gh, g_h_disk_active[i], g_hist_n, color, false);
        disk_title(i, name, sizeof(name));
        fmt_pct(g_disk_now[i].active_pm, a, sizeof(a));
        snprintf(sub, sizeof(sub), "%s  %s", disk_protocol_name(g_disks[i].protocol), a);
    } else {
        graph(c, gx, gy, gw, gh, g_h_gpu, g_hist_n, color, false);
        snprintf(name, sizeof(name), "GPU 0");
        if (g_have_gpu_usage) fmt_pct(g_gpu_now_pm, a, sizeof(a));
        else snprintf(a, sizeof(a), "-");
        snprintf(sub, sizeof(sub), "%s  %s", a, gpu_model());
    }
    text_mid(c, gx + gw + 12, y + 8, 24, name, THEME_FONT_BODY, th->text, w - gw - 28);
    text_mid(c, gx + gw + 12, y + 32, 22, sub, THEME_FONT_SMALL, th->text_dim, w - gw - 28);
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

/* Captions above a graph: what it shows on the left, its full scale on the
 * right. */
static void graph_caption(ui_canvas_t *c, int32_t x, int32_t y, int32_t w,
                          const char *left, const char *right)
{
    float cap = THEME_FONT_SMALL;
    ui_draw_text(c, x, y, left, cap, TH()->text_dim, w - ui_text_width(right, cap) - 12);
    ui_draw_text(c, x + w - ui_text_width(right, cap), y, right, cap, TH()->text_dim, 0);
}

/* "60 秒 ... 0" below a graph. */
static void graph_foot(ui_canvas_t *c, int32_t x, int32_t y, int32_t w)
{
    float cap = THEME_FONT_SMALL;
    char secs[24];
    snprintf(secs, sizeof(secs), "%u 秒", (HISTORY * SAMPLE_MS) / 1000u);
    ui_draw_text(c, x, y, secs, cap, TH()->text_dim, 0);
    ui_draw_text(c, x + w - ui_text_width("0", cap), y, "0", cap, TH()->text_dim, 0);
}

/* The heading of a device panel: its name, and the model on the right. */
static void panel_title(ui_canvas_t *c, int32_t x, int32_t y, int32_t w,
                        const char *title, const char *model)
{
    const theme_t *th = TH();
    ui_draw_text(c, x, y + 4, title, THEME_FONT_TITLE, th->text, 0);
    int32_t mw = ui_text_width(model, THEME_FONT_BODY);
    int32_t tw = ui_text_width(title, THEME_FONT_TITLE);
    if (mw > w - tw - 24) mw = w - tw - 24;
    if (mw > 0) ui_draw_text(c, x + w - mw, y + 12, model, THEME_FONT_BODY, th->text_dim, mw);
}

#define STATS_H 176

/* Left edge of the key/value list beside a panel's big numbers: the middle
 * of the panel, but never over the big numbers (which take ~300 px). */
static int32_t stats_kx(int32_t x, int32_t w)
{
    int32_t kx = x + w / 2 + 20;
    return kx < x + 300 ? x + 300 : kx;
}

static void paint_cpu_panel(ui_canvas_t *c, int32_t x, int32_t y, int32_t w, int32_t h)
{
    uint32_t color = perf_color(PERF_CPU);
    panel_title(c, x, y, w, "CPU", g_have_cpu ? g_cpu.brand : "");

    /* switch between the whole and one graph per logical processor */
    int32_t gy = y + 48;
    if (g_cores > 1u) {
        const char *mode = g_cpu_per_core ? "グラフ: 論理プロセッサ" : "グラフ: 全体";
        int32_t bw = ui_text_width(mode, THEME_FONT_BODY) + 32;
        button(c, x + w - bw, y + 40, mode, UI_ICON_NONE, BTN_OUTLINED, true, HOT_CPU_MODE, 0);
        gy += 30;
    }
    int32_t graph_h = h - (gy - y) - STATS_H - 40;
    if (graph_h < 60) graph_h = 60;
    graph_caption(c, x, gy, w, "% 使用率", "100%");
    int32_t grect_y = gy + 20;
    if (g_cpu_per_core && g_cores > 1u) {
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
            ui_draw_text(c, cx + 6, cy + 4, tag, THEME_FONT_SMALL, TH()->text_dim, 0);
        }
    } else {
        graph(c, x, grect_y, w, graph_h, g_h_cpu, g_hist_n, color, true);
    }
    int32_t foot = grect_y + graph_h + 4;
    graph_foot(c, x, foot, w);

    int32_t sy = foot + 26;
    int32_t kx = stats_kx(x, w);
    int32_t kw = x + w - kx;
    char v1[32], v2[32], v3[32], v4[32], kv[48];
    fmt_pct(g_cpu_now_pm, v1, sizeof(v1));
    if (g_have_cpu && g_cpu.frequency_mhz)
        snprintf(v2, sizeof(v2), "%u.%02u GHz", g_cpu.frequency_mhz / 1000u,
                 (g_cpu.frequency_mhz % 1000u) / 10u);
    else snprintf(v2, sizeof(v2), "-");
    snprintf(v3, sizeof(v3), "%u", g_live_procs);
    snprintf(v4, sizeof(v4), "%u", g_thread_total);
    stat_big(c, x, sy, "使用率", v1);
    stat_big(c, x + 120, sy, "速度", v2);
    stat_big(c, x, sy + 56, "プロセス", v3);
    stat_big(c, x + 120, sy + 56, "スレッド", v4);
    fmt_clock(get_uptime_ms() / 1000u, true, kv, sizeof(kv));
    stat_big(c, x, sy + 112, "稼働時間", kv);

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
}

static void paint_mem_panel(ui_canvas_t *c, int32_t x, int32_t y, int32_t w, int32_t h)
{
    const theme_t *th = TH();
    uint32_t color = perf_color(PERF_MEMORY);
    char model[32], right[32];
    fmt_bytes(g_mem.total_bytes, model, sizeof(model));
    panel_title(c, x, y, w, "メモリ", model);
    int32_t gy = y + 48;
    int32_t graph_h = h - (gy - y) - STATS_H - 40;
    if (graph_h < 60) graph_h = 60;
    fmt_bytes(g_mem.total_bytes, right, sizeof(right));
    graph_caption(c, x, gy, w, "メモリ使用量", right);
    int32_t grect_y = gy + 20;
    graph(c, x, grect_y, w, graph_h, g_h_mem, g_hist_n, color, true);
    int32_t foot = grect_y + graph_h + 4;
    graph_foot(c, x, foot, w);

    int32_t sy = foot + 26;
    int32_t kx = stats_kx(x, w);
    int32_t kw = x + w - kx;
    char v1[32], v2[32], v3[32], v4[32], kv[64];
    fmt_bytes(g_mem.used_bytes, v1, sizeof(v1));
    fmt_bytes(g_mem.free_bytes, v2, sizeof(v2));
    stat_big(c, x, sy, "使用中", v1);
    stat_big(c, x + 140, sy, "利用可能", v2);
    fmt_bytes(g_mem.cached_bytes, v3, sizeof(v3));
    fmt_bytes(g_gpu_usage.shared_memory_bytes, v4, sizeof(v4));
    stat_big(c, x, sy + 56, "キャッシュ済み", v3);
    stat_big(c, x + 140, sy + 56, "GPU バッファ", v4);

    /* composition bar: in use / GPU buffers (part of in use) / free */
    int32_t bx = x, bw = kx - x - 40, by = sy + 116, bh = 18;
    if (bw > 40 && g_mem.total_bytes) {
        ui_draw_text(c, bx, by - 2, "メモリ構成", THEME_FONT_SMALL, th->text_dim, 0);
        by += 16;
        ui_draw_fill(c, bx, by, bw, bh, th->surface);
        int32_t uw = (int32_t)((g_mem.used_bytes * (uint64_t)bw) / g_mem.total_bytes);
        uint64_t gpu = g_gpu_usage.shared_memory_bytes;
        if (gpu > g_mem.used_bytes) gpu = g_mem.used_bytes;
        int32_t gw = (int32_t)((gpu * (uint64_t)bw) / g_mem.total_bytes);
        ui_draw_fill(c, bx, by, uw, bh, color_alpha(color, 55u));
        ui_draw_fill(c, bx + uw - gw, by, gw, bh, color_alpha(perf_color(perf_gpu_index()), 55u));
        ui_draw_frame(c, bx, by, bw, bh, color);
    }

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
        stat_kv(c, kx, sy + 60, 140, "物理ページ (使用/全体)", kv, kw);
    }
}

static void paint_disk_panel(ui_canvas_t *c, int32_t x, int32_t y, int32_t w, int32_t h,
                             uint32_t i)
{
    const theme_t *th = TH();
    uint32_t color = perf_color(PERF_DISK0 + i);
    const system_disk_info_t *d = &g_disks[i];
    const disk_rate_t *r = &g_disk_now[i];
    char title[48], model[160];
    disk_title(i, title, sizeof(title));
    snprintf(model, sizeof(model), "%s", d->model[0] ? d->model : d->disk_name);
    panel_title(c, x, y, w, title, model);

    /* active time on top, transfer rate below, as in Windows */
    int32_t gy = y + 48;
    int32_t avail = h - (gy - y) - STATS_H - 40 - 44;
    if (avail < 120) avail = 120;
    int32_t active_h = avail * 55 / 100, xfer_h = avail - active_h;

    graph_caption(c, x, gy, w, "アクティブな時間", "100%");
    graph(c, x, gy + 20, w, active_h, g_h_disk_active[i], g_hist_n, color, true);
    graph_foot(c, x, gy + 20 + active_h + 4, w);

    int32_t ty = gy + 20 + active_h + 26;
    uint32_t rd[HISTORY], wr[HISTORY];
    uint64_t scale = scale_series(g_h_disk_read[i], g_h_disk_write[i], 128u * 1024u, rd, wr);
    char right[32];
    fmt_rate(scale, right, sizeof(right));
    graph_caption(c, x, ty, w, "ディスク転送速度 (実線: 読み取り / 淡色: 書き込み)", right);
    graph_box(c, x, ty + 20, w, xfer_h, color, true);
    graph_series(c, x, ty + 20, w, xfer_h, wr, g_hist_n, 0xFF2E7D32u, false);
    graph_series(c, x, ty + 20, w, xfer_h, rd, g_hist_n, color, true);
    ui_draw_frame(c, x, ty + 20, w, xfer_h, color);
    int32_t foot = ty + 20 + xfer_h + 4;
    graph_foot(c, x, foot, w);

    int32_t sy = foot + 26;
    int32_t kx = stats_kx(x, w);
    int32_t kw = x + w - kx;
    char v1[32], v2[32], v3[32], v4[32], kv[64];
    fmt_pct(r->active_pm, v1, sizeof(v1));
    fmt_ms(r->response_ns, v2, sizeof(v2));
    fmt_rate(r->read_rate, v3, sizeof(v3));
    fmt_rate(r->write_rate, v4, sizeof(v4));
    stat_big(c, x, sy, "アクティブな時間", v1);
    stat_big(c, x + 160, sy, "平均応答時間", v2);
    stat_big(c, x, sy + 56, "読み取り速度", v3);
    stat_big(c, x + 160, sy + 56, "書き込み速度", v4);
    snprintf(kv, sizeof(kv), "%llu 回/秒", (unsigned long long)r->iops);
    stat_big(c, x, sy + 112, "I/O", kv);

    const system_disk_stats_t *st = &g_disk_stats[i];
    int32_t ky = sy;
    fmt_bytes(d->total_bytes, kv, sizeof(kv));
    stat_kv(c, kx, ky, 140, "容量", kv, kw); ky += 20;
    stat_kv(c, kx, ky, 140, "接続", disk_protocol_name(d->protocol), kw); ky += 20;
    stat_kv(c, kx, ky, 140, "種類",
            (d->flags & SYSTEM_DISK_FLAG_WRITABLE) ? "読み書き" : "読み取り専用", kw); ky += 20;
    stat_kv(c, kx, ky, 140, "起動ディスク",
            (d->flags & SYSTEM_DISK_FLAG_BOOT) ? "はい" : "いいえ", kw); ky += 20;
    snprintf(kv, sizeof(kv), "%u バイト", d->sector_size);
    stat_kv(c, kx, ky, 140, "セクター サイズ", kv, kw); ky += 20;
    char a[24], b[24];
    fmt_bytes(st->read_bytes, a, sizeof(a));
    fmt_bytes(st->write_bytes, b, sizeof(b));
    snprintf(kv, sizeof(kv), "%s / %s", a, b);
    stat_kv(c, kx, ky, 140, "読み取り / 書き込み 合計", kv, kw); ky += 20;
    fmt_count(st->read_ops + st->write_ops, kv, sizeof(kv));
    stat_kv(c, kx, ky, 140, "要求の合計", kv, kw); ky += 20;
    fmt_count(st->errors, kv, sizeof(kv));
    stat_kv(c, kx, ky, 140, "I/O エラー", kv, kw);
    (void)th;
}

static void paint_gpu_panel(ui_canvas_t *c, int32_t x, int32_t y, int32_t w, int32_t h)
{
    const theme_t *th = TH();
    uint32_t color = perf_color(perf_gpu_index());
    panel_title(c, x, y, w, "GPU 0", gpu_model());

    int32_t gy = y + 48;
    if (!g_have_gpu_usage) {
        ui_draw_text(c, x, gy + 8, "このカーネルは GPU 使用率を報告していません。",
                     THEME_FONT_BODY, th->text_dim, w);
        return;
    }
    int32_t avail = h - (gy - y) - STATS_H - 40 - 44;
    if (avail < 120) avail = 120;
    int32_t eng_h = avail * 62 / 100, mem_h = avail - eng_h;

    /* one graph per engine, side by side */
    int32_t gap = 12;
    int32_t ew = (w - gap * (int32_t)(GPU_ENGINES - 1u)) / (int32_t)GPU_ENGINES;
    for (uint32_t e = 0; e < GPU_ENGINES; ++e) {
        int32_t ex = x + (int32_t)e * (ew + gap);
        char left[48], right[16];
        uint32_t flags = g_gpu_usage.engine_flags[e];
        snprintf(left, sizeof(left), "%s%s", k_engine_name[e],
                 (flags & SYSTEM_GPU_ENGINE_FLAG_SOFTWARE) ? " (CPU 転送)" : "");
        if (flags) fmt_pct(g_engine_now_pm[e], right, sizeof(right));
        else snprintf(right, sizeof(right), "なし");
        graph_caption(c, ex, gy, ew, left, right);
        if (flags) {
            graph(c, ex, gy + 20, ew, eng_h, g_h_engine[e], g_hist_n, color, true);
        } else {
            graph_box(c, ex, gy + 20, ew, eng_h, color, false);
            ui_draw_frame(c, ex, gy + 20, ew, eng_h, th->border);
            text_mid(c, ex + 12, gy + 20, eng_h, "このエンジンはありません",
                     THEME_FONT_SMALL, th->text_dim, ew - 24);
        }
    }
    graph_foot(c, x, gy + 20 + eng_h + 4, w);

    /* Shared GPU memory, against half of RAM as Windows scales it. */
    int32_t my = gy + 20 + eng_h + 26;
    uint32_t mem_pm[HISTORY];
    uint64_t full = g_mem.total_bytes / 2u;
    if (full == 0u) full = scale_series(g_h_gpu_mem, NULL, 64u * 1024u * 1024u, NULL, NULL);
    for (uint32_t i = 0; i < HISTORY; ++i) mem_pm[i] = per_mille(g_h_gpu_mem[i], full);
    char right[32];
    fmt_bytes(full, right, sizeof(right));
    graph_caption(c, x, my, w, "共有 GPU メモリ使用量", right);
    graph(c, x, my + 20, w, mem_h, mem_pm, g_hist_n, color, false);

    int32_t sy = my + 20 + mem_h + 26;
    int32_t kx = stats_kx(x, w);
    int32_t kw = x + w - kx;
    char v1[32], v2[32], v3[32], v4[32], kv[64];
    fmt_pct(g_gpu_now_pm, v1, sizeof(v1));
    fmt_bytes(g_gpu_usage.shared_memory_bytes, v2, sizeof(v2));
    fmt_rate(g_present_bytes_rate, v3, sizeof(v3));
    snprintf(v4, sizeof(v4), "%llu 回/秒", (unsigned long long)g_present_rate);
    stat_big(c, x, sy, "使用率", v1);
    stat_big(c, x + 140, sy, "共有 GPU メモリ", v2);
    stat_big(c, x, sy + 56, "画面転送", v3);
    stat_big(c, x + 140, sy + 56, "画面更新", v4);
    if (g_gpu_usage.dedicated_memory_bytes) {
        fmt_bytes(g_gpu_usage.dedicated_memory_bytes, kv, sizeof(kv));
        stat_big(c, x, sy + 112, "専用 GPU メモリ", kv);
    }

    int32_t ky = sy;
    if (g_have_gfx) {
        stat_kv(c, kx, ky, 140, "ベンダー", g_gfx.vendor, kw); ky += 20;
        snprintf(kv, sizeof(kv), "%u × %u", g_gfx.display_width, g_gfx.display_height);
        stat_kv(c, kx, ky, 140, "解像度", kv, kw); ky += 20;
        const char *accel = (g_gfx.acceleration_flags & SYSTEM_GRAPHICS_ACCEL_VIRGL) ? "virgl (3D)"
                          : (g_gfx.acceleration_flags & SYSTEM_GRAPHICS_ACCEL_3D) ? "3D"
                          : "なし (ソフトウェア描画)";
        stat_kv(c, kx, ky, 140, "アクセラレーション", accel, kw); ky += 20;
    }
    fmt_rate(g_cmd_rate, kv, sizeof(kv));
    stat_kv(c, kx, ky, 140, "3D コマンド", kv, kw); ky += 20;
    snprintf(kv, sizeof(kv), "%llu 個/秒", (unsigned long long)g_fence_rate);
    stat_kv(c, kx, ky, 140, "完了したフェンス", kv, kw); ky += 20;
    uint32_t copy_flags = g_gpu_usage.engine_flags[SYSTEM_GPU_ENGINE_COPY];
    stat_kv(c, kx, ky, 140, "計測",
            (copy_flags & SYSTEM_GPU_ENGINE_FLAG_SOFTWARE) ? "画面転送は CPU で計測"
                                                           : "デバイスで計測", kw); ky += 20;
    if (g_have_gfx && (g_gfx.vendor_id || g_gfx.device_id)) {
        snprintf(kv, sizeof(kv), "%04X:%04X", g_gfx.vendor_id, g_gfx.device_id);
        stat_kv(c, kx, ky, 140, "PCI ID", kv, kw);
    }
}

static void paint_performance(ui_canvas_t *c, int32_t x, int32_t y, int32_t w, int32_t h)
{
    const theme_t *th = TH();
    page_header(c, x, y, "パフォーマンス");
    int32_t list_w = 236;
    int32_t ty = y + PAGE_HEAD_H;
    uint32_t n = perf_device_count();
    int32_t slot = 0;
    for (uint32_t i = 0; i < n; ++i) {
        /* while the disks load, their place in the list says so */
        if (i == perf_gpu_index() && g_disk_state != DISKS_LOADED) {
            int32_t tile_y = ty + slot * (TILE_H + 4);
            ui_draw_text(c, x + 28, tile_y + 20, "ディスク情報を読み込み中…",
                         THEME_FONT_SMALL, th->text_dim, list_w - 32);
            ++slot;
            if (g_disk_state == DISKS_NONE) {
                g_disk_state = DISKS_PENDING;
                g_disk_pending_frame = g_frames;
            }
        }
        int32_t tile_y = ty + slot * (TILE_H + 4);
        if (tile_y + TILE_H > y + h) break;
        paint_perf_tile(c, x + 12, tile_y, list_w - 16, i);
        ++slot;
    }
    ui_draw_fill(c, x + list_w + 4, ty, 1, h - PAGE_HEAD_H - 12, th->border);

    int32_t px = x + list_w + 24, pw = w - list_w - 48, ph = h - PAGE_HEAD_H - 12;
    if (g_perf_sel == PERF_CPU) paint_cpu_panel(c, px, ty, pw, ph);
    else if (g_perf_sel == PERF_MEMORY) paint_mem_panel(c, px, ty, pw, ph);
    else if (perf_is_disk(g_perf_sel)) paint_disk_panel(c, px, ty, pw, ph, g_perf_sel - PERF_DISK0);
    else paint_gpu_panel(c, px, ty, pw, ph);
}

/* ------------------------------------------------------------ overlays */

typedef struct { const char *label; menu_action_t action; bool check, enabled, rule; } menu_item_t;

static bool is_app_row(const proc_t *p)
{
    return p && g_page == PAGE_PROCESSES && p->root < 0 && p->members > 1u;
}

static uint32_t menu_items(menu_item_t *items)
{
    const proc_t *p = find_proc(g_menu_pid);
    uint8_t prio = p ? p->priority : 0xFFu;
    uint32_t n = 0u;
    items[n++] = (menu_item_t){ "タスクを終了する", MENU_END, false, can_end(g_menu_pid), false };
    if (is_app_row(p))
        items[n++] = (menu_item_t){ g_expanded[p->pid] ? "折りたたむ" : "展開する",
                                    MENU_TOGGLE, false, true, false };
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
    int32_t w = 460, h = 196;
    int32_t x = (cw - w) / 2, y = (ch - h) / 2;
    ui_draw_round(c, x, y, w, h, THEME_RADIUS_LARGE, th->surface);
    char title[128], body[160];
    snprintf(title, sizeof(title), "%s を終了しますか？", g_dialog_name);
    ui_draw_text(c, x + 24, y + 24, title, THEME_FONT_TITLE, th->text, w - 48);
    if (g_dialog_tree)
        snprintf(body, sizeof(body), "PID %d と、その下の %u 個のプロセスを強制終了します。",
                 g_dialog_pid, g_dialog_members - 1u);
    else
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
    case PAGE_PROCESSES:   paint_processes(c, px, py, pw, ph); break;
    case PAGE_PERFORMANCE: paint_performance(c, px, py, pw, ph); break;
    case PAGE_BOOT:        paint_boot(c, px, py, pw, ph); break;
    case PAGE_DETAILS:     paint_details(c, px, py, pw, ph); break;
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

static void toggle_app(int32_t pid, int open)   /* open: 1, close: 0, flip: -1 */
{
    const proc_t *p = find_proc(pid);
    if (!p || p->root >= 0 || p->members <= 1u) return;
    g_expanded[pid] = open < 0 ? !g_expanded[pid] : open != 0;
    build_views();
}

static void ask_end(int32_t pid)
{
    const proc_t *p = find_proc(pid);
    if (!p || !can_end(pid)) return;
    g_dialog_open = true;
    g_dialog_pid = pid;
    /* On the Processes page an app goes with everything it started, as
     * "End task" does in Windows; Details ends exactly the one process. */
    g_dialog_tree = is_app_row(p);
    g_dialog_members = p->members;
    snprintf(g_dialog_name, sizeof(g_dialog_name), "%s", p->label);
}

static void do_end(void)
{
    g_dialog_open = false;
    int32_t root_index = g_index_of[g_dialog_pid];
    bool failed = false;
    if (g_dialog_tree && root_index >= 0) {
        /* what it started first, so nothing is left to respawn it */
        for (uint32_t i = 0; i < g_proc_count; ++i) {
            const proc_t *p = &g_procs[i];
            if (p->root == root_index && p->pid != g_self_pid && p->pid > 1)
                (void)process_kill(p->pid);
        }
    }
    if (process_kill(g_dialog_pid) < 0) failed = true;
    if (failed) {
        char message[160];
        snprintf(message, sizeof(message), "%s (PID %d) を終了できませんでした。",
                 g_dialog_name, g_dialog_pid);
        window_show_notification("タスク マネージャー", message);
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
    case MENU_TOGGLE: toggle_app(pid, -1); break;
    case MENU_PRIO_LOW: case MENU_PRIO_NORMAL: case MENU_PRIO_HIGH: case MENU_PRIO_HIGHEST:
        if (process_set_priority(pid, (uint32_t)(action - MENU_PRIO_LOW)) < 0)
            window_show_notification("タスク マネージャー", "優先度を変更できませんでした。");
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
static void move_selection(int32_t delta_rows, bool absolute)
{
    int32_t t = active_table();
    if (t < 0) return;
    uint32_t rows = table_rows(t);
    if (rows == 0u) return;
    int32_t want = t == TBL_BOOT ? g_sel_boot : g_sel_pid, cur = -1;
    for (uint32_t r = 0; r < rows; ++r) {
        int32_t key = row_key(t, row_at(t, r));
        if (key >= 0 && key == want) { cur = (int32_t)r; break; }
    }
    int32_t next = absolute ? (delta_rows < 0 ? 0 : (int32_t)rows - 1)
                            : (cur < 0 ? 0 : cur + delta_rows);
    if (next < 0) next = 0;
    if (next >= (int32_t)rows) next = (int32_t)rows - 1;
    int32_t dir = delta_rows < 0 ? -1 : 1;
    while (next >= 0 && next < (int32_t)rows && row_at(t, (uint32_t)next).kind == VR_GROUP)
        next += dir;
    if (next < 0 || next >= (int32_t)rows) return;
    select_row_key(t, row_key(t, row_at(t, (uint32_t)next)));
    if (next < g_scroll[t]) g_scroll[t] = next;
    else if (next >= g_scroll[t] + g_visible_rows[t] - 1)
        g_scroll[t] = next - g_visible_rows[t] + 2;
    clamp_scroll(t);
}

static void click(const hot_t *h)
{
    switch (h->kind) {
    case HOT_NAV:       set_page((page_t)h->a); break;
    case HOT_HEADER:    header_click(h->a, h->b); break;
    case HOT_GROUP:
        g_group_collapsed[h->a] = !g_group_collapsed[h->a];
        build_views();
        break;
    case HOT_EXPAND:    toggle_app(h->a, -1); break;
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
            case HOT_EXPAND:
                select_row_key(TBL_PROC, h->a);
                g_press = *h;
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
        } else if (h && (h->kind == HOT_ROW || h->kind == HOT_EXPAND) &&
                   (h->kind == HOT_EXPAND || h->b != TBL_BOOT)) {
            select_row_key(h->kind == HOT_EXPAND ? TBL_PROC : h->b, h->a);
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

static void search_type(const input_keyboard_event_t *ev)
{
    size_t n = strlen(g_search);
    if (n + 1u < sizeof(g_search)) {
        g_search[n] = (char)ev->ascii;
        g_search[n + 1u] = '\0';
        search_changed();
    }
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
            search_type(ev);
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
    case KEY_RIGHT:    if (t == TBL_PROC) toggle_app(g_sel_pid, 1); return true;
    case KEY_LEFT:     if (t == TBL_PROC) toggle_app(g_sel_pid, 0); return true;
    case KEY_DELETE:
        if (t != TBL_BOOT) ask_end(g_sel_pid);
        return true;
    default: break;
    }
    /* typing anywhere starts a search, as in Windows 11 */
    if (t != TBL_BOOT && ev->ascii > 32u && ev->ascii < 127u && !ctrl) {
        g_search_focus = true;
        search_type(ev);
        return true;
    }
    return false;
}

static void on_tick(void)
{
    uint64_t now = get_uptime_ms();
    if (now - g_last_sample_ms >= SAMPLE_MS) {
        g_last_sample_ms = now;
        sample();
    }
    /* The loading tile is on screen once a frame has passed since it was
     * first drawn; only then is it worth blocking on the disk probe. */
    if ((g_disk_state == DISKS_PENDING && g_frames > g_disk_pending_frame) ||
        (g_disk_state == DISKS_LOADED && g_page == PAGE_PERFORMANCE &&
         now - g_disk_ms >= 30000u))
        refresh_disks();
}

/* ------------------------------------------------------------ entry */

int _start(void);
int _start(void)
{
    serial_write_string("[taskmanager] start\n");
    /* Sized to fit a 1280x800 desktop with its title bar and taskbar. */
    g_app = ui_create(1100, 680, "タスク マネージャー");
    if (!g_app) {
        serial_write_string("[taskmanager] no window\n");
        process_exit(1);
        for (;;) {}
    }
    g_self_pid = process_get_current_pid();

    /* The window manager's greys instead of the accent colour, so the window
     * reads as part of the shell. The end-task confirmation keeps its red:
     * it is the one warning colour here. */
    uint32_t danger = g_app->theme.danger, on_danger = g_app->theme.on_danger;
    theme_make_grey(&g_app->theme);
    g_app->theme.danger = danger;
    g_app->theme.on_danger = on_danger;

    ui_canvas(g_app->root, on_paint, on_pointer, NULL);

    g_app->on_tick = on_tick;
    g_app->on_key = on_key;

    load_static_info();
    sample();
    g_last_sample_ms = get_uptime_ms();
    ui_run();
}
