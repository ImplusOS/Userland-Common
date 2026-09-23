/*
 * ImplusOS settings.
 *
 * Five panes down the left, their contents on the right. The one that
 * changes anything is Appearance: it picks the Material Design 3 seed
 * colour and the light/dark variant the whole desktop is generated from,
 * writes that to the shell's theme file, and tells the window manager to
 * rebuild its scheme from it so the change is visible without a restart.
 *
 * The rest read the system back: what it is running on, how long boot took
 * phase by phase, what is attached, and what storage it has.
 */

#include "ImUI.h"
#include "File.h"
#include "Process.h"
#include "Serial.h"
#include "SystemInfo.h"
#include "Time.h"
#include "Window.h"

#include "../../../Kernel/Source/include/kernel/keycodes.h"

#include <stdio.h>
#include <string.h>

#define THEME_DIR   "/var/System"
#define THEME_FILE  "/var/System/shell.theme"

/* ------------------------------------------------------------- panes */

typedef enum {
    PANE_APPEARANCE = 0,
    PANE_SYSTEM,
    PANE_BOOT,
    PANE_DEVICES,
    PANE_STORAGE,
    PANE_COUNT
} pane_t;

static const char *const k_pane_name[PANE_COUNT] = {
    "外観", "システム", "起動", "デバイス", "ストレージ",
};
static const char *const k_pane_note[PANE_COUNT] = {
    "配色とテーマ", "CPU・メモリ・画面", "起動の各フェーズの計測",
    "接続されている機器", "ディスクと容量",
};
static const imui_icon_t k_pane_icon[PANE_COUNT] = {
    IMUI_ICON_EDIT, IMUI_ICON_HOME, IMUI_ICON_REFRESH,
    IMUI_ICON_DRIVE, IMUI_ICON_FILE,
};

/* The System pane reads a moving target (CPU share), so it is refilled on a
 * timer rather than only when it is opened -- and refilling throws away the
 * scroll offset unless the caller puts it back. */
#define SYSTEM_REFRESH_MS 1000u

/*
 * The seeds offered for the desktop. M3 generates everything else from one
 * of these, so this list is the entire colour choice -- there is no second
 * decision to keep consistent with it. The first is M3's own baseline.
 */
static const struct { const char *name; uint32_t seed; } k_seeds[] = {
    {"バイオレット（標準）", 0xFF6750A4u},
    {"ブルー",               0xFF0B57D0u},
    {"ティール",             0xFF00696Du},
    {"グリーン",             0xFF3F6B2Fu},
    {"オレンジ",             0xFF8B5000u},
    {"レッド",               0xFF9C4146u},
    {"モノクロ",             0xFF808080u},
};
#define SEED_COUNT ((uint32_t)(sizeof(k_seeds) / sizeof(k_seeds[0])))

static imui_app_t    *g_app;
static imui_widget_t *w_nav;
static imui_widget_t *w_content;
static imui_widget_t *w_title;
static imui_widget_t *w_status;
static pane_t         g_pane = PANE_APPEARANCE;
static uint32_t       g_seed = M3_SEED_BASELINE;
static bool           g_dark;

/* CPU share is a rate, and the kernel reports cumulative idle time per
 * core, so the pane needs two samples to show one number. */
static system_cpu_usage_t g_cpu_prev;
static bool               g_cpu_have_prev;
static uint64_t           g_last_system_ms;

/* ------------------------------------------------------------- helpers */

static void format_bytes(uint64_t bytes, char *out, size_t cap)
{
    if (bytes >= 1024ull * 1024ull * 1024ull)
        snprintf(out, cap, "%llu GB", (unsigned long long)(bytes >> 30));
    else if (bytes >= 1024ull * 1024ull)
        snprintf(out, cap, "%llu MB", (unsigned long long)(bytes >> 20));
    else if (bytes >= 1024ull)
        snprintf(out, cap, "%llu KB", (unsigned long long)(bytes >> 10));
    else
        snprintf(out, cap, "%llu B", (unsigned long long)bytes);
}

