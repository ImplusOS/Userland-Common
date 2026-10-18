/*
 * PerfProbe -- a once-a-second reading of what the machine is doing, on COM1.
 *
 * Why this exists: every cost question about a Linux program running here
 * (Chromium above all) needs two numbers nobody was producing -- how much of
 * the CPU is really burning, and where it is going. The GUI process manager
 * shows both, but only to a human with a mouse; a headless QEMU run has the
 * serial log instead, and that is what gets measured.
 *
 * So this is the process manager's accounting with a serial printer on the
 * end of it:
 *
 *   busy%   1 - (sum of per-CPU hlt time / wall time), which is what the
 *           kernel accumulates in process_scheduler_add_idle_ns()
 *   mem     physical pages in use / total, from os_get_memory_info() (the
 *           same PMM numbers /proc/meminfo reports)
 *   heap    get_used_memory() -- the kernel malloc arena, kept as a separate
 *           number because it used to be the only one and the change of
 *           meaning has to be visible in an old log against a new one
 *   per-process CPU share   delta of process_perf_info_t.runtime_ns between
 *           two polls, exactly as the process manager does it
 *
 * Two lines per second at most, and the detail table only every fifth round,
 * because COM1 is 115200 baud and serial_write_char busy-waits: a chatty
 * probe would show up in the very CPU number it is reporting. Rounds are
 * anchored to the sample interval rather than slept for, so the drift in
 * sleep_ms() does not accumulate into the rate.
 *
 * Autostart it next to whatever is being measured:
 *   make image_livecd AUTOSTART="/Userland/PerfProbe/PerfProbe.ELF ..."
 * It runs until the machine is turned off.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "Process.h"
#include "Serial.h"
#include "SystemInfo.h"

/* pids index the kernel's process table, which is bounded by
 * OS_CONFIG_PROCESS_MAX_COUNT_MAX (1024), so enumeration is a scan. */
#define PID_SCAN_MAX  1024
#define SAMPLE_MS     1000u
#define DETAIL_EVERY  5u
#define MAX_DETAIL    16u
/* Nothing below this share of the CPU is interesting in the detail table;
 * keeps the line count (and so the serial time) proportional to real work. */
#define DETAIL_CPU_PERMILLE 10u
/* ...unless it is holding this much memory, which is worth naming even when
 * it is asleep. */
#define DETAIL_RSS_MB 64u

typedef struct {
    int32_t  pid;
    uint64_t runtime_ns;
    uint64_t memory_usage;
    uint64_t syscalls;
    uint64_t page_fault_count;
    char     name[32];
} sample_t;

typedef struct {
    int32_t  pid;
    uint32_t cpu_permille;
    uint64_t rss;
    uint64_t syscalls;
    uint64_t page_fault_count;
    char     name[32];
} detail_t;

static sample_t   g_prev[PID_SCAN_MAX];
static sample_t   g_cur[PID_SCAN_MAX];
static detail_t   g_rows[PID_SCAN_MAX];
static uint32_t   g_prev_count;
static system_cpu_usage_t g_cpu_prev;
static uint64_t   g_wall_prev_ns;
static uint32_t   g_round;