static void status(const char *text)
{
    imui_set_text(g_app, w_status, text);
    imui_request_paint(g_app);
}

/* ------------------------------------------------------------- theme */

/*
 * The shell reads /var/System/shell.theme before the copy shipped with the
 * image, so writing the seed and mode there is all it takes for the choice
 * to survive into the next session -- and for applications, which read the
 * same file when they start, to come up matching.
 */
static bool write_theme_file(uint32_t seed, bool dark)
{
    char text[512];
    int n = snprintf(text, sizeof(text),
        "; Written by com.ImplusOS.settings.\n"
        "; The desktop's Material Design 3 scheme is generated from `seed`;\n"
        "; every colour role follows from it.\n"
        "\n"
        "[Material]\n"
        "seed            = #%06X\n"
        "mode            = %s\n"
        "type_scale      = 1.0\n"
        "\n"
        "[Metrics]\n"
        "title_height    = 40\n"
        "corner_radius   = 16\n"
        "shadow_size     = 10\n"
        "dock_height     = 48\n",
        (unsigned)(seed & 0x00FFFFFFu), dark ? "dark" : "light");
    if (n <= 0) return false;

    (void)file_mkdir(THEME_DIR);
    int32_t fd = file_creat(THEME_FILE);
    if (fd < 0) fd = file_open(THEME_FILE, 1);
    if (fd < 0) return false;
    /* Truncate to what was just written: without it a shorter theme leaves
     * the tail of the longer one behind, and a stray setting would be
     * parsed as part of this one. file_truncate() means the old file no
     * longer has to be unlinked first -- which used to leave no file at
     * all if the create that followed failed. */
    int64_t written = file_write(fd, text, (uint64_t)n);
    int32_t trimmed = file_truncate(fd, written > 0 ? (uint64_t)written : 0u);
    file_close(fd);
    return written == (int64_t)n && trimmed >= 0;
}

static void apply_theme(void)
{
    bool saved = write_theme_file(g_seed, g_dark);
    /* The shell rebuilds its whole scheme from the seed, so this is visible
     * at once. Applications read the file at startup, so they follow when
     * they are next launched -- which is worth saying rather than leaving
     * the user to wonder. */
    window_set_theme_seed(g_seed, g_dark);
    status(saved ? "テーマを適用しました（起動中のアプリは次回起動時に反映）"
                 : "テーマを適用しました（保存できませんでした）");
}

static void on_pick_seed(imui_app_t *app, imui_widget_t *w, void *user)
{
    (void)app; (void)w;
    g_seed = k_seeds[(uintptr_t)user].seed;
    apply_theme();
}

static void on_pick_mode(imui_app_t *app, imui_widget_t *w, void *user)
{
    (void)app; (void)w;
    g_dark = (uintptr_t)user != 0u;
    apply_theme();
}

static void on_reload_wallpaper(imui_app_t *app, imui_widget_t *w, void *user)
{
    (void)app; (void)w; (void)user;
    window_reload_background();
    status("壁紙を読み込み直しました。");
}

/* ------------------------------------------------------------- panes */

static void fill_appearance(void)
{
    char line[160];
    imui_list_add(w_content, IMUI_ICON_NONE, "配色",
                  "シード色ひとつから、画面全体の配色が生成されます");
    for (uint32_t i = 0; i < SEED_COUNT; ++i) {
        bool current = (k_seeds[i].seed & 0x00FFFFFFu) == (g_seed & 0x00FFFFFFu);
        snprintf(line, sizeof(line), "#%06X%s",
                 (unsigned)(k_seeds[i].seed & 0x00FFFFFFu),
                 current ? "  ·  使用中" : "");
        imui_list_add(w_content, current ? IMUI_ICON_CHECK : IMUI_ICON_NONE,
                      k_seeds[i].name, line);
    }
    imui_list_add(w_content, IMUI_ICON_NONE, "明るさ",
                  g_dark ? "ダーク（選択中）" : "ライト（選択中）");
    imui_list_add(w_content, g_dark ? IMUI_ICON_NONE : IMUI_ICON_CHECK,
                  "ライト", "明るい背景に暗い文字");
    imui_list_add(w_content, g_dark ? IMUI_ICON_CHECK : IMUI_ICON_NONE,
                  "ダーク", "暗い背景に明るい文字");
    imui_list_add(w_content, IMUI_ICON_REFRESH, "壁紙を再読み込み",
                  "デスクトップの背景画像を読み込み直します");
}

/* CPU share is a rate between two samples of the kernel's cumulative idle
 * time, so the pane shows one number only once a second sample exists --
 * until then it says so rather than inventing a figure. */
static void fill_cpu_usage(char *value, size_t cap)
{
    system_cpu_usage_t now;
    if (os_get_cpu_usage(&now) < 0) {
        snprintf(value, cap, "取得できませんでした");
        return;
    }
    if (!g_cpu_have_prev || now.wall_ns <= g_cpu_prev.wall_ns) {
        snprintf(value, cap, "計測中…（1 秒後に更新されます）");
        g_cpu_prev = now;
        g_cpu_have_prev = true;
        return;
    }

    uint64_t wall = now.wall_ns - g_cpu_prev.wall_ns;
    uint32_t cores = now.cpu_count;
    if (cores > OS_CPU_USAGE_MAX_CORES) cores = OS_CPU_USAGE_MAX_CORES;

    char per_core[96];
    size_t off = 0u;
    uint64_t busy_total = 0u;
    for (uint32_t i = 0; i < cores; ++i) {
        uint64_t idle = now.idle_ns[i] >= g_cpu_prev.idle_ns[i]
                            ? now.idle_ns[i] - g_cpu_prev.idle_ns[i] : 0u;
        if (idle > wall) idle = wall;
        uint64_t busy = wall - idle;
        busy_total += busy;
        int n = snprintf(per_core + off, sizeof(per_core) - off, "%s%u%%",
                         (i == 0u) ? "" : " ", (unsigned)((busy * 100ull) / wall));
        if (n <= 0 || (size_t)n >= sizeof(per_core) - off) break;
        off += (size_t)n;
    }

    unsigned overall = (cores > 0u)
        ? (unsigned)((busy_total * 100ull) / (wall * (uint64_t)cores)) : 0u;
    snprintf(value, cap, "全体 %u%%  ·  コア毎 %s", overall,
             cores > 0u ? per_core : "-");
    g_cpu_prev = now;
    g_cpu_have_prev = true;
}

/* The RTC runs whether or not anything is looking at it, so this row is
 * only interesting because the pane above it is refreshed on a timer. */
static void fill_date_time(char *value, size_t cap)
{
    rtc_time_t rtc;
    if (sys_get_rtc_time(&rtc) < 0) {
        snprintf(value, cap, "取得できませんでした");
        return;
    }
    snprintf(value, cap, "%04u-%02u-%02u  %02u:%02u:%02u",
             (unsigned)rtc.year, (unsigned)rtc.month, (unsigned)rtc.day,
             (unsigned)rtc.hour, (unsigned)rtc.minute, (unsigned)rtc.second);
}