static void trim_name(const char *src, char *dst, uint32_t cap)
{
    uint32_t i = 0;
    for (; i + 1u < cap && src[i] != '\0'; ++i) {
        /* The Linux ABI keeps the module path in the name; only the last
         * component is of any use on one serial line. */
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

static uint64_t sample_idle_total(const system_cpu_usage_t *u)
{
    uint64_t total = 0;
    uint32_t n = u->cpu_count;
    if (n > OS_CPU_USAGE_MAX_CORES) {
        n = OS_CPU_USAGE_MAX_CORES;
    }
    for (uint32_t i = 0; i < n; ++i) {
        total += u->idle_ns[i];
    }
    return total;
}

/* Delta of two "percent of wall clock" quantities, as parts per thousand. */
static uint32_t idle_permille(uint64_t idle_delta, uint64_t wall_delta,
                              uint32_t cpu_count)
{
    if (wall_delta == 0u || cpu_count == 0u) {
        return 0u;
    }
    uint64_t busy = wall_delta * cpu_count;
    if (idle_delta > busy) {
        return 1000u;
    }
    return (uint32_t)(((busy - idle_delta) * 1000u) / busy);
}

/*
 * Boot phase timings, printed once before the first sample. The kernel
 * records a duration for each phase of kernel_main (boot_profile_begin/end);
 * serial is the only place they come out, and a boot-time change is only
 * meaningful against them -- especially the IO phases, which run before any
 * of the once-a-second numbers below exist.
 */
static void dump_boot_profile(void)
{
    int32_t count = get_boot_profile_count();
    char line[128];

    if (count <= 0) {
        serial_write_string("[probe] boot profile unavailable\n");
        return;
    }

    for (int32_t i = 0; i < count; ++i) {
        boot_profile_entry_t entry;
        uint64_t us;
        if (get_boot_profile_entry(i, &entry) != 0) {
            continue;
        }
        us = entry.duration_ns / 1000ULL;
        snprintf(line, sizeof(line), "[probe] phase %s %llu.%03llums\n",
                 entry.name,
                 (unsigned long long)(us / 1000ULL),
                 (unsigned long long)(us % 1000ULL));
        serial_write_string(line);
    }
}

static void emit_summary(uint32_t busy_permille, uint64_t used_bytes,
                         uint64_t total_bytes, uint64_t heap_bytes,
                         uint32_t pids)
{
    char line[192];
    snprintf(line, sizeof(line),
             "[probe] t=%llu busy=%u.%u mem=%llu/%lluMB heap=%lluMB pids=%u\n",
             (unsigned long long)get_uptime_ms(),
             (unsigned)(busy_permille / 10u),
             (unsigned)(busy_permille % 10u),
             (unsigned long long)(used_bytes >> 20),
             (unsigned long long)(total_bytes >> 20),
             (unsigned long long)(heap_bytes >> 20),
             (unsigned)pids);
    serial_write_string(line);
}

static void emit_detail(const detail_t *rows, uint32_t count, uint32_t round)
{
    char line[224];
    for (uint32_t i = 0; i < count; ++i) {
        snprintf(line, sizeof(line),
                 "[probe] r=%u pid=%d cpu=%u.%u%% rss=%lluMB sys=%llu "
                 "flt=%llu %s\n",
                 (unsigned)round,
                 (int)rows[i].pid,
                 (unsigned)(rows[i].cpu_permille / 10u),
                 (unsigned)(rows[i].cpu_permille % 10u),
                 (unsigned long long)(rows[i].rss >> 20),
                 (unsigned long long)rows[i].syscalls,
                 (unsigned long long)rows[i].page_fault_count,
                 rows[i].name);
        serial_write_string(line);
    }
}

/* Top MAX_DETAIL rows by CPU share, best first; ties broken by pid so
 * successive rounds are comparable. Selection rather than a full sort: the
 * candidate list can be as long as the process table. */
static uint32_t select_top(const detail_t *rows, uint32_t count,
                           detail_t *out, uint32_t max_out)
{
    bool taken[PID_SCAN_MAX];
    uint32_t picked = 0;

    memset(taken, 0, count);
    while (picked < max_out) {
        uint32_t best = count;
        for (uint32_t i = 0; i < count; ++i) {
            if (taken[i]) {
                continue;
            }
            if (best == count ||
                rows[i].cpu_permille > rows[best].cpu_permille ||
                (rows[i].cpu_permille == rows[best].cpu_permille &&
                 rows[i].pid < rows[best].pid)) {
                best = i;
            }
        }
        if (best == count) {
            break;
        }
        taken[best] = true;
        out[picked++] = rows[best];
    }
    return picked;
}

static void round_once(uint32_t detail)
{
    system_cpu_usage_t now = {0};
    /* mem = physical pages in use / total (os_get_memory_info, the PMM's
     * numbers). heap = the kernel malloc arena, kept so an old log's mem=
     * can still be compared against this run's heap=. The two were conflated
     * before, and the heap figure was over-counted on top of that (see the
     * realloc shrink path in Memory_Main.c). */
    system_memory_info_t mem_info = {0};
    (void)os_get_memory_info(&mem_info);
    uint64_t used = mem_info.used_bytes;
    uint64_t total = mem_info.total_bytes;
    uint64_t heap = get_used_memory();

    uint32_t cur_count = 0;
    uint32_t row_count = 0;

    for (int32_t pid = 0; pid < PID_SCAN_MAX; ++pid) {
        process_perf_info_t perf;
        if (get_process_perf_info(pid, &perf) < 0) {
            continue;
        }
        sample_t *s = &g_cur[cur_count];
        s->pid = pid;
        s->runtime_ns = perf.runtime_ns;
        s->memory_usage = perf.memory_usage;
        s->syscalls = perf.syscalls;
        s->page_fault_count = perf.page_fault_count;
        trim_name(perf.name, s->name, (uint32_t)sizeof(s->name));
        ++cur_count;
    }

    (void)os_get_cpu_usage(&now);

    uint32_t busy = 0;
    if (g_wall_prev_ns != 0u && now.wall_ns > g_wall_prev_ns) {
        uint64_t wall_delta = now.wall_ns - g_wall_prev_ns;
        uint64_t idle_now = sample_idle_total(&now);
        uint64_t idle_prev = sample_idle_total(&g_cpu_prev);
        uint64_t idle_delta = idle_now >= idle_prev ? idle_now - idle_prev : 0;
        busy = idle_permille(idle_delta, wall_delta,
                             now.cpu_count != 0u ? now.cpu_count : 1u);

        uint32_t n = cur_count < g_prev_count ? cur_count : g_prev_count;
        for (uint32_t i = 0; i < n; ++i) {
            uint64_t runtime_delta = 0;
            if (g_cur[i].runtime_ns >= g_prev[i].runtime_ns) {
                runtime_delta = g_cur[i].runtime_ns - g_prev[i].runtime_ns;
            }
            uint32_t permille =
                (uint32_t)((runtime_delta * 1000u) / wall_delta);
            bool interesting = permille >= DETAIL_CPU_PERMILLE ||
                (g_cur[i].memory_usage >> 20) >= DETAIL_RSS_MB;
            if (!interesting || row_count >= PID_SCAN_MAX) {
                continue;
            }
            detail_t *d = &g_rows[row_count];
            d->pid = g_cur[i].pid;
            d->cpu_permille = permille;
            d->rss = g_cur[i].memory_usage;
            d->syscalls = g_cur[i].syscalls;
            d->page_fault_count = g_cur[i].page_fault_count;
            memcpy(d->name, g_cur[i].name, sizeof(d->name));
            ++row_count;
        }
    }

    g_cpu_prev = now;
    g_wall_prev_ns = now.wall_ns;
    memcpy(g_prev, g_cur, sizeof(sample_t) * cur_count);
    g_prev_count = cur_count;

    if (detail != 0u) {
        detail_t top[MAX_DETAIL];
        uint32_t picked = select_top(g_rows, row_count, top, MAX_DETAIL);
        emit_detail(top, picked, g_round);
    }
    emit_summary(busy, used, total, heap, cur_count);
    ++g_round;
}

void _start(void)
{
    serial_write_string("[probe] start\n");
    dump_boot_profile();
    memset(&g_cpu_prev, 0, sizeof(g_cpu_prev));
    g_wall_prev_ns = 0;
    g_prev_count = 0;
    g_round = 0;

    /* The first round only records the baseline: with no previous sample the
     * rates would all read as zero. */
    round_once(0u);

    for (;;) {
        sleep_ms(SAMPLE_MS);
        round_once((g_round % DETAIL_EVERY) == 0u);
    }
}