static void fill_system(void)
{
    char value[160];

    system_cpu_info_t cpu;
    if (os_get_cpu_info(&cpu) >= 0) {
        snprintf(value, sizeof(value), "%s  ·  %u コア / %u スレッド  ·  %u MHz",
                 cpu.brand[0] ? cpu.brand : cpu.vendor,
                 cpu.physical_cores, cpu.logical_cores, cpu.frequency_mhz);
        imui_list_add(w_content, IMUI_ICON_NONE, "プロセッサ", value);
    }

    system_arch_info_t arch;
    if (os_get_arch_info(&arch) >= 0) {
        snprintf(value, sizeof(value), "%s  ·  %u bit", arch.name, arch.bits);
        imui_list_add(w_content, IMUI_ICON_NONE, "アーキテクチャ", value);
    }

    system_memory_info_t memory;
    if (os_get_memory_info(&memory) >= 0) {
        char used[24], total[24], freed[24];
        format_bytes(memory.used_bytes, used, sizeof(used));
        format_bytes(memory.total_bytes, total, sizeof(total));
        format_bytes(memory.free_bytes, freed, sizeof(freed));
        snprintf(value, sizeof(value), "使用 %s / 全体 %s  ·  空き %s",
                 used, total, freed);
        imui_list_add(w_content, IMUI_ICON_NONE, "メモリ", value);
    }

    system_graphics_info_t graphics;
    if (os_get_graphics_info(&graphics) >= 0) {
        snprintf(value, sizeof(value), "%s %s  ·  %ux%u  ·  %u bpp",
                 graphics.vendor, graphics.model,
                 graphics.display_width, graphics.display_height,
                 graphics.bits_per_pixel);
        imui_list_add(w_content, IMUI_ICON_NONE, "グラフィックス", value);
    }

    fill_cpu_usage(value, sizeof(value));
    imui_list_add(w_content, IMUI_ICON_NONE, "CPU 使用率", value);

    fill_date_time(value, sizeof(value));
    imui_list_add(w_content, IMUI_ICON_NONE, "日時", value);

    uint64_t uptime_ms = get_uptime_ms();
    snprintf(value, sizeof(value), "%llu 時間 %llu 分 %llu 秒",
             (unsigned long long)(uptime_ms / 3600000ull),
             (unsigned long long)((uptime_ms / 60000ull) % 60ull),
             (unsigned long long)((uptime_ms / 1000ull) % 60ull));
    imui_list_add(w_content, IMUI_ICON_NONE, "稼働時間", value);

    snprintf(value, sizeof(value), "%d プロセス", get_process_count());
    imui_list_add(w_content, IMUI_ICON_NONE, "実行中", value);
}

/* The boot profile the kernel records during early boot: one row per phase
 * with its share of the total, which is what makes a slow boot findable. */
static void fill_boot(void)
{
    char value[160];
    int32_t count = get_boot_profile_count();
    if (count <= 0) {
        imui_list_add(w_content, IMUI_ICON_NONE, "計測データがありません", "");
        return;
    }

    uint64_t total_ns = 0u;
    for (int32_t i = 0; i < count; ++i) {
        boot_profile_entry_t entry;
        if (get_boot_profile_entry(i, &entry) >= 0)
            total_ns += entry.duration_ns;
    }

    for (int32_t i = 0; i < count; ++i) {
        boot_profile_entry_t entry;
        if (get_boot_profile_entry(i, &entry) < 0) continue;
        /* A duration of exactly zero means the phase ran before the kernel
         * had any clock at all (the timer is brought up inside the "timer"
         * phase, so everything before it reads 0 ns), not that it was
         * instantaneous: say so instead of printing a fake 0.0 ms. */
        if (entry.duration_ns == 0u) {
            imui_list_add(w_content, IMUI_ICON_NONE,
                          entry.name[0] ? entry.name : "(名称なし)",
                          "—  ·  計測不能（タイマー初期化前）");
            continue;
        }
        /* Shares this small never survive integer rounding -- the boot total
         * is dominated by the seconds-long userland phase -- so carry one
         * decimal place. */
        unsigned tenths = (total_ns > 0u)
            ? (unsigned)((entry.duration_ns * 1000ull + total_ns / 2ull) / total_ns)
            : 0u;
        snprintf(value, sizeof(value), "%llu.%llu ms  ·  %u.%u%%",
                 (unsigned long long)(entry.duration_ns / 1000000ull),
                 (unsigned long long)((entry.duration_ns % 1000000ull) / 100000ull),
                 (unsigned)(tenths / 10u), (unsigned)(tenths % 10u));
        imui_list_add(w_content, IMUI_ICON_NONE,
                      entry.name[0] ? entry.name : "(名称なし)", value);
    }
    snprintf(value, sizeof(value), "%llu ms  ·  %d フェーズ",
             (unsigned long long)(total_ns / 1000000ull), (int)count);
    imui_list_add(w_content, IMUI_ICON_NONE, "合計", value);
}

static void fill_devices(void)
{
    static const char *const kind[] = {
        "不明", "ATA コントローラ", "USB コントローラ", "USB ハブ",
        "USB キーボード", "USB マウス", "USB ストレージ",
        "PS/2 キーボード", "PS/2 マウス", "ネットワーク", "オーディオ",
        "グラフィックス", "PCI ブリッジ", "ストレージコントローラ",
    };
    uint32_t count = 0u;
    if (os_get_device_count(&count) < 0 || count == 0u) {
        imui_list_add(w_content, IMUI_ICON_NONE, "デバイスが見つかりません", "");
        return;
    }
    for (uint32_t i = 0; i < count; ++i) {
        system_device_t device;
        if (os_get_device_info(i, &device) < 0) continue;
        char value[160];
        snprintf(value, sizeof(value), "%s  ·  %04X:%04X  ·  %02u:%02u.%u",
                 (uint32_t)device.type < sizeof(kind) / sizeof(kind[0])
                     ? kind[device.type] : kind[0],
                 device.vendor_id, device.device_id,
                 device.bus, device.device, device.func);
        imui_list_add(w_content, IMUI_ICON_NONE,
                      device.device_name[0] ? device.device_name : "(名称不明)",
                      value);
    }
}

static void fill_storage(void)
{
    uint32_t count = 0u;
    if (os_get_disk_count(&count) < 0 || count == 0u) {
        imui_list_add(w_content, IMUI_ICON_NONE, "ディスクが見つかりません", "");
        return;
    }
    for (uint32_t i = 0; i < count; ++i) {
        system_disk_info_t disk;
        if (os_get_disk_info(i, &disk) < 0) continue;
        char used[24], total[24], value[192];
        format_bytes(disk.used_bytes, used, sizeof(used));
        format_bytes(disk.total_bytes, total, sizeof(total));
        snprintf(value, sizeof(value), "%s %s  ·  %s / %s%s%s",
                 disk.manufacturer, disk.model, used, total,
                 (disk.flags & SYSTEM_DISK_FLAG_BOOT) ? "  ·  起動ディスク" : "",
                 (disk.flags & SYSTEM_DISK_FLAG_WRITABLE) ? "" : "  ·  読み取り専用");
        imui_list_add(w_content, IMUI_ICON_DRIVE,
                      disk.disk_name[0] ? disk.disk_name : "(名称不明)", value);
    }
}

static void show_pane(pane_t pane)
{
    g_pane = pane;
    imui_list_clear(w_content);
    imui_set_text(g_app, w_title, k_pane_name[pane]);
    switch (pane) {
    case PANE_APPEARANCE: fill_appearance(); break;
    case PANE_SYSTEM:     fill_system();     break;
    case PANE_BOOT:       fill_boot();       break;
    case PANE_DEVICES:    fill_devices();    break;
    case PANE_STORAGE:    fill_storage();    break;
    default: break;
    }
    w_nav->sel = (int32_t)pane;
    imui_request_paint(g_app);
}

/* Refilling the system pane on a timer throws the scroll offset away, so it
 * is put back: a pane you have read down should not jump to the top every
 * second while you are reading it. */
static void on_tick(imui_app_t *app, imui_widget_t *w, void *user)
{
    (void)app; (void)w; (void)user;
    if (g_pane != PANE_SYSTEM) return;

    uint64_t now = get_uptime_ms();
    if (g_last_system_ms != 0u && now - g_last_system_ms < SYSTEM_REFRESH_MS)
        return;
    g_last_system_ms = now;

    uint32_t saved_scroll = w_content->scroll;
    int32_t  saved_sel    = w_content->sel;
    show_pane(PANE_SYSTEM);
    w_content->scroll = saved_scroll;
    w_content->sel    = saved_sel;
}

/* ------------------------------------------------------------- events */

/* The pane list starts with the keyboard, so the mirror rule has to exist
 * too: Right steps into the pane's contents, Left back out to the panes.
 * Without it a pointerless session could change panes but never scroll or
 * activate a row inside one. */
static bool on_key(imui_app_t *app, const input_keyboard_event_t *ev)
{
    if (ev->keycode == KEY_RIGHT && app->focus == w_nav) {
        app->focus = w_content;
        if (w_content->sel < 0 && w_content->row_count > 0u)
            w_content->sel = 0;
        app->needs_paint = true;
        return true;
    }
    if (ev->keycode == KEY_LEFT && app->focus == w_content) {
        app->focus = w_nav;
        app->needs_paint = true;
        return true;
    }
    return false;
}

static void on_nav(imui_app_t *app, imui_widget_t *w, void *user)
{
    (void)app; (void)user;
    int row = imui_list_selected(w);
    if (row < 0 || row >= (int)PANE_COUNT) return;
    show_pane((pane_t)row);
}

/* The appearance pane is a list of actions rather than of facts, so a row
 * being chosen there means something. Everything the other panes list is
 * read-only. */
static void on_content(imui_app_t *app, imui_widget_t *w, void *user)
{
    (void)app; (void)user;
    if (g_pane != PANE_APPEARANCE) return;
    int row = imui_list_selected(w);
    if (row <= 0) return;

    uint32_t seed_row = (uint32_t)row - 1u;      /* row 0 is the heading */
    if (seed_row < SEED_COUNT) {
        on_pick_seed(NULL, NULL, (void *)(uintptr_t)seed_row);
    } else {
        uint32_t after = seed_row - SEED_COUNT;  /* heading, light, dark, wallpaper */
        if (after == 1u)      on_pick_mode(NULL, NULL, (void *)(uintptr_t)0u);
        else if (after == 2u) on_pick_mode(NULL, NULL, (void *)(uintptr_t)1u);
        else if (after == 3u) on_reload_wallpaper(NULL, NULL, NULL);
    }
    show_pane(PANE_APPEARANCE);
}

/* ------------------------------------------------------------- entry */

int _start(void);
int _start(void)
{
    serial_write_string("[settings] start\n");
    g_app = imui_create(900, 620, "Settings");
    if (!g_app) {
        serial_write_string("[settings] no window\n");
        for (;;) {}
    }

    /* Start from whatever the desktop is themed to, so the pane marks the
     * current choice as in use rather than always claiming the default. */
    m3_get_desktop_theme(&g_seed, &g_dark);

    imui_widget_t *root = g_app->root;
    root->kind = IMUI_COL; root->pad = 0; root->gap = 0;

    imui_widget_t *header = imui_container(root, IMUI_ROW, 16, 8);
    header->grow = 0; header->min_h = 72; header->fill_bg = true;
    imui_set_variant(header, IMUI_FILLED);
    w_title = imui_set_type_role(imui_label(header, "外観"), IMUI_HEADLINE);
    w_title->grow = 1;

    imui_widget_t *body = imui_container(root, IMUI_ROW, 12, 12);
    body->grow = 1;

    w_nav = imui_list(body);
    w_nav->grow = 0; w_nav->min_w = 240;
    w_nav->on_activate = on_nav;
    imui_set_single_click(w_nav, true);
    for (uint32_t i = 0; i < PANE_COUNT; ++i)
        imui_list_add(w_nav, k_pane_icon[i], k_pane_name[i], k_pane_note[i]);

    w_content = imui_list(body);
    w_content->grow = 1;
    w_content->on_activate = on_content;
    imui_set_single_click(w_content, true);

    imui_widget_t *status_bar = imui_container(root, IMUI_ROW, 12, 0);
    status_bar->grow = 0; status_bar->min_h = 28; status_bar->fill_bg = true;
    imui_set_variant(status_bar, IMUI_FILLED);
    w_status = imui_set_type_role(imui_label(status_bar, ""), IMUI_LABEL_SMALL);
    w_status->grow = 1;

    show_pane(PANE_APPEARANCE);
    g_app->on_tick = on_tick;
    g_app->on_key = on_key;
    /* The pane list takes the keyboard on startup: arrows and Enter then
     * reach the panes without the pointer having to be over them. */
    w_nav->sel = 0;
    g_app->focus = w_nav;
    return imui_run(g_app);
}
